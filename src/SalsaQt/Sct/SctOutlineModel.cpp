#include "Sct/SctOutlineModel.h"

#include "SpiceSCT/SctOpcodeMetadata.h"

#include <QString>

#include <algorithm>
#include <ranges>

namespace salsa::qt {

SctOutlineModel::SctOutlineModel(QObject* parent) : QAbstractItemModel(parent) {}

QModelIndex SctOutlineModel::index(
    const int row, const int column, const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= 2) return {};
    const auto* parentNode = parentIndex.isValid()
        ? static_cast<Node*>(parentIndex.internalPointer()) : nullptr;
    const auto& children = parentNode == nullptr ? roots_ : parentNode->children;
    if (row >= static_cast<int>(children.size())) return {};
    return createIndex(row, column, children[static_cast<std::size_t>(row)].get());
}

QModelIndex SctOutlineModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    const auto* node = static_cast<Node*>(child.internalPointer());
    return node == nullptr || node->parent == nullptr
        ? QModelIndex{} : indexForNode(node->parent);
}

int SctOutlineModel::rowCount(const QModelIndex& parentIndex) const {
    if (parentIndex.isValid() && parentIndex.column() != 0) return 0;
    const auto* node = parentIndex.isValid()
        ? static_cast<Node*>(parentIndex.internalPointer()) : nullptr;
    return static_cast<int>(node == nullptr ? roots_.size() : node->children.size());
}

int SctOutlineModel::columnCount(const QModelIndex&) const { return 2; }

QVariant SctOutlineModel::data(const QModelIndex& modelIndex, const int role) const {
    if (!modelIndex.isValid()) return {};
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    if (role == Qt::DisplayRole)
        return modelIndex.column() == 0 ? node->label : node->secondary;
    if (role == Qt::UserRole) return static_cast<int>(node->target.kind);
    if (role == Qt::UserRole + 1)
        return QVariant::fromValue<qulonglong>(node->target.id);
    return {};
}

QVariant SctOutlineModel::headerData(
    const int section, const Qt::Orientation orientation, const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? tr("Physical outline") : tr("Kind / ID");
}

void SctOutlineModel::resetFrom(
    const std::vector<core::SctOutlineItem>& outline,
    const spice::sct::SctDocument& document) {
    beginResetModel();
    roots_.clear();
    nodes_.clear();
    instructionValues_.clear();
    roots_.reserve(outline.size());
    for (const auto& item : outline) {
        roots_.push_back(makeNode(item, nullptr));
        indexNode(*roots_.back());
    }
    for (const auto& section : document.sections) {
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content)) {
            for (const auto& instruction : script->instructions)
                instructionValues_.emplace(instruction.id.value(), instruction);
        }
    }
    endResetModel();
}

bool SctOutlineModel::apply(const core::SctEditChangeSet& changes) {
    for (const auto& change : changes.sections)
        if (!applyOne(change)) return false;
    std::vector<core::SctInstructionStructuralChange> applied;
    applied.reserve(changes.instructions.size());
    for (const auto& change : changes.instructions) {
        if (applyOne(change)) {
            applied.push_back(change);
            continue;
        }
        for (auto existing = applied.rbegin(); existing != applied.rend(); ++existing) {
            auto reverse = *existing;
            std::swap(reverse.before, reverse.after);
            std::swap(reverse.beforeValue, reverse.afterValue);
            std::swap(reverse.beforeSemantics, reverse.afterSemantics);
            const auto restored = applyOne(reverse);
            Q_ASSERT(restored);
        }
        return false;
    }
    for (const auto& change : changes.footerEntries)
        if (!applyOne(change)) return false;
    return true;
}

