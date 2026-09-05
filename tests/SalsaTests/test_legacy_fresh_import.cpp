#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Legacy/LegacyFreshImport.h"
#include "SalsaCore/Legacy/LegacyConversionService.h"
#include "SalsaCore/Legacy/LegacyMetadataPromotion.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include <Windows.h>
#include <gtest/gtest.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
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
    bool littleEndianScptOverride = false;
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

[[nodiscard]] Result<Sha256Digest> digestFile(const std::filesystem::path& path) {
    auto hasher = Sha256Hasher::create();
    if (!hasher) return Result<Sha256Digest>::failure(hasher.diagnostics());
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<Sha256Digest>::failure({{DiagnosticSeverity::Error,
        DiagnosticCode::PersistenceReadFailed, "The fixture could not be opened.", path}});
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            const auto bytes = std::as_bytes(std::span{
                buffer.data(), static_cast<std::size_t>(count)});
            auto updated = hasher.value().update(bytes);
            if (!updated) return Result<Sha256Digest>::failure(updated.diagnostics());
        }
    }
    if (!input.eof()) return Result<Sha256Digest>::failure({{DiagnosticSeverity::Error,
        DiagnosticCode::PersistenceReadFailed, "The fixture could not be read completely.", path}});
    return hasher.value().finish();
}

