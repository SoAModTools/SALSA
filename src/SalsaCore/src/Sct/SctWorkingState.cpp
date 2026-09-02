#include "SalsaCore/Sct/SctWorkingState.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

[[nodiscard]] SctNavigationTarget instructionTarget(
    const spice::sct::SctInstructionId id) {
    return {SctNavigationKind::Instruction, id.value()};
}

[[nodiscard]] SctNavigationTarget messageNavigation(const SctMessageTarget& target) {
    return std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return SctNavigationTarget{SctNavigationKind::String, id.value()};
        else
            return SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
    }, target);
}

[[nodiscard]] std::string textIdentity(const SctTextTarget& target) {
    return std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        return std::string(std::is_same_v<T, spice::sct::SctStringId> ? "S:" : "F:")
            + std::to_string(id.value());
    }, target);
}

[[nodiscard]] SctOperationIssue issue(
    std::string code, std::string message,
    std::optional<SctNavigationTarget> target = std::nullopt) {
    return {std::move(code), std::move(message), target};
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

[[nodiscard]] SctInstructionStructuralChange reversed(
    const SctInstructionStructuralChange& source) {
    SctInstructionStructuralChange result;
    result.instruction = source.instruction;
    result.before = source.after;
    result.after = source.before;
    result.beforeValue = source.afterValue;
    result.afterValue = source.beforeValue;
    result.beforeSemantics = source.afterSemantics;
    result.afterSemantics = source.beforeSemantics;
    return result;
}

[[nodiscard]] SctSectionStructuralChange reversed(
    const SctSectionStructuralChange& source) {
    SctSectionStructuralChange result;
    result.section = source.section;
    result.before = source.after;
    result.after = source.before;
    result.beforeName = source.afterName;
    result.afterName = source.beforeName;
    result.beforeValue = source.afterValue;
    result.afterValue = source.beforeValue;
    return result;
}

[[nodiscard]] SctFooterEntryStructuralChange reversed(
    const SctFooterEntryStructuralChange& source) {
    SctFooterEntryStructuralChange result;
    result.entry = source.entry;
    result.before = source.after;
    result.after = source.before;
    result.beforeValue = source.afterValue;
    result.afterValue = source.beforeValue;
    return result;
}

}  // namespace

SctWorkingState::SctWorkingState(
    std::shared_ptr<const spice::sct::SctDocument> checkpoint,
    const std::span<const SctTextRepairRecord> repairs)
    : checkpoint_(std::move(checkpoint)) {
    if (!checkpoint_) return;
    nextSectionId_ = checkpoint_->nextSectionIdValue();
    nextInstructionId_ = checkpoint_->nextInstructionIdValue();
    nextStringId_ = checkpoint_->nextStringIdValue();
    nextFooterEntryId_ = checkpoint_->nextFooterEntryIdValue();
    for (const auto& section : checkpoint_->sections) {
        sections_.emplace(section.id, section);
        physicalSectionOrder_.push_back(section.id);
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content)) {
            auto& order = sectionOrder_[section.id];
            order.reserve(script->instructions.size());
            for (const auto& instruction : script->instructions) {
                auto semantics = spice::sct::SctInstructionSemanticAnalyzer::build(instruction);
                order.push_back(instruction.id);
                instructions_.emplace(instruction.id,
                    InstructionEntry{instruction, section.id, semantics});
                addContribution(semantics);
            }
        } else if (const auto* strings = std::get_if<spice::sct::SctStringSectionContent>(
                &section.content)) {
            stringValues_.emplace(strings->string.id, strings->string.value);
        }
    }
    for (const auto& entry : checkpoint_->footerEntries) {
        footerEntries_.emplace(entry.id, entry);
        footerOrder_.push_back(entry.id);
    }
    for (const auto& attachment : checkpoint_->opaqueAttachments) {
        if (const auto* instruction = std::get_if<spice::sct::SctInstructionId>(
                &attachment.anchor)) {
            opaqueAttachments_[*instruction].push_back(attachment.id);
        }
    }
    for (const auto& repair : repairs)
        textRepairProvenance_[textIdentity(repair.target)] = repair.provenance;
}

