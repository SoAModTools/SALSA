#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctParameterAuthoring.h"
#include "SalsaCore/Sct/SctInspectionLocation.h"

#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctDocumentEntityFactory.h"
#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctOpcodeMetadata.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <ranges>
#include <regex>
#include <unordered_map>
#include <unordered_set>
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

[[nodiscard]] SctPipelineDiagnostic editWarning(
    const AssetLocator& locator, std::string code, std::string message,
    std::optional<SctNavigationTarget> target = std::nullopt) {
    auto result = editError(locator, std::move(code), std::move(message), target);
    result.severity = DiagnosticSeverity::Warning;
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
    target.sections.insert(target.sections.end(),
        source.sections.begin(), source.sections.end());
    target.instructions.insert(target.instructions.end(),
        source.instructions.begin(), source.instructions.end());
    target.parameters.insert(target.parameters.end(),
        source.parameters.begin(), source.parameters.end());
    target.repeatedGroups.insert(target.repeatedGroups.end(),
        source.repeatedGroups.begin(), source.repeatedGroups.end());
    target.footerEntries.insert(target.footerEntries.end(),
        source.footerEntries.begin(), source.footerEntries.end());
    target.textValues.insert(target.textValues.end(),
        source.textValues.begin(), source.textValues.end());
    target.modified.insert(target.modified.end(),
        source.modified.begin(), source.modified.end());
    target.structuredAuthoring.insert(target.structuredAuthoring.end(),
        source.structuredAuthoring.begin(), source.structuredAuthoring.end());
    target.invalidations = target.invalidations | source.invalidations;
    target.documentChanged = target.documentChanged || source.documentChanged;
}

