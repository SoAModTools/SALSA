#include "../../src/SalsaConnectivitySurvey/Survey.h"
#include "../../src/SalsaConnectivitySurvey/Worker.h"
#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SalsaCore/Authoring/SctStoryPaths.h"
#include "SpiceMLD/MldDocumentWriter.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include <Windows.h>
#include <gtest/gtest.h>
#include <process.h>
namespace {
using namespace salsa::survey;
using namespace salsa::core;
namespace fs = std::filesystem;
namespace sct = spice::sct;
struct Temporary {
    fs::path root = fs::temp_directory_path() /
                    ("salsa survey-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Temporary() {
        fs::create_directories(root);
    }
    ~Temporary() {
        std::error_code e;
        fs::remove_all(root, e);
    }
};
sct::SctCanonicalExpression expression(const char *text) {
    auto r = SctExpressionLanguage::parse(text);
    if (!r.succeeded())
        throw std::runtime_error("fixture expression");
    return *r.expression;
}
std::vector<std::byte> bytes() {
    sct::SctDocument doc;
    for (const char *name : {"init", "loop"})
        doc.sections.push_back(
            {doc.allocateSectionId(), name, sct::SctScriptSectionContent{{{doc.allocateInstructionId(), 12}}}});
    doc.sections.push_back(
        {doc.allocateSectionId(), "me002aa25",
         sct::SctScriptSectionContent{{{doc.allocateInstructionId(), 17, false, {}, {{0, expression("1960")}}},
                                       {doc.allocateInstructionId(), 18, false, {}, {{0, expression("1960")}}},
                                       {doc.allocateInstructionId(), 12}}}});
    auto test = doc.allocateInstructionId(), ret = doc.allocateInstructionId();
    doc.sections.push_back(
        {doc.allocateSectionId(), "M00001",
         sct::SctScriptSectionContent{
             {{test, 0, false, {}, {{0, expression("BitVar[1960] == 1")}, {1, sct::SctInstructionReference{ret}}}},
              {doc.allocateInstructionId(),
               11,
               false,
               {},
               {{0,
                 sct::SctInstructionReference{
                     std::get<sct::SctScriptSectionContent>(doc.sections[2].content).instructions.front().id}}}},
              {ret, 12}}}});
    auto result = sct::SctDocumentExporter::exportDocument(
        doc, sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast,
                                           sct::kSctShiftJisByte7FEncoding,
                                           sct::SctDocumentOutputByteOrder::LittleEndian,
                                           sct::SctDocumentOutputWrapper::Raw,
                                           sct::SctOpaquePreservationPolicy::RequirePreservation,
                                           {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
    if (!result.success)
        throw std::runtime_error("fixture export");
    auto span = std::as_bytes(std::span(result.bytes));
    return {span.begin(), span.end()};
}
Json identity(const std::vector<std::byte> &data) {
    return {{"key", "synthetic:me002a.sct"},
            {"dataset", "synthetic"},
            {"path", "me002a.sct"},
            {"release", "synthetic"},
            {"disc", "1"},
            {"platform", "dreamcast"},
            {"region", "eu"},
            {"sha256", hash(data)},
            {"textConvention", nullptr}};
}
std::vector<std::byte> mldBytes(unsigned signal = 1960) {
    spice::mld::MldDocument doc;
    for (unsigned n = 0; n < 4; ++n) {
        spice::mld::MldEntry entry;
        entry.id = {n + 1};
        entry.entryId = 99;
        entry.tableId = 1;
        entry.functionName = n < 2 ? "eventhook" : "testobject";
        entry.functionParameters.resize(9);
        entry.functionParameters[8] = signal;
        doc.entries.push_back(entry);
        doc.layout.push_back(entry.id);
    }
    auto out =
        spice::mld::MldDocumentWriter::write(doc, {spice::mld::MldPlatform::Dreamcast, spice::mld::MldWrapper::Raw});
    if (!out.ok())
        throw std::runtime_error("Synthetic MLD export failed");
    auto span = std::as_bytes(std::span(out.bytes));
    return {span.begin(), span.end()};
}
Json mldIdentity(const std::vector<std::byte> &data) {
    auto result = identity(data);
    result["kind"] = "mld";
    result["path"] = "a002a.mld";
    result["key"] = "synthetic:a002a.mld";
    return result;
}
TEST(ConnectivitySurveyTest, MldProvidersAndHandlersKeepDistinctEntriesAndShowResponses) {
    auto data = bytes(), mld = mldBytes();
    auto script = extract(data, identity(data));
    auto resource = extract(mld, mldIdentity(mld));
    ASSERT_EQ(resource["status"], "analyzed");
    ASSERT_EQ(resource["entries"].size(), 4);
    auto links = linkResource(resource, script, true);
    ASSERT_EQ(links["eventhooks"].size(), 2);
    ASSERT_EQ(links["handlers"].size(), 2);
    EXPECT_NE(links["handlers"][0]["entry"], links["handlers"][1]["entry"]);
    EXPECT_EQ(links["handlers"][0]["response"]["calls"].size(), 1);
    EXPECT_EQ(links["handlers"][0]["response"]["writes"].size(), 2);
    ASSERT_EQ(links["eventhooks"][0]["consumers"].size(), 1);
    EXPECT_EQ(links["eventhooks"][0]["consumers"][0]["boundaryStatus"], "structured");
    auto foreign = script;
    foreign["identity"]["dataset"] = "another-disc";
    EXPECT_THROW(linkResource(resource, foreign), std::exception);
    auto unmatched = mldBytes(4000);
    auto absent = linkResource(extract(unmatched, mldIdentity(unmatched)), script);
    EXPECT_TRUE(absent["eventhooks"][0]["consumers"].empty());
    auto invalid = mldBytes(0xffffffff);
    auto unknown = extract(invalid, mldIdentity(invalid));
    EXPECT_TRUE(unknown["entries"][0]["signal"].is_null());
    EXPECT_TRUE(unknown["entries"][0].contains("signalIssue"));
    EXPECT_EQ(linkResource(unknown, script)["handlers"].size(), 2);
    auto dungeon = identity(data);
    dungeon["path"] = "me103a.sct";
    EXPECT_EQ(summarize(extract(data, dungeon))["scriptGroup"], "field");
    dungeon["path"] = "me099a.sct";
    EXPECT_EQ(summarize(extract(data, dungeon))["scriptGroup"], "field");
    EXPECT_EQ(summarize(extract(data, dungeon))["context"], "overworld");
}
TEST(ConnectivitySurveyTest, ResourceQueriesLimitEntrypointsAndKeepTransitionCandidatesQualified) {
    Temporary temp;
    auto data = bytes(), mld = mldBytes();
    auto script = extract(data, identity(data)), resource = extract(mld, mldIdentity(mld));
    resource["identity"]["association"] = {{"role", "base"}, {"scripts", Json::array({"me002a.sct"})}};
    const auto handler = script["sections"][3]["id"].get<std::string>();
    const auto scene = script["scopes"][0]["id"].get<std::string>();
    const auto instruction = script["instructions"].back()["id"];
    script["resourceContext"]["transitions"].push_back(
        {{"instruction", instruction}, {"target", {{"text", "me033b"}}}});
    Json run = {{"files", Json::array()}, {"summary", Json::object()}}, index;
    for (const auto &facts : {script, resource}) {
        auto row = summarize(facts);
        auto name = hash(facts.at("identity").dump()) + ".json.zlib";
        writeFacts(temp.root / "facts" / name, facts);
        row["facts"] = name;
        run["files"].push_back(row);
    }
    indexResources(run, index, temp.root);
    // Inventory candidates stay dataset-qualified; duplicates are not silently selected.
    for (const auto *path : {"field/me033b.sct", "alternative/me033b.sct"})
        run["files"].push_back(
            {{"identity", {{"key", std::string("synthetic:") + path}, {"path", path}, {"dataset", "synthetic"}}}});
    auto query = [&](const char *kind, const std::string &section) {
        return queryResources(run, index, temp.root, kind, "synthetic:me002a.sct#" + section, "synthetic:me002a.sct");
    };
    EXPECT_EQ(query("entrypoint", handler)[0]["links"][0]["handlers"].size(), 2);
    EXPECT_TRUE(query("entrypoint", script["sections"][0]["id"])[0]["links"][0]["handlers"].empty());
    EXPECT_EQ(query("cutscene", scene)[0]["links"][0]["handlers"].size(), 2);
    auto transitions = query("entrypoint", handler)[0]["links"][0]["handlers"][0]["response"]["transitions"];
    ASSERT_EQ(transitions.size(), 1);
    EXPECT_EQ(transitions[0]["targetStatus"], "ambiguous");
    EXPECT_EQ(transitions[0]["candidateScripts"].size(), 2);
    script["scopes"][0]["sections"].push_back(script["sections"][0]["id"]);
    for (auto &edge : script["controlFlow"])
        if (edge["kind"] == static_cast<unsigned>(sct::SctControlFlowKind::Call))
            edge["target"] = script["instructions"][0]["id"];
    writeFacts(temp.root / "facts" / (hash(script.at("identity").dump()) + ".json.zlib"), script);
    EXPECT_TRUE(query("cutscene", scene)[0]["links"][0]["handlers"].empty());
}
TEST(ConnectivitySurveyTest, StructuredBranchesKeepContinuationSeparateAndReadScriptTargets) {
    sct::SctDocument doc;
    auto condition = doc.allocateInstructionId(), yes = doc.allocateInstructionId();
    auto jump = doc.allocateInstructionId(), no = doc.allocateInstructionId(), join = doc.allocateInstructionId();
    auto text = doc.allocateSupplementaryTextId();
    doc.supplementaryText.push_back({text, sct::SctTextKind::PlainString, sct::SctPlainText{"me033b"}});
    doc.sections.push_back(
        {doc.allocateSectionId(), "loop",
         sct::SctScriptSectionContent{
             {{condition, 0, false, {}, {{0, expression("BitVar[1960] == 1")}, {1, sct::SctInstructionReference{no}}}},
              {yes, 17, false, {}, {{0, expression("20")}}},
              {jump, 10, false, {}, {{0, sct::SctInstructionReference{join}}}},
              {no, 18, false, {}, {{0, expression("20")}}},
              {join, 43, false, {}, {{0, sct::SctSupplementaryTextReference{text}}}},
              {doc.allocateInstructionId(), 12}}}});
    const auto context = scriptResourceContext(doc);
    ASSERT_EQ(context["transitions"].size(), 1);
    EXPECT_EQ(context["transitions"][0]["target"]["text"], "me033b");
    bool found = false;
    for (const auto &region : context["regions"])
        if (region["controller"] == std::to_string(condition.value())) {
            found = true;
            EXPECT_EQ(region["continuation"], std::to_string(join.value()));
            ASSERT_EQ(region["arms"].size(), 2);
            for (const auto &arm : region["arms"])
                for (const auto &id : arm["instructions"])
                    EXPECT_NE(id, std::to_string(join.value()));
        }
    EXPECT_TRUE(found);
}
void normalize(Json &facts) {
    facts.erase("loadMs");
    facts.erase("extractMs");
}
TEST(ConnectivitySurveyTest, SharedExtractionKeepsSetAndClearAndQualifiedQueries) {
    auto data = bytes();
    auto f = extract(data, identity(data));
    ASSERT_EQ(f.at("status"), "analyzed");
    ASSERT_EQ(f.at("accesses").size(), 3);
    ASSERT_EQ(f.at("scopes").size(), 1);
    auto q = select(f, "variable", "BitVar:1960");
    EXPECT_EQ(q.at("records").size(), 3);
    EXPECT_EQ(q["records"][0]["operation"], "set");
    EXPECT_EQ(q["records"][1]["operation"], "clear");
    auto summary = summarize(f);
    EXPECT_EQ(summary["variables"]["BitVar:1960"]["read"], 1);
    EXPECT_EQ(summary["scriptGroup"], "field");
    EXPECT_EQ(select(f, "cutscene", f["scopes"][0]["id"].get<std::string>())["records"].size(), 2);
    EXPECT_EQ(select(f, "entrypoint", f["sections"][3]["id"].get<std::string>())["records"].size(), 1);
    auto again = extract(data, identity(data));
    normalize(f);
    normalize(again);
    EXPECT_EQ(f, again);
}
TEST(ConnectivitySurveyTest, CompressionVerifiesChecksumAndRejectsMalformedData) {
    Temporary temp;
    auto data = bytes();
    auto f = extract(data, identity(data));
    auto file = temp.root / "facts.zlib";
    writeFacts(file, f);
    EXPECT_EQ(readFacts(file), f);
    auto first = readBytes(file);
    writeFacts(file, f);
    EXPECT_EQ(readBytes(file), first);
    writeText(file, "corrupt");
    EXPECT_THROW(readFacts(file), std::exception);
    auto bad = identity(data);
    bad["sha256"] = "wrong";
    EXPECT_THROW(extract(data, bad), std::exception);
}
TEST(ConnectivitySurveyTest, IncidenceStorageDoesNotAllocateTheCartesianProduct) {
    std::vector<SctStateAccess> accesses;
    SctVariableKey key{SctVariableKind::Bit, 1960};
    for (int i = 0; i < 10000; ++i) {
        SctStateAccess a{};
        a.variable = key;
        a.kind = i < 5000 ? SctStateAccessKind::Set : SctStateAccessKind::Read;
        accesses.push_back(a);
    }
    auto index = SctStateConnectivityIndex::build(accesses);
    EXPECT_EQ(index.pairCount(), 25000000ull);
    ASSERT_EQ(index.variables.size(), 1);
    EXPECT_EQ(index.variables.at(key).writers.size() + index.variables.at(key).readers.size(), accesses.size());
    unsigned visits = 0;
    EXPECT_TRUE(index.anyLink(accesses, [&](const auto &) {
        ++visits;
        return true;
    }));
    EXPECT_EQ(visits, 1);
}
int invoke(std::vector<std::wstring> arguments) {
    std::wstring executable(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    executable.resize(n);
    auto exe = fs::path(executable).parent_path() / "SalsaConnectivitySurvey.exe";
    if (!fs::exists(exe))
        throw std::runtime_error("Build SalsaConnectivitySurvey before running CLI regressions");
    std::atomic_bool stop = false;
    auto result = runIsolated(exe, arguments, std::chrono::seconds(30), stop);
    if (!result.interruption.empty())
        throw std::runtime_error("CLI regression timed out");
    return static_cast<int>(result.exitCode);
}
TEST(ConnectivitySurveyTest, CliParallelResumeCorruptionAndPartialFailure) {
    Temporary temp;
    auto input = temp.root / "input";
    fs::create_directories(input);
    auto data = bytes();
    std::string raw(reinterpret_cast<const char *>(data.data()), data.size());
    writeText(input / "me002a.sct", raw);
    writeText(input / "me002b.sct", raw);
    Json config = {{"format", "jahorta.salsa.connectivity-survey-input"},
                   {"schemaVersion", 1},
                   {"datasets", Json::array({{{"id", "synthetic"},
                                              {"release", "synthetic"},
                                              {"platform", "dreamcast"},
                                              {"region", "eu"},
                                              {"disc", "1"},
                                              {"root", input.string()}}})}};
    auto mld = mldBytes();
    writeText(input / "a002a.mld", std::string(reinterpret_cast<const char *>(mld.data()), mld.size()));
    config["datasets"][0]["mlds"] =
        Json::array({{{"path", "a002a.mld"}, {"role", "base"}, {"scripts", Json::array({"me002a.sct"})}},
                     {{"path", "missing.mld"}, {"role", "auxiliary"}, {"scripts", Json::array({"me002a.sct"})}}});
    auto manifest = temp.root / "manifest.json", output = temp.root / "output";
    writeText(manifest, config.dump());
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--jobs", L"2"}), 0);
    auto first = readJson(output / "survey.json");
    EXPECT_EQ(first["summary"]["analyzed"], 2);
    EXPECT_EQ(first["summary"]["cached"], 0);
    EXPECT_EQ(first["summary"]["resources"]["analyzed"], 1);
    EXPECT_EQ(first["summary"]["resources"]["failed"], 1);
    EXPECT_EQ(first["summary"]["resources"]["directHandlerBindings"], 2);
    EXPECT_EQ(invoke({L"query", L"--survey", output.wstring(), L"--resource", L"synthetic:a002a.mld", L"--json"}), 0);
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--resume"}), 0);
    EXPECT_EQ(readJson(output / "summary.json")["cached"], 2);
    auto factFile = output / "facts" / first["files"][0]["facts"].get<std::string>();
    writeText(factFile, "broken");
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--resume", L"--jobs",
                      L"1"}),
              0);
    EXPECT_EQ(readJson(output / "summary.json")["cached"], 1);
    EXPECT_EQ(
        invoke({L"query", L"--survey", output.wstring(), L"--variable", L"BitVar:1960", L"--limit", L"1", L"--json"}),
        0);
    auto serial = temp.root / "serial";
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", serial.wstring(), L"--jobs", L"1"}), 0);
    auto parallelFacts = readFacts(factFile),
         serialFacts = readFacts(serial / "facts" / first["files"][0]["facts"].get<std::string>());
    normalize(parallelFacts);
    normalize(serialFacts);
    EXPECT_EQ(parallelFacts, serialFacts);
    auto changedMld = mldBytes(4000);
    writeText(input / "a002a.mld", std::string(reinterpret_cast<const char *>(changedMld.data()), changedMld.size()));
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--resume"}), 0);
    EXPECT_EQ(readJson(output / "summary.json")["cached"], 2);
    EXPECT_EQ(readJson(output / "summary.json")["resources"]["cached"], 0);
    EXPECT_EQ(readJson(output / "summary.json")["resources"]["signalReadBindings"], 0);
    writeText(input / "bad.sct", "bad");
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--resume"}), 1);
    EXPECT_EQ(readJson(output / "summary.json")["analyzed"], 2);
    EXPECT_EQ(readJson(output / "summary.json")["failed"], 1);
    EXPECT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", input.wstring()}), 2);
    writeText(input / "me002a.sct", "changed");
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--resume"}), 1);
    auto changed = readJson(output / "summary.json");
    EXPECT_EQ(changed["cached"], 1);
    EXPECT_EQ(changed["analyzed"], 1);
    EXPECT_EQ(changed["failed"], 2);
    config["schemaVersion"] = 99;
    writeText(manifest, config.dump());
    EXPECT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", (temp.root / "unsupported").wstring()}),
              2);
}
} // namespace

