#pragma once
#include "StorageSupport.h"

namespace salsa::evaluation {
struct RecordLocation { uint32_t group=NoSlot, slot=NoSlot; };

// One dense table, or one independently growing table per imported section.
// Execution order remains in ProgramHost; swap removal only changes private slots.
template<bool SectionLocal> class PackedStorage {
    struct Group {
        SectionId id;
        std::pmr::vector<StoredRecord> rows;
        Group(SectionId i,std::pmr::memory_resource* r):id(i),rows(r){}
    };
public:
    struct Prepared {
        const PackedStorage* owner;
        std::pmr::vector<StorageChange> changes;
        Prepared(const PackedStorage* o,Context& c):owner(o),changes(c.resource(Domain::Preparation)){}
        Prepared(Prepared&& p) noexcept:owner(std::exchange(p.owner,nullptr)),changes(std::move(p.changes)){}
        Prepared(const Prepared&)=delete;
    };
    explicit PackedStorage(Context& c):context_(c),groups_(c.resource(Domain::Records)),
        sections_(c.resource(Domain::Directory)),directory_(c.resource(Domain::Directory)){}
    Result<void> load(std::span<const StoredRecord> rows,std::span<const SectionLayout> sections) {
        try {
            expect(!loaded_,"Store already loaded"); loaded_=true;
            groups_.reserve(SectionLocal?sections.size():1);
            if constexpr(SectionLocal) {
                for(const auto& section:sections) {
                    expect(sections_.emplace(section.id,slotCount(groups_.size())).second,"Duplicate storage section");
                    groups_.emplace_back(section.id,context_.resource(Domain::Records));
                    groups_.back().rows.reserve(section.count);
                    capacity_+=groups_.back().rows.capacity();
                }
            } else { groups_.emplace_back(SectionId{},context_.resource(Domain::Records)); groups_[0].rows.reserve(rows.size()); capacity_=groups_[0].rows.capacity(); }
            empty_=groups_.size();
            for(const auto& row:rows) {
                auto& location=directory_.ensure(row.id); expect(location.slot==NoSlot,"Duplicate instruction");
                const auto group=groupFor(row.section);
                expect(groups_[group].rows.size()<groups_[group].rows.capacity(),"Section count disagrees with input");
                append(group,row);
            }
            work_={}; return Result<void>::success();
        } catch(const std::exception& e) { return error(e.what()); }
    }
    const StoredRecord* lookup(InstructionId id) const noexcept {
        const auto* loc=directory_.find(id);
        if(!loc || loc->group>=groups_.size()) return nullptr;
        const auto& rows=groups_[loc->group].rows;
        return loc->slot<rows.size() && rows[loc->slot].id==id?&rows[loc->slot]:nullptr;
    }
    template<class Visitor> void forEachPhysical(Visitor&& visitor) const {
        for(const auto& group:groups_) for(const auto& row:group.rows) visitor(row);
    }
    Result<Prepared> prepare(std::span<const StorageChange> changes) {
        try {
            Prepared p(this,context_); p.changes.assign(changes.begin(),changes.end());
            // Only touched destinations need capacity planning. No scan of all sections.
            std::pmr::map<uint32_t,size_t> additions(context_.resource(Domain::Preparation));
            for(const auto& change:changes) {
                expect(change.before || change.after,"Empty storage change");
                const auto id=change.before?change.before->id:change.after->id;
                const auto* current=lookup(id);
                expect(change.before?(current && *current==*change.before):!current,"Storage precondition mismatch");
                if(change.after) {
                    expect(change.after->id==id,"Storage identity changed"); directory_.ensure(id);
                    const auto dest=groupFor(change.after->section);
                    if(!change.before || groupFor(change.before->section)!=dest) ++additions[dest];
                }
            }
            for(const auto& [group,additional]:additions) {
                auto& rows=groups_[group].rows;
                const auto required=rows.size()+additional; slotCount(required);
                if(required>rows.capacity()) {
                    const auto old=rows.capacity();
                    rows.reserve(std::max(required,old+old/2+1));
                    capacity_+=rows.capacity()-old; ++growths_; growthMoves_+=rows.size();
                }
            }
            return Result<Prepared>::success(std::move(p));
        } catch(const std::exception& e) { return Result<Prepared>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,e.what())); }
    }
    void apply(Prepared&& p) noexcept {
        assert(p.owner==this);
        for(const auto& change:p.changes) {
            if(!change.before) { append(groupFor(change.after->section),*change.after); ++work_.inserted; }
            else if(!change.after) { erase(change.before->id); ++work_.erased; }
            else {
                const auto location=*directory_.find(change.before->id);
                const auto dest=groupFor(change.after->section);
                if(dest==location.group) groups_[dest].rows[location.slot]=*change.after;
                else { erase(change.before->id); append(dest,*change.after); }
                ++work_.replaced;
            }
        }
        p.owner=nullptr;
    }
    Result<void> validate() const {
        uint64_t live=0,capacity=0,empty=0;
        for(uint32_t g=0;g<groups_.size();++g) {
            const auto& group=groups_[g]; capacity+=group.rows.capacity(); empty+=group.rows.empty();
            for(uint32_t slot=0;slot<group.rows.size();++slot) {
                const auto& row=group.rows[slot]; const auto* loc=directory_.find(row.id);
                if(!loc || loc->group!=g || loc->slot!=slot || (SectionLocal && row.section!=group.id)) return error("Packed directory/owner mismatch");
                ++live;
            }
        }
        return live==live_ && capacity==capacity_ && empty==empty_?Result<void>::success():error("Packed metrics mismatch");
    }
    StorageMetrics metrics() const noexcept { return {live_,live_*sizeof(StoredRecord),work_,capacity_,groups_.size(),empty_,directory_.bytes(),growths_,growthMoves_,eraseMoves_}; }
private:
    static Result<void> error(std::string message) { return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,std::move(message))); }
    uint32_t groupFor(SectionId id) const {
        if constexpr(!SectionLocal) return 0;
        else { const auto found=sections_.find(id); expect(found!=sections_.end(),"Unknown storage section"); return found->second; }
    }
    void append(uint32_t group,const StoredRecord& row) noexcept {
        auto& rows=groups_[group].rows;
        if(rows.empty()) --empty_;
        *directory_.find(row.id)={group,static_cast<uint32_t>(rows.size())}; ++work_.directoryWrites;
        rows.push_back(row); ++live_;
    }
    void erase(InstructionId id) noexcept {
        const auto loc=*directory_.find(id); auto& rows=groups_[loc.group].rows;
        if(loc.slot+1!=rows.size()) {
            rows[loc.slot]=rows.back(); *directory_.find(rows[loc.slot].id)=loc;
            ++eraseMoves_; ++work_.directoryWrites;
        }
        rows.pop_back(); if(rows.empty()) ++empty_;
        *directory_.find(id)={}; ++work_.directoryWrites; --live_;
    }
    Context& context_;
    std::pmr::vector<Group> groups_;
    std::pmr::map<SectionId,uint32_t> sections_;
    PagedDirectory<RecordLocation> directory_;
    Work work_;
    uint64_t live_{},capacity_{},empty_{},growths_{},growthMoves_{},eraseMoves_{};
    bool loaded_{};
};
using DenseStorage=PackedStorage<false>;
using SectionStorage=PackedStorage<true>;
static_assert(StorageCandidate<DenseStorage> && StorageCandidate<SectionStorage>);
} // namespace salsa::evaluation
