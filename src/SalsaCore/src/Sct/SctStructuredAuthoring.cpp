#include "SalsaCore/Sct/SctStructuredAuthoring.h"

#include "SalsaCore/Sct/SctWorkingState.h"

#include <algorithm>
#include <functional>
#include <ranges>
#include <unordered_set>

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

namespace {

using spice::sct::SctBasicBlockId;
using spice::sct::SctInstructionId;
using spice::sct::SctSectionStructure;
using spice::sct::SctStructuredArm;
using spice::sct::SctStructuredRegion;

[[nodiscard]] SctSemanticNodeKey semanticKey(std::string prefix,
    const std::uint64_t first, const std::uint64_t second = 0,
    const std::uint64_t third = 0) {
    prefix += ':' + std::to_string(first);
    if (second != 0) prefix += ':' + std::to_string(second);
    if (third != 0) prefix += ':' + std::to_string(third);
    return {std::move(prefix)};
}

[[nodiscard]] bool containsBlock(const std::vector<SctBasicBlockId>& blocks,
    const SctBasicBlockId& block) {
    return std::ranges::find(blocks, block) != blocks.end();
}

[[nodiscard]] std::vector<SctInstructionId> instructionsForBlocks(
    const SctSectionStructure& section, const SctWorkingState& workingState,
    const std::vector<SctBasicBlockId>& blocks) {
    std::unordered_set<SctInstructionId> included;
    for (const auto& block : section.blocks) {
        if (!containsBlock(blocks, block.id)) continue;
        included.insert(block.instructions.begin(), block.instructions.end());
    }
    std::vector<SctInstructionId> result;
    for (const auto instruction : workingState.instructionOrder(section.section)) {
        if (included.contains(instruction)) result.push_back(instruction);
    }
    return result;
}

[[nodiscard]] bool isScaffoldingEvidence(
    const spice::sct::SctStructureEvidenceKind kind) {
    return kind == spice::sct::SctStructureEvidenceKind::PreTargetJump
        || kind == spice::sct::SctStructureEvidenceKind::BackwardTerminatorJump
        || kind == spice::sct::SctStructureEvidenceKind::CommonForwardExit;
}

[[nodiscard]] const SctSemanticAuthoredArmProjection* authoredProjectionFor(
    const std::vector<SctSemanticAuthoredArmProjection>& authored,
    const SctInstructionId controller, const SctStructuredArm& arm) {
    const auto found = std::ranges::find_if(authored, [&](const auto& candidate) {
        if (candidate.arm.controller.instruction != controller
            || candidate.arm.kind != arm.kind) return false;
        if (arm.kind != spice::sct::SctStructuredArmKind::SwitchCase) return true;
        return candidate.arm.caseValue && std::ranges::any_of(
            arm.caseLabels, [&](const auto& label) {
                return label.value == candidate.arm.caseValue;
            });
    });
    return found == authored.end() ? nullptr : &*found;
}

[[nodiscard]] const SctStructuredRegion* nestedRegionAt(
    const SctSectionStructure& section, const SctStructuredRegion& parent,
    const SctInstructionId instruction) {
    const auto found = std::ranges::find_if(section.regions, [&](const auto& candidate) {
        return candidate.parent == parent.id
            && candidate.id.headerInstruction == instruction;
    });
    return found == section.regions.end() ? nullptr : &*found;
}

[[nodiscard]] SctSemanticProjectionNode makeInstructionNode(
    const SctInstructionId instruction,
    const std::unordered_set<SctInstructionId>& hidden,
    const std::unordered_map<std::uint64_t, std::size_t>& importedEvidence) {
    SctSemanticProjectionNode node;
    node.key = semanticKey("instruction", instruction.value());
    node.kind = SctSemanticNodeKind::Instruction;
    node.target = SctNavigationTarget{SctNavigationKind::Instruction,
        instruction.value()};
    node.physicalInstructions.push_back(instruction);
    node.hiddenByDefault = hidden.contains(instruction);
    node.verified = true;
    if (const auto found = importedEvidence.find(instruction.value());
        found != importedEvidence.end()) node.importedEvidenceCount = found->second;
    return node;
}

[[nodiscard]] SctSemanticProjectionNode buildRegionNode(
    const SctSectionStructure& section, const SctStructuredRegion& region,
    const SctWorkingState& workingState,
    const std::vector<SctSemanticAuthoredArmProjection>& authored,
    const std::unordered_set<SctInstructionId>& hidden,
    const std::unordered_map<std::uint64_t, std::size_t>& importedEvidence) {
    SctSemanticProjectionNode node;
    node.key = semanticKey("region", region.id.section.value(),
        region.id.headerInstruction.value(),
        static_cast<std::uint64_t>(region.id.kind) + 1u);
    node.kind = SctSemanticNodeKind::Region;
    node.target = SctNavigationTarget{SctNavigationKind::Instruction,
        region.id.headerInstruction.value()};
    node.controller = region.id.headerInstruction;
    if (region.join) node.joinInstruction = region.join->entryInstruction;
    node.regionKind = region.id.kind;
    node.confidence = region.minimumEdgeConfidence;
    node.evidence = region.evidence;
    node.verified = true;
    node.physicalInstructions = instructionsForBlocks(section, workingState, region.members);
    for (const auto& evidence : region.evidence) {
        if (isScaffoldingEvidence(evidence.kind) && evidence.source)
            node.managedScaffolding.push_back(*evidence.source);
    }
    if (const auto found = importedEvidence.find(region.id.headerInstruction.value());
        found != importedEvidence.end()) node.importedEvidenceCount = found->second;

    std::size_t armOrdinal = 0;
    for (const auto& arm : region.arms) {
        SctSemanticProjectionNode armNode;
        armNode.key = semanticKey("arm", region.id.headerInstruction.value(),
            static_cast<std::uint64_t>(arm.kind) + 1u, ++armOrdinal);
        armNode.kind = SctSemanticNodeKind::Arm;
        armNode.controller = region.id.headerInstruction;
        if (region.join) armNode.joinInstruction = region.join->entryInstruction;
        armNode.armKind = arm.kind;
        armNode.verified = true;
        armNode.physicalInstructions = instructionsForBlocks(
            section, workingState, arm.blocks);
        for (const auto& label : arm.caseLabels) {
            if (label.value) armNode.caseValues.push_back(*label.value);
            armNode.caseGroupOrdinals.push_back(label.repeatedGroupOrdinal);
        }
        if (armNode.caseValues.size() == 1u)
            armNode.caseValue = armNode.caseValues.front();
        if (const auto* projected = authoredProjectionFor(
                authored, region.id.headerInstruction, arm)) {
            armNode.authoredArm = projected->arm.id;
            armNode.armStatus = projected->status;
            armNode.virtualArm = projected->arm.realization
                == SctAuthoredArmRealization::Virtual;
            armNode.needsValue = projected->status == SctSemanticArmStatus::NeedsValue;
            armNode.canReturnToEmpty = projected->arm.realization
                    == SctAuthoredArmRealization::Physical
                && projected->visibleMembers.size() == 1u;
            armNode.managedScaffolding = projected->arm.managedScaffolding;
        }

        std::unordered_set<SctInstructionId> emitted;
        for (const auto instruction : armNode.physicalInstructions) {
            if (instruction == region.id.headerInstruction
                || emitted.contains(instruction)) continue;
            if (const auto* nested = nestedRegionAt(section, region, instruction)) {
                auto child = buildRegionNode(section, *nested, workingState,
                    authored, hidden, importedEvidence);
                emitted.insert(child.physicalInstructions.begin(),
                    child.physicalInstructions.end());
                armNode.children.push_back(std::move(child));
            } else {
                armNode.children.push_back(makeInstructionNode(
                    instruction, hidden, importedEvidence));
                emitted.insert(instruction);
            }
        }
        if (armNode.children.empty()) {
            SctSemanticProjectionNode placeholder;
            placeholder.key = semanticKey("empty", region.id.headerInstruction.value(),
                static_cast<std::uint64_t>(arm.kind) + 1u, armOrdinal);
            placeholder.kind = SctSemanticNodeKind::Placeholder;
            placeholder.controller = region.id.headerInstruction;
            placeholder.armKind = arm.kind;
            placeholder.authorable = false;
            armNode.children.push_back(std::move(placeholder));
        }
        node.children.push_back(std::move(armNode));
    }

    for (const auto& projected : authored) {
        if (projected.arm.controller.instruction != region.id.headerInstruction
            || projected.arm.realization != SctAuthoredArmRealization::Virtual)
            continue;
        const bool represented = std::ranges::any_of(node.children, [&](const auto& child) {
            return child.authoredArm == projected.arm.id;
        });
        if (represented) continue;
        SctSemanticProjectionNode armNode;
        armNode.key = semanticKey("authored-arm", projected.arm.id.value);
        armNode.kind = SctSemanticNodeKind::Arm;
        armNode.controller = projected.arm.controller.instruction;
        armNode.joinInstruction = projected.arm.expectedJoin;
        armNode.authoredArm = projected.arm.id;
        armNode.armKind = projected.arm.kind;
        armNode.caseValue = projected.arm.caseValue;
        if (projected.arm.caseValue)
            armNode.caseValues.push_back(*projected.arm.caseValue);
        armNode.armStatus = projected.status;
        armNode.verified = true;
        armNode.virtualArm = true;
        armNode.needsValue = projected.status == SctSemanticArmStatus::NeedsValue;
        SctSemanticProjectionNode placeholder;
        placeholder.key = semanticKey("authored-empty", projected.arm.id.value);
        placeholder.kind = SctSemanticNodeKind::Placeholder;
        placeholder.controller = projected.arm.controller.instruction;
        placeholder.authoredArm = projected.arm.id;
        placeholder.armKind = projected.arm.kind;
        placeholder.virtualArm = true;
        placeholder.needsValue = armNode.needsValue;
        placeholder.authorable = false;
        armNode.children.push_back(std::move(placeholder));
        node.children.push_back(std::move(armNode));
    }
    return node;
}

template <typename Predicate>
[[nodiscard]] const SctSemanticProjectionNode* findNode(
    const std::span<const SctSemanticProjectionNode> nodes,
    const Predicate& predicate) {
    for (const auto& node : nodes) {
        if (predicate(node)) return &node;
        if (const auto* nested = findNode(
                std::span<const SctSemanticProjectionNode>{node.children}, predicate))
            return nested;
    }
    return nullptr;
}

struct ProjectionLocation final {
    const SctSemanticProjectionNode* node = nullptr;
    const SctSemanticProjectionNode* parent = nullptr;
    std::size_t ordinal = 0;
};

[[nodiscard]] std::optional<ProjectionLocation> locateNode(
    const std::span<const SctSemanticProjectionNode> nodes,
    const SctSemanticNodeKey& key, const SctSemanticProjectionNode* parent = nullptr) {
    for (std::size_t ordinal = 0; ordinal < nodes.size(); ++ordinal) {
        if (nodes[ordinal].key == key) return ProjectionLocation{&nodes[ordinal], parent, ordinal};
        if (auto nested = locateNode(nodes[ordinal].children, key, &nodes[ordinal]))
            return nested;
    }
    return std::nullopt;
}

[[nodiscard]] Diagnostic selectionError(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctFragment,
        std::move(message), {}};
}

