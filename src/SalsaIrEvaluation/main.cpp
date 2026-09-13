#include "Evaluation.h"
#include <Windows.h>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
using namespace salsa::evaluation;
std::atomic_bool cancelled=false;
BOOL WINAPI onControl(DWORD event) {
    if (event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) { cancelled=true; return TRUE; }
    return FALSE;
}
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE v=nullptr):value(v){}
    ~Handle(){ if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete;
};
std::string utf8(std::wstring_view input) {
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0,nullptr,nullptr);
    if (!count && !input.empty()) throw std::runtime_error("Invalid Unicode argument");
    std::string output(count,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),output.data(),count,nullptr,nullptr);
    return output;
}
std::wstring wide(std::string_view input) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if (!count && !input.empty()) throw std::runtime_error("Invalid UTF-8 argument");
    std::wstring output(count,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),output.data(),count); return output;
}
// Windows argv quoting, not shell escaping. CreateProcess receives an explicit executable.
std::wstring quote(std::wstring_view arg) {
    std::wstring out=L"\""; size_t slashes=0;
    for (auto ch:arg) {
        if (ch==L'\\') { ++slashes; continue; }
        out.append(ch==L'\"'?slashes*2+1:slashes,L'\\'); slashes=0; out+=ch;
    }
    out.append(slashes*2,L'\\'); out+=L'\"'; return out;
}
void require(Result<void> result) { if (!result) throw std::runtime_error(result.diagnostics()[0].message); }
Json failureReport(const RunSpec& spec,std::string disposition,std::string message) {
    return {{"format","jahorta.salsa.ir-evaluation"},{"version",1},{"complete",true},{"spec",encodeSpec(spec)},
        {"run_id",std::to_string(GetCurrentProcessId())+"-supervisor-"+std::to_string(Clock::now().time_since_epoch().count())},
        {"disposition",disposition},{"phase","supervisor"},{"diagnostics",Json::array({message})}};
}
int supervise(const RunSpec& spec,const std::filesystem::path& output) {
    auto validation=validateSpec(spec);
    if (!validation) {
        const auto status=validation.diagnostics()[0].code==salsa::core::DiagnosticCode::EvaluationUnsupported ? "unsupported-required":"invalid-spec";
        require(writeNewReport(output,failureReport(spec,status,validation.diagnostics()[0].message))); return exitCode(status);
    }
    if (std::filesystem::exists(output)) throw std::runtime_error("Output exists; choose a fresh path");
    auto childOutput=output; childOutput+=L".worker";
    if (std::filesystem::exists(childOutput)) throw std::runtime_error("Worker output already exists");
    const auto eventName=L"Local\\SalsaIrCancel-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(Clock::now().time_since_epoch().count());
    Handle cancelEvent(CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()));
    if (!cancelEvent.value) throw std::runtime_error("Cannot create cancellation event");
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!length || length==path.size()) throw std::runtime_error("Cannot resolve executable");
    const std::wstring executable(path.data(),length);
    std::wstring command=quote(executable)+L" worker "+quote(wide(encodeSpec(spec).dump()))+L" "+quote(childOutput.wstring())+L" "+quote(eventName);
    Handle job(CreateJobObjectW(nullptr,nullptr));
    if (!job.value) throw std::runtime_error("Cannot create process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit=static_cast<SIZE_T>(spec.memoryMb*1024*1024);
    if (!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("Cannot set worker memory limit");
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))
        throw std::runtime_error("Cannot launch console worker");
    Handle thread(process.hThread), worker(process.hProcess);
    if (!AssignProcessToJobObject(job.value,worker.value)) { TerminateProcess(worker.value,9); throw std::runtime_error("Cannot contain worker in job"); }
    if (ResumeThread(thread.value)==static_cast<DWORD>(-1)) throw std::runtime_error("Cannot resume worker");
    const auto start=Clock::now(); std::string terminated;
    for (;;) {
        const auto wait=WaitForSingleObject(worker.value,25);
        if (wait==WAIT_OBJECT_0) break;
        if (wait==WAIT_FAILED) { terminated="crashed/incomplete"; break; }
        if (cancelled.load()) {
            SetEvent(cancelEvent.value); // request cancellation at the worker's next safe boundary
            if (WaitForSingleObject(worker.value,5000)!=WAIT_OBJECT_0) terminated="cancelled";
            break;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()>=static_cast<int64_t>(spec.timeoutMs)) { terminated="timed-out"; break; }
    }
    if (!terminated.empty()) {
        TerminateJobObject(job.value,static_cast<UINT>(exitCode(terminated)));
        WaitForSingleObject(worker.value,5000);
        require(writeNewReport(output,failureReport(spec,terminated,"Worker stopped; no partial sample accepted")));
        return exitCode(terminated);
    }
    DWORD code{}; if (!GetExitCodeProcess(worker.value,&code)) throw std::runtime_error("Cannot read worker exit status");
    auto report=readJson(childOutput);
    if (!report || !validateReport(report.value()) || code!=static_cast<DWORD>(exitCode(report.value().value("disposition","crashed/incomplete")))) {
        require(writeNewReport(output,failureReport(spec,"crashed/incomplete","Worker failed or did not produce a complete matching report")));
        return 9;
    }
    require(writeNewReport(output,report.value()));
    return static_cast<int>(code);
}
void help() {
    std::cout << "SalsaIrEvaluation self-test\n"
        "SalsaIrEvaluation run --candidate reference|dense|paged|section-local --report PATH\n"
        "  [--count 64] [--seed 1] [--repetition 0] [--timeout-ms 30000] [--memory-mb 512]\n"
        "  [--order-policy shared|copy] [--reader-policy selected|whole]\n"
        "  [--fixture small-story|few-long|many-short|high-use|churn|story-connectivity|lifetime] [--trace semantic-edits]\n"
        "SalsaIrEvaluation compare --manifest PATH --report PATH\n"
        "Manifest: {\"format\":\"jahorta.salsa.ir-evaluation-manifest\",\"version\":1,\"reports\":[\"relative/report.json\",...]}\n"
        "Synthetic console evaluation only. Output paths must be fresh.\n";
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace salsa::evaluation;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    SetConsoleCtrlHandler(onControl,TRUE);
    try {
        if (argc<2 || std::wstring_view(argv[1])==L"--help") { help(); return 0; }
        const auto command=utf8(argv[1]);
        if (command=="self-test") { if(argc!=2) throw std::runtime_error("self-test takes no arguments"); return selfTests(); }
        if (command=="worker") {
            if(argc!=5) throw std::runtime_error("Invalid internal worker arguments");
            auto decoded=decodeSpec(Json::parse(utf8(argv[2])));
            if(!decoded) throw std::runtime_error(decoded.diagnostics()[0].message);
            Handle cancelEvent(OpenEventW(SYNCHRONIZE,FALSE,argv[4]));
            if (!cancelEvent.value) throw std::runtime_error("Cannot open cancellation event");
            const auto poll=[](void* event)->bool { return WaitForSingleObject(static_cast<HANDLE>(event),0)==WAIT_OBJECT_0; };
            auto report=execute(decoded.value(),Controls{&cancelled,poll,cancelEvent.value});
            require(writeNewReport(argv[3],report)); return exitCode(report.at("disposition").get<std::string>());
        }
        std::map<std::string,std::wstring> options;
        for(int i=2;i<argc;i+=2) {
            if(i+1==argc || !std::wstring_view(argv[i]).starts_with(L"--") || !options.emplace(utf8(std::wstring_view(argv[i]).substr(2)),argv[i+1]).second)
                throw std::runtime_error("Expected unique --option value pairs");
        }
        auto take=[&](const char* key)->std::wstring { auto node=options.extract(key); if(node.empty()) return {}; return std::move(node.mapped()); };
        auto output=take("report"); if(output.empty()) throw std::runtime_error("--report is required");
        if(command=="run") {
            Json config=encodeSpec(RunSpec{});
            for(auto [option,field]: {std::pair{"candidate","candidate"},{"count","count"},{"seed","seed"},
                {"order-policy","order_policy"},{"reader-policy","reader_policy"},{"repetition","repetition"},{"timeout-ms","timeout_ms"},{"memory-mb","memory_mb"},{"fixture","fixture"},{"trace","trace"}}) {
                auto value=take(option); if(!value.empty()) config[field]=utf8(value);
            }
            if(!options.empty()) throw std::runtime_error("Unknown option: "+options.begin()->first);
            auto spec=decodeSpec(config);
            if(!spec) {
                const auto status=spec.diagnostics()[0].code==salsa::core::DiagnosticCode::EvaluationUnsupported?"unsupported-required":"invalid-spec";
                auto report=failureReport(RunSpec{},status,spec.diagnostics()[0].message); report["spec"]=config;
                require(writeNewReport(output,report)); return exitCode(status);
            }
            return supervise(spec.value(),std::filesystem::absolute(output));
        }
        if(command=="compare") {
            auto manifest=take("manifest"); if(manifest.empty() || !options.empty()) throw std::runtime_error("Expected --manifest and --report only");
            auto loaded=readJson(manifest); if(!loaded) throw std::runtime_error(loaded.diagnostics()[0].message);
            const auto& data=loaded.value();
            if(!data.is_object() || data.size()!=3 || data.at("format")!="jahorta.salsa.ir-evaluation-manifest" || data.at("version")!=1 || !data.at("reports").is_array())
                throw std::runtime_error("Unsupported comparison manifest");
            std::vector<Json> reports;
            for(const auto& entry:data.at("reports")) {
                auto path=std::filesystem::path(wide(entry.get<std::string>()));
                if(path.is_relative()) path=std::filesystem::path(manifest).parent_path()/path;
                auto report=readJson(path); if(!report) throw std::runtime_error(report.diagnostics()[0].message);
                reports.push_back(std::move(report).takeValue());
            }
            auto comparison=compareReports(reports); if(!comparison) throw std::runtime_error(comparison.diagnostics()[0].message);
            require(writeNewReport(output,comparison.value())); return 0;
        }
        throw std::runtime_error("Unknown command");
    } catch(const std::exception& e) { std::cerr << "Evaluation error: " << e.what() << '\n'; return 2; }
}
