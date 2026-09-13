#include "Program.h"
#include "PagedStorage.h"
#include "Connectivity.h"
#include <iostream>
#include <future>
#include <stdexcept>
#include <Windows.h>

namespace salsa::evaluation {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void lifetimeSmoke() {
    Context context; auto fixture=makeFixture(64,1); ProgramHost<DenseStorage> host(context);
    require(host.load(fixture).ok(),"Lifetime load failed");
    auto reader=host.captureRead(); const auto original=*reader->lookup({100});
    auto pendingRecord=original; pendingRecord.variable=777;
    auto pendingResult=host.prepare({original,pendingRecord},host.capture()); require(pendingResult.ok(),"Pending preparation failed");
    auto pending=std::move(pendingResult).takeValue();
    std::promise<void> release; auto gate=release.get_future();
    auto worker=std::async(std::launch::async,[read=reader,gate=std::move(gate)]() mutable { gate.wait(); return *read->lookup({100}); });
    struct Unblock { std::promise<void>& promise; ~Unblock(){ try { promise.set_value(); } catch(...) {} } } unblock{release};
    for(uint64_t i=0;i<512;++i) {
        Expression expression; expression.op=ExprOp::Literal; expression.literal=i;
        auto id=host.appendExpression(expression); require(id.ok(),"Expression allocation failed");
        auto before=*host.lookup({100}); auto after=before; after.expression=id.value(); after.value=0;
        auto prepared=host.prepare({before,after},host.capture()); require(prepared.ok() && host.commit(std::move(prepared).takeValue()).ok(),"Lifetime edit failed");
        if(i%8==0) require(host.undo().ok() && host.redo().ok(),"Lifetime undo/redo failed");
    }
    auto retained=host.reclaim(); require(retained.rootedBundles>host.metrics().live,"History/preparation payloads were not retained");
    host.clearHistory(); auto released=host.reclaim(true);
    require(released.rootedBundles==host.metrics().live+2,"Pending preparation roots were not preserved");
    require(released.bytesFreed>0 && released.expressionsRetired>=511,"Released history did not reclaim payloads/expressions");
    require(!host.accepts(reader->capture()) && *reader->lookup({100})==original,"Reader revision/content changed");
    release.set_value(); require(worker.get()==original,"Detached worker read changed after reclamation");
    host.cancel(pending); auto cancelled=host.reclaim(true);
    require(cancelled.rootedBundles==host.metrics().live && cancelled.bundlesFreed==2,"Cancelled preparation retained operands");
    auto before=*host.lookup({100}); auto after=before; after.expression={}; after.value=1;
    auto plan=host.prepare({before,after},host.capture()); require(plan.ok(),"Post-reclamation preparation failed");
    host.reclaim(); require(host.commit(std::move(plan).takeValue()).ok(),"Maintenance invalidated protected preparation");
    require(host.undo().ok() && *host.lookup({100})==before,"Undo after reclamation lost values");
    require(host.redo().ok() && *host.lookup({100})==after,"Redo after reclamation lost values");
    reader.reset(); require(host.readerMetrics().currentBytes==0,"Released reader retained bytes");
    // A change subscriber protects unread receipts without pinning operand history.
    auto cursor=host.subscribeChanges(); auto observed=host.capture();
    before=*host.lookup({100}); after=before; after.value=0;
    auto edit=host.prepare({before,after},host.capture()); require(edit.ok() && host.commit(std::move(edit).takeValue()).ok(),"Cursor edit failed");
    host.clearHistory(); host.reclaim(); size_t changed=0;
    host.changedSourcesSince(observed.revision,[&](InstructionId id,bool){ if(id.value==100) ++changed; }); require(changed==1,"Unread receipt retired");
    host.acknowledge(cursor,host.capture()); require(host.reclaim().receiptsRetired==1,"Acknowledged receipt retained");
    std::shared_ptr<const ReadSnapshot<Fact>> detached;
    { Context temporary; ProgramHost<DenseStorage> owner(temporary); require(owner.load(fixture).ok(),"Reader lifetime load failed"); detached=owner.captureRead(); }
    require(detached->lookup({100}) && *detached->lookup({100})==original,"Reader depended on destroyed host/allocator");
}
void storySmoke() {
    Context context; auto fixture=makeFixture(16,1,"story-connectivity");
    ProgramHost<DenseStorage> host(context); require(host.load(fixture).ok(),"Story load failed");
    StoryConnectivity story(context); story.load(host);
    const auto finish=[&] { story.synchronize(host); auto p=story.prepare(host); require(story.install(host,std::move(p)),"Story install failed"); };
    auto initial=story.prepare(host); require(story.install(host,std::move(initial)),"Initial story install failed");
    const auto collect=[&](StoryConnectivity& index,ModuleId id) {
        std::vector<Relationship> rows; auto ticket=index.visit(host,id,[&](const Relationship& r){ rows.push_back(r); });
        require(!ticket.truncated,"Small story truncated"); std::ranges::sort(rows); return rows;
    };
    auto rows=collect(story,{40});
    require(rows.size()==6,"Expected three state edges, helper call, transition, and scheduled call");
    require(std::ranges::any_of(rows,[](const Relationship& r){ return r.instruction.value==104 && r.condition.value==108 && r.source.value==40 && r.kind==RelationshipKind::SupportsGuard; }),"Shared helper effect missing");
    require(std::ranges::none_of(rows,[](const Relationship& r){ return r.instruction.value==109 || r.instruction.value==111; }),"Transition/scheduled effects were treated as immediate");
    const auto apply=[&](Record before,Record after){ auto p=host.prepare({before,after},host.capture()); require(p.ok() && host.commit(std::move(p).takeValue()).ok(),"Story edit failed"); };
    // Remove the only guard for BitVar[17], observe the empty bucket, then introduce a new guard there.
    auto door=*host.lookup({106}); auto displaced=door; displaced.variable=78; apply(door,displaced); finish();
    auto watched=story.visit(host,{40},[](const Relationship&){}); const auto aVersion=story.version({40});
    auto stale=story.prepare(host); auto guard=*host.lookup({110}); auto newGuard=guard; newGuard.variable=17;
    apply(guard,newGuard); require(!story.install(host,std::move(stale)),"Stale derived completion installed");
    finish(); require(!story.fresh(host,watched),"Empty query dependency missed new guard");
    require(story.version({40})==aVersion,"Unrelated source summary rebuilt for target guard change");
    rows=collect(story,{40}); require(std::ranges::any_of(rows,[](const Relationship& r){ return r.instruction.value==100 && r.condition.value==110; }),"New guard edge missing");
    auto reusable=story.visit(host,{40},[](const Relationship&){});
    auto write=*host.lookup({100}); auto flagged=write; flagged.flags=4; apply(write,flagged); finish();
    require(story.fresh(host,reusable),"Flag-only edit invalidated unchanged connectivity");
    // Multiple edits between synchronizations coalesce the helper and its two callers once.
    const auto beforeWork=story.work(); auto helper=*host.lookup({104}); auto cleared=helper; cleared.value=0; apply(helper,cleared);
    auto helperRead=*host.lookup({105}); auto helperWrite=helperRead; helperWrite.op=Op::Write; helperWrite.value=1; apply(helperRead,helperWrite);
    story.synchronize(host); require(story.pending({40}) && story.pending({41}) && story.pending({42}),"Helper callers were not pending");
    auto coalesced=story.prepare(host); require(story.install(host,std::move(coalesced)),"Coalesced install failed");
    require(story.work().modulesPrepared-beforeWork.modulesPrepared==3,"Helper invalidation was not coalesced/bounded");

    auto movedHelper=cleared; movedHelper.section={10}; apply(cleared,movedHelper); finish();
    rows=collect(story,{40});
    require(std::ranges::any_of(rows,[](const Relationship& r){ return r.instruction.value==101 && r.target.value==40; }),"Unchanged caller missed moved target ownership");
    require((story.issues({40})&uint32_t(SummaryIssue::Unresolved))!=0,"Interior call target needs an explicit boundary qualification");

    // Small fixture-only rebuild comparisons catch stale incremental state through the common trace.
    Context traceContext; ProgramHost<DenseStorage> traceHost(traceContext); require(traceHost.load(fixture).ok(),"Trace load failed");
    StoryConnectivity incremental(traceContext); incremental.load(traceHost); auto loaded=incremental.prepare(traceHost); require(incremental.install(traceHost,std::move(loaded)),"Trace initial install failed");
    bool sawCycle=false,sawUnknown=false,sawExpression=false;
    for(const auto& step:fixture.trace) {
        if(const auto* e=std::get_if<Edit>(&step.action)) { auto p=traceHost.prepare(e->change,traceHost.capture()); require(p.ok() && traceHost.commit(std::move(p).takeValue()).ok(),"Story trace edit failed"); }
        else if(const auto* b=std::get_if<EditBatch>(&step.action)) { auto p=traceHost.prepare(*b,traceHost.capture()); require(p.ok() && traceHost.commit(std::move(p).takeValue()).ok(),"Story trace batch failed"); }
        else if(std::holds_alternative<Undo>(step.action)) require(traceHost.undo().ok(),"Story undo failed");
        else if(std::holds_alternative<Redo>(step.action)) require(traceHost.redo().ok(),"Story redo failed");
        else continue;
        incremental.synchronize(traceHost); auto p=incremental.prepare(traceHost); require(incremental.install(traceHost,std::move(p)),"Trace derived install failed");
        Context rebuiltContext; StoryConnectivity rebuilt(rebuiltContext); rebuilt.load(traceHost); auto fresh=rebuilt.prepare(traceHost); require(rebuilt.install(traceHost,std::move(fresh)),"Rebuild failed");
        for(const auto& m:fixture.modules) {
            std::vector<Relationship> actual,expected;
            incremental.visit(traceHost,m.id,[&](const Relationship& r){ actual.push_back(r); });
            rebuilt.visit(traceHost,m.id,[&](const Relationship& r){ expected.push_back(r); });
            std::ranges::sort(actual); std::ranges::sort(expected);
            require(actual==expected && incremental.issues(m.id)==rebuilt.issues(m.id),"Incremental connectivity differs from fresh derived index");
        }
        sawCycle|=(incremental.issues({40})&uint32_t(SummaryIssue::Cycle))!=0;
        sawUnknown|=(incremental.issues({40})&uint32_t(SummaryIssue::Unresolved))!=0;
        sawExpression|=(incremental.issues({40})&uint32_t(SummaryIssue::UnknownValue))!=0;
    }
    require(sawCycle && sawUnknown && sawExpression,"Expected cycle/unknown qualifications absent");
}

}
// Deliberately small smoke coverage while the representation is exploratory.
// These exercise the runner, not a permanent semantic or performance specification.
int selfTests() {
    try {
        {
            const auto exercise=[]<class Storage>(bool shared) {
            Context context; auto fixture=makeFixture(8,1);
            ProgramHost<Storage> host(context,shared);
            require(host.load(fixture).ok(),"Load failed");
            const auto original=fixture.records.front();
            FactId factBefore; host.query({QueryKind::Writes,original.variable},[&](const Fact& fact){ if(fact.source==original.id) factBefore=fact.id; });
            auto replacement=original; ++replacement.variable;
            auto prepared=host.prepare({original,replacement},host.capture());
            require(prepared.ok() && host.commit(std::move(prepared).takeValue()).ok(),"Edit failed");
            require(*host.lookup(original.id)==replacement,"Edit not applied");
            FactId factAfter; host.query({QueryKind::Writes,replacement.variable},[&](const Fact& fact){ if(fact.source==original.id) factAfter=fact.id; });
            require(factBefore.value && factBefore==factAfter,"Write identity was not retained across retargeting");
            size_t oldUses=0; host.query({QueryKind::Writes,original.variable},[&](const Fact&){ ++oldUses; });
            require(oldUses==0,"Old write bucket was not updated");
            require(host.undo().ok() && *host.lookup(original.id)==original,"Undo failed");
            require(host.redo().ok() && *host.lookup(original.id)==replacement,"Redo failed");
            require(host.inspectStorage().ok(),"Storage inspection failed");
            // One structural churn example also crosses page boundaries and moves owners.
            Context churnContext; auto churn=makeFixture(512,1,"churn"); ProgramHost<Storage> churnHost(churnContext,shared);
            require(churnHost.load(churn).ok(),"Churn load failed");
            for(const auto& step:churn.trace) {
                Result<void> result=Result<void>::success();
                if(const auto* edit=std::get_if<Edit>(&step.action)) {
                    auto p=churnHost.prepare(edit->change,churnHost.capture()); require(p.ok(),"Churn prepare failed");
                    result=churnHost.commit(std::move(p).takeValue());
                } else if(const auto* batch=std::get_if<EditBatch>(&step.action)) {
                    auto p=churnHost.prepare(*batch,churnHost.capture()); require(p.ok(),"Churn batch failed");
                    result=churnHost.commit(std::move(p).takeValue());
                } else if(std::holds_alternative<Undo>(step.action)) result=churnHost.undo();
                else if(std::holds_alternative<Redo>(step.action)) result=churnHost.redo();
                require(result.ok(),"Churn publication failed");
                require(churnHost.inspectStorage().ok(),"Churn storage inspection failed");
            }
            };
            for(bool shared:{false,true}) {
                exercise.operator()<ReferenceStorage>(shared); exercise.operator()<DenseStorage>(shared);
                exercise.operator()<PagedStorage>(shared); exercise.operator()<SectionStorage>(shared);
            }
            std::cout << "PASS four-adapter indexed edit/undo/storage smoke\n";
        }
        {
            Context context; const auto fixture=makeFixture(8,1);
            ProgramHost<ReferenceStorage> host(context); require(host.load(fixture).ok(),"Load failed");
            auto original=fixture.records.front(); auto wrong=original; ++wrong.value;
            require(!host.prepare({wrong,original},host.capture()),"Invalid precondition accepted");
            require(host.capture().revision==0 && host.historySize()==0 && *host.lookup(original.id)==original,"Rejected edit changed state");
            std::atomic_bool cancelled=true;
            auto stopped=execute(RunSpec{},Controls{&cancelled});
            require(stopped.at("disposition")=="cancelled","Cancellation not reported");
            storySmoke(); lifetimeSmoke();
            std::cout << "PASS lifetime/readers, story dependencies, and failure smoke\n";
        }
        {
            RunSpec spec; spec.fixture="story-connectivity"; auto first=execute(spec); ++spec.repetition; auto second=execute(spec);
            require(first.at("disposition")=="passed" && second.at("disposition")=="passed","Normal run failed");
            require(first.at("final_checksum")==second.at("final_checksum"),"Repeated workload changed result");
            std::vector<Json> reports{first,second};
            for(const auto* candidate:{"dense","paged","section-local"}) { spec.candidate=candidate; reports.push_back(execute(spec)); }
            require(compareReports(reports).ok(),"Cross-candidate comparison failed");
            const auto folder=std::filesystem::temp_directory_path()/std::filesystem::path("salsa-ir-smoke-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(Clock::now().time_since_epoch().count()));
            const auto path=folder/"run.json";
            struct Cleanup {
                std::filesystem::path path, folder;
                ~Cleanup(){ std::error_code error; std::filesystem::remove(path,error); std::filesystem::remove(folder,error); }
            } cleanup{path,folder};
            require(writeNewReport(path,first).ok(),"Report write failed");
            auto loaded=readJson(path); require(loaded && loaded.value()==first,"Report round trip failed");
            std::cout << "PASS normal run/report/comparison smoke\n";
        }
    } catch(const std::exception& e) { std::cerr << "FAIL smoke: " << e.what() << '\n'; return 1; }
    std::cout << "3 smoke checks passed. No GUI or corpus loaded.\n";
    return 0;
}
} // namespace salsa::evaluation