[[nodiscard]] bool isDescendantOf(const SctSemanticEditorProjection& projection,
    const SctSemanticNodeKey& descendant, const SctSemanticNodeKey& ancestor) {
    auto location = locateNode(projection.roots(), descendant);
    while (location && location->parent != nullptr) {
        if (location->parent->key == ancestor) return true;
        location = locateNode(projection.roots(), location->parent->key);
    }
    return false;
}

[[nodiscard]] std::optional<SctInstructionId> instructionBefore(
    const SctWorkingState& state, const SctInstructionId instruction) {
    return state.instructionBefore(instruction);
}

}  // namespace

SctSemanticEditorProjection SctSemanticEditorProjection::build(
    const spice::sct::SctStructuredControlFlowAnalysis& analysis,
    const SctWorkingState& workingState,
    const SctStructuredAuthoringState& authoring,
    const RevisionId workingRevision,
    const RevisionId verifiedRevision) {
    SctSemanticEditorProjection result;
    result.workingRevision_ = workingRevision;
    result.verifiedRevision_ = verifiedRevision;
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

    std::unordered_map<std::uint64_t, std::size_t> importedEvidence;
    std::unordered_set<SctInstructionId> hidden;
    for (const auto& section : analysis.sections()) {
        for (const auto& candidate : section.historicalCandidates)
            ++importedEvidence[candidate.sourceInstruction.value()];
        for (const auto& region : section.regions) {
            for (const auto& evidence : region.evidence) {
                if (isScaffoldingEvidence(evidence.kind) && evidence.source)
                    hidden.insert(*evidence.source);
            }
        }
    }
    for (const auto& section : analysis.sections()) {
        SctSemanticProjectionNode root;
        root.key = semanticKey("section", section.section.value());
        root.kind = SctSemanticNodeKind::Section;
        root.target = SctNavigationTarget{SctNavigationKind::Section,
            section.section.value()};
        root.verified = workingRevision == verifiedRevision;
        root.physicalInstructions.assign(
            workingState.instructionOrder(section.section).begin(),
            workingState.instructionOrder(section.section).end());
        root.importedEvidenceCount = section.historicalCandidates.size();
        std::unordered_set<SctInstructionId> covered;
        for (const auto& region : section.regions) {
            if (region.parent) continue;
            const auto closure = instructionsForBlocks(section, workingState, region.members);
            covered.insert(closure.begin(), closure.end());
        }
        for (const auto instruction : workingState.instructionOrder(section.section)) {
            const auto region = std::ranges::find_if(section.regions, [&](const auto& item) {
                return !item.parent && item.id.headerInstruction == instruction;
            });
            if (region != section.regions.end()) {
                root.children.push_back(buildRegionNode(section, *region,
                    workingState, result.authoredArms_, hidden, importedEvidence));
            } else if (!covered.contains(instruction)) {
                root.children.push_back(makeInstructionNode(
                    instruction, hidden, importedEvidence));
            }
        }
        for (std::size_t ordinal = 0; ordinal < section.issues.size(); ++ordinal) {
            const auto& issue = section.issues[ordinal];
            SctSemanticProjectionNode issueNode;
            issueNode.key = semanticKey("issue", section.section.value(), ordinal + 1u);
            issueNode.kind = SctSemanticNodeKind::Issue;
            issueNode.issue = issue;
            issueNode.evidence = issue.evidence;
            issueNode.authorable = false;
            if (issue.instruction) issueNode.target = SctNavigationTarget{
                SctNavigationKind::Instruction, issue.instruction->value()};
            root.children.push_back(std::move(issueNode));
        }
        result.roots_.push_back(std::move(root));
    }
    return result;
}