[[nodiscard]] bool validAuthoredSectionName(const std::string_view name) {
    if (name.empty() || name.size() > 16u) return false;
    return std::ranges::all_of(name, [](const char value) {
        const auto character = static_cast<unsigned char>(value);
        return (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]] bool sameTextValue(
    const spice::sct::SctTextValue& left, const spice::sct::SctTextValue& right) {
    if (left.index() != right.index()) return false;
    return std::visit([&](const auto& leftValue) {
        using T = std::decay_t<decltype(leftValue)>;
        const auto& rightValue = std::get<T>(right);
        if constexpr (std::is_same_v<T, spice::sct::SctPlainText>)
            return leftValue.utf8 == rightValue.utf8;
        else if constexpr (std::is_same_v<T, spice::sct::SctOpaqueText>)
            return leftValue.bytes == rightValue.bytes;
        else if constexpr (std::is_same_v<T, spice::sct::SctEmptyIndexedText>)
            return true;
        else {
            const auto leftProjection = SctMessageAuthoringProfile::project(leftValue);
            const auto rightProjection = SctMessageAuthoringProfile::project(rightValue);
            return leftProjection.supported() && rightProjection.supported()
                && *leftProjection.draft == *rightProjection.draft;
        }
    }, left);
}

[[nodiscard]] bool parameterValueMatches(
    const spice::sct::SctOpcodeParameterSchema& schema,
    const spice::sct::SctDocumentParameterValue& value) {
    if (std::holds_alternative<spice::sct::SctOpaqueParameterValue>(value)
        || std::holds_alternative<spice::sct::SctUnresolvedReferenceValue>(value))
        return false;
    if (schema.referenceKind == spice::sct::SctOpcodeReferenceKind::Instruction)
        return std::holds_alternative<spice::sct::SctInstructionReference>(value);
    if (schema.referenceKind == spice::sct::SctOpcodeReferenceKind::Text) {
        return schema.textReference
            && (schema.textReference->storage == spice::sct::SctTextStorage::IndexedSection
                ? std::holds_alternative<spice::sct::SctStringReference>(value)
                : std::holds_alternative<spice::sct::SctFooterEntryReference>(value));
    }
    if (schema.encoding == spice::sct::SctOpcodeParameterEncoding::ScptExpression)
        return std::holds_alternative<spice::sct::SctCanonicalExpression>(value);
    if (schema.encoding == spice::sct::SctOpcodeParameterEncoding::RawWordsUntilSentinel)
        return std::holds_alternative<spice::sct::SctTerminatedWordSequenceValue>(value);
    return std::holds_alternative<spice::sct::SctEncodedWordValue>(value);
}

[[nodiscard]] const spice::sct::SctStructuredRegion* verifiedRegion(
    const SctDocumentSnapshot& snapshot,
    const spice::sct::SctInstructionId controller) {
    if (!snapshot.analysis) return nullptr;
    for (const auto& section : snapshot.analysis->structuredControlFlow.sections()) {
        const auto found = std::ranges::find_if(section.regions, [&](const auto& region) {
            return region.id.headerInstruction == controller;
        });
        if (found != section.regions.end()) return &*found;
    }
    return nullptr;
}

[[nodiscard]] bool hasCaseValue(
    const spice::sct::SctStructuredRegion& region,
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
            && arm.kind == spice::sct::SctStructuredArmKind::SwitchCase
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
    : SctEditSession(initialSnapshot, initialSnapshot, {}, {}) {}

SctEditSession::SctEditSession(
    std::shared_ptr<const SctDocumentSnapshot> baselineSnapshot,
    std::shared_ptr<const SctDocumentSnapshot> restoredSnapshot,
    const std::span<const SctAuthoredArm> authoredArms,
    const std::span<const SctPatchedTextRepair> textRepairs)
    : baselineSnapshot_(std::move(baselineSnapshot)),
      history_(std::make_shared<const RevisionDelta>()),
      workingState_(restoredSnapshot != nullptr ? restoredSnapshot->document : nullptr,
          textRepairs),
      structuredAuthoring_(authoredArms),
      materializedDocument_(restoredSnapshot != nullptr ? restoredSnapshot->document : nullptr),
      currentSnapshot_(std::move(restoredSnapshot)) {
    assert(baselineSnapshot_ != nullptr);
    assert(baselineSnapshot_->document != nullptr);
    assert(currentSnapshot_ != nullptr);
    structurallyValid_ = currentSnapshot_->readiness
        == spice::sct::SctDocumentReadiness::StructurallyValid;
    materializationCheckpoints_.push_back(
        {history_.currentRevision().id, currentSnapshot_});
    rebuildSemanticProjection();
}

std::unique_ptr<SctEditSession> SctEditSession::createRebased(
    std::shared_ptr<const SctDocumentSnapshot> newBaselineSnapshot,
    std::shared_ptr<const SctDocumentSnapshot> rebasedSnapshot,
    const std::span<const SctAuthoredArm> authoredArms,
    const std::span<const SctPatchedTextRepair> textRepairs) {
    if (!newBaselineSnapshot || !newBaselineSnapshot->document
        || newBaselineSnapshot->readiness
            != spice::sct::SctDocumentReadiness::StructurallyValid
        || !rebasedSnapshot || !rebasedSnapshot->document
        || rebasedSnapshot->readiness
            != spice::sct::SctDocumentReadiness::StructurallyValid)
        return nullptr;
    auto session = std::make_unique<SctEditSession>(newBaselineSnapshot);
    auto delta = std::make_shared<RevisionDelta>();
    delta->parent = session->history_.currentRevision().id;
    delta->forwardChanges.documentChanged = true;
    delta->reverseChanges.documentChanged = true;
    delta->externalBefore = RevisionDelta::ExternalState{
        newBaselineSnapshot, {}, {}};
    delta->externalAfter = RevisionDelta::ExternalState{
        rebasedSnapshot,
        {authoredArms.begin(), authoredArms.end()},
        {textRepairs.begin(), textRepairs.end()}};
    const auto committed = session->history_.commit(
        delta, "Rebase patch onto new source");
    if (!committed.created) return nullptr;
    session->installExternalState(*delta->externalAfter);
    session->materializationCheckpoints_.push_back(
        {committed.revision, rebasedSnapshot});
    session->verifiedRevision_ = committed.revision;
    session->history_.markCheckpoint();
    session->pruneMaterializationCheckpoints();
    session->rebuildSemanticProjection();
    return session;
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

SctInstructionAuthoringDraftResult SctEditSession::createInstructionDraft(
    const std::uint16_t opcode) const {
    SctInstructionAuthoringDraftResult result;
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (opcode == 9u) {
        result.diagnostics.push_back(editError(locator, "LabelInsertionReserved",
            "LabelOrStringPrefix is created only as part of section creation."));
        return result;
    }
    spice::sct::SctInstructionFactoryRequest request;
    request.opcode = opcode;
    auto created = spice::sct::SctInstructionFactory::createDraft(request);
    for (const auto& diagnostic : created.diagnostics)
        result.diagnostics.push_back(convertSctDiagnostic(
            diagnostic, SctPipelineStage::Edit, locator));
    if (!created.draft) return result;

    SctInstructionAuthoringDraft draft;
    draft.baseRevision = workingRevision();
    draft.instruction = std::move(*created.draft);
    const auto* schema = spice::sct::findSctOpcodeSchema(opcode);
    if (schema != nullptr) {
        for (const auto& parameter : draft.instruction.parameters) {
            if (parameter.value) continue;
            const auto rule = spice::sct::sctOpcodeTextReference(
                *schema, parameter.address.schemaIndex);
            if (!rule || rule->storage != spice::sct::SctTextStorage::Footer)
                continue;
            if (rule->kind == spice::sct::SctTextKind::PlainString) {
                draft.ownedFooterText.push_back({parameter.address,
                    spice::sct::SctTextKind::PlainString,
                    spice::sct::SctPlainText{}});
            } else {
                const auto message = SctMessageAuthoringProfile::materialize(
                    SctMessageDraft{});
                if (message.message) {
                    draft.ownedFooterText.push_back({parameter.address,
                        spice::sct::SctTextKind::SctString, *message.message});
                }
            }
        }
    }
    result.draft = std::move(draft);
    return result;
}

SctEditResult SctEditSession::createInstructionAfter(
    const spice::sct::SctInstructionId anchorInstruction,
    SctInstructionAuthoringDraft draft) {
    const auto preflightStart = EditClock::now();
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (draft.baseRevision != workingRevision())
        return failure({editError(locator, "InstructionDraftStale",
            "The instruction draft was created from an older document revision.")});
    if (draft.instruction.opcode == 9u)
        return failure({editError(locator, "LabelInsertionReserved",
            "LabelOrStringPrefix is created only as part of section creation.")});
    const auto placement = workingState_.placement(anchorInstruction);
    const auto* anchor = workingState_.instruction(anchorInstruction);
    if (!placement || anchor == nullptr)
        return failure({editError(locator, "InstructionNotFound",
            "The insertion anchor no longer exists.")});
    if (anchor->opcode == 12u)
        return failure({editError(locator, "InstructionInsertionAfterReturn",
            "An instruction cannot be inserted after Return.")});
    if (draft.instruction.opcode == 12u
        && workingState_.instructionAfter(anchorInstruction))
        return failure({editError(locator, "ReturnMustTerminateSection",
            "Return can only be inserted as the final instruction in a section.")});

    SctSemanticOperationBatch batch;
    auto nextFooterId = workingState_.nextFooterEntryIdValue();
    std::optional<spice::sct::SctFooterEntryId> footerAnchor;
    if (!workingState_.footerEntryOrder().empty())
        footerAnchor = workingState_.footerEntryOrder().back();
    std::unordered_set<std::string> ownedAddresses;
    const auto addressKey = [](const spice::sct::SctParameterAddress& address) {
        return std::to_string(address.schemaIndex) + ":"
            + (address.repeatedGroupOrdinal
                ? std::to_string(*address.repeatedGroupOrdinal) : "fixed");
    };
    const auto* instructionSchema =
        spice::sct::findSctOpcodeSchema(draft.instruction.opcode);
    for (const auto& owned : draft.ownedFooterText) {
        if (!ownedAddresses.insert(addressKey(owned.parameter)).second)
            return failure({editError(locator, "InstructionDraftDuplicateOwnedText",
                "The instruction draft contains duplicate owned text for one parameter.")});
        const auto found = std::ranges::find(draft.instruction.parameters,
            owned.parameter, &spice::sct::SctInstructionDraftParameter::address);
        if (found == draft.instruction.parameters.end() || found->value)
            return failure({editError(locator, "InstructionDraftOwnedTextMismatch",
                "Owned footer text does not match an unresolved draft parameter.")});
        const auto textRule = instructionSchema == nullptr ? std::nullopt
            : spice::sct::sctOpcodeTextReference(
                *instructionSchema, owned.parameter.schemaIndex);
        const bool valueMatches = owned.kind == spice::sct::SctTextKind::PlainString
            ? std::holds_alternative<spice::sct::SctPlainText>(owned.value)
            : std::holds_alternative<spice::sct::SctMessage>(owned.value);
        if (!textRule || textRule->storage != spice::sct::SctTextStorage::Footer
            || textRule->kind != owned.kind || !valueMatches) {
            return failure({editError(locator, "InstructionDraftOwnedTextMismatch",
                "Owned footer text does not match the parameter's text contract.")});
        }
        const auto id = spice::sct::SctFooterEntryId(nextFooterId++);
        spice::sct::SctDocumentFooterEntry entry{id, owned.kind, owned.value};
        batch.operations.push_back(SctInsertFooterEntryAfterOperation{footerAnchor, entry});
        footerAnchor = id;
        found->value = spice::sct::SctFooterEntryReference{id};
    }
    if (std::ranges::any_of(draft.instruction.parameters,
            [](const auto& parameter) { return !parameter.value.has_value(); })) {
        return failure({editError(locator, "InstructionDraftUnresolved",
            "Resolve every required or provisional parameter before creating the instruction.")});
    }

    spice::sct::SctDocument context;
    std::vector<spice::sct::SctDocumentInstruction> referencedInstructions;
    std::uint64_t contextSectionId = 1;
    for (const auto& parameter : draft.instruction.parameters) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
                if (const auto* target = workingState_.instruction(value.target))
                    referencedInstructions.push_back(*target);
            } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
                const auto* text = workingState_.textValue(SctTextTarget{value.target});
                const auto kind = workingState_.stringKind(value.target);
                if (text != nullptr && kind) {
                    context.sections.push_back({spice::sct::SctSectionId(contextSectionId++),
                        std::string(workingState_.stringSectionName(value.target)
                            .value_or("DRAFT_STRING")),
                        spice::sct::SctStringSectionContent{{value.target, *text, *kind}}});
                }
            } else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryReference>) {
                if (const auto* target = workingState_.footerEntry(value.target))
                    context.footerEntries.push_back(*target);
                else {
                    const auto inserted = std::ranges::find_if(batch.operations,
                        [&](const auto& primitive) {
                            const auto* insertedValue = std::get_if<SctInsertFooterEntryAfterOperation>(
                                &primitive);
                            return insertedValue != nullptr
                                && insertedValue->entry.id == value.target;
                        });
                    if (inserted != batch.operations.end())
                        context.footerEntries.push_back(
                            std::get<SctInsertFooterEntryAfterOperation>(*inserted).entry);
                }
            }
        }, *parameter.value);
    }
    if (!referencedInstructions.empty())
        context.sections.push_back({spice::sct::SctSectionId(contextSectionId++),
            "DRAFT_REFERENCES", spice::sct::SctScriptSectionContent{
                std::move(referencedInstructions)}});

    const auto materialized = spice::sct::SctInstructionFactory::materialize(
        context, draft.instruction);
    if (!materialized.instruction) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : materialized.diagnostics)
            diagnostics.push_back(convertSctDiagnostic(
                diagnostic, SctPipelineStage::Edit, locator));
        return failure(std::move(diagnostics));
    }
    auto instruction = *materialized.instruction;
    instruction.id = spice::sct::SctInstructionId(
        workingState_.nextInstructionIdValue());
    const auto inserted = instruction.id;
    batch.operations.push_back(SctInsertInstructionAfterOperation{
        anchorInstruction, std::move(instruction)});
    return commit(std::move(batch), {},
        "Insert " + opcodeName(draft.instruction.opcode),
        SelectionHints{SctNavigationTarget{SctNavigationKind::Instruction,
            anchorInstruction.value()},
            SctNavigationTarget{SctNavigationKind::Instruction, inserted.value()}},
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

    return commit(SctSemanticOperationBatch{{SctReplaceTextValueOperation{
            target, *materialized.message}}},
        {},
        messageEditDescription(editKind),
        SelectionHints{navigation, navigation}, elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::replacePlainText(
    const SctTextTarget& target, std::string utf8) {
    const auto navigation = navigationFor(target);
    const auto* current = workingState_.textValue(target);
    if (current == nullptr || !std::holds_alternative<spice::sct::SctPlainText>(*current)) {
        return failure({editError(baselineSnapshot_->provenance->source().descriptor.locator,
            "PlainTextTargetNotFound", "The selected text is not an editable plain string.", navigation)});
    }
    return replaceTextValue(target, spice::sct::SctPlainText{std::move(utf8)},
        "Edit plain text");
}

SctEditResult SctEditSession::replaceTextValue(
    const SctTextTarget& target, spice::sct::SctTextValue value,
    std::string description,
    std::optional<SctTextRepairProvenance> repairProvenance) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto navigation = navigationFor(target);
    if (!structurallyValid_) return failure({editError(locator,
        "DocumentNotStructurallyValid", "Text editing is unavailable until the document is structurally valid.", navigation)});
    const auto* current = workingState_.textValue(target);
    if (current == nullptr) return failure({editError(locator, "TextTargetNotFound",
        "The selected text entity no longer exists.", navigation)});
    if (sameTextValue(*current, value)) {
        SctEditResult result;
        result.revision = history_.currentRevision().id;
        result.snapshot = currentSnapshot_;
        result.suggestedSelection = navigation;
        return result;
    }
    const bool recordsRepair = repairProvenance.has_value();
    return commit(SctSemanticOperationBatch{{SctReplaceTextValueOperation{
            target, std::move(value), recordsRepair, std::move(repairProvenance)}}}, {}, std::move(description),
        SelectionHints{navigation, navigation});
}

SctEditResult SctEditSession::replaceParameterValue(
    const spice::sct::SctParameterSite& site,
    spice::sct::SctDocumentParameterValue value) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const SctNavigationTarget target{SctNavigationKind::Instruction,
        site.instruction.value()};
    if (!structurallyValid_) return failure({editError(locator,
        "DocumentNotStructurallyValid",
        "Parameter editing is unavailable until the document is structurally valid.", target)});
    const auto* instruction = workingState_.instruction(site.instruction);
    const auto* current = workingState_.parameter(site);
    const auto* schema = instruction == nullptr ? nullptr
        : spice::sct::findSctOpcodeSchema(instruction->opcode);
    const auto* parameterSchema = schema == nullptr ? nullptr
        : spice::sct::sctOpcodeParameterSchema(*schema, site.parameter.schemaIndex);
    if (instruction == nullptr || current == nullptr || parameterSchema == nullptr
        || parameterSchema->belongsToRepeatedGroup
            != site.parameter.repeatedGroupOrdinal.has_value()) {
        return failure({editError(locator, "ParameterTargetStale",
            "The parameter address no longer identifies the selected opcode slot.", target)});
    }
    if (parameterSchema->defaultKind
        == spice::sct::SctOpcodeDefaultKind::DerivedRepeatedGroupCount) {
        return failure({editError(locator, "DerivedParameterReadOnly",
            "Repeated-group counts are derived and cannot be edited directly.", target)});
    }
    if (!parameterValueMatches(*parameterSchema, value)) {
        return failure({editError(locator, "ParameterValueKindMismatch",
            "The replacement value does not match the opcode parameter contract.", target)});
    }
    const bool targetExists = std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>)
            return workingState_.instruction(typed.target) != nullptr;
        else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            const auto kind = workingState_.stringKind(typed.target);
            return kind && parameterSchema->textReference
                && *kind == parameterSchema->textReference->kind;
        } else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryReference>) {
            const auto* entry = workingState_.footerEntry(typed.target);
            return entry != nullptr && parameterSchema->textReference
                && entry->kind == parameterSchema->textReference->kind;
        } else return true;
    }, value);
    if (!targetExists) return failure({editError(locator,
        "ParameterReferenceTargetInvalid",
        "The selected reference target is missing or has the wrong storage or text kind.", target)});
    if (SctParameterAuthoringService::equivalent(current->value, value)) {
        SctEditResult result;
        result.revision = history_.currentRevision().id;
        result.snapshot = currentSnapshot_;
        result.suggestedSelection = target;
        return result;
    }
    return commit(SctSemanticOperationBatch{{SctReplaceParameterValueOperation{
        site, std::move(value)}}}, {}, "Edit instruction parameter",
        SelectionHints{target, target});
}