bool SctOutlineModel::applyOne(const core::SctSectionStructuralChange& change) {
    const core::SctNavigationTarget target{core::SctNavigationKind::Section,
        change.section.value()};
    if (!change.before && change.after && change.afterValue) {
        if (nodeFor(target) != nullptr) return false;
        const auto row = sectionInsertionRow(*change.after);
        if (row < 0) return false;
        beginInsertRows({}, row, row);
        auto node = sectionNode(*change.afterValue);
        auto* raw = node.get();
        roots_.insert(roots_.begin() + row, std::move(node));
        indexNode(*raw);
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &change.afterValue->content)) {
            for (const auto& instruction : script->instructions)
                instructionValues_[instruction.id.value()] = instruction;
        }
        endInsertRows();
        refreshSectionLabels();
        return true;
    }
    if (change.before && !change.after && change.beforeValue) {
        auto* node = nodeFor(target);
        if (node == nullptr || node->parent != nullptr) return false;
        const auto row = rowOf(node);
        if (row < 0) return false;
        std::vector<core::SctNavigationTarget> descendants;
        const auto collect = [&](const auto& self, const Node& current) -> void {
            descendants.push_back(current.target);
            for (const auto& child : current.children) self(self, *child);
        };
        collect(collect, *node);
        beginRemoveRows({}, row, row);
        for (const auto descendant : descendants) {
            nodes_.erase(key(descendant).toStdString());
            if (descendant.kind == core::SctNavigationKind::Instruction)
                instructionValues_.erase(descendant.id);
        }
        roots_.erase(roots_.begin() + row);
        endRemoveRows();
        refreshSectionLabels();
        return true;
    }
    if (change.before && change.after) {
        auto* node = nodeFor(target);
        if (node == nullptr || node->parent != nullptr) return false;
        const auto sourceRow = rowOf(node);
        const auto finalRow = sectionInsertionRow(*change.after);
        if (sourceRow < 0 || finalRow < 0) return false;
        if (sourceRow == finalRow || sourceRow + 1 == finalRow) return true;
        if (!beginMoveRows({}, sourceRow, sourceRow, {}, finalRow)) return false;
        auto moved = std::move(roots_[static_cast<std::size_t>(sourceRow)]);
        roots_.erase(roots_.begin() + sourceRow);
        auto adjusted = finalRow;
        if (sourceRow < finalRow) --adjusted;
        roots_.insert(roots_.begin() + adjusted, std::move(moved));
        endMoveRows();
        refreshSectionLabels();
        return true;
    }
    if (change.beforeName && change.afterName) {
        auto* node = nodeFor(target);
        if (node == nullptr) return false;
        const auto row = rowOf(node);
        const auto prefix = node->label.left(node->label.indexOf(QStringLiteral("] ")) + 2);
        node->label = prefix + QString::fromStdString(*change.afterName);
        emit dataChanged(indexForNode(node), indexForNode(node, 1));
        return true;
    }
    return false;
}

bool SctOutlineModel::applyOne(const core::SctFooterEntryStructuralChange& change) {
    auto* footer = nodeFor({core::SctNavigationKind::FooterGroup, 0});
    if (footer == nullptr) return false;
    const core::SctNavigationTarget target{core::SctNavigationKind::FooterEntry,
        change.entry.value()};
    if (!change.before && change.after && change.afterValue) {
        int row = 0;
        if (change.after->after) {
            auto* anchor = nodeFor({core::SctNavigationKind::FooterEntry,
                change.after->after->value()});
            if (anchor == nullptr || anchor->parent != footer) return false;
            row = rowOf(anchor) + 1;
        }
        beginInsertRows(indexForNode(footer), row, row);
        auto node = footerEntryNode(*change.afterValue, footer);
        auto* raw = node.get();
        footer->children.insert(footer->children.begin() + row, std::move(node));
        nodes_[key(target).toStdString()] = raw;
        endInsertRows();
        refreshFooterLabels();
        return true;
    }
    if (change.before && !change.after) {
        auto* node = nodeFor(target);
        if (node == nullptr || node->parent != footer) return false;
        const auto row = rowOf(node);
        beginRemoveRows(indexForNode(footer), row, row);
        nodes_.erase(key(target).toStdString());
        footer->children.erase(footer->children.begin() + row);
        endRemoveRows();
        refreshFooterLabels();
        return true;
    }
    return false;
}

bool SctOutlineModel::applyOne(
    const core::SctInstructionStructuralChange& change) {
    if (!change.before.has_value() && change.after.has_value()
        && change.afterValue.has_value()) {
            auto* section = nodeFor({core::SctNavigationKind::Section,
                change.after->section.value()});
            if (section == nullptr || nodeFor({core::SctNavigationKind::Instruction,
                    change.instruction.value()}) != nullptr) return false;
            const auto row = insertionRow(*section, *change.after);
            if (row < 0) return false;
            const auto parentIndex = indexForNode(section);
            beginInsertRows(parentIndex, row, row);
            auto node = instructionNode(*change.afterValue, section);
            auto* raw = node.get();
            section->children.insert(section->children.begin() + row, std::move(node));
            nodes_[key(raw->target).toStdString()] = raw;
            instructionValues_[change.instruction.value()] = *change.afterValue;
            endInsertRows();
    } else if (change.before.has_value() && !change.after.has_value()) {
            auto* node = nodeFor({core::SctNavigationKind::Instruction,
                change.instruction.value()});
            if (node == nullptr || node->parent == nullptr) return false;
            auto* section = node->parent;
            const auto row = rowOf(node);
            if (row < 0) return false;
            beginRemoveRows(indexForNode(section), row, row);
            nodes_.erase(key(node->target).toStdString());
            instructionValues_.erase(change.instruction.value());
            section->children.erase(section->children.begin() + row);
            endRemoveRows();
    } else if (change.before.has_value() && change.after.has_value()) {
            auto* node = nodeFor({core::SctNavigationKind::Instruction,
                change.instruction.value()});
            auto* destination = nodeFor({core::SctNavigationKind::Section,
                change.after->section.value()});
            if (node == nullptr || node->parent == nullptr || destination == nullptr)
                return false;
            auto* source = node->parent;
            const auto sourceRow = rowOf(node);
            const auto finalRow = insertionRow(*destination, *change.after);
            if (sourceRow < 0 || finalRow < 0) return false;
            if (source == destination && sourceRow == finalRow) return true;
            const auto destinationChild = finalRow;
            if (!beginMoveRows(indexForNode(source), sourceRow, sourceRow,
                    indexForNode(destination), destinationChild)) return false;
            auto moved = std::move(source->children[static_cast<std::size_t>(sourceRow)]);
            source->children.erase(source->children.begin() + sourceRow);
            auto adjusted = finalRow;
            if (source == destination && sourceRow < finalRow) --adjusted;
            moved->parent = destination;
            destination->children.insert(destination->children.begin() + adjusted,
                std::move(moved));
            endMoveRows();
    } else {
        return false;
    }
    return true;
}