const SctSemanticAuthoredArmProjection* SctSemanticEditorProjection::find(
    const SctAuthoredArmId id) const noexcept {
    const auto found = std::ranges::find(authoredArms_, id,
        [](const auto& arm) { return arm.arm.id; });
    return found == authoredArms_.end() ? nullptr : &*found;
}

const SctSemanticProjectionNode* SctSemanticEditorProjection::find(
    const SctSemanticNodeKey& key) const noexcept {
    return findNode(std::span<const SctSemanticProjectionNode>{roots_},
        [&](const auto& node) { return node.key == key; });
}

const SctSemanticProjectionNode* SctSemanticEditorProjection::find(
    const SctNavigationTarget target) const noexcept {
    return findNode(std::span<const SctSemanticProjectionNode>{roots_},
        [&](const auto& node) { return node.target == target; });
}

Result<SctSemanticSelection> SctSemanticCommandPlanner::normalizeSelection(
    const SctSemanticEditorProjection& projection,
    const std::span<const SctSemanticNodeKey> nodes) {
    if (!projection.current()) return Result<SctSemanticSelection>::failure(
        selectionError("Semantic structure is still being verified for the current revision."));
    if (nodes.empty()) return Result<SctSemanticSelection>::failure(
        selectionError("Select one or more semantic units first."));
    std::vector<SctSemanticNodeKey> unique;
    for (const auto& key : nodes) {
        if (!projection.find(key)) return Result<SctSemanticSelection>::failure(
            selectionError("The semantic selection is stale."));
        if (std::ranges::find(unique, key) == unique.end()) unique.push_back(key);
    }
    std::erase_if(unique, [&](const auto& candidate) {
        return std::ranges::any_of(unique, [&](const auto& ancestor) {
            return candidate != ancestor && isDescendantOf(projection, candidate, ancestor);
        });
    });
    std::vector<ProjectionLocation> locations;
    for (const auto& key : unique) locations.push_back(*locateNode(projection.roots(), key));
    const auto* parent = locations.front().parent;
    if (std::ranges::any_of(locations, [&](const auto& location) {
            return location.parent != parent || !location.node->authorable
                || location.node->kind == SctSemanticNodeKind::Placeholder
                || location.node->kind == SctSemanticNodeKind::Issue
                || location.node->kind == SctSemanticNodeKind::BasicBlock;
        })) return Result<SctSemanticSelection>::failure(selectionError(
            "Semantic selections must contain authorable sibling units."));
    std::ranges::sort(locations, {}, &ProjectionLocation::ordinal);
    for (std::size_t ordinal = 1; ordinal < locations.size(); ++ordinal) {
        if (locations[ordinal].ordinal != locations[ordinal - 1u].ordinal + 1u)
            return Result<SctSemanticSelection>::failure(selectionError(
                "Semantic selections must be contiguous sibling units."));
    }
    SctSemanticSelection selection;
    selection.revision = projection.workingRevision();
    if (parent) selection.parent = parent->key;
    for (const auto& location : locations) selection.units.push_back(location.node->key);
    return Result<SctSemanticSelection>::success(std::move(selection));
}

