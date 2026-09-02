#include "Sct/SctStructuredOutlineModel.h"

#include "SpiceSCT/SctOpcodeMetadata.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QStringList>

#include <algorithm>
#include <ranges>

namespace salsa::qt {
namespace {

using namespace spice::sct;

QString confidenceName(const spice::sct::SctSemanticConfidence confidence) {
    switch (confidence) {
    case spice::sct::SctSemanticConfidence::Known: return QObject::tr("Known");
    case spice::sct::SctSemanticConfidence::Partial: return QObject::tr("Partial");
    case spice::sct::SctSemanticConfidence::Heuristic: return QObject::tr("Heuristic");
    case spice::sct::SctSemanticConfidence::Unknown: return QObject::tr("Unknown");
    }
    return QObject::tr("Unknown");
}

QString regionName(const SctStructuredRegionKind kind) {
    switch (kind) {
    case SctStructuredRegionKind::If: return QObject::tr("If");
    case SctStructuredRegionKind::IfElse: return QObject::tr("If / Else");
    case SctStructuredRegionKind::While: return QObject::tr("While");
    case SctStructuredRegionKind::NaturalLoop: return QObject::tr("Loop");
    case SctStructuredRegionKind::Switch: return QObject::tr("Switch");
    }
    return QObject::tr("Region");
}

QString armName(const SctStructuredArm& arm) {
    switch (arm.kind) {
    case SctStructuredArmKind::Then: return QObject::tr("Then");
    case SctStructuredArmKind::Else: return QObject::tr("Else");
    case SctStructuredArmKind::LoopBody: return QObject::tr("Body");
    case SctStructuredArmKind::SwitchCase: {
        QStringList labels;
        for (const auto& label : arm.caseLabels) {
            labels.push_back(label.value
                ? QString::number(*label.value)
                : QObject::tr("entry %1").arg(label.repeatedGroupOrdinal));
        }
        return labels.isEmpty() ? QObject::tr("Case")
            : QObject::tr("Case %1").arg(labels.join(QStringLiteral(", ")));
    }
    }
    return QObject::tr("Body");
}

QString issueName(const SctStructureIssueKind kind) {
    switch (kind) {
    case SctStructureIssueKind::UnresolvedControlFlow: return QObject::tr("Unresolved control-flow target");
    case SctStructureIssueKind::CrossSectionNonCallControlFlow: return QObject::tr("Cross-section control flow");
    case SctStructureIssueKind::MissingControlFlow: return QObject::tr("Missing control-flow evidence");
    case SctStructureIssueKind::IrreducibleCycle: return QObject::tr("Irreducible cycle");
    case SctStructureIssueKind::MultipleEntryRegion: return QObject::tr("Multiple-entry region");
    case SctStructureIssueKind::AmbiguousJoin: return QObject::tr("Ambiguous join");
    case SctStructureIssueKind::OverlappingRegions: return QObject::tr("Overlapping region candidates");
    case SctStructureIssueKind::AmbiguousSwitchCases: return QObject::tr("Ambiguous switch cases");
    case SctStructureIssueKind::HistoricalEdgeConflict: return QObject::tr("Historical edge conflicts with current flow");
    case SctStructureIssueKind::RejectedStructuredCandidate: return QObject::tr("Rejected structured candidate");
    }
    return QObject::tr("Structure analysis issue");
}

QString evidenceName(const SctStructureEvidenceKind kind) {
    switch (kind) {
    case SctStructureEvidenceKind::CurrentControlFlow: return QObject::tr("current control flow");
    case SctStructureEvidenceKind::ConditionalFalseTarget: return QObject::tr("conditional false target");
    case SctStructureEvidenceKind::PreTargetJump: return QObject::tr("jump before false target");
    case SctStructureEvidenceKind::BackwardTerminatorJump: return QObject::tr("backward terminator jump");
    case SctStructureEvidenceKind::CommonForwardExit: return QObject::tr("common forward exit");
    case SctStructureEvidenceKind::PhysicalCaseBoundary: return QObject::tr("physical case boundary");
    case SctStructureEvidenceKind::SharedCaseTarget: return QObject::tr("shared case target");
    case SctStructureEvidenceKind::CaseFallthrough: return QObject::tr("case fallthrough");
    case SctStructureEvidenceKind::ImportedControlFlow: return QObject::tr("imported control flow");
    case SctStructureEvidenceKind::ImportedOpaqueControlFlowGap: return QObject::tr("imported opaque-gap edge");
    }
    return QObject::tr("evidence");
}

QString evidenceTooltip(const std::vector<SctStructureEvidence>& evidence) {
    QStringList lines;
    for (const auto& item : evidence) {
        auto line = evidenceName(item.kind) + QStringLiteral(" — ")
            + confidenceName(item.confidence);
        if (item.source) line += QObject::tr("; source %1").arg(item.source->value());
        if (item.target) line += QObject::tr("; target %1").arg(item.target->value());
        if (!item.opaqueAttachments.empty())
            line += QObject::tr("; %1 opaque attachment(s)").arg(item.opaqueAttachments.size());
        lines.push_back(std::move(line));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace

SctStructuredOutlineModel::SctStructuredOutlineModel(QObject* parent)
    : QAbstractItemModel(parent) {}

QModelIndex SctStructuredOutlineModel::index(
    const int row, const int column, const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= 2) return {};
    const auto* parentNode = parentIndex.isValid()
        ? static_cast<Node*>(parentIndex.internalPointer()) : nullptr;
    const auto& children = parentNode == nullptr ? roots_ : parentNode->children;
    if (row >= static_cast<int>(children.size())) return {};
    return createIndex(row, column, children[static_cast<std::size_t>(row)].get());
}

QModelIndex SctStructuredOutlineModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    const auto* node = static_cast<Node*>(child.internalPointer());
    return node == nullptr || node->parent == nullptr
        ? QModelIndex{} : indexForNode(node->parent);
}

int SctStructuredOutlineModel::rowCount(const QModelIndex& parentIndex) const {
    if (parentIndex.isValid() && parentIndex.column() != 0) return 0;
    const auto* node = parentIndex.isValid()
        ? static_cast<Node*>(parentIndex.internalPointer()) : nullptr;
    return static_cast<int>(node == nullptr ? roots_.size() : node->children.size());
}

int SctStructuredOutlineModel::columnCount(const QModelIndex&) const { return 2; }

QVariant SctStructuredOutlineModel::data(const QModelIndex& modelIndex, const int role) const {
    if (!modelIndex.isValid()) return {};
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    if (role == Qt::DisplayRole)
        return modelIndex.column() == 0 ? node->label : node->secondary;
    if (role == Qt::ToolTipRole && !node->tooltip.isEmpty()) return node->tooltip;
    if (role == Qt::FontRole && node->suggested) {
        QFont font;
        font.setItalic(true);
        return font;
    }
    if (role == Qt::ForegroundRole && node->suggested)
        return QBrush(QColor(190, 140, 30));
    return {};
}

QVariant SctStructuredOutlineModel::headerData(
    const int section, const Qt::Orientation orientation, const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? tr("Semantic outline") : tr("State / ID");
}

void SctStructuredOutlineModel::resetFrom(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    std::shared_ptr<const core::SctSemanticEditorProjection> projection) {
    snapshot_ = std::move(snapshot);
    projection_ = std::move(projection);
    rebuild();
}

void SctStructuredOutlineModel::setDeveloperOptions(
    const bool showBasicBlocks, const bool showRejectedEvidence,
    const bool showControlFlowInstructions) {
    if (showBasicBlocks_ == showBasicBlocks
        && showRejectedEvidence_ == showRejectedEvidence
        && showControlFlowInstructions_ == showControlFlowInstructions) return;
    showBasicBlocks_ = showBasicBlocks;
    showRejectedEvidence_ = showRejectedEvidence;
    showControlFlowInstructions_ = showControlFlowInstructions;
    rebuild();
}

std::optional<core::SctNavigationTarget> SctStructuredOutlineModel::target(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return std::nullopt;
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    return node == nullptr ? std::nullopt : node->target;
}

QModelIndex SctStructuredOutlineModel::indexForTarget(
    const core::SctNavigationTarget targetValue) const {
    const auto found = targets_.find(key(targetValue).toStdString());
    return found == targets_.end() ? QModelIndex{} : indexForNode(found->second);
}

std::optional<SctStructuredOutlineModel::EditContext>
SctStructuredOutlineModel::editContext(const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return std::nullopt;
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    return node == nullptr ? std::nullopt : node->editContext;
}

void SctStructuredOutlineModel::rebuild() {
    beginResetModel();
    roots_.clear();
    targets_.clear();
    regionsByController_.clear();
    hiddenControlFlow_.clear();
    if (!snapshot_ || !snapshot_->document || !snapshot_->analysis) {
        endResetModel();
        return;
    }
    for (const auto& section : snapshot_->analysis->structuredControlFlow.sections()) {
        for (const auto& region : section.regions) {
            for (const auto& evidence : region.evidence) {
                if ((evidence.kind == SctStructureEvidenceKind::PreTargetJump
                        || evidence.kind == SctStructureEvidenceKind::BackwardTerminatorJump
                        || evidence.kind == SctStructureEvidenceKind::CommonForwardExit)
                    && evidence.source) {
                    hiddenControlFlow_.push_back(*evidence.source);
                }
            }
        }
        auto root = std::make_unique<Node>();
        root->target = core::SctNavigationTarget{
            core::SctNavigationKind::Section, section.section.value()};
        const auto* source = snapshot_->analysis->entities.find(
            *snapshot_->document, section.section);
        root->label = source == nullptr
            ? tr("Section %1").arg(section.section.value())
            : QString::fromUtf8(source->nameBytes.data(),
                static_cast<qsizetype>(source->nameBytes.size()));
        root->secondary = tr("Script");
        const auto belongsToTopLevelRegion = [&](const SctInstructionId instruction) {
            return std::ranges::any_of(section.regions, [&](const auto& region) {
                if (region.parent) return false;
                return std::ranges::any_of(region.members, [&](const auto blockId) {
                    const auto block = std::ranges::find(section.blocks, blockId,
                        &SctStructuredBasicBlock::id);
                    return block != section.blocks.end()
                        && std::ranges::find(block->instructions, instruction)
                            != block->instructions.end();
                });
            });
        };
        for (const auto& block : section.blocks) {
            Node* blockParent = root.get();
            std::unique_ptr<Node> blockNode;
            if (showBasicBlocks_) {
                blockNode = std::make_unique<Node>();
                blockNode->parent = root.get();
                blockNode->label = tr("Basic block at instruction %1")
                    .arg(block.id.entryInstruction.value());
                blockNode->secondary = tr("Derived");
                blockParent = blockNode.get();
            }
            for (const auto instruction : block.instructions) {
                const auto region = std::ranges::find_if(section.regions,
                    [&](const auto& candidate) {
                        return !candidate.parent
                            && candidate.id.headerInstruction == instruction;
                    });
                if (region != section.regions.end()) {
                    appendRegion(*blockParent, section, *region);
                } else if (!belongsToTopLevelRegion(instruction)) {
                    appendInstruction(*blockParent, instruction);
                }
            }
            if (blockNode && !blockNode->children.empty())
                root->children.push_back(std::move(blockNode));
        }
        for (const auto& candidate : section.historicalCandidates)
            appendHistoricalCandidate(*root, candidate);
        if (showRejectedEvidence_) {
            for (const auto& issue : section.issues) appendIssue(*root, issue);
        }
        roots_.push_back(std::move(root));
        indexNode(*roots_.back());
    }
    appendAuthoredArms();
    endResetModel();
}

void SctStructuredOutlineModel::appendInstruction(Node& parent,
    const SctInstructionId instruction, std::optional<EditContext> context) {
    if (!showControlFlowInstructions_
        && std::ranges::find(hiddenControlFlow_, instruction)
            != hiddenControlFlow_.end()) return;
    auto node = std::make_unique<Node>();
    node->parent = &parent;
    node->target = core::SctNavigationTarget{
        core::SctNavigationKind::Instruction, instruction.value()};
    node->editContext = std::move(context);
    const auto* value = snapshot_->analysis->entities.find(*snapshot_->document, instruction);
    const auto* schema = value == nullptr ? nullptr
        : spice::sct::findSctOpcodeSchema(value->opcode);
    node->label = value == nullptr ? tr("Unavailable instruction")
        : QStringLiteral("%1 (%2)")
            .arg(schema == nullptr ? tr("Opcode")
                : QString::fromUtf8(schema->semantic.mnemonic.data(),
                    static_cast<qsizetype>(schema->semantic.mnemonic.size())))
            .arg(value->opcode);
    node->secondary = tr("Instruction %1").arg(instruction.value());
    parent.children.push_back(std::move(node));
}

void SctStructuredOutlineModel::appendRegion(Node& parent,
    const SctSectionStructure& section, const SctStructuredRegion& region) {
    auto node = std::make_unique<Node>();
    node->parent = &parent;
    node->target = core::SctNavigationTarget{core::SctNavigationKind::Instruction,
        region.id.headerInstruction.value()};
    node->label = regionName(region.id.kind);
    node->secondary = confidenceName(region.minimumEdgeConfidence);
    node->tooltip = evidenceTooltip(region.evidence);
    node->editContext = EditContext{region.id.headerInstruction, std::nullopt,
        region.id.kind, std::nullopt, true, false, false, false};
    regionsByController_[region.id.headerInstruction.value()] = node.get();
    for (const auto& arm : region.arms) appendArm(*node, section, region, arm);
    parent.children.push_back(std::move(node));
}

void SctStructuredOutlineModel::appendArm(Node& parent,
    const SctSectionStructure& section, const SctStructuredRegion& region,
    const SctStructuredArm& arm) {
    const bool flattened = arm.kind == SctStructuredArmKind::Then
        || arm.kind == SctStructuredArmKind::LoopBody;
    std::unique_ptr<Node> armNode;
    Node* destination = &parent;
    const EditContext context{region.id.headerInstruction, std::nullopt,
        std::nullopt, arm.kind, true, arm.blocks.empty(), false, false};
    if (!flattened) {
        armNode = std::make_unique<Node>();
        armNode->parent = &parent;
        armNode->label = armName(arm);
        armNode->secondary = tr("Branch");
        armNode->editContext = context;
        destination = armNode.get();
    }
    std::vector<SctInstructionId> emitted;
    for (const auto blockId : arm.blocks) {
        const auto block = std::ranges::find(section.blocks, blockId,
            &SctStructuredBasicBlock::id);
        if (block == section.blocks.end()) continue;
        for (const auto instruction : block->instructions) {
            if (instruction == region.id.headerInstruction
                || std::ranges::find(emitted, instruction) != emitted.end()) continue;
            const auto nested = std::ranges::find_if(section.regions,
                [&](const auto& candidate) {
                    return candidate.parent == region.id
                        && candidate.id.headerInstruction == instruction;
                });
            if (nested != section.regions.end()) {
                appendRegion(*destination, section, *nested);
                for (const auto nestedBlockId : nested->members) {
                    const auto nestedBlock = std::ranges::find(section.blocks,
                        nestedBlockId, &SctStructuredBasicBlock::id);
                    if (nestedBlock != section.blocks.end())
                        emitted.insert(emitted.end(), nestedBlock->instructions.begin(),
                            nestedBlock->instructions.end());
                }
                continue;
            }
            appendInstruction(*destination, instruction, context);
            emitted.push_back(instruction);
        }
    }
    if (destination->children.empty()) {
        auto empty = std::make_unique<Node>();
        empty->parent = destination;
        empty->label = arm.kind == SctStructuredArmKind::Then
            ? tr("Empty Then") : arm.kind == SctStructuredArmKind::LoopBody
                ? tr("Empty Body") : tr("Empty arm");
        empty->secondary = tr("Semantic placeholder");
        empty->editContext = context;
        destination->children.push_back(std::move(empty));
    }
    if (armNode) parent.children.push_back(std::move(armNode));
}

void SctStructuredOutlineModel::appendHistoricalCandidate(Node& parent,
    const SctHistoricalStructureCandidate& candidate) {
    auto node = std::make_unique<Node>();
    node->parent = &parent;
    node->suggested = true;
    node->label = candidate.suggestedKind
        ? tr("Suggested %1").arg(regionName(*candidate.suggestedKind))
        : tr("Suggested control-flow region");
    node->secondary = tr("Historical · %1")
        .arg(confidenceName(candidate.evidenceConfidence));
    node->tooltip = evidenceTooltip(candidate.evidence);
    const auto target = candidate.suggestedController.value_or(candidate.sourceInstruction);
    node->target = core::SctNavigationTarget{
        core::SctNavigationKind::Instruction, target.value()};
    for (const auto instruction : candidate.involvedInstructions)
        appendInstruction(*node, instruction);
    parent.children.push_back(std::move(node));
}

void SctStructuredOutlineModel::appendIssue(Node& parent,
    const SctStructureIssue& issue) {
    auto node = std::make_unique<Node>();
    node->parent = &parent;
    node->suggested = true;
    node->label = issueName(issue.kind);
    node->secondary = tr("Not grouped");
    node->tooltip = evidenceTooltip(issue.evidence);
    if (issue.instruction) node->target = core::SctNavigationTarget{
        core::SctNavigationKind::Instruction, issue.instruction->value()};
    parent.children.push_back(std::move(node));
}

void SctStructuredOutlineModel::appendAuthoredArms() {
    if (!projection_) return;
    for (const auto& projected : projection_->authoredArms()) {
        const auto found = regionsByController_.find(
            projected.arm.controller.instruction.value());
        if (found == regionsByController_.end()) continue;
        auto* parent = found->second;
        auto node = std::make_unique<Node>();
        node->parent = parent;
        node->editContext = EditContext{projected.arm.controller.instruction,
            projected.arm.id, std::nullopt, projected.arm.kind, true,
            projected.arm.realization == core::SctAuthoredArmRealization::Virtual,
            projected.status == core::SctSemanticArmStatus::NeedsValue,
            projected.arm.realization == core::SctAuthoredArmRealization::Physical
                && projected.visibleMembers.size() == 1u};
        if (projected.arm.kind == SctStructuredArmKind::Else) {
            node->label = tr("Else");
        } else if (projected.arm.caseValue) {
            node->label = tr("Case %1").arg(*projected.arm.caseValue);
        } else {
            node->label = tr("Case (value required)");
        }
        switch (projected.status) {
        case core::SctSemanticArmStatus::Verified: node->secondary = tr("Verified"); break;
        case core::SctSemanticArmStatus::Virtual: node->secondary = tr("Virtual"); break;
        case core::SctSemanticArmStatus::NeedsValue: node->secondary = tr("Needs value"); break;
        case core::SctSemanticArmStatus::PendingVerification:
            node->secondary = tr("Pending verification"); break;
        case core::SctSemanticArmStatus::Conflicted:
            node->secondary = tr("Needs attention");
            node->suggested = true;
            break;
        }
        if (projected.visibleMembers.empty()) {
            auto placeholder = std::make_unique<Node>();
            placeholder->parent = node.get();
            placeholder->label = tr("Empty arm");
            placeholder->secondary = projected.status == core::SctSemanticArmStatus::NeedsValue
                ? tr("Set a case value before inserting")
                : tr("Insert an instruction to realize");
            placeholder->editContext = node->editContext;
            node->children.push_back(std::move(placeholder));
        } else {
            for (const auto& instruction : projected.visibleMembers) {
                auto child = std::make_unique<Node>();
                child->parent = node.get();
                child->target = core::SctNavigationTarget{
                    core::SctNavigationKind::Instruction, instruction.id.value()};
                const auto* schema = spice::sct::findSctOpcodeSchema(instruction.opcode);
                child->label = schema == nullptr
                    ? tr("Opcode %1").arg(instruction.opcode)
                    : QString::fromUtf8(schema->semantic.mnemonic.data(),
                        static_cast<qsizetype>(schema->semantic.mnemonic.size()));
                child->secondary = tr("Instruction %1").arg(instruction.id.value());
                child->editContext = node->editContext;
                node->children.push_back(std::move(child));
            }
        }
        parent->children.push_back(std::move(node));
        indexNode(*parent->children.back());
    }
}

void SctStructuredOutlineModel::indexNode(Node& node) {
    // Keep the first (authoritative outline) occurrence when developer issue
    // rows refer to the same instruction.
    if (node.target) targets_.try_emplace(key(*node.target).toStdString(), &node);
    for (auto& child : node.children) indexNode(*child);
}

QModelIndex SctStructuredOutlineModel::indexForNode(
    const Node* node, const int column) const {
    if (node == nullptr) return {};
    return createIndex(rowOf(node), column, const_cast<Node*>(node));
}

int SctStructuredOutlineModel::rowOf(const Node* node) const {
    const auto& siblings = node->parent == nullptr ? roots_ : node->parent->children;
    const auto found = std::ranges::find_if(siblings,
        [&](const auto& candidate) { return candidate.get() == node; });
    return found == siblings.end() ? -1 : static_cast<int>(found - siblings.begin());
}

QString SctStructuredOutlineModel::key(const core::SctNavigationTarget target) {
    return QStringLiteral("%1:%2").arg(static_cast<int>(target.kind)).arg(target.id);
}

}  // namespace salsa::qt
