#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Legacy/LegacyCapsule.h"
#include "SalsaCore/Legacy/LegacyConversionService.h"
#include "../../src/SalsaLegacyConverter/LegacyConverter.h"

#include <Windows.h>
#include <gtest/gtest.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace salsa::core {
namespace {

class LegacyTemporaryDirectory final {
public:
    LegacyTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-legacy-capsule-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~LegacyTemporaryDirectory() {
        std::error_code ignored; std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_{};
};

[[nodiscard]] std::string digest(const std::string& text) {
    return sha256(std::as_bytes(std::span{text.data(), text.size()})).value().toHex();
}

[[nodiscard]] std::string digest(const std::vector<std::uint8_t>& bytes) {
    return sha256(std::as_bytes(std::span{bytes.data(), bytes.size()})).value().toHex();
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output << text;
}

void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::vector<std::uint8_t> fromHex(const std::string_view text) {
    const auto digit = [](const char value) -> std::uint8_t {
        return value >= '0' && value <= '9' ? static_cast<std::uint8_t>(value - '0')
            : static_cast<std::uint8_t>(value - 'a' + 10);
    };
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < text.size(); index += 2u)
        bytes.push_back(static_cast<std::uint8_t>(digit(text[index]) << 4u)
            | digit(text[index + 1u]));
    return bytes;
}

[[nodiscard]] std::filesystem::path executableDirectory() {
    std::wstring path(32'768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

[[nodiscard]] std::filesystem::path makeCapsule(const std::filesystem::path& parent,
    const bool withScript = false) {
    const auto root = parent / L"fixture.salsa-legacy";
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
    write(root / L"project.cbor", project);
    const std::string sourceHash(64, '0');
    const auto projectHash = digest(project);
    std::string identity(LegacyConverterContractId); identity.push_back('\0');
    identity += sourceHash; identity.push_back('\0'); identity += "project.cbor";
    identity.push_back('\0'); identity += projectHash; identity.push_back('\0');
    identity += std::to_string(project.size()); identity.push_back('\n');
    nlohmann::ordered_json scripts = nlohmann::ordered_json::array();
    nlohmann::ordered_json entries = nlohmann::ordered_json::array({{{"path", "project.cbor"},
        {"size", project.size()}, {"sha256", projectHash}, {"canonical", true}}});
    if (withScript) {
        const auto nullValue = nlohmann::ordered_json::array({1});
        nlohmann::ordered_json sidecar{
            {"folded_sects", nullValue}, {"index", nullValue}, {"sect_tree", nullValue},
            {"sect_list", nullValue}, {"string_garbage", nullValue},
            {"unused_sections", nullValue}, {"errors", nullValue},
            {"error_sections", nullValue}, {"variables", nullValue}};
        nlohmann::ordered_json record{
            {"formatId", "jahorta.salsa.legacy-script-record"}, {"schemaVersion", 2},
            {"ordinal", 0}, {"key", "A001A"}, {"storedName", "A001A"},
            {"status", "accepted"},
            {"counts", {{"sections", 0}, {"instructions", 0}, {"parameters", 0},
                {"links", 0}, {"strings", 0}}},
            {"diagnostics", nlohmann::ordered_json::array()},
            {"ir", {{"header", nullValue}, {"footer", nullValue},
                {"string_groups", nullValue}, {"strings", nullValue},
                {"sections", nullValue}, {"links", nullValue},
                {"sidecar", std::move(sidecar)}}}};
        const auto raw = nlohmann::ordered_json::to_cbor(record);
        std::vector<std::uint8_t> compressed;
        if (lodepng::compress(compressed, raw) != 0)
            throw std::runtime_error("test script compression failed");
        constexpr auto path = "scripts/000000.cbor.zlib";
        write(root / L"scripts" / L"000000.cbor.zlib", compressed);
        const auto scriptHash = digest(compressed);
        identity += path; identity.push_back('\0'); identity += scriptHash;
        identity.push_back('\0'); identity += std::to_string(compressed.size());
        identity.push_back('\n');
        scripts.push_back({{"ordinal", 0}, {"key", "A001A"}, {"storedName", "A001A"},
            {"status", "accepted"}, {"sections", 0}, {"instructions", 0},
            {"parameters", 0}, {"strings", 0}, {"diagnostics", 0}, {"path", path}});
        entries.push_back({{"path", path}, {"size", compressed.size()},
            {"sha256", scriptHash}, {"canonical", true}});
    }
    const auto capsuleId = digest(identity);
    nlohmann::ordered_json manifest{
        {"formatId", LegacyCapsuleFormatId}, {"schemaVersion", LegacyCapsuleSchemaVersion},
        {"converterContractId", LegacyConverterContractId}, {"capsuleId", capsuleId},
        {"status", "ready"},
        {"source", {{"filename", "fixture.prj"}, {"size", 123}, {"sha256", sourceHash},
            {"pickleProtocol", 4}, {"projectVersion", 7}, {"originalRetained", false}}},
        {"normalizationCount", 0}, {"scripts", std::move(scripts)},
        {"entries", std::move(entries)}};
    auto encoded = manifest.dump(2); encoded.push_back('\n'); write(root / L"capsule.json", encoded);
    return root;
}

TEST(LegacyCapsuleTest, ValidatesTypedCompressedScriptRecord) {
    LegacyTemporaryDirectory temporary;
    auto validated = LegacyCapsuleReader::validate(makeCapsule(temporary.path(), true));
    ASSERT_TRUE(validated) << validated.diagnostics().front().message;
    ASSERT_EQ(validated.value().scripts.size(), 1u);
    EXPECT_EQ(validated.value().scripts.front().key, "A001A");
}

TEST(LegacyCapsuleTest, ValidatesDeterministicShardedManifest) {
    LegacyTemporaryDirectory temporary;
    auto validated = LegacyCapsuleReader::validate(makeCapsule(temporary.path()));
    ASSERT_TRUE(validated) << validated.diagnostics().front().message;
    EXPECT_EQ(validated.value().status, LegacyCapsuleStatus::Ready);
    EXPECT_EQ(validated.value().source.projectVersion, 7u);
    EXPECT_TRUE(validated.value().scripts.empty());
}

TEST(LegacyCapsuleTest, RejectsTamperedCanonicalEntry) {
    LegacyTemporaryDirectory temporary;
    const auto capsule = makeCapsule(temporary.path());
    write(capsule / L"project.cbor", "tampered\n");
    const auto validated = LegacyCapsuleReader::validate(capsule);
    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.diagnostics().front().code,
        DiagnosticCode::LegacyCapsuleIntegrityFailed);
}

TEST(LegacyCapsuleTest, RejectsTamperedCompressedScriptEntry) {
    LegacyTemporaryDirectory temporary;
    const auto capsule = makeCapsule(temporary.path(), true);
    const auto scriptPath = capsule / L"scripts" / L"000000.cbor.zlib";
    std::ifstream input(scriptPath, std::ios::binary);
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
    ASSERT_FALSE(bytes.empty());
    bytes.back() ^= 0xffu;
    write(scriptPath, bytes);
    const auto validated = LegacyCapsuleReader::validate(capsule);
    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.diagnostics().front().code,
        DiagnosticCode::LegacyCapsuleIntegrityFailed);
}

TEST(LegacyCapsuleTest, RejectsManifestStatusThatContradictsRecords) {
    LegacyTemporaryDirectory temporary;
    const auto capsule = makeCapsule(temporary.path());
    const auto manifestPath = capsule / L"capsule.json";
    std::ifstream input(manifestPath, std::ios::binary);
    auto manifest = nlohmann::ordered_json::parse(input);
    input.close();
    manifest["status"] = "action-required";
    auto encoded = manifest.dump(2); encoded.push_back('\n');
    write(manifestPath, encoded);
    const auto validated = LegacyCapsuleReader::validate(capsule);
    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.diagnostics().front().code,
        DiagnosticCode::LegacyCapsuleIntegrityFailed);
}