Result<SctSemanticCommandPlan> SctSemanticCommandPlanner::planSelection(
    const SctSemanticEditorProjection& projection,
    const SctSemanticSelection& selection) {
    if (!projection.current() || selection.revision != projection.workingRevision())
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "The semantic selection no longer belongs to the current verified revision."));
    SctSemanticCommandPlan plan;
    plan.selection = selection;
    std::unordered_set<SctInstructionId> emitted;
    for (const auto& key : selection.units) {
        const auto* node = projection.find(key);
        if (!node) return Result<SctSemanticCommandPlan>::failure(
            selectionError("The semantic selection is stale."));
        if (node->virtualArm && node->authoredArm)
            plan.authoredArmsToRemove.push_back(*node->authoredArm);
        for (const auto instruction : node->physicalInstructions) {
            if (emitted.insert(instruction).second)
                plan.physicalInstructions.push_back(instruction);
        }
    }
    if (plan.physicalInstructions.empty() && plan.authoredArmsToRemove.empty())
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "The selected semantic units have no editable document content."));
    return Result<SctSemanticCommandPlan>::success(std::move(plan));
}

Result<SctSemanticCommandPlan> SctSemanticCommandPlanner::planMove(
    const SctSemanticEditorProjection& projection,
    const SctWorkingState& workingState,
    const SctSemanticSelection& selection,
    const SctSemanticMoveDirection direction) {
    auto planned = planSelection(projection, selection);
    if (!planned) return planned;
    if (std::ranges::any_of(selection.units, [&](const auto& key) {
            const auto* node = projection.find(key);
            return node != nullptr && node->kind == SctSemanticNodeKind::Arm;
        }))
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "Complete semantic arms cannot be transplanted between controllers."));
    if (!planned.value().authoredArmsToRemove.empty())
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "An empty semantic arm cannot be moved with the instruction movement command."));
    auto first = locateNode(projection.roots(), selection.units.front());
    auto last = locateNode(projection.roots(), selection.units.back());
    if (!first || !last || first->parent != last->parent)
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "The semantic selection is stale."));
    const auto siblings = first->parent == nullptr
        ? projection.roots()
        : std::span<const SctSemanticProjectionNode>{first->parent->children};
    const SctSemanticProjectionNode* adjacent = nullptr;
    if (direction == SctSemanticMoveDirection::Up && first->ordinal > 0u)
        adjacent = &siblings[first->ordinal - 1u];
    if (direction == SctSemanticMoveDirection::Down
        && last->ordinal + 1u < siblings.size()) adjacent = &siblings[last->ordinal + 1u];
    if (adjacent == nullptr) {
        if (first->parent == nullptr || first->parent->kind == SctSemanticNodeKind::Section)
            return Result<SctSemanticCommandPlan>::failure(selectionError(
                "The selection is already at that semantic boundary."));
        const auto* enclosing = first->parent;
        if (enclosing->kind == SctSemanticNodeKind::Arm) {
            auto armLocation = locateNode(projection.roots(), enclosing->key);
            if (!armLocation || armLocation->parent == nullptr)
                return Result<SctSemanticCommandPlan>::failure(selectionError(
                    "The semantic selection cannot leave this arm."));
            enclosing = armLocation->parent;
        }
        if (enclosing->physicalInstructions.empty())
            return Result<SctSemanticCommandPlan>::failure(selectionError(
                "The enclosing semantic unit has no physical boundary."));
        if (direction == SctSemanticMoveDirection::Up) {
            planned.value().anchorAfter = instructionBefore(
                workingState, enclosing->physicalInstructions.front());
        } else {
            planned.value().anchorAfter = enclosing->physicalInstructions.back();
        }
        return planned;
    }
    if (adjacent->physicalInstructions.empty())
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "The adjacent semantic unit has no physical placement."));
    if (direction == SctSemanticMoveDirection::Up)
        planned.value().anchorAfter = instructionBefore(
            workingState, adjacent->physicalInstructions.front());
    else planned.value().anchorAfter = adjacent->physicalInstructions.back();
    return planned;
}