std::optional<core::SctNavigationTarget> SctOutlineModel::target(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return std::nullopt;
    return static_cast<Node*>(modelIndex.internalPointer())->target;
}

QModelIndex SctOutlineModel::indexForTarget(
    const core::SctNavigationTarget target) const {
    return indexForNode(nodeFor(target));
}

const spice::sct::SctDocumentInstruction* SctOutlineModel::instruction(
    const spice::sct::SctInstructionId id) const noexcept {
    const auto found = instructionValues_.find(id.value());
    return found == instructionValues_.end() ? nullptr : &found->second;
}

std::optional<spice::sct::SctInstructionId> SctOutlineModel::previousInstruction(
    const spice::sct::SctInstructionId id) const {
    const auto* node = nodeFor({core::SctNavigationKind::Instruction, id.value()});
    if (node == nullptr || node->parent == nullptr) return std::nullopt;
    const auto row = rowOf(node);
    if (row <= 0) return std::nullopt;
    const auto target = node->parent->children[static_cast<std::size_t>(row - 1)]->target;
    return target.kind == core::SctNavigationKind::Instruction
        ? std::optional{spice::sct::SctInstructionId(target.id)} : std::nullopt;
}

std::optional<spice::sct::SctInstructionId> SctOutlineModel::nextInstruction(
    const spice::sct::SctInstructionId id) const {
    const auto* node = nodeFor({core::SctNavigationKind::Instruction, id.value()});
    if (node == nullptr || node->parent == nullptr) return std::nullopt;
    const auto row = rowOf(node);
    if (row < 0 || row + 1 >= static_cast<int>(node->parent->children.size()))
        return std::nullopt;
    const auto target = node->parent->children[static_cast<std::size_t>(row + 1)]->target;
    return target.kind == core::SctNavigationKind::Instruction
        ? std::optional{spice::sct::SctInstructionId(target.id)} : std::nullopt;
}

std::unique_ptr<SctOutlineModel::Node> SctOutlineModel::makeNode(
    const core::SctOutlineItem& item, Node* parent) {
    auto node = std::make_unique<Node>();
    node->label = QString::fromStdString(item.label);
    node->secondary = QString::fromStdString(item.secondary);
    node->target = item.target;
    node->parent = parent;
    for (const auto& child : item.children)
        node->children.push_back(makeNode(child, node.get()));
    return node;
}

void SctOutlineModel::indexNode(Node& node) {
    nodes_[key(node.target).toStdString()] = &node;
    for (auto& child : node.children) indexNode(*child);
}

QModelIndex SctOutlineModel::indexForNode(const Node* node, const int column) const {
    if (node == nullptr) return {};
    const auto row = rowOf(node);
    return row < 0 ? QModelIndex{} : createIndex(row, column, const_cast<Node*>(node));
}

int SctOutlineModel::rowOf(const Node* node) const {
    if (node == nullptr) return -1;
    const auto& siblings = node->parent == nullptr ? roots_ : node->parent->children;
    const auto found = std::ranges::find_if(siblings,
        [node](const auto& candidate) { return candidate.get() == node; });
    return found == siblings.end() ? -1
        : static_cast<int>(std::distance(siblings.begin(), found));
}

SctOutlineModel::Node* SctOutlineModel::nodeFor(
    const core::SctNavigationTarget target) const {
    const auto found = nodes_.find(key(target).toStdString());
    return found == nodes_.end() ? nullptr : found->second;
}

QString SctOutlineModel::key(const core::SctNavigationTarget target) {
    return QString::number(static_cast<int>(target.kind)) + QLatin1Char(':')
        + QString::number(target.id);
}

