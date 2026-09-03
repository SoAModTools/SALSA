#include "Sct/SctParameterTableModel.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPersistentModelIndex>
#include <QToolTip>

#include <algorithm>
#include <ranges>
#include <utility>

namespace salsa::qt {

SctParameterTableModel::SctParameterTableModel(QObject* parent)
    : QAbstractItemModel(parent) {}

QModelIndex SctParameterTableModel::index(const int row, const int column,
    const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= 3) return {};
    auto* node = nodeAt(row, parentIndex);
    return node == nullptr ? QModelIndex{} : createIndex(row, column, node);
}

QModelIndex SctParameterTableModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    const auto* node = static_cast<Node*>(child.internalPointer());
    return node == nullptr || node->parent == nullptr
        ? QModelIndex{} : indexForNode(node->parent);
}

int SctParameterTableModel::rowCount(const QModelIndex& parentIndex) const {
    if (parentIndex.isValid() && parentIndex.column() != 0) return 0;
    if (!parentIndex.isValid())
        return static_cast<int>(fixed_.size() + groups_.size());
    const auto* parent = static_cast<Node*>(parentIndex.internalPointer());
    return parent != nullptr && parent->kind == NodeKind::Group
        ? static_cast<int>(parent->children.size()) : 0;
}

int SctParameterTableModel::columnCount(const QModelIndex&) const { return 3; }

QVariant SctParameterTableModel::data(const QModelIndex& modelIndex,
    const int role) const {
    if (!modelIndex.isValid()) return {};
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    if (node == nullptr) return {};
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        if (node->kind == NodeKind::Group) {
            if (modelIndex.column() == 0)
                return tr("Group %1").arg(node->groupOrdinal + 1u);
            if (modelIndex.column() == 1)
                return tr("%1 parameters").arg(node->children.size());
            return {};
        }
        if (modelIndex.column() == 0)
            return QString::fromStdString(node->parameter.parameter);
        if (modelIndex.column() == 1)
            return QString::fromStdString(node->parameter.value);
        return QString::fromStdString(node->parameter.notes);
    }
    if (role == Qt::ToolTipRole && node->kind == NodeKind::Parameter) {
        auto tooltip = QString::fromStdString(node->parameter.value);
        if (!node->parameter.notes.empty())
            tooltip += QStringLiteral("\n\n")
                + QString::fromStdString(node->parameter.notes);
        return tooltip;
    }
    return {};
}

QVariant SctParameterTableModel::headerData(const int section,
    const Qt::Orientation orientation, const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    if (section == 0) return tr("Parameter");
    if (section == 1) return tr("Value");
    return tr("Notes");
}

Qt::ItemFlags SctParameterTableModel::flags(const QModelIndex& modelIndex) const {
    auto result = QAbstractItemModel::flags(modelIndex);
    const auto* parameterValue = parameter(modelIndex);
    if (parameterValue != nullptr && modelIndex.column() == 1) {
        using Kind = core::SctInlineParameterEditorKind;
        switch (parameterValue->editor) {
        case Kind::EncodedUnsigned: case Kind::EncodedSigned: case Kind::EncodedHex:
        case Kind::ConventionalScpt:
        case Kind::TerminatedWords:
        case Kind::PlainFooterText:
            result |= Qt::ItemIsEditable;
            break;
        default: break;
        }
    }
    return result;
}

void SctParameterTableModel::setCommitHandler(CommitHandler handler) {
    commitHandler_ = std::move(handler);
}

void SctParameterTableModel::resetFrom(
    core::SctParameterTablePresentation presentation) {
    beginResetModel();
    presentation_ = std::move(presentation);
    fixed_.clear();
    groups_.clear();
    for (auto parameter : presentation_.fixedParameters)
        fixed_.push_back(makeParameter(std::move(parameter)));
    for (auto group : presentation_.repeatedGroups)
        groups_.push_back(makeGroup(std::move(group)));
    endResetModel();
}