SctEditResult SctEditSession::editParameterText(
    const spice::sct::SctParameterSite& site, std::string text) {
    const auto* instruction = workingState_.instruction(site.instruction);
    if (instruction == nullptr) return failure({editError(
        baselineSnapshot_->provenance->source().descriptor.locator,
        "InstructionNotFound", "The parameter's instruction no longer exists.",
        SctNavigationTarget{SctNavigationKind::Instruction, site.instruction.value()})});
    const auto projection = SctParameterAuthoringService::project(
        workingState_, site.instruction);
    const SctParameterRowPresentation* row = nullptr;
    for (const auto& candidate : projection.fixedParameters)
        if (candidate.site == site) row = &candidate;
    for (const auto& group : projection.repeatedGroups)
        for (const auto& candidate : group.parameters)
            if (candidate.site == site) row = &candidate;
    if (row != nullptr
        && row->editor == SctInlineParameterEditorKind::PlainFooterText)
        return editReferencedFooterText(site, std::move(text));
    const auto parsed = SctParameterAuthoringService::parseInline(
        *instruction, site, std::move(text));
    if (!parsed.succeeded()) return failure({editError(
        baselineSnapshot_->provenance->source().descriptor.locator,
        "ParameterInputInvalid", parsed.error,
        SctNavigationTarget{SctNavigationKind::Instruction, site.instruction.value()})});
    auto result = replaceParameterValue(site, *parsed.value);
    for (const auto& warning : parsed.warnings) {
        result.diagnostics.push_back(editWarning(
            baselineSnapshot_->provenance->source().descriptor.locator,
            "ProvisionalParameterConstraint", warning,
            SctNavigationTarget{SctNavigationKind::Instruction,
                site.instruction.value()}));
    }
    return result;
}

