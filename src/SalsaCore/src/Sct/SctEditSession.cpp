#include "SalsaCore/Sct/SctEditSession.h"

#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctOpcodeMetadata.h"

#include <algorithm>
#include <cassert>
#include <ranges>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

[[nodiscard]] DiagnosticSeverity severityOf(const spice::sct::SctDiagnosticSeverity severity) {
    switch (severity) {
    case spice::sct::SctDiagnosticSeverity::Info: return DiagnosticSeverity::Info;
    case spice::sct::SctDiagnosticSeverity::Warning: return DiagnosticSeverity::Warning;
    case spice::sct::SctDiagnosticSeverity::Error: return DiagnosticSeverity::Error;
    }
    return DiagnosticSeverity::Error;
}

[[nodiscard]] std::string diagnosticCodeName(const spice::sct::SctDiagnosticCode code) {
    using enum spice::sct::SctDiagnosticCode;
    switch (code) {
    case ParseFailed: return "ParseFailed";
    case UnsafePhysicalStructure: return "UnsafePhysicalStructure";
    case OverlappingSourceClaims: return "OverlappingSourceClaims";
    case UnresolvedReference: return "UnresolvedReference";
    case RepeatedCountMismatch: return "RepeatedCountMismatch";
    case AmbiguousExpression: return "AmbiguousExpression";
    case AmbiguousString: return "AmbiguousString";
    case InvalidId: return "InvalidId";
    case DuplicateId: return "DuplicateId";
    case AllocatorDiscontinuity: return "AllocatorDiscontinuity";
    case InvalidName: return "InvalidName";
    case InvalidContent: return "InvalidContent";
    case OpcodeUnavailable: return "OpcodeUnavailable";
    case ParameterMismatch: return "ParameterMismatch";
    case ExpressionInvalid: return "ExpressionInvalid";
    case AttachmentInvalid: return "AttachmentInvalid";
    case OpaquePlatformUnverified: return "OpaquePlatformUnverified";
    case LayoutOverflow: return "LayoutOverflow";
    case EncodingUnsupported: return "EncodingUnsupported";
    case RelocationOutOfRange: return "RelocationOutOfRange";
    case OpaquePlacementUnsatisfied: return "OpaquePlacementUnsatisfied";
    case CompressionFailed: return "CompressionFailed";
    case ProvisionalAuthoringDefault: return "ProvisionalAuthoringDefault";
    case ProvisionalOpcodeConstraint: return "ProvisionalOpcodeConstraint";
    case TextInvalid: return "TextInvalid";
    case HeaderUnavailable: return "HeaderUnavailable";
    }
    return "UnknownSctDiagnostic";
}

[[nodiscard]] std::optional<SctNavigationTarget> navigationFor(
    const std::optional<spice::sct::SctDocumentEntityId>& entity) {
    if (!entity.has_value()) return std::nullopt;
    return std::visit([](const auto& id) -> std::optional<SctNavigationTarget> {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, std::monostate>) return std::nullopt;
        else if constexpr (std::is_same_v<T, spice::sct::SctSectionId>)
            return SctNavigationTarget{ SctNavigationKind::Section, id.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return SctNavigationTarget{ SctNavigationKind::Instruction, id.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return SctNavigationTarget{ SctNavigationKind::String, id.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryId>)
            return SctNavigationTarget{ SctNavigationKind::FooterEntry, id.value() };
        else
            return SctNavigationTarget{ SctNavigationKind::OpaqueAttachment, id.value() };
    }, *entity);
}

[[nodiscard]] SctPipelineDiagnostic convertDiagnostic(
    const AssetLocator& locator,
    const SctPipelineStage stage,
    const spice::sct::SctDocumentDiagnostic& source) {
    SctPipelineDiagnostic converted;
    converted.severity = severityOf(source.severity);
    converted.stage = stage;
    converted.code = diagnosticCodeName(source.code);
    converted.message = source.message;
    converted.locator = locator;
    converted.target = navigationFor(source.entity);
    if (source.parameter.has_value()) {
        converted.schemaIndex = source.parameter->schemaIndex;
        converted.repeatedGroupOrdinal = source.parameter->repeatedGroupOrdinal;
    }
    converted.expressionChildPath = source.expressionChildPath;
    if (source.textRange.has_value()) {
        converted.textOffset = source.textRange->offset;
        converted.textSize = source.textRange->size;
    } else if (source.textLocation.has_value()) {
        converted.textOffset = source.textLocation->utf8Range.offset;
        converted.textSize = source.textLocation->utf8Range.size;
    }
    return converted;
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
        if (source.elementOrdinal.has_value())
            diagnostic.textOffset = static_cast<std::uint32_t>(*source.elementOrdinal);
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
        result.push_back(convertDiagnostic(locator, SctPipelineStage::Validation, diagnostic));
    return result;
}

}  // namespace

