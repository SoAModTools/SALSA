#include "SalsaCore/Sct/SctStructuredAuthoring.h"

#include "SalsaCore/Sct/SctWorkingState.h"

#include <algorithm>
#include <ranges>

namespace salsa::core {

SctStructuredAuthoringState::SctStructuredAuthoringState(
    const std::span<const SctAuthoredArm> arms,
    const std::span<const SctUnboundReferenceOrigin> unboundReferences,
    const std::span<const SctVariableAlias> aliases,
    const std::span<const SctEntityAnnotation> annotations,
    const std::span<const SctSectionFolder> folders)
    : arms_(arms.begin(), arms.end()),
      unboundReferences_(unboundReferences.begin(), unboundReferences.end()),
      aliases_(aliases.begin(), aliases.end()), annotations_(annotations.begin(), annotations.end()),
      folders_(folders.begin(), folders.end()) {
    for (const auto& arm : arms_) nextId_ = std::max(nextId_, arm.id.value + 1u);
    for (const auto& folder : folders_)
        nextFolderId_ = std::max(nextFolderId_, folder.id.value + 1u);
    std::ranges::sort(unboundReferences_, {}, &SctUnboundReferenceOrigin::site);
    std::ranges::sort(aliases_, {}, &SctVariableAlias::variable);
    std::ranges::sort(annotations_, {}, &SctEntityAnnotation::target);
    std::ranges::sort(folders_, {}, [](const auto& value) { return value.id.value; });
}

std::span<const SctUnboundReferenceOrigin>
SctStructuredAuthoringState::unboundReferences() const noexcept {
    return unboundReferences_;
}

std::span<const SctVariableAlias> SctStructuredAuthoringState::aliases() const noexcept {
    return aliases_;
}

const SctVariableAlias* SctStructuredAuthoringState::findAlias(
    const SctVariableKey variable) const noexcept {
    const auto found = std::ranges::find(aliases_, variable, &SctVariableAlias::variable);
    return found == aliases_.end() ? nullptr : &*found;
}

std::span<const SctEntityAnnotation> SctStructuredAuthoringState::annotations() const noexcept {
    return annotations_;
}

const SctEntityAnnotation* SctStructuredAuthoringState::findAnnotation(
    const SctAuthoringTarget target) const noexcept {
    const auto found = std::ranges::find(annotations_, target, &SctEntityAnnotation::target);
    return found == annotations_.end() ? nullptr : &*found;
}

std::span<const SctSectionFolder> SctStructuredAuthoringState::folders() const noexcept {
    return folders_;
}

const SctSectionFolder* SctStructuredAuthoringState::findFolder(
    const SctSectionFolderId id) const noexcept {
    const auto found = std::ranges::find(folders_, id, &SctSectionFolder::id);
    return found == folders_.end() ? nullptr : &*found;
}

SctSectionFolderId SctStructuredAuthoringState::nextFolderId() const noexcept {
    return {nextFolderId_};
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
    if (batch.empty()) {
        result.issue = "An authoring operation batch cannot be empty.";
        return result;
    }
    auto candidate = arms_;
    auto candidateUnbound = unboundReferences_;
    auto candidateAliases = aliases_;
    auto candidateAnnotations = annotations_;
    auto candidateFolders = folders_;
    auto candidateNext = nextId_;
    auto candidateNextFolder = nextFolderId_;
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
    for (const auto& operation : batch.unboundReferences) {
        if (!operation.site.instruction) {
            result.issue = "An unbound reference site cannot use instruction ID zero.";
            return result;
        }
        const auto found = std::ranges::find(candidateUnbound, operation.site,
            &SctUnboundReferenceOrigin::site);
        const auto current = found == candidateUnbound.end()
            ? std::optional<SctUnboundReferenceOrigin>{}
            : std::optional<SctUnboundReferenceOrigin>{*found};
        if (current != operation.before) {
            result.issue = "The unbound reference origin no longer has the expected state.";
            return result;
        }
        if (operation.after && operation.after->site != operation.site) {
            result.issue = "The replacement unbound reference origin has a different site.";
            return result;
        }
        if (found != candidateUnbound.end()) {
            if (operation.after) *found = *operation.after;
            else candidateUnbound.erase(found);
        } else if (operation.after) {
            candidateUnbound.push_back(*operation.after);
        }
        result.inverse.unboundReferences.insert(
            result.inverse.unboundReferences.begin(),
            SctSetUnboundReferenceOriginOperation{
                operation.site, operation.after, operation.before});
    }
    for (const auto& operation : batch.aliases) {
        const auto found = std::ranges::find(candidateAliases, operation.variable,
            &SctVariableAlias::variable);
        const auto current = found == candidateAliases.end()
            ? std::optional<SctVariableAlias>{} : std::optional<SctVariableAlias>{*found};
        if (current != operation.before || (operation.after
                && operation.after->variable != operation.variable)) {
            result.issue = "The variable alias no longer has the expected state.";
            return result;
        }
        if (found != candidateAliases.end()) {
            if (operation.after) *found = *operation.after;
            else candidateAliases.erase(found);
        } else if (operation.after) candidateAliases.push_back(*operation.after);
        result.inverse.aliases.insert(result.inverse.aliases.begin(),
            SctSetVariableAliasOperation{operation.variable, operation.after, operation.before});
    }
    for (const auto& operation : batch.annotations) {
        const auto found = std::ranges::find(candidateAnnotations, operation.target,
            &SctEntityAnnotation::target);
        const auto current = found == candidateAnnotations.end()
            ? std::optional<SctEntityAnnotation>{} : std::optional<SctEntityAnnotation>{*found};
        if (current != operation.before || (operation.after
                && operation.after->target != operation.target)) {
            result.issue = "The entity annotation no longer has the expected state.";
            return result;
        }
        if (found != candidateAnnotations.end()) {
            if (operation.after) *found = *operation.after;
            else candidateAnnotations.erase(found);
        } else if (operation.after) candidateAnnotations.push_back(*operation.after);
        result.inverse.annotations.insert(result.inverse.annotations.begin(),
            SctSetEntityAnnotationOperation{operation.target, operation.after, operation.before});
    }
    for (const auto& operation : batch.folders) {
        const auto found = std::ranges::find(candidateFolders, operation.id,
            &SctSectionFolder::id);
        const auto current = found == candidateFolders.end()
            ? std::optional<SctSectionFolder>{} : std::optional<SctSectionFolder>{*found};
        if (!operation.id.valid() || current != operation.before || (operation.after
                && operation.after->id != operation.id)) {
            result.issue = "The section folder no longer has the expected state.";
            return result;
        }
        if (found != candidateFolders.end()) {
            if (operation.after) *found = *operation.after;
            else candidateFolders.erase(found);
        } else if (operation.after) candidateFolders.push_back(*operation.after);
        candidateNextFolder = std::max(candidateNextFolder, operation.id.value + 1u);
        result.inverse.folders.insert(result.inverse.folders.begin(),
            SctSetSectionFolderOperation{operation.id, operation.after, operation.before});
    }
    std::ranges::sort(candidate, {}, &SctAuthoredArm::id);
    std::ranges::sort(candidateUnbound, {}, &SctUnboundReferenceOrigin::site);
    std::ranges::sort(candidateAliases, {}, &SctVariableAlias::variable);
    std::ranges::sort(candidateAnnotations, {}, &SctEntityAnnotation::target);
    std::ranges::sort(candidateFolders, {}, [](const auto& value) { return value.id.value; });
    arms_ = std::move(candidate);
    unboundReferences_ = std::move(candidateUnbound);
    aliases_ = std::move(candidateAliases);
    annotations_ = std::move(candidateAnnotations);
    folders_ = std::move(candidateFolders);
    nextId_ = candidateNext;
    nextFolderId_ = candidateNextFolder;
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