[[nodiscard]] std::filesystem::path executableDirectory() {
    std::wstring path(32'768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

[[nodiscard]] std::filesystem::path makeFreshImportCapsule(
    const std::filesystem::path& parent,
    const std::vector<FixtureScript>& fixtureScripts,
    const bool invalidInstructionColors = false,
    const bool includeAuthoringMetadata = false) {
    const auto root = parent / L"input.salsa-legacy";
    const std::string sourceContents(123, 'p');
    writeFile(parent / L"fixture.prj", sourceContents);
    const auto legacyString = [](const std::string_view text) {
        return nlohmann::ordered_json::array({5,
            nlohmann::ordered_json::binary(std::vector<std::uint8_t>(
                text.begin(), text.end()))});
    };
    const auto legacyInteger = [](const std::int64_t value) {
        return nlohmann::ordered_json::array({3, std::to_string(value)});
    };
    const auto legacyList = [](const std::uint64_t id,
        nlohmann::ordered_json items = nlohmann::ordered_json::array()) {
        return nlohmann::ordered_json::array({7, id, std::move(items)});
    };
    const auto legacyDictionary = [](const std::uint64_t id,
        nlohmann::ordered_json items = nlohmann::ordered_json::array()) {
        return nlohmann::ordered_json::array({11, id, std::move(items)});
    };
    auto projectAliases = legacyDictionary(1);
    auto projectColors = legacyDictionary(2);
    if (includeAuthoringMetadata) {
        projectAliases = legacyDictionary(1, nlohmann::ordered_json::array({
            nlohmann::ordered_json::array({legacyString("BitVar"),
                legacyDictionary(2, nlohmann::ordered_json::array({
                    nlohmann::ordered_json::array({legacyInteger(4),
                        legacyDictionary(3, nlohmann::ordered_json::array({
                            nlohmann::ordered_json::array({legacyString("alias"),
                                legacyString("ProjectFlag")})}))})}))})}));
        projectColors = legacyDictionary(4, nlohmann::ordered_json::array({
            nlohmann::ordered_json::array({legacyInteger(12), legacyString("#123456")})}));
    }
    nlohmann::ordered_json projectRecord{
        {"formatId", "jahorta.salsa.legacy-project-record"}, {"schemaVersion", 2},
        {"fieldDispositions", nlohmann::ordered_json::object()},
        {"fields", {{"global_variables", std::move(projectAliases)},
                    {"version", nlohmann::ordered_json::array({3, "7"})},
                    {"inst_id_colors", std::move(projectColors)}}},
        {"normalizations", nlohmann::ordered_json::array()},
        {"diagnostics", nlohmann::ordered_json::array()}};
    if (invalidInstructionColors)
        projectRecord["diagnostics"].push_back({
            {"code", "invalid-instruction-colors"},
            {"path", "project.inst_id_colors"},
            {"message", "Instruction colors do not have the official v7 dictionary shape."}});
    const auto project = nlohmann::ordered_json::to_cbor(projectRecord);
    writeFile(root / L"project.cbor", project);
    const std::string sourceHash = digestText(sourceContents);
    const auto projectHash = digestBytes(project);
    std::string identity(LegacyConverterContractId);
    identity.push_back('\0'); identity += sourceHash; identity.push_back('\0');
    identity += "project.cbor"; identity.push_back('\0'); identity += projectHash;
    identity.push_back('\0'); identity += std::to_string(project.size()); identity.push_back('\n');
    nlohmann::ordered_json scripts = nlohmann::ordered_json::array();
    nlohmann::ordered_json entries = nlohmann::ordered_json::array({{{"path", "project.cbor"},
        {"size", project.size()}, {"sha256", projectHash}, {"canonical", true}}});
    bool actionRequired = invalidInstructionColors;
    for (std::uint32_t ordinal = 0; ordinal < fixtureScripts.size(); ++ordinal) {
        const auto& fixture = fixtureScripts[ordinal];
        actionRequired = actionRequired || !fixture.accepted;
        const std::uint64_t addedParameterNodes =
            fixture.littleEndianScptOverride ? 3u : 0u;
        const auto nullValue = nlohmann::ordered_json::array({1});
        const auto legacyBytes = [](std::vector<std::uint8_t> bytes) {
            return nlohmann::ordered_json::array(
                {6, nlohmann::ordered_json::binary(std::move(bytes))});
        };
        nlohmann::ordered_json sidecar{
            {"folded_sects", nullValue}, {"index", nullValue}, {"sect_tree", nullValue},
            {"sect_list", nullValue}, {"string_garbage", nullValue},
            {"unused_sections", nullValue}, {"errors", nullValue},
            {"error_sections", nullValue}, {"variables", nullValue}};
        if (includeAuthoringMetadata) {
            sidecar["variables"] = legacyDictionary(21 + addedParameterNodes,
                nlohmann::ordered_json::array({
                    nlohmann::ordered_json::array({legacyString("IntVar"),
                        legacyDictionary(22 + addedParameterNodes, nlohmann::ordered_json::array({
                            nlohmann::ordered_json::array({legacyInteger(7),
                                legacyDictionary(23 + addedParameterNodes, nlohmann::ordered_json::array({
                                    nlohmann::ordered_json::array({legacyString("alias"),
                                        legacyString("LocalCounter")})}))})}))})}));
            const auto groupChildren = legacyList(19 + addedParameterNodes);
            const auto group = legacyDictionary(18 + addedParameterNodes,
                nlohmann::ordered_json::array({nlohmann::ordered_json::array({
                    legacyString("main|group"), groupChildren})}));
            sidecar["sect_tree"] = legacyList(17 + addedParameterNodes,
                nlohmann::ordered_json::array({group}));
        }
        nlohmann::ordered_json diagnostics = nlohmann::ordered_json::array();
        if (!fixture.accepted) diagnostics.push_back({{"code", "fixture-failure"},
            {"path", "script"}, {"message", "Synthetic script failure."}});
        nlohmann::ordered_json ir = nullptr;
        if (fixture.accepted) {
            const auto instructionId = std::string{"fixture-instruction"};
            auto opcode = std::string{"12"};
            auto parameters = legacyDictionary(7);
            if (fixture.littleEndianScptOverride) {
                opcode = "59";
                nlohmann::ordered_json parameterFields = nlohmann::ordered_json::array({
                    nlohmann::ordered_json::array({3, "0"}), legacyString("scpt|int"),
                    nullValue, legacyList(9), legacyDictionary(10),
                    legacyString("override"), legacyBytes({}), legacyString(""),
                    legacyBytes({0xffu, 0xffu, 0x7fu, 0x7fu})});
                auto parameterPairs = nlohmann::ordered_json::array();
                parameterPairs.push_back(nlohmann::ordered_json::array({
                    nlohmann::ordered_json::array({3, "0"}),
                    nlohmann::ordered_json::array(
                        {12, 8, 5, std::move(parameterFields)})}));
                parameters = legacyDictionary(7, std::move(parameterPairs));
            }
            nlohmann::ordered_json instructionFields = nlohmann::ordered_json::array({
                legacyString(instructionId), nlohmann::ordered_json::array({3, opcode}),
                nlohmann::ordered_json::array({2, false}), nullValue,
                legacyList(5), legacyList(6), std::move(parameters),
                legacyList(8 + addedParameterNodes),
                legacyList(9 + addedParameterNodes),
                legacyList(10 + addedParameterNodes), legacyString(""),
                nlohmann::ordered_json::array({2, true})});
            const auto instruction = nlohmann::ordered_json::array(
                {12, 4, 4, std::move(instructionFields)});
            auto instructionPairs = nlohmann::ordered_json::array();
            instructionPairs.push_back(nlohmann::ordered_json::array(
                {legacyString(instructionId), std::move(instruction)}));
            nlohmann::ordered_json sectionFields = nlohmann::ordered_json::array({
                legacyString("main"), legacyDictionary(3, std::move(instructionPairs)),
                legacyList(11 + addedParameterNodes), legacyList(12 + addedParameterNodes,
                    nlohmann::ordered_json::array({legacyString(instructionId)})),
                legacyList(13 + addedParameterNodes), legacyList(14 + addedParameterNodes),
                legacyDictionary(15 + addedParameterNodes),
                legacyDictionary(16 + addedParameterNodes), legacyString(""),
                nlohmann::ordered_json::array({2, false}), legacyString("Script")});
            const auto section = nlohmann::ordered_json::array(
                {12, 2, 3, std::move(sectionFields)});
            auto sectionPairs = nlohmann::ordered_json::array();
            sectionPairs.push_back(nlohmann::ordered_json::array(
                {legacyString("main"), std::move(section)}));
            sidecar["sect_list"] = legacyList(17 + addedParameterNodes
                    + (includeAuthoringMetadata ? 3u : 0u),
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
                {"instructions", fixture.accepted ? 1 : 0},
                {"parameters", fixture.littleEndianScptOverride ? 1 : 0},
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
            {"instructions", fixture.accepted ? 1 : 0},
            {"parameters", fixture.littleEndianScptOverride ? 1 : 0},
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

TEST(LegacyFreshImportTest, DecodesLegacyScptOverrideUsingSelectedByteOrder) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(
        temporary.path(), {{"A001A", "A001A", true, true}});
    auto request = requestFor(temporary, capsule);
    const auto littleEndian = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(littleEndian) << littleEndian.diagnostics().front().message;
    ASSERT_EQ(littleEndian.value().scripts.size(), 1u);
    EXPECT_EQ(littleEndian.value().scripts.front().status,
        FreshLegacyScriptPlanStatus::Ready);
    EXPECT_TRUE(littleEndian.value().scripts.front().reparseEquivalent);

    request.publication.byteOrder =
        spice::sct::SctDocumentOutputByteOrder::BigEndian;
    const auto bigEndian = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(bigEndian) << bigEndian.diagnostics().front().message;
    ASSERT_EQ(bigEndian.value().scripts.size(), 1u);
    EXPECT_EQ(bigEndian.value().scripts.front().status,
        FreshLegacyScriptPlanStatus::Blocked);
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
    const auto pending = std::ranges::find_if(retained.value().metadata,
        [](const auto& record) {
            return record.disposition == LegacyMetadataDisposition::Pending;
        });
    ASSERT_NE(pending, retained.value().metadata.end());
    request.metadataDecisions.push_back(
        {pending->recordId, LegacyMetadataDecisionAction::Drop});
    const auto discarded = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(discarded);
    EXPECT_NE(retained.value().planId, discarded.value().planId);
    EXPECT_TRUE(std::ranges::any_of(retained.value().metadata, [](const auto& record) {
        return record.disposition == LegacyMetadataDisposition::Pending;
    }));
    EXPECT_TRUE(std::ranges::any_of(discarded.value().metadata, [](const auto& record) {
        return record.disposition == LegacyMetadataDisposition::DroppedByUser;
    }));
}

TEST(LegacyFreshImportTest, InvalidProjectMetadataBlocksUntilExplicitlyDiscarded) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(
        temporary.path(), {{"A001A", "A001A"}}, true);
    auto request = requestFor(temporary, capsule);
    const auto blocked = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(blocked);
    EXPECT_FALSE(blocked.value().ready());
    const auto invalid = std::ranges::find_if(blocked.value().metadata,
        [](const auto& record) {
            return record.kind == LegacyMetadataKind::OpcodeColors
                && record.disposition == LegacyMetadataDisposition::Invalid;
        });
    ASSERT_NE(invalid, blocked.value().metadata.end());
    request.metadataDecisions.push_back(
        {invalid->recordId, LegacyMetadataDecisionAction::Drop});
    const auto discarded = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(discarded);
    EXPECT_TRUE(discarded.value().ready());
    EXPECT_TRUE(std::ranges::any_of(discarded.value().metadata, [](const auto& record) {
        return record.kind == LegacyMetadataKind::OpcodeColors
            && record.disposition == LegacyMetadataDisposition::DroppedByUser;
    }));
}

TEST(LegacyFreshImportTest, EmptyImportIsNotReadyToCommit) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {});
    const auto planned = LegacyFreshImportPlanner::plan(requestFor(temporary, capsule));
    ASSERT_TRUE(planned);
    EXPECT_FALSE(planned.value().ready());
}

TEST(LegacyFreshImportTest, CommitsFreshDatasetWorkspaceCapsuleAndBaselines) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    const auto request = requestFor(temporary, capsule);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared) << prepared.diagnostics().front().message;
    ASSERT_TRUE(prepared.value().plan.ready());
    const auto result = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"});
    ASSERT_TRUE(result.succeeded()) << (result.diagnostics.empty()
        ? "no diagnostic" : result.diagnostics.front().message);
    ASSERT_TRUE(result.dataset.has_value());
    EXPECT_TRUE(std::filesystem::is_regular_file(request.sourceDirectory / L"A001A.sct"));
    EXPECT_TRUE(std::filesystem::is_regular_file(request.workspaceDirectory
        / L"imports" / prepared.value().plan.capsuleId / L"capsule" / L"capsule.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(request.workspaceDirectory
        / L"imports" / prepared.value().plan.capsuleId / L"report.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(request.workspaceDirectory
        / L"import-state" / (prepared.value().plan.capsuleId + ".json")));
    EXPECT_FALSE(std::filesystem::exists(request.sourceDirectory
        / L".salsa-legacy-import.json"));
    EXPECT_TRUE(std::filesystem::is_empty(temporary.path() / L"recovery"));

    auto workspace = LocalSalsaWorkspace::openOrCreate(
        request.workspaceDirectory, *result.dataset);
    ASSERT_TRUE(workspace);
    const auto revision = SourceRevision{*prepared.value().plan.scripts.front().outputDigest};
    const auto baseline = workspace.value().loadBaseline(revision);
    ASSERT_TRUE(baseline);
    EXPECT_TRUE(baseline.value().has_value());
    EXPECT_TRUE(std::filesystem::is_empty(request.workspaceDirectory / L"patches"));

    std::ifstream report(request.workspaceDirectory / L"imports"
        / prepared.value().plan.capsuleId / L"report.json", std::ios::binary);
    const std::string reportText{std::istreambuf_iterator<char>{report}, {}};
    EXPECT_EQ(reportText.find(temporary.path().string()), std::string::npos);
    EXPECT_NE(reportText.find("\"applicationVersion\""), std::string::npos);
}