Result<SctSemanticCommandPlan> SctSemanticCommandPlanner::planMove(
    const SctSemanticEditorProjection& projection,
    const SctWorkingState& workingState,
    const SctSemanticSelection& selection,
    const SctSemanticDestination& destination) {
    auto planned = planSelection(projection, selection);
    if (!planned) return planned;
    if (std::ranges::any_of(selection.units, [&](const auto& key) {
            const auto* node = projection.find(key);
            return node != nullptr && node->kind == SctSemanticNodeKind::Arm;
        }))
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "Complete semantic arms cannot be transplanted between controllers."));
    if (destination.revision != projection.workingRevision())
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "The semantic drop destination is stale."));
    const auto* target = projection.find(destination.node);
    if (!target) return Result<SctSemanticCommandPlan>::failure(selectionError(
        "The semantic drop destination no longer exists."));
    if (std::ranges::find(selection.units, target->key) != selection.units.end()
        || std::ranges::any_of(selection.units, [&](const auto& key) {
            return isDescendantOf(projection, target->key, key);
        })) return Result<SctSemanticCommandPlan>::failure(selectionError(
            "A semantic unit cannot be dropped into itself."));
    if (destination.placement == SctSemanticDestinationPlacement::IntoStart
        || destination.placement == SctSemanticDestinationPlacement::IntoEnd) {
        const SctSemanticProjectionNode* container = target;
        if (target->kind == SctSemanticNodeKind::Region) {
            const auto primary = std::ranges::find_if(target->children, [](const auto& child) {
                return child.kind == SctSemanticNodeKind::Arm
                    && (child.armKind == spice::sct::SctStructuredArmKind::Then
                        || child.armKind == spice::sct::SctStructuredArmKind::LoopBody);
            });
            if (primary == target->children.end())
                return Result<SctSemanticCommandPlan>::failure(selectionError(
                    "Choose a specific semantic arm for this drop."));
            container = &*primary;
        }
        if (container->kind != SctSemanticNodeKind::Arm)
            return Result<SctSemanticCommandPlan>::failure(selectionError(
                "This semantic row is not an instruction container."));
        if (container->virtualArm && container->authoredArm) {
            if (std::ranges::any_of(selection.units, [&](const auto& key) {
                    const auto* node = projection.find(key);
                    return node != nullptr
                        && node->kind != SctSemanticNodeKind::Instruction;
                }))
                return Result<SctSemanticCommandPlan>::failure(selectionError(
                    "An empty authored arm accepts instruction siblings only; moving a region would require dismantling its source controller."));
            const auto source = locateNode(projection.roots(), selection.units.front());
            if (source && source->parent != nullptr
                && source->parent->kind == SctSemanticNodeKind::Arm) {
                const bool leavesBody = std::ranges::any_of(
                    source->parent->children, [&](const auto& sibling) {
                        return sibling.kind == SctSemanticNodeKind::Instruction
                            && !sibling.hiddenByDefault
                            && std::ranges::find(selection.units, sibling.key)
                                == selection.units.end();
                    });
                if (!leavesBody)
                    return Result<SctSemanticCommandPlan>::failure(selectionError(
                        "The move would empty its source arm and requires an explicit source-controller detachment plan."));
            }
            planned.value().destinationAuthoredArm = container->authoredArm;
            return planned;
        }
        planned.value().destinationController = container->controller;
        planned.value().destinationArmKind = container->armKind;
        std::vector<SctInstructionId> visible;
        for (const auto& child : container->children) {
            if (child.kind == SctSemanticNodeKind::Placeholder) continue;
            visible.insert(visible.end(), child.physicalInstructions.begin(),
                child.physicalInstructions.end());
        }
        if (destination.placement == SctSemanticDestinationPlacement::IntoStart)
            planned.value().anchorAfter = container->controller;
        else if (!visible.empty()) planned.value().anchorAfter = visible.back();
        else planned.value().anchorAfter = container->controller;
        return planned;
    }
    if (target->physicalInstructions.empty())
        return Result<SctSemanticCommandPlan>::failure(selectionError(
            "The semantic drop target has no physical placement."));
    planned.value().anchorAfter = destination.placement
            == SctSemanticDestinationPlacement::Before
        ? instructionBefore(workingState, target->physicalInstructions.front())
        : std::optional<SctInstructionId>{target->physicalInstructions.back()};
    return planned;
}

}  // namespace salsa::core
