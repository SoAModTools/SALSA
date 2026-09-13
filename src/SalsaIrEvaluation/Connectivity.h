#pragma once
#include "Program.h"

namespace salsa::evaluation {
enum class SummaryIssue : uint32_t { None=0, Cycle=1, Unresolved=2, Budget=4, MissingEntry=8, UnknownValue=16 };
enum class RelationshipKind : uint8_t { SupportsGuard, OpposesGuard, UnknownEffect, Call, Transition };
// Derived identity is the source module plus supporting fact roles, not a physical slot.
struct Relationship {
    ModuleId source,target; FactId effect,guard; InstructionId instruction,condition,targetInstruction;
    RelationshipKind kind; Bank bank; uint64_t variable{},written{},required{}; ExprId schedule;
    auto operator<=>(const Relationship&) const = default;
};
struct ConnectivityMetrics { uint64_t live,slots,capacity,modules,pending,directoryBytes; };
struct ConnectivityWork { uint64_t sourcesExamined{},contributionsChanged{},modulesInvalidated{},modulesPrepared{},callsTraversed{},relationshipsEmitted{}; };

// Derived, discardable index. The host remains the sole authored authority.
class StoryConnectivity {
    struct Source { uint32_t head=NoSlot; };
    struct Node {
        Fact fact; ExprId schedule; uint32_t module=NoSlot,callee=NoSlot;
        uint32_t previous=NoSlot,next=NoSlot,sourceNext=NoSlot,reversePrevious=NoSlot,reverseNext=NoSlot;
        bool transition{},entryTarget{},live{};
    };
    struct State {
        Module declaration;
        std::array<uint32_t,4> heads{NoSlot,NoSlot,NoSlot,NoSlot}; uint32_t callers=NoSlot;
        uint64_t version{}; bool pending{},entryPresent{}; uint32_t issues{},unknownWrites{};
        std::pmr::vector<uint32_t> closure;
        State(const Module& d,std::pmr::memory_resource* r):declaration(d),closure(r){}
    };
    struct Ready {
        uint32_t module,issues{}; std::pmr::vector<uint32_t> closure;
        Ready(uint32_t m,std::pmr::memory_resource* r):module(m),closure(r){}
    };
public:
    struct Prepared {
        friend class StoryConnectivity;
    private:
        const StoryConnectivity* owner; Capture capture; uint64_t generation;
        std::pmr::vector<Ready> modules;
        Prepared(const StoryConnectivity* o,Capture c,uint64_t g,std::pmr::memory_resource* r):owner(o),capture(c),generation(g),modules(r){}
    public:
        Prepared(Prepared&& p) noexcept:owner(std::exchange(p.owner,nullptr)),capture(p.capture),generation(p.generation),modules(std::move(p.modules)){}
        Prepared(const Prepared&)=delete;
    };
    struct Watch { Bank bank; uint64_t key,version,bindingVersion; };
    struct Ticket {
        const StoryConnectivity* owner; Capture capture; ModuleId source; uint64_t version;
        std::pmr::vector<Watch> watches; bool truncated{};
        Ticket(const StoryConnectivity* o,Capture c,ModuleId m,uint64_t v,std::pmr::memory_resource* r):owner(o),capture(c),source(m),version(v),watches(r){}
    };
    explicit StoryConnectivity(Context& c):resource_(c.resource(Domain::Connectivity)),states_(resource_),moduleSlots_(resource_),
        sectionModules_(resource_),entryModules_(resource_),pendingSlots_(resource_),sources_(resource_),nodes_(resource_),guardBindings_(resource_){}

