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

[[nodiscard]] SctOperationIssue issue(
    std::string code, std::string message,
    std::optional<SctNavigationTarget> target = std::nullopt) {
    return {std::move(code), std::move(message), target};
}

void appendChanges(SctEditChangeSet& target, SctEditChangeSet source) {
    target.instructions.insert(target.instructions.end(),
        std::make_move_iterator(source.instructions.begin()),
        std::make_move_iterator(source.instructions.end()));
    target.modified.insert(target.modified.end(),
        std::make_move_iterator(source.modified.begin()),
        std::make_move_iterator(source.modified.end()));
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

}  // namespace

SctWorkingState::SctWorkingState(
    std::shared_ptr<const spice::sct::SctDocument> checkpoint)
    : checkpoint_(std::move(checkpoint)) {
    if (!checkpoint_) return;
    nextInstructionId_ = checkpoint_->nextInstructionIdValue();
    for (const auto& section : checkpoint_->sections) {
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
            if (const auto* message = std::get_if<spice::sct::SctMessage>(
                    &strings->string.value)) {
                stringMessages_.emplace(strings->string.id, *message);
            }
        }
    }
    for (const auto& entry : checkpoint_->footerEntries) {
        if (const auto* message = std::get_if<spice::sct::SctMessage>(&entry.value))
            footerMessages_.emplace(entry.id, *message);
    }
    for (const auto& attachment : checkpoint_->opaqueAttachments) {
        if (const auto* instruction = std::get_if<spice::sct::SctInstructionId>(
                &attachment.anchor)) {
            opaqueAttachments_[*instruction].push_back(attachment.id);
        }
    }
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

const spice::sct::SctMessage* SctWorkingState::message(
    const SctMessageTarget& target) const noexcept {
    return std::visit([this](const auto id) -> const spice::sct::SctMessage* {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            const auto found = stringMessages_.find(id);
            return found == stringMessages_.end() ? nullptr : &found->second;
        } else {
            const auto found = footerMessages_.find(id);
            return found == footerMessages_.end() ? nullptr : &found->second;
        }
    }, target);
}

std::uint64_t SctWorkingState::nextInstructionIdValue() const noexcept {
    return nextInstructionId_;
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
        if constexpr (std::is_same_v<T, SctInsertInstructionAfterOperation>) {
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
            nextInstructionId_ = std::max(nextInstructionId_, typed.instruction.id.value() + 1u);
            addContribution(semantics);
            SctInstructionStructuralChange change;
            change.instruction = typed.instruction.id;
            change.after = after;
            change.afterValue = typed.instruction;
            change.afterSemantics = semantics;
            forward.instructions.push_back(change);
            reverseChanges.instructions.push_back(reversed(change));
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
            SctInstructionStructuralChange change;
            change.instruction = typed.instruction;
            change.before = before;
            change.beforeValue = removed;
            change.beforeSemantics = semantics;
            forward.instructions.push_back(change);
            reverseChanges.instructions.push_back(reversed(change));
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
            inverse = SctRelocateInstructionAfterOperation{typed.instruction, *before->after};
            return std::nullopt;
        } else {
            auto* current = const_cast<spice::sct::SctMessage*>(message(typed.target));
            if (current == nullptr)
                return issue("MessageTargetNotFound", "The replaced message does not exist or is not semantic SCT text.",
                    messageNavigation(typed.target));
            auto previous = *current;
            *current = typed.message;
            const auto target = messageNavigation(typed.target);
            forward.modified.push_back(target);
            reverseChanges.modified.push_back(target);
            inverse = SctReplaceMessageOperation{typed.target, std::move(previous)};
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
