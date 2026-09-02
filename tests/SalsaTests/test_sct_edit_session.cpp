#include "SalsaCore/Sct/SctEditSession.h"
#include "SpiceSCT/SctTextBuilder.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <ranges>
#include <vector>

namespace {
using namespace salsa::core;
using namespace spice::sct;

Sha256Digest zeroDigest() {
    return Sha256Digest(std::array<std::byte, Sha256Digest::Size>{});
}

AssetLocator testLocator() {
    auto result = AssetLocator::fromRelativePath("scripts/edit_test.sct");
    EXPECT_TRUE(result);
    return std::move(result).takeValue();
}

SctDocument makeScriptDocument(
    const std::vector<std::uint16_t>& bodyOpcodes = {},
    const bool includeReturn = true) {
    SctDocumentBuilder builder;
    const auto sectionId = builder.allocateSectionId();
    SctScriptSectionContent script;

    auto appendInstruction = [&](const std::uint16_t opcode) {
        SctInstructionFactoryRequest request;
        request.opcode = opcode;
        const auto draft = SctInstructionFactory::createDraft(request);
        EXPECT_TRUE(draft.draft.has_value());
        const auto instruction = SctInstructionFactory::materialize(builder.document(), *draft.draft);
        EXPECT_TRUE(instruction.instruction.has_value());
        script.instructions.push_back(*instruction.instruction);
    };
    appendInstruction(9);
    for (const auto opcode : bodyOpcodes) appendInstruction(opcode);
    if (includeReturn) appendInstruction(12);
    builder.document().sections.push_back({sectionId, "SCRIPT", std::move(script)});
    return std::move(builder).finish();
}

SctInlineCommand noArgumentCommand(const SctMessageCommandCode code) {
    const auto built = SctTextBuilder::noArgumentCommand(code);
    EXPECT_TRUE(built.command.has_value());
    return *built.command;
}

SctDocument makeMessageDocument() {
    auto document = makeScriptDocument();
    const auto stringId = document.allocateStringId();
    document.sections.push_back({document.allocateSectionId(), "MS0000001",
        SctStringSectionContent{SctDocumentString{stringId,
            SctMessage{std::nullopt, SctFormattedText{
                {SctTextChunk{"Indexed"}, noArgumentCommand(SctMessageCommandCode::E)}}},
            SctTextKind::SctString}}});
    document.footerEntries.push_back({document.allocateFooterEntryId(),
        SctTextKind::SctString,
        SctMessage{std::nullopt, SctFormattedText{
            {SctTextChunk{"Footer"}, noArgumentCommand(SctMessageCommandCode::C)}}}});
    return document;
}

std::vector<std::byte> exportedBytes(const SctDocument& document) {
    const SctDocumentExportOptions options{SctPlatform::GameCube,
        kSctShiftJisByte7FEncoding, SctDocumentOutputByteOrder::BigEndian,
        SctDocumentOutputWrapper::Raw, SctOpaquePreservationPolicy::RequirePreservation};
    const auto exported = SctDocumentExporter::exportDocument(document, options);
    EXPECT_TRUE(exported.success);
    std::vector<std::byte> result(exported.bytes.size());
    std::memcpy(result.data(), exported.bytes.data(), exported.bytes.size());
    return result;
}

class FakeCatalog final : public AssetCatalog {
public:
    FakeCatalog(AssetLocator locator, std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)), descriptor_(AssetDescriptor{
              std::move(locator), static_cast<std::uint64_t>(bytes_.size()),
              SourceRevision{zeroDigest()}}),
          snapshot_(AssetCatalogSnapshot{{descriptor_}, DatasetFingerprint{zeroDigest()}}) {}

    const AssetCatalogSnapshot& snapshot() const noexcept override { return snapshot_; }

    Result<SourceAssetSnapshot> loadAsset(const AssetLocator& requested) const override {
        if (requested != descriptor_.locator) {
            return Result<SourceAssetSnapshot>::failure({DiagnosticSeverity::Error,
                DiagnosticCode::AssetNotFound, "not found", requested.path()});
        }
        return Result<SourceAssetSnapshot>::success({descriptor_, bytes_});
    }

private:
    std::vector<std::byte> bytes_;
    AssetDescriptor descriptor_;
    AssetCatalogSnapshot snapshot_;
};

class FakeProject final : public GameProjectContext {
public:
    FakeProject(AssetLocator locator, std::vector<std::byte> bytes)
        : dataset_(DatasetContext{{}, DatasetIdentity{
              std::nullopt, std::nullopt, DatasetFingerprint{zeroDigest()}}}),
          catalog_(std::move(locator), std::move(bytes)) {}

    const DatasetContext& dataset() const noexcept override { return dataset_; }
    const AssetCatalog& assets() const noexcept override { return catalog_; }

private:
    DatasetContext dataset_;
    FakeCatalog catalog_;
};