SctEditSession::SctEditSession(std::shared_ptr<const SctDocumentSnapshot> initialSnapshot)
    : baselineSnapshot_(std::move(initialSnapshot)),
      history_(std::make_shared<const RevisionDelta>()),
      materializedDocument_(baselineSnapshot_ != nullptr ? baselineSnapshot_->document : nullptr),
      currentSnapshot_(baselineSnapshot_) {
    assert(baselineSnapshot_ != nullptr);
    assert(baselineSnapshot_->document != nullptr);
    const auto validation = spice::sct::SctDocumentValidator::validateDocument(
        *materializedDocument_);
    rebuildSnapshot(materializedDocument_, validation);
    materializationCheckpoints_.push_back(
        {history_.currentRevision().id, materializedDocument_});
}

SctEditResult SctEditSession::insertInstructionAfter(
    const spice::sct::SctInstructionId anchorInstruction,
    const std::uint16_t opcode) {
    const auto& locator = baselineSnapshot_->source.descriptor.locator;
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

    const auto index = spice::sct::SctDocumentIndex::build(*materializedDocument_);
    const auto location = index.instructionLocation(anchorInstruction);
    const auto* anchor = index.find(anchorInstruction);
    if (!location.has_value() || anchor == nullptr) {
        return failure({ editError(locator, "InstructionNotFound",
            "The insertion anchor no longer exists.",
            SctNavigationTarget{ SctNavigationKind::Instruction, anchorInstruction.value() }) });
    }
    const auto* currentSection = index.find(location->sectionId);
    const auto* currentScript = currentSection == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&currentSection->content);
    if (currentScript == nullptr || location->instructionOrdinal >= currentScript->instructions.size()) {
        return failure({ editError(locator, "SectionIsNotScript",
            "Instructions can only be inserted relative to an instruction in a script section.",
            SctNavigationTarget{ SctNavigationKind::Instruction, anchorInstruction.value() }) });
    }
    if (anchor->opcode == 12u) {
        return failure({ editError(locator, "InstructionInsertionAfterReturn",
            "An instruction cannot be inserted after Return.",
            SctNavigationTarget{ SctNavigationKind::Instruction, anchorInstruction.value() }) });
    }
    const auto ordinal = location->instructionOrdinal + 1u;
    if (opcode == 12u && ordinal != currentScript->instructions.size()) {
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
            diagnostics.push_back(convertDiagnostic(locator, SctPipelineStage::Edit, diagnostic));
        return failure(std::move(diagnostics));
    }
    spice::sct::SctDocument factoryContext;
    while (factoryContext.nextInstructionIdValue()
        < materializedDocument_->nextInstructionIdValue()) {
        (void)factoryContext.allocateInstructionId();
    }
    const auto materialized = spice::sct::SctInstructionFactory::materialize(
        factoryContext, *draft.draft);
    if (!materialized.instruction.has_value()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : materialized.diagnostics)
            diagnostics.push_back(convertDiagnostic(locator, SctPipelineStage::Edit, diagnostic));
        return failure(std::move(diagnostics));
    }
    const auto insertedId = materialized.instruction->id;
    const SctNavigationTarget inserted{ SctNavigationKind::Instruction, insertedId.value() };
    return commit(SctSemanticOperationBatch{{SctInsertInstructionAfterOperation{
            anchorInstruction, *materialized.instruction}}},
        "Insert " + opcodeName(opcode),
        SelectionHints{ SctNavigationTarget{ SctNavigationKind::Instruction,
            anchorInstruction.value() }, inserted });
}

