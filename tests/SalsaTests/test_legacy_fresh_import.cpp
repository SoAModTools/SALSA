#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Legacy/LegacyFreshImport.h"

#include <Windows.h>
#include <gtest/gtest.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <ranges>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {
namespace {

class FreshImportTemporaryDirectory final {
public:
    FreshImportTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-fresh-import-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~FreshImportTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_{};
};

struct FixtureScript final {
    std::string key;
    std::string storedName;
    bool accepted = true;
};

[[nodiscard]] std::string digestText(const std::string& text) {
    return sha256(std::as_bytes(std::span{text.data(), text.size()})).value().toHex();
}

[[nodiscard]] std::string digestBytes(const std::vector<std::uint8_t>& bytes) {
    return sha256(std::as_bytes(std::span{bytes.data(), bytes.size()})).value().toHex();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

void writeFile(const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::filesystem::path makeFreshImportCapsule(
    const std::filesystem::path& parent,
    const std::vector<FixtureScript>& fixtureScripts) {
    const auto root = parent / L"input.salsa-legacy";
    nlohmann::ordered_json projectRecord{
        {"formatId", "jahorta.salsa.legacy-project-record"}, {"schemaVersion", 2},
        {"fieldDispositions", nlohmann::ordered_json::object()},
        {"fields", {{"global_variables", nlohmann::ordered_json::array(
                        {11, 1, nlohmann::ordered_json::array()})},
                    {"version", nlohmann::ordered_json::array({3, "7"})},
                    {"inst_id_colors", nlohmann::ordered_json::array(
                        {11, 2, nlohmann::ordered_json::array()})}}},
        {"normalizations", nlohmann::ordered_json::array()},
        {"diagnostics", nlohmann::ordered_json::array()}};
    const auto project = nlohmann::ordered_json::to_cbor(projectRecord);
    writeFile(root / L"project.cbor", project);
    const std::string sourceHash(64, '0');
    const auto projectHash = digestBytes(project);
    std::string identity(LegacyConverterContractId);
    identity.push_back('\0'); identity += sourceHash; identity.push_back('\0');
    identity += "project.cbor"; identity.push_back('\0'); identity += projectHash;
    identity.push_back('\0'); identity += std::to_string(project.size()); identity.push_back('\n');
    nlohmann::ordered_json scripts = nlohmann::ordered_json::array();
    nlohmann::ordered_json entries = nlohmann::ordered_json::array({{{"path", "project.cbor"},
        {"size", project.size()}, {"sha256", projectHash}, {"canonical", true}}});
    bool actionRequired = false;
    for (std::uint32_t ordinal = 0; ordinal < fixtureScripts.size(); ++ordinal) {
        const auto& fixture = fixtureScripts[ordinal];
        actionRequired = actionRequired || !fixture.accepted;
        const auto nullValue = nlohmann::ordered_json::array({1});
        const auto legacyString = [](const std::string_view text) {
            return nlohmann::ordered_json::array({5,
                nlohmann::ordered_json::binary(std::vector<std::uint8_t>(
                    text.begin(), text.end()))});
        };
        const auto legacyList = [](const std::uint64_t id,
            nlohmann::ordered_json items = nlohmann::ordered_json::array()) {
            return nlohmann::ordered_json::array({7, id, std::move(items)});
        };
        const auto legacyDictionary = [](const std::uint64_t id,
            nlohmann::ordered_json items = nlohmann::ordered_json::array()) {
            return nlohmann::ordered_json::array({11, id, std::move(items)});
        };
        nlohmann::ordered_json sidecar{
            {"folded_sects", nullValue}, {"index", nullValue}, {"sect_tree", nullValue},
            {"sect_list", nullValue}, {"string_garbage", nullValue},
            {"unused_sections", nullValue}, {"errors", nullValue},
            {"error_sections", nullValue}, {"variables", nullValue}};
        nlohmann::ordered_json diagnostics = nlohmann::ordered_json::array();
        if (!fixture.accepted) diagnostics.push_back({{"code", "fixture-failure"},
            {"path", "script"}, {"message", "Synthetic script failure."}});
        nlohmann::ordered_json ir = nullptr;
        if (fixture.accepted) {
            const auto instructionId = std::string{"fixture-instruction"};
            nlohmann::ordered_json instructionFields = nlohmann::ordered_json::array({
                legacyString(instructionId), nlohmann::ordered_json::array({3, "12"}),
                nlohmann::ordered_json::array({2, false}), nullValue,
                legacyList(5), legacyList(6), legacyDictionary(7), legacyList(8),
                legacyList(9), legacyList(10), legacyString(""),
                nlohmann::ordered_json::array({2, true})});
            const auto instruction = nlohmann::ordered_json::array(
                {12, 4, 4, std::move(instructionFields)});
            auto instructionPairs = nlohmann::ordered_json::array();
            instructionPairs.push_back(nlohmann::ordered_json::array(
                {legacyString(instructionId), std::move(instruction)}));
            nlohmann::ordered_json sectionFields = nlohmann::ordered_json::array({
                legacyString("main"), legacyDictionary(3, std::move(instructionPairs)),
                legacyList(11), legacyList(12,
                    nlohmann::ordered_json::array({legacyString(instructionId)})),
                legacyList(13), legacyList(14), legacyDictionary(15),
                legacyDictionary(16), legacyString(""),
                nlohmann::ordered_json::array({2, false}), legacyString("Script")});
            const auto section = nlohmann::ordered_json::array(
                {12, 2, 3, std::move(sectionFields)});
            auto sectionPairs = nlohmann::ordered_json::array();
            sectionPairs.push_back(nlohmann::ordered_json::array(
                {legacyString("main"), std::move(section)}));
            sidecar["sect_list"] = legacyList(17,
                nlohmann::ordered_json::array({legacyString("main")}));
            ir = {{"header", nullValue}, {"footer", nullValue},
                {"string_groups", nullValue}, {"strings", nullValue},
                {"sections", legacyDictionary(1, std::move(sectionPairs))},
                {"links", nullValue}, {"sidecar", std::move(sidecar)}};
        }
        nlohmann::ordered_json record{
            {"formatId", "jahorta.salsa.legacy-script-record"}, {"schemaVersion", 2},
            {"ordinal", ordinal}, {"key", fixture.key}, {"storedName", fixture.storedName},
            {"status", fixture.accepted ? "accepted" : "failed"},
            {"counts", {{"sections", fixture.accepted ? 1 : 0},
                {"instructions", fixture.accepted ? 1 : 0}, {"parameters", 0},
                {"links", 0}, {"strings", 0}}},
            {"diagnostics", diagnostics}, {"ir", std::move(ir)}};
        const auto raw = nlohmann::ordered_json::to_cbor(record);
        std::vector<std::uint8_t> compressed;
        if (lodepng::compress(compressed, raw) != 0)
            throw std::runtime_error("test script compression failed");
        std::ostringstream relative;
        relative << "scripts/" << std::setw(6) << std::setfill('0') << ordinal
            << ".cbor.zlib";
        writeFile(root / std::filesystem::path(relative.str()), compressed);
        const auto scriptHash = digestBytes(compressed);
        identity += relative.str(); identity.push_back('\0'); identity += scriptHash;
        identity.push_back('\0'); identity += std::to_string(compressed.size());
        identity.push_back('\n');
        scripts.push_back({{"ordinal", ordinal}, {"key", fixture.key},
            {"storedName", fixture.storedName},
            {"status", fixture.accepted ? "accepted" : "failed"},
            {"sections", fixture.accepted ? 1 : 0},
            {"instructions", fixture.accepted ? 1 : 0}, {"parameters", 0},
            {"strings", 0}, {"diagnostics", diagnostics.size()},
            {"path", relative.str()}});
        entries.push_back({{"path", relative.str()}, {"size", compressed.size()},
            {"sha256", scriptHash}, {"canonical", true}});
    }
    nlohmann::ordered_json manifest{
        {"formatId", LegacyCapsuleFormatId}, {"schemaVersion", LegacyCapsuleSchemaVersion},
        {"converterContractId", LegacyConverterContractId},
        {"capsuleId", digestText(identity)},
        {"status", actionRequired ? "action-required" : "ready"},
        {"source", {{"filename", "fixture.prj"}, {"size", 123}, {"sha256", sourceHash},
            {"pickleProtocol", 4}, {"projectVersion", 7}, {"originalRetained", false}}},
        {"normalizationCount", 0}, {"scripts", std::move(scripts)},
        {"entries", std::move(entries)}};
    auto encoded = manifest.dump(2); encoded.push_back('\n');
    writeFile(root / L"capsule.json", encoded);
    return root;
}

[[nodiscard]] FreshLegacyImportRequest requestFor(
    const FreshImportTemporaryDirectory& temporary,
    const std::filesystem::path& capsule) {
    FreshLegacyImportRequest request;
    request.capsuleRoot = capsule;
    request.sourceDirectory = temporary.path() / L"fresh-source";
    request.workspaceDirectory = temporary.path() / L"fresh-workspace";
    request.targetScope = LegacyImportTargetScope::DreamcastDisc1;
    request.publication.platform = spice::sct::SctPlatform::Dreamcast;
    request.publication.textEncoding = spice::sct::kSctWindows1252Byte7FEncoding;
    request.publication.byteOrder = spice::sct::SctDocumentOutputByteOrder::LittleEndian;
    return request;
}

TEST(LegacyFreshImportTest, ProducesDeterministicReadOnlyPlanForFreshDestinations) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    const auto request = requestFor(temporary, capsule);
    const auto first = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(first) << first.diagnostics().front().message;
    ASSERT_TRUE(first.value().ready());
    ASSERT_EQ(first.value().scripts.size(), 1u);
    EXPECT_EQ(first.value().scripts.front().status, FreshLegacyScriptPlanStatus::Ready);
    EXPECT_TRUE(first.value().scripts.front().reparseEquivalent);
    EXPECT_EQ(first.value().scripts.front().outputRelativePath, L"A001A.sct");
    EXPECT_FALSE(std::filesystem::exists(request.sourceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory));

    const auto second = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(second) << second.diagnostics().front().message;
    EXPECT_EQ(first.value().planId, second.value().planId);
    EXPECT_EQ(first.value().scripts.front().outputDigest,
        second.value().scripts.front().outputDigest);
}

TEST(LegacyFreshImportTest, RejectsNonFreshOrOverlappingDestinationsWithoutWrites) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {});
    auto request = requestFor(temporary, capsule);
    std::filesystem::create_directories(request.sourceDirectory);
    writeFile(request.sourceDirectory / L"existing.txt", "occupied");
    auto planned = LegacyFreshImportPlanner::plan(request);
    ASSERT_FALSE(planned);
    EXPECT_EQ(planned.diagnostics().front().code,
        DiagnosticCode::LegacyImportDestinationNotFresh);

