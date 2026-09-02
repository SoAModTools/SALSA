#include "SalsaCore/Sct/SctEditSession.h"

#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctOpcodeMetadata.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <ranges>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

using EditClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsedMicroseconds(const EditClock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            EditClock::now() - start).count());
}

[[nodiscard]] SctPipelineDiagnostic editError(
    const AssetLocator& locator,
    std::string code,
    std::string message,
    std::optional<SctNavigationTarget> target = std::nullopt) {
    SctPipelineDiagnostic result;
    result.severity = DiagnosticSeverity::Error;
    result.stage = SctPipelineStage::Edit;
    result.code = std::move(code);
    result.message = std::move(message);
    result.locator = locator;
    result.target = target;
    return result;
}

[[nodiscard]] std::string opcodeName(const std::uint16_t opcode) {
    const auto* schema = spice::sct::findSctOpcodeSchema(opcode);
    if (schema != nullptr && !schema->semantic.mnemonic.empty())
        return std::string(schema->semantic.mnemonic);
    return "Opcode " + std::to_string(opcode);
}

[[nodiscard]] SctNavigationTarget navigationFor(const SctMessageTarget& target) {
    return std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            return SctNavigationTarget{SctNavigationKind::String, id.value()};
        } else {
            return SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
        }
    }, target);
}

[[nodiscard]] std::string messageEditDescription(const SctMessageEditKind kind) {
    switch (kind) {
    case SctMessageEditKind::Typing: return "Edit message text";
    case SctMessageEditKind::Deletion: return "Delete message text";
    case SctMessageEditKind::Paste: return "Paste message text";
    case SctMessageEditKind::Replacement: return "Replace message text";
    case SctMessageEditKind::Formatting: return "Format message text";
    case SctMessageEditKind::Header: return "Edit message header";
    case SctMessageEditKind::Completion: return "Change message completion";
    case SctMessageEditKind::Options: return "Change message options";
    }
    return "Edit message";
}