std::shared_ptr<const SctDocumentSnapshot> loadedSnapshot() {
    const auto locator = testLocator();
    const auto document = makeScriptDocument();
    FakeProject project(locator, exportedBytes(document));
    const auto loaded = SctDocumentLoader::load(project, locator);
    EXPECT_TRUE(loaded.succeeded());
    return loaded.document;
}

std::shared_ptr<const SctDocumentSnapshot> snapshotWith(
    std::shared_ptr<const SctDocumentSnapshot> baseline,
    SctDocument document) {
    auto result = std::make_shared<SctDocumentSnapshot>(*baseline);
    result->document = std::make_shared<const SctDocument>(std::move(document));
    result->analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*result->document,
            result->provenance->importEvidence
                ? &*result->provenance->importEvidence : nullptr));
    const auto validation = SctDocumentValidator::validateDocument(*result->document);
    result->readiness = validation.validDocument
        ? SctDocumentReadiness::StructurallyValid
        : SctDocumentReadiness::Inspectable;
    return result;
}

const SctScriptSectionContent& script(const SctDocumentSnapshot& snapshot) {
    return std::get<SctScriptSectionContent>(snapshot.document->sections.front().content);
}

bool hasCode(const SctEditResult& result, const std::string_view code) {
    return std::ranges::any_of(result.diagnostics, [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}
}  // namespace

TEST(SctEditSession, StartsAtACleanCheckpointAndHistoriesAreIndependent) {
    const auto baseline = loadedSnapshot();
    ASSERT_NE(baseline, nullptr);
    SctEditSession first(baseline);
    SctEditSession second(baseline);

    EXPECT_TRUE(first.structurallyValid());
    EXPECT_FALSE(first.isDirty());
    EXPECT_FALSE(first.canUndo());
    EXPECT_FALSE(first.canRedo());
    EXPECT_EQ(first.currentRevision().value, 1u);

    const auto anchor = script(*baseline).instructions.front().id;
    const auto inserted = first.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(inserted.committed);
    EXPECT_TRUE(first.isDirty());
    EXPECT_EQ(first.currentRevision().value, 2u);
    EXPECT_FALSE(second.isDirty());
    EXPECT_EQ(script(*second.currentSnapshot()).instructions.size(), 2u);
}

TEST(SctEditSession, OffersOnlyFactoryCompletePlatformAgnosticOpcodes) {
    const auto& choices = SctEditSession::insertableOpcodes();
    EXPECT_TRUE(std::ranges::any_of(choices, [](const auto& choice) {
        return choice.opcode == 12 && choice.mnemonic == "Return";
    }));
    EXPECT_FALSE(std::ranges::any_of(choices, [](const auto& choice) {
        return choice.opcode == 9;
    }));
    EXPECT_FALSE(std::ranges::any_of(choices, [](const auto& choice) {
        return choice.opcode == 10;
    }));

    SctEditSession session(loadedSnapshot());
    const auto anchor = script(*session.currentSnapshot()).instructions.front().id;
    const auto rejected = session.insertInstructionAfter(anchor, 10);
    EXPECT_FALSE(rejected.committed);
    EXPECT_TRUE(hasCode(rejected, "OpcodeRequiresParameters"));
    EXPECT_EQ(session.currentRevision().value, 1u);
    EXPECT_FALSE(session.isDirty());
}

TEST(SctEditSession, InsertUndoRedoAndBranchingPreserveAtomicHistory) {
    SctEditSession session(loadedSnapshot());
    const auto section = session.currentSnapshot()->document->sections.front().id;
    const auto original = script(*session.currentSnapshot()).instructions.front().id;

    const auto inserted = session.insertInstructionAfter(original, 125);
    ASSERT_TRUE(inserted.committed);
    EXPECT_TRUE(hasInvalidation(inserted.changes.invalidations,
        SctDerivedAnalysisInvalidation::StructuredControlFlow));
    ASSERT_EQ(inserted.changes.instructions.size(), 1u);
    const auto insertedId = inserted.changes.instructions.front().instruction;
    EXPECT_NE(insertedId, original);
    ASSERT_EQ(session.workingState().instructionOrder(section).size(), 3u);
    EXPECT_EQ(session.workingState().instructionOrder(section)[1], insertedId);
    EXPECT_FALSE(session.undoDescription()->empty());
    ASSERT_TRUE(inserted.suggestedSelection.has_value());
    EXPECT_EQ(inserted.suggestedSelection->id, insertedId.value());

    const auto undone = session.undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(session.workingState().instructionOrder(section).size(), 2u);
    EXPECT_FALSE(session.isDirty());
    EXPECT_TRUE(session.canRedo());
    EXPECT_FALSE(session.redoDescription()->empty());
    ASSERT_TRUE(undone->suggestedSelection.has_value());
    EXPECT_EQ(undone->suggestedSelection->kind, SctNavigationKind::Instruction);
    EXPECT_EQ(undone->suggestedSelection->id, original.value());

    const auto redone = session.redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_EQ(session.workingState().instructionOrder(section)[1], insertedId);
    EXPECT_TRUE(session.isDirty());

    ASSERT_TRUE(session.undo().has_value());
    const auto replacement = session.insertInstructionAfter(original, 125);
    ASSERT_TRUE(replacement.committed);
    EXPECT_FALSE(session.canRedo());
    EXPECT_GT(replacement.revision.value, inserted.revision.value);
    ASSERT_EQ(replacement.changes.instructions.size(), 1u);
    EXPECT_GT(replacement.changes.instructions.front().instruction.value(), insertedId.value());
}

TEST(SctEditSession, DeletesUnreferencedInstructionsWithDeterministicSelectionAndUndo) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({125, 125, 125}));
    SctEditSession session(baseline);
    const auto& instructions = script(*baseline).instructions;
    const auto first = instructions[1].id;
    const auto middle = instructions[2].id;
    const auto last = instructions[3].id;

    const auto deleted = session.deleteInstruction(middle);
    ASSERT_TRUE(deleted.committed);
    EXPECT_TRUE(hasInvalidation(deleted.changes.invalidations,
        SctDerivedAnalysisInvalidation::StructuredControlFlow));
    ASSERT_TRUE(deleted.suggestedSelection.has_value());
    EXPECT_EQ(deleted.suggestedSelection->id, last.value());
    const auto section = baseline->document->sections.front().id;
    ASSERT_EQ(session.workingState().instructionOrder(section).size(), 4u);
    EXPECT_EQ(session.workingState().instructionOrder(section)[1], first);
    EXPECT_EQ(session.workingState().instructionOrder(section)[2], last);

    const auto undone = session.undo();
    ASSERT_TRUE(undone.has_value());
    ASSERT_TRUE(undone->suggestedSelection.has_value());
    EXPECT_EQ(undone->suggestedSelection->id, middle.value());
    EXPECT_EQ(session.workingState().instructionOrder(section).size(), 5u);
}

