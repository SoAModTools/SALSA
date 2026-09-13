#pragma once
#include "PackedStorage.h"
#include <bit>

namespace salsa::evaluation {
class PagedStorage {
    static constexpr uint32_t PageSize=256;
    struct Page {
        std::array<StoredRecord,PageSize> rows{};
        std::array<uint64_t,PageSize/64> occupied{};
        uint32_t live{},nonFull=NoSlot;
    };
    struct DeletePage {
        std::pmr::memory_resource* resource;
        void operator()(Page* p) const noexcept { if(p) { std::destroy_at(p); resource->deallocate(p,sizeof(Page),alignof(Page)); } }
    };
    using PagePtr=std::unique_ptr<Page,DeletePage>;
public:
    struct Prepared {
        const PagedStorage* owner;
        std::pmr::vector<StorageChange> changes;
        std::pmr::vector<PagePtr> pages;
        Prepared(const PagedStorage* o,Context& c):owner(o),changes(c.resource(Domain::Preparation)),pages(c.resource(Domain::Preparation)){}
        Prepared(Prepared&& p) noexcept:owner(std::exchange(p.owner,nullptr)),changes(std::move(p.changes)),pages(std::move(p.pages)){}
        Prepared(const Prepared&)=delete;
    };
    explicit PagedStorage(Context& c):context_(c),pages_(c.resource(Domain::Records)),
        nonFull_(c.resource(Domain::Directory)),directory_(c.resource(Domain::Directory)){}
    Result<void> load(std::span<const StoredRecord> input,std::span<const Section>) {
        try {
            expect(!loaded_,"Store already loaded"); loaded_=true;
            const auto count=(input.size()+PageSize-1)/PageSize;
            pages_.reserve(count); nonFull_.reserve(count);
            for(size_t i=0;i<count;++i) addPage(newPage());
            // Populate source order from the first page while using the same O(1) free-list path.
            std::ranges::reverse(nonFull_);
            for(uint32_t i=0;i<nonFull_.size();++i) pages_[nonFull_[i]]->nonFull=i;
            for(const auto& row:input) {
                auto& loc=directory_.ensure(row.id); expect(loc.slot==NoSlot,"Duplicate instruction"); append(row);
            }
            work_={}; return Result<void>::success();
        } catch(const std::exception& e) { return error(e.what()); }
    }
    const StoredRecord* lookup(InstructionId id) const noexcept {
        const auto* loc=directory_.find(id);
        if(!loc || loc->group>=pages_.size() || loc->slot>=PageSize) return nullptr;
        const auto& page=*pages_[loc->group];
        return occupied(page,loc->slot) && page.rows[loc->slot].id==id?&page.rows[loc->slot]:nullptr;
    }
    template<class Visitor> void forEachPhysical(Visitor&& visitor) const {
        for(const auto& page:pages_) for(uint32_t word=0;word<4;++word) {
            auto bits=page->occupied[word];
            while(bits) { const auto bit=std::countr_zero(bits); visitor(page->rows[word*64+bit]); bits&=bits-1; }
        }
    }
    Result<Prepared> prepare(std::span<const StorageChange> changes) {
        try {
            Prepared p(this,context_); p.changes.assign(changes.begin(),changes.end());
            size_t additions=0;
            for(const auto& change:changes) {
                expect(change.before || change.after,"Empty storage change");
                const auto id=change.before?change.before->id:change.after->id;
                const auto* current=lookup(id);
                expect(change.before?(current && *current==*change.before):!current,"Storage precondition mismatch");
                if(change.after) { expect(change.after->id==id,"Storage identity changed"); directory_.ensure(id); }
                additions+=!change.before;
            }
            const auto required=(live_+additions+PageSize-1)/PageSize; slotCount(required);
            if(required>pages_.size()) {
                if(required>pages_.capacity()) pages_.reserve(std::max<size_t>(required,pages_.capacity()+pages_.capacity()/2+1));
                if(required>nonFull_.capacity()) nonFull_.reserve(std::max<size_t>(required,nonFull_.capacity()+nonFull_.capacity()/2+1));
                p.pages.reserve(required-pages_.size());
                for(size_t i=pages_.size();i<required;++i) p.pages.push_back(newPage());
            }
            return Result<Prepared>::success(std::move(p));
        } catch(const std::exception& e) { return Result<Prepared>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,e.what())); }
    }
    void apply(Prepared&& p) noexcept {
        assert(p.owner==this);
        for(auto& page:p.pages) { addPage(std::move(page)); ++growths_; }
        for(const auto& change:p.changes) {
            if(!change.before) { append(*change.after); ++work_.inserted; }
            else if(!change.after) { erase(change.before->id); ++work_.erased; }
            else { const auto loc=*directory_.find(change.before->id); pages_[loc.group]->rows[loc.slot]=*change.after; ++work_.replaced; }
        }
        p.owner=nullptr;
    }
    Result<void> validate() const {
        uint64_t live=0,empty=0,nonFull=0;
        for(uint32_t g=0;g<pages_.size();++g) {
            const auto& page=*pages_[g]; uint32_t count=0;
            for(uint32_t slot=0;slot<PageSize;++slot) if(occupied(page,slot)) {
                ++count; const auto* loc=directory_.find(page.rows[slot].id);
                if(!loc || loc->group!=g || loc->slot!=slot) return error("Paged directory mismatch");
            }
            if(count!=page.live) return error("Page occupancy mismatch");
            if(count<PageSize) {
                ++nonFull;
                if(page.nonFull>=nonFull_.size() || nonFull_[page.nonFull]!=g) return error("Non-full page list mismatch");
            } else if(page.nonFull!=NoSlot) return error("Full page in free list");
            live+=count; empty+=count==0;
        }
        return live==live_ && empty==empty_ && nonFull==nonFull_.size()?Result<void>::success():error("Paged metrics mismatch");
    }
    StorageMetrics metrics() const noexcept { return {live_,live_*sizeof(StoredRecord),work_,pages_.size()*PageSize,pages_.size(),empty_,directory_.bytes(),growths_,0,0}; }
