#pragma once
#include "StorageSupport.h"
#include <bit>

namespace salsa::evaluation {
// Stable private bundle handles. Reclamation reuses dead slots without moving survivors.
class OperandPool {
    static constexpr uint32_t Slots=256;
    struct Page {
        std::array<std::array<Operand,3>,Slots> bundles{};
        std::array<uint64_t,4> occupied{},marked{};
    };
public:
    struct Metrics { uint64_t bundles,pages,bytes; };
    explicit OperandPool(std::pmr::memory_resource* r):resource_(r),pages_(r),available_(r){}
    ~OperandPool() { for(auto* p:pages_) destroy(p); }
    OperandPool(const OperandPool&)=delete;
    void reserve(size_t operands) { pages_.reserve((operands+3*Slots-1)/(3*Slots)); available_.reserve(pages_.capacity()); }
    uint32_t append(const std::array<Operand,3>& values) {
        if(available_.empty()) {
            expect(pages_.size()<NoSlot/(3*Slots),"Operand handle limit exceeded");
            // Keep an empty slot after failed allocation; maintenance also finds it.
            pages_.push_back(nullptr);
            try { available_.push_back(static_cast<uint32_t>(pages_.size()-1)); }
            catch(...) { pages_.pop_back(); throw; }
        }
        const auto page=available_.back();
        if(!pages_[page]) {
            auto* p=static_cast<Page*>(resource_->allocate(sizeof(Page),alignof(Page)));
            std::construct_at(p); pages_[page]=p; ++allocatedPages_;
        }
        auto& p=*pages_[page]; uint32_t word=0;
        while(p.occupied[word]==UINT64_MAX) ++word;
        const auto slot=word*64+static_cast<uint32_t>(std::countr_zero(~p.occupied[word]));
        p.bundles[slot]=values; p.occupied[word]|=uint64_t{1}<<(slot%64); ++bundles_;
        if(std::ranges::all_of(p.occupied,[](uint64_t bits){ return bits==UINT64_MAX; })) available_.pop_back();
        return (page*Slots+slot)*3;
    }
    const Operand& operator[](uint32_t handle) const {
        const auto bundle=handle/3,page=bundle/Slots,slot=bundle%Slots;
        expect(page<pages_.size() && pages_[page] && (pages_[page]->occupied[slot/64]&(uint64_t{1}<<(slot%64))),"Retired operand handle");
        return pages_[page]->bundles[slot][handle%3];
    }
    void beginMark() { for(auto* p:pages_) if(p) p->marked={}; }
    void mark(uint32_t handle) {
        (void)(*this)[handle]; const auto bundle=handle/3; auto& p=*pages_[bundle/Slots];
        p.marked[(bundle%Slots)/64]|=uint64_t{1}<<(bundle%64);
    }
    template<class Sink> void markedValues(Sink&& sink) const {
        for(auto* p:pages_) if(p) for(uint32_t w=0;w<4;++w) {
            auto bits=p->marked[w]; while(bits) { const auto slot=w*64+std::countr_zero(bits); for(const auto& v:p->bundles[slot]) sink(v); bits&=bits-1; }
        }
    }
    Metrics metrics() const noexcept { return {bundles_,allocatedPages_,allocatedPages_*sizeof(Page)}; }
    uint64_t markedBundles() const { uint64_t count=0; for(auto* p:pages_) if(p) for(auto bits:p->marked) count+=std::popcount(bits); return count; }
    void sweep() {
        // Reserve scratch before publishing any reclamation changes.
        std::pmr::vector<uint32_t> available(resource_); available.reserve(pages_.size());
        bundles_=0;
        for(uint32_t i=0;i<pages_.size();++i) {
            auto* p=pages_[i]; uint32_t live=0;
            if(p) { p->occupied=p->marked; for(auto bits:p->occupied) live+=std::popcount(bits); }
            if(!live && p) { destroy(p); pages_[i]=nullptr; --allocatedPages_; }
            if(live<Slots) available.push_back(i);
            bundles_+=live;
        }
        available_.swap(available);
    }
private:
    void destroy(Page* p) { if(p) { std::destroy_at(p); resource_->deallocate(p,sizeof(Page),alignof(Page)); } }
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Page*> pages_; std::pmr::vector<uint32_t> available_;
    uint64_t bundles_{},allocatedPages_{};
};

// Copy policy is an explicit experiment variant. Shared lists detach only on mutation.
class OrderList {
    using Values=std::pmr::vector<InstructionId>;
    std::shared_ptr<Values> values_;
    std::pmr::memory_resource* resource_;
    uint64_t* copied_;
    bool shared_;
    std::shared_ptr<Values> clone() const {
        auto result=std::allocate_shared<Values>(std::pmr::polymorphic_allocator<Values>(resource_));
        if(values_) { result->assign(values_->begin(),values_->end()); *copied_+=values_->size(); }
        return result;
    }
    void writable() { if(!values_ || values_.use_count()!=1) values_=clone(); }
public:
    OrderList(std::pmr::memory_resource* r,uint64_t& copied,bool shared):resource_(r),copied_(&copied),shared_(shared){}
    template<class Range> OrderList(const Range& values,std::pmr::memory_resource* r,uint64_t& copied,bool shared):OrderList(r,copied,shared) { writable(); values_->assign(values.begin(),values.end()); }
    OrderList(const OrderList& o):values_(o.values_),resource_(o.resource_),copied_(o.copied_),shared_(o.shared_) { if(!shared_) values_=clone(); }
    OrderList(OrderList&&)=default;
    OrderList& operator=(const OrderList& o) { if(this!=&o) { OrderList copy(o); swap(copy); } return *this; }
    OrderList& operator=(OrderList&&)=default;
    const InstructionId* data() const noexcept { static const InstructionId empty{}; return values_ && !values_->empty()?values_->data():&empty; }
    size_t size() const noexcept { return values_?values_->size():0; }
    const InstructionId* begin() const noexcept { return data(); }
    const InstructionId* end() const noexcept { return size()?data()+size():data(); }
    const InstructionId& operator[](size_t i) const { return (*values_)[i]; }
    void push_back(InstructionId id) { writable(); values_->push_back(id); }
    void erase(const InstructionId* at) { const auto i=at-begin(); writable(); values_->erase(values_->begin()+i); }
    void insert(const InstructionId* at,InstructionId id) { const auto i=size()?at-begin():0; writable(); values_->insert(values_->begin()+i,id); }
    void swap(OrderList& o) noexcept { assert(resource_==o.resource_ && shared_==o.shared_); values_.swap(o.values_); }
};

struct ChangeCursorState { Capture capture; };
using ChangeCursor=std::shared_ptr<ChangeCursorState>;
struct ReaderGauges {
    std::atomic_uint64_t currentBytes{},peakBytes{},readers{};
    void add(uint64_t bytes) { ++readers; const auto current=currentBytes.fetch_add(bytes)+bytes; auto peak=peakBytes.load(); while(peak<current && !peakBytes.compare_exchange_weak(peak,current)) {} }
};

// Owns ordinary C++ values and its allocator lifetime, independently of host/Context.
template<class Fact> class ReadSnapshot {
    template<StorageCandidate> friend class ProgramHost;
    Capture capture_; bool whole_{}; std::vector<Record> records_; std::vector<Fact> facts_;
    std::vector<Expression> expressions_; std::vector<Section> sections_; std::vector<Script> scripts_;
    std::vector<std::string> evidence_;
    std::shared_ptr<ReaderGauges> gauges_; uint64_t bytes_{};
public:
    explicit ReadSnapshot(std::shared_ptr<ReaderGauges> gauges):gauges_(std::move(gauges)){}
    ~ReadSnapshot() { if(bytes_) { gauges_->currentBytes-=bytes_; --gauges_->readers; } }
    ReadSnapshot(const ReadSnapshot&)=delete;
    Capture capture() const noexcept { return capture_; }
    uint64_t bytes() const noexcept { return bytes_; }
    bool wholeProgram() const noexcept { return whole_; }
    const auto& records() const noexcept { return records_; }
    const auto& facts() const noexcept { return facts_; }
    const auto& expressions() const noexcept { return expressions_; }
    const auto& sections() const noexcept { return sections_; }
    const auto& scripts() const noexcept { return scripts_; }
    const auto& evidence() const noexcept { return evidence_; }
    const Record* lookup(InstructionId id) const { const auto at=std::ranges::lower_bound(records_,id,{},&Record::id); return at!=records_.end() && at->id==id?&*at:nullptr; }
};
} // namespace salsa::evaluation
