#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Legacy/LegacyCapsule.h"
#include "SalsaCore/Legacy/LegacyConversionService.h"

#include <Windows.h>
#include <gtest/gtest.h>
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

[[nodiscard]] std::filesystem::path makeCapsule(const std::filesystem::path& parent) {
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
    const auto capsuleId = digest(identity);
    nlohmann::ordered_json manifest{
        {"formatId", LegacyCapsuleFormatId}, {"schemaVersion", LegacyCapsuleSchemaVersion},
        {"converterContractId", LegacyConverterContractId}, {"capsuleId", capsuleId},
        {"status", "ready"},
        {"source", {{"filename", "fixture.prj"}, {"size", 123}, {"sha256", sourceHash},
            {"pickleProtocol", 4}, {"projectVersion", 7}, {"originalRetained", false}}},
        {"normalizationCount", 0}, {"scripts", nlohmann::ordered_json::array()},
        {"entries", nlohmann::ordered_json::array({{{"path", "project.cbor"},
            {"size", project.size()}, {"sha256", projectHash}, {"canonical", true}}})}};
    auto encoded = manifest.dump(2); encoded.push_back('\n'); write(root / L"capsule.json", encoded);
    return root;
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
    request.trustedInputConfirmed = true;
    const auto result = LegacyConversionService::convert(request);
    ASSERT_TRUE(result.finalized())
        << (result.diagnostics.empty() ? "no diagnostic" : result.diagnostics.front().message);
    ASSERT_TRUE(result.capsule.has_value());
    EXPECT_EQ(result.status, LegacyConversionStatus::Ready);
    EXPECT_EQ(result.capsule->source.projectVersion, 7u);
    EXPECT_TRUE(result.capsule->scripts.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(request.destination / L"capsule.json"));
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path()))
        EXPECT_FALSE(entry.path().filename().wstring().starts_with(
            request.destination.filename().wstring() + L".staging-"));
}

}  // namespace
}  // namespace salsa::core
