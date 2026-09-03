#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/SctScriptPatch.h"
#include "SalsaCore/Sct/SctEditSession.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <span>
#include <string>
#include <cstring>

namespace salsa::core {
namespace {

using namespace spice::sct;

class PatchTemporaryDirectory final {
public:
    PatchTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-sct-patch-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~PatchTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::span<const std::byte> patchBytes(const std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

[[nodiscard]] Sha256Digest patchDigest(const std::string_view text) {
    return sha256(patchBytes(text)).value();
}

[[nodiscard]] AssetLocator patchLocator(const std::string_view value) {
    return AssetLocator::fromRelativePath(std::filesystem::path(value)).value();
}

[[nodiscard]] DatasetContext patchDataset(const std::filesystem::path& root,
    const std::string_view identity = "workspace-dataset") {
    return {root, DatasetIdentity{std::nullopt, std::nullopt,
        DatasetFingerprint{patchDigest(identity)}}};
}

[[nodiscard]] SctDocument makeBaselineDocument() {
    SctDocument document;
    const auto scriptSection = document.allocateSectionId();
    const auto label = document.allocateInstructionId();
    const auto terminalReturn = document.allocateInstructionId();
    const auto textSection = document.allocateSectionId();
    const auto string = document.allocateStringId();
    const auto footer = document.allocateFooterEntryId();

    document.sections.push_back({scriptSection, "M00001",
        SctScriptSectionContent{{
            SctDocumentInstruction{label, 9u},
            SctDocumentInstruction{terminalReturn, 12u},
        }}});
    document.sections.push_back({textSection, "M00001_E",
        SctStringSectionContent{{string, SctOpaqueText{{
            'o', 'p', 'a', 'q', 'u', 'e', '-', 's', 'o', 'u', 'r', 'c', 'e'}},
            SctTextKind::PlainString}}});
    document.footerEntries.push_back({footer, SctTextKind::PlainString,
        SctPlainText{"footer-original"}});
    return document;
}

[[nodiscard]] SctSemanticState semanticState(const SctDocument& document,
    const std::span<const SctAuthoredArm> arms = {},
    const std::span<const SctPatchedTextRepair> repairs = {}) {
    return {std::make_shared<const SctDocument>(document),
        {arms.begin(), arms.end()}, {repairs.begin(), repairs.end()}};
}

[[nodiscard]] const SctScriptSectionContent& scriptAt(
    const SctDocument& document, const std::size_t index) {
    return std::get<SctScriptSectionContent>(document.sections.at(index).content);
}

[[nodiscard]] SctDocument makeExportableDocument() {
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

[[nodiscard]] std::vector<std::byte> exportDocumentBytes(const SctDocument& document) {
    const SctDocumentExportOptions options{SctPlatform::GameCube,
        kSctShiftJisByte7FEncoding, SctDocumentOutputByteOrder::BigEndian,
        SctDocumentOutputWrapper::Raw, SctOpaquePreservationPolicy::RequirePreservation};
    const auto exported = SctDocumentExporter::exportDocument(document, options);
    EXPECT_TRUE(exported.success);
    std::vector<std::byte> result(exported.bytes.size());
    std::memcpy(result.data(), exported.bytes.data(), exported.bytes.size());
    return result;
}

class PatchFakeCatalog final : public AssetCatalog {
public:
    PatchFakeCatalog(AssetLocator locator, std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)), descriptor_{std::move(locator),
              static_cast<std::uint64_t>(bytes_.size()),
              SourceRevision{sha256(bytes_).value()}},
          snapshot_{{descriptor_}, DatasetFingerprint{patchDigest("catalog")}} {}

    [[nodiscard]] const AssetCatalogSnapshot& snapshot() const noexcept override {
        return snapshot_;
    }

    [[nodiscard]] Result<SourceAssetSnapshot> loadAsset(
        const AssetLocator& locator) const override {
        if (locator != descriptor_.locator) {
            return Result<SourceAssetSnapshot>::failure({DiagnosticSeverity::Error,
                DiagnosticCode::AssetNotFound, "not found", locator.path()});
        }
        return Result<SourceAssetSnapshot>::success({descriptor_, bytes_});
    }

private:
    std::vector<std::byte> bytes_;
    AssetDescriptor descriptor_;
    AssetCatalogSnapshot snapshot_;
};

class PatchFakeProject final : public GameProjectContext {
public:
    PatchFakeProject(AssetLocator locator, std::vector<std::byte> bytes,
        const std::string_view datasetIdentity)
        : dataset_{{}, {std::nullopt, std::nullopt,
              DatasetFingerprint{patchDigest(datasetIdentity)}}},
          catalog_(std::move(locator), std::move(bytes)) {}

    [[nodiscard]] const DatasetContext& dataset() const noexcept override {
        return dataset_;
    }
    [[nodiscard]] const AssetCatalog& assets() const noexcept override {
        return catalog_;
    }

private:
    DatasetContext dataset_;
    PatchFakeCatalog catalog_;
};

class MemoryPatchStore final : public SctPatchStore, public SctBaselineStore {
public:
    [[nodiscard]] Result<std::optional<PatchEnvelope>> load(
        const AssetLocator&) const override {
        if (loadFailure_) {
            return Result<std::optional<PatchEnvelope>>::failure({
                DiagnosticSeverity::Error, DiagnosticCode::PersistenceReadFailed,
                "stored patch is corrupt", std::nullopt});
        }
        return Result<std::optional<PatchEnvelope>>::success(envelope_);
    }

    [[nodiscard]] Result<void> checkpoint(
        const AssetLocator&, const PatchEnvelope& envelope) const override {
        envelope_ = envelope;
        return Result<void>::success();
    }

    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> loadBaseline(
        const SourceRevision& revision) const override {
        const auto found = baselines_.find(revision.digest.toHex());
        if (found == baselines_.end())
            return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
        return Result<std::optional<std::vector<std::byte>>>::success(found->second);
    }

    [[nodiscard]] Result<void> retainBaseline(const SourceRevision& revision,
        const std::span<const std::byte> bytes) const override {
        if (baselineRetainFailure_) {
            return Result<void>::failure({DiagnosticSeverity::Error,
                DiagnosticCode::PersistenceWriteFailed,
                "retained baseline could not be written", std::nullopt});
        }
        baselines_[revision.digest.toHex()] = {bytes.begin(), bytes.end()};
        return Result<void>::success();
    }

    void failLoads() noexcept { loadFailure_ = true; }
    void failBaselineRetains() noexcept { baselineRetainFailure_ = true; }
    [[nodiscard]] const std::optional<PatchEnvelope>& envelope() const noexcept {
        return envelope_;
    }

private:
    mutable std::optional<PatchEnvelope> envelope_;
    mutable std::unordered_map<std::string, std::vector<std::byte>> baselines_;
    bool loadFailure_ = false;
    bool baselineRetainFailure_ = false;
};

TEST(SalsaScriptPatchTest, SquashesAndReappliesSemanticDocumentChanges) {
    const auto baseline = makeBaselineDocument();
    auto working = baseline;
    auto& script = std::get<SctScriptSectionContent>(working.sections[0].content);
    const auto inserted = working.allocateInstructionId();
    SctDocumentInstruction sleep{inserted, 16u};
    sleep.skipRefresh = true;
    sleep.fixedParameters.push_back({0u, SctEncodedWordValue{15u}});
    script.instructions.insert(script.instructions.begin() + 1, sleep);
    working.sections[0].nameBytes = "M00001_EDIT";
    auto& indexed = std::get<SctStringSectionContent>(working.sections[1].content);
    indexed.string.value = SctMessage{std::optional<std::string>{"Speaker"},
        SctFormattedText{{SctTextChunk{"Hello"},
            SctInlineCommand{SctMessageCommandCode::E, SctNoCommandArgument{}}}}};
    std::get<SctPlainText>(working.footerEntries[0].value).utf8 = "footer-edited";
    const auto secondFooter = working.allocateFooterEntryId();
    working.footerEntries.push_back({secondFooter, SctTextKind::PlainString,
        SctPlainText{"second"}});

    const SctAuthoredArm arm{
        SctAuthoredArmId{4},
        {working.sections[0].id, script.instructions.front().id},
        spice::sct::SctStructuredArmKind::Else,
        std::nullopt,
        script.instructions.back().id,
        {inserted},
        {},
        SctAuthoredArmRealization::Physical,
    };
    const SctPatchedTextRepair repair{
        indexed.string.id,
        {kSctWindows1252Byte7FEncoding,
            SctKnownTextConvention::Windows1252Byte7F,
            patchDigest("opaque-source").toHex()},
    };

    const std::array arms{arm};
    const std::array repairs{repair};
    const auto baselineState = semanticState(baseline);
    const auto workingState = semanticState(working, arms, repairs);
    const auto patch = SalsaScriptPatchService::diff(baselineState, workingState,
        SctKnownTextConvention::Windows1252Byte7F);
    ASSERT_TRUE(patch);
    EXPECT_FALSE(patch.value().empty());
    ASSERT_EQ(patch.value().scriptSections.size(), 1u);
    EXPECT_EQ(patch.value().scriptSections.front().section, working.sections[0].id);
    ASSERT_EQ(patch.value().textValues.size(), 1u);
    ASSERT_EQ(patch.value().footerEntries.size(), 2u);

    const auto encoded = SalsaScriptPatchCodec::serialize(patch.value());
    ASSERT_TRUE(encoded);
    const auto encodedAgain = SalsaScriptPatchCodec::serialize(patch.value());
    ASSERT_TRUE(encodedAgain);
    EXPECT_EQ(encoded.value(), encodedAgain.value());
    ASSERT_FALSE(encoded.value().empty());
    EXPECT_EQ(encoded.value().back(), std::byte{'\n'});

    const auto decoded = SalsaScriptPatchCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    const auto applied = SalsaScriptPatchService::apply(baselineState, decoded.value());
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied.value().document->sections.size(), working.sections.size());
    EXPECT_EQ(applied.value().document->sections[0].nameBytes, "M00001_EDIT");
    ASSERT_EQ(scriptAt(*applied.value().document, 0).instructions.size(), 3u);
    const auto& appliedSleep = scriptAt(*applied.value().document, 0).instructions[1];
    EXPECT_EQ(appliedSleep.id, inserted);
    EXPECT_EQ(appliedSleep.opcode, 16u);
    EXPECT_TRUE(appliedSleep.skipRefresh);
    ASSERT_EQ(appliedSleep.fixedParameters.size(), 1u);
    EXPECT_EQ(std::get<SctEncodedWordValue>(
        appliedSleep.fixedParameters.front().value).value, 15u);
    const auto& appliedIndexed = std::get<SctStringSectionContent>(
        applied.value().document->sections[1].content);
    ASSERT_TRUE(std::holds_alternative<SctMessage>(appliedIndexed.string.value));
    EXPECT_EQ(std::get<SctMessage>(appliedIndexed.string.value).headerUtf8,
        std::optional<std::string>{"Speaker"});
    ASSERT_EQ(applied.value().document->footerEntries.size(), 2u);
    EXPECT_EQ(std::get<SctPlainText>(
        applied.value().document->footerEntries[0].value).utf8, "footer-edited");
    EXPECT_EQ(applied.value().authoredArms, std::vector<SctAuthoredArm>{arm});
    ASSERT_EQ(applied.value().textRepairs.size(), 1u);
    EXPECT_EQ(applied.value().textRepairs.front().provenance, repair.provenance);

    const auto net = SalsaScriptPatchService::diff(baselineState,
        applied.value(), decoded.value().sourceTextConvention);
    ASSERT_TRUE(net);
    const auto reencoded = SalsaScriptPatchCodec::serialize(net.value());
    ASSERT_TRUE(reencoded);
    EXPECT_EQ(reencoded.value(), encoded.value());
}

TEST(SalsaScriptPatchTest, EmptyDocumentDeltaRemainsCanonical) {
    const auto baseline = makeBaselineDocument();
    const auto state = semanticState(baseline);
    const auto patch = SalsaScriptPatchService::diff(state, state, std::nullopt);
    ASSERT_TRUE(patch);
    EXPECT_TRUE(patch.value().empty());
    const auto encoded = SalsaScriptPatchCodec::serialize(patch.value());
    ASSERT_TRUE(encoded);
    const auto decoded = SalsaScriptPatchCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded.value().empty());
}

TEST(SalsaScriptPatchTest, PreservesAllocatorHighWaterAfterTransientEdits) {
    const auto baseline = makeBaselineDocument();
    auto working = baseline;
    const auto discardedSection = working.allocateSectionId();
    const auto discardedInstruction = working.allocateInstructionId();
    const auto discardedString = working.allocateStringId();
    const auto discardedFooter = working.allocateFooterEntryId();
    const auto discardedOpaque = working.allocateOpaqueAttachmentId();
    (void)discardedSection;
    (void)discardedInstruction;
    (void)discardedString;
    (void)discardedFooter;
    (void)discardedOpaque;

    const auto baselineState = semanticState(baseline);
    const auto patch = SalsaScriptPatchService::diff(
        baselineState, semanticState(working), std::nullopt);
    ASSERT_TRUE(patch);
    ASSERT_TRUE(patch.value().allocatorState.has_value());
    EXPECT_FALSE(patch.value().empty());
    const auto applied = SalsaScriptPatchService::apply(baselineState, patch.value());
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().document->nextSectionIdValue(),
        working.nextSectionIdValue());
    EXPECT_EQ(applied.value().document->nextInstructionIdValue(),
        working.nextInstructionIdValue());
    EXPECT_EQ(applied.value().document->nextStringIdValue(),
        working.nextStringIdValue());
    EXPECT_EQ(applied.value().document->nextFooterEntryIdValue(),
        working.nextFooterEntryIdValue());
    EXPECT_EQ(applied.value().document->nextOpaqueAttachmentIdValue(),
        working.nextOpaqueAttachmentIdValue());
}

TEST(SalsaScriptPatchTest, RoundTripsV3ScptProgramsAndStringGroupMarkers) {
    SalsaScriptPatch patch;
    SctDocumentInstruction instruction{SctInstructionId{1u}, 125u};
    instruction.scheduledExpression = SctCanonicalExpression{SctTypedScptProgram{{
        SctScptValueOperation{SctScptValueKind::DecimalLiteral, 0x08000180u, {}},
        SctScptValueOperation{SctScptValueKind::BitVariable, 0x20000005u, {}},
        SctScptBinaryOperation{SctScptBinaryOperationKind::Comparison, 0x04u},
        SctScptStackOverwritePreviousWithTopOperation{},
        SctScptInertOperation{},
    }}, SctExpressionTermination::StopCode};
    patch.sections.push_back({std::nullopt, SctDocumentSection{SctSectionId{1u}, "GROUP",
        SctStringGroupMarkerSectionContent{{9u, 0x1du}}}});
    patch.sections.push_back({std::nullopt, SctDocumentSection{SctSectionId{2u}, "SCRIPT",
        SctScriptSectionContent{{instruction}}}});

    const auto encoded = SalsaScriptPatchCodec::serialize(patch);
    ASSERT_TRUE(encoded);
    const std::string json(reinterpret_cast<const char*>(encoded.value().data()),
        encoded.value().size());
    EXPECT_NE(json.find("\"schemaVersion\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"stringGroupMarker\""), std::string::npos);
    EXPECT_NE(json.find("\"stackOverwrite\""), std::string::npos);
    EXPECT_NE(json.find("\"inert\""), std::string::npos);

    const auto decoded = SalsaScriptPatchCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.value().sections.size(), 2u);
    const auto& marker = std::get<SctStringGroupMarkerSectionContent>(
        decoded.value().sections[0].after->content);
    EXPECT_EQ(marker.preambleWords, (std::vector<std::uint32_t>{9u, 0x1du}));
    const auto& restoredInstruction = std::get<SctScriptSectionContent>(
        decoded.value().sections[1].after->content).instructions.front();
    ASSERT_TRUE(restoredInstruction.scheduledExpression.has_value());
    const auto& program = std::get<SctTypedScptProgram>(
        restoredInstruction.scheduledExpression->body);
    ASSERT_EQ(program.operations.size(), 5u);
    EXPECT_TRUE(std::holds_alternative<SctScptValueOperation>(program.operations[0]));
    EXPECT_TRUE(std::holds_alternative<SctScptBinaryOperation>(program.operations[2]));
    EXPECT_TRUE(std::holds_alternative<SctScptStackOverwritePreviousWithTopOperation>(
        program.operations[3]));
    EXPECT_TRUE(std::holds_alternative<SctScptInertOperation>(program.operations[4]));
}