    request.sourceDirectory = temporary.path() / L"parent";
    request.workspaceDirectory = request.sourceDirectory / L"workspace";
    planned = LegacyFreshImportPlanner::plan(request);
    ASSERT_FALSE(planned);
    EXPECT_EQ(planned.diagnostics().front().code,
        DiagnosticCode::LegacyImportInvalidRequest);
}

TEST(LegacyFreshImportTest, ExcludesFailedScriptAndPlansAcceptedRemainder) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(),
        {{"A001A", "A001A"}, {"A002A", "A002A", false}});
    auto request = requestFor(temporary, capsule);
    request.scriptDecisions.push_back({1u, false});
    const auto planned = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(planned) << planned.diagnostics().front().message;
    ASSERT_TRUE(planned.value().ready());
    ASSERT_EQ(planned.value().scripts.size(), 2u);
    EXPECT_EQ(planned.value().scripts[0].status, FreshLegacyScriptPlanStatus::Ready);
    EXPECT_EQ(planned.value().scripts[1].status, FreshLegacyScriptPlanStatus::Excluded);
}

TEST(LegacyFreshImportTest, RequiresExplicitUniqueValidStemsWithinTarget) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(),
        {{"A001A", "A001A"}, {"A002A", "A002A"}});
    auto request = requestFor(temporary, capsule);
    request.scriptDecisions.push_back({1u, true, "a001a"});
    const auto collision = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(collision);
    EXPECT_FALSE(collision.value().ready());
    EXPECT_EQ(collision.value().scripts[1].status, FreshLegacyScriptPlanStatus::Blocked);

    request.scriptDecisions[0].remappedStem = "A003A";
    const auto remapped = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(remapped);
    EXPECT_TRUE(remapped.value().ready());
    EXPECT_EQ(remapped.value().scripts[1].outputRelativePath, L"A003A.sct");
}