TEST(SctEditSession, RejectsDeletionWithTypedReferencesOrOpaqueAttachments) {
    auto referenceDocument = makeScriptDocument({125});
    auto& referenceScript = std::get<SctScriptSectionContent>(
        referenceDocument.sections.front().content);
    const auto target = referenceScript.instructions[1].id;
    SctDocumentInstruction jump{referenceDocument.allocateInstructionId(), 10};
    jump.fixedParameters.push_back({0, SctInstructionReference{target}});
    referenceScript.instructions.insert(referenceScript.instructions.begin() + 1, jump);
    ASSERT_TRUE(SctDocumentValidator::validateDocument(referenceDocument).validDocument);

    SctEditSession referenced(snapshotWith(loadedSnapshot(), std::move(referenceDocument)));
    const auto referenceRejected = referenced.deleteInstruction(target);
    EXPECT_FALSE(referenceRejected.committed);
    EXPECT_TRUE(hasCode(referenceRejected, "InstructionHasIncomingReference"));
    EXPECT_FALSE(referenced.isDirty());

    auto attachmentDocument = makeScriptDocument({125});
    auto& attachmentScript = std::get<SctScriptSectionContent>(
        attachmentDocument.sections.front().content);
    const auto anchored = attachmentScript.instructions[1].id;
    attachmentDocument.opaqueAttachments.push_back({
        attachmentDocument.allocateOpaqueAttachmentId(), {0xaa}, anchored,
        SctOpaquePlacement::After, std::nullopt, 1,
        SctOpaqueRelocationSupport::Relocatable, SctOpaqueReason::Gap});
    ASSERT_TRUE(SctDocumentValidator::validateDocument(attachmentDocument).validDocument);

    SctEditSession attached(snapshotWith(loadedSnapshot(), std::move(attachmentDocument)));
    const auto attachmentRejected = attached.deleteInstruction(anchored);
    EXPECT_FALSE(attachmentRejected.committed);
    EXPECT_TRUE(hasCode(attachmentRejected, "InstructionHasOpaqueAttachment"));
    EXPECT_FALSE(attached.isDirty());
}

