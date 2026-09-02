#include "SalsaCore/Sct/SctSemanticOperation.h"

#include "SpiceSCT/SctDocumentIndex.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

struct PrimitiveApplication final {
    std::optional<SctPrimitiveOperation> inverse{};
    SctEditChangeSet forwardChanges{};
    SctEditChangeSet reverseChanges{};
    std::optional<SctOperationIssue> issue{};
};

[[nodiscard]] SctNavigationTarget instructionTarget(
    const spice::sct::SctInstructionId id) {
    return {SctNavigationKind::Instruction, id.value()};
}

[[nodiscard]] SctNavigationTarget messageTarget(const SctMessageTarget& target) {
    return std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return SctNavigationTarget{SctNavigationKind::String, id.value()};
        else
            return SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
    }, target);
}

[[nodiscard]] SctOperationIssue issue(
    std::string code,
    std::string message,
    std::optional<SctNavigationTarget> target = std::nullopt) {
    return {std::move(code), std::move(message), target};
}

[[nodiscard]] spice::sct::SctDocumentSection* findSection(
    spice::sct::SctDocument& document,
    const spice::sct::SctSectionId id) {
    const auto found = std::ranges::find(
        document.sections, id, &spice::sct::SctDocumentSection::id);
    return found == document.sections.end() ? nullptr : &*found;
}

[[nodiscard]] spice::sct::SctDocumentString* findString(
    spice::sct::SctDocument& document,
    const spice::sct::SctStringId id) {
    for (auto& section : document.sections) {
        if (auto* content = std::get_if<spice::sct::SctStringSectionContent>(&section.content);
            content != nullptr && content->string.id == id) return &content->string;
    }
    return nullptr;
}

[[nodiscard]] spice::sct::SctDocumentFooterEntry* findFooterEntry(
    spice::sct::SctDocument& document,
    const spice::sct::SctFooterEntryId id) {
    const auto found = std::ranges::find(
        document.footerEntries, id, &spice::sct::SctDocumentFooterEntry::id);
    return found == document.footerEntries.end() ? nullptr : &*found;
}

void appendChanges(SctEditChangeSet& target, SctEditChangeSet source) {
    target.created.insert(target.created.end(),
        std::make_move_iterator(source.created.begin()),
        std::make_move_iterator(source.created.end()));
    target.removed.insert(target.removed.end(),
        std::make_move_iterator(source.removed.begin()),
        std::make_move_iterator(source.removed.end()));
    target.moved.insert(target.moved.end(),
        std::make_move_iterator(source.moved.begin()),
        std::make_move_iterator(source.moved.end()));
    target.modified.insert(target.modified.end(),
        std::make_move_iterator(source.modified.begin()),
        std::make_move_iterator(source.modified.end()));
}