TEST(LegacyCapsuleTest, ValidatesOptionalPrivateOrganicCapsule) {
    std::wstring value(32'768, L'\0');
    const auto length = GetEnvironmentVariableW(L"SALSA_LEGACY_CAPSULE_FIXTURE",
        value.data(), static_cast<DWORD>(value.size()));
    if (length == 0) GTEST_SKIP() << "No private capsule fixture was requested.";
    ASSERT_LT(length, value.size()); value.resize(length);
    const auto validated = LegacyCapsuleReader::validate(value);
    ASSERT_TRUE(validated) << validated.diagnostics().front().message;
    EXPECT_EQ(validated.value().status, LegacyCapsuleStatus::Ready);
    EXPECT_FALSE(validated.value().scripts.empty());
    EXPECT_TRUE(std::ranges::all_of(validated.value().scripts, [](const auto& script) {
        return script.status == LegacyScriptStatus::Accepted;
    }));
}

TEST(LegacyConversionServiceTest, RequiresFreshTrustedInputConfirmation) {
    LegacyTemporaryDirectory temporary;
    LegacyConversionRequest request{};
    request.source = temporary.path() / L"input.prj";
    request.destination = temporary.path() / L"output.salsa-legacy";
    const auto result = LegacyConversionService::convert(request);
    EXPECT_EQ(result.status, LegacyConversionStatus::Rejected)
        << result.diagnostics.front().message;
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code,
        DiagnosticCode::LegacyConversionNotTrusted);
    EXPECT_FALSE(std::filesystem::exists(request.destination));
}

