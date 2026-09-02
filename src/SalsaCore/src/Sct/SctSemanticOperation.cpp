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

[[nodiscard]] SctNavigationTarget textTarget(const SctTextTarget& target) {
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
    target.sections.insert(target.sections.end(),
        std::make_move_iterator(source.sections.begin()),
        std::make_move_iterator(source.sections.end()));
    target.instructions.insert(target.instructions.end(),
        std::make_move_iterator(source.instructions.begin()),
        std::make_move_iterator(source.instructions.end()));
    target.footerEntries.insert(target.footerEntries.end(),
        std::make_move_iterator(source.footerEntries.begin()),
        std::make_move_iterator(source.footerEntries.end()));
    target.textValues.insert(target.textValues.end(),
        std::make_move_iterator(source.textValues.begin()),
        std::make_move_iterator(source.textValues.end()));
    target.modified.insert(target.modified.end(),
        std::make_move_iterator(source.modified.begin()),
        std::make_move_iterator(source.modified.end()));
    target.structuredAuthoring.insert(target.structuredAuthoring.end(),
        std::make_move_iterator(source.structuredAuthoring.begin()),
        std::make_move_iterator(source.structuredAuthoring.end()));
    target.invalidations = target.invalidations | source.invalidations;
    target.documentChanged = target.documentChanged || source.documentChanged;
}

[[nodiscard]] SctEditChangeSet instructionChangeSet(
    SctInstructionStructuralChange change, std::vector<SctNavigationTarget> modified = {}) {
    SctEditChangeSet result;
    result.instructions.push_back(std::move(change));
    result.modified = std::move(modified);
    result.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
    result.documentChanged = true;
    return result;
}

[[nodiscard]] PrimitiveApplication applyInsertSection(
    spice::sct::SctDocument& document,
    const SctInsertSectionAfterOperation& operation) {
    const SctNavigationTarget target{SctNavigationKind::Section, operation.section.id.value()};
    if (findSection(document, operation.section.id) != nullptr)
        return {.issue = issue("SectionAlreadyExists", "The inserted section ID already exists.", target)};
    auto destination = document.sections.begin();
    if (operation.anchor) {
        const auto anchor = std::ranges::find(document.sections, *operation.anchor,
            &spice::sct::SctDocumentSection::id);
        if (anchor == document.sections.end())
            return {.issue = issue("SectionAnchorNotFound", "The section insertion anchor does not exist.")};
        destination = std::next(anchor);
    }
    while (document.nextSectionIdValue() <= operation.section.id.value())
        (void)document.allocateSectionId();
    if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&operation.section.content)) {
        for (const auto& instruction : script->instructions)
            while (document.nextInstructionIdValue() <= instruction.id.value())
                (void)document.allocateInstructionId();
    } else if (const auto* string = std::get_if<spice::sct::SctStringSectionContent>(&operation.section.content)) {
        while (document.nextStringIdValue() <= string->string.id.value())
            (void)document.allocateStringId();
    }
    document.sections.insert(destination, operation.section);
    SctSectionStructuralChange change;
    change.section = operation.section.id;
    change.after = SctSectionPlacement{operation.anchor};
    change.afterName = operation.section.nameBytes;
    change.afterValue = operation.section;
    auto reverse = change;
    reverse.before = reverse.after;
    reverse.after.reset();
    reverse.beforeName = reverse.afterName;
    reverse.afterName.reset();
    reverse.beforeValue = reverse.afterValue;
    reverse.afterValue.reset();
    SctEditChangeSet forward, backward;
    forward.sections.push_back(std::move(change));
    backward.sections.push_back(std::move(reverse));
    forward.invalidations = backward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
    forward.documentChanged = backward.documentChanged = true;
    return {SctDeleteSectionOperation{operation.section.id}, std::move(forward), std::move(backward)};
}