bool SctParameterTableModel::apply(
    core::SctParameterTablePresentation presentation,
    const core::SctEditChangeSet& changes) {
    if (presentation.instruction != presentation_.instruction
        || presentation.fixedParameters.size() != fixed_.size()) return false;
    if (!changes.repeatedGroups.empty()) {
        if (changes.repeatedGroups.size() != 1u) return false;
        const auto& change = changes.repeatedGroups.front();
        if (!change.beforeOrdinal && change.afterOrdinal) {
            const auto row = static_cast<int>(*change.afterOrdinal);
            if (row < 0 || row > static_cast<int>(groups_.size())
                || *change.afterOrdinal >= presentation.repeatedGroups.size()) return false;
            beginInsertRows({}, static_cast<int>(fixed_.size()) + row,
                static_cast<int>(fixed_.size()) + row);
            groups_.insert(groups_.begin() + row,
                makeGroup(presentation.repeatedGroups[*change.afterOrdinal]));
            endInsertRows();
        } else if (change.beforeOrdinal && !change.afterOrdinal) {
            const auto row = static_cast<int>(*change.beforeOrdinal);
            if (row < 0 || row >= static_cast<int>(groups_.size())) return false;
            beginRemoveRows({}, static_cast<int>(fixed_.size()) + row,
                static_cast<int>(fixed_.size()) + row);
            groups_.erase(groups_.begin() + row);
            endRemoveRows();
        } else if (change.beforeOrdinal && change.afterOrdinal) {
            const auto source = static_cast<int>(*change.beforeOrdinal);
            const auto target = static_cast<int>(*change.afterOrdinal);
            if (source < 0 || target < 0 || source >= static_cast<int>(groups_.size())
                || target >= static_cast<int>(groups_.size())) return false;
            const auto offset = static_cast<int>(fixed_.size());
            const auto destinationChild = offset + target + (source < target ? 1 : 0);
            if (!beginMoveRows({}, offset + source, offset + source, {}, destinationChild))
                return false;
            auto moved = std::move(groups_[source]);
            groups_.erase(groups_.begin() + source);
            groups_.insert(groups_.begin() + target, std::move(moved));
            endMoveRows();
        } else return false;
    } else if (presentation.repeatedGroups.size() != groups_.size()) return false;
    refreshFrom(std::move(presentation));
    return true;
}

std::optional<QString> SctParameterTableModel::commit(
    const QModelIndex& modelIndex, const QString& text) {
    const auto* row = parameter(modelIndex);
    if (row == nullptr || !commitHandler_) return tr("This value is read-only.");
    return commitHandler_(*row, text);
}

const core::SctParameterRowPresentation* SctParameterTableModel::parameter(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return nullptr;
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    return node == nullptr || node->kind != NodeKind::Parameter
        ? nullptr : &node->parameter;
}

std::optional<std::uint32_t> SctParameterTableModel::groupOrdinal(
    const QModelIndex& modelIndex) const noexcept {
    if (!modelIndex.isValid()) return std::nullopt;
    const auto* node = static_cast<Node*>(modelIndex.internalPointer());
    if (node == nullptr) return std::nullopt;
    if (node->kind == NodeKind::Group) return node->groupOrdinal;
    return node->parent != nullptr && node->parent->kind == NodeKind::Group
        ? std::optional{node->parent->groupOrdinal} : std::nullopt;
}

const core::SctParameterTablePresentation&
SctParameterTableModel::presentation() const noexcept { return presentation_; }

SctParameterTableModel::Node* SctParameterTableModel::nodeAt(
    const int row, const QModelIndex& parentIndex) const {
    if (parentIndex.isValid()) {
        auto* parent = static_cast<Node*>(parentIndex.internalPointer());
        if (parent == nullptr || parent->kind != NodeKind::Group
            || row >= static_cast<int>(parent->children.size())) return nullptr;
        return parent->children[static_cast<std::size_t>(row)].get();
    }
    if (row < static_cast<int>(fixed_.size())) return fixed_[row].get();
    const auto group = row - static_cast<int>(fixed_.size());
    return group >= 0 && group < static_cast<int>(groups_.size())
        ? groups_[group].get() : nullptr;
}

QModelIndex SctParameterTableModel::indexForNode(
    const Node* node, const int column) const {
    const auto row = rowOf(node);
    return row < 0 ? QModelIndex{}
        : createIndex(row, column, const_cast<Node*>(node));
}

int SctParameterTableModel::rowOf(const Node* node) const {
    if (node == nullptr) return -1;
    if (node->parent != nullptr) {
        const auto found = std::ranges::find_if(node->parent->children,
            [&](const auto& candidate) { return candidate.get() == node; });
        return found == node->parent->children.end() ? -1
            : static_cast<int>(std::distance(node->parent->children.begin(), found));
    }
    const auto fixed = std::ranges::find_if(fixed_,
        [&](const auto& candidate) { return candidate.get() == node; });
    if (fixed != fixed_.end())
        return static_cast<int>(std::distance(fixed_.begin(), fixed));
    const auto group = std::ranges::find_if(groups_,
        [&](const auto& candidate) { return candidate.get() == node; });
    return group == groups_.end() ? -1
        : static_cast<int>(fixed_.size() + std::distance(groups_.begin(), group));
}

std::unique_ptr<SctParameterTableModel::Node>
SctParameterTableModel::makeParameter(
    core::SctParameterRowPresentation parameter, Node* parent) {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Parameter;
    node->parent = parent;
    node->parameter = std::move(parameter);
    return node;
}

