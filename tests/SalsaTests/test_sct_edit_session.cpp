#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctGlyphCatalog.h"
#include "SalsaCore/Sct/SctParameterAuthoring.h"
#include "SpiceSCT/SctScptEncoding.h"
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

TEST(SctEditSession, RebasedSessionHasOneUndoBackToTheNewSourceBaseline) {
    const auto baseline = loadedSnapshot();
    ASSERT_NE(baseline, nullptr);
    auto rebasedDocument = *baseline->document;
    auto& instructions = std::get<SctScriptSectionContent>(
        rebasedDocument.sections.front().content).instructions;
    instructions.front().skipRefresh = true;
    const auto rebased = snapshotWith(baseline, std::move(rebasedDocument));

    auto session = SctEditSession::createRebased(baseline, rebased, {}, {});
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(session->isDirty());
    EXPECT_TRUE(session->canUndo());
    EXPECT_EQ(session->undoDescription(), "Rebase patch onto new source");
    EXPECT_TRUE(script(*session->currentSnapshot()).instructions.front().skipRefresh);

    const auto undone = session->undo();
    ASSERT_TRUE(undone.has_value());
    EXPECT_FALSE(script(*session->currentSnapshot()).instructions.front().skipRefresh);
    EXPECT_TRUE(session->isDirty());
    EXPECT_TRUE(session->canRedo());

    const auto redone = session->redo();
    ASSERT_TRUE(redone.has_value());
    EXPECT_TRUE(script(*session->currentSnapshot()).instructions.front().skipRefresh);
    EXPECT_FALSE(session->isDirty());
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

TEST(SctEditSession, MovesDeletesAndRestoresContiguousInstructionRangesAtomically) {
    auto baseline = snapshotWith(loadedSnapshot(), makeScriptDocument({125, 125, 125}));
    SctEditSession session(baseline);
    const auto original = script(*baseline).instructions;
    const auto section = baseline->document->sections.front().id;
    const std::array selected{original[1].id, original[2].id};

    const auto moved = session.moveInstructionsAfter(selected, original[3].id);
    ASSERT_TRUE(moved.committed);
    EXPECT_EQ(moved.suggestedSelectionRange,
        (std::vector<SctNavigationTarget>{{SctNavigationKind::Instruction,
            original[1].id.value()}, {SctNavigationKind::Instruction,
            original[2].id.value()}}));
    const auto order = [&] {
        const auto current = session.workingState().instructionOrder(section);
        return std::vector<SctInstructionId>(current.begin(), current.end());
    };
    EXPECT_EQ(order(), (std::vector<SctInstructionId>{original[0].id,
        original[3].id, original[1].id, original[2].id, original[4].id}));

    const auto undoneMove = session.undo();
    ASSERT_TRUE(undoneMove.has_value());
    EXPECT_EQ(order(),
        (std::vector<SctInstructionId>{original[0].id, original[1].id,
            original[2].id, original[3].id, original[4].id}));
    ASSERT_TRUE(session.redo().has_value());

    const auto deleted = session.deleteInstructions(selected);
    ASSERT_TRUE(deleted.committed);
    EXPECT_EQ(order(),
        (std::vector<SctInstructionId>{original[0].id, original[3].id,
            original[4].id}));
    const auto undoneDelete = session.undo();
    ASSERT_TRUE(undoneDelete.has_value());
    EXPECT_EQ(undoneDelete->suggestedSelectionRange,
        (std::vector<SctNavigationTarget>{{SctNavigationKind::Instruction,
            original[1].id.value()}, {SctNavigationKind::Instruction,
            original[2].id.value()}}));
    EXPECT_EQ(order(),
        (std::vector<SctInstructionId>{original[0].id, original[3].id,
            original[1].id, original[2].id, original[4].id}));
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
    EXPECT_FALSE(materialized.analysis->structuredControlFlow.sections().empty());
    EXPECT_EQ(materialized.analysis->usage.opcodeUsages().size(), 4u);
    EXPECT_TRUE(materialized.analysis->importedSites.has_value());
    EXPECT_EQ(materialized.generation, 17u);
    EXPECT_TRUE(session.installVerifiedMaterialization(materialized));
    EXPECT_EQ(session.verifiedSnapshot()->document.get(), materialized.document.get());
    EXPECT_EQ(session.verifiedSnapshot()->analysis.get(), materialized.analysis.get());
    EXPECT_EQ(&session.verifiedSnapshot()->analysis->structuredControlFlow,
        &materialized.analysis->structuredControlFlow);
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

TEST(SctEditSession, CreatesRenamesMovesAndDeletesScriptSectionsAtomically) {
    SctEditSession session(loadedSnapshot());
    const auto original = session.workingState().sectionOrder().front();

    const auto created = session.createScriptSection("NEW_BLOCK", original, true);
    ASSERT_TRUE(created.committed);
    ASSERT_EQ(created.changes.sections.size(), 1u);
    const auto sectionId = created.changes.sections.front().section;
    const auto* sectionValue = session.workingState().section(sectionId);
    ASSERT_NE(sectionValue, nullptr);
    const auto& instructions = std::get<SctScriptSectionContent>(sectionValue->content).instructions;
    ASSERT_EQ(instructions.size(), 2u);
    EXPECT_EQ(instructions.front().opcode, 9u);
    EXPECT_EQ(instructions.back().opcode, 12u);

    const auto renamed = session.renameSection(sectionId, "new_block");
    ASSERT_TRUE(renamed.committed);
    EXPECT_EQ(session.workingState().section(sectionId)->nameBytes, "new_block");

    const auto moved = session.moveSection(sectionId, SctSectionMoveDirection::Up);
    ASSERT_TRUE(moved.committed);
    EXPECT_EQ(session.workingState().sectionOrder().front(), sectionId);

    const auto removed = session.deleteSection(sectionId);
    ASSERT_TRUE(removed.committed);
    EXPECT_EQ(session.workingState().section(sectionId), nullptr);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_NE(session.workingState().section(sectionId), nullptr);
}

TEST(SctEditSession, EnforcesNewSectionNameProfileAndExactDuplicatePolicy) {
    SctEditSession session(loadedSnapshot());
    EXPECT_TRUE(hasCode(session.createScriptSection("bad-name", std::nullopt, true),
        "InvalidAuthoredSectionName"));
    EXPECT_TRUE(hasCode(session.createScriptSection("SCRIPT", std::nullopt, true),
        "DuplicateSectionName"));
    const auto created = session.createScriptSection("script", std::nullopt, false);
    ASSERT_TRUE(created.committed);
    const auto sectionId = created.changes.sections.front().section;
    const auto& instructions = std::get<SctScriptSectionContent>(
        session.workingState().section(sectionId)->content).instructions;
    ASSERT_EQ(instructions.size(), 1u);
    EXPECT_EQ(instructions.front().opcode, 9u);
}

TEST(SctEditSession, CreatesEditsAndDeletesIndexedAndFooterText) {
    SctEditSession session(loadedSnapshot());
    const auto createdString = session.createIndexedString("MS_TEST", std::nullopt);
    ASSERT_TRUE(createdString.committed);
    ASSERT_TRUE(createdString.suggestedSelection.has_value());
    const SctTextTarget stringTarget{SctStringId(createdString.suggestedSelection->id)};
    ASSERT_NE(session.workingState().message(stringTarget), nullptr);

    const auto createdFooter = session.createFooterText(
        SctCreatedFooterTextKind::Message, std::nullopt);
    ASSERT_TRUE(createdFooter.committed);
    const SctTextTarget footerTarget{SctFooterEntryId(createdFooter.suggestedSelection->id)};
    ASSERT_NE(session.workingState().message(footerTarget), nullptr);

    const auto rejectedPlain = session.createFooterText(
        SctCreatedFooterTextKind::PlainText, std::nullopt);
    EXPECT_FALSE(rejectedPlain.committed);
    EXPECT_TRUE(hasCode(rejectedPlain, "StandaloneFooterPlainTextUnsupported"));

    ASSERT_TRUE(session.deleteTextEntity(stringTarget).committed);
    EXPECT_EQ(session.workingState().textValue(stringTarget), nullptr);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_NE(session.workingState().textValue(stringTarget), nullptr);
}

TEST(SctEditSession, MaterializesCombinedSectionAndTextLifecycleJournal) {
    SctEditSession session(loadedSnapshot());
    ASSERT_TRUE(session.createScriptSection("SECOND", std::nullopt, true).committed);
    ASSERT_TRUE(session.createIndexedString("MS_NEW", std::nullopt).committed);
    ASSERT_TRUE(session.createFooterText(
        SctCreatedFooterTextKind::Message, std::nullopt).committed);
    const auto request = session.materializationRequest(41);
    ASSERT_TRUE(request.has_value());
    const auto materialized = SctDocumentMaterializer::materialize(*request);
    ASSERT_TRUE(materialized.succeeded());
    ASSERT_NE(materialized.document, nullptr);
    EXPECT_EQ(materialized.document->sections.size(), 3u);
    EXPECT_EQ(materialized.document->footerEntries.size(), 1u);
    EXPECT_TRUE(session.installVerifiedMaterialization(materialized));
}

TEST(SctEditSession, ReplacesOpaqueTextWithOneSemanticRepairRevision) {
    auto document = makeMessageDocument();
    auto& value = document.footerEntries.front().value;
    value = SctOpaqueText{{'H', 'i', 0}};
    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));
    const SctTextTarget target{session.workingState().footerEntryOrder().front()};
    const auto before = session.currentRevision();
    const auto repaired = session.replaceTextValue(
        target, SctPlainText{"Hi"}, "Repair opaque text interpretation",
        SctTextRepairProvenance{kSctWindows1252Byte7FEncoding,
            SctKnownTextConvention::Windows1252Byte7F, "digest"});
    ASSERT_TRUE(repaired.committed);
    EXPECT_EQ(repaired.revision.value, before.value + 1u);
    ASSERT_NE(std::get_if<SctPlainText>(session.workingState().textValue(target)), nullptr);
    ASSERT_TRUE(session.workingState().textRepairProvenance(target).has_value());
    EXPECT_EQ(session.workingState().textRepairProvenance(target)->sourceSha256, "digest");
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_NE(std::get_if<SctOpaqueText>(session.workingState().textValue(target)), nullptr);
    EXPECT_FALSE(session.workingState().textRepairProvenance(target).has_value());
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_TRUE(session.workingState().textRepairProvenance(target).has_value());
}