[[nodiscard]] std::vector<SctPipelineDiagnostic> profileDiagnostics(
    const AssetLocator& locator,
    const SctNavigationTarget target,
    const std::vector<SctMessageProfileIssue>& issues) {
    std::vector<SctPipelineDiagnostic> diagnostics;
    diagnostics.reserve(issues.size());
    for (const auto& source : issues) {
        auto diagnostic = editError(locator, "MessageOutsideAuthoringProfile",
            source.message, target);
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

[[nodiscard]] std::vector<SctPipelineDiagnostic> validationDiagnostics(
    const AssetLocator& locator,
    const spice::sct::SctDocumentValidationResult& validation) {
    std::vector<SctPipelineDiagnostic> result;
    result.reserve(validation.diagnostics.size());
    for (const auto& diagnostic : validation.diagnostics)
        result.push_back(convertSctDiagnostic(diagnostic, SctPipelineStage::Validation, locator));
    return result;
}

void appendChanges(SctEditChangeSet& target, const SctEditChangeSet& source) {
    target.instructions.insert(target.instructions.end(),
        source.instructions.begin(), source.instructions.end());
    target.modified.insert(target.modified.end(),
        source.modified.begin(), source.modified.end());
}

}  // namespace

SctEditSession::SctEditSession(std::shared_ptr<const SctDocumentSnapshot> initialSnapshot)
    : baselineSnapshot_(std::move(initialSnapshot)),
      history_(std::make_shared<const RevisionDelta>()),
      workingState_(baselineSnapshot_ != nullptr ? baselineSnapshot_->document : nullptr),
      materializedDocument_(baselineSnapshot_ != nullptr ? baselineSnapshot_->document : nullptr),
      currentSnapshot_(baselineSnapshot_) {
    assert(baselineSnapshot_ != nullptr);
    assert(baselineSnapshot_->document != nullptr);
    structurallyValid_ = baselineSnapshot_->readiness
        == spice::sct::SctDocumentReadiness::StructurallyValid;
    materializationCheckpoints_.push_back(
        {history_.currentRevision().id, baselineSnapshot_});
}

SctEditResult SctEditSession::insertInstructionAfter(
    const spice::sct::SctInstructionId anchorInstruction,
    const std::uint16_t opcode) {
    const auto preflightStart = EditClock::now();
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (!structurallyValid_) {
        return failure({ editError(locator, "DocumentNotStructurallyValid",
            "Instruction editing is unavailable until the document is structurally valid.") });
    }
    if (opcode == 9u) {
        return failure({ editError(locator, "LabelInsertionReserved",
            "LabelOrStringPrefix is created only as part of section creation.") });
    }
    if (!std::ranges::any_of(insertableOpcodes(), [opcode](const auto& choice) {
            return choice.opcode == opcode;
        })) {
        return failure({ editError(locator, "OpcodeRequiresParameters",
            "The opcode cannot be inserted until its required parameters can be authored.") });
    }

    const auto placement = workingState_.placement(anchorInstruction);
    const auto* anchor = workingState_.instruction(anchorInstruction);
    if (!placement.has_value() || anchor == nullptr) {
        return failure({ editError(locator, "InstructionNotFound",
            "The insertion anchor no longer exists.",
            SctNavigationTarget{ SctNavigationKind::Instruction, anchorInstruction.value() }) });
    }
    if (anchor->opcode == 12u) {
        return failure({ editError(locator, "InstructionInsertionAfterReturn",
            "An instruction cannot be inserted after Return.",
            SctNavigationTarget{ SctNavigationKind::Instruction, anchorInstruction.value() }) });
    }
    if (opcode == 12u && workingState_.instructionAfter(anchorInstruction).has_value()) {
        return failure({ editError(locator, "ReturnMustTerminateSection",
            "Return can only be inserted as the final instruction in a section.",
            SctNavigationTarget{ SctNavigationKind::Instruction, anchorInstruction.value() }) });
    }

    spice::sct::SctInstructionFactoryRequest request;
    request.opcode = opcode;
    const auto draft = spice::sct::SctInstructionFactory::createDraft(request);
    if (!draft.draft.has_value()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : draft.diagnostics)
            diagnostics.push_back(convertSctDiagnostic(diagnostic, SctPipelineStage::Edit, locator));
        return failure(std::move(diagnostics));
    }
    spice::sct::SctDocument factoryContext;
    const auto materialized = spice::sct::SctInstructionFactory::materialize(
        factoryContext, *draft.draft);
    if (!materialized.instruction.has_value()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : materialized.diagnostics)
            diagnostics.push_back(convertSctDiagnostic(diagnostic, SctPipelineStage::Edit, locator));
        return failure(std::move(diagnostics));
    }
    auto instruction = *materialized.instruction;
    instruction.id = spice::sct::SctInstructionId(
        workingState_.nextInstructionIdValue());
    const auto insertedId = instruction.id;
    const SctNavigationTarget inserted{ SctNavigationKind::Instruction, insertedId.value() };
    return commit(SctSemanticOperationBatch{{SctInsertInstructionAfterOperation{
            anchorInstruction, std::move(instruction)}}},
        "Insert " + opcodeName(opcode),
        SelectionHints{ SctNavigationTarget{ SctNavigationKind::Instruction,
            anchorInstruction.value() }, inserted },
        elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::deleteInstruction(
    const spice::sct::SctInstructionId instruction) {
    const auto preflightStart = EditClock::now();
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (!structurallyValid_) {
        return failure({ editError(locator, "DocumentNotStructurallyValid",
            "Instruction editing is unavailable until the document is structurally valid.") });
    }
    const auto placement = workingState_.placement(instruction);
    const auto* existing = workingState_.instruction(instruction);
    if (!placement.has_value() || existing == nullptr) {
        return failure({ editError(locator, "InstructionNotFound",
            "The instruction no longer exists.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    if (existing->opcode == 9u && !placement->after.has_value()) {
        return failure({ editError(locator, "ProtectedSectionLabel",
            "The initial LabelOrStringPrefix instruction belongs to the section and cannot be deleted.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }

    std::vector<SctPipelineDiagnostic> blockers;
    if (workingState_.incomingReferenceCount(instruction) != 0u) {
        blockers.push_back(editError(locator, "InstructionHasIncomingReference",
            "The instruction cannot be deleted while another instruction references it.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }));
    }
    for (const auto attachment : workingState_.opaqueAttachments(instruction)) {
        blockers.push_back(editError(locator, "InstructionHasOpaqueAttachment",
            "The instruction cannot be deleted while opaque source data is anchored to it.",
            SctNavigationTarget{ SctNavigationKind::OpaqueAttachment, attachment.value() }));
    }
    if (!blockers.empty()) return failure(std::move(blockers));

    SctNavigationTarget fallback{ SctNavigationKind::Section, placement->section.value() };
    if (const auto after = workingState_.instructionAfter(instruction); after.has_value())
        fallback = {SctNavigationKind::Instruction, after->value()};
    else if (placement->after.has_value())
        fallback = {SctNavigationKind::Instruction, placement->after->value()};
    const SctNavigationTarget removed{ SctNavigationKind::Instruction, instruction.value() };
    return commit(SctSemanticOperationBatch{{SctDeleteInstructionOperation{instruction}}},
        "Delete " + opcodeName(existing->opcode),
        SelectionHints{ removed, fallback }, elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::moveInstruction(
    const spice::sct::SctInstructionId instruction,
    const SctInstructionMoveDirection direction) {
    const auto preflightStart = EditClock::now();
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (!structurallyValid_) {
        return failure({ editError(locator, "DocumentNotStructurallyValid",
            "Instruction editing is unavailable until the document is structurally valid.") });
    }
    const auto placement = workingState_.placement(instruction);
    const auto* existing = workingState_.instruction(instruction);
    if (!placement.has_value() || existing == nullptr) {
        return failure({ editError(locator, "InstructionNotFound",
            "The instruction no longer exists.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }

    const auto previous = workingState_.instructionBefore(instruction);
    const auto next = workingState_.instructionAfter(instruction);
    const bool atBoundary = direction == SctInstructionMoveDirection::Up
        ? !previous.has_value() : !next.has_value();
    if (atBoundary) {
        return failure({ editError(locator, "InstructionMoveAtBoundary",
            "The instruction is already at that section boundary.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    const auto otherId = direction == SctInstructionMoveDirection::Up ? *previous : *next;
    const auto* otherInstruction = workingState_.instruction(otherId);
    assert(otherInstruction != nullptr);
    const auto otherOpcode = otherInstruction->opcode;
    if (existing->opcode == 9u || otherOpcode == 9u) {
        return failure({ editError(locator, "InstructionMoveAcrossLabel",
            "The section label cannot be moved or crossed.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    if (existing->opcode == 12u || otherOpcode == 12u) {
        return failure({ editError(locator, "InstructionMoveAcrossReturn",
            "Return must remain the final instruction and cannot be moved or crossed.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    const auto destinationAnchor = direction == SctInstructionMoveDirection::Up
        ? workingState_.instructionBefore(*previous)
        : next;
    if (!destinationAnchor.has_value()) {
        return failure({ editError(locator, "InstructionMoveAcrossLabel",
            "The section label cannot be moved or crossed.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    const SctNavigationTarget moved{ SctNavigationKind::Instruction, instruction.value() };
    return commit(SctSemanticOperationBatch{{SctRelocateInstructionAfterOperation{
            instruction, *destinationAnchor}}},
        "Move " + opcodeName(existing->opcode) + (direction == SctInstructionMoveDirection::Up ? " up" : " down"),
        SelectionHints{ moved, moved }, elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::replaceMessage(
    const SctMessageTarget& target,
    const SctMessageDraft& draft,
    const SctMessageEditKind editKind) {
    const auto preflightStart = EditClock::now();
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto navigation = navigationFor(target);
    if (!structurallyValid_) {
        return failure({editError(locator, "DocumentNotStructurallyValid",
            "Message editing is unavailable until the document is structurally valid.", navigation)});
    }

    const auto* currentMessage = workingState_.message(target);
    if (currentMessage == nullptr) {
        return failure({editError(locator, "MessageTargetNotFound",
            "The selected SCT message no longer exists or is not an SCT-message entity.", navigation)});
    }
    const auto currentProjection = SctMessageAuthoringProfile::project(*currentMessage);
    if (!currentProjection.supported())
        return failure(profileDiagnostics(locator, navigation, currentProjection.issues));

    const auto materialized = SctMessageAuthoringProfile::materialize(draft);
    if (!materialized.succeeded())
        return failure(profileDiagnostics(locator, navigation, materialized.issues));
    if (*currentProjection.draft == draft) {
        SctEditResult result;
        result.revision = history_.currentRevision().id;
        result.snapshot = currentSnapshot_;
        result.suggestedSelection = navigation;
        return result;
    }

    return commit(SctSemanticOperationBatch{{SctReplaceMessageOperation{
            target, *materialized.message}}},
        messageEditDescription(editKind),
        SelectionHints{navigation, navigation}, elapsedMicroseconds(preflightStart));
}

std::optional<SctEditResult> SctEditSession::undo() {
    const auto journalStart = EditClock::now();
    if (!history_.canUndo()) return std::nullopt;
    const auto source = history_.currentRevision();
    const auto application = workingState_.apply(source.state->inverse);
    if (!application.succeeded()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application.issues) {
            diagnostics.push_back(editError(
                baselineSnapshot_->provenance->source().descriptor.locator,
                problem.code, problem.message, problem.target));
        }
        return failure(std::move(diagnostics));
    }
    const auto navigation = history_.undo();
    assert(navigation.has_value());
    SctEditResult result;
    result.committed = true;
    result.revision = navigation->to;
    result.snapshot = currentSnapshot_;
    result.changes = source.state->reverseChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Undo,
        navigation->from, navigation->to, result.changes};
    result.suggestedSelection = source.state->selections.undoSelection;
    result.journalMicroseconds = elapsedMicroseconds(journalStart);
    return result;
}

std::optional<SctEditResult> SctEditSession::redo() {
    const auto journalStart = EditClock::now();
    const auto target = history_.redoTarget();
    if (!target.has_value()) return std::nullopt;
    const auto application = workingState_.apply(target->state->forward);
    if (!application.succeeded()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application.issues) {
            diagnostics.push_back(editError(
                baselineSnapshot_->provenance->source().descriptor.locator,
                problem.code, problem.message, problem.target));
        }
        return failure(std::move(diagnostics));
    }
    const auto navigation = history_.redo();
    assert(navigation.has_value());
    SctEditResult result;
    result.committed = true;
    result.revision = navigation->to;
    result.snapshot = currentSnapshot_;
    result.changes = target->state->forwardChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Redo,
        navigation->from, navigation->to, result.changes};
    result.suggestedSelection = target->state->selections.redoSelection;
    result.journalMicroseconds = elapsedMicroseconds(journalStart);
    return result;
}

std::shared_ptr<const SctDocumentSnapshot> SctEditSession::currentSnapshot() const noexcept {
    return currentSnapshot_;
}

std::shared_ptr<const SctDocumentSnapshot> SctEditSession::verifiedSnapshot() const noexcept {
    return currentSnapshot_;
}

RevisionId SctEditSession::workingRevision() const {
    return history_.currentRevision().id;
}

RevisionId SctEditSession::currentRevision() const {
    return history_.currentRevision().id;
}

bool SctEditSession::structurallyValid() const noexcept { return structurallyValid_; }
bool SctEditSession::canUndo() const noexcept { return history_.canUndo(); }
bool SctEditSession::canRedo() const noexcept { return history_.canRedo(); }
bool SctEditSession::isDirty() const noexcept { return history_.isDirty(); }

std::optional<std::string_view> SctEditSession::undoDescription() const noexcept {
    return history_.undoDescription();
}

std::optional<std::string_view> SctEditSession::redoDescription() const noexcept {
    return history_.redoDescription();
}

std::optional<std::shared_ptr<const spice::sct::SctDocument>>
SctEditSession::materializeRevision(const RevisionId target) const {
    if (!target.valid() || !history_.revision(target).has_value()) return std::nullopt;

    std::vector<RevisionId> replay;
    auto cursor = target;
    const MaterializationCheckpoint* checkpoint = nullptr;
    while (cursor.valid()) {
        const auto foundCheckpoint = std::ranges::find(
            materializationCheckpoints_, cursor, &MaterializationCheckpoint::revision);
        if (foundCheckpoint != materializationCheckpoints_.end()) {
            checkpoint = &*foundCheckpoint;
            break;
        }
        const auto revision = history_.revision(cursor);
        if (!revision.has_value()) return std::nullopt;
        replay.push_back(cursor);
        cursor = revision->state->parent;
    }
    if (checkpoint == nullptr) return std::nullopt;

    auto document = checkpoint->snapshot->document;
    for (auto revisionId = replay.rbegin(); revisionId != replay.rend(); ++revisionId) {
        const auto revision = history_.revision(*revisionId);
        if (!revision.has_value()) return std::nullopt;
        const auto application = SctSemanticOperationService::apply(
            *document, revision->state->forward);
        if (!application.succeeded()) return std::nullopt;
        document = application.document;
    }
    return document;
}

std::optional<SctMaterializationRequest> SctEditSession::materializationRequest(
    const std::uint64_t generation) const {
    const auto target = history_.currentRevision().id;
    std::vector<SctSemanticOperationBatch> reversedTail;
    auto cursor = target;
    const MaterializationCheckpoint* checkpoint = nullptr;
    while (cursor.valid()) {
        const auto foundCheckpoint = std::ranges::find(
            materializationCheckpoints_, cursor, &MaterializationCheckpoint::revision);
        if (foundCheckpoint != materializationCheckpoints_.end()) {
            checkpoint = &*foundCheckpoint;
            break;
        }
        const auto revision = history_.revision(cursor);
        if (!revision.has_value() || !revision->state->parent.valid()) return std::nullopt;
        reversedTail.push_back(revision->state->forward);
        cursor = revision->state->parent;
    }
    if (checkpoint == nullptr) return std::nullopt;
    std::ranges::reverse(reversedTail);
    return SctMaterializationRequest{
        generation,
        checkpoint->revision,
        target,
        checkpoint->snapshot->document,
        baselineSnapshot_->provenance->source().descriptor.locator,
        checkpoint->snapshot->provenance->importEvidence,
        std::move(reversedTail),
    };
}

bool SctEditSession::installVerifiedMaterialization(
    const SctMaterializationResult& result) {
    if (!result.succeeded() || !isActiveRevision(result.targetRevision)) return false;
    auto verifiedSnapshot = std::make_shared<SctDocumentSnapshot>(SctDocumentSnapshot{
        baselineSnapshot_->provenance,
        result.document,
        result.analysis,
        spice::sct::SctDocumentReadiness::StructurallyValid,
        {},
    });
    auto validationMessages = validationDiagnostics(
        baselineSnapshot_->provenance->source().descriptor.locator, result.validation);
    verifiedSnapshot->diagnostics = baselineSnapshot_->provenance->baselineDiagnostics;
    verifiedSnapshot->diagnostics.insert(verifiedSnapshot->diagnostics.end(),
        std::make_move_iterator(validationMessages.begin()),
        std::make_move_iterator(validationMessages.end()));
    materializationCheckpoints_.push_back({result.targetRevision, verifiedSnapshot});
    if (result.targetRevision == history_.currentRevision().id) {
        verifiedRevision_ = result.targetRevision;
        materializedDocument_ = result.document;
        currentSnapshot_ = std::move(verifiedSnapshot);
        structurallyValid_ = true;
    }
    pruneMaterializationCheckpoints();
    return true;
}

std::optional<SctEditResult> SctEditSession::rejectToVerifiedRevision(
    const RevisionId revision,
    std::vector<SctPipelineDiagnostic> diagnostics) {
    if (!isActiveRevision(revision)) return std::nullopt;
    const auto from = history_.currentRevision().id;
    SctEditChangeSet rollbackChanges;
    rejectedTail_.clear();
    auto cursor = from;
    while (cursor != revision) {
        const auto entry = history_.revision(cursor);
        if (!entry.has_value()) return std::nullopt;
        const auto application = workingState_.apply(entry->state->inverse);
        if (!application.succeeded()) return std::nullopt;
        appendChanges(rollbackChanges, entry->state->reverseChanges);
        rejectedTail_.push_back(*entry->state);
        cursor = entry->state->parent;
    }
    if (!history_.selectAncestorAndDiscardDescendants(revision).has_value())
        return std::nullopt;

    const auto checkpoint = std::ranges::find(
        materializationCheckpoints_, revision, &MaterializationCheckpoint::revision);
    if (checkpoint == materializationCheckpoints_.end()) return std::nullopt;
    currentSnapshot_ = checkpoint->snapshot;
    materializedDocument_ = currentSnapshot_->document;
    verifiedRevision_ = revision;
    structurallyValid_ = currentSnapshot_->readiness
        == spice::sct::SctDocumentReadiness::StructurallyValid;
    pruneMaterializationCheckpoints();

    SctEditResult result;
    result.committed = true;
    result.revision = revision;
    result.snapshot = currentSnapshot_;
    result.changes = rollbackChanges;
    result.transition = SctRevisionTransition{
        SctRevisionTransitionKind::VerificationRollback,
        from, revision, rollbackChanges, std::nullopt,
        SctRevisionVerification::Rejected};
    result.diagnostics = std::move(diagnostics);
    return result;
}

bool SctEditSession::isActiveRevision(const RevisionId revision) const {
    if (!revision.valid()) return false;
    auto cursor = history_.currentRevision().id;
    while (cursor.valid()) {
        if (cursor == revision) return true;
        const auto found = history_.revision(cursor);
        if (!found.has_value()) return false;
        cursor = found->state->parent;
    }
    return false;
}

const SctWorkingState& SctEditSession::workingState() const noexcept {
    return workingState_;
}

const std::vector<SctInsertableOpcode>& SctEditSession::insertableOpcodes() {
    static const auto choices = [] {
        std::vector<SctInsertableOpcode> result;
        for (const auto& schema : spice::sct::sctOpcodeSchemas()) {
            if (schema.opcode == 9u) continue;
            spice::sct::SctInstructionFactoryRequest request;
            request.opcode = schema.opcode;
            const auto draft = spice::sct::SctInstructionFactory::createDraft(request);
            if (!draft.draft.has_value()) continue;
            if (!std::ranges::all_of(draft.draft->parameters, [](const auto& parameter) {
                    return parameter.value.has_value();
                })) continue;
            result.push_back({ schema.opcode,
                schema.semantic.mnemonic.empty()
                    ? "Opcode " + std::to_string(schema.opcode)
                    : std::string(schema.semantic.mnemonic) });
        }
        return result;
    }();
    return choices;
}

SctEditResult SctEditSession::failure(
    std::vector<SctPipelineDiagnostic> diagnostics) const {
    SctEditResult result;
    result.revision = history_.currentRevision().id;
    result.snapshot = currentSnapshot_;
    result.diagnostics = std::move(diagnostics);
    return result;
}

SctEditResult SctEditSession::commit(
    SctSemanticOperationBatch operation,
    std::string description,
    SelectionHints selections,
    const std::uint64_t preflightMicroseconds) {
    const auto journalStart = EditClock::now();
    auto application = workingState_.apply(operation);
    if (!application.succeeded()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application.issues) {
            diagnostics.push_back(editError(
                baselineSnapshot_->provenance->source().descriptor.locator,
                problem.code, problem.message, problem.target));
        }
        return failure(std::move(diagnostics));
    }

    const auto parent = history_.currentRevision().id;
    auto delta = std::make_shared<RevisionDelta>();
    delta->parent = parent;
    delta->forward = std::move(operation);
    delta->inverse = std::move(application.inverse);
    delta->forwardChanges = application.forwardChanges;
    delta->reverseChanges = application.reverseChanges;
    delta->selections = selections;
    const auto committed = history_.commit(std::move(delta), std::move(description));
    assert(committed.created);
    pruneMaterializationCheckpoints();

    SctEditResult result;
    result.committed = true;
    result.revision = committed.revision;
    result.snapshot = currentSnapshot_;
    result.changes = application.forwardChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Commit,
        parent, committed.revision, result.changes};
    result.suggestedSelection = selections.redoSelection;
    result.preflightMicroseconds = preflightMicroseconds;
    result.journalMicroseconds = elapsedMicroseconds(journalStart);
    return result;
}

void SctEditSession::pruneMaterializationCheckpoints() {
    std::erase_if(materializationCheckpoints_, [this](const auto& checkpoint) {
        return !history_.revision(checkpoint.revision).has_value();
    });
}

}  // namespace salsa::core
