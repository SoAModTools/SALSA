#include "Sct/SctStructuredOutlineModel.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include "SpiceSCT/SctOpcodeMetadata.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QMimeData>
#include <QStyle>
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
    if (role == Qt::DecorationRole && modelIndex.column() == 0
        && node->importedEvidenceCount != 0u)
        return QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
    if (role == Qt::AccessibleDescriptionRole
        && node->importedEvidenceCount != 0u) {
        return tr("%1 imported control-flow evidence item(s). Select to inspect.")
            .arg(node->importedEvidenceCount);
    }
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

Qt::ItemFlags SctStructuredOutlineModel::flags(const QModelIndex& modelIndex) const {
    auto result = QAbstractItemModel::flags(modelIndex);
    if (!modelIndex.isValid()) return result;
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    if (!node->authorable) return result;
    if (node->kind == core::SctSemanticNodeKind::Instruction
        || node->kind == core::SctSemanticNodeKind::Region)
        result |= Qt::ItemIsDragEnabled;
    if (node->kind == core::SctSemanticNodeKind::Region
        || node->kind == core::SctSemanticNodeKind::Arm)
        result |= Qt::ItemIsDropEnabled;
    return result;
}

QStringList SctStructuredOutlineModel::mimeTypes() const {
    return {QStringLiteral("application/vnd.jahorta.salsa.semantic-units")};
}

QMimeData* SctStructuredOutlineModel::mimeData(const QModelIndexList& indexes) const {
    auto* data = new QMimeData;
    std::vector<const Node*> nodes;
    for (const auto& index : indexes) {
        if (!index.isValid() || index.column() != 0) continue;
        const auto* node = static_cast<Node*>(index.internalPointer());
        if (!node->authorable || !node->key.valid()) continue;
        nodes.push_back(node);
    }
    std::ranges::sort(nodes, {}, [this](const Node* node) { return rowOf(node); });
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    if (nodes.empty()) return data;
    const auto* parent = nodes.front()->parent;
    for (std::size_t ordinal = 0; ordinal < nodes.size(); ++ordinal) {
        if (nodes[ordinal]->parent != parent
            || rowOf(nodes[ordinal]) != rowOf(nodes.front()) + static_cast<int>(ordinal))
            return data;
    }
    QByteArray encoded;
    for (const auto* node : nodes) {
        if (!encoded.isEmpty()) encoded.push_back('\n');
        encoded += QByteArray::fromStdString(node->key.value);
    }
    data->setData(mimeTypes().front(), encoded);
    return data;
}

bool SctStructuredOutlineModel::dropMimeData(const QMimeData* data,
    const Qt::DropAction action, int row, const int column,
    const QModelIndex& parentIndex) {
    if (action == Qt::IgnoreAction) return true;
    if (action != Qt::MoveAction || column > 0 || data == nullptr
        || !data->hasFormat(mimeTypes().front())) return false;
    QStringList keys;
    for (const auto& token : data->data(mimeTypes().front()).split('\n')) {
        if (!token.isEmpty()) keys.push_back(QString::fromUtf8(token));
    }
    if (keys.empty()) return false;
    const Node* destination = parentIndex.isValid()
        ? static_cast<Node*>(parentIndex.internalPointer()) : nullptr;
    auto placement = core::SctSemanticDestinationPlacement::IntoEnd;
    if (row >= 0) {
        const auto& siblings = destination == nullptr ? roots_ : destination->children;
        if (siblings.empty()) return false;
        if (row < static_cast<int>(siblings.size())) {
            destination = siblings[static_cast<std::size_t>(row)].get();
            placement = core::SctSemanticDestinationPlacement::Before;
        } else {
            destination = siblings.back().get();
            placement = core::SctSemanticDestinationPlacement::After;
        }
    }
    if (destination == nullptr || !destination->key.valid()) return false;
    emit semanticUnitsDropRequested(keys,
        QString::fromStdString(destination->key.value), static_cast<int>(placement));
    return true;
}

