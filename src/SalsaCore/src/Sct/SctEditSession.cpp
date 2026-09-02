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
    target.structuredAuthoring.insert(target.structuredAuthoring.end(),
        source.structuredAuthoring.begin(), source.structuredAuthoring.end());
    target.invalidations = target.invalidations | source.invalidations;
    target.documentChanged = target.documentChanged || source.documentChanged;
}

[[nodiscard]] const spice_sct_prototype::SctStructuredRegion* verifiedRegion(
    const SctDocumentSnapshot& snapshot,
    const spice::sct::SctInstructionId controller) {
    if (!snapshot.structuredControlFlow) return nullptr;
    for (const auto& section : snapshot.structuredControlFlow->sections()) {
        const auto found = std::ranges::find_if(section.regions, [&](const auto& region) {
            return region.id.headerInstruction == controller
                && region.strength
                    == spice_sct_prototype::SctStructureClaimStrength::Verified;
        });
        if (found != section.regions.end()) return &*found;
    }
    return nullptr;
}

[[nodiscard]] bool hasCaseValue(
    const spice_sct_prototype::SctStructuredRegion& region,
    const SctStructuredAuthoringState& authoring,
    const spice::sct::SctInstructionId controller,
    const std::int32_t value,
    const std::optional<SctAuthoredArmId> excluding = std::nullopt) {
    for (const auto& arm : region.arms) {
        for (const auto& label : arm.caseLabels) {
            if (label.value == value) return true;
        }
    }
    return std::ranges::any_of(authoring.arms(), [&](const auto& arm) {
        return arm.controller.instruction == controller
            && (!excluding || arm.id != *excluding)
            && arm.kind == spice_sct_prototype::SctStructuredArmKind::SwitchCase
            && arm.caseValue == value;
    });
}

[[nodiscard]] std::optional<spice::sct::SctDocumentInstruction> makeInstruction(
    const std::uint16_t opcode,
    const std::uint64_t id) {
    spice::sct::SctInstructionFactoryRequest request;
    request.opcode = opcode;
    const auto draft = spice::sct::SctInstructionFactory::createDraft(request);
    if (!draft.draft) return std::nullopt;
    spice::sct::SctDocument context;
    const auto materialized = spice::sct::SctInstructionFactory::materialize(
        context, *draft.draft);
    if (!materialized.instruction) return std::nullopt;
    auto result = *materialized.instruction;
    result.id = spice::sct::SctInstructionId(id);
    return result;
}