SctEditResult SctEditSession::insertRepeatedGroup(
    const spice::sct::SctInstructionId instructionId, const std::uint32_t ordinal,
    spice::sct::SctDocumentRepeatedParameterGroup group) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const SctNavigationTarget target{SctNavigationKind::Instruction, instructionId.value()};
    const auto* instruction = workingState_.instruction(instructionId);
    const auto* schema = instruction == nullptr ? nullptr
        : spice::sct::findSctOpcodeSchema(instruction->opcode);
    const auto repeated = schema == nullptr
        ? std::nullopt : spice::sct::sctOpcodeRepeatedGroup(*schema);
    if (!structurallyValid_ || instruction == nullptr || !repeated
        || ordinal > instruction->repeatedParameterGroups.size()) {
        return failure({editError(locator, "RepeatedGroupInsertionInvalid",
            "The repeated-group insertion target is unavailable or stale.", target)});
    }
    if (schema->semantic.controlRole == spice::sct::SctOpcodeControlRole::Switch
        && std::ranges::any_of(structuredAuthoring_.arms(), [&](const auto& arm) {
            return arm.controller.instruction == instructionId;
        })) {
        return failure({editError(locator, "RepeatedGroupManagedBySemanticEditor",
            "Switch case groups managed by the Semantic Outline cannot be edited as raw groups.", target)});
    }
    std::vector<spice::sct::SctRepeatedParameterOverride> overrides;
    for (auto& parameter : group.parameters)
        overrides.push_back({parameter.schemaIndex, std::move(parameter.value)});
    auto draft = spice::sct::SctInstructionFactory::createRepeatedGroupDraft(
        instruction->opcode, overrides);
    if (!draft.draft) return failure({editError(locator,
        "RepeatedGroupDraftInvalid", "The repeated group does not match the opcode schema.", target)});
    const auto materialized = spice::sct::SctInstructionFactory::materializeRepeatedGroup(
        *draft.draft);
    if (!materialized.group) return failure({editError(locator,
        "RepeatedGroupDraftIncomplete",
        "Required or provisional repeated parameters must be resolved before creation.", target)});
    return commit(SctSemanticOperationBatch{{SctInsertRepeatedGroupOperation{
        instructionId, ordinal, *materialized.group}}}, {}, "Add repeated parameter group",
        SelectionHints{target, target});
}

SctEditResult SctEditSession::deleteRepeatedGroup(
    const spice::sct::SctInstructionId instructionId, const std::uint32_t ordinal) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const SctNavigationTarget target{SctNavigationKind::Instruction, instructionId.value()};
    const auto* instruction = workingState_.instruction(instructionId);
    const auto* schema = instruction == nullptr ? nullptr
        : spice::sct::findSctOpcodeSchema(instruction->opcode);
    const auto repeated = schema == nullptr
        ? std::nullopt : spice::sct::sctOpcodeRepeatedGroup(*schema);
    const auto minimum = repeated && repeated->firstParameter < schema->parameters.paramCount
        ? 1u : 0u;
    if (!structurallyValid_ || instruction == nullptr || !repeated
        || ordinal >= instruction->repeatedParameterGroups.size()) {
        return failure({editError(locator, "RepeatedGroupDeletionInvalid",
            "The repeated group no longer exists.", target)});
    }
    if (instruction->repeatedParameterGroups.size() <= minimum) {
        return failure({editError(locator, "RepeatedGroupMinimum",
            "The opcode schema requires at least one repeated parameter group.", target)});
    }
    if (schema->semantic.controlRole == spice::sct::SctOpcodeControlRole::Switch
        && std::ranges::any_of(structuredAuthoring_.arms(), [&](const auto& arm) {
            return arm.controller.instruction == instructionId;
        })) {
        return failure({editError(locator, "RepeatedGroupManagedBySemanticEditor",
            "Switch case groups managed by the Semantic Outline cannot be edited as raw groups.", target)});
    }
    return commit(SctSemanticOperationBatch{{SctDeleteRepeatedGroupOperation{
        instructionId, ordinal}}}, {}, "Delete repeated parameter group",
        SelectionHints{target, target});
}

SctEditResult SctEditSession::moveRepeatedGroup(
    const spice::sct::SctInstructionId instructionId, const std::uint32_t ordinal,
    const SctRepeatedGroupMoveDirection direction) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const SctNavigationTarget target{SctNavigationKind::Instruction, instructionId.value()};
    const auto* instruction = workingState_.instruction(instructionId);
    const auto* schema = instruction == nullptr ? nullptr
        : spice::sct::findSctOpcodeSchema(instruction->opcode);
    if (!structurallyValid_ || instruction == nullptr
        || ordinal >= instruction->repeatedParameterGroups.size()) {
        return failure({editError(locator, "RepeatedGroupMoveInvalid",
            "The repeated group no longer exists.", target)});
    }
    if ((direction == SctRepeatedGroupMoveDirection::Up && ordinal == 0u)
        || (direction == SctRepeatedGroupMoveDirection::Down
            && ordinal + 1u >= instruction->repeatedParameterGroups.size())) {
        return failure({editError(locator, "RepeatedGroupMoveAtBoundary",
            "The repeated group is already at that boundary.", target)});
    }
    if (schema != nullptr
        && schema->semantic.controlRole == spice::sct::SctOpcodeControlRole::Switch
        && std::ranges::any_of(structuredAuthoring_.arms(), [&](const auto& arm) {
            return arm.controller.instruction == instructionId;
        })) {
        return failure({editError(locator, "RepeatedGroupManagedBySemanticEditor",
            "Switch case groups managed by the Semantic Outline cannot be edited as raw groups.", target)});
    }
    const auto destination = direction == SctRepeatedGroupMoveDirection::Up
        ? ordinal - 1u : ordinal + 1u;
    return commit(SctSemanticOperationBatch{{SctRelocateRepeatedGroupOperation{
        instructionId, ordinal, destination}}}, {}, "Move repeated parameter group",
        SelectionHints{target, target});
}