TEST(SctEditSession, CurrentDiagnosticsTrackAmbiguousTextRepairAndUndo) {
    auto document = makeMessageDocument();
    document.footerEntries.front().value = SctOpaqueText{{'H', 'i', 0}};
    auto snapshot = std::make_shared<SctDocumentSnapshot>(
        *snapshotWith(loadedSnapshot(), std::move(document)));
    const auto footer = snapshot->document->footerEntries.front().id;
    SctPipelineDiagnostic warning;
    warning.severity = DiagnosticSeverity::Warning;
    warning.stage = SctPipelineStage::Import;
    warning.code = "AmbiguousString";
    warning.message = "The source text is ambiguous.";
    warning.primaryLocation = SctDiagnosticLocation{
        SctDocumentEntityId{footer}};
    snapshot->diagnostics.push_back(warning);

    SctEditSession session(snapshot);
    const SctTextTarget target{footer};
    const auto ambiguousCount = [&session] {
        return std::ranges::count_if(session.currentDiagnostics(), [](const auto& diagnostic) {
            return diagnostic.code == "AmbiguousString";
        });
    };
    ASSERT_EQ(ambiguousCount(), 1);
    ASSERT_TRUE(session.replaceTextValue(target, SctPlainText{"Hi"}).committed);
    EXPECT_EQ(ambiguousCount(), 0);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(ambiguousCount(), 1);
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_EQ(ambiguousCount(), 0);
}

