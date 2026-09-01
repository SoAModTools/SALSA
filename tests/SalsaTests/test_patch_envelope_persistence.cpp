#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"
#include "SalsaCore/Persistence/PatchEnvelopeFileStore.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace salsa::core {
namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
            (L"salsa-persistence-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored{};
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] Sha256Digest digestOf(const std::string_view text) {
    return sha256(bytes(text)).value();
}

[[nodiscard]] AssetLocator locator(const std::filesystem::path& path) {
    return AssetLocator::fromRelativePath(path).value();
}

[[nodiscard]] PatchEnvelope makeEnvelope(
    std::vector<std::byte> payloadBytes = {
        std::byte{ 0x00 },
        std::byte{ 0x01 },
        std::byte{ 0x02 },
        std::byte{ 0xff },
    }) {
    return {
        DatasetFingerprint{ digestOf("dataset") },
        {
            { locator(L"scripts/Z.sct"), SourceRevision{ digestOf("z-source") } },
            { locator(L"scripts/a.sct"), SourceRevision{ digestOf("a-source") } },
        },
        {
            locator(L"shared/会話.sct"),
            locator(L"shared/B.sct"),
        },
        {
            "jahorta.salsa.test-patch",
            7,
            std::move(payloadBytes),
        },
    };
}

[[nodiscard]] bool hasCode(
    const std::vector<Diagnostic>& diagnostics,
    const DiagnosticCode code) {
    return std::ranges::any_of(diagnostics, [code](const Diagnostic& diagnostic) {
        return diagnostic.code == code;
    });
}

void replaceOnce(
    std::string& value,
    const std::string_view original,
    const std::string_view replacement) {
    const auto position = value.find(original);
    ASSERT_NE(position, std::string::npos);
    value.replace(position, original.size(), replacement);
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void writeFile(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output);
}

[[nodiscard]] std::size_t temporaryFileCount(
    const std::filesystem::path& directory,
    const std::filesystem::path& destination) {
    const auto prefix = destination.filename().wstring() + L".tmp-";
    return static_cast<std::size_t>(std::ranges::count_if(
        std::filesystem::directory_iterator(directory),
        [&prefix](const std::filesystem::directory_entry& entry) {
            return entry.path().filename().wstring().starts_with(prefix);
        }));
}

TEST(PatchEnvelopeCodecTest, SerializesDeterministicallyInCanonicalOrder) {
    auto first = makeEnvelope();
    auto second = first;
    std::ranges::reverse(second.affectedAssets);
    std::ranges::reverse(second.dependencies);

    const auto firstJson = PatchEnvelopeCodec::serialize(first);
    const auto secondJson = PatchEnvelopeCodec::serialize(second);

    ASSERT_TRUE(firstJson);
    ASSERT_TRUE(secondJson);
    EXPECT_EQ(firstJson.value(), secondJson.value());
    EXPECT_TRUE(firstJson.value().ends_with("\n"));
    EXPECT_EQ(firstJson.value().find('\r'), std::string::npos);
    EXPECT_LT(firstJson.value().find("scripts/a.sct"), firstJson.value().find("scripts/Z.sct"));
    EXPECT_LT(firstJson.value().find("shared/B.sct"), firstJson.value().find("shared/会話.sct"));
    EXPECT_EQ(firstJson.value().find("platform"), std::string::npos);
    EXPECT_EQ(firstJson.value().find("region"), std::string::npos);
    EXPECT_EQ(firstJson.value().find("exportTarget"), std::string::npos);
    EXPECT_NE(firstJson.value().find("\"encoding\": \"base64\""), std::string::npos);
    EXPECT_NE(firstJson.value().find("\"data\": \"AAEC/w==\""), std::string::npos);
    EXPECT_LT(firstJson.value().find("\"header\""), firstJson.value().find("\"sourceDatasetFingerprint\""));
    EXPECT_LT(firstJson.value().find("\"sourceDatasetFingerprint\""), firstJson.value().find("\"affectedAssets\""));
    EXPECT_LT(firstJson.value().find("\"affectedAssets\""), firstJson.value().find("\"dependencies\""));
    EXPECT_LT(firstJson.value().find("\"dependencies\""), firstJson.value().find("\"payload\""));
}