TEST(LegacyFreshImportTest, ReplacesPreexistingEmptyDestinationsAndRemovesBackups) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    const auto request = requestFor(temporary, capsule);
    std::filesystem::create_directories(request.sourceDirectory);
    std::filesystem::create_directories(request.workspaceDirectory);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared);
    const auto committed = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"});
    ASSERT_TRUE(committed.succeeded()) << (committed.diagnostics.empty()
        ? "no diagnostic" : committed.diagnostics.front().message);
    EXPECT_TRUE(std::filesystem::is_regular_file(request.sourceDirectory / L"A001A.sct"));
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path()))
        EXPECT_EQ(entry.path().filename().wstring().find(L".salsa-backup-"),
            std::wstring::npos);
}

TEST(LegacyFreshImportTest, RecoversAfterOnlySourceWasPublished) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    const auto request = requestFor(temporary, capsule);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared);
    const auto interrupted = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"}, {
            [](const std::size_t published) { return published != 1u; }});
    ASSERT_EQ(interrupted.status, FreshLegacyImportCommitStatus::Interrupted);
    EXPECT_TRUE(std::filesystem::is_directory(request.sourceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory));
    const auto recovered = LegacyFreshImportRecoveryService::recoverAll(
        temporary.path() / L"recovery");
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered.front().status, FreshLegacyImportCommitStatus::Committed);
    EXPECT_TRUE(std::filesystem::is_directory(request.workspaceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.sourceDirectory
        / L".salsa-legacy-import.json"));
}