const spice::sct::SctDocumentSection* SctWorkingState::section(
    const spice::sct::SctSectionId id) const noexcept {
    const auto found = sections_.find(id);
    return found == sections_.end() ? nullptr : &found->second;
}

std::span<const spice::sct::SctSectionId> SctWorkingState::sectionOrder() const noexcept {
    return physicalSectionOrder_;
}

std::optional<SctSectionPlacement> SctWorkingState::sectionPlacement(
    const spice::sct::SctSectionId id) const noexcept {
    const auto found = std::ranges::find(physicalSectionOrder_, id);
    if (found == physicalSectionOrder_.end()) return std::nullopt;
    std::optional<spice::sct::SctSectionId> after;
    if (found != physicalSectionOrder_.begin()) after = *std::prev(found);
    return SctSectionPlacement{after};
}

const spice::sct::SctDocumentInstruction* SctWorkingState::instruction(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = instructions_.find(id);
    return found == instructions_.end() ? nullptr : &found->second.value;
}

std::optional<SctInstructionPlacement> SctWorkingState::placement(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = instructions_.find(id);
    if (found == instructions_.end()) return std::nullopt;
    const auto order = sectionOrder_.find(found->second.section);
    if (order == sectionOrder_.end()) return std::nullopt;
    const auto position = std::ranges::find(order->second, id);
    if (position == order->second.end()) return std::nullopt;
    std::optional<spice::sct::SctInstructionId> after;
    if (position != order->second.begin()) after = *std::prev(position);
    return SctInstructionPlacement{found->second.section, after};
}

std::span<const spice::sct::SctInstructionId> SctWorkingState::instructionOrder(
    const spice::sct::SctSectionId section) const noexcept {
    const auto found = sectionOrder_.find(section);
    return found == sectionOrder_.end()
        ? std::span<const spice::sct::SctInstructionId>{}
        : std::span<const spice::sct::SctInstructionId>{found->second};
}

std::optional<spice::sct::SctInstructionId> SctWorkingState::instructionBefore(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto place = placement(id);
    return place.has_value() ? place->after : std::nullopt;
}

std::optional<spice::sct::SctInstructionId> SctWorkingState::instructionAfter(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = instructions_.find(id);
    if (found == instructions_.end()) return std::nullopt;
    const auto& order = sectionOrder_.at(found->second.section);
    const auto position = std::ranges::find(order, id);
    if (position == order.end() || std::next(position) == order.end()) return std::nullopt;
    return *std::next(position);
}

const spice::sct::SctInstructionSemanticContribution* SctWorkingState::contribution(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = instructions_.find(id);
    return found == instructions_.end() ? nullptr : &found->second.semantics;
}

std::size_t SctWorkingState::incomingReferenceCount(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = incomingReferences_.find(id);
    return found == incomingReferences_.end() ? 0u : found->second;
}

std::span<const spice::sct::SctOpaqueAttachmentId> SctWorkingState::opaqueAttachments(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = opaqueAttachments_.find(id);
    return found == opaqueAttachments_.end()
        ? std::span<const spice::sct::SctOpaqueAttachmentId>{}
        : std::span<const spice::sct::SctOpaqueAttachmentId>{found->second};
}

const spice::sct::SctTextValue* SctWorkingState::textValue(
    const SctTextTarget& target) const noexcept {
    return std::visit([this](const auto id) -> const spice::sct::SctTextValue* {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            const auto found = stringValues_.find(id);
            return found == stringValues_.end() ? nullptr : &found->second;
        } else {
            const auto found = footerEntries_.find(id);
            return found == footerEntries_.end() ? nullptr : &found->second.value;
        }
    }, target);
}

const spice::sct::SctMessage* SctWorkingState::message(
    const SctMessageTarget& target) const noexcept {
    const auto* value = textValue(target);
    return value == nullptr ? nullptr : std::get_if<spice::sct::SctMessage>(value);
}

std::optional<SctTextRepairProvenance> SctWorkingState::textRepairProvenance(
    const SctTextTarget& target) const {
    const auto found = textRepairProvenance_.find(textIdentity(target));
    return found == textRepairProvenance_.end()
        ? std::nullopt : std::optional{found->second};
}