TEST(PatchEnvelopeCodecTest, RoundTripsUnicodeLocatorsAndOpaqueBinaryPayload) {
    const auto original = makeEnvelope();
    const auto serialized = PatchEnvelopeCodec::serialize(original);
    ASSERT_TRUE(serialized);

    const auto decoded = PatchEnvelopeCodec::deserialize(serialized.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().sourceDatasetFingerprint, original.sourceDatasetFingerprint);
    ASSERT_EQ(decoded.value().affectedAssets.size(), 2U);
    EXPECT_EQ(decoded.value().affectedAssets[0].locator.identityKey(), "scripts/a.sct");
    EXPECT_EQ(decoded.value().affectedAssets[1].locator.identityKey(), "scripts/z.sct");
    ASSERT_EQ(decoded.value().dependencies.size(), 2U);
    EXPECT_EQ(decoded.value().dependencies[1].path().generic_wstring(), L"shared/会話.sct");
    EXPECT_EQ(decoded.value().payload.type, original.payload.type);
    EXPECT_EQ(decoded.value().payload.schemaVersion, original.payload.schemaVersion);
    EXPECT_EQ(decoded.value().payload.bytes, original.payload.bytes);

    const auto reserialized = PatchEnvelopeCodec::serialize(decoded.value());
    ASSERT_TRUE(reserialized);
    EXPECT_EQ(reserialized.value(), serialized.value());
}

TEST(PatchEnvelopeCodecTest, SupportsAnEmptyOpaquePayload) {
    const auto serialized = PatchEnvelopeCodec::serialize(makeEnvelope({}));
    ASSERT_TRUE(serialized);
    EXPECT_NE(serialized.value().find("\"data\": \"\""), std::string::npos);

    const auto decoded = PatchEnvelopeCodec::deserialize(serialized.value());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded.value().payload.bytes.empty());
}

TEST(PatchEnvelopeCodecTest, RejectsMalformedUnknownAndPlatformSpecificJson) {
    const auto serialized = PatchEnvelopeCodec::serialize(makeEnvelope());
    ASSERT_TRUE(serialized);

    const auto malformed = PatchEnvelopeCodec::deserialize("{");
    ASSERT_FALSE(malformed);
    EXPECT_TRUE(hasCode(malformed.diagnostics(), DiagnosticCode::MalformedPersistenceJson));

    for (const std::string_view field : { "platform", "region", "exportTarget" }) {
        auto withUnknownField = serialized.value();
        const auto closingBrace = withUnknownField.rfind('}');
        ASSERT_NE(closingBrace, std::string::npos);
        withUnknownField.insert(
            closingBrace,
            ",\n  \"" + std::string(field) + "\": \"unsupported\"\n");
        const auto decoded = PatchEnvelopeCodec::deserialize(withUnknownField);
        ASSERT_FALSE(decoded);
        EXPECT_TRUE(hasCode(decoded.diagnostics(), DiagnosticCode::InvalidPatchEnvelope));
    }

    auto missingDependencies = serialized.value();
    replaceOnce(missingDependencies, "  \"dependencies\": [", "  \"missingDependencies\": [");
    const auto missing = PatchEnvelopeCodec::deserialize(missingDependencies);
    ASSERT_FALSE(missing);
    EXPECT_TRUE(hasCode(missing.diagnostics(), DiagnosticCode::InvalidPatchEnvelope));
}