TEST(SctEditSession, MovesOnlyWithinASectionAndPreservesInstructionIdentity) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({125, 125, 125}));
    SctEditSession session(baseline);
    const auto original = script(*baseline).instructions;

    const auto moved = session.moveInstruction(original[2].id, SctInstructionMoveDirection::Up);
    ASSERT_TRUE(moved.committed);
    EXPECT_TRUE(hasInvalidation(moved.changes.invalidations,
        SctDerivedAnalysisInvalidation::StructuredControlFlow));
    const auto section = baseline->document->sections.front().id;
    const auto reordered = session.workingState().instructionOrder(section);
    ASSERT_EQ(reordered.size(), 5u);
    EXPECT_EQ(reordered[1], original[2].id);
    EXPECT_EQ(reordered[2], original[1].id);
    ASSERT_EQ(moved.changes.instructions.size(), 1u);
    EXPECT_EQ(moved.changes.instructions.front().instruction, original[2].id);

    const auto boundary = session.moveInstruction(original[2].id, SctInstructionMoveDirection::Up);
    EXPECT_FALSE(boundary.committed);
    EXPECT_TRUE(hasCode(boundary, "InstructionMoveAcrossLabel"));
    EXPECT_EQ(session.currentRevision(), moved.revision);
}

TEST(SctEditSession, EnforcesLabelAndReturnAuthoringBoundaries) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({125}));
    SctEditSession session(baseline);
    const auto& instructions = script(*baseline).instructions;
    const auto label = instructions[0].id;
    const auto body = instructions[1].id;
    const auto terminalReturn = instructions[2].id;

    const auto afterReturn = session.insertInstructionAfter(terminalReturn, 125);
    EXPECT_FALSE(afterReturn.committed);
    EXPECT_TRUE(hasCode(afterReturn, "InstructionInsertionAfterReturn"));

    const auto earlyReturn = session.insertInstructionAfter(label, 12);
    EXPECT_FALSE(earlyReturn.committed);
    EXPECT_TRUE(hasCode(earlyReturn, "ReturnMustTerminateSection"));

    const auto genericLabel = session.insertInstructionAfter(body, 9);
    EXPECT_FALSE(genericLabel.committed);
    EXPECT_TRUE(hasCode(genericLabel, "LabelInsertionReserved"));

    const auto deleteLabel = session.deleteInstruction(label);
    EXPECT_FALSE(deleteLabel.committed);
    EXPECT_TRUE(hasCode(deleteLabel, "ProtectedSectionLabel"));

    const auto moveLabel = session.moveInstruction(label, SctInstructionMoveDirection::Down);
    EXPECT_FALSE(moveLabel.committed);
    EXPECT_TRUE(hasCode(moveLabel, "InstructionMoveAcrossLabel"));
    const auto moveBodyAboveLabel = session.moveInstruction(body, SctInstructionMoveDirection::Up);
    EXPECT_FALSE(moveBodyAboveLabel.committed);
    EXPECT_TRUE(hasCode(moveBodyAboveLabel, "InstructionMoveAcrossLabel"));
    const auto moveBodyAfterReturn = session.moveInstruction(body, SctInstructionMoveDirection::Down);
    EXPECT_FALSE(moveBodyAfterReturn.committed);
    EXPECT_TRUE(hasCode(moveBodyAfterReturn, "InstructionMoveAcrossReturn"));
    const auto moveReturn = session.moveInstruction(terminalReturn, SctInstructionMoveDirection::Up);
    EXPECT_FALSE(moveReturn.committed);
    EXPECT_TRUE(hasCode(moveReturn, "InstructionMoveAcrossReturn"));

    EXPECT_EQ(session.currentRevision().value, 1u);
    EXPECT_FALSE(session.isDirty());
    EXPECT_FALSE(session.canUndo());
}

TEST(SctEditSession, AllowsAnOptionalTerminalReturnToBeAddedAndRemoved) {
    auto withoutReturn = snapshotWith(loadedSnapshot(), makeScriptDocument({125}, false));
    SctEditSession session(withoutReturn);
    const auto body = script(*withoutReturn).instructions.back().id;

    const auto inserted = session.insertInstructionAfter(body, 12);
    ASSERT_TRUE(inserted.committed);
    const auto section = withoutReturn->document->sections.front().id;
    const auto insertedInstructions = session.workingState().instructionOrder(section);
    const auto terminalReturn = insertedInstructions.back();
    ASSERT_EQ(session.workingState().instruction(terminalReturn)->opcode, 12u);

    const auto removed = session.deleteInstruction(terminalReturn);
    ASSERT_TRUE(removed.committed);
    EXPECT_EQ(session.workingState().instructionOrder(section).size(), 2u);

    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingState().instruction(
        session.workingState().instructionOrder(section).back())->opcode, 12u);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingState().instruction(
        session.workingState().instructionOrder(section).back())->opcode, 125u);
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_EQ(session.workingState().instruction(
        session.workingState().instructionOrder(section).back())->opcode, 12u);
}