[[nodiscard]] bool setInstructionReference(
    spice::sct::SctDocumentInstruction& instruction,
    const std::uint32_t schemaIndex,
    const spice::sct::SctInstructionId target) {
    const auto found = std::ranges::find(
        instruction.fixedParameters, schemaIndex,
        &spice::sct::SctDocumentParameter::schemaIndex);
    if (found == instruction.fixedParameters.end()) return false;
    found->value = spice::sct::SctInstructionReference{target};
    return true;
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
    rebuildSemanticProjection();
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
        {},
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
    return commit(SctSemanticOperationBatch{{SctDeleteInstructionOperation{instruction}}}, {},
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
        {},
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
        {},
        messageEditDescription(editKind),
        SelectionHints{navigation, navigation}, elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::addVirtualElse(
    const spice::sct::SctInstructionId controller) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* region = verifiedRegion(*currentSnapshot_, controller);
    if (region == nullptr
        || region->id.kind != spice_sct_prototype::SctStructuredRegionKind::If) {
        return failure({editError(locator, "ElseRequiresVerifiedIf",
            "An empty Else arm can be added only to a verified If without an Else.",
            SctNavigationTarget{SctNavigationKind::Instruction, controller.value()})});
    }
    if (!region->join) {
        return failure({editError(locator, "ElseRequiresConcreteJoin",
            "The If does not have a concrete join where an Else arm can be lowered.")});
    }
    if (std::ranges::any_of(structuredAuthoring_.arms(), [&](const auto& arm) {
            return arm.controller.instruction == controller
                && arm.kind == spice_sct_prototype::SctStructuredArmKind::Else;
        })) {
        return failure({editError(locator, "ElseAlreadyExists",
            "This If already has an authored Else arm.")});
    }
    const auto id = structuredAuthoring_.nextId();
    SctAuthoredArm arm{id,
        {region->id.section, controller},
        spice_sct_prototype::SctStructuredArmKind::Else};
    arm.expectedJoin = region->join->entryInstruction;
    return commit({}, {{SctSetAuthoredArmOperation{id, std::nullopt, arm}}},
        "Add empty Else",
        SelectionHints{{SctNavigationTarget{SctNavigationKind::Instruction,
            controller.value()}},
            {SctNavigationTarget{SctNavigationKind::Instruction, controller.value()}}});
}

SctEditResult SctEditSession::addVirtualCase(
    const spice::sct::SctInstructionId controller) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* region = verifiedRegion(*currentSnapshot_, controller);
    if (region == nullptr
        || region->id.kind != spice_sct_prototype::SctStructuredRegionKind::Switch) {
        return failure({editError(locator, "CaseRequiresVerifiedSwitch",
            "A case can be added only to a verified Switch.",
            SctNavigationTarget{SctNavigationKind::Instruction, controller.value()})});
    }
    if (!region->join) {
        return failure({editError(locator, "CaseRequiresConcreteJoin",
            "The Switch does not have a concrete join where a case can be lowered.")});
    }
    const auto id = structuredAuthoring_.nextId();
    SctAuthoredArm arm{id,
        {region->id.section, controller},
        spice_sct_prototype::SctStructuredArmKind::SwitchCase};
    arm.expectedJoin = region->join->entryInstruction;
    return commit({}, {{SctSetAuthoredArmOperation{id, std::nullopt, arm}}},
        "Add empty Switch case",
        SelectionHints{{SctNavigationTarget{SctNavigationKind::Instruction,
            controller.value()}},
            {SctNavigationTarget{SctNavigationKind::Instruction, controller.value()}}});
}

SctEditResult SctEditSession::setVirtualCaseValue(
    const SctAuthoredArmId id, const std::optional<std::int32_t> value) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* current = structuredAuthoring_.find(id);
    if (current == nullptr
        || current->kind != spice_sct_prototype::SctStructuredArmKind::SwitchCase
        || current->realization != SctAuthoredArmRealization::Virtual) {
        return failure({editError(locator, "VirtualCaseNotFound",
            "Only an unrealized authored case can change its value.")});
    }
    if (current->caseValue == value) return failure({});
    const auto* region = verifiedRegion(*currentSnapshot_, current->controller.instruction);
    if (region == nullptr) {
        return failure({editError(locator, "CaseControllerNotVerified",
            "The case controller is no longer a verified Switch.")});
    }
    if (value && hasCaseValue(*region, structuredAuthoring_,
            current->controller.instruction, *value, id)) {
        return failure({editError(locator, "DuplicateSwitchCaseValue",
            "The Switch already contains this signed case value.")});
    }
    auto next = *current;
    next.caseValue = value;
    return commit({}, {{SctSetAuthoredArmOperation{id, *current, next}}},
        "Set Switch case value", {});
}

SctEditResult SctEditSession::removeVirtualArm(const SctAuthoredArmId id) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* current = structuredAuthoring_.find(id);
    if (current == nullptr || current->realization != SctAuthoredArmRealization::Virtual) {
        return failure({editError(locator, "VirtualArmNotRemovable",
            "Only an empty unrealized Else or Case arm can be removed.")});
    }
    return commit({}, {{SctSetAuthoredArmOperation{id, *current, std::nullopt}}},
        current->kind == spice_sct_prototype::SctStructuredArmKind::Else
            ? "Remove empty Else" : "Remove empty Switch case", {});
}