TEST(ConnectivitySurveyProcessTest, TimeoutCancellationAndExitStatusAreDistinct) {
    wchar_t windows[MAX_PATH];
    ASSERT_GT(GetWindowsDirectoryW(windows, MAX_PATH), 0u);
    auto exe = std::filesystem::path(windows) / L"System32/WindowsPowerShell/v1.0/powershell.exe";
    std::atomic_bool stop = false;
    auto timed = salsa::survey::runIsolated(exe, {L"-NoProfile", L"-Command", L"Start-Sleep -Seconds 60"},
                                            std::chrono::milliseconds(100), stop);
    EXPECT_EQ(timed.interruption, "timeout");
    EXPECT_LT(timed.wallMs, 10000);
    stop = true;
    auto cancelled = salsa::survey::runIsolated(exe, {L"-NoProfile", L"-Command", L"Start-Sleep -Seconds 60"},
                                                std::chrono::seconds(60), stop);
    EXPECT_EQ(cancelled.interruption, "cancelled");
    stop = false;
    auto exited =
        salsa::survey::runIsolated(exe, {L"-NoProfile", L"-Command", L"exit 7"}, std::chrono::seconds(10), stop);
    EXPECT_TRUE(exited.interruption.empty());
    EXPECT_EQ(exited.exitCode, 7u);
}

