#pragma once

#include "SalsaCore/Project/ProjectTypes.h"

#include <QWidget>

#include <optional>

class QLabel;

namespace salsa::qt {

class WorkspaceDetailsWidget final : public QWidget {
public:
    explicit WorkspaceDetailsWidget(QWidget* parent = nullptr);

    void clear();
    void setWorkspace(
        const core::DatasetContext& dataset,
        const core::AssetCatalogSnapshot& catalog);
    void setSelectedAsset(const std::optional<core::AssetDescriptor>& asset);

private:
    QLabel* heading_ = nullptr;
    QLabel* datasetRoot_ = nullptr;
    QLabel* assetCount_ = nullptr;
    QLabel* datasetFingerprint_ = nullptr;
    QLabel* selectionHeading_ = nullptr;
    QLabel* assetPath_ = nullptr;
    QLabel* assetSize_ = nullptr;
    QLabel* assetRevision_ = nullptr;
};

}  // namespace salsa::qt