TEST(SalsaScriptPatchTest, RejectsUnknownFieldsVersionsAndInvalidTargets) {
    const auto encoded = SalsaScriptPatchCodec::serialize(SalsaScriptPatch{});
    ASSERT_TRUE(encoded);
    std::string text(reinterpret_cast<const char*>(encoded.value().data()),
        encoded.value().size());
    const auto finalBrace = text.rfind('}');
    ASSERT_NE(finalBrace, std::string::npos);
    text.insert(finalBrace, ",\n  \"unknown\": true\n");
    EXPECT_FALSE(SalsaScriptPatchCodec::deserialize(patchBytes(text)));

    std::string wrongVersion(reinterpret_cast<const char*>(encoded.value().data()),
        encoded.value().size());
    const auto version = wrongVersion.find("\"schemaVersion\": 3");
    ASSERT_NE(version, std::string::npos);
    wrongVersion.replace(version, std::string("\"schemaVersion\": 3").size(),
        "\"schemaVersion\": 2");
    const auto unsupported = SalsaScriptPatchCodec::deserialize(patchBytes(wrongVersion));
    ASSERT_FALSE(unsupported);
    ASSERT_FALSE(unsupported.diagnostics().empty());
    EXPECT_EQ(unsupported.diagnostics().front().code,
        DiagnosticCode::UnsupportedSctPatchSchema);

    SalsaScriptPatch conventionPatch;
    conventionPatch.sourceTextConvention = SctKnownTextConvention::ShiftJisByte7F;
    const auto conventionBytes = SalsaScriptPatchCodec::serialize(conventionPatch);
    ASSERT_TRUE(conventionBytes);
    std::string badConvention(reinterpret_cast<const char*>(conventionBytes.value().data()),
        conventionBytes.value().size());
    const auto convention = badConvention.find("\"sourceTextConvention\": 1");
    ASSERT_NE(convention, std::string::npos);
    badConvention.replace(convention,
        std::string("\"sourceTextConvention\": 1").size(),
        "\"sourceTextConvention\": 99");
    EXPECT_FALSE(SalsaScriptPatchCodec::deserialize(patchBytes(badConvention)));

    SalsaScriptPatch missingTarget;
    missingTarget.textValues.push_back({SctStringId{99},
        SctPlainText{"expected"}, SctPlainText{"missing"}});
    const auto applied = SalsaScriptPatchService::apply(
        semanticState(makeBaselineDocument()), missingTarget);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.diagnostics().front().code, DiagnosticCode::SctPatchApplyFailed);
}