TEST(SctEditSession, InvalidDocumentsStayReadOnlyAndFailedEditsDoNotConsumeHistory) {
    auto invalidDocument = makeScriptDocument();
    invalidDocument.sections.push_back(invalidDocument.sections.front());
    auto baseline = snapshotWith(loadedSnapshot(), std::move(invalidDocument));
    SctEditSession invalid(baseline);
    EXPECT_FALSE(invalid.structurallyValid());
    const auto instruction = script(*baseline).instructions.front().id;
    const auto rejected = invalid.deleteInstruction(instruction);
    EXPECT_FALSE(rejected.committed);
    EXPECT_TRUE(hasCode(rejected, "DocumentNotStructurallyValid"));
    EXPECT_EQ(invalid.currentRevision().value, 1u);

    SctEditSession valid(loadedSnapshot());
    const auto missing = valid.deleteInstruction(SctInstructionId{999});
    EXPECT_FALSE(missing.committed);
    EXPECT_EQ(valid.currentRevision().value, 1u);
    const auto anchor = script(*valid.currentSnapshot()).instructions.front().id;
    const auto inserted = valid.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(inserted.committed);
    EXPECT_EQ(inserted.revision.value, 2u);
}

TEST(SctEditSession, EditedSnapshotsRetainSourceAndInterpretationProvenance) {
    const auto baseline = loadedSnapshot();
    SctEditSession session(baseline);
    const auto anchor = script(*baseline).instructions.front().id;
    const auto edited = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(edited.committed);

    EXPECT_EQ(edited.snapshot->provenance.get(), baseline->provenance.get());
    EXPECT_EQ(edited.snapshot->provenance->source().descriptor.locator,
        baseline->provenance->source().descriptor.locator);
    EXPECT_EQ(edited.snapshot->provenance->source().descriptor.revision,
        baseline->provenance->source().descriptor.revision);
    EXPECT_EQ(edited.snapshot->provenance->inspection.get(),
        baseline->provenance->inspection.get());
    EXPECT_EQ(edited.snapshot->provenance->textConvention,
        baseline->provenance->textConvention);
    EXPECT_EQ(edited.snapshot->provenance->textSelectionOrigin,
        baseline->provenance->textSelectionOrigin);
    EXPECT_EQ(edited.snapshot->readiness, SctDocumentReadiness::StructurallyValid);
    EXPECT_TRUE(std::ranges::none_of(edited.snapshot->diagnostics, [](const auto& diagnostic) {
        return diagnostic.stage == SctPipelineStage::Edit;
    }));
}

TEST(SctEditSession, ReplacesIndexedAndFooterMessagesAsAtomicRevisions) {
    auto baseline = snapshotWith(loadedSnapshot(), makeMessageDocument());
    ASSERT_EQ(baseline->document->sections.size(), 2u);
    const auto& indexed = std::get<SctStringSectionContent>(
        baseline->document->sections[1].content).string;
    const auto& footer = baseline->document->footerEntries.front();
    SctEditSession session(baseline);

    const auto indexedProjection = SctMessageAuthoringProfile::project(
        std::get<SctMessage>(indexed.value));
    ASSERT_TRUE(indexedProjection.supported());
    auto indexedDraft = *indexedProjection.draft;
    indexedDraft.headerUtf8 = "Speaker";
    indexedDraft.body = {{"Changed", {true, SctMessageRgb{1, 2, 3}}}};
    const auto indexedEdit = session.replaceMessage(
        SctMessageTarget{indexed.id}, indexedDraft, SctMessageEditKind::Typing);
    ASSERT_TRUE(indexedEdit.committed);
    EXPECT_FALSE(hasInvalidation(indexedEdit.changes.invalidations,
        SctDerivedAnalysisInvalidation::StructuredControlFlow));
    ASSERT_EQ(indexedEdit.changes.modified.size(), 1u);
    EXPECT_EQ(indexedEdit.changes.modified.front(),
        (SctNavigationTarget{SctNavigationKind::String, indexed.id.value()}));
    EXPECT_EQ(session.undoDescription(), std::optional<std::string_view>{"Edit message text"});
    EXPECT_EQ(session.currentRevision().value, 2u);

    const auto noOp = session.replaceMessage(
        SctMessageTarget{indexed.id}, indexedDraft, SctMessageEditKind::Formatting);
    EXPECT_FALSE(noOp.committed);
    EXPECT_TRUE(noOp.diagnostics.empty());
    EXPECT_EQ(session.currentRevision().value, 2u);

    const auto footerProjection = SctMessageAuthoringProfile::project(
        std::get<SctMessage>(footer.value));
    ASSERT_TRUE(footerProjection.supported());
    auto footerDraft = *footerProjection.draft;
    footerDraft.position = SctMessagePosition::Lower;
    footerDraft.disableFastForward = true;
    footerDraft.openDuration = 20;
    const auto footerEdit = session.replaceMessage(
        SctMessageTarget{footer.id}, footerDraft, SctMessageEditKind::Options);
    ASSERT_TRUE(footerEdit.committed);
    EXPECT_EQ(footerEdit.suggestedSelection,
        (SctNavigationTarget{SctNavigationKind::FooterEntry, footer.id.value()}));
    EXPECT_EQ(session.currentRevision().value, 3u);

    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.currentRevision().value, 2u);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_FALSE(session.isDirty());
    ASSERT_TRUE(session.redo().has_value());
    const auto* changed = session.workingState().message(SctMessageTarget{indexed.id});
    ASSERT_NE(changed, nullptr);
    const auto changedProjection = SctMessageAuthoringProfile::project(
        *changed);
    ASSERT_TRUE(changedProjection.supported());
    EXPECT_EQ(*changedProjection.draft, indexedDraft);
}