    template<class Host> void load(const Host& host) {
        expect(!loaded_,"Connectivity already loaded"); loaded_=true; capture_=host.capture();
        states_.reserve(host.modules().size());
        for(const auto& module:host.modules()) {
            expect(module.id.value && !module.sections.empty(),"Missing module identity/boundaries");
            const auto slot=slotCount(states_.size()); expect(moduleSlots_.emplace(module.id,slot).second,"Duplicate module");
            for(auto section:module.sections) {
                expect(sectionModules_.emplace(section,slot).second,"Overlapping module sections");
                bool owned=false; host.enumerateSections([&](SectionId id,ScriptId script,auto,auto){ if(id==section) owned=script==module.script; });
                expect(owned,"Module section/script owner mismatch");
            }
            expect(module.entry.value && entryModules_.emplace(module.entry,slot).second,"Missing or duplicate module entry identity");
            states_.emplace_back(module,resource_);
            const auto entry=host.lookup(module.entry); states_.back().entryPresent=entry && moduleFor(entry->section)==slot;
        }
        if(states_.empty()) { healthy_=true; return; }
        subscription_=host.subscribeChanges();
        nodes_.reserve(host.metrics().live);
        host.enumerate([&](const Record& r){ if(moduleFor(r.section)!=NoSlot) refreshSource(host,r.id); });
        for(uint32_t i=0;i<states_.size();++i) invalidate(i);
        work_={}; healthy_=true;
    }
    template<class Host> void synchronize(const Host& host) {
        expect(loaded_ && host.capture().instance==capture_.instance && host.capture().project==capture_.project,"Wrong connectivity project/host");
        healthy_=false;
        // Coalesce all publications since the last synchronization, including undo/redo.
        std::pmr::vector<InstructionId> changed(resource_),bindingTargets(resource_);
        host.changedSourcesSince(capture_.revision,[&](InstructionId id,bool bindingChanged){ changed.push_back(id); if(bindingChanged) bindingTargets.push_back(id); });
        std::ranges::sort(bindingTargets); bindingTargets.erase(std::unique(bindingTargets.begin(),bindingTargets.end()),bindingTargets.end());
        // Value/guard edits leave incoming bindings alone; only presence/owner changes need callers.
        for(auto id:bindingTargets) host.query({QueryKind::Incoming,id.value},[&](const Fact& f){ changed.push_back(f.source); });
        std::ranges::sort(changed); changed.erase(std::unique(changed.begin(),changed.end()),changed.end());
        for(auto id:changed) {
            refreshSource(host,id);
            if(const auto found=entryModules_.find(id);found!=entryModules_.end()) {
                auto& state=states_[found->second]; const auto entry=host.lookup(id);
                const bool present=entry && moduleFor(entry->section)==found->second;
                if(present!=state.entryPresent) { state.entryPresent=present; invalidate(found->second); }
            }
        }
        // Traverse only reverse helper dependencies of invalidated modules. Cycles coalesce.
        for(size_t at=0;at<pendingSlots_.size();++at) for(auto n=states_[pendingSlots_[at]].callers;n!=NoSlot;n=nodes_[n].reverseNext)
            invalidate(nodes_[n].module);
        capture_=host.capture(); host.acknowledge(subscription_,capture_); ++generation_; healthy_=true;
    }
    template<class Host> Prepared prepare(const Host& host) {
        expect(healthy_ && host.capture()==capture_,"Synchronize before preparing connectivity");
        Prepared prepared(this,capture_,generation_,resource_);
        for(auto i:pendingSlots_) {
            prepared.modules.emplace_back(i,resource_); auto& ready=prepared.modules.back(); ++work_.modulesPrepared;
            std::pmr::map<uint32_t,uint8_t> colors(resource_); uint32_t traversed=0;
            auto visit=[&](auto&& self,uint32_t m)->void {
                if(colors.contains(m)) { if(colors[m]==1) ready.issues|=uint32_t(SummaryIssue::Cycle); return; }
                if(colors.size()>=64) { ready.issues|=uint32_t(SummaryIssue::Budget); return; }
                colors[m]=1; ready.closure.push_back(m);
                const auto& state=states_[m]; const auto entry=host.lookup(state.declaration.entry);
                if(!entry || moduleFor(entry->section)!=m) ready.issues|=uint32_t(SummaryIssue::MissingEntry);
                if(state.unknownWrites) ready.issues|=uint32_t(SummaryIssue::UnknownValue);
                if(state.heads[3]!=NoSlot) ready.issues|=uint32_t(SummaryIssue::Unresolved);
                for(auto n=state.heads[2];n!=NoSlot;n=nodes_[n].next) {
                    if(++traversed>4096) { ready.issues|=uint32_t(SummaryIssue::Budget); break; }
                    ++work_.callsTraversed; const auto& node=nodes_[n];
                    if(node.callee==NoSlot || (!node.transition && !node.schedule.value && !node.entryTarget)) ready.issues|=uint32_t(SummaryIssue::Unresolved);
                    else if(inlineCall(node)) self(self,node.callee);
                }
                colors[m]=2;
            };
            visit(visit,i);
        }
        return prepared;
    }
    template<class Host> bool install(const Host& host,Prepared&& p) noexcept {
        if(!healthy_ || p.owner!=this || p.capture!=host.capture() || p.capture!=capture_ || p.generation!=generation_) return false;
        for(auto& ready:p.modules) { auto& state=states_[ready.module]; state.closure.swap(ready.closure); state.issues=ready.issues; state.pending=false; }
        pendingSlots_.clear(); p.owner=nullptr; return true;
    }
    // Relationships are streamed, never stored as the cross-product of all readers/writers.
    template<class Host,class Sink> Ticket visit(const Host& host,ModuleId source,Sink&& sink,size_t limit=100000) {
        expect(healthy_ && capture_==host.capture(),"Connectivity requires synchronization");
        const auto slot=moduleSlots_.at(source); const auto& summary=states_[slot]; expect(!summary.pending,"Connectivity summary is pending");
        Ticket ticket(this,capture_,source,summary.version,resource_); size_t emitted=0;
        std::pmr::map<std::pair<Bank,uint64_t>,Watch> watches(resource_);
        const auto emit=[&](const Relationship& r){ if(emitted==limit) { ticket.truncated=true; return; } ++emitted; ++work_.relationshipsEmitted; sink(r); };
        for(auto m:summary.closure) {
            for(auto n=states_[m].heads[0];n!=NoSlot && !ticket.truncated;n=nodes_[n].next) {
                const auto& node=nodes_[n]; const auto& f=node.fact;
                const Query query{QueryKind::Guards,f.key,f.bank}; const auto key=std::pair{f.bank,f.key};
                watches.try_emplace(key,Watch{f.bank,f.key,host.bucketVersion(query),guardVersion(key)});
                host.query(query,[&](const Fact& guard){
                    const auto record=host.lookup(guard.source); const auto target=record?moduleFor(record->section):NoSlot;
                    if(target==NoSlot || ticket.truncated) return;
                    emit({source,states_[target].declaration.id,f.id,guard.id,f.source,guard.source,states_[target].declaration.entry,
                        f.expression.value?RelationshipKind::UnknownEffect:f.value==guard.value?RelationshipKind::SupportsGuard:RelationshipKind::OpposesGuard,
                        f.bank,f.key,f.value,guard.value,node.schedule});
                });
            }
            for(auto n=states_[m].heads[2];n!=NoSlot && !ticket.truncated;n=nodes_[n].next) {
                const auto& node=nodes_[n]; if(node.callee==NoSlot) continue;
                emit({source,states_[node.callee].declaration.id,node.fact.id,{},node.fact.source,{},InstructionId{node.fact.key},
                    node.transition?RelationshipKind::Transition:RelationshipKind::Call,Bank::Bit,0,0,0,node.schedule});
            }
        }
        ticket.watches.reserve(watches.size()); for(const auto& [key,watch]:watches) ticket.watches.push_back(watch);
        return ticket;
    }
    template<class Host> bool fresh(const Host& host,const Ticket& ticket) const {
        if(!healthy_ || ticket.owner!=this || capture_!=host.capture() || ticket.capture.instance!=capture_.instance || ticket.capture.project!=capture_.project) return false;
        const auto& state=states_[moduleSlots_.at(ticket.source)]; if(state.pending || state.version!=ticket.version) return false;
        for(const auto& w:ticket.watches) if(host.bucketVersion({QueryKind::Guards,w.key,w.bank})!=w.version || guardVersion({w.bank,w.key})!=w.bindingVersion) return false;
        return true;
    }
    uint64_t version(ModuleId id) const { return states_[moduleSlots_.at(id)].version; }
    uint32_t issues(ModuleId id) const { return states_[moduleSlots_.at(id)].issues; }
    bool pending(ModuleId id) const { return states_[moduleSlots_.at(id)].pending; }
    ConnectivityWork work() const noexcept { return work_; }
    ConnectivityMetrics metrics() const noexcept { return {live_,nodes_.size(),nodes_.capacity(),states_.size(),pendingSlots_.size(),sources_.bytes()}; }
private:
    static uint32_t kind(const Fact& f) { return f.kind==FactKind::Write?0:f.kind==FactKind::Guard?1:f.kind==FactKind::Call?2:3; }
    static bool relevant(const Fact& f) { return ((f.kind==FactKind::Write || f.kind==FactKind::Guard) && (f.bank==Bank::Bit || f.bank==Bank::Byte)) || f.kind==FactKind::Call || f.kind==FactKind::Unknown; }
    static bool inlineCall(const Node& n) { return n.fact.kind==FactKind::Call && !n.transition && !n.schedule.value && n.callee!=NoSlot && n.entryTarget; }
    uint32_t moduleFor(SectionId section) const { const auto found=sectionModules_.find(section); return found==sectionModules_.end()?NoSlot:found->second; }
    uint64_t guardVersion(std::pair<Bank,uint64_t> key) const { const auto found=guardBindings_.find(key); return found==guardBindings_.end()?0:found->second; }
    void invalidate(uint32_t m) { if(m!=NoSlot && !states_[m].pending) { pendingSlots_.push_back(m); states_[m].pending=true; ++states_[m].version; ++work_.modulesInvalidated; } }
    void unlink(uint32_t at) {
        auto& n=nodes_[at]; auto& state=states_[n.module];
        if(n.previous!=NoSlot) nodes_[n.previous].next=n.next; else state.heads[kind(n.fact)]=n.next;
        if(n.next!=NoSlot) nodes_[n.next].previous=n.previous;
        if(inlineCall(n)) {
            if(n.reversePrevious!=NoSlot) nodes_[n.reversePrevious].reverseNext=n.reverseNext; else states_[n.callee].callers=n.reverseNext;
            if(n.reverseNext!=NoSlot) nodes_[n.reverseNext].reversePrevious=n.reversePrevious;
        }
        if(n.fact.kind==FactKind::Guard) ++guardBindings_[{n.fact.bank,n.fact.key}];
        if(n.fact.kind==FactKind::Write && n.fact.expression.value) --state.unknownWrites;
        invalidate(n.module);
    }
    void link(uint32_t at) {
        auto& n=nodes_[at]; auto& state=states_[n.module]; n.previous=NoSlot; n.next=state.heads[kind(n.fact)];
        if(n.next!=NoSlot) nodes_[n.next].previous=at; state.heads[kind(n.fact)]=at;
        n.reversePrevious=n.reverseNext=NoSlot;
        if(inlineCall(n)) { n.reverseNext=states_[n.callee].callers; if(n.reverseNext!=NoSlot) nodes_[n.reverseNext].reversePrevious=at; states_[n.callee].callers=at; }
        if(n.fact.kind==FactKind::Guard) ++guardBindings_[{n.fact.bank,n.fact.key}];
        if(n.fact.kind==FactKind::Write && n.fact.expression.value) ++state.unknownWrites;
        invalidate(n.module);
    }
    template<class Host> void refreshSource(const Host& host,InstructionId id) {
        ++work_.sourcesExamined; const auto record=host.lookup(id); const auto module=record?moduleFor(record->section):NoSlot;
        std::array<Node,32> desired{}; size_t count=0;
        if(module!=NoSlot) host.sourceFacts(id,[&](const Fact& fact){ if(relevant(fact)) {
            expect(count<desired.size(),"Synthetic contribution budget exceeded"); auto& n=desired[count++]; n.fact=fact; n.module=module;
            n.schedule=record->schedule; n.transition=record->op==Op::Transition;
            if(fact.kind==FactKind::Call && !fact.evidence) { const auto target=host.lookup(InstructionId{fact.key}); if(target) n.callee=moduleFor(target->section); if(n.callee!=NoSlot) n.entryTarget=states_[n.callee].declaration.entry.value==fact.key; }
        }});
        auto* existing=sources_.find(id); if(!existing && !count) return;
        auto& source=sources_.ensure(id); uint32_t previous=NoSlot,at=source.head;
        while(at!=NoSlot) {
            auto& n=nodes_[at]; const auto next=n.sourceNext;
            auto found=std::find_if(desired.begin(),desired.begin()+count,[&](const Node& d){ return d.fact.id==n.fact.id; });
            if(found==desired.begin()+count) {
                unlink(at); if(previous==NoSlot) source.head=next; else nodes_[previous].sourceNext=next;
                n.live=false; --live_; n.sourceNext=free_; free_=at; ++work_.contributionsChanged;
            } else {
                if(n.fact!=found->fact || n.module!=found->module || n.callee!=found->callee || n.schedule!=found->schedule || n.transition!=found->transition || n.entryTarget!=found->entryTarget) {
                    unlink(at); n.fact=found->fact; n.module=found->module; n.callee=found->callee; n.schedule=found->schedule; n.transition=found->transition; n.entryTarget=found->entryTarget; link(at); ++work_.contributionsChanged;
                }
                found->live=true; previous=at;
            }
            at=next;
        }
        for(size_t i=0;i<count;++i) if(!desired[i].live) {
            uint32_t slot;
            if(free_!=NoSlot) { slot=free_; free_=nodes_[slot].sourceNext; }
            else { slot=slotCount(nodes_.size()); nodes_.emplace_back(); }
            nodes_[slot]=desired[i]; nodes_[slot].live=true; ++live_; nodes_[slot].sourceNext=source.head; source.head=slot; link(slot); ++work_.contributionsChanged;
        }
    }
    std::pmr::memory_resource* resource_;
    std::pmr::vector<State> states_; std::pmr::map<ModuleId,uint32_t> moduleSlots_; std::pmr::map<SectionId,uint32_t> sectionModules_;
    std::pmr::map<InstructionId,uint32_t> entryModules_; std::pmr::vector<uint32_t> pendingSlots_;
    PagedDirectory<Source> sources_; std::pmr::vector<Node> nodes_; std::pmr::map<std::pair<Bank,uint64_t>,uint64_t> guardBindings_;
    ChangeCursor subscription_;
    Capture capture_; ConnectivityWork work_; uint32_t free_=NoSlot; uint64_t generation_{},live_{}; bool loaded_{},healthy_{};
};
} // namespace salsa::evaluation