TEST(LocalSalsaWorkspaceTest, CreatesReopensAndSeparatesPatchFilesFromDataset) {
    PatchTemporaryDirectory temporary;
    const auto dataset = temporary.path() / L"dataset";
    const auto workspace = temporary.path() / L"workspace";
    std::filesystem::create_directory(dataset);

    auto created = LocalSalsaWorkspace::openOrCreate(workspace, patchDataset(dataset));
    ASSERT_TRUE(created);
    EXPECT_TRUE(std::filesystem::exists(workspace / L"project.json"));
    EXPECT_TRUE(std::filesystem::is_directory(workspace / L"patches"));
    EXPECT_FALSE(std::filesystem::exists(dataset / L"project.json"));

    const auto locator = patchLocator("scripts/M00001.SCT");
    const auto path = created.value().patchPath(locator);
    EXPECT_EQ(path.parent_path(), workspace / L"patches");
    EXPECT_EQ(path.extension(), L".json");

    PatchEnvelope envelope{
        DatasetFingerprint{patchDigest("dataset")},
        {{locator, SourceRevision{patchDigest("asset")}}}, {},
        {std::string(SalsaScriptPatchCodec::PayloadType),
            SalsaScriptPatchCodec::SchemaVersion, {std::byte{0x01}}}};
    ASSERT_TRUE(created.value().checkpoint(locator, envelope));
    ASSERT_TRUE(std::filesystem::exists(path));
    const auto loaded = created.value().load(locator);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(loaded.value()->payload.bytes, envelope.payload.bytes);

    const auto reopened = LocalSalsaWorkspace::openOrCreate(workspace, patchDataset(dataset));
    EXPECT_TRUE(reopened);
    const auto otherDataset = temporary.path() / L"other-dataset";
    std::filesystem::create_directory(otherDataset);
    EXPECT_FALSE(LocalSalsaWorkspace::openOrCreate(
        workspace, patchDataset(otherDataset, "other-dataset")));
}

