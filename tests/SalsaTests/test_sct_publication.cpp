#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctPublication.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctParser.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace {
using namespace salsa::core;
using namespace spice::sct;

class PublicationTemporaryDirectory final {
public:
    PublicationTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-publication-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_ / L"scripts");
        std::filesystem::create_directories(path_ / L"output");
    }
    ~PublicationTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

SctDocument makeDocument() {
    SctDocumentBuilder builder;
    SctScriptSectionContent script;
    for (const std::uint16_t opcode : {9u, 12u}) {
        const auto draft = SctInstructionFactory::createDraft({opcode});
        EXPECT_TRUE(draft.draft.has_value());
        const auto instruction = SctInstructionFactory::materialize(
            builder.document(), *draft.draft);
        EXPECT_TRUE(instruction.instruction.has_value());
        script.instructions.push_back(*instruction.instruction);
    }
    builder.document().sections.push_back(
        {builder.allocateSectionId(), "SCRIPT", std::move(script)});
    return std::move(builder).finish();
}

std::vector<std::byte> sourceBytes() {
    const SctDocumentExportOptions options{SctPlatform::GameCube,
        kSctShiftJisByte7FEncoding, SctDocumentOutputByteOrder::BigEndian,
        SctDocumentOutputWrapper::Raw};
    const auto exported = SctDocumentExporter::exportDocument(
        makeDocument(), options);
    EXPECT_TRUE(exported.success);
    std::vector<std::byte> result(exported.bytes.size());
    std::memcpy(result.data(), exported.bytes.data(), exported.bytes.size());
    return result;
}

