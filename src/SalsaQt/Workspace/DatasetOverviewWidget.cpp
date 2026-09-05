#include "Workspace/DatasetOverviewWidget.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace salsa::qt {
namespace {

void configureValueLabel(QLabel& label) {
    label.setTextInteractionFlags(Qt::TextSelectableByMouse);
    label.setWordWrap(true);
}

QString countText(const std::size_t value) {
    return QLocale().toString(static_cast<qulonglong>(value));
}

}  // namespace

DatasetOverviewWidget::DatasetOverviewWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    heading_ = new QLabel(this);
    auto headingFont = heading_->font();
    headingFont.setPointSize(headingFont.pointSize() + 3);
    headingFont.setBold(true);
    heading_->setFont(headingFont);
    layout->addWidget(heading_);

    auto* datasetGroup = new QGroupBox(tr("Dataset"), this);
    auto* datasetForm = new QFormLayout(datasetGroup);
    datasetState_ = new QLabel(datasetGroup);
    datasetRoot_ = new QLabel(datasetGroup);
    assetCount_ = new QLabel(datasetGroup);
    datasetFingerprint_ = new QLabel(datasetGroup);
    configureValueLabel(*datasetRoot_);
    configureValueLabel(*datasetFingerprint_);
    datasetForm->addRow(tr("State:"), datasetState_);
    datasetForm->addRow(tr("Root:"), datasetRoot_);
    datasetForm->addRow(tr("SCT files:"), assetCount_);
    datasetForm->addRow(tr("Fingerprint:"), datasetFingerprint_);
    layout->addWidget(datasetGroup);

    workspaceGroup_ = new QGroupBox(tr("Workspace"), this);
    auto* workspaceLayout = new QVBoxLayout(workspaceGroup_);
    auto* workspaceForm = new QFormLayout;
    workspaceState_ = new QLabel(workspaceGroup_);
    workspaceRoot_ = new QLabel(workspaceGroup_);
    workspaceId_ = new QLabel(workspaceGroup_);
    authoringState_ = new QLabel(workspaceGroup_);
    configureValueLabel(*workspaceRoot_);
    configureValueLabel(*workspaceId_);
    workspaceForm->addRow(tr("State:"), workspaceState_);
    workspaceForm->addRow(tr("Root:"), workspaceRoot_);
    workspaceForm->addRow(tr("ID:"), workspaceId_);
    workspaceForm->addRow(tr("Authoring:"), authoringState_);
    workspaceLayout->addLayout(workspaceForm);
    workspaceGuidance_ = new QLabel(workspaceGroup_);
    workspaceGuidance_->setWordWrap(true);
    workspaceLayout->addWidget(workspaceGuidance_);
    layout->addWidget(workspaceGroup_);

    auto* sessionGroup = new QGroupBox(tr("Editing session"), this);
    auto* sessionForm = new QFormLayout(sessionGroup);
    openCount_ = new QLabel(sessionGroup);
    dirtyCount_ = new QLabel(sessionGroup);
    conflictCount_ = new QLabel(sessionGroup);
    sessionForm->addRow(tr("Open documents:"), openCount_);
    sessionForm->addRow(tr("Modified since checkpoint:"), dirtyCount_);
    sessionForm->addRow(tr("Patch conflicts:"), conflictCount_);
    layout->addWidget(sessionGroup);

    selectionGroup_ = new QGroupBox(tr("Selected SCT"), this);
    auto* selectionLayout = new QVBoxLayout(selectionGroup_);
    selectionHeading_ = new QLabel(selectionGroup_);
    selectionLayout->addWidget(selectionHeading_);
    auto* assetForm = new QFormLayout;
    assetPath_ = new QLabel(selectionGroup_);
    assetSize_ = new QLabel(selectionGroup_);
    assetRevision_ = new QLabel(selectionGroup_);
    configureValueLabel(*assetPath_);
    configureValueLabel(*assetRevision_);
    assetForm->addRow(tr("Locator:"), assetPath_);
    assetForm->addRow(tr("Size:"), assetSize_);
    assetForm->addRow(tr("Source revision:"), assetRevision_);
    selectionLayout->addLayout(assetForm);
    layout->addWidget(selectionGroup_);

    actionReason_ = new QLabel(this);
    actionReason_->setWordWrap(true);
    actionReason_->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(actionReason_);
    auto* actions = new QHBoxLayout;
    openDatasetButton_ = new QPushButton(tr("Open Dataset..."), this);
    importLegacyButton_ = new QPushButton(tr("Import Legacy Project..."), this);
    associateWorkspaceButton_ = new QPushButton(tr("Open or Create Workspace..."), this);
    openAssetButton_ = new QPushButton(tr("Open SCT"), this);
    actions->addWidget(openDatasetButton_);
    actions->addWidget(importLegacyButton_);
    actions->addWidget(associateWorkspaceButton_);
    actions->addWidget(openAssetButton_);
    actions->addStretch(1);
    layout->addLayout(actions);
    layout->addStretch(1);

    connect(openDatasetButton_, &QPushButton::clicked,
        this, &DatasetOverviewWidget::openDatasetRequested);
    connect(importLegacyButton_, &QPushButton::clicked,
        this, &DatasetOverviewWidget::importLegacyProjectRequested);
    connect(associateWorkspaceButton_, &QPushButton::clicked,
        this, &DatasetOverviewWidget::associateWorkspaceRequested);
    connect(openAssetButton_, &QPushButton::clicked,
        this, &DatasetOverviewWidget::openSelectedAssetRequested);

    clearDataset();
    clearWorkspace();
    setSessionCounts(0, 0, 0);
    setPrimaryActionAvailability(true, false, {}, false, {});
}