TEST(LegacyFreshImportTest, ReportsMonotonicScriptProgress) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(),
        {{"A001A", "A001A"}, {"A002A", "A002A"}});
    const auto request = requestFor(temporary, capsule);
    std::vector<FreshLegacyImportProgress> progress;
    const auto planned = LegacyFreshImportPlanner::plan(request, {}, {},
        [&](const auto& update) { progress.push_back(update); });
    ASSERT_TRUE(planned);
    std::uint64_t previous = 0;
    std::size_t events = 0;
    for (const auto& update : progress) {
        if (update.phase != FreshLegacyImportPhase::ConvertingScripts) continue;
        EXPECT_GE(update.completed, previous);
        EXPECT_LE(update.completed, update.total);
        previous = update.completed;
        ++events;
    }
    EXPECT_EQ(previous, 2u);
    EXPECT_EQ(events, 3u);
}

TEST(LegacyFreshImportTest, DetectsShardChangesBetweenValidationAndTranslation) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(),
        {{"A001A", "A001A"}, {"A002A", "A002A"}});
    const auto request = requestFor(temporary, capsule);
    bool changed = false;
    const auto planned = LegacyFreshImportPlanner::plan(request, {}, {},
        [&](const auto& update) {
            if (changed || update.phase != FreshLegacyImportPhase::ValidatingCapsule
                || update.completed != 1u) return;
            std::ofstream output(capsule / L"scripts" / L"000000.cbor.zlib",
                std::ios::binary | std::ios::app);
            output.put('\0');
            changed = true;
        });
    ASSERT_TRUE(planned) << planned.diagnostics().front().message;
    ASSERT_TRUE(changed);
    ASSERT_EQ(planned.value().scripts.size(), 2u);
    EXPECT_EQ(planned.value().scripts.front().status, FreshLegacyScriptPlanStatus::Blocked);
    ASSERT_FALSE(planned.value().scripts.front().reasons.empty());
    EXPECT_EQ(planned.value().scripts.front().reasons.front(),
        "A legacy capsule shard size changed after validation.");
    EXPECT_EQ(planned.value().scripts.back().status, FreshLegacyScriptPlanStatus::Ready);
}

