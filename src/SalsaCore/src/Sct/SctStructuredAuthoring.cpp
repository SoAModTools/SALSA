#include "SalsaCore/Sct/SctStructuredAuthoring.h"

#include "SalsaCore/Sct/SctWorkingState.h"

#include <algorithm>
#include <ranges>

namespace salsa::core {

SctStructuredAuthoringState::SctStructuredAuthoringState(
    const std::span<const SctAuthoredArm> arms)
    : arms_(arms.begin(), arms.end()) {
    for (const auto& arm : arms_) nextId_ = std::max(nextId_, arm.id.value + 1u);
}

SctAuthoredArmId SctStructuredAuthoringState::nextId() const noexcept {
    return SctAuthoredArmId{nextId_};
}

std::span<const SctAuthoredArm> SctStructuredAuthoringState::arms() const noexcept {
    return arms_;
}

const SctAuthoredArm* SctStructuredAuthoringState::find(
    const SctAuthoredArmId id) const noexcept {
    const auto found = std::ranges::find(arms_, id, &SctAuthoredArm::id);
    return found == arms_.end() ? nullptr : &*found;
}

SctStructuredAuthoringApplication SctStructuredAuthoringState::apply(
    const SctStructuredAuthoringOperationBatch& batch) {
    SctStructuredAuthoringApplication result;
    if (batch.operations.empty()) {
        result.issue = "An authoring operation batch cannot be empty.";
        return result;
    }
    auto candidate = arms_;
    auto candidateNext = nextId_;
    for (const auto& operation : batch.operations) {
        if (!operation.id.valid()) {
            result.issue = "An authored arm ID cannot be zero.";
            return result;
        }
        const auto found = std::ranges::find(candidate, operation.id, &SctAuthoredArm::id);
        const auto current = found == candidate.end()
            ? std::optional<SctAuthoredArm>{} : std::optional<SctAuthoredArm>{*found};
        if (current != operation.before) {
            result.issue = "The authored arm no longer has the expected state.";
            return result;
        }
        if (operation.after && operation.after->id != operation.id) {
            result.issue = "The replacement authored arm has a different ID.";
            return result;
        }
        if (found != candidate.end()) {
            if (operation.after) *found = *operation.after;
            else candidate.erase(found);
        } else if (operation.after) {
            candidate.push_back(*operation.after);
        }
        candidateNext = std::max(candidateNext, operation.id.value + 1u);
        result.changes.push_back({operation.id, operation.before, operation.after});
        result.inverse.operations.insert(result.inverse.operations.begin(),
            SctSetAuthoredArmOperation{operation.id, operation.after, operation.before});
    }
    std::ranges::sort(candidate, {}, &SctAuthoredArm::id);
    arms_ = std::move(candidate);
    nextId_ = candidateNext;
    return result;
}

SctSemanticEditorProjection SctSemanticEditorProjection::build(
    const spice::sct::SctStructuredControlFlowAnalysis& analysis,
    const SctWorkingState& workingState,
    const SctStructuredAuthoringState& authoring,
    const RevisionId workingRevision,
    const RevisionId verifiedRevision) {
    SctSemanticEditorProjection result;
    for (const auto& arm : authoring.arms()) {
        SctSemanticAuthoredArmProjection projected;
        projected.arm = arm;
        for (const auto member : arm.members) {
            if (const auto* instruction = workingState.instruction(member); instruction != nullptr)
                projected.visibleMembers.push_back(*instruction);
        }
        if (arm.realization == SctAuthoredArmRealization::Virtual) {
            projected.status = arm.kind
                    == spice::sct::SctStructuredArmKind::SwitchCase
                && !arm.caseValue.has_value()
                ? SctSemanticArmStatus::NeedsValue
                : SctSemanticArmStatus::Virtual;
        } else if (workingRevision != verifiedRevision) {
            projected.status = SctSemanticArmStatus::PendingVerification;
        } else {
            bool matched = false;
            if (const auto* section = analysis.findSection(arm.controller.section)) {
                for (const auto& region : section->regions) {
                    if (region.id.headerInstruction != arm.controller.instruction) continue;
                    const auto foundArm = std::ranges::find_if(region.arms, [&](const auto& candidate) {
                        if (candidate.kind != arm.kind) return false;
                        if (arm.kind != spice::sct::SctStructuredArmKind::SwitchCase)
                            return true;
                        return arm.caseValue && std::ranges::any_of(candidate.caseLabels,
                            [&](const auto& label) { return label.value == arm.caseValue; });
                    });
                    matched = foundArm != region.arms.end();
                    if (matched) break;
                }
            }
            projected.status = matched
                ? SctSemanticArmStatus::Verified : SctSemanticArmStatus::Conflicted;
        }
        result.authoredArms_.push_back(std::move(projected));
    }
    return result;
}

const SctSemanticAuthoredArmProjection* SctSemanticEditorProjection::find(
    const SctAuthoredArmId id) const noexcept {
    const auto found = std::ranges::find(authoredArms_, id,
        [](const auto& arm) { return arm.arm.id; });
    return found == authoredArms_.end() ? nullptr : &*found;
}

}  // namespace salsa::core