void DatasetOverviewWidget::clearDataset() {
    hasDataset_ = false;
    heading_->setText(tr("No dataset is open"));
    datasetState_->setText(tr("Not loaded"));
    datasetRoot_->setText(tr("Not available"));
    assetCount_->setText(QStringLiteral("0"));
    datasetFingerprint_->setText(tr("Not available"));
    setSelectedAsset(std::nullopt);
    syncPrimaryActions();
}

void DatasetOverviewWidget::setDataset(
    const core::DatasetContext& dataset,
    const core::AssetCatalogSnapshot& catalog) {
    hasDataset_ = true;
    heading_->setText(tr("Dataset ready"));
    datasetState_->setText(tr("Loaded"));
    datasetRoot_->setText(QString::fromStdWString(dataset.root.wstring()));
    assetCount_->setText(countText(catalog.assets.size()));
    datasetFingerprint_->setText(QString::fromStdString(catalog.fingerprint.digest.toHex()));
    syncPrimaryActions();
}

void DatasetOverviewWidget::setWorkspace(
    const QString& root, const QString& workspaceId) {
    hasWorkspace_ = true;
    workspaceState_->setText(tr("Associated"));
    workspaceRoot_->setText(root);
    workspaceId_->setText(workspaceId);
    authoringState_->setText(tr("Persistent patch authoring"));
    workspaceGuidance_->setText(tr(
        "Patch checkpointing and persistent workspace authoring are available."));
    syncPrimaryActions();
}

void DatasetOverviewWidget::clearWorkspace() {
    hasWorkspace_ = false;
    workspaceState_->setText(tr("Not associated"));
    workspaceRoot_->setText(tr("Not available"));
    workspaceId_->setText(tr("Not available"));
    authoringState_->setText(hasDataset_
        ? tr("Temporary export-only editing") : tr("Unavailable"));
    workspaceGuidance_->setText(hasDataset_ ? tr(
        "Temporary editing is available. Changes can be exported, but cannot be saved "
        "as a patch checkpoint and will be discarded when the document closes.")
        : tr("Open a dataset before associating a workspace."));
    syncPrimaryActions();
}

void DatasetOverviewWidget::setSessionCounts(
    const std::size_t open, const std::size_t dirty, const std::size_t conflicted) {
    openCount_->setText(countText(open));
    dirtyCount_->setText(countText(dirty));
    conflictCount_->setText(countText(conflicted));
}

void DatasetOverviewWidget::setSelectedAsset(
    const std::optional<core::AssetDescriptor>& asset) {
    hasSelectedAsset_ = asset.has_value();
    if (!asset) {
        selectionHeading_->setText(hasDataset_
            ? tr("Select an SCT file in the Dataset Explorer.")
            : tr("Open a dataset to select an SCT file."));
        assetPath_->setText(tr("Not selected"));
        assetSize_->setText(tr("Not selected"));
        assetRevision_->setText(tr("Not selected"));
        syncPrimaryActions();
        return;
    }
    selectionHeading_->setText(tr("Source metadata"));
    assetPath_->setText(QString::fromStdWString(asset->locator.path().generic_wstring()));
    assetSize_->setText(tr("%1 bytes").arg(countText(asset->byteSize)));
    assetRevision_->setText(QString::fromStdString(asset->revision.digest.toHex()));
    syncPrimaryActions();
}

void DatasetOverviewWidget::setPrimaryActionAvailability(
    const bool shellAvailable, const bool workspaceAvailable,
    const QString& workspaceReason, const bool assetAvailable,
    const QString& assetReason) {
    openDatasetButton_->setEnabled(shellAvailable);
    importLegacyButton_->setEnabled(shellAvailable);
    associateWorkspaceButton_->setEnabled(workspaceAvailable);
    associateWorkspaceButton_->setToolTip(workspaceAvailable ? QString{} : workspaceReason);
    openAssetButton_->setEnabled(assetAvailable);
    openAssetButton_->setToolTip(assetAvailable ? QString{} : assetReason);
    actionReason_->setText(!hasDataset_ || hasWorkspace_ || workspaceAvailable
        ? assetReason : workspaceReason);
    syncPrimaryActions();
}

void DatasetOverviewWidget::syncPrimaryActions() {
    openDatasetButton_->setVisible(!hasDataset_);
    importLegacyButton_->setVisible(!hasDataset_);
    associateWorkspaceButton_->setVisible(hasDataset_ && !hasWorkspace_);
    openAssetButton_->setVisible(hasDataset_ && hasSelectedAsset_);
    actionReason_->setVisible(!actionReason_->text().isEmpty());
}

}  // namespace salsa::qt