std::vector<SctTextRepairRecord> SctWorkingState::textRepairProvenances() const {
    std::vector<SctTextRepairRecord> result;
    result.reserve(textRepairProvenance_.size());
    for (const auto& [key, provenance] : textRepairProvenance_) {
        if (key.size() < 3 || key[1] != ':') continue;
        const auto raw = std::stoull(key.substr(2));
        SctTextTarget target = key[0] == 'S'
            ? SctTextTarget{spice::sct::SctStringId(raw)}
            : SctTextTarget{spice::sct::SctFooterEntryId(raw)};
        result.push_back({std::move(target), provenance});
    }
    std::ranges::sort(result, {}, [](const auto& value) {
        return textIdentity(value.target);
    });
    return result;
}

const spice::sct::SctDocumentFooterEntry* SctWorkingState::footerEntry(
    const spice::sct::SctFooterEntryId id) const noexcept {
    const auto found = footerEntries_.find(id);
    return found == footerEntries_.end() ? nullptr : &found->second;
}

std::span<const spice::sct::SctFooterEntryId>
SctWorkingState::footerEntryOrder() const noexcept {
    return footerOrder_;
}

std::vector<spice::sct::SctInstructionId> SctWorkingState::inboundReferenceSources(
    const spice::sct::SctDocumentReferenceTarget& target) const {
    std::vector<spice::sct::SctInstructionId> result;
    for (const auto sectionId : physicalSectionOrder_) {
        const auto foundOrder = sectionOrder_.find(sectionId);
        if (foundOrder == sectionOrder_.end()) continue;
        for (const auto instructionId : foundOrder->second) {
            const auto found = instructions_.find(instructionId);
            if (found == instructions_.end()) continue;
            for (const auto& reference : found->second.semantics.references) {
                if (reference.target == target) result.push_back(instructionId);
            }
        }
    }
    return result;
}

std::vector<spice::sct::SctOpaqueAttachmentId> SctWorkingState::opaqueAttachments(
    const spice::sct::SctOpaqueAnchor& anchor) const {
    std::vector<spice::sct::SctOpaqueAttachmentId> result;
    if (!checkpoint_) return result;
    for (const auto& attachment : checkpoint_->opaqueAttachments) {
        if (attachment.anchor == anchor) result.push_back(attachment.id);
    }
    return result;
}

std::uint64_t SctWorkingState::nextSectionIdValue() const noexcept {
    return nextSectionId_;
}

std::uint64_t SctWorkingState::nextInstructionIdValue() const noexcept {
    return nextInstructionId_;
}

std::uint64_t SctWorkingState::nextStringIdValue() const noexcept {
    return nextStringId_;
}

std::uint64_t SctWorkingState::nextFooterEntryIdValue() const noexcept {
    return nextFooterEntryId_;
}

SctWorkingApplication SctWorkingState::apply(const SctSemanticOperationBatch& batch) {
    SctWorkingApplication result;
    if (batch.operations.empty()) {
        result.issues.push_back(issue(
            "EmptyOperationBatch", "A semantic operation batch cannot be empty."));
        return result;
    }

    std::vector<SctPrimitiveOperation> appliedInverses;
    std::vector<SctEditChangeSet> reverseParts;
    appliedInverses.reserve(batch.operations.size());
    reverseParts.reserve(batch.operations.size());
    for (const auto& operation : batch.operations) {
        SctPrimitiveOperation inverse;
        SctEditChangeSet forward;
        SctEditChangeSet reverse;
        if (auto problem = applyPrimitive(operation, inverse, forward, reverse);
            problem.has_value()) {
            for (auto undo = appliedInverses.rbegin(); undo != appliedInverses.rend(); ++undo) {
                SctPrimitiveOperation ignoredInverse;
                SctEditChangeSet ignoredForward;
                SctEditChangeSet ignoredReverse;
                (void)applyPrimitive(*undo, ignoredInverse, ignoredForward, ignoredReverse);
            }
            result.issues.push_back(std::move(*problem));
            return result;
        }
        appliedInverses.push_back(inverse);
        result.inverse.operations.insert(result.inverse.operations.begin(), std::move(inverse));
        appendChanges(result.forwardChanges, std::move(forward));
        reverseParts.push_back(std::move(reverse));
    }
    for (auto part = reverseParts.rbegin(); part != reverseParts.rend(); ++part)
        appendChanges(result.reverseChanges, std::move(*part));
    return result;
}