TEST(LegacyConversionServiceTest, CalculatesAdaptiveConverterMemoryLimit) {
    constexpr std::uint64_t gibibyte = 1ull << 30;
    EXPECT_EQ(legacyConverterMemoryLimit(0), 8ull * gibibyte);
    EXPECT_EQ(legacyConverterMemoryLimit(2ull * gibibyte), 4ull * gibibyte);
    EXPECT_EQ(legacyConverterMemoryLimit(8ull * gibibyte), 4ull * gibibyte);
    EXPECT_EQ(legacyConverterMemoryLimit(24ull * gibibyte), 12ull * gibibyte);
    EXPECT_EQ(legacyConverterMemoryLimit(96ull * gibibyte), 16ull * gibibyte);
}

TEST(LegacyConversionServiceTest, ResolvesAutoAndExplicitScriptWorkers) {
    using salsa::legacy::resolveScriptWorkerCount;
    EXPECT_EQ(resolveScriptWorkerCount(0, 16, 20), 4u);
    EXPECT_EQ(resolveScriptWorkerCount(0, 2, 20), 2u);
    EXPECT_EQ(resolveScriptWorkerCount(0, 0, 20), 1u);
    EXPECT_EQ(resolveScriptWorkerCount(0, 16, 0), 0u);
    EXPECT_EQ(resolveScriptWorkerCount(4, 1, 2), 2u);
    EXPECT_EQ(resolveScriptWorkerCount(3, 16, 20), 3u);
}

TEST(LegacyConversionServiceTest, RejectsMalformedInputThroughIsolatedBroker) {
    LegacyTemporaryDirectory temporary;
    const auto converter = executableDirectory() / L"SalsaLegacyConverter.exe";
    if (!std::filesystem::is_regular_file(converter))
        GTEST_SKIP() << "The separately built converter executable is unavailable.";
    const auto source = temporary.path() / L"malformed.prj";
    write(source, std::string{"\x80\x04N.", 4});
    LegacyConversionRequest request{};
    request.source = source;
    request.destination = temporary.path() / L"output.salsa-legacy";
    request.converterExecutable = converter;
    request.trustedInputConfirmed = true;
    const auto result = LegacyConversionService::convert(request);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.status, LegacyConversionStatus::Rejected)
        << result.diagnostics.front().message;
    EXPECT_EQ(result.diagnostics.front().code,
        DiagnosticCode::LegacyConversionInvalidRequest)
        << result.diagnostics.front().message;
    EXPECT_FALSE(std::filesystem::exists(request.destination));
}