SctEditResult SctEditSession::insertInstructionIntoAuthoredArm(
    const SctAuthoredArmId id, const std::uint16_t opcode) {
    const auto preflightStart = EditClock::now();
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* current = structuredAuthoring_.find(id);
    if (current == nullptr || current->realization != SctAuthoredArmRealization::Virtual) {
        return failure({editError(locator, "VirtualArmNotFound",
            "The selected semantic arm is no longer an empty virtual arm.")});
    }
    if (opcode == 9u || opcode == 12u
        || !std::ranges::any_of(insertableOpcodes(), [opcode](const auto& item) {
            return item.opcode == opcode;
        })) {
        return failure({editError(locator, "OpcodeUnavailableForSemanticArm",
            "This opcode cannot be inserted as the first instruction of a semantic arm.")});
    }
    if (current->kind == spice_sct_prototype::SctStructuredArmKind::SwitchCase
        && !current->caseValue) {
        return failure({editError(locator, "SwitchCaseValueRequired",
            "Choose a unique signed case value before inserting the first instruction.")});
    }
    const auto* region = verifiedRegion(*currentSnapshot_, current->controller.instruction);
    if (region == nullptr || !region->join || !current->expectedJoin
        || region->join->entryInstruction != *current->expectedJoin) {
        return failure({editError(locator, "SemanticArmContextChanged",
            "The verified controller or join changed before the arm could be realized.")});
    }
    const auto join = *current->expectedJoin;
    const auto beforeJoin = workingState_.instructionBefore(join);
    if (!beforeJoin) {
        return failure({editError(locator, "SemanticArmJoinUnavailable",
            "The semantic arm cannot be placed before its join.")});
    }
    auto child = makeInstruction(opcode, workingState_.nextInstructionIdValue());
    if (!child) {
        return failure({editError(locator, "OpcodeRequiresParameters",
            "The opcode cannot be inserted until its required parameters can be authored.")});
    }

    SctSemanticOperationBatch document;
    auto next = *current;
    next.realization = SctAuthoredArmRealization::Physical;
    next.members.push_back(child->id);
    std::uint64_t nextId = child->id.value() + 1u;

    if (current->kind == spice_sct_prototype::SctStructuredArmKind::Else) {
        auto anchor = *beforeJoin;
        const auto* preceding = workingState_.instruction(anchor);
        bool hasExit = false;
        if (preceding != nullptr && preceding->opcode == 10u) {
            const auto parameter = std::ranges::find(preceding->fixedParameters, 0u,
                &spice::sct::SctDocumentParameter::schemaIndex);
            hasExit = parameter != preceding->fixedParameters.end()
                && std::get_if<spice::sct::SctInstructionReference>(&parameter->value) != nullptr
                && std::get<spice::sct::SctInstructionReference>(parameter->value).target == join;
        }
        if (!hasExit) {
            spice::sct::SctDocumentInstruction exit{
                spice::sct::SctInstructionId(nextId++), 10u};
            exit.fixedParameters.push_back({0u,
                spice::sct::SctInstructionReference{join}});
            document.operations.push_back(
                SctInsertInstructionAfterOperation{anchor, exit});
            next.managedScaffolding.push_back(exit.id);
            anchor = exit.id;
        }
        document.operations.push_back(
            SctInsertInstructionAfterOperation{anchor, *child});
        auto controller = *workingState_.instruction(current->controller.instruction);
        if (!setInstructionReference(controller, 1u, child->id)) {
            return failure({editError(locator, "IfFalseTargetUnavailable",
                "The verified If does not expose its false-target parameter.")});
        }
        document.operations.push_back(SctReplaceInstructionOperation{
            controller.id, std::move(controller)});
    } else if (current->kind
            == spice_sct_prototype::SctStructuredArmKind::SwitchCase) {
        document.operations.push_back(
            SctInsertInstructionAfterOperation{*beforeJoin, *child});
        spice::sct::SctDocumentInstruction exit{
            spice::sct::SctInstructionId(nextId++), 10u};
        exit.fixedParameters.push_back({0u, spice::sct::SctInstructionReference{join}});
        document.operations.push_back(SctInsertInstructionAfterOperation{child->id, exit});
        next.managedScaffolding.push_back(exit.id);
        auto controller = *workingState_.instruction(current->controller.instruction);
        controller.repeatedParameterGroups.push_back({{{2u,
            spice::sct::SctEncodedWordValue{
                static_cast<std::uint32_t>(*current->caseValue)}},
            {3u, spice::sct::SctInstructionReference{child->id}}}});
        const auto count = std::ranges::find(controller.fixedParameters, 1u,
            &spice::sct::SctDocumentParameter::schemaIndex);
        if (count == controller.fixedParameters.end()) {
            return failure({editError(locator, "SwitchCaseCountUnavailable",
                "The verified Switch does not expose its repeated-group count.")});
        }
        count->value = spice::sct::SctEncodedWordValue{
            static_cast<std::uint32_t>(controller.repeatedParameterGroups.size())};
        document.operations.push_back(SctReplaceInstructionOperation{
            controller.id, std::move(controller)});
    } else {
        return failure({editError(locator, "SemanticArmKindUnsupported",
            "This semantic arm kind cannot be realized by this command.")});
    }

    const SctNavigationTarget inserted{SctNavigationKind::Instruction, child->id.value()};
    return commit(std::move(document),
        {{SctSetAuthoredArmOperation{id, *current, next}}},
        current->kind == spice_sct_prototype::SctStructuredArmKind::Else
            ? "Insert first Else instruction" : "Insert first Switch case instruction",
        SelectionHints{{SctNavigationTarget{SctNavigationKind::Instruction,
            current->controller.instruction.value()}}, inserted},
        elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::insertInstructionIntoStructuredArm(
    const spice::sct::SctInstructionId controller,
    const spice_sct_prototype::SctStructuredArmKind armKind,
    const std::uint16_t opcode) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* region = verifiedRegion(*currentSnapshot_, controller);
    if (region == nullptr) {
        return failure({editError(locator, "StructuredArmNotVerified",
            "Instructions can be inserted semantically only into a verified arm.")});
    }
    const auto arm = std::ranges::find_if(region->arms, [&](const auto& candidate) {
        return candidate.kind == armKind;
    });
    if (arm == region->arms.end()) {
        return failure({editError(locator, "StructuredArmNotFound",
            "The verified region no longer contains this arm.")});
    }
    std::vector<spice::sct::SctInstructionId> members;
    const auto* section = currentSnapshot_->structuredControlFlow->findSection(
        region->id.section);
    if (section == nullptr) {
        return failure({editError(locator, "StructuredSectionNotFound",
            "The verified section is no longer available.")});
    }
    for (const auto blockId : arm->blocks) {
        const auto block = std::ranges::find(
            section->blocks, blockId, &spice_sct_prototype::SctBasicBlock::id);
        if (block != section->blocks.end())
            members.insert(members.end(), block->instructions.begin(), block->instructions.end());
    }
    std::vector<spice::sct::SctInstructionId> scaffolding;
    for (const auto& evidence : region->evidence) {
        if ((evidence.kind == spice_sct_prototype::SctStructureEvidenceKind::PreTargetJump
                || evidence.kind
                    == spice_sct_prototype::SctStructureEvidenceKind::BackwardTerminatorJump
                || evidence.kind
                    == spice_sct_prototype::SctStructureEvidenceKind::CommonForwardExit)
            && evidence.source) {
            scaffolding.push_back(*evidence.source);
        }
    }
    const auto order = workingState_.instructionOrder(region->id.section);
    std::optional<spice::sct::SctInstructionId> anchor;
    for (const auto id : order) {
        if (std::ranges::find(members, id) != members.end()
            && std::ranges::find(scaffolding, id) == scaffolding.end()) anchor = id;
    }
    if (!anchor) anchor = controller;
    return insertInstructionAfter(*anchor, opcode);
}

SctEditResult SctEditSession::deleteOnlyInstructionFromAuthoredArm(
    const SctAuthoredArmId id,
    const spice::sct::SctInstructionId instruction) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* current = structuredAuthoring_.find(id);
    if (current == nullptr || current->realization != SctAuthoredArmRealization::Physical
        || current->members.size() != 1u || current->members.front() != instruction) {
        return failure({editError(locator, "SemanticArmDeleteNotApplicable",
            "This command applies only to the sole visible instruction in a SALSA-authored arm.")});
    }
    if (workingState_.incomingReferenceCount(instruction) > 1u) {
        return failure({editError(locator, "SemanticArmInstructionExternallyReferenced",
            "The arm cannot become empty while another instruction also targets its body.",
            SctNavigationTarget{SctNavigationKind::Instruction, instruction.value()})});
    }
    auto controller = *workingState_.instruction(current->controller.instruction);
    SctSemanticOperationBatch document;
    if (current->kind == spice_sct_prototype::SctStructuredArmKind::Else) {
        if (!current->expectedJoin
            || !setInstructionReference(controller, 1u, *current->expectedJoin)) {
            return failure({editError(locator, "IfFalseTargetUnavailable",
                "The authored Else can no longer restore the verified If join.")});
        }
    } else if (current->kind
            == spice_sct_prototype::SctStructuredArmKind::SwitchCase) {
        const auto group = std::ranges::find_if(controller.repeatedParameterGroups,
            [&](const auto& candidate) {
                return std::ranges::any_of(candidate.parameters, [&](const auto& parameter) {
                    const auto* reference = std::get_if<spice::sct::SctInstructionReference>(
                        &parameter.value);
                    return parameter.schemaIndex == 3u && reference != nullptr
                        && reference->target == instruction;
                });
            });
        if (group == controller.repeatedParameterGroups.end()) {
            return failure({editError(locator, "AuthoredSwitchCaseNotFound",
                "The authored Switch case group can no longer be identified.")});
        }
        controller.repeatedParameterGroups.erase(group);
        const auto count = std::ranges::find(controller.fixedParameters, 1u,
            &spice::sct::SctDocumentParameter::schemaIndex);
        if (count == controller.fixedParameters.end()) {
            return failure({editError(locator, "SwitchCaseCountUnavailable",
                "The authored Switch no longer exposes its repeated-group count.")});
        }
        count->value = spice::sct::SctEncodedWordValue{
            static_cast<std::uint32_t>(controller.repeatedParameterGroups.size())};
    } else {
        return failure({editError(locator, "SemanticArmKindUnsupported",
            "This authored arm kind cannot be returned to an empty state.")});
    }
    document.operations.push_back(SctReplaceInstructionOperation{
        controller.id, std::move(controller)});
    for (const auto scaffold : current->managedScaffolding) {
        if (workingState_.incomingReferenceCount(scaffold) != 0u) {
            return failure({editError(locator, "ManagedScaffoldReferenced",
                "A managed control-flow instruction acquired an external reference and cannot be removed.")});
        }
        document.operations.push_back(SctDeleteInstructionOperation{scaffold});
    }
    document.operations.push_back(SctDeleteInstructionOperation{instruction});
    auto next = *current;
    next.members.clear();
    next.managedScaffolding.clear();
    next.realization = SctAuthoredArmRealization::Virtual;
    return commit(std::move(document),
        {{SctSetAuthoredArmOperation{id, *current, next}}},
        current->kind == spice_sct_prototype::SctStructuredArmKind::Else
            ? "Return Else to empty" : "Return Switch case to empty",
        SelectionHints{{SctNavigationTarget{SctNavigationKind::Instruction,
            instruction.value()}},
            {SctNavigationTarget{SctNavigationKind::Instruction,
                current->controller.instruction.value()}}});
}