std::unique_ptr<SctParameterTableModel::Node>
SctParameterTableModel::makeGroup(core::SctRepeatedGroupPresentation group) {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Group;
    node->groupOrdinal = group.ordinal;
    for (auto parameter : group.parameters)
        node->children.push_back(makeParameter(std::move(parameter), node.get()));
    return node;
}

SctParameterTableModel::Node* SctParameterTableModel::findParameter(
    const spice::sct::SctParameterSite& site) const {
    const auto match = [&](const auto& node) {
        return node->parameter.site == site;
    };
    const auto fixed = std::ranges::find_if(fixed_, match);
    if (fixed != fixed_.end()) return fixed->get();
    for (const auto& group : groups_) {
        const auto found = std::ranges::find_if(group->children, match);
        if (found != group->children.end()) return found->get();
    }
    return nullptr;
}

void SctParameterTableModel::refreshFrom(
    core::SctParameterTablePresentation presentation) {
    presentation_ = std::move(presentation);
    const auto displayChanged = [](const auto& before, const auto& after) {
        return before.parameter != after.parameter || before.value != after.value
            || before.notes != after.notes || before.editor != after.editor;
    };
    for (std::size_t i = 0; i < fixed_.size(); ++i) {
        const auto changed = displayChanged(
            fixed_[i]->parameter, presentation_.fixedParameters[i]);
        fixed_[i]->parameter = presentation_.fixedParameters[i];
        if (changed) emit dataChanged(indexForNode(fixed_[i].get(), 0),
            indexForNode(fixed_[i].get(), 2));
    }
    for (std::size_t i = 0; i < groups_.size(); ++i) {
        auto& node = groups_[i];
        const auto& source = presentation_.repeatedGroups[i];
        const auto ordinalChanged = node->groupOrdinal != source.ordinal;
        node->groupOrdinal = source.ordinal;
        if (node->children.size() != source.parameters.size()) {
            beginResetModel();
            fixed_.clear(); groups_.clear();
            for (auto parameter : presentation_.fixedParameters)
                fixed_.push_back(makeParameter(std::move(parameter)));
            for (auto group : presentation_.repeatedGroups)
                groups_.push_back(makeGroup(std::move(group)));
            endResetModel();
            return;
        }
        if (ordinalChanged) emit dataChanged(
            indexForNode(node.get(), 0), indexForNode(node.get(), 1));
        for (std::size_t p = 0; p < node->children.size(); ++p) {
            const auto changed = displayChanged(
                node->children[p]->parameter, source.parameters[p]);
            node->children[p]->parameter = source.parameters[p];
            if (changed) emit dataChanged(
                index(static_cast<int>(p), 0, indexForNode(node.get())),
                index(static_cast<int>(p), 2, indexForNode(node.get())));
        }
    }
}

SctParameterItemDelegate::SctParameterItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

QWidget* SctParameterItemDelegate::createEditor(QWidget* parent,
    const QStyleOptionViewItem&, const QModelIndex& index) const {
    if ((index.flags() & Qt::ItemIsEditable) == 0) return nullptr;
    auto* editor = new QLineEdit(parent);
    editor->setProperty("salsaParameterIndex",
        QVariant::fromValue(QPersistentModelIndex(index)));
    editor->installEventFilter(const_cast<SctParameterItemDelegate*>(this));
    return editor;
}

void SctParameterItemDelegate::setEditorData(
    QWidget* editor, const QModelIndex& index) const {
    if (auto* line = qobject_cast<QLineEdit*>(editor)) {
        line->setText(index.data(Qt::EditRole).toString());
        line->selectAll();
    }
}

bool SctParameterItemDelegate::eventFilter(QObject* watched, QEvent* event) {
    auto* editor = qobject_cast<QWidget*>(watched);
    if (editor == nullptr) return QStyledItemDelegate::eventFilter(watched, event);
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            emit closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            const auto index = editor->property("salsaParameterIndex")
                .value<QPersistentModelIndex>();
            auto* model = qobject_cast<SctParameterTableModel*>(
                const_cast<QAbstractItemModel*>(index.model()));
            const auto error = model == nullptr
                ? std::optional{tr("Parameter editor is unavailable.")}
                : model->commit(index, qobject_cast<QLineEdit*>(editor)->text());
            if (error) {
                editor->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #c44; }"));
                editor->setToolTip(*error);
                QToolTip::showText(editor->mapToGlobal(editor->rect().bottomLeft()),
                    *error, editor);
                return true;
            }
            emit closeEditor(editor, QAbstractItemDelegate::SubmitModelCache);
            return true;
        }
    }
    if (event->type() == QEvent::FocusOut) {
        emit closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
        return true;
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

} // namespace salsa::qt