TEST(LocalSalsaWorkspaceTest, RejectsAConflictingNonemptyDirectory) {
    PatchTemporaryDirectory temporary;
    const auto dataset = temporary.path() / L"dataset";
    const auto workspace = temporary.path() / L"not-empty";
    std::filesystem::create_directory(dataset);
    std::filesystem::create_directory(workspace);
    std::ofstream(workspace / L"unrelated.txt") << "keep";

    const auto result = LocalSalsaWorkspace::openOrCreate(
        workspace, patchDataset(dataset));
    ASSERT_FALSE(result);
    EXPECT_TRUE(std::filesystem::exists(workspace / L"unrelated.txt"));
    EXPECT_FALSE(std::filesystem::exists(workspace / L"project.json"));
}

TEST(SctPatchCheckpointServiceTest, SavesAndRestoresACleanDocumentCheckpoint) {
    const auto locator = patchLocator("scripts/checkpoint.sct");
    const auto source = exportDocumentBytes(makeExportableDocument());
    PatchFakeProject project(locator, source, "dataset-a");
    auto loaded = SctPatchCheckpointService::load(project, nullptr, nullptr, locator);
    ASSERT_TRUE(loaded.load.succeeded());
    ASSERT_NE(loaded.baseline, nullptr);

    SctEditSession session(loaded.load.document);
    const auto& sourceScript = std::get<SctScriptSectionContent>(
        loaded.load.document->document->sections.front().content);
    const auto inserted = session.insertInstructionAfter(
        sourceScript.instructions.front().id, 125u);
    ASSERT_TRUE(inserted.committed);
    ASSERT_TRUE(session.isDirty());
    auto request = session.checkpointRequest(7u);
    ASSERT_TRUE(request.has_value());

    MemoryPatchStore store;
    const auto saved = SctPatchCheckpointService::checkpoint(*request, store, store);
    ASSERT_TRUE(saved.saved);
    ASSERT_TRUE(store.envelope().has_value());
    EXPECT_EQ(store.envelope()->affectedAssets.front().locator, locator);
    EXPECT_EQ(store.envelope()->sourceDatasetFingerprint,
        project.dataset().identity.fingerprint);
    EXPECT_TRUE(session.markPatchCheckpoint(
        saved.revision, saved.historyStateToken));
    EXPECT_FALSE(session.isDirty());

    auto restored = SctPatchCheckpointService::load(project, &store, &store, locator);
    ASSERT_TRUE(restored.load.succeeded());
    EXPECT_TRUE(restored.patchApplied);
    EXPECT_FALSE(restored.patchConflict);
    ASSERT_EQ(scriptAt(*restored.load.document->document, 0).instructions.size(), 3u);
    SctEditSession restoredSession(restored.baseline, restored.load.document,
        restored.authoredArms, restored.textRepairs);
    EXPECT_FALSE(restoredSession.isDirty());
}

