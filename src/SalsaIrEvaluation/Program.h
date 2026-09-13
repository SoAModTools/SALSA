#pragma once
#include "Reference.h"
#include "StorageSupport.h"
#include "Lifetime.h"
#include "ProgramInput.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace salsa::evaluation {
struct InstructionState { bool live{}, issued{}, published{}; uint32_t section=NoSlot,rank=NoSlot,head=NoSlot,callBucket=NoSlot,fallthroughBucket=NoSlot; };
using InstructionDirectory=PagedDirectory<InstructionState>;
struct ExpressionLocation { uint32_t slot=NoSlot; };

enum class FactKind : uint8_t { Read, Write, Call, Fallthrough, Unknown, Guard };
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

inline std::atomic_uint64_t NextProgramInstance{1};

// All candidates use these values, ordering, facts and transaction rules.
template<StorageCandidate Storage>
class ProgramHost {
    struct SectionState {
        SectionId id; ScriptId script; std::string name; OrderList order;
        template<class SectionRow> SectionState(const SectionRow& section,std::pmr::memory_resource* r,uint64_t& copied,bool shared):id(section.id),script(section.script),name(section.name),order(section.order,r,copied,shared){}
    };
    struct OrderPatch {
        uint32_t section; OrderList before,after;
        OrderPatch(uint32_t s,std::pmr::memory_resource* r,uint64_t& copied,bool shared):section(s),before(r,copied,shared),after(r,copied,shared){}
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
    struct SourceChange { InstructionId id; bool bindingChanged; };
    struct Publication { std::pmr::vector<SourceChange> sources; explicit Publication(std::pmr::vector<SourceChange>&& s):sources(std::move(s)){} };
    struct ReadyOrder {
        uint32_t section; OrderList order;
        ReadyOrder(uint32_t s,const OrderList& ids,std::pmr::memory_resource*):section(s),order(ids){}
    };
public:
    class Prepared {
        friend class ProgramHost;
        const ProgramHost* owner{}; Capture capture;
        std::shared_ptr<const ChangeSet> changes;
        std::optional<typename Storage::Prepared> storage;
        std::pmr::vector<ReadyOrder> orders;
        std::vector<std::pair<uint32_t,std::string>> names;
        std::pmr::vector<SourceChange> notice;
        int historyMode{}; // 0 new, -1 undo, +1 redo
        Prepared(const ProgramHost* o,Context& c):owner(o),orders(c.resource(Domain::Preparation)),notice(c.resource(Domain::History)){}
    public:
        Prepared(Prepared&& other) noexcept:owner(std::exchange(other.owner,nullptr)),capture(other.capture),changes(std::move(other.changes)),storage(std::move(other.storage)),orders(std::move(other.orders)),names(std::move(other.names)),notice(std::move(other.notice)),historyMode(other.historyMode){}
        Prepared(const Prepared&)=delete;
    };
    explicit ProgramHost(Context& c,bool sharedOrder=true):context_(c),store_(c),directory_(c.resource(Domain::Directory)),
        sections_(c.resource(Domain::Order)),sectionSlots_(c.resource(Domain::Directory)),operands_(c.resource(Domain::Values)),
        expressions_(c.resource(Domain::Values)),expressionSlots_(c.resource(Domain::Directory)),index_(c.resource(Domain::Index),work_,directory_),history_(c.resource(Domain::History)),publications_(c.resource(Domain::History)),changeRoots_(c.resource(Domain::History)),sharedOrder_(sharedOrder) {
        capture_.instance=NextProgramInstance.fetch_add(1);
    }
    template<class Input> Result<void> load(const Input& input) {
        try {
            const auto loadStart=Clock::now();
            expect(!loadAttempted_,"A load attempt requires a fresh host"); loadAttempted_=true;
            expect(input.project!=ProjectId{},"Missing project identity"); capture_.project=input.project;
            std::pmr::vector<uint64_t> ids(context_.resource(Domain::Preparation));
            ids.reserve(input.scripts.size()+input.sections.size()+input.expressions.size()+input.modules.size()+input.records.size()+input.reserved.size());
            auto claim=[&](uint64_t id){ expect(id!=0,"Zero project-local ID"); ids.push_back(id); highWater_=std::max(highWater_,id); };
            for(const auto& script:input.scripts) { claim(script.id.value); expect(!script.source.empty(),"Script source qualification missing"); }
            scripts_=input.scripts; evidence_=input.evidence; modules_=input.modules;
            for(const auto& module:modules_) claim(module.id.value);
            for(const auto& s:input.sections) {
                claim(s.id.value);
                expect(std::ranges::any_of(scripts_,[&](const Script& script){ return script.id==s.script; }),"Dangling section owner");
                sectionSlots_.emplace(s.id,slotCount(sections_.size())); sections_.emplace_back(s,context_.resource(Domain::Order),work_.orderIdsCopied,sharedOrder_);
            }
            for(const auto& e:input.expressions) { claim(e.id.value); validateExpression(e); expressionSlots_.ensure(e.id).slot=slotCount(expressions_.size()); expressions_.push_back(e); }
            for(const auto& r:input.records) { claim(r.id.value); auto& state=directory_.ensure(r.id); state.issued=true; }
            for(auto id:input.reserved) { claim(id.value); directory_.ensure(id).issued=true; }
            const auto identity=[&](InstructionId id)->std::optional<IdentityInput> {
                if(!input.checkpoint) return std::nullopt;
                const auto& seeds=input.checkpoint->identities;
                auto at=std::ranges::lower_bound(seeds,id,{},[](const auto& seed){return seed.instruction;});
                expect(at!=seeds.end() && (*at).instruction==id,"Missing persisted instruction identity"); return identityInput(*at);
            };
            if(input.checkpoint) {
                const auto& saved=*input.checkpoint;
                expect(saved.identities.size()==input.records.size(),"Incomplete persisted identity roles");
                InstructionId previous;
                for(const auto& seed:saved.identities) {
                    expect(seed.instruction>previous,"Duplicate/unordered persisted identity"); previous=seed.instruction;
                    for(auto id:seed.operands) claim(id.value);
                    for(const auto& fact:seed.facts) claim(fact.id.value);
                }
                expect(saved.highWater>=highWater_ && saved.highWater<UINT64_MAX && saved.revision<UINT64_MAX,"Invalid persisted allocator/revision");
                highWater_=saved.highWater; capture_.revision=saved.revision; publicationBase_=saved.revision;
            }
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
                if(isControl(r.op) && !r.evidence) {
                    const auto* target=directory_.find(InstructionId{r.value}); expect(target && target->rank!=NoSlot,"Dangling resolved call target");
                }
                const auto seed=identity(r.id); rows.push_back(encode(r,nullptr,seed?&*seed:nullptr));
            }
            const auto storageStart=Clock::now();
            std::pmr::vector<SectionLayout> layouts(context_.resource(Domain::Preparation)); layouts.reserve(sections_.size());
            for(const auto& section:sections_) layouts.push_back({section.id,section.order.size()});
            auto loaded=store_.load(rows,layouts); if(!loaded) return loaded;
            const auto indexStart=Clock::now();
            loadPhases_[0]=std::chrono::duration_cast<std::chrono::nanoseconds>(storageStart-loadStart).count();
            loadPhases_[1]=std::chrono::duration_cast<std::chrono::nanoseconds>(indexStart-storageStart).count();
            for(const auto& row:rows) { auto& state=*directory_.find(row.id); state.live=true; state.published=true; }
            index_.reserve(input.records.size()*3);
            std::pmr::vector<Fact> facts(context_.resource(Domain::Preparation)); facts.reserve(8);
            for(const auto& section:sections_) for(auto id:section.order) {
                const auto* row=store_.lookup(id); extract(*row,nextInOrder(id),facts);
                if(const auto seed=identity(id)) {
                    expect(seed->facts.size()==facts.size(),"Persisted fact roles do not match content");
                    for(auto& f:facts) {
                        auto at=std::ranges::find_if(seed->facts,[&](const FactIdentity& v){ return v.operand==f.operand && v.kind==static_cast<uint8_t>(f.kind) && v.role==f.role; });
                        expect(at!=seed->facts.end(),"Missing persisted fact role"); f.id=(*at).id;
                    }
                } else for(auto& f:facts) f.id=FactId{allocate()};
                index_.prepareBuckets(facts); index_.reserve(facts.size());
                index_.apply(directory_.find(id)->head,facts);
            }
            loadPhases_[2]=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-indexStart).count();
            loaded_=true; work_={}; return Result<void>::success();
        } catch(const std::exception& e) { return failure<void>(e.what()); }
    }
    Capture capture() const noexcept { return capture_; }
    // Explicit save capture. No history, private layout, caches or baseline bytes.
    Fixture checkpoint() const {
        expect(loaded_,"Host is not loaded"); Fixture saved; saved.project=capture_.project;
        saved.scripts=scripts_; saved.modules=modules_; saved.evidence=evidence_;
        saved.expressions.assign(expressions_.begin(),expressions_.end());
        saved.checkpoint=Checkpoint{capture_.revision,highWater_,{}};
        saved.records.reserve(store_.metrics().live); saved.checkpoint->identities.reserve(store_.metrics().live);
        for(const auto& section:sections_) {
            saved.sections.push_back({section.id,section.script,section.name,{section.order.begin(),section.order.end()}});
            for(auto id:section.order) {
                const auto* row=store_.lookup(id); saved.records.push_back(decode(*row)); RecordIdentity seed; seed.instruction=id;
                for(uint32_t i=0;i<3;++i) seed.operands[i]=operands_[row->firstOperand+i].id;
                sourceFacts(id,[&](const Fact& f){ seed.facts.push_back({f.id,f.operand,static_cast<uint8_t>(f.kind),f.role}); });
                std::ranges::sort(seed.facts,{},&FactIdentity::id); saved.checkpoint->identities.push_back(std::move(seed));
            }
        }
        std::ranges::sort(saved.checkpoint->identities,{},&RecordIdentity::instruction);
        directory_.forEachAllocated([&](uint64_t id,const InstructionState& state){ if(state.issued && !state.published) saved.reserved.push_back(InstructionId{id}); });
        return saved;
    }
    std::optional<Record> lookup(InstructionId id) const { const auto* row=store_.lookup(id); return row?std::optional<Record>(decode(*row)):std::nullopt; }
    template<class Sink> void enumerate(Sink&& sink) const { for(const auto& section:sections_) for(auto id:section.order) sink(decode(*store_.lookup(id))); }
    template<class Sink> void query(Query q,Sink&& sink) const {
        const auto kind=q.kind==QueryKind::Reads?FactKind::Read:q.kind==QueryKind::Writes?FactKind::Write:q.kind==QueryKind::Incoming?FactKind::Call:q.kind==QueryKind::Fallthrough?FactKind::Fallthrough:q.kind==QueryKind::Guards?FactKind::Guard:FactKind::Unknown;
        index_.query({kind,q.bank,q.key},[&](const Fact& f){ ++examined_; sink(f); });
    }
    uint64_t bucketVersion(Query q) const {
        const auto kind=q.kind==QueryKind::Reads?FactKind::Read:q.kind==QueryKind::Writes?FactKind::Write:q.kind==QueryKind::Incoming?FactKind::Call:q.kind==QueryKind::Fallthrough?FactKind::Fallthrough:q.kind==QueryKind::Guards?FactKind::Guard:FactKind::Unknown;
        return index_.version({kind,q.bank,q.key});
    }
    InstructionId allocateInstructionId() { expect(loaded_,"Host is not loaded"); ++capture_.generation; InstructionId id{allocate()}; directory_.ensure(id).issued=true; return id; }
    Result<ExprId> appendExpression(Expression expression) {
        try { expect(loaded_,"Host is not loaded"); ++capture_.generation; validateExpression(expression); expression.id=ExprId{allocate()}; const auto slot=slotCount(expressions_.size()); expressions_.push_back(expression); expressionSlots_.ensure(expression.id).slot=slot; return Result<ExprId>::success(expression.id); }
        catch(const std::exception& e) { return failure<ExprId>(e.what()); }
    }
    std::span<const InstructionId> order(SectionId id) const { const auto& order=sections_.at(sectionSlot(id)).order; return {order.data(),order.size()}; }
    std::string_view name(SectionId id) const { return sections_.at(sectionSlot(id)).name; }
    const auto& expressions() const noexcept { return expressions_; }
    const auto& modules() const noexcept { return modules_; }
    template<class Sink> void sourceFacts(InstructionId id,Sink&& sink) const {
        const auto* state=directory_.find(id); if(state && state->live) index_.source(state->head,std::forward<Sink>(sink));
    }
    ChangeCursor subscribeChanges() const {
        auto cursor=std::make_shared<ChangeCursorState>(ChangeCursorState{capture_}); subscriptions_.push_back(cursor); return cursor;
    }
    void acknowledge(const ChangeCursor& cursor,Capture captured) const {
        expect(cursor && cursor->capture.instance==capture_.instance && captured==capture_,"Wrong or stale change cursor"); cursor->capture=captured;
    }
    // Compact receipts keep identities, not old operand/order/history payloads.
    template<class Sink> void changedSourcesSince(uint64_t revision,Sink&& sink) const {
        expect(revision>=publicationBase_ && revision<=capture_.revision,"Change receipts retired; rebuild derived state");
        for(size_t i=static_cast<size_t>(revision-publicationBase_);i<publications_.size();++i)
            for(const auto& source:publications_[i].sources) sink(source.id,source.bindingChanged);
    }
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
    uint64_t retainedOperandBytes() const noexcept { return operands_.metrics().bundles*3*sizeof(Operand); }
    Result<Prepared> prepare(Mutation mutation,Capture expected) { return prepare(EditBatch{{mutation},{},{}},expected); }
    Result<Prepared> prepare(const EditBatch& batch,Capture expected) {
        try {
            expect(loaded_ && expected==capture_,"Stale project/revision or unloaded host"); ++capture_.generation;
            auto changes=std::allocate_shared<ChangeSet>(std::pmr::polymorphic_allocator<ChangeSet>(context_.resource(Domain::History)),context_);
            changeRoots_.push_back(changes);
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
                    if(isControl(record.op) && !record.evidence) expect(future(InstructionId{record.value}),"Dangling resolved call target");
                }
                if(change.before && !change.after) index_.query({FactKind::Call,Bank::Bit,change.before->id.value},[&](const Fact& f){
                    const auto* caller=future(f.source); if(!caller) return;
                    const auto r=decode(*caller); expect(!isControl(r.op) || r.evidence || r.value!=change.before->id.value,"Delete requires explicit incoming-call repair");
                });
            }
            auto patchFor=[&](SectionId id)->OrderPatch& {
                const auto slot=sectionSlot(id);
                for(auto& p:changes->order) if(p.section==slot) return p;
                changes->order.emplace_back(slot,context_.resource(Domain::Order),work_.orderIdsCopied,sharedOrder_); auto& patch=changes->order.back();
                patch.before=sections_[slot].order; patch.after=patch.before;
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
                const auto& section=sections_[sectionSlot(rename.section)];
                const auto script=std::ranges::find(scripts_,section.script,&Script::id);
                expect(!script->native || (section.name!="init" && section.name!="loop") || rename.after==rename.before,"Required lifecycle section cannot be renamed");
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
        publications_.emplace_back(std::move(plan.notice));
        ++capture_.revision; ++capture_.generation; plan.owner=nullptr; plan.changes.reset(); plan.storage.reset(); plan.orders.clear(); plan.names.clear(); plan.notice.clear();
        return Result<void>::success();
    }
    void cancel(Prepared& plan) noexcept { if(plan.owner==this) { plan.owner=nullptr; plan.changes.reset(); plan.storage.reset(); plan.orders.clear(); plan.names.clear(); plan.notice.clear(); ++capture_.generation; } }
    Result<void> undo() { return restore(true); }
    Result<void> redo() { return restore(false); }
    void clearHistory() {
        std::pmr::vector<std::shared_ptr<const ChangeSet>> empty(context_.resource(Domain::History)); history_.swap(empty);
        cursor_=0; ++capture_.generation; // outstanding history installations are now obsolete
    }
    struct Maintenance {
        uint64_t bundlesBefore{},rootedBundles{},bundlesFreed{},pagesFreed{},bytesFreed{},expressionsRetired{},receiptsRetired{};
    };
    Maintenance reclaim(bool retireUnreferencedExpressions=false) {
        expect(loaded_,"Host is not loaded"); Maintenance result;
        const auto before=operands_.metrics(); result.bundlesBefore=before.bundles;
        operands_.beginMark();
        store_.forEachPhysical([&](const StoredRecord& r){ operands_.mark(r.firstOperand); });
        for(const auto& weak:changeRoots_) if(const auto root=weak.lock()) for(const auto& c:root->storage) {
            if(c.before) operands_.mark(c.before->firstOperand);
            if(c.after) operands_.mark(c.after->firstOperand);
        }
        result.rootedBundles=operands_.markedBundles();
        std::pmr::vector<Expression> retained(context_.resource(Domain::Values));
        if(retireUnreferencedExpressions) {
            std::set<ExprId> reachable;
            auto visit=[&](auto&& self,ExprId id)->void {
                if(!id.value || !reachable.insert(id).second) return;
                const auto& e=expression(id); self(self,e.left); self(self,e.right);
            };
            operands_.markedValues([&](const Operand& v){ if(v.kind==ValueKind::Expression) visit(visit,ExprId{v.value}); });
            retained.reserve(reachable.size()); for(const auto& e:expressions_) if(reachable.contains(e.id)) retained.push_back(e);
        }
        operands_.sweep(); // survivors keep their handles; no candidate record relocation
        if(retireUnreferencedExpressions) {
            result.expressionsRetired=expressions_.size()-retained.size();
            for(const auto& e:expressions_) expressionSlots_.find(e.id)->slot=NoSlot;
            expressions_.swap(retained);
            for(uint32_t i=0;i<expressions_.size();++i) expressionSlots_.find(expressions_[i].id)->slot=i;
            if(result.expressionsRetired) ++capture_.generation;
        }
        std::erase_if(changeRoots_,[](const auto& weak){ return weak.expired(); });
        auto acknowledged=capture_.revision;
        std::erase_if(subscriptions_,[](const auto& weak){ return weak.expired(); });
        for(const auto& weak:subscriptions_) if(const auto cursor=weak.lock()) acknowledged=std::min(acknowledged,cursor->capture.revision);
        result.receiptsRetired=acknowledged-publicationBase_;
        publications_.erase(publications_.begin(),publications_.begin()+static_cast<size_t>(result.receiptsRetired));
        publicationBase_=acknowledged;
        const auto after=operands_.metrics();
        result.bundlesFreed=before.bundles-after.bundles; result.pagesFreed=before.pages-after.pages; result.bytesFreed=before.bytes-after.bytes;
        return result;
    }
    auto payloadMetrics() const noexcept { return operands_.metrics(); }
    const ReaderGauges& readerMetrics() const noexcept { return *readerGauges_; }
    bool accepts(Capture captured) const noexcept { return captured==capture_; }
    std::shared_ptr<const ReadSnapshot<Fact>> captureRead(std::span<const SectionId> selected={},bool whole=true) const {
        expect(loaded_,"Host is not loaded");
        auto read=std::make_shared<ReadSnapshot<Fact>>(readerGauges_); read->capture_=capture_; read->whole_=whole;
        std::vector<uint32_t> slots;
        if(whole) { slots.reserve(sections_.size()); for(uint32_t i=0;i<sections_.size();++i) slots.push_back(i); }
        else { for(auto id:selected) slots.push_back(sectionSlot(id)); std::ranges::sort(slots); slots.erase(std::unique(slots.begin(),slots.end()),slots.end()); }
        size_t records=0; for(auto slot:slots) records+=sections_[slot].order.size();
        read->records_.reserve(records); read->facts_.reserve(records*3); read->sections_.reserve(slots.size());
        std::set<ScriptId> scripts; std::set<ExprId> expressions;
        auto visitExpression=[&](auto&& self,ExprId id)->void {
            if(!id.value || !expressions.insert(id).second) return;
            const auto& e=expression(id); self(self,e.left); self(self,e.right);
        };
        for(auto slot:slots) {
            const auto& s=sections_[slot]; scripts.insert(s.script);
            read->sections_.push_back({s.id,s.script,s.name,{s.order.begin(),s.order.end()}});
            for(auto id:s.order) {
                auto record=decode(*store_.lookup(id)); read->records_.push_back(record);
                sourceFacts(id,[&](const Fact& f){ read->facts_.push_back(f); });
                visitExpression(visitExpression,record.expression); visitExpression(visitExpression,record.schedule);
            }
        }
        std::ranges::sort(read->records_,{},&Record::id);
        for(const auto& s:scripts_) if(scripts.contains(s.id)) read->scripts_.push_back(s);
        read->expressions_.reserve(expressions.size()); for(auto id:expressions) read->expressions_.push_back(expression(id));
        read->evidence_=evidence_; // preserve 1-based evidence indices, including scope-external entries
        uint64_t bytes=sizeof(*read)+read->records_.capacity()*sizeof(Record)+read->facts_.capacity()*sizeof(Fact)+read->expressions_.capacity()*sizeof(Expression)
            +read->sections_.capacity()*sizeof(Section)+read->scripts_.capacity()*sizeof(Script)+read->evidence_.capacity()*sizeof(std::string);
        for(const auto& s:read->sections_) bytes+=s.name.capacity()+s.order.capacity()*sizeof(InstructionId);
        for(const auto& s:read->scripts_) bytes+=s.name.capacity()+s.source.capacity();
        for(const auto& e:read->evidence_) bytes+=e.capacity();
        read->bytes_=bytes; readerGauges_->add(bytes);
        return read;
    }