SctEditResult SctEditSession::editReferencedFooterText(
    const spice::sct::SctParameterSite& site, std::string utf8) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const SctNavigationTarget instructionTarget{SctNavigationKind::Instruction,
        site.instruction.value()};
    const auto* parameter = workingState_.parameter(site);
    const auto* reference = parameter == nullptr ? nullptr
        : std::get_if<spice::sct::SctFooterEntryReference>(&parameter->value);
    const auto* entry = reference == nullptr ? nullptr
        : workingState_.footerEntry(reference->target);
    const auto* plain = entry == nullptr ? nullptr
        : std::get_if<spice::sct::SctPlainText>(&entry->value);
    if (!structurallyValid_ || reference == nullptr || entry == nullptr
        || entry->kind != spice::sct::SctTextKind::PlainString || plain == nullptr
        || plain->utf8.find('\n') != std::string::npos || utf8.find('\n') != std::string::npos) {
        return failure({editError(locator, "InlineFooterTextUnavailable",
            "This parameter does not reference an inline-editable single-line footer string.",
            instructionTarget)});
    }
    if (plain->utf8 == utf8) {
        SctEditResult result;
        result.revision = history_.currentRevision().id;
        result.snapshot = currentSnapshot_;
        result.suggestedSelection = instructionTarget;
        return result;
    }
    const auto occurrences = workingState_.referenceOccurrenceCount(
        spice::sct::SctDocumentReferenceTarget{reference->target});
    if (occurrences <= 1u) {
        return commit(SctSemanticOperationBatch{{SctReplaceTextValueOperation{
            SctTextTarget{reference->target}, spice::sct::SctPlainText{std::move(utf8)}}}},
            {}, "Edit referenced footer text",
            SelectionHints{instructionTarget, instructionTarget});
    }
    const auto newId = spice::sct::SctFooterEntryId(
        workingState_.nextFooterEntryIdValue());
    auto copy = *entry;
    copy.id = newId;
    copy.value = spice::sct::SctPlainText{std::move(utf8)};
    const auto order = workingState_.footerEntryOrder();
    const auto anchor = order.empty()
        ? std::optional<spice::sct::SctFooterEntryId>{}
        : std::optional{order.back()};
    SctSemanticOperationBatch batch;
    batch.operations.push_back(SctInsertFooterEntryAfterOperation{anchor, copy});
    batch.operations.push_back(SctReplaceParameterValueOperation{
        site, spice::sct::SctFooterEntryReference{newId}});
    return commit(std::move(batch), {}, "Make private footer text copy",
        SelectionHints{instructionTarget, instructionTarget});
}

SctEditResult SctEditSession::createScriptSection(
    std::string name, const std::optional<spice::sct::SctSectionId> after,
    const bool includeReturn) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (!validAuthoredSectionName(name)) return failure({editError(locator,
        "InvalidAuthoredSectionName", "Section names must match [A-Za-z0-9_]{1,16}.")});
    if (std::ranges::any_of(workingState_.sectionOrder(), [&](const auto id) {
            const auto* value = workingState_.section(id);
            return value != nullptr && value->nameBytes == name;
        })) return failure({editError(locator, "DuplicateSectionName",
            "A section with that exact name already exists.")});
    if (after && workingState_.section(*after) == nullptr)
        return failure({editError(locator, "SectionAnchorNotFound", "The selected section no longer exists.")});

    spice::sct::SctDocument context;
    auto created = spice::sct::SctDocumentEntityFactory::createScriptSection(context, name);
    if (!created.section) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : created.diagnostics)
            diagnostics.push_back(convertSctDiagnostic(diagnostic, SctPipelineStage::Edit, locator));
        return failure(std::move(diagnostics));
    }
    auto section = std::move(*created.section);
    section.id = spice::sct::SctSectionId(workingState_.nextSectionIdValue());
    auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section.content);
    auto label = makeInstruction(9u, workingState_.nextInstructionIdValue());
    if (!label) return failure({editError(locator, "SectionLabelCreationFailed",
        "The mandatory section label instruction could not be constructed.")});
    script->instructions.push_back(*label);
    if (includeReturn) {
        auto terminal = makeInstruction(12u, workingState_.nextInstructionIdValue() + 1u);
        if (!terminal) return failure({editError(locator, "SectionReturnCreationFailed",
            "The optional terminal Return instruction could not be constructed.")});
        script->instructions.push_back(*terminal);
    }
    auto insertionAnchor = after;
    if (!insertionAnchor && !workingState_.sectionOrder().empty())
        insertionAnchor = workingState_.sectionOrder().back();
    const auto id = section.id;
    const SctNavigationTarget target{SctNavigationKind::Section, id.value()};
    return commit(SctSemanticOperationBatch{{SctInsertSectionAfterOperation{
            insertionAnchor, std::move(section)}}}, {}, "Create script section " + name,
        SelectionHints{after ? std::optional<SctNavigationTarget>{SctNavigationTarget{
            SctNavigationKind::Section, after->value()}} : std::nullopt, target});
}

SctEditResult SctEditSession::createIndexedString(
    std::string name, const std::optional<spice::sct::SctSectionId> after) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (!validAuthoredSectionName(name)) return failure({editError(locator,
        "InvalidAuthoredSectionName", "Section names must match [A-Za-z0-9_]{1,16}.")});
    if (std::ranges::any_of(workingState_.sectionOrder(), [&](const auto id) {
            const auto* value = workingState_.section(id);
            return value != nullptr && value->nameBytes == name;
        })) return failure({editError(locator, "DuplicateSectionName",
            "A section with that exact name already exists.")});
    SctMessageDraft draft;
    const auto message = SctMessageAuthoringProfile::materialize(draft);
    if (!message.message) return failure({editError(locator, "DefaultMessageCreationFailed",
        "The default SCT message could not be constructed.")});
    spice::sct::SctDocument context;
    auto created = spice::sct::SctDocumentEntityFactory::createIndexedStringSection(
        context, name, *message.message);
    if (!created.section) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : created.diagnostics)
            diagnostics.push_back(convertSctDiagnostic(diagnostic, SctPipelineStage::Edit, locator));
        return failure(std::move(diagnostics));
    }
    auto section = std::move(*created.section);
    section.id = spice::sct::SctSectionId(workingState_.nextSectionIdValue());
    auto& strings = std::get<spice::sct::SctStringSectionContent>(section.content);
    strings.string.id = spice::sct::SctStringId(workingState_.nextStringIdValue());
    const auto id = strings.string.id;
    auto insertionAnchor = after;
    if (!insertionAnchor && !workingState_.sectionOrder().empty())
        insertionAnchor = workingState_.sectionOrder().back();
    return commit(SctSemanticOperationBatch{{SctInsertSectionAfterOperation{insertionAnchor, std::move(section)}}},
        {}, "Create indexed string " + name,
        SelectionHints{{}, SctNavigationTarget{SctNavigationKind::String, id.value()}});
}

SctEditResult SctEditSession::renameSection(
    const spice::sct::SctSectionId sectionId, std::string name) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* section = workingState_.section(sectionId);
    if (section == nullptr) return failure({editError(locator, "SectionNotFound", "The section no longer exists.")});
    if (!validAuthoredSectionName(name)) return failure({editError(locator,
        "InvalidAuthoredSectionName", "Section names must match [A-Za-z0-9_]{1,16}.")});
    if (section->nameBytes == name) {
        SctEditResult result;
        result.revision = history_.currentRevision().id;
        result.snapshot = currentSnapshot_;
        result.suggestedSelection = SctNavigationTarget{
            SctNavigationKind::Section, sectionId.value()};
        return result;
    }
    if (std::ranges::any_of(workingState_.sectionOrder(), [&](const auto id) {
            const auto* value = workingState_.section(id);
            return id != sectionId && value != nullptr && value->nameBytes == name;
        })) return failure({editError(locator, "DuplicateSectionName", "A section with that exact name already exists.")});
    const SctNavigationTarget target{SctNavigationKind::Section, sectionId.value()};
    return commit(SctSemanticOperationBatch{{SctRenameSectionOperation{sectionId, std::move(name)}}},
        {}, "Rename section", SelectionHints{target, target});
}