[[nodiscard]] PrimitiveApplication applyDeleteSection(
    spice::sct::SctDocument& document, const SctDeleteSectionOperation& operation) {
    const auto found = std::ranges::find(document.sections, operation.section,
        &spice::sct::SctDocumentSection::id);
    if (found == document.sections.end())
        return {.issue = issue("SectionNotFound", "The deleted section does not exist.",
            SctNavigationTarget{SctNavigationKind::Section, operation.section.value()})};
    const auto ordinal = static_cast<std::size_t>(std::distance(document.sections.begin(), found));
    std::optional<spice::sct::SctSectionId> anchor;
    if (ordinal != 0u) anchor = document.sections[ordinal - 1u].id;
    auto removed = *found;
    document.sections.erase(found);
    SctSectionStructuralChange change;
    change.section = operation.section;
    change.before = SctSectionPlacement{anchor};
    change.beforeName = removed.nameBytes;
    change.beforeValue = removed;
    auto reverse = change;
    reverse.after = reverse.before;
    reverse.before.reset();
    reverse.afterName = reverse.beforeName;
    reverse.beforeName.reset();
    reverse.afterValue = reverse.beforeValue;
    reverse.beforeValue.reset();
    SctEditChangeSet forward, backward;
    forward.sections.push_back(std::move(change));
    backward.sections.push_back(std::move(reverse));
    forward.invalidations = backward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
    forward.documentChanged = backward.documentChanged = true;
    return {SctInsertSectionAfterOperation{anchor, std::move(removed)},
        std::move(forward), std::move(backward)};
}

[[nodiscard]] PrimitiveApplication applyRelocateSection(
    spice::sct::SctDocument& document, const SctRelocateSectionAfterOperation& operation) {
    if (operation.anchor && *operation.anchor == operation.section)
        return {.issue = issue("SectionMoveSelfAnchor", "A section cannot be positioned relative to itself.")};
    const auto found = std::ranges::find(document.sections, operation.section,
        &spice::sct::SctDocumentSection::id);
    if (found == document.sections.end()) return {.issue = issue("SectionNotFound", "The moved section does not exist.")};
    const auto ordinal = static_cast<std::size_t>(std::distance(document.sections.begin(), found));
    std::optional<spice::sct::SctSectionId> oldAnchor;
    if (ordinal != 0u) oldAnchor = document.sections[ordinal - 1u].id;
    if (oldAnchor == operation.anchor) return {.issue = issue("SectionMoveNoChange", "The section is already at that position.")};
    if (operation.anchor && std::ranges::find(document.sections, *operation.anchor,
            &spice::sct::SctDocumentSection::id) == document.sections.end())
        return {.issue = issue("SectionAnchorNotFound", "The section destination anchor does not exist.")};
    auto moved = *found;
    document.sections.erase(found);
    auto destination = document.sections.begin();
    if (operation.anchor) {
        const auto anchor = std::ranges::find(document.sections, *operation.anchor,
            &spice::sct::SctDocumentSection::id);
        if (anchor == document.sections.end()) return {.issue = issue("SectionAnchorNotFound", "The section destination anchor does not exist.")};
        destination = std::next(anchor);
    }
    document.sections.insert(destination, std::move(moved));
    SctSectionStructuralChange change;
    change.section = operation.section;
    change.before = SctSectionPlacement{oldAnchor};
    change.after = SctSectionPlacement{operation.anchor};
    auto reverse = change;
    std::swap(reverse.before, reverse.after);
    SctEditChangeSet forward, backward;
    forward.sections.push_back(std::move(change));
    backward.sections.push_back(std::move(reverse));
    forward.invalidations = backward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
    forward.documentChanged = backward.documentChanged = true;
    return {SctRelocateSectionAfterOperation{operation.section, oldAnchor}, std::move(forward), std::move(backward)};
}

[[nodiscard]] PrimitiveApplication applyRenameSection(
    spice::sct::SctDocument& document, const SctRenameSectionOperation& operation) {
    auto* section = findSection(document, operation.section);
    if (section == nullptr) return {.issue = issue("SectionNotFound", "The renamed section does not exist.")};
    auto previous = section->nameBytes;
    section->nameBytes = operation.nameBytes;
    SctSectionStructuralChange change;
    change.section = operation.section;
    change.before = change.after = SctSectionPlacement{};
    change.beforeName = previous;
    change.afterName = operation.nameBytes;
    auto reverse = change;
    std::swap(reverse.beforeName, reverse.afterName);
    SctEditChangeSet forward, backward;
    forward.sections.push_back(std::move(change));
    backward.sections.push_back(std::move(reverse));
    forward.modified.push_back({SctNavigationKind::Section, operation.section.value()});
    backward.modified = forward.modified;
    forward.documentChanged = backward.documentChanged = true;
    return {SctRenameSectionOperation{operation.section, std::move(previous)}, std::move(forward), std::move(backward)};
}

