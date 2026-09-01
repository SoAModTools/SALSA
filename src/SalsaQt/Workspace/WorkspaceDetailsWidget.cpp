#include "Workspace/WorkspaceDetailsWidget.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

namespace salsa::qt {
namespace {

void configureValueLabel(QLabel& label) {
    label.setTextInteractionFlags(Qt::TextSelectableByMouse);
    label.setWordWrap(true);
}

}  // namespace

WorkspaceDetailsWidget::WorkspaceDetailsWidget(QWidget* parent)
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
    datasetRoot_ = new QLabel(datasetGroup);
    assetCount_ = new QLabel(datasetGroup);
    datasetFingerprint_ = new QLabel(datasetGroup);
    configureValueLabel(*datasetRoot_);
    configureValueLabel(*datasetFingerprint_);
    datasetForm->addRow(tr("Root:"), datasetRoot_);
    datasetForm->addRow(tr("SCT assets:"), assetCount_);
    datasetForm->addRow(tr("Fingerprint:"), datasetFingerprint_);
    layout->addWidget(datasetGroup);

    auto* selectionGroup = new QGroupBox(tr("Selected asset"), this);
    auto* selectionLayout = new QVBoxLayout(selectionGroup);
    selectionHeading_ = new QLabel(selectionGroup);
    selectionLayout->addWidget(selectionHeading_);
    auto* assetForm = new QFormLayout();
    assetPath_ = new QLabel(selectionGroup);
    assetSize_ = new QLabel(selectionGroup);
    assetRevision_ = new QLabel(selectionGroup);
    configureValueLabel(*assetPath_);
    configureValueLabel(*assetRevision_);
    assetForm->addRow(tr("Locator:"), assetPath_);
    assetForm->addRow(tr("Size:"), assetSize_);
    assetForm->addRow(tr("Source revision:"), assetRevision_);
    selectionLayout->addLayout(assetForm);
    layout->addWidget(selectionGroup);
    layout->addStretch(1);

    clear();
}

void WorkspaceDetailsWidget::clear() {
    heading_->setText(tr("No workspace is open."));
    datasetRoot_->setText(tr("Not available"));
    assetCount_->setText(QStringLiteral("0"));
    datasetFingerprint_->setText(tr("Not available"));
    setSelectedAsset(std::nullopt);
}

void WorkspaceDetailsWidget::setWorkspace(
    const core::DatasetContext& dataset,
    const core::AssetCatalogSnapshot& catalog) {
    heading_->setText(tr("Dataset workspace"));
    datasetRoot_->setText(QString::fromStdWString(dataset.root.wstring()));
    assetCount_->setText(QLocale().toString(static_cast<qulonglong>(catalog.assets.size())));
    datasetFingerprint_->setText(QString::fromStdString(catalog.fingerprint.digest.toHex()));
}

void WorkspaceDetailsWidget::setSelectedAsset(
    const std::optional<core::AssetDescriptor>& asset) {
    if (!asset.has_value()) {
        selectionHeading_->setText(tr("Select an SCT asset in the Project Explorer."));
        assetPath_->setText(tr("Not selected"));
        assetSize_->setText(tr("Not selected"));
        assetRevision_->setText(tr("Not selected"));
        return;
    }

    selectionHeading_->setText(tr("Source metadata"));
    assetPath_->setText(QString::fromStdWString(asset->locator.path().generic_wstring()));
    assetSize_->setText(tr("%1 bytes").arg(
        QLocale().toString(static_cast<qulonglong>(asset->byteSize))));
    assetRevision_->setText(QString::fromStdString(asset->revision.digest.toHex()));
}

}  // namespace salsa::qt