[[nodiscard]] PrimitiveApplication applyInsert(
    spice::sct::SctDocument& document,
    const SctInsertInstructionAfterOperation& operation) {
    const auto target = instructionTarget(operation.instruction.id);
    const auto index = spice::sct::SctDocumentIndex::build(document);
    if (index.find(operation.instruction.id) != nullptr) {
        return {.issue = issue("InstructionAlreadyExists",
            "The inserted instruction ID already exists.", target)};
    }
    const auto location = index.instructionLocation(operation.anchor);
    if (!location.has_value()) {
        return {.issue = issue("InstructionAnchorNotFound",
            "The instruction insertion anchor does not exist.",
            instructionTarget(operation.anchor))};
    }
    auto* section = findSection(document, location->sectionId);
    auto* script = section == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&section->content);
    if (script == nullptr || location->instructionOrdinal >= script->instructions.size()) {
        return {.issue = issue("InstructionAnchorNotScript",
            "The instruction insertion anchor is not in a script section.",
            instructionTarget(operation.anchor))};
    }

    while (document.nextInstructionIdValue() <= operation.instruction.id.value())
        (void)document.allocateInstructionId();
    script->instructions.insert(
        script->instructions.begin()
            + static_cast<std::ptrdiff_t>(location->instructionOrdinal + 1u),
        operation.instruction);
    return {
        SctDeleteInstructionOperation{operation.instruction.id},
        SctEditChangeSet{{target}, {}, {}, {}},
        SctEditChangeSet{{}, {target}, {}, {}},
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyDelete(
    spice::sct::SctDocument& document,
    const SctDeleteInstructionOperation& operation) {
    const auto target = instructionTarget(operation.instruction);
    const auto index = spice::sct::SctDocumentIndex::build(document);
    const auto location = index.instructionLocation(operation.instruction);
    const auto* existing = index.find(operation.instruction);
    if (!location.has_value() || existing == nullptr) {
        return {.issue = issue("InstructionNotFound",
            "The deleted instruction does not exist.", target)};
    }
    auto* section = findSection(document, location->sectionId);
    auto* script = section == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&section->content);
    if (script == nullptr || location->instructionOrdinal == 0u
        || location->instructionOrdinal >= script->instructions.size()) {
        return {.issue = issue("InstructionCannotBeRestored",
            "The operation cannot delete the first instruction in a section.", target)};
    }
    const auto removed = script->instructions[location->instructionOrdinal];
    const auto anchor = script->instructions[location->instructionOrdinal - 1u].id;
    script->instructions.erase(script->instructions.begin()
        + static_cast<std::ptrdiff_t>(location->instructionOrdinal));
    return {
        SctInsertInstructionAfterOperation{anchor, removed},
        SctEditChangeSet{{}, {target}, {}, {}},
        SctEditChangeSet{{target}, {}, {}, {}},
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyRelocate(
    spice::sct::SctDocument& document,
    const SctRelocateInstructionAfterOperation& operation) {
    const auto target = instructionTarget(operation.instruction);
    if (operation.instruction == operation.anchor) {
        return {.issue = issue("InstructionMoveSelfAnchor",
            "An instruction cannot be positioned relative to itself.", target)};
    }
    const auto index = spice::sct::SctDocumentIndex::build(document);
    const auto sourceLocation = index.instructionLocation(operation.instruction);
    const auto anchorLocation = index.instructionLocation(operation.anchor);
    if (!sourceLocation.has_value() || !anchorLocation.has_value()) {
        return {.issue = issue("InstructionMoveTargetNotFound",
            "The moved instruction or its destination anchor does not exist.", target)};
    }
    if (sourceLocation->sectionId != anchorLocation->sectionId) {
        return {.issue = issue("InstructionMoveCrossesSection",
            "An instruction operation cannot relocate across sections.", target)};
    }
    auto* section = findSection(document, sourceLocation->sectionId);
    auto* script = section == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&section->content);
    if (script == nullptr || sourceLocation->instructionOrdinal == 0u) {
        return {.issue = issue("InstructionMoveInvalidSource",
            "The instruction cannot be relocated from this position.", target)};
    }
    const auto oldAnchor = script->instructions[sourceLocation->instructionOrdinal - 1u].id;
    if (oldAnchor == operation.anchor) {
        return {.issue = issue("InstructionMoveNoChange",
            "The instruction is already after the requested anchor.", target)};
    }
    auto moved = script->instructions[sourceLocation->instructionOrdinal];
    script->instructions.erase(script->instructions.begin()
        + static_cast<std::ptrdiff_t>(sourceLocation->instructionOrdinal));
    const auto destination = std::ranges::find(
        script->instructions, operation.anchor,
        &spice::sct::SctDocumentInstruction::id);
    if (destination == script->instructions.end()) {
        return {.issue = issue("InstructionMoveAnchorLost",
            "The instruction destination anchor could not be resolved.", target)};
    }
    script->instructions.insert(std::next(destination), std::move(moved));
    return {
        SctRelocateInstructionAfterOperation{operation.instruction, oldAnchor},
        SctEditChangeSet{{}, {}, {target}, {}},
        SctEditChangeSet{{}, {}, {target}, {}},
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyReplaceMessage(
    spice::sct::SctDocument& document,
    const SctReplaceMessageOperation& operation) {
    const auto target = messageTarget(operation.target);
    spice::sct::SctTextValue* value = std::visit([&](const auto id)
        -> spice::sct::SctTextValue* {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            auto* string = findString(document, id);
            return string != nullptr && string->kind == spice::sct::SctTextKind::SctString
                ? &string->value : nullptr;
        } else {
            auto* entry = findFooterEntry(document, id);
            return entry != nullptr && entry->kind == spice::sct::SctTextKind::SctString
                ? &entry->value : nullptr;
        }
    }, operation.target);
    auto* existing = value == nullptr ? nullptr
        : std::get_if<spice::sct::SctMessage>(value);
    if (existing == nullptr) {
        return {.issue = issue("MessageTargetNotFound",
            "The replaced message does not exist or is not semantic SCT text.", target)};
    }
    auto previous = *existing;
    *value = operation.message;
    return {
        SctReplaceMessageOperation{operation.target, std::move(previous)},
        SctEditChangeSet{{}, {}, {}, {target}},
        SctEditChangeSet{{}, {}, {}, {target}},
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyPrimitive(
    spice::sct::SctDocument& document,
    const SctPrimitiveOperation& operation) {
    return std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SctInsertInstructionAfterOperation>)
            return applyInsert(document, typed);
        else if constexpr (std::is_same_v<T, SctDeleteInstructionOperation>)
            return applyDelete(document, typed);
        else if constexpr (std::is_same_v<T, SctRelocateInstructionAfterOperation>)
            return applyRelocate(document, typed);
        else
            return applyReplaceMessage(document, typed);
    }, operation);
}

}  // namespace

SctOperationApplication SctSemanticOperationService::apply(
    const spice::sct::SctDocument& document,
    const SctSemanticOperationBatch& batch) {
    if (batch.operations.empty()) {
        SctOperationApplication result;
        result.issues.push_back(issue(
            "EmptyOperationBatch", "A semantic operation batch cannot be empty."));
        return result;
    }

    auto candidate = std::make_shared<spice::sct::SctDocument>(document);
    SctOperationApplication result;
    std::vector<SctEditChangeSet> reverseChanges;
    reverseChanges.reserve(batch.operations.size());
    for (const auto& operation : batch.operations) {
        auto applied = applyPrimitive(*candidate, operation);
        if (applied.issue.has_value()) {
            result.issues.push_back(std::move(*applied.issue));
            return result;
        }
        result.inverse.operations.insert(
            result.inverse.operations.begin(), std::move(*applied.inverse));
        appendChanges(result.forwardChanges, std::move(applied.forwardChanges));
        reverseChanges.push_back(std::move(applied.reverseChanges));
    }
    for (auto changes = reverseChanges.rbegin(); changes != reverseChanges.rend(); ++changes)
        appendChanges(result.reverseChanges, std::move(*changes));
    result.document = std::move(candidate);
    return result;
}

}  // namespace salsa::core