Qt::DropActions SctStructuredOutlineModel::supportedDropActions() const {
    return Qt::MoveAction;
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

std::optional<core::SctSemanticNodeKey> SctStructuredOutlineModel::nodeKey(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return std::nullopt;
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    return node != nullptr && node->key.valid()
        ? std::optional{node->key} : std::nullopt;
}

core::SctSemanticNodeKind SctStructuredOutlineModel::nodeKind(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return core::SctSemanticNodeKind::Issue;
    return static_cast<Node*>(modelIndex.internalPointer())->kind;
}

std::span<const SctInstructionId> SctStructuredOutlineModel::physicalInstructions(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return {};
    return static_cast<Node*>(modelIndex.internalPointer())->physicalInstructions;
}

void SctStructuredOutlineModel::rebuild() {
    beginResetModel();
    roots_.clear();
    targets_.clear();
    regionsByController_.clear();
    importedEvidenceByInstruction_.clear();
    hiddenControlFlow_.clear();
    if (!snapshot_ || !snapshot_->document || !snapshot_->analysis || !projection_) {
        endResetModel();
        return;
    }
    for (const auto& source : projection_->roots()) appendProjectionNode(nullptr, source);
    endResetModel();
}

void SctStructuredOutlineModel::appendProjectionNode(
    Node* parent, const core::SctSemanticProjectionNode& source) {
    if (source.kind == core::SctSemanticNodeKind::Issue && !showRejectedEvidence_) return;
    if (source.hiddenByDefault && !showControlFlowInstructions_) return;
    auto node = std::make_unique<Node>();
    node->parent = parent;
    node->key = source.key;
    node->kind = source.kind;
    node->target = source.target;
    node->importedEvidenceCount = source.importedEvidenceCount;
    node->physicalInstructions = source.physicalInstructions;
    node->authorable = source.authorable;
    switch (source.kind) {
    case core::SctSemanticNodeKind::Section: {
        const auto id = spice::sct::SctSectionId(source.target ? source.target->id : 0u);
        const auto* value = snapshot_->analysis->entities.find(*snapshot_->document, id);
        node->label = value == nullptr ? tr("Section %1").arg(id.value())
            : QString::fromUtf8(value->nameBytes.data(),
                static_cast<qsizetype>(value->nameBytes.size()));
        node->secondary = tr("Script");
        break;
    }
    case core::SctSemanticNodeKind::Instruction: {
        const auto id = spice::sct::SctInstructionId(source.target ? source.target->id : 0u);
        const auto* value = snapshot_->analysis->entities.find(*snapshot_->document, id);
        const auto resolved = value == nullptr ? core::SctResolvedCatalogEntry{}
            : core::SctCatalogResolver::resolve(value->opcode);
        node->label = value == nullptr ? tr("Unavailable instruction")
            : QStringLiteral("%1 (%2)").arg(resolved.mnemonic.empty() ? tr("Opcode")
                : QString::fromStdString(resolved.mnemonic)).arg(value->opcode);
        node->secondary = tr("Instruction %1").arg(id.value());
        break;
    }
    case core::SctSemanticNodeKind::Region:
        node->label = source.regionKind ? regionName(*source.regionKind) : tr("Region");
        node->secondary = source.confidence ? confidenceName(*source.confidence) : tr("Derived");
        node->tooltip = evidenceTooltip(source.evidence);
        break;
    case core::SctSemanticNodeKind::Arm: {
        if (source.armKind == SctStructuredArmKind::Then) node->label = tr("Then");
        else if (source.armKind == SctStructuredArmKind::LoopBody) node->label = tr("Body");
        else if (source.armKind == SctStructuredArmKind::Else) node->label = tr("Else");
        else if (source.caseValue) node->label = tr("Case %1").arg(*source.caseValue);
        else node->label = tr("Case (value required)");
        switch (source.armStatus) {
        case core::SctSemanticArmStatus::Verified: node->secondary = tr("Verified"); break;
        case core::SctSemanticArmStatus::Virtual: node->secondary = tr("Virtual"); break;
        case core::SctSemanticArmStatus::NeedsValue: node->secondary = tr("Needs value"); break;
        case core::SctSemanticArmStatus::PendingVerification:
            node->secondary = tr("Pending verification"); break;
        case core::SctSemanticArmStatus::Conflicted:
            node->secondary = tr("Needs attention"); node->suggested = true; break;
        }
        break;
    }
    case core::SctSemanticNodeKind::Placeholder:
        node->label = tr("[Empty]");
        node->secondary = source.needsValue
            ? tr("Set a case value before inserting")
            : tr("Insert an instruction to realize");
        break;
    case core::SctSemanticNodeKind::Issue:
        node->label = source.issue ? issueName(source.issue->kind) : tr("Structure issue");
        node->secondary = tr("Not grouped");
        node->tooltip = evidenceTooltip(source.evidence);
        node->suggested = true;
        break;
    case core::SctSemanticNodeKind::BasicBlock:
        node->label = source.basicBlock
            ? tr("Basic block at instruction %1").arg(source.basicBlock->entryInstruction.value())
            : tr("Basic block");
        node->secondary = tr("Derived");
        break;
    }
    if (source.controller || source.authoredArm || source.regionKind || source.armKind) {
        node->editContext = EditContext{source.controller, source.authoredArm,
            source.regionKind, source.armKind, source.verified, source.virtualArm,
            source.needsValue, source.canReturnToEmpty};
    }
    if (node->importedEvidenceCount != 0u) {
        node->secondary += tr(" | Imported evidence: %1")
            .arg(node->importedEvidenceCount);
    }
    auto* raw = node.get();
    if (parent == nullptr) roots_.push_back(std::move(node));
    else parent->children.push_back(std::move(node));
    for (const auto& child : source.children) appendProjectionNode(raw, child);
    indexNode(*raw);
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
    const auto resolved = value == nullptr ? core::SctResolvedCatalogEntry{}
        : core::SctCatalogResolver::resolve(value->opcode);
    node->label = value == nullptr ? tr("Unavailable instruction")
        : QStringLiteral("%1 (%2)")
            .arg(resolved.mnemonic.empty() ? tr("Opcode")
                : QString::fromStdString(resolved.mnemonic))
            .arg(value->opcode);
    node->secondary = tr("Instruction %1").arg(instruction.value());
    markImportedEvidence(*node, instruction);
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
    markImportedEvidence(*node, region.id.headerInstruction);
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
        empty->label = tr("[Empty]");
        empty->secondary = tr("Semantic placeholder");
        empty->editContext = context;
        destination->children.push_back(std::move(empty));
    }
    if (armNode) parent.children.push_back(std::move(armNode));
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
            placeholder->label = tr("[Empty]");
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
                const auto resolved = core::SctCatalogResolver::resolve(instruction.opcode);
                child->label = resolved.mnemonic.empty()
                    ? tr("Opcode %1").arg(instruction.opcode)
                    : QString::fromStdString(resolved.mnemonic);
                child->secondary = tr("Instruction %1").arg(instruction.id.value());
                markImportedEvidence(*child, instruction.id);
                child->editContext = node->editContext;
                node->children.push_back(std::move(child));
            }
        }
        parent->children.push_back(std::move(node));
        indexNode(*parent->children.back());
    }
}

void SctStructuredOutlineModel::markImportedEvidence(
    Node& node, const SctInstructionId instruction) const {
    const auto found = importedEvidenceByInstruction_.find(instruction.value());
    if (found == importedEvidenceByInstruction_.end() || found->second == 0u) return;
    node.importedEvidenceCount = found->second;
    node.secondary += tr(" | Imported evidence: %1").arg(found->second);
    const auto evidence = tr("%1 imported control-flow evidence item(s). "
        "Select the instruction to inspect.").arg(found->second);
    if (!node.tooltip.isEmpty()) node.tooltip += QLatin1Char('\n');
    node.tooltip += evidence;
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
