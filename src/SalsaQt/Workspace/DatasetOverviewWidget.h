#pragma once

#include "SalsaCore/Project/ProjectTypes.h"

#include <QWidget>

#include <cstddef>
#include <optional>

class QLabel;
class QPushButton;
class QGroupBox;

namespace salsa::qt {

class DatasetOverviewWidget final : public QWidget {
    Q_OBJECT

public:
    explicit DatasetOverviewWidget(QWidget* parent = nullptr);

    void clearDataset();
    void setDataset(
        const core::DatasetContext& dataset,
        const core::AssetCatalogSnapshot& catalog);
    void setWorkspace(const QString& root, const QString& workspaceId);
    void clearWorkspace();
    void setSessionCounts(std::size_t open, std::size_t dirty, std::size_t conflicted);
    void setSelectedAsset(const std::optional<core::AssetDescriptor>& asset);
    void setPrimaryActionAvailability(bool shellAvailable,
        bool workspaceAvailable, const QString& workspaceReason,
        bool assetAvailable, const QString& assetReason);

signals:
    void openDatasetRequested();
    void importLegacyProjectRequested();
    void associateWorkspaceRequested();
    void openSelectedAssetRequested();

private:
    void syncPrimaryActions();

    QLabel* heading_ = nullptr;
    QLabel* datasetState_ = nullptr;
    QLabel* datasetRoot_ = nullptr;
    QLabel* assetCount_ = nullptr;
    QLabel* datasetFingerprint_ = nullptr;
    QGroupBox* workspaceGroup_ = nullptr;
    QLabel* workspaceState_ = nullptr;
    QLabel* workspaceRoot_ = nullptr;
    QLabel* workspaceId_ = nullptr;
    QLabel* authoringState_ = nullptr;
    QLabel* workspaceGuidance_ = nullptr;
    QLabel* openCount_ = nullptr;
    QLabel* dirtyCount_ = nullptr;
    QLabel* conflictCount_ = nullptr;
    QGroupBox* selectionGroup_ = nullptr;
    QLabel* selectionHeading_ = nullptr;
    QLabel* assetPath_ = nullptr;
    QLabel* assetSize_ = nullptr;
    QLabel* assetRevision_ = nullptr;
    QLabel* actionReason_ = nullptr;
    QPushButton* openDatasetButton_ = nullptr;
    QPushButton* importLegacyButton_ = nullptr;
    QPushButton* associateWorkspaceButton_ = nullptr;
    QPushButton* openAssetButton_ = nullptr;
    bool hasDataset_ = false;
    bool hasWorkspace_ = false;
    bool hasSelectedAsset_ = false;
};

}  // namespace salsa::qt