std::optional<SctEditResult> SctEditSession::undo() {
    const auto journalStart = EditClock::now();
    if (!history_.canUndo()) return std::nullopt;
    const auto source = history_.currentRevision();
    std::optional<SctStructuredAuthoringApplication> authoringApplication;
    if (!source.state->authoringInverse.operations.empty())
        authoringApplication = structuredAuthoring_.apply(source.state->authoringInverse);
    if (authoringApplication && !authoringApplication->succeeded())
        return failure({editError(
            baselineSnapshot_->provenance->source().descriptor.locator,
            "StructuredUndoFailed", *authoringApplication->issue)});
    std::optional<SctWorkingApplication> application;
    if (!source.state->inverse.operations.empty())
        application = workingState_.apply(source.state->inverse);
    if (application && !application->succeeded()) {
        if (authoringApplication)
            (void)structuredAuthoring_.apply(authoringApplication->inverse);
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application->issues) {
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
    if (!result.changes.documentChanged && verifiedRevision_ == navigation->from)
        verifiedRevision_ = navigation->to;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Undo,
        navigation->from, navigation->to, result.changes, std::nullopt,
        navigation->to == verifiedRevision_ ? SctRevisionVerification::Verified
            : SctRevisionVerification::Pending};
    result.suggestedSelection = source.state->selections.undoSelection;
    result.journalMicroseconds = elapsedMicroseconds(journalStart);
    rebuildSemanticProjection();
    return result;
}