TEST(PatchEnvelopeCodecTest, RejectsInvalidHeaderHashesLocatorsAndTypes) {
    const auto serialized = PatchEnvelopeCodec::serialize(makeEnvelope());
    ASSERT_TRUE(serialized);

    auto wrongOwner = serialized.value();
    replaceOnce(wrongOwner, "\"owner\": \"SALSA\"", "\"owner\": \"OTHER\"");
    EXPECT_TRUE(hasCode(
        PatchEnvelopeCodec::deserialize(wrongOwner).diagnostics(),
        DiagnosticCode::InvalidPatchEnvelope));

    auto wrongFormat = serialized.value();
    replaceOnce(
        wrongFormat,
        "\"format\": \"jahorta.salsa.patch-envelope\"",
        "\"format\": \"jahorta.other.patch-envelope\"");
    EXPECT_TRUE(hasCode(
        PatchEnvelopeCodec::deserialize(wrongFormat).diagnostics(),
        DiagnosticCode::InvalidPatchEnvelope));

    auto unsupportedVersion = serialized.value();
    replaceOnce(unsupportedVersion, "\"schemaVersion\": 1", "\"schemaVersion\": 2");
    EXPECT_TRUE(hasCode(
        PatchEnvelopeCodec::deserialize(unsupportedVersion).diagnostics(),
        DiagnosticCode::UnsupportedPersistenceSchemaVersion));

    auto invalidHash = serialized.value();
    replaceOnce(
        invalidHash,
        "\"sourceDatasetFingerprint\": \"" + digestOf("dataset").toHex() + "\"",
        "\"sourceDatasetFingerprint\": \"bad\"");
    EXPECT_TRUE(hasCode(
        PatchEnvelopeCodec::deserialize(invalidHash).diagnostics(),
        DiagnosticCode::InvalidPatchEnvelope));

    auto invalidLocator = serialized.value();
    replaceOnce(invalidLocator, "scripts/a.sct", "../a.sct");
    EXPECT_TRUE(hasCode(
        PatchEnvelopeCodec::deserialize(invalidLocator).diagnostics(),
        DiagnosticCode::InvalidPatchEnvelope));

    auto wrongFingerprintType = serialized.value();
    replaceOnce(
        wrongFingerprintType,
        "\"sourceDatasetFingerprint\": \"" + digestOf("dataset").toHex() + "\"",
        "\"sourceDatasetFingerprint\": 7");
    EXPECT_TRUE(hasCode(
        PatchEnvelopeCodec::deserialize(wrongFingerprintType).diagnostics(),
        DiagnosticCode::InvalidPatchEnvelope));
}

TEST(PatchEnvelopeCodecTest, RejectsCorruptAndNoncanonicalPayloadData) {
    const auto serialized = PatchEnvelopeCodec::serialize(makeEnvelope());
    ASSERT_TRUE(serialized);

    auto corrupt = serialized.value();
    replaceOnce(corrupt, "\"data\": \"AAEC/w==\"", "\"data\": \"AQEC/w==\"");
    const auto corruptResult = PatchEnvelopeCodec::deserialize(corrupt);
    ASSERT_FALSE(corruptResult);
    EXPECT_TRUE(hasCode(corruptResult.diagnostics(), DiagnosticCode::PatchPayloadCorrupt));

    auto noncanonical = serialized.value();
    replaceOnce(noncanonical, "\"data\": \"AAEC/w==\"", "\"data\": \"AB==\"");
    const auto noncanonicalResult = PatchEnvelopeCodec::deserialize(noncanonical);
    ASSERT_FALSE(noncanonicalResult);
    EXPECT_TRUE(hasCode(noncanonicalResult.diagnostics(), DiagnosticCode::InvalidPatchEnvelope));

    auto malformed = serialized.value();
    replaceOnce(malformed, "\"data\": \"AAEC/w==\"", "\"data\": \"A===\"");
    const auto malformedResult = PatchEnvelopeCodec::deserialize(malformed);
    ASSERT_FALSE(malformedResult);
    EXPECT_TRUE(hasCode(malformedResult.diagnostics(), DiagnosticCode::InvalidPatchEnvelope));
}

TEST(PatchEnvelopeCodecTest, RejectsInvalidAndAmbiguousInMemoryEnvelopes) {
    auto noAssets = makeEnvelope();
    noAssets.affectedAssets.clear();
    EXPECT_FALSE(PatchEnvelopeCodec::serialize(noAssets));

    auto noType = makeEnvelope();
    noType.payload.type.clear();
    EXPECT_FALSE(PatchEnvelopeCodec::serialize(noType));

    auto zeroPayloadVersion = makeEnvelope();
    zeroPayloadVersion.payload.schemaVersion = 0;
    EXPECT_FALSE(PatchEnvelopeCodec::serialize(zeroPayloadVersion));

    auto duplicateAffected = makeEnvelope();
    duplicateAffected.affectedAssets.push_back({
        locator(L"SCRIPTS/A.SCT"),
        SourceRevision{ digestOf("duplicate") },
    });
    EXPECT_FALSE(PatchEnvelopeCodec::serialize(duplicateAffected));

    auto duplicateDependency = makeEnvelope();
    duplicateDependency.dependencies.push_back(locator(L"SHARED/b.SCT"));
    EXPECT_FALSE(PatchEnvelopeCodec::serialize(duplicateDependency));

    auto overlap = makeEnvelope();
    overlap.dependencies.push_back(locator(L"SCRIPTS/A.SCT"));
    EXPECT_FALSE(PatchEnvelopeCodec::serialize(overlap));
}