TEST(LegacyFreshImportTest, RecoversAfterBothDestinationsWerePublished) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    const auto request = requestFor(temporary, capsule);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared);
    const auto interrupted = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"}, {
            [](const std::size_t published) { return published != 2u; }});
    ASSERT_EQ(interrupted.status, FreshLegacyImportCommitStatus::Interrupted);
    EXPECT_TRUE(std::filesystem::is_directory(request.sourceDirectory));
    EXPECT_TRUE(std::filesystem::is_directory(request.workspaceDirectory));
    const auto recovered = LegacyFreshImportRecoveryService::recoverAll(
        temporary.path() / L"recovery");
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered.front().status, FreshLegacyImportCommitStatus::Committed);
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory
        / L".salsa-legacy-import.json"));
}

TEST(LegacyFreshImportTest, CommitRejectsOriginalAndDestinationDriftWithoutPublishing) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    auto request = requestFor(temporary, capsule);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared);
    writeFile(temporary.path() / L"fixture.prj", "changed");
    auto rejected = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"});
    EXPECT_EQ(rejected.status, FreshLegacyImportCommitStatus::RecoveryBlocked);
    EXPECT_FALSE(std::filesystem::exists(request.sourceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory));

    writeFile(temporary.path() / L"fixture.prj", std::string(123, 'p'));
    std::filesystem::create_directories(request.sourceDirectory);
    rejected = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"});
    EXPECT_EQ(rejected.status, FreshLegacyImportCommitStatus::RecoveryBlocked);
    EXPECT_TRUE(std::filesystem::is_empty(request.sourceDirectory));
    EXPECT_FALSE(std::filesystem::exists(request.workspaceDirectory));
}

