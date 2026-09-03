#include "Workspace/WorkspaceModel.h"

#include <QLocale>

#include <algorithm>
#include <iterator>
#include <utility>

namespace salsa::qt {

WorkspaceModel::WorkspaceModel(QObject* parent)
    : QAbstractItemModel(parent) {}

void WorkspaceModel::setSnapshot(const core::AssetCatalogSnapshot& snapshot) {
    beginResetModel();
    root_.children.clear();

    for (const auto& asset : snapshot.assets) {
        Node* parent = &root_;
        auto component = asset.locator.path().begin();
        const auto end = asset.locator.path().end();
        while (component != end) {
            const auto next = std::next(component);
            const auto name = QString::fromStdWString(component->wstring());
            if (next == end) {
                auto node = std::make_unique<Node>();
                node->name = name;
                node->parent = parent;
                node->asset = asset;
                parent->children.push_back(std::move(node));
            } else {
                auto* directory = findDirectory(*parent, name);
                if (directory == nullptr) {
                    auto node = std::make_unique<Node>();
                    node->name = name;
                    node->parent = parent;
                    directory = node.get();
                    parent->children.push_back(std::move(node));
                }
                parent = directory;
            }
            component = next;
        }
    }

    sortChildren(root_);
    endResetModel();
}

void WorkspaceModel::clear() {
    beginResetModel();
    root_.children.clear();
    endResetModel();
}

std::optional<core::AssetDescriptor> WorkspaceModel::assetAt(
    const QModelIndex& index) const {
    if (!index.isValid()) {
        return std::nullopt;
    }
    const auto* node = static_cast<const Node*>(index.internalPointer());
    return node->asset;
}

QModelIndex WorkspaceModel::indexForLocator(const core::AssetLocator& locator) const {
    return indexForLocator(root_, locator);
}

std::optional<std::filesystem::path> WorkspaceModel::logicalDirectoryAt(
    const QModelIndex& index) const {
    if (!index.isValid()) return std::nullopt;
    const auto* node = static_cast<const Node*>(index.internalPointer());
    if (node->asset) return std::nullopt;
    std::filesystem::path result;
    std::vector<std::wstring> components;
    while (node != nullptr && node != &root_) {
        components.push_back(node->name.toStdWString());
        node = node->parent;
    }
    for (auto component = components.rbegin(); component != components.rend(); ++component)
        result /= *component;
    return result;
}

QModelIndex WorkspaceModel::indexForLogicalDirectory(
    const std::filesystem::path& path) const {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return {};
    return indexForLogicalDirectory(root_, path.begin(), path.end());
}

QModelIndex WorkspaceModel::index(
    const int row,
    const int column,
    const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= columnCount(parentIndex)) {
        return {};
    }
    const auto* parentNode = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer())
        : &root_;
    if (static_cast<std::size_t>(row) >= parentNode->children.size()) {
        return {};
    }
    return createIndex(row, column, parentNode->children[static_cast<std::size_t>(row)].get());
}

QModelIndex WorkspaceModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    const auto* node = static_cast<const Node*>(child.internalPointer());
    const auto* parentNode = node->parent;
    if (parentNode == nullptr || parentNode == &root_) {
        return {};
    }
    const auto* grandparent = parentNode->parent;
    if (grandparent == nullptr) {
        return {};
    }
    const auto found = std::ranges::find_if(grandparent->children, [parentNode](const auto& entry) {
        return entry.get() == parentNode;
    });
    if (found == grandparent->children.end()) {
        return {};
    }
    return createIndex(
        static_cast<int>(std::distance(grandparent->children.begin(), found)),
        0,
        const_cast<Node*>(parentNode));
}

int WorkspaceModel::rowCount(const QModelIndex& parentIndex) const {
    if (parentIndex.isValid() && parentIndex.column() != 0) {
        return 0;
    }
    const auto* node = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer())
        : &root_;
    return static_cast<int>(node->children.size());
}

int WorkspaceModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant WorkspaceModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid()) {
        return {};
    }
    const auto* node = static_cast<const Node*>(index.internalPointer());
    if (role == Qt::DisplayRole) {
        if (index.column() == 0) {
            return node->name;
        }
    }
    if (role == Qt::ToolTipRole && node->asset.has_value()) {
        return tr("%1\n%2 bytes")
            .arg(QString::fromStdWString(node->asset->locator.path().generic_wstring()))
            .arg(QLocale().toString(static_cast<qulonglong>(node->asset->byteSize)));
    }
    return {};
}

QVariant WorkspaceModel::headerData(
    const int section,
    const Qt::Orientation orientation,
    const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    if (section == 0) {
        return tr("Asset");
    }
    return {};
}

WorkspaceModel::Node* WorkspaceModel::findDirectory(Node& parent, const QString& name) {
    const auto found = std::ranges::find_if(parent.children, [&name](const auto& entry) {
        return !entry->asset.has_value() &&
            entry->name.compare(name, Qt::CaseInsensitive) == 0;
    });
    return found == parent.children.end() ? nullptr : found->get();
}

void WorkspaceModel::sortChildren(Node& parent) {
    std::ranges::sort(parent.children, [](const auto& left, const auto& right) {
        if (left->asset.has_value() != right->asset.has_value()) {
            return !left->asset.has_value();
        }
        return left->name.compare(right->name, Qt::CaseInsensitive) < 0;
    });
    for (const auto& child : parent.children) {
        sortChildren(*child);
    }
}

QModelIndex WorkspaceModel::indexForLocator(
    const Node& parent,
    const core::AssetLocator& locator) const {
    for (std::size_t row = 0; row < parent.children.size(); ++row) {
        const auto& child = *parent.children[row];
        if (child.asset.has_value() && child.asset->locator == locator) {
            return createIndex(static_cast<int>(row), 0, const_cast<Node*>(&child));
        }
        const auto nested = indexForLocator(child, locator);
        if (nested.isValid()) {
            return nested;
        }
    }
    return {};
}

QModelIndex WorkspaceModel::indexForLogicalDirectory(
    const Node& parent,
    const std::filesystem::path::const_iterator component,
    const std::filesystem::path::const_iterator end) const {
    if (component == end) return {};
    for (std::size_t row = 0; row < parent.children.size(); ++row) {
        const auto& child = *parent.children[row];
        if (child.asset || child.name.compare(
                QString::fromStdWString(component->wstring()), Qt::CaseInsensitive) != 0)
            continue;
        const auto current = createIndex(static_cast<int>(row), 0,
            const_cast<Node*>(&child));
        const auto next = std::next(component);
        return next == end ? current : indexForLogicalDirectory(child, next, end);
    }
    return {};
}

}  // namespace salsa::qt