std::optional<SctOperationIssue> SctWorkingState::applyPrimitive(
    const SctPrimitiveOperation& operation,
    SctPrimitiveOperation& inverse,
    SctEditChangeSet& forward,
    SctEditChangeSet& reverseChanges) {
    return std::visit([&](const auto& typed) -> std::optional<SctOperationIssue> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SctInsertSectionAfterOperation>) {
            if (sections_.contains(typed.section.id))
                return issue("SectionAlreadyExists", "The inserted section ID already exists.",
                    SctNavigationTarget{SctNavigationKind::Section, typed.section.id.value()});
            if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                    &typed.section.content)) {
                if (std::ranges::any_of(script->instructions, [this](const auto& instruction) {
                        return instructions_.contains(instruction.id);
                    })) return issue("InstructionAlreadyExists",
                        "An instruction ID in the inserted section already exists.");
            } else if (const auto* strings = std::get_if<spice::sct::SctStringSectionContent>(
                    &typed.section.content); strings != nullptr
                    && stringValues_.contains(strings->string.id)) {
                return issue("StringAlreadyExists", "The inserted string ID already exists.");
            }
            auto destination = physicalSectionOrder_.begin();
            if (typed.anchor) {
                const auto anchor = std::ranges::find(physicalSectionOrder_, *typed.anchor);
                if (anchor == physicalSectionOrder_.end())
                    return issue("SectionAnchorNotFound", "The section insertion anchor does not exist.");
                destination = std::next(anchor);
            }
            physicalSectionOrder_.insert(destination, typed.section.id);
            sections_.emplace(typed.section.id, typed.section);
            nextSectionId_ = std::max(nextSectionId_, typed.section.id.value() + 1u);
            if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                    &typed.section.content)) {
                auto& order = sectionOrder_[typed.section.id];
                for (const auto& instruction : script->instructions) {
                    auto semantics = spice::sct::SctInstructionSemanticAnalyzer::build(instruction);
                    order.push_back(instruction.id);
                    instructions_.emplace(instruction.id,
                        InstructionEntry{instruction, typed.section.id, semantics});
                    nextInstructionId_ = std::max(nextInstructionId_, instruction.id.value() + 1u);
                    addContribution(semantics);
                }
            } else if (const auto* strings = std::get_if<spice::sct::SctStringSectionContent>(
                    &typed.section.content)) {
                stringValues_.emplace(strings->string.id, strings->string.value);
                nextStringId_ = std::max(nextStringId_, strings->string.id.value() + 1u);
            }
            SctSectionStructuralChange change;
            change.section = typed.section.id;
            change.after = SctSectionPlacement{typed.anchor};
            change.afterName = typed.section.nameBytes;
            change.afterValue = typed.section;
            forward.sections.push_back(change);
            reverseChanges.sections.push_back(reversed(change));
            forward.invalidations = reverseChanges.invalidations =
                SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = reverseChanges.documentChanged = true;
            inverse = SctDeleteSectionOperation{typed.section.id};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctDeleteSectionOperation>) {
            const auto found = sections_.find(typed.section);
            const auto before = sectionPlacement(typed.section);
            if (found == sections_.end() || !before)
                return issue("SectionNotFound", "The deleted section does not exist.",
                    SctNavigationTarget{SctNavigationKind::Section, typed.section.value()});
            auto removed = found->second;
            if (auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                    &removed.content)) {
                script->instructions.clear();
                const auto order = sectionOrder_.find(typed.section);
                if (order != sectionOrder_.end()) {
                    for (const auto id : order->second) {
                        const auto instruction = instructions_.find(id);
                        if (instruction == instructions_.end()) continue;
                        script->instructions.push_back(instruction->second.value);
                        removeContribution(instruction->second.semantics);
                        instructions_.erase(instruction);
                    }
                    sectionOrder_.erase(order);
                }
            } else if (auto* strings = std::get_if<spice::sct::SctStringSectionContent>(
                    &removed.content)) {
                const auto value = stringValues_.find(strings->string.id);
                if (value != stringValues_.end()) strings->string.value = value->second;
                stringValues_.erase(strings->string.id);
            }
            std::erase(physicalSectionOrder_, typed.section);
            sections_.erase(found);
            SctSectionStructuralChange change;
            change.section = typed.section;
            change.before = before;
            change.beforeName = removed.nameBytes;
            change.beforeValue = removed;
            forward.sections.push_back(change);
            reverseChanges.sections.push_back(reversed(change));
            forward.invalidations = reverseChanges.invalidations =
                SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = reverseChanges.documentChanged = true;
            inverse = SctInsertSectionAfterOperation{before->after, std::move(removed)};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctRelocateSectionAfterOperation>) {
            if (typed.anchor && *typed.anchor == typed.section)
                return issue("SectionMoveSelfAnchor", "A section cannot be positioned relative to itself.");
            const auto before = sectionPlacement(typed.section);
            if (!before) return issue("SectionNotFound", "The moved section does not exist.");
            if (before->after == typed.anchor)
                return issue("SectionMoveNoChange", "The section is already at that position.");
            if (typed.anchor && std::ranges::find(physicalSectionOrder_, *typed.anchor)
                    == physicalSectionOrder_.end())
                return issue("SectionAnchorNotFound", "The section destination anchor does not exist.");
            std::erase(physicalSectionOrder_, typed.section);
            auto destination = physicalSectionOrder_.begin();
            if (typed.anchor) {
                const auto anchor = std::ranges::find(physicalSectionOrder_, *typed.anchor);
                if (anchor == physicalSectionOrder_.end())
                    return issue("SectionAnchorNotFound", "The section destination anchor does not exist.");
                destination = std::next(anchor);
            }
            physicalSectionOrder_.insert(destination, typed.section);
            SctSectionStructuralChange change;
            change.section = typed.section;
            change.before = before;
            change.after = SctSectionPlacement{typed.anchor};
            forward.sections.push_back(change);
            reverseChanges.sections.push_back(reversed(change));
            forward.invalidations = reverseChanges.invalidations =
                SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = reverseChanges.documentChanged = true;
            inverse = SctRelocateSectionAfterOperation{typed.section, before->after};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctRenameSectionOperation>) {
            const auto found = sections_.find(typed.section);
            if (found == sections_.end()) return issue("SectionNotFound", "The renamed section does not exist.");
            auto previous = found->second.nameBytes;
            found->second.nameBytes = typed.nameBytes;
            SctSectionStructuralChange change;
            change.section = typed.section;
            change.beforeName = previous;
            change.afterName = typed.nameBytes;
            forward.sections.push_back(change);
            reverseChanges.sections.push_back(reversed(change));
            const SctNavigationTarget target{SctNavigationKind::Section, typed.section.value()};
            forward.modified.push_back(target);
            reverseChanges.modified.push_back(target);
            forward.documentChanged = reverseChanges.documentChanged = true;
            inverse = SctRenameSectionOperation{typed.section, std::move(previous)};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctInsertInstructionAfterOperation>) {
            if (instructions_.contains(typed.instruction.id))
                return issue("InstructionAlreadyExists", "The inserted instruction ID already exists.",
                    instructionTarget(typed.instruction.id));
            const auto anchor = instructions_.find(typed.anchor);
            if (anchor == instructions_.end())
                return issue("InstructionAnchorNotFound", "The instruction insertion anchor does not exist.",
                    instructionTarget(typed.anchor));
            auto& order = sectionOrder_[anchor->second.section];
            const auto position = std::ranges::find(order, typed.anchor);
            if (position == order.end())
                return issue("InstructionAnchorNotFound", "The instruction insertion anchor does not exist.",
                    instructionTarget(typed.anchor));
            const auto semantics = spice::sct::SctInstructionSemanticAnalyzer::build(typed.instruction);
            const SctInstructionPlacement after{anchor->second.section, typed.anchor};
            order.insert(std::next(position), typed.instruction.id);
            instructions_.emplace(typed.instruction.id,
                InstructionEntry{typed.instruction, anchor->second.section, semantics});
            if (auto section = sections_.find(anchor->second.section); section != sections_.end()) {
                if (auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section->second.content)) {
                    const auto valueAnchor = std::ranges::find(script->instructions, typed.anchor,
                        &spice::sct::SctDocumentInstruction::id);
                    if (valueAnchor != script->instructions.end()) script->instructions.insert(
                        std::next(valueAnchor), typed.instruction);
                }
            }
            nextInstructionId_ = std::max(nextInstructionId_, typed.instruction.id.value() + 1u);
            addContribution(semantics);
            SctInstructionStructuralChange change;
            change.instruction = typed.instruction.id;
            change.after = after;
            change.afterValue = typed.instruction;
            change.afterSemantics = semantics;
            forward.instructions.push_back(change);
            reverseChanges.instructions.push_back(reversed(change));
            forward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            reverseChanges.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = true;
            reverseChanges.documentChanged = true;
            inverse = SctDeleteInstructionOperation{typed.instruction.id};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctDeleteInstructionOperation>) {
            const auto found = instructions_.find(typed.instruction);
            const auto before = placement(typed.instruction);
            if (found == instructions_.end() || !before.has_value())
                return issue("InstructionNotFound", "The deleted instruction does not exist.",
                    instructionTarget(typed.instruction));
            if (!before->after.has_value())
                return issue("InstructionCannotBeRestored", "The first instruction in a section cannot be deleted.",
                    instructionTarget(typed.instruction));
            auto removed = found->second.value;
            auto semantics = found->second.semantics;
            auto& order = sectionOrder_.at(found->second.section);
            std::erase(order, typed.instruction);
            removeContribution(semantics);
            instructions_.erase(found);
            if (auto section = sections_.find(before->section); section != sections_.end()) {
                if (auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section->second.content))
                    std::erase_if(script->instructions, [&](const auto& value) { return value.id == typed.instruction; });
            }
            SctInstructionStructuralChange change;
            change.instruction = typed.instruction;
            change.before = before;
            change.beforeValue = removed;
            change.beforeSemantics = semantics;
            forward.instructions.push_back(change);
            reverseChanges.instructions.push_back(reversed(change));
            forward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            reverseChanges.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = true;
            reverseChanges.documentChanged = true;
            inverse = SctInsertInstructionAfterOperation{*before->after, std::move(removed)};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctRelocateInstructionAfterOperation>) {
            if (typed.instruction == typed.anchor)
                return issue("InstructionMoveSelfAnchor", "An instruction cannot be positioned relative to itself.",
                    instructionTarget(typed.instruction));
            const auto found = instructions_.find(typed.instruction);
            const auto anchor = instructions_.find(typed.anchor);
            const auto before = placement(typed.instruction);
            if (found == instructions_.end() || anchor == instructions_.end() || !before.has_value())
                return issue("InstructionMoveTargetNotFound", "The moved instruction or its destination anchor does not exist.",
                    instructionTarget(typed.instruction));
            if (found->second.section != anchor->second.section)
                return issue("InstructionMoveCrossesSection", "An instruction cannot be relocated across sections.",
                    instructionTarget(typed.instruction));
            if (!before->after.has_value())
                return issue("InstructionMoveInvalidSource", "The first instruction in a section cannot be moved.",
                    instructionTarget(typed.instruction));
            if (*before->after == typed.anchor)
                return issue("InstructionMoveNoChange", "The instruction is already after the requested anchor.",
                    instructionTarget(typed.instruction));
            auto& order = sectionOrder_.at(found->second.section);
            std::erase(order, typed.instruction);
            const auto destination = std::ranges::find(order, typed.anchor);
            if (destination == order.end())
                return issue("InstructionMoveAnchorLost", "The destination anchor could not be resolved.",
                    instructionTarget(typed.instruction));
            order.insert(std::next(destination), typed.instruction);
            if (auto section = sections_.find(found->second.section); section != sections_.end()) {
                if (auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section->second.content)) {
                    auto value = *std::ranges::find(script->instructions, typed.instruction,
                        &spice::sct::SctDocumentInstruction::id);
                    std::erase_if(script->instructions, [&](const auto& item) { return item.id == typed.instruction; });
                    const auto valueAnchor = std::ranges::find(script->instructions, typed.anchor,
                        &spice::sct::SctDocumentInstruction::id);
                    script->instructions.insert(std::next(valueAnchor), std::move(value));
                }
            }
            const SctInstructionPlacement after{found->second.section, typed.anchor};
            SctInstructionStructuralChange change;
            change.instruction = typed.instruction;
            change.before = before;
            change.after = after;
            change.beforeValue = found->second.value;
            change.afterValue = found->second.value;
            change.beforeSemantics = found->second.semantics;
            change.afterSemantics = found->second.semantics;
            forward.instructions.push_back(change);
            reverseChanges.instructions.push_back(reversed(change));
            forward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            reverseChanges.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = true;
            reverseChanges.documentChanged = true;
            inverse = SctRelocateInstructionAfterOperation{typed.instruction, *before->after};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctReplaceInstructionOperation>) {
            const auto found = instructions_.find(typed.instruction);
            if (found == instructions_.end())
                return issue("InstructionNotFound", "The replaced instruction does not exist.",
                    instructionTarget(typed.instruction));
            if (typed.replacement.id != typed.instruction)
                return issue("InstructionReplacementIdMismatch",
                    "An instruction replacement must retain its stable ID.",
                    instructionTarget(typed.instruction));
            if (typed.replacement.opcode != found->second.value.opcode)
                return issue("InstructionReplacementOpcodeMismatch",
                    "An instruction replacement cannot change the opcode.",
                    instructionTarget(typed.instruction));
            auto previous = found->second.value;
            auto previousSemantics = found->second.semantics;
            auto nextSemantics = spice::sct::SctInstructionSemanticAnalyzer::build(
                typed.replacement);
            removeContribution(previousSemantics);
            found->second.value = typed.replacement;
            found->second.semantics = nextSemantics;
            if (auto section = sections_.find(found->second.section); section != sections_.end()) {
                if (auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section->second.content)) {
                    const auto value = std::ranges::find(script->instructions, typed.instruction,
                        &spice::sct::SctDocumentInstruction::id);
                    if (value != script->instructions.end()) *value = typed.replacement;
                }
            }
            addContribution(nextSemantics);
            SctInstructionStructuralChange change;
            change.instruction = typed.instruction;
            change.before = placement(typed.instruction);
            change.after = change.before;
            change.beforeValue = previous;
            change.afterValue = typed.replacement;
            change.beforeSemantics = previousSemantics;
            change.afterSemantics = nextSemantics;
            forward.instructions.push_back(change);
            reverseChanges.instructions.push_back(reversed(change));
            forward.modified.push_back(instructionTarget(typed.instruction));
            reverseChanges.modified.push_back(instructionTarget(typed.instruction));
            forward.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            reverseChanges.invalidations = SctDerivedAnalysisInvalidation::StructuredControlFlow;
            forward.documentChanged = true;
            reverseChanges.documentChanged = true;
            inverse = SctReplaceInstructionOperation{typed.instruction, std::move(previous)};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctReplaceTextValueOperation>) {
            auto* current = const_cast<spice::sct::SctTextValue*>(textValue(typed.target));
            if (current == nullptr)
                return issue("TextTargetNotFound", "The replaced text entity does not exist.",
                    messageNavigation(typed.target));
            auto previous = *current;
            const auto previousProvenance = textRepairProvenance(typed.target);
            *current = typed.value;
            if (typed.updatesRepairProvenance) {
                if (typed.repairProvenance)
                    textRepairProvenance_[textIdentity(typed.target)] = *typed.repairProvenance;
                else
                    textRepairProvenance_.erase(textIdentity(typed.target));
            }
            std::visit([&](const auto id) {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, spice::sct::SctStringId>) {
                    for (auto& [sectionId, sectionValue] : sections_) {
                        if (auto* strings = std::get_if<spice::sct::SctStringSectionContent>(&sectionValue.content);
                            strings != nullptr && strings->string.id == id) {
                            strings->string.value = typed.value;
                            break;
                        }
                    }
                } else if (const auto entry = footerEntries_.find(id); entry != footerEntries_.end()) {
                    entry->second.value = typed.value;
                }
            }, typed.target);
            const auto target = messageNavigation(typed.target);
            forward.textValues.push_back({typed.target, previous, typed.value});
            reverseChanges.textValues.push_back({typed.target, typed.value, previous});
            forward.modified.push_back(target);
            reverseChanges.modified.push_back(target);
            forward.documentChanged = true;
            reverseChanges.documentChanged = true;
            inverse = SctReplaceTextValueOperation{typed.target, std::move(previous),
                typed.updatesRepairProvenance, previousProvenance};
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, SctInsertFooterEntryAfterOperation>) {
            if (footerEntries_.contains(typed.entry.id))
                return issue("FooterEntryAlreadyExists", "The footer entry ID already exists.");
            auto destination = footerOrder_.begin();
            if (typed.anchor) {
                const auto anchor = std::ranges::find(footerOrder_, *typed.anchor);
                if (anchor == footerOrder_.end()) return issue("FooterEntryAnchorNotFound", "The footer insertion anchor does not exist.");
                destination = std::next(anchor);
            }
            footerOrder_.insert(destination, typed.entry.id);
            footerEntries_.emplace(typed.entry.id, typed.entry);
            nextFooterEntryId_ = std::max(nextFooterEntryId_, typed.entry.id.value() + 1u);
            SctFooterEntryStructuralChange change;
            change.entry = typed.entry.id;
            change.after = SctFooterEntryPlacement{typed.anchor};
            change.afterValue = typed.entry;
            forward.footerEntries.push_back(change);
            reverseChanges.footerEntries.push_back(reversed(change));
            forward.documentChanged = reverseChanges.documentChanged = true;
            inverse = SctDeleteFooterEntryOperation{typed.entry.id};
            return std::nullopt;
        } else {
            const auto found = footerEntries_.find(typed.entry);
            if (found == footerEntries_.end())
                return issue("FooterEntryNotFound", "The deleted footer entry does not exist.");
            const auto position = std::ranges::find(footerOrder_, typed.entry);
            std::optional<spice::sct::SctFooterEntryId> anchor;
            if (position != footerOrder_.begin()) anchor = *std::prev(position);
            auto removed = found->second;
            footerEntries_.erase(found);
            footerOrder_.erase(position);
            SctFooterEntryStructuralChange change;
            change.entry = typed.entry;
            change.before = SctFooterEntryPlacement{anchor};
            change.beforeValue = removed;
            forward.footerEntries.push_back(change);
            reverseChanges.footerEntries.push_back(reversed(change));
            forward.documentChanged = reverseChanges.documentChanged = true;
            inverse = SctInsertFooterEntryAfterOperation{anchor, std::move(removed)};
            return std::nullopt;
        }
    }, operation);
}

void SctWorkingState::addContribution(
    const spice::sct::SctInstructionSemanticContribution& contribution) {
    for (const auto& reference : contribution.references) {
        if (const auto* target = std::get_if<spice::sct::SctInstructionId>(&reference.target))
            ++incomingReferences_[*target];
    }
}

void SctWorkingState::removeContribution(
    const spice::sct::SctInstructionSemanticContribution& contribution) {
    for (const auto& reference : contribution.references) {
        const auto* target = std::get_if<spice::sct::SctInstructionId>(&reference.target);
        if (target == nullptr) continue;
        const auto found = incomingReferences_.find(*target);
        if (found == incomingReferences_.end()) continue;
        if (found->second <= 1u) incomingReferences_.erase(found);
        else --found->second;
    }
}

}  // namespace salsa::core