TEST(LegacyFreshImportTest, SameStemCanBePlannedForSeparateTargetScopes) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    auto discOne = requestFor(temporary, capsule);
    auto discTwo = discOne;
    discTwo.sourceDirectory = temporary.path() / L"disc-two-source";
    discTwo.workspaceDirectory = temporary.path() / L"disc-two-workspace";
    discTwo.targetScope = LegacyImportTargetScope::DreamcastDisc2;
    const auto first = LegacyFreshImportPlanner::plan(discOne);
    const auto second = LegacyFreshImportPlanner::plan(discTwo);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_TRUE(first.value().ready());
    EXPECT_TRUE(second.value().ready());
    EXPECT_NE(first.value().planId, second.value().planId);
    EXPECT_EQ(first.value().scripts.front().outputStem,
        second.value().scripts.front().outputStem);
}

class FixtureMetadataAdapter final : public LegacyMetadataPromotionAdapter {
public:
    [[nodiscard]] LegacyMetadataKind kind() const noexcept override {
        return LegacyMetadataKind::ProjectVariableAliases;
    }
    [[nodiscard]] LegacyMetadataPromotionAssessment assess(
        const LegacyMetadataPlanRecord&,
        const LocalSalsaWorkspace&,
        const std::filesystem::path& capsuleRoot) const override {
        EXPECT_TRUE(std::filesystem::is_directory(capsuleRoot));
        return {true, false, "Fixture adapter accepts aliases."};
    }
    [[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> prepare(
        const LegacyMetadataPlanRecord&,
        const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        EXPECT_TRUE(std::filesystem::is_regular_file(capsuleRoot / L"capsule.json"));
        const std::string text = "fixture aliases\n";
        const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
        return Result<std::vector<WorkspaceArtifactMutation>>::success({{
            workspace.descriptor().components.authoring / L"fixture-aliases.txt",
            false, std::nullopt, std::vector<std::byte>{bytes.begin(), bytes.end()}}});
    }
};

TEST(LegacyFreshImportTest, TypedMetadataPromotionIsAtomicAndIdempotent) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(temporary.path(), {{"A001A", "A001A"}});
    const auto request = requestFor(temporary, capsule);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared);
    const auto committed = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"});
    ASSERT_TRUE(committed.succeeded());
    auto workspace = LocalSalsaWorkspace::openOrCreate(
        request.workspaceDirectory, *committed.dataset);
    ASSERT_TRUE(workspace);
    LegacyMetadataPromotionRegistry registry;
    ASSERT_TRUE(registry.add(std::make_shared<FixtureMetadataAdapter>()));
    auto preview = LegacyMetadataPromotionService::preview(
        workspace.value(), prepared.value().plan.capsuleId, registry);
    ASSERT_TRUE(preview);
    const auto item = std::ranges::find_if(preview.value().items, [](const auto& candidate) {
        return candidate.record.kind == LegacyMetadataKind::ProjectVariableAliases;
    });
    ASSERT_NE(item, preview.value().items.end());
    ASSERT_TRUE(item->assessment.eligible);
    const std::array selected{item->record.recordId};
    const auto promoted = LegacyMetadataPromotionService::promote(
        workspace.value(), preview.value(), selected, registry);
    ASSERT_TRUE(promoted.applied) << (promoted.diagnostics.empty()
        ? "no diagnostic" : promoted.diagnostics.front().message);
    EXPECT_TRUE(std::filesystem::is_regular_file(request.workspaceDirectory
        / L"authoring" / L"fixture-aliases.txt"));
    auto after = LegacyMetadataPromotionService::preview(
        workspace.value(), prepared.value().plan.capsuleId, registry);
    ASSERT_TRUE(after);
    const auto applied = std::ranges::find_if(after.value().items, [&](const auto& candidate) {
        return candidate.record.recordId == selected.front();
    });
    ASSERT_NE(applied, after.value().items.end());
    EXPECT_EQ(applied->record.disposition, LegacyMetadataDisposition::Applied);
    EXPECT_FALSE(applied->assessment.eligible);
}