TEST(SctGlyphCatalog, PreservesLegacyMembershipAndSearchEvidence) {
    const auto& catalog = SctGlyphCatalog::legacySupportedSet();
    EXPECT_GT(catalog.entries().size(), 300u);
    const auto euro = catalog.search("U+20AC", SctGlyphMembership::European);
    ASSERT_EQ(euro.size(), 1u);
    EXPECT_EQ(euro.front().utf8, "€");
    EXPECT_FALSE(euro.front().provenance.empty());
    const auto music = catalog.search("♪", SctGlyphMembership::UsJapanese);
    ASSERT_EQ(music.size(), 1u);
    EXPECT_EQ(music.front().category, "US/JP legacy set");
}

TEST(SctWorkingState, FailedSectionRelocationIsAtomic) {
    auto document = std::make_shared<const SctDocument>(makeScriptDocument());
    SctWorkingState state(document);
    const auto before = std::vector<SctSectionId>(
        state.sectionOrder().begin(), state.sectionOrder().end());
    const auto applied = state.apply(SctSemanticOperationBatch{{
        SctRelocateSectionAfterOperation{before.front(), SctSectionId(9999)}}});
    EXPECT_FALSE(applied.succeeded());
    EXPECT_EQ(std::vector<SctSectionId>(state.sectionOrder().begin(),
        state.sectionOrder().end()), before);
}

