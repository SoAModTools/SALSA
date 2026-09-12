#include "../../src/SalsaConnectivitySurvey/Survey.h"
#include "../../src/SalsaConnectivitySurvey/Worker.h"
#include "SalsaCore/Authoring/SctInterpretation.h"
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
    auto manifest = temp.root / "manifest.json", output = temp.root / "output";
    writeText(manifest, config.dump());
    ASSERT_EQ(invoke({L"scan", L"--manifest", manifest.wstring(), L"--output", output.wstring(), L"--jobs", L"2"}), 0);
    auto first = readJson(output / "survey.json");
    EXPECT_EQ(first["summary"]["analyzed"], 2);
    EXPECT_EQ(first["summary"]["cached"], 0);
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
