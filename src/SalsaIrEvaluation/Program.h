#pragma once
#include "Reference.h"
#include "StorageSupport.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace salsa::evaluation {
struct InstructionState { bool live{}, issued{}, published{}; uint32_t section=NoSlot,rank=NoSlot,head=NoSlot,callBucket=NoSlot,fallthroughBucket=NoSlot; };
using InstructionDirectory=PagedDirectory<InstructionState>;
struct ExpressionLocation { uint32_t slot=NoSlot; };

enum class FactKind : uint8_t { Read, Write, Call, Fallthrough, Unknown };
struct Fact {
    FactId id; InstructionId source; OperandId operand;
    FactKind kind{}; uint64_t role{}; Bank bank{}; uint64_t key{}, value{};
    ExprId expression{}; uint32_t evidence{};
    auto operator<=>(const Fact&) const = default;
};
struct BucketKey {
    FactKind kind{}; Bank bank{}; uint64_t key{};
    auto operator<=>(const BucketKey&) const = default;
};
inline BucketKey bucketKey(const Fact& f) { return f.kind==FactKind::Call && f.evidence?BucketKey{FactKind::Unknown,Bank::Bit,0}:BucketKey{f.kind,f.bank,f.key}; }
inline bool sameRole(const Fact& a,const Fact& b) { return a.source==b.source && a.operand==b.operand && a.kind==b.kind && a.role==b.role; }
struct ProgramWork {
    uint64_t instructionsExamined{}, factsExtracted{}, factsChanged{}, postingsChanged{}, orderIdsCopied{}, ranksRepaired{};
};
class FactIndex {
    struct Node { Fact fact; uint32_t previous=NoSlot,next=NoSlot,sourceNext=NoSlot,bucket=NoSlot; bool live{}; };
    struct Bucket { uint32_t head=NoSlot; uint64_t version{}; };
public:
    FactIndex(std::pmr::memory_resource* r,ProgramWork& work,InstructionDirectory& directory):nodes_(r),buckets_(r),variableBuckets_(r),work_(work),directory_(directory){}
    void reserve(size_t additional) {
        const auto required=nodes_.size()+additional; expect(required<NoSlot,"Fact slot limit exceeded");
        if(required>nodes_.capacity()) nodes_.reserve(std::max(required,nodes_.capacity()+nodes_.capacity()/2+1));
    }
    void prepareBuckets(std::span<const Fact> facts) {
        for(const auto& f:facts) {
            const auto key=bucketKey(f);
            if(key.kind==FactKind::Call || key.kind==FactKind::Fallthrough) {
                auto& state=directory_.ensure(InstructionId{key.key});
                auto& slot=key.kind==FactKind::Call?state.callBucket:state.fallthroughBucket;
                if(slot==NoSlot) { const auto newSlot=slotCount(buckets_.size()); buckets_.emplace_back(); slot=newSlot; }
            } else if(!variableBuckets_.contains(key)) {
                const auto newSlot=slotCount(buckets_.size()); buckets_.emplace_back(); variableBuckets_.emplace(key,newSlot);
            }
        }
    }
    template<class Sink> void source(uint32_t head,Sink&& sink) const { for(auto i=head;i!=NoSlot;i=nodes_[i].sourceNext) sink(nodes_[i].fact); }
    template<class Sink> void query(BucketKey key,Sink&& sink) const {
        const auto slot=bucketSlot(key); if(slot==NoSlot) return;
        for(auto i=buckets_[slot].head;i!=NoSlot;i=nodes_[i].next) sink(nodes_[i].fact);
    }
    uint64_t version(BucketKey key) const { const auto slot=bucketSlot(key); return slot==NoSlot?0:buckets_[slot].version; }
    void apply(uint32_t& head,std::span<const Fact> desired) noexcept {
        auto previous=NoSlot, current=head;
        while(current!=NoSlot) {
            auto& node=nodes_[current]; const auto next=node.sourceNext;
            const auto found=std::ranges::find(desired,node.fact.id,&Fact::id);
            if(found==desired.end()) {
                detach(current);
                if(previous==NoSlot) head=next; else nodes_[previous].sourceNext=next;
                node.live=false; node.sourceNext=free_; free_=current; --live_;
                ++work_.factsChanged;
            } else {
                if(node.fact!=*found) {
                    if(bucketKey(node.fact)!=bucketKey(*found)) { detach(current); node.fact=*found; attach(current); }
                    else { node.fact=*found; ++buckets_[node.bucket].version; }
                    ++work_.factsChanged;
                }
                previous=current;
            }
            current=next;
        }
        for(const auto& fact:desired) {
            bool exists=false;
            for(auto i=head;i!=NoSlot;i=nodes_[i].sourceNext) if(nodes_[i].fact.id==fact.id) { exists=true; break; }
            if(exists) continue;
            uint32_t index;
            if(free_!=NoSlot) { index=free_; free_=nodes_[index].sourceNext; }
            else { index=static_cast<uint32_t>(nodes_.size()); nodes_.emplace_back(); }
            auto& node=nodes_[index]; node.fact=fact; node.live=true; node.sourceNext=head; head=index;
            attach(index); ++live_; ++work_.factsChanged;
        }
    }
    uint64_t live() const noexcept { return live_; }
private:
    uint32_t bucketSlot(BucketKey key) const {
        if(key.kind==FactKind::Call || key.kind==FactKind::Fallthrough) {
            const auto* state=directory_.find(InstructionId{key.key});
            return state?(key.kind==FactKind::Call?state->callBucket:state->fallthroughBucket):NoSlot;
        }
        const auto found=variableBuckets_.find(key); return found==variableBuckets_.end()?NoSlot:found->second;
    }
    void detach(uint32_t index) noexcept {
        auto& node=nodes_[index]; auto& bucket=buckets_[node.bucket];
        if(node.previous!=NoSlot) nodes_[node.previous].next=node.next; else bucket.head=node.next;
        if(node.next!=NoSlot) nodes_[node.next].previous=node.previous;
        ++bucket.version; ++work_.postingsChanged;
    }
    void attach(uint32_t index) noexcept {
        auto& node=nodes_[index]; node.bucket=bucketSlot(bucketKey(node.fact)); auto& bucket=buckets_[node.bucket];
        node.previous=NoSlot; node.next=bucket.head;
        if(node.next!=NoSlot) nodes_[node.next].previous=index;
        bucket.head=index; ++bucket.version; ++work_.postingsChanged;
    }
    std::pmr::vector<Node> nodes_;
    std::pmr::vector<Bucket> buckets_; // empty buckets retain versions
    std::pmr::map<BucketKey,uint32_t> variableBuckets_; // variable keys only; targets use the paged directory
    ProgramWork& work_;
    InstructionDirectory& directory_;
    uint32_t free_=NoSlot;
    uint64_t live_{};
};

