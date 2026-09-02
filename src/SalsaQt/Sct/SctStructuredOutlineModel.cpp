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

using namespace salsa::spice_sct_prototype;

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

QString armName(const SctStructuredOutlineItem& item) {
    if (!item.arm) return QObject::tr("Body");
    switch (*item.arm) {
    case SctStructuredArmKind::Then: return QObject::tr("Then");
    case SctStructuredArmKind::Else: return QObject::tr("Else");
    case SctStructuredArmKind::LoopBody: return QObject::tr("Body");
    case SctStructuredArmKind::SwitchCase: {
        QStringList labels;
        for (const auto& label : item.caseLabels) {
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
    case SctStructureIssueKind::CrossSectionControlFlow: return QObject::tr("Cross-section control flow");
    case SctStructureIssueKind::MissingControlFlow: return QObject::tr("Missing control-flow evidence");
    case SctStructureIssueKind::InsufficientConfidence: return QObject::tr("Insufficient confidence");
    case SctStructureIssueKind::IrreducibleCycle: return QObject::tr("Irreducible cycle");
    case SctStructureIssueKind::MultipleEntryRegion: return QObject::tr("Multiple-entry region");
    case SctStructureIssueKind::AmbiguousJoin: return QObject::tr("Ambiguous join");
    case SctStructureIssueKind::OverlappingRegions: return QObject::tr("Overlapping region candidates");
    case SctStructureIssueKind::AmbiguousSwitchCases: return QObject::tr("Ambiguous switch cases");
    case SctStructureIssueKind::HistoricalEdgeConflict: return QObject::tr("Historical edge conflicts with current flow");
    case SctStructureIssueKind::RejectedLegacyCandidate: return QObject::tr("Rejected legacy-pattern candidate");
    }
    return QObject::tr("Structure analysis issue");
}

QString evidenceName(const SctStructureEvidenceKind kind) {
    switch (kind) {
    case SctStructureEvidenceKind::CanonicalControlFlow: return QObject::tr("canonical control flow");
    case SctStructureEvidenceKind::ConditionalFalseTarget: return QObject::tr("conditional false target");
    case SctStructureEvidenceKind::PreTargetJump: return QObject::tr("jump before false target");
    case SctStructureEvidenceKind::BackwardTerminatorJump: return QObject::tr("backward terminator jump");
    case SctStructureEvidenceKind::CommonForwardExit: return QObject::tr("common forward exit");
    case SctStructureEvidenceKind::PhysicalCaseBoundary: return QObject::tr("physical case boundary");
    case SctStructureEvidenceKind::SharedCaseTarget: return QObject::tr("shared case target");
    case SctStructureEvidenceKind::CaseFallthrough: return QObject::tr("case fallthrough");
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
        if (item.opaqueAttachment)
            line += QObject::tr("; opaque attachment %1").arg(item.opaqueAttachment->value());
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
    if (!snapshot_ || !snapshot_->document || !snapshot_->analysis
        || !snapshot_->structuredControlFlow) {
        endResetModel();
        return;
    }
    for (const auto& section : snapshot_->structuredControlFlow->sections()) {
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
        for (const auto& item : section.outline) appendItem(*root, item);
        roots_.push_back(std::move(root));
        indexNode(*roots_.back());
    }
    appendAuthoredArms();
    endResetModel();
}

void SctStructuredOutlineModel::appendItem(Node& parent,
    const SctStructuredOutlineItem& item,
    std::optional<spice::sct::SctInstructionId> controller,
    const bool verifiedRegion) {
    if (item.kind == SctStructuredOutlineItemKind::BasicBlock && !showBasicBlocks_) {
        for (const auto& child : item.children)
            appendItem(parent, child, controller, verifiedRegion);
        return;
    }
    if (item.kind == SctStructuredOutlineItemKind::Issue && !showRejectedEvidence_) return;
    if (item.kind == SctStructuredOutlineItemKind::Instruction && item.instruction
        && !showControlFlowInstructions_
        && std::ranges::find(hiddenControlFlow_, *item.instruction)
            != hiddenControlFlow_.end()) return;

    if (item.kind == SctStructuredOutlineItemKind::Arm && item.arm
        && (*item.arm == SctStructuredArmKind::Then
            || *item.arm == SctStructuredArmKind::LoopBody)) {
        if (item.children.empty()) {
            auto empty = std::make_unique<Node>();
            empty->parent = &parent;
            empty->label = *item.arm == SctStructuredArmKind::Then
                ? tr("Empty Then") : tr("Empty Body");
            empty->secondary = tr("Semantic placeholder");
            empty->suggested = !verifiedRegion;
            empty->editContext = EditContext{controller, std::nullopt,
                std::nullopt, item.arm, verifiedRegion, true, false, false};
            parent.children.push_back(std::move(empty));
        } else {
            for (const auto& child : item.children)
                appendItem(parent, child, controller, verifiedRegion);
        }
        return;
    }

    auto node = std::make_unique<Node>();
    node->parent = &parent;
    node->suggested = item.strength == SctStructureClaimStrength::EvidenceLimited;
    node->tooltip = evidenceTooltip(item.evidence);
    if (item.instruction) {
        node->target = core::SctNavigationTarget{
            core::SctNavigationKind::Instruction, item.instruction->value()};
    }
    switch (item.kind) {
    case SctStructuredOutlineItemKind::Instruction: {
        const auto* value = item.instruction
            ? snapshot_->analysis->entities.find(*snapshot_->document, *item.instruction)
            : nullptr;
        const auto* schema = value == nullptr ? nullptr
            : spice::sct::findSctOpcodeSchema(value->opcode);
        node->label = value == nullptr ? tr("Unavailable instruction")
            : QStringLiteral("%1 (%2)")
                .arg(schema == nullptr ? tr("Opcode")
                    : QString::fromUtf8(schema->semantic.mnemonic.data(),
                        static_cast<qsizetype>(schema->semantic.mnemonic.size())))
                .arg(value->opcode);
        node->secondary = item.instruction
            ? tr("Instruction %1").arg(item.instruction->value()) : QString{};
        break;
    }
    case SctStructuredOutlineItemKind::Region:
        node->label = item.region ? regionName(item.region->kind) : tr("Region");
        node->secondary = node->suggested
            ? tr("Suggested · %1").arg(confidenceName(item.confidence))
            : confidenceName(item.confidence);
        controller = item.instruction;
        node->editContext = EditContext{controller, std::nullopt,
            item.region ? std::optional{item.region->kind} : std::nullopt,
            std::nullopt, !node->suggested, false, false, false};
        if (controller) regionsByController_[controller->value()] = node.get();
        break;
    case SctStructuredOutlineItemKind::Arm:
        node->label = armName(item);
        node->secondary = tr("Branch");
        node->editContext = EditContext{controller, std::nullopt, std::nullopt,
            item.arm, verifiedRegion, item.children.empty(), false, false};
        break;
    case SctStructuredOutlineItemKind::BasicBlock:
        node->label = item.block
            ? tr("Basic block at instruction %1").arg(item.block->entryInstruction.value())
            : tr("Basic block");
        node->secondary = tr("Derived");
        break;
    case SctStructuredOutlineItemKind::Issue:
        node->label = item.issue ? issueName(*item.issue) : tr("Structure analysis issue");
        node->secondary = tr("Not grouped");
        node->suggested = true;
        break;
    }
    const bool childVerified = item.kind == SctStructuredOutlineItemKind::Region
        ? !node->suggested : verifiedRegion;
    for (const auto& child : item.children)
        appendItem(*node, child, controller, childVerified);
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