SctEditResult SctEditSession::deleteSection(const spice::sct::SctSectionId sectionId) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* section = workingState_.section(sectionId);
    if (section == nullptr) return failure({editError(locator, "SectionNotFound", "The section no longer exists.")});
    if (!std::holds_alternative<spice::sct::SctScriptSectionContent>(section->content))
        return failure({editError(locator, "SectionLifecycleKindUnsupported",
            "This command deletes script sections only. Use Delete Text for indexed strings.")});
    std::unordered_set<std::uint64_t> internal;
    for (const auto id : workingState_.instructionOrder(sectionId)) internal.insert(id.value());
    std::vector<SctPipelineDiagnostic> blockers;
    for (const auto id : workingState_.instructionOrder(sectionId)) {
        for (const auto source : workingState_.inboundReferenceSources(
                spice::sct::SctDocumentReferenceTarget{id})) {
            if (!internal.contains(source.value())) blockers.push_back(editError(locator,
                "SectionHasExternalReference", "The section cannot be deleted while an instruction outside it references one of its instructions.",
                SctNavigationTarget{SctNavigationKind::Instruction, source.value()}));
        }
        for (const auto attachment : workingState_.opaqueAttachments(spice::sct::SctOpaqueAnchor{id}))
            blockers.push_back(editError(locator, "SectionInstructionHasOpaqueAttachment",
                "The section cannot be deleted while opaque source data is anchored to an instruction.",
                SctNavigationTarget{SctNavigationKind::OpaqueAttachment, attachment.value()}));
    }
    for (const auto attachment : workingState_.opaqueAttachments(spice::sct::SctOpaqueAnchor{sectionId}))
        blockers.push_back(editError(locator, "SectionHasOpaqueAttachment",
            "The section cannot be deleted while opaque source data is anchored to it.",
            SctNavigationTarget{SctNavigationKind::OpaqueAttachment, attachment.value()}));
    if (!blockers.empty()) return failure(std::move(blockers));
    SctStructuredAuthoringOperationBatch authored;
    for (const auto& arm : structuredAuthoring_.arms()) {
        if (arm.controller.section == sectionId)
            authored.operations.push_back({arm.id, arm, std::nullopt});
    }
    const auto placement = workingState_.sectionPlacement(sectionId);
    const SctNavigationTarget removed{SctNavigationKind::Section, sectionId.value()};
    std::optional<SctNavigationTarget> next;
    const auto order = workingState_.sectionOrder();
    const auto position = std::ranges::find(order, sectionId);
    if (position != order.end() && std::next(position) != order.end())
        next = SctNavigationTarget{SctNavigationKind::Section, std::next(position)->value()};
    else if (placement && placement->after)
        next = SctNavigationTarget{SctNavigationKind::Section, placement->after->value()};
    return commit(SctSemanticOperationBatch{{SctDeleteSectionOperation{sectionId}}},
        std::move(authored), "Delete script section " + section->nameBytes,
        SelectionHints{removed, next});
}

SctEditResult SctEditSession::moveSection(
    const spice::sct::SctSectionId sectionId, const SctSectionMoveDirection direction) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* section = workingState_.section(sectionId);
    if (section == nullptr) return failure({editError(locator, "SectionNotFound", "The section no longer exists.")});
    if (!std::holds_alternative<spice::sct::SctScriptSectionContent>(section->content))
        return failure({editError(locator, "SectionLifecycleKindUnsupported", "Only script sections can be moved with this command.")});
    const auto order = workingState_.sectionOrder();
    const auto position = std::ranges::find(order, sectionId);
    if (position == order.end()) return failure({editError(locator, "SectionNotFound", "The section no longer exists.")});
    std::optional<spice::sct::SctSectionId> anchor;
    if (direction == SctSectionMoveDirection::Up) {
        if (position == order.begin()) return failure({editError(locator, "SectionMoveAtBoundary", "The section is already first.")});
        const auto previous = std::prev(position);
        if (previous != order.begin()) anchor = *std::prev(previous);
    } else {
        if (std::next(position) == order.end()) return failure({editError(locator, "SectionMoveAtBoundary", "The section is already last.")});
        anchor = *std::next(position);
    }
    const SctNavigationTarget target{SctNavigationKind::Section, sectionId.value()};
    return commit(SctSemanticOperationBatch{{SctRelocateSectionAfterOperation{sectionId, anchor}}},
        {}, direction == SctSectionMoveDirection::Up ? "Move section up" : "Move section down",
        SelectionHints{target, target});
}

SctEditResult SctEditSession::createFooterText(
    const SctCreatedFooterTextKind kind,
    const std::optional<spice::sct::SctFooterEntryId> after) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    if (kind == SctCreatedFooterTextKind::PlainText) {
        return failure({editError(locator, "StandaloneFooterPlainTextUnsupported",
            "Plain footer text is created as an instruction-owned parameter.")});
    }
    spice::sct::SctTextKind textKind = spice::sct::SctTextKind::PlainString;
    spice::sct::SctTextValue value = spice::sct::SctPlainText{};
    if (kind == SctCreatedFooterTextKind::Message) {
        textKind = spice::sct::SctTextKind::SctString;
        const auto created = SctMessageAuthoringProfile::materialize(SctMessageDraft{});
        if (!created.message) return failure({editError(locator, "DefaultMessageCreationFailed", "The default SCT message could not be constructed.")});
        value = *created.message;
    }
    spice::sct::SctDocument context;
    auto created = spice::sct::SctDocumentEntityFactory::createFooterEntry(context, textKind, value);
    if (!created.entry) {
        std::vector<SctPipelineDiagnostic> diagnostics;
        for (const auto& diagnostic : created.diagnostics)
            diagnostics.push_back(convertSctDiagnostic(diagnostic, SctPipelineStage::Edit, locator));
        return failure(std::move(diagnostics));
    }
    auto entry = std::move(*created.entry);
    entry.id = spice::sct::SctFooterEntryId(workingState_.nextFooterEntryIdValue());
    const auto id = entry.id;
    auto insertionAnchor = after;
    if (!insertionAnchor && !workingState_.footerEntryOrder().empty())
        insertionAnchor = workingState_.footerEntryOrder().back();
    return commit(SctSemanticOperationBatch{{SctInsertFooterEntryAfterOperation{insertionAnchor, std::move(entry)}}},
        {}, kind == SctCreatedFooterTextKind::Message ? "Create footer message" : "Create footer text",
        SelectionHints{{}, SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()}});
}