// All candidates use these values, ordering, facts and transaction rules.
template<StorageCandidate Storage>
class ProgramHost {
    struct SectionState {
        SectionId id; ScriptId script; std::string name; std::pmr::vector<InstructionId> order;
        SectionState(const Section& section,std::pmr::memory_resource* r):id(section.id),script(section.script),name(section.name),order(section.order.begin(),section.order.end(),r){}
    };
    struct OrderPatch {
        uint32_t section; std::pmr::vector<InstructionId> before,after;
        OrderPatch(uint32_t s,std::pmr::memory_resource* r):section(s),before(r),after(r){}
    };
    struct FactPatch {
        InstructionId source; std::pmr::vector<Fact> before,after;
        FactPatch(InstructionId s,std::pmr::memory_resource* r):source(s),before(r),after(r){}
    };
    struct ChangeSet {
        std::pmr::vector<StorageChange> storage;
        std::pmr::vector<OrderPatch> order;
        std::pmr::vector<FactPatch> facts;
        std::vector<Rename> names;
        explicit ChangeSet(Context& c):storage(c.resource(Domain::History)),order(c.resource(Domain::History)),facts(c.resource(Domain::History)){}
    };
    struct ReadyOrder {
        uint32_t section; std::pmr::vector<InstructionId> order;
        ReadyOrder(uint32_t s,std::span<const InstructionId> ids,std::pmr::memory_resource* r):section(s),order(ids.begin(),ids.end(),r){}
    };
public:
    class Prepared {
        friend class ProgramHost;
        const ProgramHost* owner{}; Capture capture;
        std::shared_ptr<const ChangeSet> changes;
        std::optional<typename Storage::Prepared> storage;
        std::pmr::vector<ReadyOrder> orders;
        std::vector<std::pair<uint32_t,std::string>> names;
        int historyMode{}; // 0 new, -1 undo, +1 redo
        Prepared(const ProgramHost* o,Context& c):owner(o),orders(c.resource(Domain::Preparation)){}
    public:
        Prepared(Prepared&& other) noexcept:owner(std::exchange(other.owner,nullptr)),capture(other.capture),changes(std::move(other.changes)),storage(std::move(other.storage)),orders(std::move(other.orders)),names(std::move(other.names)),historyMode(other.historyMode){}
        Prepared(const Prepared&)=delete;
    };
    explicit ProgramHost(Context& c):context_(c),store_(c),directory_(c.resource(Domain::Directory)),
        sections_(c.resource(Domain::Order)),sectionSlots_(c.resource(Domain::Directory)),operands_(c.resource(Domain::Values)),
        expressions_(c.resource(Domain::Values)),expressionSlots_(c.resource(Domain::Directory)),index_(c.resource(Domain::Index),work_,directory_),history_(c.resource(Domain::History)) {
        capture_.instance=nextInstance_.fetch_add(1);
    }
    Result<void> load(const Fixture& input) {
        try {
            const auto loadStart=Clock::now();
            expect(!loadAttempted_,"A load attempt requires a fresh host"); loadAttempted_=true;
            expect(input.project!=ProjectId{},"Missing project identity"); capture_.project=input.project;
            std::pmr::vector<uint64_t> ids(context_.resource(Domain::Preparation));
            ids.reserve(input.scripts.size()+input.sections.size()+input.expressions.size()+input.records.size()+input.reserved.size());
            auto claim=[&](uint64_t id){ expect(id!=0,"Zero project-local ID"); ids.push_back(id); highWater_=std::max(highWater_,id); };
            for(const auto& script:input.scripts) { claim(script.id.value); expect(!script.source.empty(),"Script source qualification missing"); }
            scripts_=input.scripts; evidence_=input.evidence;
            for(const auto& s:input.sections) {
                claim(s.id.value);
                expect(std::ranges::any_of(scripts_,[&](const Script& script){ return script.id==s.script; }),"Dangling section owner");
                sectionSlots_.emplace(s.id,slotCount(sections_.size())); sections_.emplace_back(s,context_.resource(Domain::Order));
            }
            for(const auto& e:input.expressions) { claim(e.id.value); validateExpression(e); expressionSlots_.ensure(e.id).slot=slotCount(expressions_.size()); expressions_.push_back(e); }
            for(const auto& r:input.records) { claim(r.id.value); auto& state=directory_.ensure(r.id); state.issued=true; }
            for(auto id:input.reserved) { claim(id.value); directory_.ensure(id).issued=true; }
            std::ranges::sort(ids); expect(std::adjacent_find(ids.begin(),ids.end())==ids.end(),"Duplicate project-local ID");
            size_t orderedCount=0;
            for(uint32_t s=0;s<sections_.size();++s) for(uint32_t rank=0;rank<sections_[s].order.size();++rank) {
                const auto id=sections_[s].order[rank];
                auto* state=directory_.find(id); expect(state && state->issued,"Order references an unknown instruction");
                expect(state->rank==NoSlot,"Instruction appears twice in authored order"); ++orderedCount;
                state->section=s; state->rank=rank;
            }
            expect(orderedCount==input.records.size(),"Order does not cover the program exactly");
            operands_.reserve(input.records.size()*3);
            std::pmr::vector<StoredRecord> rows(context_.resource(Domain::Preparation)); rows.reserve(input.records.size());
            for(const auto& r:input.records) {
                validateRecord(r);
                auto* state=directory_.find(r.id);
                expect(state->section!=NoSlot && sections_[state->section].id==r.section,"Instruction/order owner mismatch");
                if(r.op==Op::Call && !r.evidence) {
                    const auto* target=directory_.find(InstructionId{r.value}); expect(target && target->rank!=NoSlot,"Dangling resolved call target");
                }
                rows.push_back(encode(r,nullptr));
            }
            const auto storageStart=Clock::now();
            auto loaded=store_.load(rows,input.sections); if(!loaded) return loaded;
            const auto indexStart=Clock::now();
            loadPhases_[0]=std::chrono::duration_cast<std::chrono::nanoseconds>(storageStart-loadStart).count();
            loadPhases_[1]=std::chrono::duration_cast<std::chrono::nanoseconds>(indexStart-storageStart).count();
            for(const auto& row:rows) { auto& state=*directory_.find(row.id); state.live=true; state.published=true; }
            index_.reserve(input.records.size()*3);
            std::pmr::vector<Fact> facts(context_.resource(Domain::Preparation)); facts.reserve(8);
            for(const auto& section:sections_) for(auto id:section.order) {
                const auto* row=store_.lookup(id); extract(*row,nextInOrder(id),facts);
                for(auto& f:facts) f.id=FactId{allocate()};
                index_.prepareBuckets(facts); index_.reserve(facts.size());
                index_.apply(directory_.find(id)->head,facts);
            }
            loadPhases_[2]=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-indexStart).count();
            loaded_=true; work_={}; return Result<void>::success();
        } catch(const std::exception& e) { return failure<void>(e.what()); }
    }
    Capture capture() const noexcept { return capture_; }
    std::optional<Record> lookup(InstructionId id) const { const auto* row=store_.lookup(id); return row?std::optional<Record>(decode(*row)):std::nullopt; }
    template<class Sink> void enumerate(Sink&& sink) const { for(const auto& section:sections_) for(auto id:section.order) sink(decode(*store_.lookup(id))); }
    template<class Sink> void query(Query q,Sink&& sink) const {
        const auto kind=q.kind==QueryKind::Reads?FactKind::Read:q.kind==QueryKind::Writes?FactKind::Write:q.kind==QueryKind::Incoming?FactKind::Call:q.kind==QueryKind::Fallthrough?FactKind::Fallthrough:FactKind::Unknown;
        index_.query({kind,q.bank,q.key},[&](const Fact& f){ ++examined_; sink(f); });
    }
    uint64_t bucketVersion(Query q) const {
        const auto kind=q.kind==QueryKind::Reads?FactKind::Read:q.kind==QueryKind::Writes?FactKind::Write:q.kind==QueryKind::Incoming?FactKind::Call:q.kind==QueryKind::Fallthrough?FactKind::Fallthrough:FactKind::Unknown;
        return index_.version({kind,q.bank,q.key});
    }
    InstructionId allocateInstructionId() { expect(loaded_,"Host is not loaded"); ++capture_.generation; InstructionId id{allocate()}; directory_.ensure(id).issued=true; return id; }
    Result<ExprId> appendExpression(Expression expression) {
        try { expect(loaded_,"Host is not loaded"); ++capture_.generation; validateExpression(expression); expression.id=ExprId{allocate()}; const auto slot=slotCount(expressions_.size()); expressions_.push_back(expression); expressionSlots_.ensure(expression.id).slot=slot; return Result<ExprId>::success(expression.id); }
        catch(const std::exception& e) { return failure<ExprId>(e.what()); }
    }
    std::span<const InstructionId> order(SectionId id) const { return sections_.at(sectionSlot(id)).order; }
    std::string_view name(SectionId id) const { return sections_.at(sectionSlot(id)).name; }
    const auto& expressions() const noexcept { return expressions_; }
    const auto& scripts() const noexcept { return scripts_; }
    const auto& evidence() const noexcept { return evidence_; }
    const Expression& expressionNode(ExprId id) const { return expression(id); }
    template<class Sink> void enumerateSections(Sink&& sink) const { for(const auto& section:sections_) sink(section.id,section.script,section.name,std::span<const InstructionId>(section.order)); }
    uint64_t highWater() const noexcept { return highWater_; }
    const auto& loadPhases() const noexcept { return loadPhases_; }
    ProgramWork work() const noexcept { return work_; }
    uint64_t factCount() const noexcept { return index_.live(); }
    uint64_t examined() const noexcept { return examined_; }
    StorageMetrics metrics() const noexcept { return store_.metrics(); }
    Result<void> inspectStorage() const { return store_.validate(); }
    size_t historySize() const noexcept { return history_.size(); }
    uint64_t retainedOperandBytes() const noexcept { return operands_.size()*sizeof(Operand); }
    Result<Prepared> prepare(Mutation mutation,Capture expected) { return prepare(EditBatch{{mutation},{},{}},expected); }
    Result<Prepared> prepare(const EditBatch& batch,Capture expected) {
        try {
            expect(loaded_ && expected==capture_,"Stale project/revision or unloaded host"); ++capture_.generation;
            auto changes=std::allocate_shared<ChangeSet>(std::pmr::polymorphic_allocator<ChangeSet>(context_.resource(Domain::History)),context_);
            std::set<InstructionId> modified;
            std::map<InstructionId,std::optional<StoredRecord>> final;
            for(const auto& mutation:batch.mutations) {
                const auto id=mutation.before.id.value?mutation.before.id:mutation.after.id;
                expect(id.value && modified.insert(id).second,"Empty or conflicting instruction changes");
                auto* state=directory_.find(id);
                const auto* old=store_.lookup(id);
                expect(mutation.before.id.value?old && decode(*old)==mutation.before:!old && state && state->issued && !state->published,"Instruction precondition/identity mismatch");
                StorageChange change; if(old) change.before=*old;
                if(mutation.after.id.value) {
                    expect(mutation.after.id==id,"Cannot change instruction identity"); validateRecord(mutation.after);
                    if(old) expect(sections_[sectionSlot(old->section)].script==sections_[sectionSlot(mutation.after.section)].script,"Cross-script moves are not supported");
                    change.after=encode(mutation.after,old);
                }
                changes->storage.push_back(change); final.emplace(id,change.after);
            }
            auto future=[&](InstructionId id)->const StoredRecord* { const auto found=final.find(id); return found!=final.end()?(found->second?&*found->second:nullptr):store_.lookup(id); };
            // Only changed targets and existing incoming callers need reference checks.
            for(const auto& change:changes->storage) {
                if(change.after) {
                    const auto record=decode(*change.after);
                    if(record.op==Op::Call && !record.evidence) expect(future(InstructionId{record.value}),"Dangling resolved call target");
                }
                if(change.before && !change.after) index_.query({FactKind::Call,Bank::Bit,change.before->id.value},[&](const Fact& f){
                    const auto* caller=future(f.source); if(!caller) return;
                    const auto r=decode(*caller); expect(r.op!=Op::Call || r.evidence || r.value!=change.before->id.value,"Delete requires explicit incoming-call repair");
                });
            }
            auto patchFor=[&](SectionId id)->OrderPatch& {
                const auto slot=sectionSlot(id);
                for(auto& p:changes->order) if(p.section==slot) return p;
                changes->order.emplace_back(slot,context_.resource(Domain::History)); auto& patch=changes->order.back();
                patch.before=sections_[slot].order; patch.after=patch.before; work_.orderIdsCopied+=patch.before.size()*2;
                return patch;
            };
            for(const auto& change:changes->storage) {
                if(change.before && (!change.after || change.before->section!=change.after->section)) {
                    auto& order=patchFor(change.before->section).after; const auto at=std::ranges::find(order,change.before->id); expect(at!=order.end(),"Missing old order position"); order.erase(at);
                }
                if(change.after && (!change.before || change.before->section!=change.after->section)) patchFor(change.after->section).after.push_back(change.after->id);
            }
            std::set<InstructionId> positioned;
            for(const auto& place:batch.placements) {
                expect(positioned.insert(place.instruction).second,"Conflicting placements");
                const auto* record=future(place.instruction); expect(record && record->section==place.section && place.before!=place.instruction,"Invalid placement owner/anchor");
                auto& order=patchFor(place.section).after; const auto at=std::ranges::find(order,place.instruction); expect(at!=order.end(),"Placement source missing"); order.erase(at);
                const auto anchor=place.before.value?std::ranges::find(order,place.before):order.end();
                expect(!place.before.value || anchor!=order.end(),"Placement anchor missing"); order.insert(anchor,place.instruction);
            }
            std::set<SectionId> renamed;
            for(const auto& rename:batch.names) {
                expect(renamed.insert(rename.section).second && sections_[sectionSlot(rename.section)].name==rename.before,"Rename precondition mismatch");
                changes->names.push_back(rename);
            }
            // Identify changed physical fallthrough without marking every copied order ID changed.
            std::set<InstructionId> affected=modified;
            std::map<InstructionId,InstructionId> nextOverrides;
            for(const auto& patch:changes->order) {
                for(size_t i=0;i<patch.after.size();++i) {
                    const auto id=patch.after[i]; const auto next=i+1<patch.after.size()?patch.after[i+1]:InstructionId{};
                    if(next!=nextInOrder(id)) affected.insert(id);
                    nextOverrides.emplace(id,next);
                }
                for(auto id:patch.before) if(!future(id)) nextOverrides[id]=InstructionId{};
            }
            for(auto id:affected) {
                const auto* futureRow=future(id); const auto* oldRow=store_.lookup(id);
                const auto futureNext=nextOverrides.contains(id)?nextOverrides.at(id):nextInOrder(id);
                if(futureRow && oldRow && futureRow->op==oldRow->op && futureRow->firstOperand==oldRow->firstOperand && futureNext==nextInOrder(id)) continue;
                changes->facts.emplace_back(id,context_.resource(Domain::History)); auto& patch=changes->facts.back();
                if(const auto* state=directory_.find(id); state && state->live) index_.source(state->head,[&](const Fact& f){ patch.before.push_back(f); });
                if(const auto* row=future(id)) {
                    const auto next=nextOverrides.contains(id)?nextOverrides.at(id):nextInOrder(id);
                    extract(*row,next,patch.after);
                    for(auto& fact:patch.after) {
                        auto old=std::ranges::find_if(patch.before,[&](const Fact& f){ return sameRole(f,fact); });
                        fact.id=old==patch.before.end()?FactId{allocate()}:old->id;
                    }
                }
            }
            return prepareInstall(std::move(changes),0);
        } catch(const std::exception& e) { return failure<Prepared>(e.what()); }
    }
    Result<void> commit(Prepared&& plan) {
        if(plan.owner!=this || plan.capture!=capture_) return failure<void>("Stale, cancelled, wrong-host, or consumed preparation");
        const bool reverse=plan.historyMode<0;
        store_.apply(std::move(*plan.storage));
        for(const auto& change:plan.changes->storage) {
            const auto& after=reverse?change.before:change.after; const auto& before=reverse?change.after:change.before;
            const auto id=after?after->id:before->id; auto& state=*directory_.find(id);
            state.live=after.has_value(); if(after) state.published=true;
        }
        for(auto& patch:plan.orders) {
            auto& section=sections_[patch.section]; section.order.swap(patch.order);
            for(uint32_t rank=0;rank<section.order.size();++rank) { auto& state=*directory_.find(section.order[rank]); state.section=patch.section; state.rank=rank; ++work_.ranksRepaired; }
        }
        for(const auto& patch:plan.changes->facts) index_.apply(directory_.find(patch.source)->head,reverse?std::span<const Fact>(patch.before):std::span<const Fact>(patch.after));
        for(auto& [slot,name]:plan.names) sections_[slot].name.swap(name);
        if(plan.historyMode==0) { history_.resize(cursor_); history_.push_back(plan.changes); ++cursor_; }
        else if(reverse) --cursor_; else ++cursor_;
        ++capture_.revision; ++capture_.generation; plan.owner=nullptr;
        return Result<void>::success();
    }
    void cancel(Prepared& plan) noexcept { if(plan.owner==this) { plan.owner=nullptr; ++capture_.generation; } }
    Result<void> undo() { return restore(true); }
    Result<void> redo() { return restore(false); }