TEST(SctEditSession, RejectsMissingWrongKindAndUnsupportedMessageTargetsWithoutHistory) {
    auto document = makeMessageDocument();
    auto& indexed = std::get<SctStringSectionContent>(document.sections[1].content).string;
    const auto stringId = indexed.id;
    auto unsupported = std::get<SctMessage>(indexed.value);
    unsupported.body.elements.insert(unsupported.body.elements.begin(),
        SctTextBuilder::decimalCommand(SctMessageCommandCode::S, 5).command.value());
    indexed.value = unsupported;
    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));

    SctMessageDraft replacement;
    replacement.body = {{"Replacement", {}}};
    const auto unsupportedResult = session.replaceMessage(
        SctMessageTarget{stringId}, replacement, SctMessageEditKind::Replacement);
    EXPECT_FALSE(unsupportedResult.committed);
    EXPECT_TRUE(hasCode(unsupportedResult, "MessageOutsideAuthoringProfile"));

    const auto missing = session.replaceMessage(
        SctMessageTarget{SctStringId{9999}}, replacement, SctMessageEditKind::Typing);
    EXPECT_FALSE(missing.committed);
    EXPECT_TRUE(hasCode(missing, "MessageTargetNotFound"));
    EXPECT_EQ(session.currentRevision().value, 1u);
    EXPECT_FALSE(session.isDirty());
}

TEST(SctSemanticOperation, AppliesBatchesAtomicallyAndReturnsAReplayableInverse) {
    auto document = makeScriptDocument({125, 125});
    const auto original = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctDocumentInstruction inserted{SctInstructionId{
        document.nextInstructionIdValue()}, 125};
    const SctSemanticOperationBatch batch{{
        SctInsertInstructionAfterOperation{original.front().id, inserted},
        SctDeleteInstructionOperation{original[1].id},
    }};

    const auto applied = SctSemanticOperationService::apply(document, batch);
    ASSERT_TRUE(applied.succeeded());
    ASSERT_EQ(applied.forwardChanges.instructions.size(), 2u);
    EXPECT_FALSE(applied.forwardChanges.instructions[0].before.has_value());
    EXPECT_FALSE(applied.forwardChanges.instructions[1].after.has_value());
    ASSERT_EQ(applied.inverse.operations.size(), 2u);
    const auto& edited = std::get<SctScriptSectionContent>(
        applied.document->sections.front().content).instructions;
    ASSERT_EQ(edited.size(), original.size());
    EXPECT_EQ(edited[1].id, inserted.id);
    EXPECT_GT(applied.document->nextInstructionIdValue(), inserted.id.value());

    const auto restored = SctSemanticOperationService::apply(
        *applied.document, applied.inverse);
    ASSERT_TRUE(restored.succeeded());
    EXPECT_GT(restored.document->nextInstructionIdValue(), inserted.id.value());
    const auto& roundTrip = std::get<SctScriptSectionContent>(
        restored.document->sections.front().content).instructions;
    ASSERT_EQ(roundTrip.size(), original.size());
    for (std::size_t index = 0; index < original.size(); ++index) {
        EXPECT_EQ(roundTrip[index].id, original[index].id);
        EXPECT_EQ(roundTrip[index].opcode, original[index].opcode);
    }

    const SctSemanticOperationBatch invalid{{
        SctInsertInstructionAfterOperation{original.front().id, inserted},
        SctDeleteInstructionOperation{SctInstructionId{999999}},
    }};
    const auto rejected = SctSemanticOperationService::apply(document, invalid);
    EXPECT_FALSE(rejected.succeeded());
    EXPECT_EQ(std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions.size(), original.size());
}

