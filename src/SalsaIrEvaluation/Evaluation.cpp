#include "Program.h"
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
uint64_t textHash(uint64_t h,std::string_view text) { h=word(h,text.size()); for(unsigned char c:text) h=word(h,c); return h; }
uint64_t expressionHash(uint64_t h,const Expression& e) {
    for(auto value:{e.id.value,static_cast<uint64_t>(e.op),static_cast<uint64_t>(e.bank),static_cast<uint64_t>(e.variable),e.literal,e.left.value,e.right.value}) h=word(h,value);
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
        {"build", buildIdentity()}, {"interface_version", 2}, {"common_variants", "program-host-v2/order-v2/direct-facts-v1/history-v2"},
        {"semantic_coverage", "synthetic-expression-batch-order-unknown-v2"}, {"workers", 1},
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
uint64_t hashRecord(uint64_t h, const Record& r) {
    for (auto v : {r.id.value, r.section.value, static_cast<uint64_t>(r.op), static_cast<uint64_t>(r.variable), r.value, static_cast<uint64_t>(r.bank), r.expression.value, r.schedule.value, static_cast<uint64_t>(r.evidence), static_cast<uint64_t>(r.flags)}) h = word(h, v);
    return h;
}
uint64_t hashFact(uint64_t h,const Fact& f) {
    for(auto v:{f.id.value,f.source.value,f.operand.value,static_cast<uint64_t>(f.kind),f.role,static_cast<uint64_t>(f.bank),f.key,f.value,f.expression.value,static_cast<uint64_t>(f.evidence)}) h=word(h,v);
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
    f.project={seed,1};
    f.scripts={{ScriptId{1},"Script A","synthetic-baseline-a"},{ScriptId{2},"Script B","synthetic-baseline-b"}};
    f.sections={{SectionId{10},ScriptId{1},"cutscene",{}},{SectionId{11},ScriptId{1},"helper",{}},{SectionId{20},ScriptId{2},"interaction",{}}};
    f.expressions={{ExprId{50},ExprOp::Variable,Bank::Byte,6},{ExprId{51},ExprOp::Literal,Bank::Bit,0,2},
        {ExprId{52},ExprOp::Add,Bank::Bit,0,0,ExprId{50},ExprId{51}}};
    f.evidence={"Target is explicitly unresolved in this synthetic draft"};
    f.reserved={InstructionId{100+count},InstructionId{101+count}};
    f.records.reserve(static_cast<size_t>(count));
    uint64_t state = seed;
    for (uint64_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        f.records.push_back({InstructionId{100 + i}, SectionId{i < count / 2 ? 10ull : 20ull},
            i % 2 ? Op::Read : Op::Write, static_cast<uint32_t>(100 + (state >> 32) % 256), state & 1});
    }
    f.records[0].op = Op::Write; f.records[0].variable = 17; f.records[0].value = 1;
    f.records[1].op = Op::Read; f.records[1].variable = 17; f.records[1].value = 0;
    f.records[2].op = Op::Call; f.records[2].variable = 0; f.records[2].value = 104;
    f.records[3].op = Op::Write; f.records[3].variable = 18; f.records[3].value = 0;
    f.records[4].op = Op::Read; f.records[4].variable = 18; f.records[4].value = 0;
    f.records[6].op=Op::Write; f.records[6].variable=19; f.records[6].value=0; f.records[6].expression=ExprId{52};
    f.records[6].schedule=ExprId{50};
    for(const auto& r:f.records) for(auto& section:f.sections) if(section.id==r.section) section.order.push_back(r.id);
    auto bit = f.records[0]; bit.variable = 18;
    auto target = f.records[2]; target.value = 105;
    std::vector<Action> actions{
        Query{QueryKind::Writes,17}, Query{QueryKind::Reads,17}, Edit{{f.records[0],bit}},
        Query{QueryKind::Writes,17}, Query{QueryKind::Writes,18}, Edit{{f.records[2],target}},
        Query{QueryKind::Incoming,104}, Query{QueryKind::Incoming,105}, Undo{},
        Query{QueryKind::Incoming,104}, Undo{}, Query{QueryKind::Writes,17}, Redo{}, Redo{}, Query{QueryKind::Writes,18}};
    Record inserted{f.reserved[0],SectionId{11},Op::Write,19,1};
    actions.push_back(EditBatch{{{{},inserted}},{{inserted.id,inserted.section,{}}},{}});
    actions.push_back(Query{QueryKind::Writes,19});
    auto moved=f.records[1]; moved.section=SectionId{11};
    actions.push_back(EditBatch{{{f.records[1],moved}},{{moved.id,moved.section,inserted.id}}, {}});
    actions.push_back(EditBatch{{},{{bit.id,bit.section,f.records[3].id}}, {}});
    actions.push_back(EditBatch{{},{},{{SectionId{11},"helper","shared helper"}}});
    auto repaired=target; repaired.value=inserted.id.value;
    actions.push_back(EditBatch{{{target,repaired},{f.records[5],{}}},{},{}});
    actions.push_back(Query{QueryKind::Incoming,105});
    actions.push_back(Undo{}); actions.push_back(Redo{});
    auto flagged=bit; flagged.flags=4;
    actions.push_back(Edit{{bit,flagged}});
    Record unresolved{f.reserved[1],SectionId{20},Op::Call}; unresolved.evidence=1;
    actions.push_back(EditBatch{{{{},unresolved}}, {}, {}});
    actions.push_back(Query{QueryKind::Unknown,0});
    actions.push_back(Query{QueryKind::Reads,6,Bank::Byte});
    uint64_t traceHash = FnvBasis;
    for (const auto& action : actions) {
        f.trace.push_back({f.trace.size() + 1, action});
        traceHash = word(traceHash, action.index());
        if (auto q = std::get_if<Query>(&action)) { traceHash = word(traceHash, static_cast<uint64_t>(q->kind)); traceHash = word(traceHash, q->key); traceHash=word(traceHash,static_cast<uint64_t>(q->bank)); }
        if (auto e = std::get_if<Edit>(&action)) { traceHash = hashRecord(traceHash, e->change.before); traceHash = hashRecord(traceHash, e->change.after); }
        if (auto batch=std::get_if<EditBatch>(&action)) {
            for(const auto& m:batch->mutations) { traceHash=hashRecord(traceHash,m.before); traceHash=hashRecord(traceHash,m.after); }
            for(const auto& p:batch->placements) { traceHash=word(traceHash,p.instruction.value); traceHash=word(traceHash,p.section.value); traceHash=word(traceHash,p.before.value); }
            for(const auto& n:batch->names) { traceHash=word(traceHash,n.section.value); for(auto c:n.before) traceHash=word(traceHash,static_cast<unsigned char>(c)); traceHash=word(traceHash,0); for(auto c:n.after) traceHash=word(traceHash,static_cast<unsigned char>(c)); }
        }
    }
    uint64_t inputHash=word(word(FnvBasis,f.project.high),f.project.low);
    for(const auto& script:f.scripts) inputHash=textHash(textHash(word(inputHash,script.id.value),script.name),script.source);
    for(const auto& section:f.sections) {
        inputHash=textHash(word(word(inputHash,section.id.value),section.script.value),section.name);
        inputHash=word(inputHash,section.order.size()); for(auto id:section.order) inputHash=word(inputHash,id.value);
    }
    for(const auto& e:f.expressions) inputHash=expressionHash(inputHash,e);
    for(const auto& evidence:f.evidence) inputHash=textHash(inputHash,evidence);
    for(auto id:f.reserved) inputHash=word(inputHash,id.value);
    for(const auto& r:f.records) inputHash=hashRecord(inputHash,r);
    f.checksum=hex(inputHash); f.traceChecksum=hex(traceHash);
    return f;
}
Result<void> validateSpec(const RunSpec& s) {
    if (s.candidate != "reference" || s.fixture != "small-story" || s.trace != "semantic-edits")
        return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationUnsupported, "IR2 supports reference / small-story / semantic-edits only"));
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
        report["capabilities"]={{"supported",{"load","lookup","enumerate","replace","query","undo","redo","insert","erase","same-script-owner-move","reorder","rename","expressions","unresolved-drafts"}},
            {"unsupported",{"guard-summary","snapshot","native-sct","persistence","cross-script-owner-move"}}};
        std::vector<Sample> samples; samples.reserve(fixture.trace.size()*3);
        const auto modelReadyStart=Clock::now();
        Context context;
        ProgramHost<Storage> host(context);
        report["phase"]="load";
        auto start=Clock::now();
        auto loaded=host.load(fixture);
        if (!loaded) throw std::runtime_error(loaded.diagnostics()[0].message);
        const auto loadNs=elapsed(start);
        std::vector<Record>().swap(fixture.records);
        std::vector<Section>().swap(fixture.sections); std::vector<Script>().swap(fixture.scripts);
        std::vector<Expression>().swap(fixture.expressions); std::vector<std::string>().swap(fixture.evidence);
        std::vector<InstructionId>().swap(fixture.reserved);
        const auto modelReadyNs=elapsed(modelReadyStart);
        // Model construction and input release are included; report formatting is excluded.
        report["load"]={{"fixture_ns",std::to_string(fixtureNs)},{"program_load_ns",std::to_string(loadNs)},{"model_ready_ns",std::to_string(modelReadyNs)},
            {"load_to_ready_ns",std::to_string(fixtureNs+modelReadyNs)},
            {"shared_input_values_order_ns",std::to_string(host.loadPhases()[0])},
            {"candidate_storage_ns",std::to_string(host.loadPhases()[1])},{"direct_indexes_ns",std::to_string(host.loadPhases()[2])},
            {"scope","synthetic normalization + shared program and indexes; no SCT decode"}};
        struct OperationWork { uint64_t operation; ProgramWork work; };
        std::vector<OperationWork> operationWork; operationWork.reserve(fixture.trace.size());
        for (const auto& step : fixture.trace) {
            const auto workBefore=host.work();
            if (controls.stopRequested()) { fail(report,"cancelled","Stopped at operation boundary"); break; }
            report["operation"]=std::to_string(step.id);
            Result<void> result=Result<void>::success();
            if (samples.size()+3 > samples.capacity()) throw std::runtime_error("Measurement sample buffer exhausted");
            if (const auto* query=std::get_if<Query>(&step.action)) {
                report["phase"]="query";
                uint64_t count=0,digest=0;
                start=Clock::now();
                host.query(*query,[&](const Fact& r) { ++count; digest ^= hashFact(FnvBasis,r); });
                samples.push_back({step.id,elapsed(start),count,digest,"query-visit"});
                // Separate cold materialization workload, same for each adapter.
                std::pmr::vector<Fact> rows(context.resource(Domain::Results));
                start=Clock::now();
                host.query(*query,[&](const Fact& r){ rows.push_back(r); });
                const auto materializeNs=elapsed(start);
                uint64_t retainedDigest=0; for (const auto& r:rows) retainedDigest ^= hashFact(FnvBasis,r);
                samples.push_back({step.id,materializeNs,rows.size(),retainedDigest,"query-materialize-cold"});
                if (rows.size()!=count || retainedDigest!=digest) throw std::runtime_error("Materialized query diverged");
            } else if (std::holds_alternative<Edit>(step.action) || std::holds_alternative<EditBatch>(step.action)) {
                report["phase"]="prepare"; start=Clock::now();
                auto prepared=std::holds_alternative<Edit>(step.action)?host.prepare(std::get<Edit>(step.action).change,host.capture()):host.prepare(std::get<EditBatch>(step.action),host.capture());
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
            const auto after=host.work();
            operationWork.push_back({step.id,{after.instructionsExamined-workBefore.instructionsExamined,after.factsExtracted-workBefore.factsExtracted,
                after.factsChanged-workBefore.factsChanged,after.postingsChanged-workBefore.postingsChanged,after.orderIdsCopied-workBefore.orderIdsCopied,after.ranksRepaired-workBefore.ranksRepaired}});

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
        uint64_t programHash=word(word(FnvBasis,host.capture().project.high),host.capture().project.low);
        for(const auto& script:host.scripts()) programHash=textHash(textHash(word(programHash,script.id.value),script.name),script.source);
        host.enumerateSections([&](SectionId id,ScriptId script,std::string_view name,std::span<const InstructionId> order){
            programHash=textHash(word(word(programHash,id.value),script.value),name); programHash=word(programHash,order.size());
            for(auto instruction:order) programHash=word(programHash,instruction.value);
        });
        std::set<ExprId> reachable;
        const auto visitExpression=[&](auto&& self,ExprId id)->void {
            if(!id.value || !reachable.insert(id).second) return;
            const auto& e=host.expressionNode(id); if(e.left.value) self(self,e.left); if(e.right.value) self(self,e.right);
        };
        for(const auto& r:finalRecords) {
            programHash=hashRecord(programHash,r); visitExpression(visitExpression,r.expression); visitExpression(visitExpression,r.schedule);
            if(r.evidence) programHash=textHash(programHash,host.evidence()[r.evidence-1]);
        }
        for(auto id:reachable) programHash=expressionHash(programHash,host.expressionNode(id));
        report["final_checksum"]=hex(programHash);
        report["samples"]=Json::array();
        for (const auto& sample:samples) report["samples"].push_back({{"operation",std::to_string(sample.operation)},
            {"kind",sample.kind},{"ns",std::to_string(sample.ns)},{"rows",std::to_string(sample.emitted)},{"digest",hex(sample.digest)}});
        report["memory"]=Json::object();
        for (size_t i=0;i<totals.size();++i) report["memory"][DomainNames[i]]=allocation(totals[i]);
        report["memory"]["coverage"]="PMR storage, value/order/index/directory pools, history and result buffers; cold metadata strings, input, temporary associative work lists, harness, JSON and allocator overhead uninstrumented";
        report["memory"]["private_bytes"]=memoryAvailable?Json(std::to_string(process.PrivateUsage)):Json(nullptr);
        report["memory"]["sample_scope"]="process at workload end before summary formatting; not exact live object bytes";
        report["work"]={{"records_replaced",std::to_string(metrics.work.replaced)},{"query_facts_visited",std::to_string(examined)},
            {"live_records",std::to_string(metrics.live)},{"record_payload_bytes",std::to_string(metrics.recordBytes)}};
        report["work"]["records_inserted"]=std::to_string(metrics.work.inserted);
        report["work"]["records_erased"]=std::to_string(metrics.work.erased);
        report["work"]["live_facts"]=std::to_string(host.factCount());
        report["work"]["retained_operand_bytes"]=std::to_string(host.retainedOperandBytes());
        report["work"]["live_operand_bytes"]=std::to_string(metrics.live*3*sizeof(Operand));
        report["work"]["retired_operand_bytes"]=std::to_string(host.retainedOperandBytes()-metrics.live*3*sizeof(Operand));
        report["work"]["allocator_high_water"]=std::to_string(host.highWater());
        report["operation_work"]=Json::array();
        for(const auto& entry:operationWork) report["operation_work"].push_back({{"operation",std::to_string(entry.operation)},
            {"instructions_examined",std::to_string(entry.work.instructionsExamined)},{"facts_extracted",std::to_string(entry.work.factsExtracted)},
            {"facts_changed",std::to_string(entry.work.factsChanged)},{"postings_changed",std::to_string(entry.work.postingsChanged)},
            {"order_ids_copied",std::to_string(entry.work.orderIdsCopied)},{"ranks_repaired",std::to_string(entry.work.ranksRepaired)}});
        report["query_scope"]="Known direct facts; unknown effects and unresolved targets are available through the Unknown bucket. No guard or runtime reachability inference.";
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