TEST(LegacyFreshImportTest, BuiltInPromotionAppliesV7AliasesColorsAndSectionFolders) {
    FreshImportTemporaryDirectory temporary;
    const auto capsule = makeFreshImportCapsule(
        temporary.path(), {{"A001A", "A001A"}}, false, true);
    const auto request = requestFor(temporary, capsule);
    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared) << prepared.diagnostics().front().message;
    const auto committed = LegacyFreshImportCommitService::commit({prepared.value(),
        temporary.path() / L"fixture.prj", temporary.path() / L"workspace-stage",
        temporary.path() / L"recovery"});
    ASSERT_TRUE(committed.succeeded()) << (committed.diagnostics.empty()
        ? "no diagnostic" : committed.diagnostics.front().message);
    auto workspace = LocalSalsaWorkspace::openOrCreate(
        request.workspaceDirectory, *committed.dataset);
    ASSERT_TRUE(workspace);
    LegacyMetadataPromotionRegistry registry;
    ASSERT_TRUE(registerBuiltInLegacyMetadataPromotionAdapters(registry));
    auto preview = LegacyMetadataPromotionService::preview(
        workspace.value(), prepared.value().plan.capsuleId, registry);
    ASSERT_TRUE(preview) << preview.diagnostics().front().message;
    std::vector<std::string> aliasRecords;
    for (const auto& item : preview.value().items)
        if (item.assessment.eligible && !item.assessment.conflict
            && (item.record.kind == LegacyMetadataKind::ProjectVariableAliases
                || item.record.kind == LegacyMetadataKind::ScriptVariableAliases))
            aliasRecords.push_back(item.record.recordId);
    ASSERT_EQ(aliasRecords.size(), 2u);
    const auto promotedAliases = LegacyMetadataPromotionService::promote(
        workspace.value(), preview.value(), aliasRecords, registry);
    ASSERT_TRUE(promotedAliases.applied) << (promotedAliases.diagnostics.empty()
        ? "no diagnostic" : promotedAliases.diagnostics.front().message);

    auto workspaceMetadata = SctWorkspaceAuthoringStore(
        request.workspaceDirectory / L"authoring" / L"workspace.json").load();
    ASSERT_TRUE(workspaceMetadata);
    ASSERT_EQ(workspaceMetadata.value().projectAliases.size(), 1u);
    EXPECT_EQ(workspaceMetadata.value().projectAliases.front().alias, "ProjectFlag");
    EXPECT_TRUE(workspaceMetadata.value().opcodeColors.empty());

    auto locator = AssetLocator::fromRelativePath(L"A001A.sct");
    ASSERT_TRUE(locator);
    auto project = LocalGameProject::inspect({request.sourceDirectory,
        committed.dataset->identity.platform, committed.dataset->identity.region});
    ASSERT_TRUE(project);
    auto loaded = SctPatchCheckpointService::load(
        project.value(), &workspace.value(), &workspace.value(), locator.value());
    ASSERT_TRUE(loaded.load.succeeded());
    ASSERT_FALSE(loaded.patchConflict);
    ASSERT_EQ(loaded.aliases.size(), 1u);
    EXPECT_EQ(loaded.aliases.front().alias, "LocalCounter");
    EXPECT_TRUE(loaded.folders.empty());

    preview = LegacyMetadataPromotionService::preview(
        workspace.value(), prepared.value().plan.capsuleId, registry);
    ASSERT_TRUE(preview) << preview.diagnostics().front().message;
    std::vector<std::string> presentationRecords;
    for (const auto& item : preview.value().items)
        if (item.assessment.eligible && !item.assessment.conflict
            && (item.record.kind == LegacyMetadataKind::OpcodeColors
                || item.record.kind == LegacyMetadataKind::SectionGroups))
            presentationRecords.push_back(item.record.recordId);
    ASSERT_EQ(presentationRecords.size(), 2u);
    const auto promotedPresentation = LegacyMetadataPromotionService::promote(
        workspace.value(), preview.value(), presentationRecords, registry);
    ASSERT_TRUE(promotedPresentation.applied) << (
        promotedPresentation.diagnostics.empty() ? "no diagnostic"
            : promotedPresentation.diagnostics.front().message);

    workspaceMetadata = SctWorkspaceAuthoringStore(
        request.workspaceDirectory / L"authoring" / L"workspace.json").load();
    ASSERT_TRUE(workspaceMetadata);
    ASSERT_EQ(workspaceMetadata.value().opcodeColors.size(), 1u);
    EXPECT_EQ(workspaceMetadata.value().opcodeColors.front(),
        (SctOpcodeColor{12, 0x123456u}));
    loaded = SctPatchCheckpointService::load(
        project.value(), &workspace.value(), &workspace.value(), locator.value());
    ASSERT_TRUE(loaded.load.succeeded());
    ASSERT_FALSE(loaded.patchConflict);
    ASSERT_EQ(loaded.folders.size(), 1u);
    EXPECT_EQ(loaded.folders.front().name, "main");
    ASSERT_EQ(loaded.folders.front().sections.size(), 1u);
}