[[nodiscard]] PrimitiveApplication applyInsert(
    spice::sct::SctDocument& document,
    const SctInsertInstructionAfterOperation& operation) {
    const auto target = instructionTarget(operation.instruction.id);
    const auto index = spice::sct::SctDocumentIndex::build(document);
    if (index.find(document, operation.instruction.id) != nullptr) {
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
    const auto semantics = spice::sct::SctInstructionSemanticAnalyzer::build(operation.instruction);
    SctInstructionStructuralChange change;
    change.instruction = operation.instruction.id;
    change.after = SctInstructionPlacement{location->sectionId, operation.anchor};
    change.afterValue = operation.instruction;
    change.afterSemantics = semantics;
    auto reverse = change;
    reverse.before = change.after;
    reverse.after.reset();
    reverse.beforeValue = change.afterValue;
    reverse.afterValue.reset();
    reverse.beforeSemantics = change.afterSemantics;
    reverse.afterSemantics = {};
    return {
        SctDeleteInstructionOperation{operation.instruction.id},
        instructionChangeSet(std::move(change)),
        instructionChangeSet(std::move(reverse)),
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyDelete(
    spice::sct::SctDocument& document,
    const SctDeleteInstructionOperation& operation) {
    const auto target = instructionTarget(operation.instruction);
    const auto index = spice::sct::SctDocumentIndex::build(document);
    const auto location = index.instructionLocation(operation.instruction);
    const auto* existing = index.find(document, operation.instruction);
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
    const auto semantics = spice::sct::SctInstructionSemanticAnalyzer::build(removed);
    script->instructions.erase(script->instructions.begin()
        + static_cast<std::ptrdiff_t>(location->instructionOrdinal));
    SctInstructionStructuralChange change;
    change.instruction = operation.instruction;
    change.before = SctInstructionPlacement{location->sectionId, anchor};
    change.beforeValue = removed;
    change.beforeSemantics = semantics;
    auto reverse = change;
    reverse.after = change.before;
    reverse.before.reset();
    reverse.afterValue = change.beforeValue;
    reverse.beforeValue.reset();
    reverse.afterSemantics = change.beforeSemantics;
    reverse.beforeSemantics = {};
    return {
        SctInsertInstructionAfterOperation{anchor, removed},
        instructionChangeSet(std::move(change)),
        instructionChangeSet(std::move(reverse)),
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
    const auto movedPosition = std::ranges::find(
        script->instructions, operation.instruction,
        &spice::sct::SctDocumentInstruction::id);
    const auto* movedValue = std::addressof(*movedPosition);
    const auto semantics = spice::sct::SctInstructionSemanticAnalyzer::build(*movedValue);
    SctInstructionStructuralChange change;
    change.instruction = operation.instruction;
    change.before = SctInstructionPlacement{sourceLocation->sectionId, oldAnchor};
    change.after = SctInstructionPlacement{sourceLocation->sectionId, operation.anchor};
    change.beforeValue = *movedValue;
    change.afterValue = *movedValue;
    change.beforeSemantics = semantics;
    change.afterSemantics = semantics;
    auto reverse = change;
    std::swap(reverse.before, reverse.after);
    return {
        SctRelocateInstructionAfterOperation{operation.instruction, oldAnchor},
        instructionChangeSet(std::move(change)),
        instructionChangeSet(std::move(reverse)),
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyReplaceInstruction(
    spice::sct::SctDocument& document,
    const SctReplaceInstructionOperation& operation) {
    const auto target = instructionTarget(operation.instruction);
    const auto index = spice::sct::SctDocumentIndex::build(document);
    const auto location = index.instructionLocation(operation.instruction);
    const auto* existing = index.find(document, operation.instruction);
    if (!location || existing == nullptr) {
        return {.issue = issue("InstructionNotFound",
            "The replaced instruction does not exist.", target)};
    }
    if (operation.replacement.id != operation.instruction) {
        return {.issue = issue("InstructionReplacementIdMismatch",
            "An instruction replacement must retain its stable ID.", target)};
    }
    if (operation.replacement.opcode != existing->opcode) {
        return {.issue = issue("InstructionReplacementOpcodeMismatch",
            "An instruction replacement cannot change the opcode.", target)};
    }
    auto* section = findSection(document, location->sectionId);
    auto* script = section == nullptr ? nullptr
        : std::get_if<spice::sct::SctScriptSectionContent>(&section->content);
    if (script == nullptr || location->instructionOrdinal >= script->instructions.size()) {
        return {.issue = issue("InstructionReplacementNotScript",
            "The instruction is not in a script section.", target)};
    }
    auto previous = script->instructions[location->instructionOrdinal];
    script->instructions[location->instructionOrdinal] = operation.replacement;
    SctInstructionStructuralChange change;
    change.instruction = operation.instruction;
    const auto after = location->instructionOrdinal == 0u
        ? std::optional<spice::sct::SctInstructionId>{}
        : std::optional{script->instructions[location->instructionOrdinal - 1u].id};
    change.before = SctInstructionPlacement{location->sectionId, after};
    change.after = change.before;
    change.beforeValue = previous;
    change.afterValue = operation.replacement;
    change.beforeSemantics = spice::sct::SctInstructionSemanticAnalyzer::build(previous);
    change.afterSemantics = spice::sct::SctInstructionSemanticAnalyzer::build(
        operation.replacement);
    auto reverse = change;
    std::swap(reverse.beforeValue, reverse.afterValue);
    std::swap(reverse.beforeSemantics, reverse.afterSemantics);
    return {
        SctReplaceInstructionOperation{operation.instruction, std::move(previous)},
        instructionChangeSet(std::move(change), {target}),
        instructionChangeSet(std::move(reverse), {target}),
        std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyReplaceText(
    spice::sct::SctDocument& document,
    const SctReplaceTextValueOperation& operation) {
    const auto target = textTarget(operation.target);
    spice::sct::SctTextValue* value = std::visit([&](const auto id)
        -> spice::sct::SctTextValue* {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            auto* string = findString(document, id);
            return string != nullptr ? &string->value : nullptr;
        } else {
            auto* entry = findFooterEntry(document, id);
            return entry != nullptr ? &entry->value : nullptr;
        }
    }, operation.target);
    if (value == nullptr) return {.issue = issue("TextTargetNotFound",
        "The replaced text entity does not exist.", target)};
    auto previous = *value;
    *value = operation.value;
    SctTextValueChange change{operation.target, previous, operation.value};
    SctTextValueChange reverse{operation.target, operation.value, previous};
    SctEditChangeSet forward, backward;
    forward.textValues.push_back(std::move(change));
    backward.textValues.push_back(std::move(reverse));
    forward.modified.push_back(target);
    backward.modified.push_back(target);
    forward.documentChanged = backward.documentChanged = true;
    return {
        SctReplaceTextValueOperation{operation.target, std::move(previous)},
        std::move(forward), std::move(backward), std::nullopt,
    };
}

[[nodiscard]] PrimitiveApplication applyInsertFooter(
    spice::sct::SctDocument& document,
    const SctInsertFooterEntryAfterOperation& operation) {
    const auto target = SctNavigationTarget{SctNavigationKind::FooterEntry, operation.entry.id.value()};
    if (findFooterEntry(document, operation.entry.id) != nullptr)
        return {.issue = issue("FooterEntryAlreadyExists", "The footer entry ID already exists.", target)};
    auto destination = document.footerEntries.begin();
    if (operation.anchor) {
        const auto anchor = std::ranges::find(document.footerEntries, *operation.anchor,
            &spice::sct::SctDocumentFooterEntry::id);
        if (anchor == document.footerEntries.end()) return {.issue = issue("FooterEntryAnchorNotFound", "The footer insertion anchor does not exist.")};
        destination = std::next(anchor);
    }
    while (document.nextFooterEntryIdValue() <= operation.entry.id.value())
        (void)document.allocateFooterEntryId();
    document.footerEntries.insert(destination, operation.entry);
    SctFooterEntryStructuralChange change;
    change.entry = operation.entry.id;
    change.after = SctFooterEntryPlacement{operation.anchor};
    change.afterValue = operation.entry;
    auto reverse = change;
    reverse.before = reverse.after; reverse.after.reset();
    reverse.beforeValue = reverse.afterValue; reverse.afterValue.reset();
    SctEditChangeSet forward, backward;
    forward.footerEntries.push_back(std::move(change));
    backward.footerEntries.push_back(std::move(reverse));
    forward.documentChanged = backward.documentChanged = true;
    return {SctDeleteFooterEntryOperation{operation.entry.id}, std::move(forward), std::move(backward)};
}

[[nodiscard]] PrimitiveApplication applyDeleteFooter(
    spice::sct::SctDocument& document, const SctDeleteFooterEntryOperation& operation) {
    const auto found = std::ranges::find(document.footerEntries, operation.entry,
        &spice::sct::SctDocumentFooterEntry::id);
    if (found == document.footerEntries.end()) return {.issue = issue("FooterEntryNotFound", "The deleted footer entry does not exist.")};
    const auto ordinal = static_cast<std::size_t>(std::distance(document.footerEntries.begin(), found));
    std::optional<spice::sct::SctFooterEntryId> anchor;
    if (ordinal != 0u) anchor = document.footerEntries[ordinal - 1u].id;
    auto removed = *found;
    document.footerEntries.erase(found);
    SctFooterEntryStructuralChange change;
    change.entry = operation.entry;
    change.before = SctFooterEntryPlacement{anchor};
    change.beforeValue = removed;
    auto reverse = change;
    reverse.after = reverse.before; reverse.before.reset();
    reverse.afterValue = reverse.beforeValue; reverse.beforeValue.reset();
    SctEditChangeSet forward, backward;
    forward.footerEntries.push_back(std::move(change));
    backward.footerEntries.push_back(std::move(reverse));
    forward.documentChanged = backward.documentChanged = true;
    return {SctInsertFooterEntryAfterOperation{anchor, std::move(removed)}, std::move(forward), std::move(backward)};
}

[[nodiscard]] PrimitiveApplication applyPrimitive(
    spice::sct::SctDocument& document,
    const SctPrimitiveOperation& operation) {
    return std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SctInsertSectionAfterOperation>)
            return applyInsertSection(document, typed);
        else if constexpr (std::is_same_v<T, SctDeleteSectionOperation>)
            return applyDeleteSection(document, typed);
        else if constexpr (std::is_same_v<T, SctRelocateSectionAfterOperation>)
            return applyRelocateSection(document, typed);
        else if constexpr (std::is_same_v<T, SctRenameSectionOperation>)
            return applyRenameSection(document, typed);
        else if constexpr (std::is_same_v<T, SctInsertInstructionAfterOperation>)
            return applyInsert(document, typed);
        else if constexpr (std::is_same_v<T, SctDeleteInstructionOperation>)
            return applyDelete(document, typed);
        else if constexpr (std::is_same_v<T, SctRelocateInstructionAfterOperation>)
            return applyRelocate(document, typed);
        else if constexpr (std::is_same_v<T, SctReplaceInstructionOperation>)
            return applyReplaceInstruction(document, typed);
        else if constexpr (std::is_same_v<T, SctReplaceTextValueOperation>)
            return applyReplaceText(document, typed);
        else if constexpr (std::is_same_v<T, SctInsertFooterEntryAfterOperation>)
            return applyInsertFooter(document, typed);
        else
            return applyDeleteFooter(document, typed);
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

SctOperationReplay SctSemanticOperationService::applyInPlace(
    spice::sct::SctDocument& document,
    const SctSemanticOperationBatch& batch) {
    SctOperationReplay result;
    if (batch.operations.empty()) {
        result.issues.push_back(issue(
            "EmptyOperationBatch", "A semantic operation batch cannot be empty."));
        return result;
    }
    std::vector<SctPrimitiveOperation> appliedInverses;
    std::vector<SctEditChangeSet> reverseChanges;
    for (const auto& operation : batch.operations) {
        auto applied = applyPrimitive(document, operation);
        if (applied.issue.has_value()) {
            for (auto inverse = appliedInverses.rbegin();
                inverse != appliedInverses.rend(); ++inverse) {
                (void)applyPrimitive(document, *inverse);
            }
            result.issues.push_back(std::move(*applied.issue));
            return result;
        }
        appliedInverses.push_back(*applied.inverse);
        result.inverse.operations.insert(result.inverse.operations.begin(),
            std::move(*applied.inverse));
        appendChanges(result.forwardChanges, std::move(applied.forwardChanges));
        reverseChanges.push_back(std::move(applied.reverseChanges));
    }
    for (auto changes = reverseChanges.rbegin(); changes != reverseChanges.rend(); ++changes)
        appendChanges(result.reverseChanges, std::move(*changes));
    return result;
}

}  // namespace salsa::core