TEST(SctPatchCheckpointServiceTest, WarnsForDatasetDriftButBlocksAssetOrPayloadConflict) {
    const auto locator = patchLocator("scripts/checkpoint.sct");
    const auto source = exportDocumentBytes(makeExportableDocument());
    PatchFakeProject firstProject(locator, source, "dataset-a");
    auto loaded = SctPatchCheckpointService::load(firstProject, nullptr, nullptr, locator);
    ASSERT_TRUE(loaded.load.succeeded());
    SctEditSession session(loaded.load.document);
    const auto& script = std::get<SctScriptSectionContent>(
        loaded.load.document->document->sections.front().content);
    ASSERT_TRUE(session.insertInstructionAfter(script.instructions.front().id, 125u).committed);
    MemoryPatchStore store;
    ASSERT_TRUE(SctPatchCheckpointService::checkpoint(
        *session.checkpointRequest(1u), store, store).saved);

    PatchFakeProject movedDataset(locator, source, "dataset-b");
    const auto moved = SctPatchCheckpointService::load(
        movedDataset, &store, &store, locator);
    EXPECT_TRUE(moved.patchApplied);
    EXPECT_FALSE(moved.patchConflict);
    EXPECT_TRUE(std::ranges::any_of(moved.load.infrastructureDiagnostics,
        [](const auto& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Warning
                && diagnostic.code == DiagnosticCode::SctPatchSourceMismatch;
        }));

    auto wrongAssetEnvelope = *store.envelope();
    wrongAssetEnvelope.affectedAssets.front().expectedRevision =
        SourceRevision{patchDigest("different-source")};
    MemoryPatchStore wrongAssetStore;
    ASSERT_TRUE(wrongAssetStore.checkpoint(locator, wrongAssetEnvelope));
    const auto wrongAsset = SctPatchCheckpointService::load(
        firstProject, &wrongAssetStore, &wrongAssetStore, locator);
    EXPECT_FALSE(wrongAsset.patchApplied);
    EXPECT_TRUE(wrongAsset.patchConflict);
    EXPECT_TRUE(wrongAsset.load.succeeded());

    MemoryPatchStore missingBaselineStore;
    ASSERT_TRUE(missingBaselineStore.checkpoint(locator, *store.envelope()));
    const auto missingBaseline = SctPatchCheckpointService::load(
        firstProject, &missingBaselineStore, &missingBaselineStore, locator);
    EXPECT_FALSE(missingBaseline.patchApplied);
    EXPECT_TRUE(missingBaseline.patchConflict);
    EXPECT_TRUE(std::ranges::any_of(missingBaseline.load.infrastructureDiagnostics,
        [](const auto& diagnostic) {
            return diagnostic.code == DiagnosticCode::SctBaselineMissing;
        }));

    MemoryPatchStore corruptStore;
    corruptStore.failLoads();
    const auto corrupt = SctPatchCheckpointService::load(
        firstProject, &corruptStore, &corruptStore, locator);
    EXPECT_FALSE(corrupt.patchApplied);
    EXPECT_TRUE(corrupt.patchConflict);
    EXPECT_TRUE(corrupt.load.succeeded());
}