TEST(SctParameterEditing, ReplacesConventionalScptByStableSiteAndPreservesOutlineScope) {
    auto document = makeScriptDocument();
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 16u;
    request.parameterOverrides.push_back({{0u, std::nullopt},
        SctExpressionFactory::encodedDecimalLiteral(1)});
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    ASSERT_TRUE(SctDocumentValidator::validateDocument(document).validDocument);

    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));
    const SctParameterSite site{built.instruction->id, {0u, std::nullopt}};
    const auto presentation = SctParameterAuthoringService::project(
        session.workingState(), site.instruction);
    ASSERT_EQ(presentation.fixedParameters.size(), 1u);
    EXPECT_EQ(presentation.fixedParameters.front().editor,
        SctInlineParameterEditorKind::ConventionalScpt);

    const auto edited = session.editParameterText(site, "2.5");
    ASSERT_TRUE(edited.committed);
    ASSERT_EQ(edited.changes.parameters.size(), 1u);
    ASSERT_EQ(edited.changes.instructions.size(), 1u);
    EXPECT_EQ(edited.changes.instructions.front().before,
        edited.changes.instructions.front().after);
    EXPECT_FALSE(hasInvalidation(edited.changes.invalidations,
        SctDerivedAnalysisInvalidation::StructuredControlFlow));
    const auto* parameter = session.workingState().parameter(site);
    ASSERT_NE(parameter, nullptr);
    const auto* expression = std::get_if<SctCanonicalExpression>(&parameter->value);
    ASSERT_NE(expression, nullptr);
    EXPECT_EQ(encodeSctCanonicalExpressionWords(*expression),
        encodeSctCanonicalExpressionWords(
            SctExpressionFactory::encodedDecimalLiteral(2, 128)));
    const auto revision = session.currentRevision();
    const auto noOp = session.editParameterText(site, "2.5");
    EXPECT_FALSE(noOp.committed);
    EXPECT_TRUE(noOp.diagnostics.empty());
    EXPECT_EQ(session.currentRevision(), revision);
    ASSERT_TRUE(session.undo().has_value());
    ASSERT_TRUE(session.redo().has_value());
    const auto requestAfterEdit = session.materializationRequest(301u);
    ASSERT_TRUE(requestAfterEdit.has_value());
    const auto materialized = SctDocumentMaterializer::materialize(*requestAfterEdit);
    ASSERT_TRUE(materialized.succeeded());
    const auto index = SctDocumentIndex::build(*materialized.document);
    const auto* materializedInstruction = index.find(
        *materialized.document, site.instruction);
    ASSERT_NE(materializedInstruction, nullptr);
    const auto materializedParameter = std::ranges::find(
        materializedInstruction->fixedParameters, site.parameter.schemaIndex,
        &SctDocumentParameter::schemaIndex);
    ASSERT_NE(materializedParameter, materializedInstruction->fixedParameters.end());
    EXPECT_EQ(encodeSctCanonicalExpressionWords(
        std::get<SctCanonicalExpression>(materializedParameter->value)),
        encodeSctCanonicalExpressionWords(
            SctExpressionFactory::encodedDecimalLiteral(2, 128)));
}

TEST(SctParameterEditing, RepeatedGroupLifecycleUsesExactReversibleChanges) {
    auto document = makeScriptDocument();
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 42u;
    request.repeatedGroupCount = 1u;
    request.parameterOverrides = {
        {{0u, std::nullopt}, SctExpressionFactory::encodedDecimalLiteral(0)},
        {{2u, 0u}, SctExpressionFactory::encodedDecimalLiteral(1)},
    };
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    ASSERT_TRUE(SctDocumentValidator::validateDocument(document).validDocument);

    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));
    auto groupDraft = SctInstructionFactory::createRepeatedGroupDraft(42u, {
        {2u, SctExpressionFactory::encodedDecimalLiteral(7)}});
    ASSERT_TRUE(groupDraft.draft.has_value());
    const auto group = SctInstructionFactory::materializeRepeatedGroup(*groupDraft.draft);
    ASSERT_TRUE(group.group.has_value());
    const auto inserted = session.insertRepeatedGroup(
        built.instruction->id, 1u, *group.group);
    ASSERT_TRUE(inserted.committed);
    ASSERT_EQ(inserted.changes.repeatedGroups.size(), 1u);
    EXPECT_FALSE(inserted.changes.repeatedGroups.front().beforeOrdinal.has_value());
    EXPECT_EQ(inserted.changes.repeatedGroups.front().afterOrdinal, 1u);
    ASSERT_EQ(session.workingState().instruction(built.instruction->id)
        ->repeatedParameterGroups.size(), 2u);

    const auto moved = session.moveRepeatedGroup(built.instruction->id, 1u,
        SctRepeatedGroupMoveDirection::Up);
    ASSERT_TRUE(moved.committed);
    EXPECT_EQ(moved.changes.repeatedGroups.front().beforeOrdinal, 1u);
    EXPECT_EQ(moved.changes.repeatedGroups.front().afterOrdinal, 0u);
    ASSERT_TRUE(session.undo().has_value());
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingState().instruction(built.instruction->id)
        ->repeatedParameterGroups.size(), 1u);
    const auto minimum = session.deleteRepeatedGroup(built.instruction->id, 0u);
    EXPECT_FALSE(minimum.committed);
    EXPECT_TRUE(hasCode(minimum, "RepeatedGroupMinimum"));
}