SctEditResult SctEditSession::deleteInstruction(
    const spice::sct::SctInstructionId instruction) {
    const auto& locator = baselineSnapshot_->source.descriptor.locator;
    if (!structurallyValid_) {
        return failure({ editError(locator, "DocumentNotStructurallyValid",
            "Instruction editing is unavailable until the document is structurally valid.") });
    }
    const auto index = spice::sct::SctDocumentIndex::build(*materializedDocument_);
    const auto location = index.instructionLocation(instruction);
    const auto* existing = index.find(instruction);
    if (!location.has_value() || existing == nullptr) {
        return failure({ editError(locator, "InstructionNotFound",
            "The instruction no longer exists.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    if (existing->opcode == 9u && location->instructionOrdinal == 0u) {
        return failure({ editError(locator, "ProtectedSectionLabel",
            "The initial LabelOrStringPrefix instruction belongs to the section and cannot be deleted.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }

    std::vector<SctPipelineDiagnostic> blockers;
    for (const auto& reference : index.inboundReferences(
            spice::sct::SctDocumentReferenceTarget{ instruction })) {
        auto diagnostic = editError(locator, "InstructionHasIncomingReference",
            "The instruction cannot be deleted while another instruction references it.",
            SctNavigationTarget{ SctNavigationKind::Instruction, reference.sourceInstruction.value() });
        diagnostic.schemaIndex = reference.parameter.schemaIndex;
        diagnostic.repeatedGroupOrdinal = reference.parameter.repeatedGroupOrdinal;
        blockers.push_back(std::move(diagnostic));
    }
    const auto attachments = index.attachmentsFor(spice::sct::SctOpaqueAnchor{ instruction });
    for (const auto* attachment : attachments) {
        blockers.push_back(editError(locator, "InstructionHasOpaqueAttachment",
            "The instruction cannot be deleted while opaque source data is anchored to it.",
            SctNavigationTarget{ SctNavigationKind::OpaqueAttachment, attachment->id.value() }));
    }
    if (!blockers.empty()) return failure(std::move(blockers));

    const auto* currentSection = index.find(location->sectionId);
    const auto* currentScript = currentSection == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&currentSection->content);
    assert(currentScript != nullptr);
    SctNavigationTarget fallback{ SctNavigationKind::Section, location->sectionId.value() };
    if (location->instructionOrdinal + 1u < currentScript->instructions.size()) {
        fallback = {SctNavigationKind::Instruction,
            currentScript->instructions[location->instructionOrdinal + 1u].id.value()};
    } else if (location->instructionOrdinal > 0u) {
        fallback = {SctNavigationKind::Instruction,
            currentScript->instructions[location->instructionOrdinal - 1u].id.value()};
    }
    const SctNavigationTarget removed{ SctNavigationKind::Instruction, instruction.value() };
    return commit(SctSemanticOperationBatch{{SctDeleteInstructionOperation{instruction}}},
        "Delete " + opcodeName(existing->opcode),
        SelectionHints{ removed, fallback });
}

SctEditResult SctEditSession::moveInstruction(
    const spice::sct::SctInstructionId instruction,
    const SctInstructionMoveDirection direction) {
    const auto& locator = baselineSnapshot_->source.descriptor.locator;
    if (!structurallyValid_) {
        return failure({ editError(locator, "DocumentNotStructurallyValid",
            "Instruction editing is unavailable until the document is structurally valid.") });
    }
    const auto index = spice::sct::SctDocumentIndex::build(*materializedDocument_);
    const auto location = index.instructionLocation(instruction);
    const auto* existing = index.find(instruction);
    if (!location.has_value() || existing == nullptr) {
        return failure({ editError(locator, "InstructionNotFound",
            "The instruction no longer exists.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }

    const auto* section = index.find(location->sectionId);
    const auto* script = section == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&section->content);
    assert(script != nullptr);
    const auto ordinal = location->instructionOrdinal;
    const bool atBoundary = direction == SctInstructionMoveDirection::Up
        ? ordinal == 0 : ordinal + 1 >= script->instructions.size();
    if (atBoundary) {
        return failure({ editError(locator, "InstructionMoveAtBoundary",
            "The instruction is already at that section boundary.",
            SctNavigationTarget{ SctNavigationKind::Instruction, instruction.value() }) });
    }
    const auto other = direction == SctInstructionMoveDirection::Up ? ordinal - 1 : ordinal + 1;
    const auto otherOpcode = script->instructions[other].opcode;
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
        ? script->instructions[ordinal - 2u].id
        : script->instructions[ordinal + 1u].id;
    const SctNavigationTarget moved{ SctNavigationKind::Instruction, instruction.value() };
    return commit(SctSemanticOperationBatch{{SctRelocateInstructionAfterOperation{
            instruction, destinationAnchor}}},
        "Move " + opcodeName(existing->opcode) + (direction == SctInstructionMoveDirection::Up ? " up" : " down"),
        SelectionHints{ moved, moved });
}

SctEditResult SctEditSession::replaceMessage(
    const SctMessageTarget& target,
    const SctMessageDraft& draft,
    const SctMessageEditKind editKind) {
    const auto& locator = baselineSnapshot_->source.descriptor.locator;
    const auto navigation = navigationFor(target);
    if (!structurallyValid_) {
        return failure({editError(locator, "DocumentNotStructurallyValid",
            "Message editing is unavailable until the document is structurally valid.", navigation)});
    }

    const auto index = spice::sct::SctDocumentIndex::build(*materializedDocument_);
    const spice::sct::SctTextValue* currentValue = std::visit([&](const auto id)
        -> const spice::sct::SctTextValue* {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            const auto* string = index.find(id);
            return string != nullptr && string->kind == spice::sct::SctTextKind::SctString
                ? &string->value : nullptr;
        } else {
            const auto* entry = index.find(id);
            return entry != nullptr && entry->kind == spice::sct::SctTextKind::SctString
                ? &entry->value : nullptr;
        }
    }, target);
    if (currentValue == nullptr) {
        return failure({editError(locator, "MessageTargetNotFound",
            "The selected SCT message no longer exists or is not an SCT-message entity.", navigation)});
    }
    const auto* currentMessage = std::get_if<spice::sct::SctMessage>(currentValue);
    if (currentMessage == nullptr) {
        return failure({editError(locator, "MessageTargetNotSemantic",
            "The selected text is not an editable semantic SCT message.", navigation)});
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
        SelectionHints{navigation, navigation});
}

std::optional<SctEditResult> SctEditSession::undo() {
    if (!history_.canUndo()) return std::nullopt;
    const auto source = history_.currentRevision();
    const auto application = SctSemanticOperationService::apply(
        *materializedDocument_, source.state->inverse);
    if (!application.succeeded()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application.issues) {
            diagnostics.push_back(editError(
                baselineSnapshot_->source.descriptor.locator,
                problem.code, problem.message, problem.target));
        }
        return failure(std::move(diagnostics));
    }
    const auto validation = spice::sct::SctDocumentValidator::validateDocument(
        *application.document);
    auto diagnostics = validationDiagnostics(
        baselineSnapshot_->source.descriptor.locator, validation);
    if (!validation.validDocument) return failure(std::move(diagnostics));

    const auto navigation = history_.undo();
    assert(navigation.has_value());
    materializedDocument_ = application.document;
    rebuildSnapshot(materializedDocument_, validation);
    SctEditResult result;
    result.committed = true;
    result.revision = navigation->to;
    result.snapshot = currentSnapshot_;
    result.changes = source.state->reverseChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Undo,
        navigation->from, navigation->to, result.changes};
    result.suggestedSelection = source.state->selections.undoSelection;
    result.diagnostics = std::move(diagnostics);
    return result;
}

std::optional<SctEditResult> SctEditSession::redo() {
    const auto target = history_.redoTarget();
    if (!target.has_value()) return std::nullopt;
    const auto application = SctSemanticOperationService::apply(
        *materializedDocument_, target->state->forward);
    if (!application.succeeded()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application.issues) {
            diagnostics.push_back(editError(
                baselineSnapshot_->source.descriptor.locator,
                problem.code, problem.message, problem.target));
        }
        return failure(std::move(diagnostics));
    }
    const auto validation = spice::sct::SctDocumentValidator::validateDocument(
        *application.document);
    auto diagnostics = validationDiagnostics(
        baselineSnapshot_->source.descriptor.locator, validation);
    if (!validation.validDocument) return failure(std::move(diagnostics));

    const auto navigation = history_.redo();
    assert(navigation.has_value());
    materializedDocument_ = application.document;
    rebuildSnapshot(materializedDocument_, validation);
    SctEditResult result;
    result.committed = true;
    result.revision = navigation->to;
    result.snapshot = currentSnapshot_;
    result.changes = target->state->forwardChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Redo,
        navigation->from, navigation->to, result.changes};
    result.suggestedSelection = target->state->selections.redoSelection;
    result.diagnostics = std::move(diagnostics);
    return result;
}

std::shared_ptr<const SctDocumentSnapshot> SctEditSession::currentSnapshot() const noexcept {
    return currentSnapshot_;
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

    auto document = checkpoint->document;
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
    SelectionHints selections) {
    auto application = SctSemanticOperationService::apply(
        *materializedDocument_, operation);
    if (!application.succeeded()) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application.issues) {
            diagnostics.push_back(editError(
                baselineSnapshot_->source.descriptor.locator,
                problem.code, problem.message, problem.target));
        }
        return failure(std::move(diagnostics));
    }

    const auto validation = spice::sct::SctDocumentValidator::validateDocument(
        *application.document);
    auto diagnostics = validationDiagnostics(baselineSnapshot_->source.descriptor.locator, validation);
    if (!validation.validDocument) return failure(std::move(diagnostics));

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
    materializedDocument_ = std::move(application.document);
    rebuildSnapshot(materializedDocument_, validation);
    pruneMaterializationCheckpoints();
    ++commitsSinceMaterializationCheckpoint_;
    maybeAddMaterializationCheckpoint();

    SctEditResult result;
    result.committed = true;
    result.revision = committed.revision;
    result.snapshot = currentSnapshot_;
    result.changes = application.forwardChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Commit,
        parent, committed.revision, result.changes};
    result.suggestedSelection = selections.redoSelection;
    result.diagnostics = std::move(diagnostics);
    return result;
}

void SctEditSession::rebuildSnapshot(
    std::shared_ptr<const spice::sct::SctDocument> document,
    const spice::sct::SctDocumentValidationResult& validation) {
    structurallyValid_ = validation.validDocument;
    std::vector<SctPipelineDiagnostic> diagnostics;
    for (const auto& diagnostic : baselineSnapshot_->diagnostics) {
        if (diagnostic.stage != SctPipelineStage::Validation)
            diagnostics.push_back(diagnostic);
    }
    auto currentValidation = validationDiagnostics(
        baselineSnapshot_->source.descriptor.locator, validation);
    diagnostics.insert(diagnostics.end(),
        std::make_move_iterator(currentValidation.begin()),
        std::make_move_iterator(currentValidation.end()));
    currentSnapshot_ = std::make_shared<SctDocumentSnapshot>(SctDocumentSnapshot{
        baselineSnapshot_->source,
        baselineSnapshot_->inspection,
        baselineSnapshot_->textConvention,
        baselineSnapshot_->textSelectionOrigin,
        std::move(document),
        baselineSnapshot_->importReceipt,
        validation.validDocument ? spice::sct::SctDocumentReadiness::StructurallyValid
                                 : spice::sct::SctDocumentReadiness::Inspectable,
        std::move(diagnostics),
    });
}

void SctEditSession::pruneMaterializationCheckpoints() {
    std::erase_if(materializationCheckpoints_, [this](const auto& checkpoint) {
        return !history_.revision(checkpoint.revision).has_value();
    });
}

void SctEditSession::maybeAddMaterializationCheckpoint() {
    constexpr std::size_t MaterializationCheckpointInterval = 32;
    if (commitsSinceMaterializationCheckpoint_ < MaterializationCheckpointInterval) return;
    materializationCheckpoints_.push_back(
        {history_.currentRevision().id, materializedDocument_});
    commitsSinceMaterializationCheckpoint_ = 0;
}

}  // namespace salsa::core