std::optional<SctEditResult> SctEditSession::redo() {
    const auto journalStart = EditClock::now();
    const auto target = history_.redoTarget();
    if (!target.has_value()) return std::nullopt;
    std::optional<SctStructuredAuthoringApplication> authoringApplication;
    if (!target->state->authoringForward.operations.empty())
        authoringApplication = structuredAuthoring_.apply(target->state->authoringForward);
    if (authoringApplication && !authoringApplication->succeeded())
        return failure({editError(
            baselineSnapshot_->provenance->source().descriptor.locator,
            "StructuredRedoFailed", *authoringApplication->issue)});
    std::optional<SctWorkingApplication> application;
    if (!target->state->forward.operations.empty())
        application = workingState_.apply(target->state->forward);
    if (application && !application->succeeded()) {
        if (authoringApplication)
            (void)structuredAuthoring_.apply(authoringApplication->inverse);
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application->issues) {
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
    if (!result.changes.documentChanged && verifiedRevision_ == navigation->from)
        verifiedRevision_ = navigation->to;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Redo,
        navigation->from, navigation->to, result.changes, std::nullopt,
        navigation->to == verifiedRevision_ ? SctRevisionVerification::Verified
            : SctRevisionVerification::Pending};
    result.suggestedSelection = target->state->selections.redoSelection;
    result.journalMicroseconds = elapsedMicroseconds(journalStart);
    rebuildSemanticProjection();
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
        if (revision->state->forward.operations.empty()) continue;
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
        if (!revision->state->forward.operations.empty())
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
        std::vector<SctAuthoredArm>{structuredAuthoring_.arms().begin(),
            structuredAuthoring_.arms().end()},
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
        result.structuredControlFlow,
    });
    auto validationMessages = validationDiagnostics(
        baselineSnapshot_->provenance->source().descriptor.locator, result.validation);
    verifiedSnapshot->diagnostics = baselineSnapshot_->provenance->baselineDiagnostics;
    verifiedSnapshot->diagnostics.insert(verifiedSnapshot->diagnostics.end(),
        std::make_move_iterator(validationMessages.begin()),
        std::make_move_iterator(validationMessages.end()));
    materializationCheckpoints_.push_back({result.targetRevision, verifiedSnapshot});
    auto promotedRevision = result.targetRevision;
    bool onlyAuthoringAfterTarget = true;
    auto cursor = history_.currentRevision().id;
    while (cursor != result.targetRevision) {
        const auto entry = history_.revision(cursor);
        if (!entry || entry->state->forward.operations.size() != 0u) {
            onlyAuthoringAfterTarget = false;
            break;
        }
        cursor = entry->state->parent;
    }
    if (onlyAuthoringAfterTarget) promotedRevision = history_.currentRevision().id;
    if (promotedRevision == history_.currentRevision().id) {
        verifiedRevision_ = promotedRevision;
        materializedDocument_ = result.document;
        currentSnapshot_ = std::move(verifiedSnapshot);
        structurallyValid_ = true;
        if (promotedRevision != result.targetRevision)
            materializationCheckpoints_.push_back({promotedRevision, currentSnapshot_});
    }
    pruneMaterializationCheckpoints();
    rebuildSemanticProjection();
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
        if (!entry->state->authoringInverse.operations.empty()) {
            const auto authoring = structuredAuthoring_.apply(
                entry->state->authoringInverse);
            if (!authoring.succeeded()) return std::nullopt;
        }
        if (!entry->state->inverse.operations.empty()) {
            const auto application = workingState_.apply(entry->state->inverse);
            if (!application.succeeded()) return std::nullopt;
        }
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
    rebuildSemanticProjection();

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