TEST(SctParameterEditing, RepeatedGroupEditsKeepUnboundReferenceOriginsAtTheirValues) {
    auto document = makeScriptDocument();
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 42u;
    request.repeatedGroupCount = 2u;
    request.parameterOverrides = {
        {{0u, std::nullopt}, SctExpressionFactory::encodedDecimalLiteral(0)},
        {{2u, 0u}, SctExpressionFactory::encodedDecimalLiteral(1)},
        {{2u, 1u}, SctExpressionFactory::encodedDecimalLiteral(2)},
    };
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    ASSERT_TRUE(SctDocumentValidator::validateDocument(document).validDocument);

    const auto baseline = snapshotWith(loadedSnapshot(), std::move(document));
    const SctParameterSite site{built.instruction->id, {2u, 1u}};
    const SctUnboundReferenceOrigin origin{site, "scripts/source.sct",
        SctInstructionId{90u}, std::nullopt};
    SctEditSession session(baseline, baseline, {}, {}, std::span{&origin, 1u});
    auto groupDraft = SctInstructionFactory::createRepeatedGroupDraft(42u, {
        {2u, SctExpressionFactory::encodedDecimalLiteral(7)}});
    ASSERT_TRUE(groupDraft.draft.has_value());
    const auto group = SctInstructionFactory::materializeRepeatedGroup(*groupDraft.draft);
    ASSERT_TRUE(group.group.has_value());

    ASSERT_TRUE(session.insertRepeatedGroup(
        built.instruction->id, 0u, *group.group).committed);
    ASSERT_EQ(session.unboundReferences().size(), 1u);
    EXPECT_EQ(session.unboundReferences().front().site.parameter.repeatedGroupOrdinal, 2u);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.unboundReferences().front().site.parameter.repeatedGroupOrdinal, 1u);

    ASSERT_TRUE(session.moveRepeatedGroup(built.instruction->id, 1u,
        SctRepeatedGroupMoveDirection::Up).committed);
    EXPECT_EQ(session.unboundReferences().front().site.parameter.repeatedGroupOrdinal, 0u);
    ASSERT_TRUE(session.deleteRepeatedGroup(built.instruction->id, 0u).committed);
    EXPECT_TRUE(session.unboundReferences().empty());
    ASSERT_TRUE(session.undo().has_value());
    ASSERT_EQ(session.unboundReferences().size(), 1u);
    EXPECT_EQ(session.unboundReferences().front().site.parameter.repeatedGroupOrdinal, 0u);
}

TEST(SctParameterEditing, SharedFooterPlainTextUsesAtomicCopyOnWrite) {
    auto document = makeScriptDocument();
    const auto footer = document.allocateFooterEntryId();
    document.footerEntries.push_back({footer, SctTextKind::PlainString,
        SctPlainText{"shared"}});
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    const auto makeReference = [&]() {
        SctInstructionFactoryRequest request;
        request.opcode = 110u;
        request.parameterOverrides.push_back({{0u, std::nullopt},
            SctFooterEntryReference{footer}});
        const auto draft = SctInstructionFactory::createDraft(request);
        EXPECT_TRUE(draft.draft.has_value());
        const auto built = SctInstructionFactory::materialize(document, *draft.draft);
        EXPECT_TRUE(built.instruction.has_value());
        return *built.instruction;
    };
    const auto first = makeReference();
    const auto second = makeReference();
    instructions.insert(std::prev(instructions.end()), first);
    instructions.insert(std::prev(instructions.end()), second);
    ASSERT_TRUE(SctDocumentValidator::validateDocument(document).validDocument);

    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));
    const SctParameterSite site{first.id, {0u, std::nullopt}};
    const auto beforeFooterCount = session.workingState().footerEntryOrder().size();
    const auto edited = session.editReferencedFooterText(site, "private");
    ASSERT_TRUE(edited.committed);
    EXPECT_EQ(session.workingState().footerEntryOrder().size(), beforeFooterCount + 1u);
    const auto* parameter = session.workingState().parameter(site);
    ASSERT_NE(parameter, nullptr);
    const auto privateReference = std::get<SctFooterEntryReference>(parameter->value).target;
    EXPECT_NE(privateReference, footer);
    EXPECT_EQ(session.workingState().footerEntryOrder().back(), privateReference);
    EXPECT_EQ(std::get<SctPlainText>(
        session.workingState().footerEntry(privateReference)->value).utf8, "private");
    EXPECT_EQ(std::get<SctFooterEntryReference>(
        session.workingState().parameter({second.id, {0u, std::nullopt}})->value).target,
        footer);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingState().footerEntryOrder().size(), beforeFooterCount);
    EXPECT_EQ(std::get<SctFooterEntryReference>(
        session.workingState().parameter(site)->value).target, footer);
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_EQ(session.workingState().footerEntryOrder().back(), privateReference);
}