private:
    static Result<void> error(std::string message) { return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,std::move(message))); }
    static bool occupied(const Page& p,uint32_t slot) noexcept { return (p.occupied[slot/64] & (uint64_t{1}<<(slot%64)))!=0; }
    PagePtr newPage() {
        auto* r=context_.resource(Domain::Records); auto* p=static_cast<Page*>(r->allocate(sizeof(Page),alignof(Page)));
        std::construct_at(p); return PagePtr(p,DeletePage{r});
    }
    void addPage(PagePtr p) noexcept {
        const auto group=static_cast<uint32_t>(pages_.size()); p->nonFull=static_cast<uint32_t>(nonFull_.size());
        pages_.push_back(std::move(p)); nonFull_.push_back(group); ++empty_;
    }
    void append(const StoredRecord& row) noexcept {
        const auto group=nonFull_.back(); auto& page=*pages_[group];
        uint32_t word=0; while(page.occupied[word]==UINT64_MAX) ++word;
        const auto slot=word*64+static_cast<uint32_t>(std::countr_zero(~page.occupied[word]));
        page.rows[slot]=row; page.occupied[word]|=uint64_t{1}<<(slot%64);
        if(!page.live) --empty_;
        if(++page.live==PageSize) { nonFull_.pop_back(); page.nonFull=NoSlot; }
        *directory_.find(row.id)={group,slot}; ++work_.directoryWrites; ++live_;
    }
    void erase(InstructionId id) noexcept {
        const auto loc=*directory_.find(id); auto& page=*pages_[loc.group];
        if(page.live==PageSize) { page.nonFull=static_cast<uint32_t>(nonFull_.size()); nonFull_.push_back(loc.group); }
        page.occupied[loc.slot/64]&=~(uint64_t{1}<<(loc.slot%64));
        if(!--page.live) ++empty_;
        *directory_.find(id)={}; ++work_.directoryWrites; --live_;
    }
    Context& context_;
    std::pmr::vector<PagePtr> pages_;
    std::pmr::vector<uint32_t> nonFull_;
    PagedDirectory<RecordLocation> directory_;
    Work work_;
    uint64_t live_{},empty_{},growths_{};
    bool loaded_{};
};
static_assert(StorageCandidate<PagedStorage>);
} // namespace salsa::evaluation