TEST(LegacyFreshImportTest, ValidatesOptionalPrivateOrganicCapsuleWithoutWriting) {
    std::wstring value(32'768, L'\0');
    const auto length = GetEnvironmentVariableW(L"SALSA_LEGACY_CAPSULE_FIXTURE",
        value.data(), static_cast<DWORD>(value.size()));
    if (length == 0) GTEST_SKIP() << "No private capsule fixture was requested.";
    ASSERT_LT(length, value.size());
    value.resize(length);
    FreshImportTemporaryDirectory temporary;
    const auto summary = LegacyCapsuleReader::validate(value);
    ASSERT_TRUE(summary) << summary.diagnostics().front().message;
    auto request = requestFor(temporary, value);
    const auto sourceFilename = std::filesystem::path(
        summary.value().source.filename).filename().wstring();
    request.region = sourceFilename.starts_with(L"EU_")
        ? LegacyImportRegion::Europe : LegacyImportRegion::NorthAmerica;
    request.publication.textEncoding = request.region == LegacyImportRegion::Europe
        ? spice::sct::kSctWindows1252Byte7FEncoding
        : spice::sct::kSctShiftJisByte7FEncoding;
    const auto planned = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(planned) << planned.diagnostics().front().message;
    EXPECT_FALSE(planned.value().scripts.empty());
    EXPECT_EQ(planned.value().scripts.size(),
        summary.value().scripts.size());
    std::map<std::string, std::size_t, std::less<>> firstReasons;
    for (const auto& script : planned.value().scripts)
        if (script.status == FreshLegacyScriptPlanStatus::Blocked && !script.reasons.empty())
            ++firstReasons[script.reasons.front()];
    std::ostringstream reasonSummary;
    for (const auto& [reason, count] : firstReasons)
        reasonSummary << count << " x " << reason << '\n';
    for (const auto& script : planned.value().scripts) {
        if (script.status != FreshLegacyScriptPlanStatus::Blocked) continue;
        reasonSummary << "BLOCKED " << script.legacyKey << ":\n";
        for (const auto& reason : script.reasons)
            reasonSummary << "  " << reason << '\n';
    }
    std::cout << "Organic capsule review: " << summary.value().source.filename << ", "
        << std::ranges::count_if(planned.value().scripts, [](const auto& script) {
            return script.status == FreshLegacyScriptPlanStatus::Ready;
        }) << " ready, "
        << std::ranges::count_if(planned.value().scripts, [](const auto& script) {
            return script.status == FreshLegacyScriptPlanStatus::Blocked;
        }) << " blocked.\n" << reasonSummary.str();
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

TEST(LegacyFreshImportTest, ImportsOptionalPrivateOrganicProjectEndToEnd) {
    std::wstring value(32'768, L'\0');
    const auto length = GetEnvironmentVariableW(L"SALSA_LEGACY_PROJECT_FIXTURE",
        value.data(), static_cast<DWORD>(value.size()));
    if (length == 0) GTEST_SKIP() << "No private legacy project fixture was requested.";
    ASSERT_LT(length, value.size());
    value.resize(length);
    const std::filesystem::path fixture(value);
    const auto before = digestFile(fixture);
    ASSERT_TRUE(before);

    FreshImportTemporaryDirectory temporary;
    core::LegacyConversionRequest conversionRequest;
    conversionRequest.source = fixture;
    conversionRequest.destination = temporary.path() / L"organic-capsule";
    conversionRequest.converterExecutable = executableDirectory()
        / L"SalsaLegacyConverter.exe";
    conversionRequest.receiptDirectory = temporary.path() / L"receipts";
    conversionRequest.trustedInputConfirmed = true;
    conversionRequest.scriptWorkers = 0;
    const auto converted = LegacyConversionService::convert(conversionRequest);
    ASSERT_TRUE(converted.finalized()) << (converted.diagnostics.empty()
        ? "no diagnostic" : converted.diagnostics.front().message);
    ASSERT_TRUE(converted.capsule.has_value());

    auto request = requestFor(temporary, conversionRequest.destination);
    const auto filename = fixture.filename().wstring();
    request.targetScope = filename.find(L"D2") != std::wstring::npos
        ? LegacyImportTargetScope::DreamcastDisc2
        : LegacyImportTargetScope::DreamcastDisc1;
    request.region = filename.starts_with(L"EU_")
        ? LegacyImportRegion::Europe : LegacyImportRegion::NorthAmerica;
    request.publication.textEncoding = request.region == LegacyImportRegion::Europe
        ? spice::sct::kSctWindows1252Byte7FEncoding
        : spice::sct::kSctShiftJisByte7FEncoding;
    const auto firstPlan = LegacyFreshImportPlanner::plan(request);
    ASSERT_TRUE(firstPlan) << firstPlan.diagnostics().front().message;
    std::map<std::string, std::size_t, std::less<>> blockerReasons;
    std::size_t readyScripts = 0;
    for (const auto& script : firstPlan.value().scripts)
        if (script.status == FreshLegacyScriptPlanStatus::Blocked) {
            request.scriptDecisions.push_back({script.ordinal, false});
            ++blockerReasons[script.reasons.empty()
                ? "No blocker reason was recorded." : script.reasons.front()];
        } else if (script.status == FreshLegacyScriptPlanStatus::Ready) {
            ++readyScripts;
        }
    std::cout << "Organic import review: " << fixture.filename().string() << ", "
        << readyScripts << " ready, " << request.scriptDecisions.size() << " blocked.\n";
    for (const auto& [reason, count] : blockerReasons)
        std::cout << "  " << count << " x " << reason << '\n';
    for (const auto& script : firstPlan.value().scripts) {
        if (script.status != FreshLegacyScriptPlanStatus::Blocked) continue;
        std::cout << "  BLOCKED " << script.legacyKey << ":\n";
        for (const auto& reason : script.reasons)
            std::cout << "    " << reason << '\n';
    }
    ASSERT_TRUE(std::ranges::any_of(firstPlan.value().scripts, [](const auto& script) {
        return script.status == FreshLegacyScriptPlanStatus::Ready;
    }));

    auto prepared = LegacyFreshImportPreparer::prepare(
        request, temporary.path() / L"source-stage");
    ASSERT_TRUE(prepared) << prepared.diagnostics().front().message;
    ASSERT_TRUE(prepared.value().plan.ready());
    const auto committed = LegacyFreshImportCommitService::commit({prepared.value(), fixture,
        temporary.path() / L"workspace-stage", temporary.path() / L"recovery"});
    ASSERT_TRUE(committed.succeeded()) << (committed.diagnostics.empty()
        ? "no diagnostic" : committed.diagnostics.front().message);
    ASSERT_TRUE(committed.dataset.has_value());
    const auto reopened = LocalGameProject::inspect({request.sourceDirectory,
        GamePlatform::Dreamcast, request.region == LegacyImportRegion::Europe
            ? std::optional{GameRegion::Europe}
            : std::optional{GameRegion::NorthAmerica}});
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened.value().snapshot().assets.size(), std::ranges::count_if(
        prepared.value().plan.scripts, [](const auto& script) {
            return script.status == FreshLegacyScriptPlanStatus::Ready;
        }));
    EXPECT_TRUE(std::filesystem::is_regular_file(request.workspaceDirectory
        / L"imports" / prepared.value().plan.capsuleId / L"capsule" / L"capsule.json"));
    const auto after = digestFile(fixture);
    ASSERT_TRUE(after);
    EXPECT_EQ(before.value(), after.value());
}

}  // namespace
}  // namespace salsa::core