TEST(SctParameterAuthoring, EnforcesConfirmedMasksAndExactConventionalScptDomains) {
    SctDocumentInstruction variableWord;
    variableWord.id = SctInstructionId(1);
    variableWord.opcode = 5u;
    variableWord.fixedParameters = {
        {0u, SctEncodedWordValue{0x10000001u}},
        {1u, SctExpressionFactory::encodedDecimalLiteral(1)},
    };

    const SctParameterSite wordSite{variableWord.id, {0u, std::nullopt}};
    const auto badMask = SctParameterAuthoringService::parseInline(
        variableWord, wordSite, "0x00000001");
    EXPECT_FALSE(badMask.succeeded());
    EXPECT_FALSE(badMask.error.empty());
    const auto goodMask = SctParameterAuthoringService::parseInline(
        variableWord, wordSite, "0x10001234");
    ASSERT_TRUE(goodMask.succeeded());
    EXPECT_EQ(std::get<SctEncodedWordValue>(*goodMask.value).value, 0x10001234u);

    const SctParameterSite expressionSite{variableWord.id, {1u, std::nullopt}};
    const auto exact = SctParameterAuthoringService::parseInline(
        variableWord, expressionSite, "-12.5");
    ASSERT_TRUE(exact.succeeded());
    EXPECT_EQ(encodeSctCanonicalExpressionWords(
        std::get<SctCanonicalExpression>(*exact.value)),
        encodeSctCanonicalExpressionWords(
            SctExpressionFactory::scaledDecimalLiteral(-3200).expression.value()));
    EXPECT_FALSE(SctParameterAuthoringService::parseInline(
        variableWord, expressionSite, "0.1").succeeded());
    EXPECT_FALSE(SctParameterAuthoringService::parseInline(
        variableWord, expressionSite, "32768").succeeded());
}

TEST(SctParameterEditing, RejectsStaleAndWrongVariantTargetsWithoutHistory) {
    auto document = makeScriptDocument();
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 16u;
    request.parameterOverrides.push_back({{0u, std::nullopt},
        SctExpressionFactory::encodedDecimalLiteral(1)});
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));
    const auto revision = session.currentRevision();

    const auto wrongKind = session.replaceParameterValue(
        {built.instruction->id, {0u, std::nullopt}}, SctEncodedWordValue{1u});
    EXPECT_FALSE(wrongKind.committed);
    EXPECT_TRUE(hasCode(wrongKind, "ParameterValueKindMismatch"));
    const auto stale = session.replaceParameterValue(
        {built.instruction->id, {0u, 3u}},
        SctExpressionFactory::encodedDecimalLiteral(2));
    EXPECT_FALSE(stale.committed);
    EXPECT_TRUE(hasCode(stale, "ParameterTargetStale"));
    EXPECT_EQ(session.currentRevision(), revision);
    EXPECT_FALSE(session.canUndo());
}

TEST(SctParameterAuthoring, PreservesVariableKindsAndConstrainsSimpleChoices) {
    SctDocumentInstruction instruction;
    instruction.id = SctInstructionId(1);
    instruction.opcode = 16u;
    instruction.fixedParameters = {
        {0u, SctExpressionFactory::integerVariable(8).expression.value()},
    };
    const SctParameterSite site{instruction.id, {0u, std::nullopt}};
    const auto variable = SctParameterAuthoringService::parseInline(
        instruction, site, "IntVar[1193046]");
    ASSERT_TRUE(variable.succeeded());
    const auto& variableExpression = std::get<SctCanonicalExpression>(*variable.value);
    const auto& variableOperation = std::get<SctScptValueOperation>(
        std::get<SctTypedScptProgram>(variableExpression.body).operations.front());
    EXPECT_EQ(variableOperation.kind, SctScptValueKind::IntegerVariable);
    EXPECT_EQ(variableOperation.encodingWord & 0x00ffffffu, 0x123456u);
    EXPECT_FALSE(SctParameterAuthoringService::parseInline(
        instruction, site, "0x1000000").succeeded());

    instruction.fixedParameters.front().value =
        SctExpressionFactory::secondaryValue(SctExpressionSecondaryValue::Gold);
    const auto secondary = SctParameterAuthoringService::parseInline(
        instruction, site, "VyseLevel");
    ASSERT_TRUE(secondary.succeeded());
    const auto& secondaryOperation = std::get<SctScptValueOperation>(
        std::get<SctTypedScptProgram>(
            std::get<SctCanonicalExpression>(*secondary.value).body).operations.front());
    EXPECT_EQ(secondaryOperation.kind, SctScptValueKind::SecondaryValue);
    EXPECT_EQ(secondaryOperation.encodingWord & 0x00ffffffu, 0x4au);
    EXPECT_FALSE(SctParameterAuthoringService::parseInline(
        instruction, site, "invented state").succeeded());
}

TEST(SctParameterAuthoring, ReplacesOpaqueSequenceOnlyThroughSchemaTypedParsing) {
    SctDocumentInstruction instruction;
    instruction.id = SctInstructionId(1);
    instruction.opcode = 9u;
    instruction.fixedParameters = {
        {0u, SctOpaqueParameterValue{{0xaaaaaaaau}}},
    };
    const SctParameterSite site{instruction.id, {0u, std::nullopt}};
    const auto parsed = SctParameterAuthoringService::parseInline(
        instruction, site, "1 0x00000002");
    ASSERT_TRUE(parsed.succeeded());
    const auto& sequence = std::get<SctTerminatedWordSequenceValue>(*parsed.value);
    EXPECT_EQ(sequence.words,
        (std::vector<std::uint32_t>{1u, 2u, 0x1du}));
}