const SctStructuredAuthoringState& SctEditSession::structuredAuthoring() const noexcept {
    return structuredAuthoring_;
}

std::shared_ptr<const SctSemanticEditorProjection>
SctEditSession::semanticProjection() const noexcept {
    return semanticProjection_;
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
    SctStructuredAuthoringOperationBatch authoringOperation,
    std::string description,
    SelectionHints selections,
    const std::uint64_t preflightMicroseconds) {
    const auto journalStart = EditClock::now();
    if (operation.operations.empty() && authoringOperation.operations.empty())
        return failure({editError(
            baselineSnapshot_->provenance->source().descriptor.locator,
            "EmptyEdit", "An edit must contain a document or semantic authoring operation.")});

    std::optional<SctStructuredAuthoringApplication> authoringApplication;
    if (!authoringOperation.operations.empty())
        authoringApplication = structuredAuthoring_.apply(authoringOperation);
    if (authoringApplication && !authoringApplication->succeeded()) {
        return failure({editError(
            baselineSnapshot_->provenance->source().descriptor.locator,
            "StructuredAuthoringFailed", *authoringApplication->issue)});
    }
    std::optional<SctWorkingApplication> application;
    if (!operation.operations.empty()) application = workingState_.apply(operation);
    if (application && !application->succeeded()) {
        if (authoringApplication)
            (void)structuredAuthoring_.apply(authoringApplication->inverse);
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& problem : application->issues) {
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
    if (application) delta->inverse = std::move(application->inverse);
    delta->authoringForward = std::move(authoringOperation);
    if (authoringApplication)
        delta->authoringInverse = std::move(authoringApplication->inverse);
    if (application) {
        delta->forwardChanges = application->forwardChanges;
        delta->reverseChanges = application->reverseChanges;
    }
    if (authoringApplication) {
        delta->forwardChanges.structuredAuthoring = authoringApplication->changes;
        for (auto change = authoringApplication->changes.rbegin();
            change != authoringApplication->changes.rend(); ++change) {
            delta->reverseChanges.structuredAuthoring.push_back(
                {change->id, change->after, change->before});
        }
    }
    delta->selections = selections;
    const auto committed = history_.commit(delta, std::move(description));
    assert(committed.created);
    if (!delta->forwardChanges.documentChanged && parent == verifiedRevision_) {
        verifiedRevision_ = committed.revision;
        materializationCheckpoints_.push_back({committed.revision, currentSnapshot_});
    }
    pruneMaterializationCheckpoints();
    rebuildSemanticProjection();

    SctEditResult result;
    result.committed = true;
    result.revision = committed.revision;
    result.snapshot = currentSnapshot_;
    result.changes = delta->forwardChanges;
    result.transition = SctRevisionTransition{SctRevisionTransitionKind::Commit,
        parent, committed.revision, result.changes, std::nullopt,
        committed.revision == verifiedRevision_ ? SctRevisionVerification::Verified
            : SctRevisionVerification::Pending};
    result.suggestedSelection = selections.redoSelection;
    result.preflightMicroseconds = preflightMicroseconds;
    result.journalMicroseconds = elapsedMicroseconds(journalStart);
    return result;
}

void SctEditSession::rebuildSemanticProjection() {
    if (!currentSnapshot_ || !currentSnapshot_->structuredControlFlow) {
        semanticProjection_.reset();
        return;
    }
    semanticProjection_ = std::make_shared<const SctSemanticEditorProjection>(
        SctSemanticEditorProjection::build(*currentSnapshot_->structuredControlFlow,
            workingState_, structuredAuthoring_, history_.currentRevision().id,
            verifiedRevision_));
}

void SctEditSession::pruneMaterializationCheckpoints() {
    std::erase_if(materializationCheckpoints_, [this](const auto& checkpoint) {
        return !history_.revision(checkpoint.revision).has_value();
    });
}

}  // namespace salsa::core