TEST(LegacyFreshImportTest, CancellationStopsBeforeTheNextScript) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(),
        {{"A001A", "A001A"}, {"A002A", "A002A"}});
    const auto request = requestFor(temporary, capsule);
    std::stop_source cancellation;
    const auto planned = LegacyFreshImportPlanner::plan(request, {},
        cancellation.get_token(), [&](const auto& update) {
            if (update.phase == FreshLegacyImportPhase::ConvertingScripts
                && update.completed == 1u) cancellation.request_stop();
        });
    ASSERT_FALSE(planned);
    EXPECT_EQ(planned.diagnostics().front().code, DiagnosticCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(request.sourceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory));
}

TEST(LegacyFreshImportTest, MetadataDiscardIsExplicitAndChangesPlanIdentity) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    auto request = requestFor(temporary, capsule);
    const auto retained = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(retained);
    request.discardUnsupportedMetadata = true;
    const auto discarded = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(discarded);
    EXPECT_NE(retained.value().planId, discarded.value().planId);
    EXPECT_TRUE(std::ranges::any_of(retained.value().metadata, [](const auto& record) {
        return record.disposition == LegacyMetadataDisposition::Pending;
    }));
    EXPECT_TRUE(std::ranges::none_of(discarded.value().metadata, [](const auto& record) {
        return record.disposition == LegacyMetadataDisposition::Pending;
    }));
    EXPECT_TRUE(std::ranges::any_of(discarded.value().metadata, [](const auto& record) {
        return record.disposition == LegacyMetadataDisposition::DroppedByUser;
    }));
}

TEST(LegacyFreshImportTest, EmptyImportIsNotReadyToCommit) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {});
    const auto planned = LegacyFreshImportPlanner::plan(requestFor(temporary, capsule));
    ASSERT_TRUE(planned);
    EXPECT_FALSE(planned.value().ready());
}

TEST(LegacyFreshImportTest, ValidatesOptionalPrivateOrganicCapsuleWithoutWriting) {
    std::wstring value(32'768, L'\0');
    const auto length = GetEnvironmentVariableW(L"SALSA_LEGACY_CAPSULE_FIXTURE",
        value.data(), static_cast<DWORD>(value.size()));
    if (length == 0) GTEST_SKIP() << "No private capsule fixture was requested.";
    ASSERT_LT(length, value.size());
    value.resize(length);
    FreshImportTemporaryDirectory temporary;
    const auto request = requestFor(temporary, value);
    const auto planned = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(planned) << planned.diagnostics().front().message;
    EXPECT_FALSE(planned.value().scripts.empty());
    EXPECT_EQ(planned.value().scripts.size(),
        LegacyCapsuleReader::validate(value).value().scripts.size());
    std::map<std::string, std::size_t, std::less<>> firstReasons;
    for (const auto& script : planned.value().scripts)
        if (script.status == FreshLegacyScriptPlanStatus::Blocked && !script.reasons.empty())
            ++firstReasons[script.reasons.front()];
    std::ostringstream reasonSummary;
    for (const auto& [reason, count] : firstReasons)
        reasonSummary << count << " x " << reason << '\n';
    EXPECT_TRUE(std::ranges::any_of(planned.value().scripts, [](const auto& script) {
        return script.status == FreshLegacyScriptPlanStatus::Ready;
    })) << reasonSummary.str();
    EXPECT_TRUE(std::ranges::all_of(planned.value().scripts, [](const auto& script) {
        return script.status != FreshLegacyScriptPlanStatus::Blocked
            || !script.reasons.empty();
    }));
    EXPECT_FALSE(std::filesystem::exists(request.sourceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory));
}

}  // namespace
}  // namespace salsa::core