std::unique_ptr<SctOutlineModel::Node> SctOutlineModel::instructionNode(
    const spice::sct::SctDocumentInstruction& instruction, Node* parent) {
    auto node = std::make_unique<Node>();
    const auto* schema = spice::sct::findSctOpcodeSchema(instruction.opcode);
    const auto mnemonic = schema != nullptr && !schema->semantic.mnemonic.empty()
        ? QString::fromUtf8(schema->semantic.mnemonic.data(),
            static_cast<qsizetype>(schema->semantic.mnemonic.size()))
        : QStringLiteral("Opcode");
    node->label = mnemonic + QStringLiteral(" (")
        + QString::number(instruction.opcode) + QLatin1Char(')');
    node->secondary = tr("Instruction %1").arg(instruction.id.value());
    node->target = {core::SctNavigationKind::Instruction, instruction.id.value()};
    node->parent = parent;
    return node;
}

int SctOutlineModel::insertionRow(
    Node& section, const core::SctInstructionPlacement& placement) const {
    if (!placement.after.has_value()) return 0;
    const auto* anchor = nodeFor({core::SctNavigationKind::Instruction,
        placement.after->value()});
    if (anchor == nullptr || anchor->parent != &section) return -1;
    return rowOf(anchor) + 1;
}

int SctOutlineModel::sectionInsertionRow(
    const core::SctSectionPlacement& placement) const {
    if (!placement.after) return roots_.empty() ? 0 : 1;
    const auto* anchor = nodeFor({core::SctNavigationKind::Section,
        placement.after->value()});
    return anchor == nullptr || anchor->parent != nullptr ? -1 : rowOf(anchor) + 1;
}

std::unique_ptr<SctOutlineModel::Node> SctOutlineModel::sectionNode(
    const spice::sct::SctDocumentSection& section, Node* parent) {
    auto node = std::make_unique<Node>();
    node->label = QStringLiteral("[0] ") + QString::fromStdString(section.nameBytes);
    node->target = {core::SctNavigationKind::Section, section.id.value()};
    node->parent = parent;
    if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section.content)) {
        node->secondary = tr("Script");
        for (const auto& instruction : script->instructions)
            node->children.push_back(instructionNode(instruction, node.get()));
    } else if (const auto* strings = std::get_if<spice::sct::SctStringSectionContent>(&section.content)) {
        node->secondary = tr("Indexed string");
        auto child = std::make_unique<Node>();
        child->label = tr("Indexed string");
        child->secondary = strings->string.kind == spice::sct::SctTextKind::SctString
            ? tr("SCT message") : tr("Plain string");
        child->target = {core::SctNavigationKind::String, strings->string.id.value()};
        child->parent = node.get();
        node->children.push_back(std::move(child));
    } else if (std::holds_alternative<spice::sct::SctStringGroupMarkerSectionContent>(section.content)) {
        node->secondary = tr("String group marker");
    } else {
        node->secondary = tr("Opaque");
    }
    return node;
}

std::unique_ptr<SctOutlineModel::Node> SctOutlineModel::footerEntryNode(
    const spice::sct::SctDocumentFooterEntry& entry, Node* parent) {
    auto node = std::make_unique<Node>();
    node->label = QStringLiteral("[0] ") + (entry.kind == spice::sct::SctTextKind::SctString
        ? tr("SCT message") : tr("Plain string"));
    node->target = {core::SctNavigationKind::FooterEntry, entry.id.value()};
    node->parent = parent;
    return node;
}

void SctOutlineModel::refreshSectionLabels() {
    int ordinal = 0;
    for (auto& node : roots_) {
        if (node->target.kind != core::SctNavigationKind::Section) continue;
        const auto suffix = node->label.mid(node->label.indexOf(QStringLiteral("] ")) + 2);
        node->label = QStringLiteral("[%1] %2").arg(ordinal++).arg(suffix);
        emit dataChanged(indexForNode(node.get()), indexForNode(node.get(), 1));
    }
}

void SctOutlineModel::refreshFooterLabels() {
    auto* footer = nodeFor({core::SctNavigationKind::FooterGroup, 0});
    if (footer == nullptr) return;
    footer->secondary = tr("%1 entries").arg(footer->children.size());
    emit dataChanged(indexForNode(footer), indexForNode(footer, 1));
    for (std::size_t i = 0; i < footer->children.size(); ++i) {
        auto* node = footer->children[i].get();
        const auto suffix = node->label.mid(node->label.indexOf(QStringLiteral("] ")) + 2);
        node->label = QStringLiteral("[%1] %2").arg(i).arg(suffix);
        emit dataChanged(indexForNode(node), indexForNode(node, 1));
    }
}

}  // namespace salsa::qt