void writeBytes(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> raw{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::vector<std::byte> result(raw.size());
    std::memcpy(result.data(), raw.data(), raw.size());
    return result;
}

struct PublicationFixture final {
    PublicationTemporaryDirectory temporary;
    AssetLocator locator = AssetLocator::fromRelativePath("scripts/test.sct").value();
    std::filesystem::path source = temporary.path() / locator.path();
    LocalGameProject project = [&] {
        const auto bytes = sourceBytes();
        writeBytes(source, bytes);
        return LocalGameProject::inspect({temporary.path()}).value();
    }();
    std::shared_ptr<const SctDocumentSnapshot> snapshot =
        SctDocumentLoader::load(project, locator).document;
    SctEditSession session{snapshot};

    SctPublicationRequest request(const std::filesystem::path& destination) {
        return {
            locator,
            project.dataset().identity.fingerprint,
            snapshot->provenance->source().descriptor.revision,
            *session.capturePublicationRevision(1u),
            {SctPlatform::GameCube, kSctShiftJisByte7FEncoding,
                SctDocumentOutputByteOrder::BigEndian,
                SctDocumentOutputWrapper::Raw},
            destination,
            false,
        };
    }
};

TEST(SctPublicationTest, ExportsVerifiedRevisionAndReturnsReceipt) {
    PublicationFixture fixture;
    const auto destination = fixture.temporary.path() / L"output" / L"published.sct";
    const auto request = fixture.request(destination);

    const auto result = SctPublicationService::publish(fixture.project, request);

    ASSERT_TRUE(result.succeeded());
    ASSERT_TRUE(result.receipt.has_value());
    EXPECT_EQ(result.receipt->revision, fixture.session.workingRevision());
    EXPECT_EQ(result.receipt->destination, destination);
    const auto bytes = readBytes(destination);
    EXPECT_EQ(result.receipt->outputDigest, sha256(bytes).value());
    EXPECT_FALSE(result.receipt->replacedSource);
    EXPECT_FALSE(result.receipt->layout.instructions.empty());
    ASSERT_TRUE(result.receipt->preservation.header.has_value());
    const auto parsed = SctParser{}.parse(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    EXPECT_TRUE(parsed.parseOk);
}

TEST(SctPublicationTest, CapturesPendingRevisionIndependentlyOfLaterEdits) {
    PublicationFixture fixture;
    ASSERT_TRUE(fixture.session.renameSection(
        fixture.snapshot->document->sections.front().id, "RENAMED").committed);
    const auto capturedRevision = fixture.session.workingRevision();
    auto request = fixture.request(
        fixture.temporary.path() / L"output" / L"captured.sct");
    ASSERT_TRUE(request.capturedRevision.materialization.has_value());
    ASSERT_TRUE(fixture.session.undo().has_value());

    const auto result = SctPublicationService::publish(fixture.project, request);

    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.receipt->revision, capturedRevision);
    EXPECT_NE(result.receipt->revision, fixture.session.workingRevision());
    EXPECT_EQ(result.receipt->layout.instructions.size(), 2u);
    const auto bytes = readBytes(result.receipt->destination);
    const auto parsed = SctParser{}.parse(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    ASSERT_TRUE(parsed.parseOk);
    ASSERT_EQ(parsed.file.sections.size(), 1u);
    EXPECT_EQ(parsed.file.sections.front().id.name, "RENAMED");
}

TEST(SctPublicationTest, TargetFailurePreservesExistingDestination) {
    PublicationFixture fixture;
    const auto destination = fixture.temporary.path() / L"output" / L"existing.sct";
    const std::vector<std::byte> original{std::byte{0x11}, std::byte{0x22}};
    writeBytes(destination, original);
    auto request = fixture.request(destination);
    auto invalid = *fixture.snapshot->document;
    invalid.sections.front().nameBytes = std::string(64u, 'X');
    auto analysis = SctDocumentAnalysis::build(invalid,
        fixture.snapshot->provenance->importEvidence
            ? &*fixture.snapshot->provenance->importEvidence : nullptr);
    request.capturedRevision.verifiedSnapshot = std::make_shared<SctDocumentSnapshot>(
        SctDocumentSnapshot{fixture.snapshot->provenance,
            std::make_shared<const SctDocument>(std::move(invalid)),
            std::make_shared<const SctDocumentAnalysis>(std::move(analysis)),
            SctDocumentReadiness::StructurallyValid, {}});
    request.capturedRevision.materialization.reset();

    const auto result = SctPublicationService::publish(fixture.project, request);

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(readBytes(destination), original);
    EXPECT_FALSE(result.diagnostics.empty());
}

TEST(SctPublicationTest, SourceReplacementRequiresConfirmationAndCurrentSource) {
    PublicationFixture fixture;
    auto request = fixture.request(fixture.source);

    auto rejected = SctPublicationService::publish(fixture.project, request);
    EXPECT_FALSE(rejected.succeeded());
    ASSERT_FALSE(rejected.infrastructureDiagnostics.empty());
    EXPECT_EQ(rejected.infrastructureDiagnostics.front().code,
        DiagnosticCode::PublicationSourceReplacementNotConfirmed);

    request.allowSourceReplacement = true;
    const auto replaced = SctPublicationService::publish(fixture.project, request);
    ASSERT_TRUE(replaced.succeeded());
    EXPECT_TRUE(replaced.receipt->replacedSource);

    writeBytes(fixture.source, std::span<const std::byte>{
        std::array{std::byte{0x55}, std::byte{0x66}}});
    const auto stale = SctPublicationService::publish(fixture.project, request);
    EXPECT_FALSE(stale.succeeded());
    ASSERT_FALSE(stale.infrastructureDiagnostics.empty());
    EXPECT_EQ(stale.infrastructureDiagnostics.front().code,
        DiagnosticCode::PublicationSourceChanged);
}

TEST(SctPublicationTest, CancellationDoesNotCreateDestination) {
    PublicationFixture fixture;
    const auto destination = fixture.temporary.path() / L"output" / L"cancelled.sct";
    std::stop_source stop;
    stop.request_stop();

    const auto result = SctPublicationService::publish(
        fixture.project, fixture.request(destination), stop.get_token());

    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST(SctPublicationTest, ExportsLittleEndianAklzForDreamcastTarget) {
    PublicationFixture fixture;
    const auto destination = fixture.temporary.path() / L"output" / L"dreamcast.sct";
    auto request = fixture.request(destination);
    request.options = {SctPlatform::Dreamcast, kSctWindows1252Byte7FEncoding,
        SctDocumentOutputByteOrder::LittleEndian,
        SctDocumentOutputWrapper::Aklz};

    const auto result = SctPublicationService::publish(fixture.project, request);

    ASSERT_TRUE(result.succeeded());
    const auto bytes = readBytes(destination);
    const auto parsed = SctParser{}.parse(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    EXPECT_TRUE(parsed.parseOk);
    EXPECT_TRUE(parsed.file.originalCompressedAklz);
    EXPECT_EQ(parsed.file.detectedEndian, "little");
}

TEST(SctPublicationTest, ChangedSourceWarnsButAllowsExternalExport) {
    PublicationFixture fixture;
    const auto destination = fixture.temporary.path() / L"output" / L"stale-source.sct";
    auto request = fixture.request(destination);
    writeBytes(fixture.source, std::span<const std::byte>{
        std::array{std::byte{0x11}, std::byte{0x22}}});

    const auto result = SctPublicationService::publish(fixture.project, request);

    ASSERT_TRUE(result.succeeded());
    ASSERT_FALSE(result.infrastructureDiagnostics.empty());
    EXPECT_EQ(result.infrastructureDiagnostics.front().severity,
        DiagnosticSeverity::Warning);
    EXPECT_EQ(result.infrastructureDiagnostics.front().code,
        DiagnosticCode::PublicationSourceChanged);
    EXPECT_TRUE(std::filesystem::exists(destination));
}

TEST(SctPublicationTest, DefaultsUseEvidenceWithoutGuessingPlatform) {
    PublicationFixture fixture;
    const auto defaults = SctPublicationService::defaultsFor(
        fixture.project.dataset(), *fixture.snapshot);

    EXPECT_FALSE(defaults.platform.has_value());
    ASSERT_TRUE(defaults.byteOrder.has_value());
    EXPECT_EQ(*defaults.byteOrder, SctDocumentOutputByteOrder::BigEndian);
    ASSERT_TRUE(defaults.wrapper.has_value());
    EXPECT_EQ(*defaults.wrapper, SctDocumentOutputWrapper::Raw);
}

}  // namespace
