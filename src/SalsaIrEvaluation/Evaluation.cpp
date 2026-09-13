#include "Reference.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <Windows.h>
#include <Psapi.h>
#include <bcrypt.h>

namespace salsa::evaluation {
namespace {
constexpr uint64_t FnvBasis = 14695981039346656037ull;
uint64_t word(uint64_t h, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) { h ^= value & 255; h *= 1099511628211ull; value >>= 8; }
    return h;
}
std::string hex(uint64_t value) { std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << value; return s.str(); }
uint64_t decimal(const Json& j) {
    const auto& s = j.get_ref<const std::string&>();
    uint64_t value{};
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || result.ec != std::errc{} || result.ptr != s.data() + s.size() || (s.size() > 1 && s.front() == '0'))
        throw std::invalid_argument("Expected canonical unsigned decimal string");
    return value;
}
void keys(const Json& j, std::initializer_list<std::string_view> expected) {
    if (!j.is_object() || j.size() != expected.size()) throw std::invalid_argument("Unexpected/missing schema fields");
    for (auto key : expected) if (!j.contains(key)) throw std::invalid_argument("Missing schema field");
}
uint64_t elapsed(Clock::time_point start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(); }
Json allocation(const AllocationTotals& a) {
    return {{"allocations", std::to_string(a.allocations)}, {"deallocations", std::to_string(a.deallocations)},
        {"requested_bytes", std::to_string(a.requested)}, {"current_bytes", std::to_string(a.current)}, {"peak_bytes", std::to_string(a.peak)}};
}
Json compatibility(const RunSpec& spec, const Fixture& fixture) {
    auto config = encodeSpec(spec);
    for (auto key : {"repetition"}) config.erase(key);
    // Candidate is deliberately separate, so future storage variants can be compared.
    config.erase("candidate");
    return {{"spec", config}, {"fixture_checksum", fixture.checksum}, {"trace_checksum", fixture.traceChecksum},
        {"build", buildIdentity()}, {"interface_version", 1}, {"common_variants", "reference-host-v1/order-v1/history-v1"},
        {"semantic_coverage", "synthetic-read-write-call-replace-undo-v1"}, {"workers", 1},
        {"instrumentation", "pmr-domain-counters-v1"}, {"cache_policy", "fresh-process-no-warmup-os-cache-uncontrolled"}};
}
void fail(Json& report, std::string disposition, std::string message) {
    report["disposition"] = disposition;
    const auto code = disposition == "unsupported-required" ? core::DiagnosticCode::EvaluationUnsupported :
        disposition == "cancelled" ? core::DiagnosticCode::Cancelled :
        disposition == "invalid-spec" ? core::DiagnosticCode::EvaluationInvalidInput :
        disposition == "operation-failed" ? core::DiagnosticCode::EvaluationOperationRejected : core::DiagnosticCode::EvaluationVerificationFailed;
    report["diagnostics"].push_back({{"code",core::diagnosticCodeName(code)},{"message",std::move(message)}});
}

}
core::Diagnostic diagnostic(core::DiagnosticCode code, std::string message) {
    return {core::DiagnosticSeverity::Error, code, std::move(message), std::nullopt};
}
void* CountingResource::do_allocate(size_t bytes, size_t alignment) {
    if (failAfter && *failAfter == 0) throw std::bad_alloc();
    void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    if (failAfter) --*failAfter;
    ++totals.allocations; totals.requested += bytes; totals.current += bytes;
    totals.peak = std::max(totals.peak, totals.current);
    return result;
}
void CountingResource::do_deallocate(void* p, size_t bytes, size_t alignment) {
    ++totals.deallocations; assert(bytes <= totals.current); totals.current -= bytes;
    std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
}
bool matches(const Record& r, Query q) {
    return (q.kind == QueryKind::Reads && r.op == Op::Read && r.variable == q.key) ||
        (q.kind == QueryKind::Writes && r.op == Op::Write && r.variable == q.key) ||
        (q.kind == QueryKind::Incoming && r.op == Op::Call && r.value == q.key);
}
uint64_t hashRecord(uint64_t h, const Record& r) {
    for (auto v : {r.id.value, r.section.value, static_cast<uint64_t>(r.op), static_cast<uint64_t>(r.variable), r.value}) h = word(h, v);
    return h;
}
std::string canonicalDigest(std::span<const Record> records) {
    uint64_t h = FnvBasis;
    for (const auto& r : records) h = hashRecord(h, r);
    return hex(h);
}
Fixture makeFixture(uint64_t count, uint64_t seed) {
    if (count < 8 || count > 1000000) throw std::invalid_argument("Fixture count outside supported range");
    Fixture f;
    f.records.reserve(static_cast<size_t>(count));
    uint64_t state = seed;
    for (uint64_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        f.records.push_back({InstructionId{100 + i}, SectionId{i < count / 2 ? 1ull : 2ull},
            i % 2 ? Op::Read : Op::Write, static_cast<uint32_t>(100 + (state >> 32) % 256), state & 1});
    }
    f.records[0].op = Op::Write; f.records[0].variable = 17; f.records[0].value = 1;
    f.records[1].op = Op::Read; f.records[1].variable = 17; f.records[1].value = 0;
    f.records[2].op = Op::Call; f.records[2].variable = 0; f.records[2].value = 104;
    f.records[3].op = Op::Write; f.records[3].variable = 18; f.records[3].value = 0;
    f.records[4].op = Op::Read; f.records[4].variable = 18; f.records[4].value = 0;
    auto bit = f.records[0]; bit.variable = 18;
    auto target = f.records[2]; target.value = 105;
    const std::vector<Action> actions{
        Query{QueryKind::Writes,17}, Query{QueryKind::Reads,17}, Edit{{f.records[0],bit}},
        Query{QueryKind::Writes,17}, Query{QueryKind::Writes,18}, Edit{{f.records[2],target}},
        Query{QueryKind::Incoming,104}, Query{QueryKind::Incoming,105}, Undo{},
        Query{QueryKind::Incoming,104}, Undo{}, Query{QueryKind::Writes,17}, Redo{}, Redo{}, Query{QueryKind::Writes,18}};
    uint64_t traceHash = FnvBasis;
    for (const auto& action : actions) {
        f.trace.push_back({f.trace.size() + 1, action});
        traceHash = word(traceHash, action.index());
        if (auto q = std::get_if<Query>(&action)) { traceHash = word(traceHash, static_cast<uint64_t>(q->kind)); traceHash = word(traceHash, q->key); }
        if (auto e = std::get_if<Edit>(&action)) { traceHash = hashRecord(traceHash, e->change.before); traceHash = hashRecord(traceHash, e->change.after); }
    }
    f.checksum = canonicalDigest(f.records); f.traceChecksum = hex(traceHash);
    return f;
}
Result<void> validateSpec(const RunSpec& s) {
    if (s.candidate != "reference" || s.fixture != "small-story" || s.trace != "semantic-edits")
        return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationUnsupported, "IR1 supports reference / small-story / semantic-edits only"));
    if (s.count < 8 || s.count > 1000000 || s.timeoutMs < 1 || s.timeoutMs > 3600000 || s.memoryMb < 32 || s.memoryMb > 65536)
        return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput, "Counts/limits outside supported range"));
    return Result<void>::success();
}
Json encodeSpec(const RunSpec& s) {
    return {{"format","jahorta.salsa.ir-evaluation-spec"},{"version",1},{"candidate",s.candidate},{"fixture",s.fixture},{"trace",s.trace},
        {"seed",std::to_string(s.seed)},{"count",std::to_string(s.count)},
        {"timeout_ms",std::to_string(s.timeoutMs)},{"memory_mb",std::to_string(s.memoryMb)},
        {"repetition",std::to_string(s.repetition)}};
}
Result<RunSpec> decodeSpec(const Json& j) {
    try {
        keys(j,{"format","version","candidate","fixture","trace","seed","count","timeout_ms","memory_mb","repetition"});
        if (j.at("format") != "jahorta.salsa.ir-evaluation-spec" || j.at("version") != 1) throw std::invalid_argument("Unsupported spec schema");
        RunSpec s;
        s.candidate=j.at("candidate").get<std::string>(); s.fixture=j.at("fixture").get<std::string>(); s.trace=j.at("trace").get<std::string>();
        s.seed=decimal(j.at("seed")); s.count=decimal(j.at("count")); s.timeoutMs=decimal(j.at("timeout_ms")); s.memoryMb=decimal(j.at("memory_mb"));
        const auto repetition=decimal(j.at("repetition")); if (repetition > UINT32_MAX) throw std::invalid_argument("Repetition overflow");
        s.repetition=static_cast<uint32_t>(repetition);
        auto valid=validateSpec(s); if (!valid) return Result<RunSpec>::failure(valid.diagnostics());
        return Result<RunSpec>::success(std::move(s));
    } catch (const std::exception& e) { return Result<RunSpec>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput,e.what())); }
}
Json buildIdentity() {
    // Hash the actual executable, including changes in other translation units.
    static const std::string binaryHash = [] {
        std::array<wchar_t,32768> path{};
        const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if (!length || length==path.size()) throw std::runtime_error("Cannot identify executable");
        std::ifstream stream(std::filesystem::path(std::wstring(path.data(),length)),std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot read executable identity");
        struct HashHandles {
            BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
            ~HashHandles(){ if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
        } handles;
        if (BCryptOpenAlgorithmProvider(&handles.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
            BCryptCreateHash(handles.algorithm,&handles.hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("Cannot create binary hash");
        std::array<unsigned char,65536> block{};
        while (stream) {
            stream.read(reinterpret_cast<char*>(block.data()),block.size());
            if (stream.gcount() && BCryptHashData(handles.hash,block.data(),static_cast<ULONG>(stream.gcount()),0)<0)
                throw std::runtime_error("Cannot hash binary");
        }
        if (!stream.eof()) throw std::runtime_error("Cannot finish binary read");
        std::array<unsigned char,32> digest{};
        if (BCryptFinishHash(handles.hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("Cannot finish binary hash");
        std::ostringstream out; out << std::hex << std::setfill('0');
        for (auto byte:digest) out << std::setw(2) << static_cast<unsigned>(byte);
        return out.str();
    }();
    return {{"compiler",std::to_string(_MSC_FULL_VER)},{"architecture","x64"},
#ifdef NDEBUG
        {"configuration","Release"},
#else
        {"configuration","Debug"},
#endif
        {"compiled",__DATE__ " " __TIME__},{"implementation",1},{"binary_sha256",binaryHash}};
}
Result<Json> readJson(const std::filesystem::path& path) {
    try {
        if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Report/spec exceeds 16 MiB limit");
        std::ifstream stream(path, std::ios::binary); if (!stream) throw std::runtime_error("Cannot open JSON file");
        Json result=Json::parse(stream); return Result<Json>::success(std::move(result));
    } catch (const std::exception& e) { return Result<Json>::failure(diagnostic(core::DiagnosticCode::EvaluationReportFailed,e.what())); }
}
Result<void> writeNewReport(const std::filesystem::path& path, const Json& report) {
    auto partial=path; partial += L".partial";
    HANDLE file=INVALID_HANDLE_VALUE;
    try {
        if (std::filesystem::exists(path)) throw std::runtime_error("Output already exists; choose a fresh report path");
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        file=CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create exclusive partial report");
        const auto bytes=report.dump(2) + "\n";
        DWORD written{};
        if (bytes.size() > MAXDWORD || !WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file))
            throw std::runtime_error("Cannot finish report write");
        CloseHandle(file); file=INVALID_HANDLE_VALUE;
        if (!MoveFileExW(partial.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot finalize report (target may exist)");
        return Result<void>::success();
    } catch (const std::exception& e) {
        if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
        return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationReportFailed,e.what()));
    }
}
template<StorageCandidate Storage>
Json executeCandidate(const RunSpec& spec, Controls controls) {
    Json report={{"format","jahorta.salsa.ir-evaluation"},{"version",1},{"complete",false},{"spec",encodeSpec(spec)},
        {"run_id",std::to_string(GetCurrentProcessId())+"-"+std::to_string(Clock::now().time_since_epoch().count())},
        {"disposition","passed"},{"diagnostics",Json::array()},{"phase","spec"},{"operation","0"}};
    try {
        auto valid=validateSpec(spec);
        if (!valid) { fail(report,valid.diagnostics()[0].code==core::DiagnosticCode::EvaluationUnsupported ? "unsupported-required":"invalid-spec",valid.diagnostics()[0].message); report["complete"]=true; return report; }
        const auto readinessStart=Clock::now();
        report["phase"]="fixture";
        auto fixture=makeFixture(spec.count,spec.seed);
        const auto fixtureNs=elapsed(readinessStart);
        report["compatibility"]=compatibility(spec,fixture);
        report["capabilities"]={{"supported",{"load","lookup","enumerate","replace","query","undo","redo"}},
            {"unsupported",{"insert","erase","owner-move","guard-summary","snapshot","native-sct","persistence"}}};
        std::vector<Sample> samples; samples.reserve(fixture.trace.size()*3);
        const auto modelReadyStart=Clock::now();
        Context context;
        ReferenceHost<Storage> host(context);
        report["phase"]="load";
        auto start=Clock::now();
        auto loaded=host.load(fixture.records);
        if (!loaded) throw std::runtime_error(loaded.diagnostics()[0].message);
        const auto loadNs=elapsed(start);
        std::vector<Record>().swap(fixture.records);
        const auto modelReadyNs=elapsed(modelReadyStart);
        // Model construction and input release are included; report formatting is excluded.
        report["load"]={{"fixture_ns",std::to_string(fixtureNs)},{"store_ns",std::to_string(loadNs)},{"model_ready_ns",std::to_string(modelReadyNs)},
            {"load_to_ready_ns",std::to_string(fixtureNs+modelReadyNs)},
            {"scope","synthetic normalization + reference store; no SCT decode"}};
        for (const auto& step : fixture.trace) {
            if (controls.stopRequested()) { fail(report,"cancelled","Stopped at operation boundary"); break; }
            report["operation"]=std::to_string(step.id);
            Result<void> result=Result<void>::success();
            if (samples.size()+3 > samples.capacity()) throw std::runtime_error("Measurement sample buffer exhausted");
            if (const auto* query=std::get_if<Query>(&step.action)) {
                report["phase"]="query";
                uint64_t count=0,digest=0;
                start=Clock::now();
                host.query(*query,[&](const Record& r) { ++count; digest ^= hashRecord(FnvBasis,r); });
                samples.push_back({step.id,elapsed(start),count,digest,"query-visit"});
                // Separate cold materialization workload, same for each adapter.
                std::pmr::vector<Record> rows(context.resource(Domain::Results));
                start=Clock::now();
                host.query(*query,[&](const Record& r){ rows.push_back(r); });
                const auto materializeNs=elapsed(start);
                uint64_t retainedDigest=0; for (const auto& r:rows) retainedDigest ^= hashRecord(FnvBasis,r);
                samples.push_back({step.id,materializeNs,rows.size(),retainedDigest,"query-materialize-cold"});
                if (rows.size()!=count || retainedDigest!=digest) throw std::runtime_error("Materialized query diverged");
            } else if (const auto* edit=std::get_if<Edit>(&step.action)) {
                report["phase"]="prepare"; start=Clock::now();
                auto prepared=host.prepare(edit->change,host.capture());
                samples.push_back({step.id,elapsed(start),0,0,"prepare"});
                if (!prepared) result=Result<void>::failure(prepared.diagnostics());
                else if (controls.stopRequested()) {
                    auto discarded=std::move(prepared).takeValue(); host.cancel(discarded);
                    fail(report,"cancelled","Stopped before publication"); break;
                } else {
                    report["phase"]="install"; start=Clock::now();
                    result=host.commit(std::move(prepared).takeValue());
                    samples.push_back({step.id,elapsed(start),0,0,"install"});
                }
            } else {
                report["phase"]=std::holds_alternative<Undo>(step.action)?"undo":"redo"; start=Clock::now();
                result=std::holds_alternative<Undo>(step.action)?host.undo():host.redo();
                samples.push_back({step.id,elapsed(start),0,0,std::holds_alternative<Undo>(step.action)?"undo":"redo"});
            }
            if (!result) { fail(report,"operation-failed",result.diagnostics()[0].message); break; }

        }
        // Freeze measured state before formatting summaries. Smoke checks are separate from normal execution.
        const auto metrics=host.metrics();
        std::array<AllocationTotals,static_cast<size_t>(Domain::Count)> totals;
        for (size_t i=0;i<totals.size();++i) totals[i]=context.memory[i].totals;
        PROCESS_MEMORY_COUNTERS_EX process{}; process.cb=sizeof(process);
        const bool memoryAvailable=GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process))!=FALSE;
        const auto examined=host.examined();
        report["phase"]="summarize";
        std::vector<Record> finalRecords;
        finalRecords.reserve(static_cast<size_t>(metrics.live));
        host.enumerate([&](const Record& record){ finalRecords.push_back(record); });
        std::ranges::sort(finalRecords,{},&Record::id);
        report["final_checksum"]=canonicalDigest(finalRecords);
        report["samples"]=Json::array();
        for (const auto& sample:samples) report["samples"].push_back({{"operation",std::to_string(sample.operation)},
            {"kind",sample.kind},{"ns",std::to_string(sample.ns)},{"rows",std::to_string(sample.emitted)},{"digest",hex(sample.digest)}});
        report["memory"]=Json::object();
        const std::array names{"records","preparation","history","query_results"};
        for (size_t i=0;i<totals.size();++i) report["memory"][names[i]]=allocation(totals[i]);
        report["memory"]["coverage"]="PMR reference storage/preparation/history/results only; input, harness, JSON and allocator overhead uninstrumented";
        report["memory"]["private_bytes"]=memoryAvailable?Json(std::to_string(process.PrivateUsage)):Json(nullptr);
        report["memory"]["sample_scope"]="process at workload end before summary formatting; not exact live object bytes";
        report["work"]={{"records_replaced",std::to_string(metrics.work.replaced)},{"query_records_examined",std::to_string(examined)},
            {"live_records",std::to_string(metrics.live)},{"record_payload_bytes",std::to_string(metrics.recordBytes)}};
        report["phase"]="finished";
    } catch (const std::bad_alloc&) { fail(report,"resource-limited","Allocation failed"); }
    catch (const std::exception& e) { fail(report,"operation-failed",e.what()); }
    report["complete"]=true;
    return report;
}
Json execute(const RunSpec& spec, Controls controls) {
    return executeCandidate<ReferenceStorage>(spec,controls);
}
Result<void> validateReport(const Json& r) {
    try {
        if (!r.is_object() || r.at("format")!="jahorta.salsa.ir-evaluation" || r.at("version")!=1 || r.at("complete")!=true)
            throw std::invalid_argument("Unsupported/incomplete report");
        if (!r.at("spec").is_object()) throw std::invalid_argument("Missing report spec");
        const auto disposition=r.at("disposition").get<std::string>();
        if (exitCode(disposition)==99) throw std::invalid_argument("Unknown report disposition");
        r.at("run_id").get<std::string>();
        if (disposition=="passed") {
            auto spec=decodeSpec(r.at("spec")); if (!spec) throw std::invalid_argument("Invalid report spec");
            if (!r.at("compatibility").is_object() || !r.at("final_checksum").is_string() || !r.at("samples").is_array())
                throw std::invalid_argument("Completed report lacks result data");
            for (const auto& sample:r.at("samples")) {
                decimal(sample.at("operation")); decimal(sample.at("ns")); decimal(sample.at("rows"));
                if (!sample.at("kind").is_string() || !sample.at("digest").is_string()) throw std::invalid_argument("Malformed operation sample");
            }
            auto config=encodeSpec(spec.value());
            for (auto key:{"mode","repetition","candidate"}) config.erase(key);
            if (r.at("compatibility").at("spec")!=config) throw std::invalid_argument("Report compatibility contradicts run spec");
        }
        return Result<void>::success();
    } catch (const std::exception& e) { return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationReportFailed,e.what())); }
}
Result<Json> compareReports(const std::vector<Json>& reports) {
    if (reports.size()<2) return Result<Json>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput,"Comparison requires at least two reports"));
    Json comparison={{"format","jahorta.salsa.ir-evaluation-comparison"},{"version",1},{"complete",true},{"runs",Json::array()},{"groups",Json::array()}};
    std::map<std::string,std::vector<uint64_t>> times;
    std::vector<std::string> runIds;
    std::vector<std::pair<std::string,std::string>> expectedSchedule;
    for (const auto& r:reports) {
        auto valid=validateReport(r); if (!valid) return Result<Json>::failure(valid.diagnostics());
        if (r.at("disposition")!="passed" || r.at("compatibility")!=reports.front().at("compatibility"))
            return Result<Json>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput,"Only matching completed runs are comparable"));
        if (r.at("final_checksum")!=reports.front().at("final_checksum"))
            return Result<Json>::failure(diagnostic(core::DiagnosticCode::EvaluationVerificationFailed,"Final canonical checksums differ"));
        const auto runId=r.at("run_id").get<std::string>();
        if (std::ranges::find(runIds,runId)!=runIds.end()) return Result<Json>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput,"Duplicate run ID"));
        runIds.push_back(runId); comparison["runs"].push_back(runId);
        std::vector<std::pair<std::string,std::string>> schedule;
        for (const auto& s:r.at("samples")) {
            schedule.emplace_back(s.at("operation").get<std::string>(),s.at("kind").get<std::string>());
            const auto key=r.at("spec").at("candidate").get<std::string>()+"/"+s.at("kind").get<std::string>()+"/"+s.at("operation").get<std::string>();
            times[key].push_back(decimal(s.at("ns")));
        }
        if (runIds.size()==1) expectedSchedule=std::move(schedule);
        else if (schedule!=expectedSchedule) return Result<Json>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput,"Runs executed different operation schedules"));
    }
    for (auto& [key,values]:times) {
        const auto observations=values;
        std::ranges::sort(values);
        Json raw=Json::array(); for (auto v:observations) raw.push_back(std::to_string(v));
        comparison["groups"].push_back({{"key",key},{"samples",values.size()},{"observations_ns",raw},
            {"median_ns",std::to_string(values[(values.size()-1)/2])},{"p95_ns",std::to_string(values[(95*values.size()+99)/100-1])},
            {"max_ns",std::to_string(values.back())}});
    }
    comparison["aggregation"]="nearest-rank percentiles, lower median; tiny samples are descriptive, not tail estimates";
    return Result<Json>::success(std::move(comparison));
}
int exitCode(std::string_view disposition) {
    if (disposition=="passed") return 0;
    if (disposition=="invalid-spec") return 2;
    if (disposition=="unsupported-required") return 3;
    if (disposition=="correctness-failed") return 4;
    if (disposition=="operation-failed") return 5;
    if (disposition=="cancelled") return 6;
    if (disposition=="timed-out") return 7;
    if (disposition=="resource-limited") return 8;
    if (disposition=="crashed/incomplete") return 9;
    return 99;
}
} // namespace salsa::evaluation