TEST(SctParameterAuthoring, ReferenceCandidatesHonorStorageAndTextKind) {
    auto document = makeScriptDocument();
    const auto sctString = document.allocateStringId();
    document.sections.push_back({document.allocateSectionId(), "MS0000001",
        SctStringSectionContent{SctDocumentString{sctString,
            SctMessage{std::nullopt, {}}, SctTextKind::SctString}}});
    const auto plainString = document.allocateStringId();
    document.sections.push_back({document.allocateSectionId(), "PLAIN",
        SctStringSectionContent{SctDocumentString{plainString,
            SctPlainText{"plain"}, SctTextKind::PlainString}}});
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 144u;
    request.parameterOverrides.push_back({{0u, std::nullopt},
        SctStringReference{sctString}});
    request.parameterOverrides.push_back({{1u, std::nullopt},
        SctExpressionFactory::encodedDecimalLiteral(0)});
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);

    SctWorkingState state(std::make_shared<const SctDocument>(std::move(document)));
    const SctParameterSite site{built.instruction->id, {0u, std::nullopt}};
    const auto candidates = SctParameterAuthoringService::referenceCandidates(state, site);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(std::get<SctStringId>(candidates.front().target), sctString);
    EXPECT_NE(candidates.front().label.find("MS0000001"), std::string::npos);

    const auto presentation = SctParameterAuthoringService::project(
        state, built.instruction->id);
    ASSERT_EQ(presentation.fixedParameters.size(), 2u);
    EXPECT_NE(presentation.fixedParameters.front().value.find("MS0000001"),
        std::string::npos);
    EXPECT_EQ(presentation.fixedParameters.front().navigation,
        (SctNavigationTarget{SctNavigationKind::String, sctString.value()}));
}

TEST(SctInstructionDraft, CreatesOwnedFooterTextAndInstructionAtomically) {
    SctEditSession session(loadedSnapshot());
    auto created = session.createInstructionDraft(110u);
    ASSERT_TRUE(created.draft.has_value());
    ASSERT_EQ(created.draft->ownedFooterText.size(), 1u);
    EXPECT_EQ(created.draft->ownedFooterText.front().kind, SctTextKind::PlainString);
    created.draft->ownedFooterText.front().value = SctPlainText{"owned text"};
    const auto section = session.workingState().sectionOrder().front();
    const auto anchor = session.workingState().instructionOrder(section).front();
    const auto beforeFooter = session.workingState().footerEntryOrder().size();

    const auto inserted = session.createInstructionAfter(anchor, *created.draft);
    ASSERT_TRUE(inserted.committed) << (inserted.diagnostics.empty()
        ? std::string{} : inserted.diagnostics.front().message);
    ASSERT_EQ(inserted.changes.instructions.size(), 1u);
    ASSERT_EQ(inserted.changes.footerEntries.size(), 1u);
    const auto instruction = inserted.changes.instructions.front().instruction;
    const auto* value = session.workingState().parameter({instruction, {0u, std::nullopt}});
    ASSERT_NE(value, nullptr);
    const auto footer = std::get<SctFooterEntryReference>(value->value).target;
    ASSERT_EQ(session.workingState().footerEntryOrder().size(), beforeFooter + 1u);
    EXPECT_EQ(std::get<SctPlainText>(session.workingState().footerEntry(footer)->value).utf8,
        "owned text");

    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingState().instruction(instruction), nullptr);
    EXPECT_EQ(session.workingState().footerEntry(footer), nullptr);
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_NE(session.workingState().instruction(instruction), nullptr);
    EXPECT_NE(session.workingState().footerEntry(footer), nullptr);
}

TEST(SctInstructionDraft, CreatesDefaultOwnedFooterMessageWhenRequired) {
    SctEditSession session(loadedSnapshot());
    auto created = session.createInstructionDraft(24u);
    ASSERT_TRUE(created.draft.has_value());
    ASSERT_EQ(created.draft->ownedFooterText.size(), 1u);
    EXPECT_EQ(created.draft->ownedFooterText.front().kind, SctTextKind::SctString);
    EXPECT_TRUE(std::holds_alternative<SctMessage>(
        created.draft->ownedFooterText.front().value));

    const auto section = session.workingState().sectionOrder().front();
    const auto anchor = session.workingState().instructionOrder(section).front();
    const auto inserted = session.createInstructionAfter(anchor, *created.draft);
    ASSERT_TRUE(inserted.committed) << (inserted.diagnostics.empty()
        ? std::string{} : inserted.diagnostics.front().message);
    const auto instruction = inserted.changes.instructions.front().instruction;
    const auto* value = session.workingState().parameter(
        {instruction, {0u, std::nullopt}});
    ASSERT_NE(value, nullptr);
    const auto footer = std::get<SctFooterEntryReference>(value->value).target;
    const auto* entry = session.workingState().footerEntry(footer);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, SctTextKind::SctString);
    EXPECT_TRUE(std::holds_alternative<SctMessage>(entry->value));
}