TEST(SctPatchCheckpointServiceTest, RetainsBaselineBeforePublishingPatch) {
    const auto locator = patchLocator("scripts/checkpoint.sct");
    PatchFakeProject project(locator,
        exportDocumentBytes(makeExportableDocument()), "dataset-a");
    const auto loaded = SctPatchCheckpointService::load(
        project, nullptr, nullptr, locator);
    ASSERT_TRUE(loaded.load.succeeded());
    SctEditSession session(loaded.load.document);
    const auto& script = std::get<SctScriptSectionContent>(
        loaded.load.document->document->sections.front().content);
    ASSERT_TRUE(session.insertInstructionAfter(
        script.instructions.front().id, 125u).committed);

    MemoryPatchStore store;
    store.failBaselineRetains();
    const auto saved = SctPatchCheckpointService::checkpoint(
        *session.checkpointRequest(1u), store, store);
    EXPECT_FALSE(saved.saved);
    EXPECT_FALSE(store.envelope().has_value());
    ASSERT_FALSE(saved.diagnostics.empty());
    EXPECT_EQ(saved.diagnostics.front().code,
        DiagnosticCode::PersistenceWriteFailed);
}

TEST(SctPatchCheckpointServiceTest, CapturedCheckpointSurvivesConcurrentBranchReplacement) {
    const auto locator = patchLocator("scripts/checkpoint.sct");
    PatchFakeProject project(locator,
        exportDocumentBytes(makeExportableDocument()), "dataset-a");
    const auto loaded = SctPatchCheckpointService::load(
        project, nullptr, nullptr, locator);
    ASSERT_TRUE(loaded.load.succeeded());
    SctEditSession session(loaded.load.document);
    const auto& script = std::get<SctScriptSectionContent>(
        loaded.load.document->document->sections.front().content);
    ASSERT_TRUE(session.insertInstructionAfter(script.instructions.front().id, 125u).committed);
    const auto captured = session.checkpointRequest(1u);
    ASSERT_TRUE(captured.has_value());
    ASSERT_TRUE(session.undo().has_value());
    ASSERT_TRUE(session.insertInstructionAfter(script.instructions.front().id, 125u).committed);
    ASSERT_NE(session.currentRevision(), captured->revision);

    MemoryPatchStore store;
    const auto saved = SctPatchCheckpointService::checkpoint(
        *captured, store, store);
    ASSERT_TRUE(saved.saved);
    EXPECT_TRUE(session.markPatchCheckpoint(
        saved.revision, saved.historyStateToken));
    EXPECT_TRUE(session.isDirty());
}

}  // namespace
}  // namespace salsa::core