private:
    template<class T> static Result<T> failure(std::string message) { return Result<T>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,std::move(message))); }
    uint64_t allocate() { expect(highWater_<UINT64_MAX,"ID allocator exhausted"); return ++highWater_; }
    uint32_t sectionSlot(SectionId id) const { auto found=sectionSlots_.find(id); expect(found!=sectionSlots_.end(),"Dangling section owner"); return found->second; }
    const Expression& expression(ExprId id) const { const auto* found=expressionSlots_.find(id); expect(found && found->slot!=NoSlot,"Dangling expression"); return expressions_[found->slot]; }
    void validateExpression(const Expression& e) const {
        expect(e.op<=ExprOp::Unknown && e.bank<=Bank::Float,"Unsupported expression/bank");
        if(e.op==ExprOp::Add) { expression(e.left); expression(e.right); }
        else expect(!e.left.value && !e.right.value,"Unexpected expression children");
    }
    void validateRecord(const Record& r) const {
        sectionSlot(r.section); expect(r.op<=Op::Unknown && r.bank<=Bank::Float,"Unsupported opcode/bank");
        if(r.expression.value) { expression(r.expression); expect(r.op==Op::Write && r.value==0,"Expression value requires a write without a second literal"); }
        if(r.schedule.value) expression(r.schedule);
        if(r.evidence) expect(r.evidence<=evidence_.size() && (r.op==Op::Call || r.op==Op::Unknown),"Invalid unresolved evidence");
        if(r.op==Op::Unknown) expect(r.evidence!=0 && !r.variable,"Unknown instruction needs evidence and no inferred variable");
        if(r.op==Op::Call) expect(r.bank==Bank::Bit && !r.expression.value && !r.variable && (r.evidence?r.value==0:r.value!=0),"Invalid call binding");
    }
    StoredRecord encode(const Record& r,const StoredRecord* old) {
        if(old) {
            auto previous=decode(*old); previous.section=r.section; previous.flags=r.flags;
            if(previous==r) { auto shared=*old; shared.section=r.section; shared.flags=r.flags; return shared; }
        }
        expect(operands_.size()+3<NoSlot,"Operand pool exhausted");
        StoredRecord stored{r.id,r.section,r.op,slotCount(operands_.size()),r.flags};
        const auto id=[&](uint32_t position){ return old?operands_[old->firstOperand+position].id:OperandId{allocate()}; };
        const auto primaryId=id(0), valueId=id(1), scheduleId=id(2);
        operands_.push_back({primaryId,r.evidence?ValueKind::Unresolved:r.op==Op::Call?ValueKind::Target:ValueKind::Address,r.bank,r.evidence?r.evidence:r.op==Op::Call?r.value:r.variable});
        operands_.push_back({valueId,r.expression.value?ValueKind::Expression:ValueKind::Literal,Bank::Bit,r.expression.value?r.expression.value:r.value});
        operands_.push_back({scheduleId,r.schedule.value?ValueKind::Expression:ValueKind::Empty,Bank::Bit,r.schedule.value});
        return stored;
    }
    Record decode(const StoredRecord& row) const {
        const auto& address=operands_[row.firstOperand]; const auto& value=operands_[row.firstOperand+1]; const auto& schedule=operands_[row.firstOperand+2];
        Record r{row.id,row.section,row.op}; r.flags=row.flags; r.bank=address.bank;
        if(address.kind==ValueKind::Unresolved) r.evidence=static_cast<uint32_t>(address.value);
        else if(row.op==Op::Call) r.value=address.value; else r.variable=static_cast<uint32_t>(address.value);
        if(value.kind==ValueKind::Expression) r.expression=ExprId{value.value}; else if(row.op!=Op::Call) r.value=value.value;
        r.schedule=ExprId{schedule.value}; return r;
    }
    InstructionId nextInOrder(InstructionId id) const {
        const auto* state=directory_.find(id); if(!state || !state->live) return {};
        const auto& order=sections_[state->section].order; return state->rank+1<order.size()?order[state->rank+1]:InstructionId{};
    }
    void extract(const StoredRecord& row,InstructionId next,std::pmr::vector<Fact>& facts) {
        ++work_.instructionsExamined; facts.clear();
        const auto r=decode(row); const auto address=operands_[row.firstOperand];
        if(r.op==Op::Unknown) facts.push_back({{},r.id,address.id,FactKind::Unknown,0,Bank::Bit,0,0,{},r.evidence});
        else if(r.op==Op::Call) facts.push_back({{},r.id,address.id,FactKind::Call,0,Bank::Bit,r.value,0,{},r.evidence});
        else facts.push_back({{},r.id,address.id,r.op==Op::Read?FactKind::Read:FactKind::Write,0,r.bank,r.variable,r.value,r.expression});
        auto visit=[&](auto&& self,ExprId id,OperandId operand,uint64_t path,unsigned depth)->void {
            expect(depth<24,"Expression nesting exceeds the supported bound"); const auto& e=expression(id);
            if(e.op==ExprOp::Variable) facts.push_back({{},r.id,operand,FactKind::Read,path,e.bank,e.variable});
            else if(e.op==ExprOp::Unknown) facts.push_back({{},r.id,operand,FactKind::Unknown,path,Bank::Bit,0,0,id});
            else if(e.op==ExprOp::Add) { self(self,e.left,operand,path*3+1,depth+1); self(self,e.right,operand,path*3+2,depth+1); }
        };
        if(r.expression.value) visit(visit,r.expression,operands_[row.firstOperand+1].id,1,0);
        if(r.schedule.value) visit(visit,r.schedule,operands_[row.firstOperand+2].id,1,0);
        if(next.value) facts.push_back({{},r.id,{},FactKind::Fallthrough,0,Bank::Bit,next.value});
        work_.factsExtracted+=facts.size();
    }
    Result<Prepared> prepareInstall(std::shared_ptr<const ChangeSet> changes,int historyMode) {
        Prepared plan(this,context_); plan.capture=capture_; plan.changes=std::move(changes); plan.historyMode=historyMode;
        const bool reverse=historyMode<0;
        std::pmr::vector<StorageChange> storage(context_.resource(Domain::Preparation)); storage.reserve(plan.changes->storage.size());
        for(const auto& change:plan.changes->storage) storage.push_back(reverse?StorageChange{change.after,change.before}:change);
        auto prepared=store_.prepare(storage); if(!prepared) return Result<Prepared>::failure(prepared.diagnostics());
        plan.storage.emplace(std::move(prepared).takeValue());
        for(const auto& patch:plan.changes->order) {
            const auto& order=reverse?patch.before:patch.after;
            // Same allocator as live order allows no-allocation swap at publication.
            plan.orders.emplace_back(patch.section,order,context_.resource(Domain::Order)); work_.orderIdsCopied+=order.size();
        }
        size_t additional=0;
        for(const auto& patch:plan.changes->facts) { const auto& facts=reverse?patch.before:patch.after; index_.prepareBuckets(facts); additional+=facts.size(); }
        expect(additional<NoSlot,"Fact batch too large"); index_.reserve(additional);
        for(const auto& rename:plan.changes->names) plan.names.emplace_back(sectionSlot(rename.section),reverse?rename.before:rename.after);
        if(!historyMode) history_.reserve(cursor_+1>history_.capacity()?std::max(cursor_+1,history_.capacity()+history_.capacity()/2+1):history_.capacity());
        return Result<Prepared>::success(std::move(plan));
    }
    Result<void> restore(bool undo) {
        try {
            expect(undo?cursor_!=0:cursor_<history_.size(),"No history in requested direction"); ++capture_.generation;
            auto prepared=prepareInstall(history_[undo?cursor_-1:cursor_],undo?-1:1);
            if(!prepared) return Result<void>::failure(prepared.diagnostics());
            return commit(std::move(prepared).takeValue());
        } catch(const std::exception& e) { return failure<void>(e.what()); }
    }
    Context& context_; Storage store_; InstructionDirectory directory_;
    std::vector<Script> scripts_; std::vector<std::string> evidence_;
    std::pmr::vector<SectionState> sections_; std::pmr::map<SectionId,uint32_t> sectionSlots_;
    std::pmr::vector<Operand> operands_; std::pmr::vector<Expression> expressions_; PagedDirectory<ExpressionLocation> expressionSlots_;
    ProgramWork work_; FactIndex index_;
    std::pmr::vector<std::shared_ptr<const ChangeSet>> history_;
    size_t cursor_{}; uint64_t highWater_{}; mutable uint64_t examined_{};
    std::array<uint64_t,3> loadPhases_{};
    Capture capture_; bool loaded_{},loadAttempted_{};
    inline static std::atomic_uint64_t nextInstance_{1};
};
uint64_t hashFact(uint64_t hash,const Fact& fact);
} // namespace salsa::evaluation