TEST(ConnectivitySurveyPathsTest, ConditionalEffectsPreserveBothWritesAndDatasetIsolation) {
    auto data=bytes();auto a=extract(data,identity(data));auto b=a;
    b["identity"]["key"]="synthetic:me002b.sct";b["identity"]["path"]="me002b.sct";
    auto graph=SctStoryPaths::propose({a,b});ASSERT_TRUE(graph);
    bool enables=false,inhibits=false;
    for(const auto& e:graph.value()["edges"]) {enables|=e["kind"]=="enables";inhibits|=e["kind"]=="inhibits";}
    EXPECT_TRUE(enables);EXPECT_TRUE(inhibits);
    b["identity"]["dataset"]="another-disc";
    auto isolated=SctStoryPaths::propose({a,b});ASSERT_TRUE(isolated);
    for(const auto& e:isolated.value()["edges"])EXPECT_FALSE(e["kind"]=="enables"||e["kind"]=="inhibits");
    EXPECT_EQ(SctStoryPaths::propose({a}).value(),SctStoryPaths::propose({a}).value());
}
TEST(ConnectivitySurveyPathsTest, CyclesAndBoundsRemainExplicit) {
    auto data=bytes();auto a=extract(data,identity(data));
    auto scene=a["instructions"][2]["id"],handler=a["instructions"][5]["id"];
    a["controlFlow"].push_back({{"source",scene},{"target",handler},{"kind",5},{"site",nullptr}});
    auto graph=SctStoryPaths::propose({a});ASSERT_TRUE(graph);
    EXPECT_TRUE(std::ranges::any_of(graph.value()["components"],[](const auto& c){return c.at("cycle")==true;}));
    SctStoryPathOptions options;options.maximumEdges=1;options.maximumPaths=1;options.maximumSteps=1;
    auto bounded=SctStoryPaths::propose({a},{},options);ASSERT_TRUE(bounded);EXPECT_TRUE(bounded.value()["truncated"]);
    options.start="unavailable";EXPECT_FALSE(SctStoryPaths::propose({a},{},options));
}