TEST(SctInstructionDraft, RejectsStaleAndUnresolvedDraftsWithoutPartialCreation) {
    SctEditSession session(loadedSnapshot());
    auto draft = session.createInstructionDraft(110u);
    ASSERT_TRUE(draft.draft.has_value());
    const auto section = session.workingState().sectionOrder().front();
    const auto anchor = session.workingState().instructionOrder(section).front();
    const auto beforeFooter = session.workingState().footerEntryOrder().size();

    ASSERT_TRUE(session.insertInstructionAfter(anchor, 125u).committed);
    const auto stale = session.createInstructionAfter(anchor, *draft.draft);
    EXPECT_FALSE(stale.committed);
    EXPECT_TRUE(hasCode(stale, "InstructionDraftStale"));
    EXPECT_EQ(session.workingState().footerEntryOrder().size(), beforeFooter);

    auto unresolved = session.createInstructionDraft(110u);
    ASSERT_TRUE(unresolved.draft.has_value());
    unresolved.draft->ownedFooterText.clear();
    const auto rejected = session.createInstructionAfter(anchor, *unresolved.draft);
    EXPECT_FALSE(rejected.committed);
    EXPECT_TRUE(hasCode(rejected, "InstructionDraftUnresolved"));
    EXPECT_EQ(session.workingState().footerEntryOrder().size(), beforeFooter);
}

TEST(SctFooterOwnership, DeletesOnlyNewlyOrphanedUnprotectedPlainText) {
    auto document = makeScriptDocument();
    const auto footer = document.allocateFooterEntryId();
    document.footerEntries.push_back({footer, SctTextKind::PlainString,
        SctPlainText{"owned"}});
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 110u;
    request.parameterOverrides.push_back({{0u, std::nullopt},
        SctFooterEntryReference{footer}});
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));

    const auto removed = session.deleteInstruction(built.instruction->id);
    ASSERT_TRUE(removed.committed);
    EXPECT_EQ(session.workingState().footerEntry(footer), nullptr);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_NE(session.workingState().footerEntry(footer), nullptr);
    EXPECT_NE(session.workingState().instruction(built.instruction->id), nullptr);
}

TEST(SctFooterOwnership, RetargetingCleansOldTargetButPreservesImportedOrphans) {
    auto document = makeScriptDocument();
    const auto oldFooter = document.allocateFooterEntryId();
    const auto newFooter = document.allocateFooterEntryId();
    const auto importedOrphan = document.allocateFooterEntryId();
    document.footerEntries = {
        {oldFooter, SctTextKind::PlainString, SctPlainText{"old"}},
        {newFooter, SctTextKind::PlainString, SctPlainText{"new"}},
        {importedOrphan, SctTextKind::PlainString, SctPlainText{"orphan"}},
    };
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 110u;
    request.parameterOverrides.push_back({{0u, std::nullopt},
        SctFooterEntryReference{oldFooter}});
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));

    const auto changed = session.replaceParameterValue(
        {built.instruction->id, {0u, std::nullopt}},
        SctFooterEntryReference{newFooter});
    ASSERT_TRUE(changed.committed);
    EXPECT_EQ(session.workingState().footerEntry(oldFooter), nullptr);
    EXPECT_NE(session.workingState().footerEntry(newFooter), nullptr);
    EXPECT_NE(session.workingState().footerEntry(importedOrphan), nullptr);
}

TEST(SctFooterOwnership, OpaqueAnchorsProtectNewlyOrphanedPlainText) {
    auto document = makeScriptDocument();
    const auto footer = document.allocateFooterEntryId();
    document.footerEntries.push_back({footer, SctTextKind::PlainString,
        SctPlainText{"protected"}});
    auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    SctInstructionFactoryRequest request;
    request.opcode = 110u;
    request.parameterOverrides.push_back({{0u, std::nullopt},
        SctFooterEntryReference{footer}});
    const auto draft = SctInstructionFactory::createDraft(request);
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(document, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    instructions.insert(std::prev(instructions.end()), *built.instruction);
    SctOpaqueAttachment attachment;
    attachment.id = document.allocateOpaqueAttachmentId();
    attachment.bytes = {0xaa};
    attachment.anchor = footer;
    attachment.placement = SctOpaquePlacement::After;
    attachment.relocation = SctOpaqueRelocationSupport::Relocatable;
    document.opaqueAttachments.push_back(std::move(attachment));
    SctEditSession session(snapshotWith(loadedSnapshot(), std::move(document)));

    ASSERT_TRUE(session.deleteInstruction(built.instruction->id).committed);
    EXPECT_NE(session.workingState().footerEntry(footer), nullptr);
}
