#include "Program.h"
#include "PagedStorage.h"
#include <iostream>
#include <stdexcept>
#include <Windows.h>

namespace salsa::evaluation {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
// Deliberately small smoke coverage while the representation is exploratory.
// These exercise the runner, not a permanent semantic or performance specification.
int selfTests() {
    try {
        {
            const auto exercise=[]<class Storage>() {
            Context context; auto fixture=makeFixture(8,1);
            ProgramHost<Storage> host(context);
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
            Context churnContext; auto churn=makeFixture(512,1,"churn"); ProgramHost<Storage> churnHost(churnContext);
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
            exercise.operator()<ReferenceStorage>(); exercise.operator()<DenseStorage>();
            exercise.operator()<PagedStorage>(); exercise.operator()<SectionStorage>();
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
            std::cout << "PASS rejected edit/cancellation smoke\n";
        }
        {
            RunSpec spec; auto first=execute(spec); ++spec.repetition; auto second=execute(spec);
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