SctEditResult SctEditSession::deleteTextEntity(const SctTextTarget& target) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto navigation = navigationFor(target);
    if (workingState_.textValue(target) == nullptr)
        return failure({editError(locator, "TextTargetNotFound", "The text entity no longer exists.", navigation)});
    const auto references = std::visit([&](const auto id) {
        return workingState_.inboundReferenceSources(spice::sct::SctDocumentReferenceTarget{id});
    }, target);
    if (!references.empty()) return failure({editError(locator, "TextHasIncomingReference",
        "The text entity cannot be deleted while an instruction references it.",
        SctNavigationTarget{SctNavigationKind::Instruction, references.front().value()})});
    const auto attachments = std::visit([&](const auto id) {
        return workingState_.opaqueAttachments(spice::sct::SctOpaqueAnchor{id});
    }, target);
    if (!attachments.empty()) return failure({editError(locator, "TextHasOpaqueAttachment",
        "The text entity cannot be deleted while opaque source data is anchored to it.",
        SctNavigationTarget{SctNavigationKind::OpaqueAttachment, attachments.front().value()})});
    return std::visit([&](const auto id) -> SctEditResult {
        using Id = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<Id, spice::sct::SctStringId>) {
            for (const auto sectionId : workingState_.sectionOrder()) {
                const auto* section = workingState_.section(sectionId);
                const auto* content = section == nullptr ? nullptr
                    : std::get_if<spice::sct::SctStringSectionContent>(&section->content);
                if (content != nullptr && content->string.id == id)
                    return commit(SctSemanticOperationBatch{{SctDeleteSectionOperation{sectionId}}},
                        {}, "Delete indexed string", SelectionHints{navigation,
                            SctNavigationTarget{SctNavigationKind::Document, 0}});
            }
            return failure({editError(locator, "TextSectionNotFound", "The indexed string section no longer exists.", navigation)});
        } else {
            return commit(SctSemanticOperationBatch{{SctDeleteFooterEntryOperation{id}}},
                {}, "Delete footer text", SelectionHints{navigation,
                    SctNavigationTarget{SctNavigationKind::Document, 0}});
        }
    }, target);
}

SctEditResult SctEditSession::addVirtualElse(
    const spice::sct::SctInstructionId controller) {
    const auto& locator = baselineSnapshot_->provenance->source().descriptor.locator;
    const auto* region = verifiedRegion(*currentSnapshot_, controller);
    if (region == nullptr
        || region->id.kind != spice::sct::SctStructuredRegionKind::If) {
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
                && arm.kind == spice::sct::SctStructuredArmKind::Else;
        })) {
        return failure({editError(locator, "ElseAlreadyExists",
            "This If already has an authored Else arm.")});
    }
    const auto id = structuredAuthoring_.nextId();
    SctAuthoredArm arm{id,
        {region->id.section, controller},
        spice::sct::SctStructuredArmKind::Else};
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
        || region->id.kind != spice::sct::SctStructuredRegionKind::Switch) {
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
        spice::sct::SctStructuredArmKind::SwitchCase};
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
        || current->kind != spice::sct::SctStructuredArmKind::SwitchCase
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
        current->kind == spice::sct::SctStructuredArmKind::Else
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
    if (current->kind == spice::sct::SctStructuredArmKind::SwitchCase
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

    if (current->kind == spice::sct::SctStructuredArmKind::Else) {
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
            == spice::sct::SctStructuredArmKind::SwitchCase) {
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
        current->kind == spice::sct::SctStructuredArmKind::Else
            ? "Insert first Else instruction" : "Insert first Switch case instruction",
        SelectionHints{{SctNavigationTarget{SctNavigationKind::Instruction,
            current->controller.instruction.value()}}, inserted},
        elapsedMicroseconds(preflightStart));
}

