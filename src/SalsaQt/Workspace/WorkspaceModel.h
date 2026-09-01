#pragma once

#include "SalsaCore/Project/ProjectTypes.h"

#include <QAbstractItemModel>

#include <memory>
#include <optional>
#include <vector>

namespace salsa::qt {

class WorkspaceModel final : public QAbstractItemModel {
public:
    explicit WorkspaceModel(QObject* parent = nullptr);

    void setSnapshot(const core::AssetCatalogSnapshot& snapshot);
    void clear();

    [[nodiscard]] std::optional<core::AssetDescriptor> assetAt(
        const QModelIndex& index) const;
    [[nodiscard]] QModelIndex indexForLocator(const core::AssetLocator& locator) const;

    [[nodiscard]] QModelIndex index(
        int row,
        int column,
        const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;

private:
    struct Node final {
        QString name{};
        Node* parent = nullptr;
        std::optional<core::AssetDescriptor> asset{};
        std::vector<std::unique_ptr<Node>> children{};
    };

    [[nodiscard]] static Node* findDirectory(Node& parent, const QString& name);
    static void sortChildren(Node& parent);
    [[nodiscard]] QModelIndex indexForLocator(
        const Node& parent,
        const core::AssetLocator& locator) const;

    Node root_{};
};

}  // namespace salsa::qt