private:
    template<class T> static Result<T> failure(std::string message) { return Result<T>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,std::move(message))); }
    uint64_t allocate() { expect(highWater_<UINT64_MAX,"ID allocator exhausted"); return ++highWater_; }
    uint32_t sectionSlot(SectionId id) const { auto found=sectionSlots_.find(id); expect(found!=sectionSlots_.end(),"Dangling section owner"); return found->second; }
    const Expression& expression(ExprId id) const { const auto* found=expressionSlots_.find(id); expect(found && found->slot!=NoSlot,"Dangling expression"); return expressions_[found->slot]; }
    void validateExpression(const Expression& e) const {
        expect(e.op<=ExprOp::Fixed && e.bank<=Bank::Float,"Unsupported expression/bank");
        if(e.op==ExprOp::Add) { expression(e.left); expression(e.right); }
        else expect(!e.left.value && !e.right.value,"Unexpected expression children");
    }
    void validateRecord(const Record& r) const {
        sectionSlot(r.section); expect(r.op<=Op::Sleep && r.bank<=Bank::Float,"Unsupported opcode/bank");
        if(r.expression.value) { expression(r.expression); expect((r.op==Op::Write || r.op==Op::Sleep) && r.value==0,"Expression value requires a write/wait without a second literal"); }
        if(r.schedule.value) expression(r.schedule);
        if(r.evidence) expect(r.evidence<=evidence_.size() && (isControl(r.op) || r.op==Op::Unknown),"Invalid unresolved evidence");
        if(r.op==Op::Guard) expect((r.bank==Bank::Bit || r.bank==Bank::Byte) && r.value<=(r.bank==Bank::Bit?1:255) && !r.expression.value && !r.schedule.value,"Unsupported guard; use literal bit/byte equality");
        if(r.op==Op::Unknown) expect(r.evidence!=0 && !r.variable,"Unknown instruction needs evidence and no inferred variable");
        if(r.op==Op::Return || r.op==Op::Frame || r.op==Op::Sleep) {
            expect(r.bank==Bank::Bit && !r.variable && !r.evidence,"Native terminal/wait has unexpected address/evidence");
            if(r.op!=Op::Sleep) expect(!r.value && !r.expression.value,"Native terminal/frame has unexpected value");
        }
        if(isControl(r.op)) expect(r.bank==Bank::Bit && !r.expression.value && !r.variable && (r.evidence?r.value==0:r.value!=0),"Invalid call binding");
    }
    StoredRecord encode(const Record& r,const StoredRecord* old,const IdentityInput* seed=nullptr) {
        if(old) {
            auto previous=decode(*old); previous.section=r.section; previous.flags=r.flags;
            if(previous==r) { auto shared=*old; shared.section=r.section; shared.flags=r.flags; return shared; }
        }
        StoredRecord stored{r.id,r.section,r.op,0,r.flags};
        const auto id=[&](uint32_t position){ return old?operands_[old->firstOperand+position].id:seed?seed->operands[position]:OperandId{allocate()}; };
        const auto primaryId=id(0), valueId=id(1), scheduleId=id(2);
        stored.firstOperand=operands_.append({Operand{primaryId,r.evidence?ValueKind::Unresolved:isControl(r.op)?ValueKind::Target:ValueKind::Address,r.bank,r.evidence?r.evidence:isControl(r.op)?r.value:r.variable},
            Operand{valueId,r.expression.value?ValueKind::Expression:ValueKind::Literal,Bank::Bit,r.expression.value?r.expression.value:r.value},
            Operand{scheduleId,r.schedule.value?ValueKind::Expression:ValueKind::Empty,Bank::Bit,r.schedule.value}});
        return stored;
    }
    Record decode(const StoredRecord& row) const {
        const auto& address=operands_[row.firstOperand]; const auto& value=operands_[row.firstOperand+1]; const auto& schedule=operands_[row.firstOperand+2];
        Record r{row.id,row.section,row.op}; r.flags=row.flags; r.bank=address.bank;
        if(address.kind==ValueKind::Unresolved) r.evidence=static_cast<uint32_t>(address.value);
        else if(isControl(row.op)) r.value=address.value; else r.variable=static_cast<uint32_t>(address.value);
        if(value.kind==ValueKind::Expression) r.expression=ExprId{value.value}; else if(!isControl(row.op)) r.value=value.value;
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
        else if(isControl(r.op)) facts.push_back({{},r.id,address.id,FactKind::Call,0,Bank::Bit,r.value,0,{},r.evidence});
        else if(r.op==Op::Read || r.op==Op::Guard || r.op==Op::Write) facts.push_back({{},r.id,address.id,r.op==Op::Read?FactKind::Read:r.op==Op::Guard?FactKind::Guard:FactKind::Write,0,r.bank,r.variable,r.value,r.expression});
        auto visit=[&](auto&& self,ExprId id,OperandId operand,uint64_t path,unsigned depth)->void {
            expect(depth<24,"Expression nesting exceeds the supported bound"); const auto& e=expression(id);
            if(e.op==ExprOp::Variable) facts.push_back({{},r.id,operand,FactKind::Read,path,e.bank,e.variable});
            else if(e.op==ExprOp::Unknown) facts.push_back({{},r.id,operand,FactKind::Unknown,path,Bank::Bit,0,0,id});
            else if(e.op==ExprOp::Add) { self(self,e.left,operand,path*3+1,depth+1); self(self,e.right,operand,path*3+2,depth+1); }
        };
        if(r.expression.value) visit(visit,r.expression,operands_[row.firstOperand+1].id,1,0);
        if(r.schedule.value) visit(visit,r.schedule,operands_[row.firstOperand+2].id,1,0);
        if(next.value && (r.schedule.value || (r.op!=Op::Jump && r.op!=Op::Return))) facts.push_back({{},r.id,{},FactKind::Fallthrough,0,Bank::Bit,next.value});
        work_.factsExtracted+=facts.size();
    }
    Result<Prepared> prepareInstall(std::shared_ptr<const ChangeSet> changes,int historyMode) {
        Prepared plan(this,context_); plan.capture=capture_; plan.changes=std::move(changes); plan.historyMode=historyMode;
        const bool reverse=historyMode<0;
        std::pmr::vector<StorageChange> storage(context_.resource(Domain::Preparation)); storage.reserve(plan.changes->storage.size());
        for(const auto& change:plan.changes->storage) storage.push_back(reverse?StorageChange{change.after,change.before}:change);
        auto prepared=store_.prepare(storage); if(!prepared) return Result<Prepared>::failure(prepared.diagnostics());
        plan.storage.emplace(std::move(prepared).takeValue());
        plan.notice.reserve(storage.size());
        for(const auto& change:storage) plan.notice.push_back({change.before?change.before->id:change.after->id,!change.before || !change.after || change.before->section!=change.after->section});
        for(const auto& patch:plan.changes->order) {
            const auto& order=reverse?patch.before:patch.after;
            // Same allocator as live order allows no-allocation swap at publication.
            plan.orders.emplace_back(patch.section,order,context_.resource(Domain::Order));
        }
        size_t additional=0;
        for(const auto& patch:plan.changes->facts) { const auto& facts=reverse?patch.before:patch.after; index_.prepareBuckets(facts); additional+=facts.size(); }
        expect(additional<NoSlot,"Fact batch too large"); index_.reserve(additional);
        for(const auto& rename:plan.changes->names) plan.names.emplace_back(sectionSlot(rename.section),reverse?rename.before:rename.after);
        if(!historyMode) history_.reserve(cursor_+1>history_.capacity()?std::max(cursor_+1,history_.capacity()+history_.capacity()/2+1):history_.capacity());
        if(publications_.size()==publications_.capacity()) publications_.reserve(publications_.size()+publications_.size()/2+1);
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
    std::vector<Module> modules_; std::vector<Script> scripts_; std::vector<std::string> evidence_;
    std::pmr::vector<SectionState> sections_; std::pmr::map<SectionId,uint32_t> sectionSlots_;
    OperandPool operands_; std::pmr::vector<Expression> expressions_; PagedDirectory<ExpressionLocation> expressionSlots_;
    ProgramWork work_; FactIndex index_;
    std::pmr::vector<std::shared_ptr<const ChangeSet>> history_;
    std::pmr::vector<Publication> publications_;
    std::pmr::vector<std::weak_ptr<const ChangeSet>> changeRoots_;
    mutable std::vector<std::weak_ptr<ChangeCursorState>> subscriptions_;
    std::shared_ptr<ReaderGauges> readerGauges_=std::make_shared<ReaderGauges>();
    uint64_t publicationBase_{}; bool sharedOrder_;
    size_t cursor_{}; uint64_t highWater_{}; mutable uint64_t examined_{};
    std::array<uint64_t,3> loadPhases_{};
    Capture capture_; bool loaded_{},loadAttempted_{};
};
uint64_t hashFact(uint64_t hash,const Fact& fact);
} // namespace salsa::evaluation