TEST(SctSemanticOperation, RelocatesAndReplacesMessagesWithExactInverseChanges) {
    auto document = makeMessageDocument();
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctDocumentInstruction firstBody{document.allocateInstructionId(), 125};
    SctDocumentInstruction secondBody{document.allocateInstructionId(), 125};
    instructions.insert(instructions.begin() + 1, firstBody);
    instructions.insert(instructions.begin() + 2, secondBody);
    const auto stringId = std::get<SctStringSectionContent>(
        document.sections[1].content).string.id;
    auto replacement = std::get<SctMessage>(std::get<SctStringSectionContent>(
        document.sections[1].content).string.value);
    replacement.headerUtf8 = "Changed";

    const SctSemanticOperationBatch batch{{
        SctRelocateInstructionAfterOperation{secondBody.id, instructions.front().id},
        SctReplaceMessageOperation{SctMessageTarget{stringId}, replacement},
    }};
    const auto applied = SctSemanticOperationService::apply(document, batch);
    ASSERT_TRUE(applied.succeeded());
    ASSERT_EQ(applied.forwardChanges.instructions.size(), 1u);
    ASSERT_EQ(applied.forwardChanges.modified.size(), 1u);
    EXPECT_EQ(applied.reverseChanges.instructions.front().before,
        applied.forwardChanges.instructions.front().after);
    EXPECT_EQ(applied.reverseChanges.instructions.front().after,
        applied.forwardChanges.instructions.front().before);
    EXPECT_EQ(applied.reverseChanges.modified, applied.forwardChanges.modified);

    const auto restored = SctSemanticOperationService::apply(
        *applied.document, applied.inverse);
    ASSERT_TRUE(restored.succeeded());
    const auto index = SctDocumentIndex::build(*restored.document);
    ASSERT_NE(index.find(*restored.document, stringId), nullptr);
    const auto projection = SctMessageAuthoringProfile::project(
        std::get<SctMessage>(index.find(*restored.document, stringId)->value));
    ASSERT_TRUE(projection.supported());
    EXPECT_FALSE(projection.draft->headerUtf8.has_value());
}

TEST(SctEditSession, UndoRedoExposeForwardAndReverseRevisionTransitions) {
    SctEditSession session(loadedSnapshot());
    const auto anchor = script(*session.currentSnapshot()).instructions.front().id;
    const auto inserted = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(inserted.transition.has_value());
    EXPECT_EQ(inserted.transition->kind, SctRevisionTransitionKind::Commit);
    EXPECT_EQ(inserted.transition->from.value, 1u);
    EXPECT_EQ(inserted.transition->to, inserted.revision);
    ASSERT_EQ(inserted.transition->changes.instructions.size(), 1u);

    const auto undone = session.undo();
    ASSERT_TRUE(undone.has_value());
    ASSERT_TRUE(undone->transition.has_value());
    EXPECT_EQ(undone->transition->kind, SctRevisionTransitionKind::Undo);
    EXPECT_EQ(undone->transition->from, inserted.revision);
    EXPECT_EQ(undone->transition->to.value, 1u);
    ASSERT_EQ(undone->changes.instructions.size(), 1u);
    EXPECT_FALSE(undone->changes.instructions.front().after.has_value());
    EXPECT_EQ(undone->changes.instructions.front().instruction,
        inserted.changes.instructions.front().instruction);

    const auto redone = session.redo();
    ASSERT_TRUE(redone.has_value());
    ASSERT_TRUE(redone->transition.has_value());
    EXPECT_EQ(redone->transition->kind, SctRevisionTransitionKind::Redo);
    ASSERT_EQ(redone->changes.instructions.size(), 1u);
    EXPECT_EQ(redone->changes.instructions.front().instruction,
        inserted.changes.instructions.front().instruction);
}

TEST(SctEditSession, MaterializesLongJournalsAndPrunesDiscardedCheckpointBranches) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({}, false));
    SctEditSession session(baseline);
    const auto anchor = script(*baseline).instructions.front().id;
    std::vector<RevisionId> revisions{session.currentRevision()};

    for (int edit = 0; edit < 40; ++edit) {
        auto result = session.insertInstructionAfter(anchor, 125);
        ASSERT_TRUE(result.committed);
        revisions.push_back(result.revision);
    }
    const auto materialized = session.materializeRevision(revisions[20]);
    ASSERT_TRUE(materialized.has_value());
    EXPECT_EQ(std::get<SctScriptSectionContent>(
        (*materialized)->sections.front().content).instructions.size(), 21u);

    for (int undo = 0; undo < 10; ++undo) ASSERT_TRUE(session.undo().has_value());
    const auto discardedRevision = revisions.back();
    const auto branched = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(branched.committed);
    EXPECT_GT(branched.revision.value, discardedRevision.value);
    EXPECT_FALSE(session.canRedo());
    EXPECT_FALSE(session.materializeRevision(discardedRevision).has_value());
}

TEST(SctEditSession, KeepsVerifiedSnapshotStableWhileWorkingRevisionsAdvance) {
    SctEditSession session(loadedSnapshot());
    const auto anchor = script(*session.currentSnapshot()).instructions.front().id;
    const auto verified = session.verifiedSnapshot();
    auto first = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(first.committed);
    const auto second = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(second.committed);
    EXPECT_EQ(session.verifiedSnapshot().get(), verified.get());
    EXPECT_EQ(session.workingRevision(), second.revision);
    EXPECT_GT(session.workingState().instructionOrder(
        verified->document->sections.front().id).size(),
        script(*verified).instructions.size());
}