TEST(LegacyConversionServiceTest, PreCancelledConversionRemovesAllStagingOutput) {
    LegacyTemporaryDirectory temporary;
    const auto converter = executableDirectory() / L"SalsaLegacyConverter.exe";
    if (!std::filesystem::is_regular_file(converter))
        GTEST_SKIP() << "The separately built converter executable is unavailable.";
    const auto source = temporary.path() / L"input.prj";
    write(source, "malformed");
    LegacyConversionRequest request{};
    request.source = source;
    request.destination = temporary.path() / L"output.salsa-legacy";
    request.converterExecutable = converter;
    request.trustedInputConfirmed = true;
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto result = LegacyConversionService::convert(
        request, cancellation.get_token());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.status, LegacyConversionStatus::Cancelled);
    EXPECT_EQ(result.diagnostics.front().code,
        DiagnosticCode::LegacyConversionCancelled);
    EXPECT_FALSE(std::filesystem::exists(request.destination));
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path()))
        EXPECT_FALSE(entry.path().filename().wstring().starts_with(
            request.destination.filename().wstring() + L".staging-"));
}

TEST(LegacyConversionServiceTest, ConvertsV7ThroughIsolatedBrokerAndInstallsAtomically) {
    LegacyTemporaryDirectory temporary;
    const auto converter = executableDirectory() / L"SalsaLegacyConverter.exe";
    if (!std::filesystem::is_regular_file(converter))
        GTEST_SKIP() << "The separately built converter executable is unavailable.";
    constexpr std::string_view pickleHex =
        "800495cf000000000000008c1f53414c53412e50726f6a6563742e70726f6a6563745f"
        "636f6e7461696e6572948c0a53435450726f6a6563749493942981947d94288c047363"
        "7473947d948c0966696c655f6e616d65948c0c556e7469746c65642e70726a948c0866"
        "696c6570617468944e8c10676c6f62616c5f7661726961626c6573947d94288c064269"
        "74566172947d948c06496e74566172947d948c0742797465566172947d948c08466c6f"
        "6174566172947d94758c0776657273696f6e944b078c0e696e73745f69645f636f6c6f"
        "7273947d9475622e";
    const auto source = temporary.path() / L"official-v7.prj";
    write(source, fromHex(pickleHex));
    LegacyConversionRequest request{};
    request.source = source;
    request.destination = temporary.path() / L"output.salsa-legacy";
    request.converterExecutable = converter;
    request.receiptDirectory = temporary.path() / L"receipts";
    request.trustedInputConfirmed = true;
    request.scriptWorkers = 1;
    const auto result = LegacyConversionService::convert(request);
    ASSERT_TRUE(result.finalized())
        << (result.diagnostics.empty() ? "no diagnostic" : result.diagnostics.front().message);
    ASSERT_TRUE(result.capsule.has_value());
    EXPECT_EQ(result.status, LegacyConversionStatus::Ready);
    EXPECT_EQ(result.capsule->source.projectVersion, 7u);
    EXPECT_TRUE(result.capsule->scripts.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(request.destination / L"capsule.json"));
    const auto receiptPath = *request.receiptDirectory
        / (result.capsule->capsuleId + ".json");
    std::ifstream receiptInput(receiptPath, std::ios::binary);
    ASSERT_TRUE(receiptInput);
    const auto receipt = nlohmann::ordered_json::parse(receiptInput);
    EXPECT_EQ(receipt["helperResult"]["scriptWorkers"]["requested"], 1);
    EXPECT_EQ(receipt["helperResult"]["scriptWorkers"]["used"], 0);
    EXPECT_TRUE(receipt["helperResult"]["timingsMs"].contains("normalizeScripts"));
    EXPECT_TRUE(receipt["helperResult"]["timingsMs"].contains("analyzeScripts"));
    EXPECT_TRUE(receipt["helperResult"]["timingsMs"].contains("encodeScripts"));
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path()))
        EXPECT_FALSE(entry.path().filename().wstring().starts_with(
            request.destination.filename().wstring() + L".staging-"));
}

}  // namespace
}  // namespace salsa::core