SctEditResult SctEditSession::insertInstructionIntoStructuredArm(
    const spice::sct::SctInstructionId controller,
    const spice::sct::SctStructuredArmKind armKind,
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
    const auto* section = currentSnapshot_->analysis->structuredControlFlow.findSection(
        region->id.section);
    if (section == nullptr) {
        return failure({editError(locator, "StructuredSectionNotFound",
            "The verified section is no longer available.")});
    }
    for (const auto blockId : arm->blocks) {
        const auto block = std::ranges::find(
            section->blocks, blockId, &spice::sct::SctStructuredBasicBlock::id);
        if (block != section->blocks.end())
            members.insert(members.end(), block->instructions.begin(), block->instructions.end());
    }
    std::vector<spice::sct::SctInstructionId> scaffolding;
    for (const auto& evidence : region->evidence) {
        if ((evidence.kind == spice::sct::SctStructureEvidenceKind::PreTargetJump
                || evidence.kind
                    == spice::sct::SctStructureEvidenceKind::BackwardTerminatorJump
                || evidence.kind
                    == spice::sct::SctStructureEvidenceKind::CommonForwardExit)
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
    if (current->kind == spice::sct::SctStructuredArmKind::Else) {
        if (!current->expectedJoin
            || !setInstructionReference(controller, 1u, *current->expectedJoin)) {
            return failure({editError(locator, "IfFalseTargetUnavailable",
                "The authored Else can no longer restore the verified If join.")});
        }
    } else if (current->kind
            == spice::sct::SctStructuredArmKind::SwitchCase) {
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
        current->kind == spice::sct::SctStructuredArmKind::Else
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
    if (source.state->externalBefore) {
        installExternalState(*source.state->externalBefore);
        const auto navigation = history_.undo();
        assert(navigation.has_value());
        verifiedRevision_ = navigation->to;
        rebuildSemanticProjection();
        SctEditResult result;
        result.committed = true;
        result.revision = navigation->to;
        result.snapshot = currentSnapshot_;
        result.changes = source.state->reverseChanges;
        result.transition = SctRevisionTransition{SctRevisionTransitionKind::Undo,
            navigation->from, navigation->to, result.changes, std::nullopt,
            SctRevisionVerification::Verified};
        return result;
    }
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
    if (target->state->externalAfter) {
        installExternalState(*target->state->externalAfter);
        const auto navigation = history_.redo();
        assert(navigation.has_value());
        verifiedRevision_ = navigation->to;
        rebuildSemanticProjection();
        SctEditResult result;
        result.committed = true;
        result.revision = navigation->to;
        result.snapshot = currentSnapshot_;
        result.changes = target->state->forwardChanges;
        result.transition = SctRevisionTransition{SctRevisionTransitionKind::Redo,
            navigation->from, navigation->to, result.changes, std::nullopt,
            SctRevisionVerification::Verified};
        return result;
    }
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

std::optional<SctCheckpointRequest> SctEditSession::checkpointRequest(
    const std::uint64_t generation) const {
    auto request = materializationRequest(generation);
    if (!request.has_value()) return std::nullopt;
    const auto historyState = history_.revision(history_.currentRevision().id);
    if (!historyState.has_value()) return std::nullopt;
    return SctCheckpointRequest{
        history_.currentRevision().id,
        historyState->state,
        baselineSnapshot_,
        std::move(*request),
        workingState_.textRepairProvenances(),
    };
}

std::optional<SctPublicationRevision> SctEditSession::capturePublicationRevision(
    const std::uint64_t generation) const {
    const auto revision = history_.currentRevision();
    if (!revision.id.valid() || revision.state == nullptr) return std::nullopt;
    SctPublicationRevision result;
    result.revision = revision.id;
    result.historyStateToken = revision.state;
    result.provenance = baselineSnapshot_->provenance;
    if (verifiedRevision_ == revision.id && currentSnapshot_
        && currentSnapshot_->document) {
        result.verifiedSnapshot = currentSnapshot_;
    } else {
        result.materialization = materializationRequest(generation);
        if (!result.materialization) return std::nullopt;
    }
    return result;
}

bool SctEditSession::markPatchCheckpoint(const RevisionId revision,
    std::shared_ptr<const void> historyStateToken) noexcept {
    if (historyStateToken == nullptr) return false;
    return history_.markCheckpoint(revision,
        std::static_pointer_cast<const RevisionDelta>(std::move(historyStateToken)));
}

std::vector<SctPipelineDiagnostic> SctEditSession::currentDiagnostics() const {
    std::vector<SctPipelineDiagnostic> result;
    result.reserve(currentSnapshot_->diagnostics.size());
    for (const auto& diagnostic : currentSnapshot_->diagnostics) {
        if (diagnostic.code == "AmbiguousString" && diagnostic.primaryLocation) {
            const auto location = inspectionLocationForDiagnostic(
                *diagnostic.primaryLocation);
            if (location) {
                const auto target = owningNavigationTarget(*location);
                if (target.kind == SctNavigationKind::String
                    || target.kind == SctNavigationKind::FooterEntry) {
                    const SctTextTarget textTarget = target.kind == SctNavigationKind::String
                        ? SctTextTarget{spice::sct::SctStringId(target.id)}
                        : SctTextTarget{spice::sct::SctFooterEntryId(target.id)};
                    const auto* value = workingState_.textValue(textTarget);
                    if (value == nullptr
                        || !std::holds_alternative<spice::sct::SctOpaqueText>(*value)) {
                        continue;
                    }
                }
            }
        }
        result.push_back(diagnostic);
    }
    return result;
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

const std::vector<SctInsertableOpcode>& SctEditSession::authorableOpcodes() {
    static const auto choices = [] {
        std::vector<SctInsertableOpcode> result;
        for (const auto& schema : spice::sct::sctOpcodeSchemas()) {
            if (schema.opcode == 9u
                || schema.documentRole == spice::sct::SctOpcodeDocumentRole::FoldedModifier)
                continue;
            spice::sct::SctInstructionFactoryRequest request;
            request.opcode = schema.opcode;
            const auto draft = spice::sct::SctInstructionFactory::createDraft(request);
            if (!draft.draft.has_value()) continue;
            result.push_back({schema.opcode,
                schema.semantic.mnemonic.empty()
                    ? "Opcode " + std::to_string(schema.opcode)
                    : std::string(schema.semantic.mnemonic)});
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

void SctEditSession::appendOrphanedFooterPlainTextCleanup(
    SctSemanticOperationBatch& operation) const {
    using DeltaMap = std::unordered_map<spice::sct::SctFooterEntryId, std::int64_t>;
    DeltaMap deltas;
    const auto addValue = [&](const spice::sct::SctDocumentParameterValue& value,
                              const std::int64_t amount) {
        if (const auto* reference =
                std::get_if<spice::sct::SctFooterEntryReference>(&value))
            deltas[reference->target] += amount;
    };
    const auto addInstruction = [&](const spice::sct::SctDocumentInstruction& instruction,
                                    const std::int64_t amount) {
        for (const auto& parameter : instruction.fixedParameters)
            addValue(parameter.value, amount);
        for (const auto& group : instruction.repeatedParameterGroups)
            for (const auto& parameter : group.parameters)
                addValue(parameter.value, amount);
    };
    const auto addGroup = [&](const spice::sct::SctDocumentRepeatedParameterGroup& group,
                              const std::int64_t amount) {
        for (const auto& parameter : group.parameters) addValue(parameter.value, amount);
    };

    for (const auto& primitive : operation.operations) {
        std::visit([&](const auto& typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, SctInsertInstructionAfterOperation>) {
                addInstruction(typed.instruction, 1);
            } else if constexpr (std::is_same_v<T, SctDeleteInstructionOperation>) {
                if (const auto* current = workingState_.instruction(typed.instruction))
                    addInstruction(*current, -1);
            } else if constexpr (std::is_same_v<T, SctReplaceInstructionOperation>) {
                if (const auto* current = workingState_.instruction(typed.instruction))
                    addInstruction(*current, -1);
                addInstruction(typed.replacement, 1);
            } else if constexpr (std::is_same_v<T, SctReplaceParameterValueOperation>) {
                if (const auto* current = workingState_.parameter(typed.site))
                    addValue(current->value, -1);
                addValue(typed.value, 1);
            } else if constexpr (std::is_same_v<T, SctInsertRepeatedGroupOperation>) {
                addGroup(typed.group, 1);
            } else if constexpr (std::is_same_v<T, SctDeleteRepeatedGroupOperation>) {
                if (const auto* instruction = workingState_.instruction(typed.instruction);
                    instruction != nullptr
                    && typed.ordinal < instruction->repeatedParameterGroups.size())
                    addGroup(instruction->repeatedParameterGroups[typed.ordinal], -1);
            } else if constexpr (std::is_same_v<T, SctDeleteSectionOperation>) {
                for (const auto instruction : workingState_.instructionOrder(typed.section))
                    if (const auto* current = workingState_.instruction(instruction))
                        addInstruction(*current, -1);
            } else if constexpr (std::is_same_v<T, SctInsertSectionAfterOperation>) {
                if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                        &typed.section.content))
                    for (const auto& instruction : script->instructions)
                        addInstruction(instruction, 1);
            }
        }, primitive);
    }

    std::unordered_set<spice::sct::SctFooterEntryId> alreadyDeleted;
    for (const auto& primitive : operation.operations)
        if (const auto* deletion = std::get_if<SctDeleteFooterEntryOperation>(&primitive))
            alreadyDeleted.insert(deletion->entry);

    for (const auto id : workingState_.footerEntryOrder()) {
        const auto delta = deltas.contains(id) ? deltas.at(id) : 0;
        if (delta >= 0 || alreadyDeleted.contains(id)) continue;
        const auto current = static_cast<std::int64_t>(
            workingState_.referenceOccurrenceCount(
                spice::sct::SctDocumentReferenceTarget{id}));
        if (current <= 0 || current + delta != 0) continue;
        const auto* entry = workingState_.footerEntry(id);
        if (entry == nullptr || entry->kind != spice::sct::SctTextKind::PlainString)
            continue;
        if (!workingState_.opaqueAttachments(spice::sct::SctOpaqueAnchor{id}).empty())
            continue;
        operation.operations.push_back(SctDeleteFooterEntryOperation{id});
    }
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

    appendOrphanedFooterPlainTextCleanup(operation);

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
    if (!currentSnapshot_ || !currentSnapshot_->analysis) {
        semanticProjection_.reset();
        return;
    }
    semanticProjection_ = std::make_shared<const SctSemanticEditorProjection>(
        SctSemanticEditorProjection::build(currentSnapshot_->analysis->structuredControlFlow,
            workingState_, structuredAuthoring_, history_.currentRevision().id,
            verifiedRevision_));
}

void SctEditSession::installExternalState(
    const RevisionDelta::ExternalState& state) {
    assert(state.snapshot != nullptr);
    assert(state.snapshot->document != nullptr);
    workingState_ = SctWorkingState(state.snapshot->document, state.textRepairs);
    structuredAuthoring_ = SctStructuredAuthoringState(state.authoredArms);
    materializedDocument_ = state.snapshot->document;
    currentSnapshot_ = state.snapshot;
    structurallyValid_ = state.snapshot->readiness
        == spice::sct::SctDocumentReadiness::StructurallyValid;
}

void SctEditSession::pruneMaterializationCheckpoints() {
    std::erase_if(materializationCheckpoints_, [this](const auto& checkpoint) {
        return !history_.revision(checkpoint.revision).has_value();
    });
}

}  // namespace salsa::core