TEST(SctWorkingState, AppliesBatchesAtomicallyWithoutReusingAllocatedIds) {
    auto document = std::make_shared<const SctDocument>(makeScriptDocument({125}, false));
    SctWorkingState state(document);
    const auto section = document->sections.front().id;
    const auto anchor = std::get<SctScriptSectionContent>(
        document->sections.front().content).instructions.back().id;
    const auto nextId = state.nextInstructionIdValue();
    SctDocumentInstruction inserted{SctInstructionId{nextId}, 125};
    const auto rejected = state.apply(SctSemanticOperationBatch{{
        SctInsertInstructionAfterOperation{anchor, inserted},
        SctDeleteInstructionOperation{SctInstructionId{999999}},
    }});
    EXPECT_FALSE(rejected.succeeded());
    EXPECT_EQ(state.instructionOrder(section).size(), 2u);
    EXPECT_EQ(state.instruction(inserted.id), nullptr);
    EXPECT_GT(state.nextInstructionIdValue(), nextId);
}

TEST(SctEditSession, MaterializesAndInstallsTheNewestWorkingRevision) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({125}, false));
    SctEditSession session(baseline);
    const auto section = baseline->document->sections.front().id;
    const auto anchor = script(*baseline).instructions.back().id;
    const auto first = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(first.committed);
    const auto second = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(second.committed);
    ASSERT_EQ(session.workingState().instructionOrder(section).size(), 4u);

    const auto request = session.materializationRequest(17);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->targetRevision, second.revision);
    EXPECT_EQ(request->journalTail.size(), 2u);
    const auto materialized = SctDocumentMaterializer::materialize(*request);
    ASSERT_TRUE(materialized.succeeded());
    ASSERT_NE(materialized.analysis, nullptr);
    ASSERT_NE(materialized.structuredControlFlow, nullptr);
    EXPECT_EQ(materialized.analysis->usage.opcodeUsages().size(), 4u);
    EXPECT_TRUE(materialized.analysis->importedSites.has_value());
    EXPECT_EQ(materialized.generation, 17u);
    EXPECT_TRUE(session.installVerifiedMaterialization(materialized));
    EXPECT_EQ(session.verifiedSnapshot()->document.get(), materialized.document.get());
    EXPECT_EQ(session.verifiedSnapshot()->analysis.get(), materialized.analysis.get());
    EXPECT_EQ(session.verifiedSnapshot()->structuredControlFlow.get(),
        materialized.structuredControlFlow.get());
    EXPECT_EQ(script(*session.verifiedSnapshot()).instructions.size(), 4u);
}

TEST(SctDocumentMaterializer, HonorsCancellationBeforeReplay) {
    auto document = std::make_shared<const SctDocument>(makeScriptDocument({125}, false));
    SctMaterializationRequest request;
    request.generation = 3;
    request.baseRevision = RevisionId{1};
    request.targetRevision = RevisionId{2};
    request.baseDocument = document;
    std::stop_source stop;
    stop.request_stop();
    const auto result = SctDocumentMaterializer::materialize(request, stop.get_token());
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.succeeded());
}

TEST(SctEditSession, VerificationRejectionRollsBackAndDiscardsTheRejectedLineage) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({125}, false));
    SctEditSession session(baseline);
    const auto section = baseline->document->sections.front().id;
    const auto anchor = script(*baseline).instructions.back().id;
    const auto first = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(first.committed);
    const auto second = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(second.committed);

    SctPipelineDiagnostic rejection;
    rejection.severity = DiagnosticSeverity::Error;
    rejection.stage = SctPipelineStage::Validation;
    rejection.code = "TestRejection";
    rejection.message = "Synthetic background rejection.";
    const auto rollback = session.rejectToVerifiedRevision(
        RevisionId{1}, {rejection});
    ASSERT_TRUE(rollback.has_value());
    ASSERT_TRUE(rollback->transition.has_value());
    EXPECT_EQ(rollback->transition->kind,
        SctRevisionTransitionKind::VerificationRollback);
    EXPECT_EQ(session.workingRevision().value, 1u);
    EXPECT_EQ(session.workingState().instructionOrder(section).size(), 2u);
    EXPECT_EQ(session.verifiedSnapshot().get(), baseline.get());
    EXPECT_EQ(session.verifiedSnapshot()->analysis.get(), baseline->analysis.get());
    EXPECT_FALSE(session.canRedo());
    EXPECT_FALSE(session.materializeRevision(second.revision).has_value());

    const auto replacement = session.insertInstructionAfter(anchor, 125);
    ASSERT_TRUE(replacement.committed);
    EXPECT_GT(replacement.revision.value, second.revision.value);
}