TEST(PatchEnvelopeFileStoreTest, TreatsMissingFileAsAnEmptySuccessfulLoad) {
    TemporaryDirectory directory{};
    const PatchEnvelopeFileStore store{};

    const auto loaded = store.load(directory.path() / L"missing.json");

    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded.value().has_value());
}

TEST(PatchEnvelopeFileStoreTest, CheckpointsLoadsAndReplacesAnEnvelope) {
    TemporaryDirectory directory{};
    const auto path = directory.path() / L"patch.json";
    const PatchEnvelopeFileStore store{};
    const auto first = makeEnvelope();

    ASSERT_TRUE(store.checkpoint(path, first));
    const auto firstText = readFile(path);
    ASSERT_FALSE(firstText.empty());
    auto loaded = store.load(path);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(loaded.value()->payload.bytes, first.payload.bytes);

    auto replacement = makeEnvelope({ std::byte{ 0x42 } });
    ASSERT_TRUE(store.checkpoint(path, replacement));
    EXPECT_NE(readFile(path), firstText);
    loaded = store.load(path);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(loaded.value()->payload.bytes, replacement.payload.bytes);
    EXPECT_EQ(temporaryFileCount(directory.path(), path), 0U);
}

TEST(PatchEnvelopeFileStoreTest, ReportsCorruptFilesWithTheirPath) {
    TemporaryDirectory directory{};
    const auto path = directory.path() / L"corrupt.json";
    writeFile(path, "{");

    const auto loaded = PatchEnvelopeFileStore{}.load(path);

    ASSERT_FALSE(loaded);
    ASSERT_TRUE(hasCode(loaded.diagnostics(), DiagnosticCode::MalformedPersistenceJson));
    ASSERT_FALSE(loaded.diagnostics().empty());
    EXPECT_EQ(loaded.diagnostics().front().path, path);
}

TEST(PatchEnvelopeFileStoreTest, RejectsBeforeMutationAndRequiresAnExistingParent) {
    TemporaryDirectory directory{};
    const auto path = directory.path() / L"patch.json";
    const PatchEnvelopeFileStore store{};
    ASSERT_TRUE(store.checkpoint(path, makeEnvelope()));
    const auto original = readFile(path);

    auto invalid = makeEnvelope();
    invalid.affectedAssets.clear();
    const auto rejected = store.checkpoint(path, invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(readFile(path), original);
    EXPECT_EQ(temporaryFileCount(directory.path(), path), 0U);

    const auto missingParent = directory.path() / L"missing" / L"patch.json";
    const auto missingResult = store.checkpoint(missingParent, makeEnvelope());
    ASSERT_FALSE(missingResult);
    EXPECT_TRUE(hasCode(missingResult.diagnostics(), DiagnosticCode::PersistenceWriteFailed));
    EXPECT_FALSE(std::filesystem::exists(missingParent.parent_path()));
}

TEST(PatchEnvelopeFileStoreTest, LockedDestinationPreservesThePreviousCheckpoint) {
    TemporaryDirectory directory{};
    const auto path = directory.path() / L"patch.json";
    const PatchEnvelopeFileStore store{};
    const auto originalEnvelope = makeEnvelope();
    ASSERT_TRUE(store.checkpoint(path, originalEnvelope));
    const auto originalText = readFile(path);

    const HANDLE lock = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    const auto result = store.checkpoint(path, makeEnvelope({ std::byte{ 0x42 } }));
    CloseHandle(lock);

    ASSERT_FALSE(result);
    EXPECT_TRUE(hasCode(result.diagnostics(), DiagnosticCode::PersistenceReplaceFailed));
    EXPECT_EQ(readFile(path), originalText);
    EXPECT_EQ(temporaryFileCount(directory.path(), path), 0U);
}

}  // namespace
}  // namespace salsa::core
