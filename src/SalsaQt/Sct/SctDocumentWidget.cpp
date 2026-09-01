#include "Sct/SctDocumentWidget.h"
#include "Sct/SctDocumentController.h"

#include "SalsaCore/Sct/SctPresentation.h"

#include <QComboBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextFormat>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace salsa::qt {
namespace {

constexpr int KindRole = Qt::UserRole;
constexpr int IdRole = Qt::UserRole + 1;

void addProperty(QTreeWidget* tree, QTreeWidgetItem* parent, const core::SctPropertyItem& property) {
    auto* item = parent == nullptr ? new QTreeWidgetItem(tree) : new QTreeWidgetItem(parent);
    item->setText(0, QString::fromStdString(property.name));
    item->setText(1, QString::fromStdString(property.value));
    item->setText(2, QString::fromStdString(property.notes));
    for (const auto& child : property.children) addProperty(tree, item, child);
}

}  // namespace

SctDocumentWidget::SctDocumentWidget(core::AssetLocator locator, QWidget* parent)
    : QWidget(parent), locator_(std::move(locator)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    sourceBanner_ = new QLabel(this);
    sourceBanner_->setWordWrap(true);
    sourceBanner_->hide();
    reloadButton_ = new QPushButton(tr("Reload from dataset"), this);
    reloadButton_->hide();
    auto* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(sourceBanner_, 1);
    sourceRow->addWidget(reloadButton_);
    layout->addLayout(sourceRow);

    conventionBanner_ = new QLabel(this);
    conventionBanner_->setWordWrap(true);
    conventionCombo_ = new QComboBox(this);
    for (const auto& descriptor : spice::sct::sctKnownTextConventions()) {
        conventionCombo_->addItem(QString::fromUtf8(descriptor.stableName.data(),
            static_cast<qsizetype>(descriptor.stableName.size())),
            static_cast<int>(descriptor.convention));
    }
    applyConventionButton_ = new QPushButton(tr("Apply interpretation"), this);
    auto* conventionRow = new QHBoxLayout;
    conventionRow->addWidget(conventionBanner_, 1);
    conventionRow->addWidget(conventionCombo_);
    conventionRow->addWidget(applyConventionButton_);
    layout->addLayout(conventionRow);

    auto* splitter = new QSplitter(this);
    outline_ = new QTreeWidget(splitter);
    outline_->setHeaderLabels({ tr("Physical outline"), tr("Kind / ID") });
    outline_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    outline_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    auto* details = new QWidget(splitter);
    auto* detailsLayout = new QVBoxLayout(details);
    title_ = new QLabel(details);
    auto titleFont = title_->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title_->setFont(titleFont);
    subtitle_ = new QLabel(details);
    subtitle_->setWordWrap(true);
    properties_ = new QTreeWidget(details);
    properties_->setHeaderLabels({ tr("Property"), tr("Value"), tr("Notes") });
    properties_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    properties_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    properties_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    preview_ = new QTextEdit(details);
    preview_->setReadOnly(true);
    preview_->setPlaceholderText(tr("No visual text preview is available for this entity."));
    preview_->setMaximumHeight(170);
    detailsLayout->addWidget(title_);
    detailsLayout->addWidget(subtitle_);
    detailsLayout->addWidget(properties_, 1);
    detailsLayout->addWidget(preview_);
    splitter->addWidget(outline_);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    connect(outline_, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* current) {
            if (current == nullptr) return;
            currentTarget_ = core::SctNavigationTarget{
                static_cast<core::SctNavigationKind>(current->data(0, KindRole).toInt()),
                current->data(0, IdRole).toULongLong() };
            showTarget(*currentTarget_);
            emit becameActive(QString::fromStdString(locator_.identityKey()));
        });
    connect(applyConventionButton_, &QPushButton::clicked, this, [this]() {
        emit textConventionRequested(QString::fromStdString(locator_.identityKey()),
            conventionCombo_->currentData().toInt());
    });
    connect(reloadButton_, &QPushButton::clicked, this, [this]() {
        emit reloadRequested(QString::fromStdString(locator_.identityKey()));
    });
}

const core::AssetLocator& SctDocumentWidget::locator() const noexcept { return locator_; }

void SctDocumentWidget::setSnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus) {
    snapshot_ = std::move(snapshot);
    updateSourceBanner(sourceStatus);
    if (!snapshot_) return;
    const auto& assessment = snapshot_->inspection->textAssessment;
    const bool hasText = !assessment.records.empty();
    conventionBanner_->setVisible(hasText);
    conventionCombo_->setVisible(hasText);
    applyConventionButton_->setVisible(hasText);
    if (!hasText) {
        conventionBanner_->setText({});
    } else if (snapshot_->textConvention.has_value()) {
        conventionBanner_->setText(tr("Text is interpreted as %1.")
            .arg(QString::fromUtf8(
                core::sctTextConventionName(*snapshot_->textConvention).data(),
                static_cast<qsizetype>(core::sctTextConventionName(*snapshot_->textConvention).size()))));
        const auto comboIndex = conventionCombo_->findData(static_cast<int>(*snapshot_->textConvention));
        if (comboIndex >= 0) conventionCombo_->setCurrentIndex(comboIndex);
    } else if (assessment.viableConventions.empty()) {
        conventionBanner_->setText(tr("No convention decodes every text record. Text remains opaque; choose an interpretation to inspect it."));
    } else {
        conventionBanner_->setText(tr("Multiple text conventions are viable. Text remains opaque until you choose one."));
    }
    rebuildOutline();
}

void SctDocumentWidget::selectTarget(const core::SctNavigationTarget target) {
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
        if (static_cast<core::SctNavigationKind>(item->data(0, KindRole).toInt()) == target.kind
            && item->data(0, IdRole).toULongLong() == target.id) return item;
        for (int i = 0; i < item->childCount(); ++i)
            if (auto* result = find(item->child(i))) return result;
        return nullptr;
    };
    for (int i = 0; i < outline_->topLevelItemCount(); ++i) {
        if (auto* found = find(outline_->topLevelItem(i))) {
            outline_->setCurrentItem(found);
            outline_->scrollToItem(found);
            return;
        }
    }
}

void SctDocumentWidget::rebuildOutline() {
    outline_->clear();
    if (!snapshot_) return;
    for (const auto& item : core::SctPresentationService::outline(*snapshot_)) addOutlineItem(nullptr, item);
    outline_->expandToDepth(1);
    if (outline_->topLevelItemCount() != 0) {
        const auto retained = currentTarget_;
        outline_->setCurrentItem(outline_->topLevelItem(0));
        if (retained.has_value()) selectTarget(*retained);
    }
}

void SctDocumentWidget::showTarget(const core::SctNavigationTarget target) {
    if (!snapshot_) return;
    const auto presentation = core::SctPresentationService::describe(*snapshot_, target);
    title_->setText(QString::fromStdString(presentation.title));
    subtitle_->setText(QString::fromStdString(presentation.subtitle));
    properties_->clear();
    for (const auto& property : presentation.properties) addProperty(properties_, nullptr, property);
    properties_->expandToDepth(1);
    preview_->clear();
    QTextCursor cursor(preview_->document());
    for (const auto& run : presentation.preview) {
        QTextCharFormat format;
        format.setFontWeight(run.bold ? QFont::Bold : QFont::Normal);
        if (run.rgb.has_value()) format.setForeground(QColor::fromRgb(*run.rgb));
        cursor.insertText(QString::fromStdString(run.text), format);
    }
    preview_->setVisible(!presentation.preview.empty());
}

void SctDocumentWidget::updateSourceBanner(const int sourceStatus) {
    const auto status = static_cast<SctDocumentController::SourceStatus>(sourceStatus);
    if (status == SctDocumentController::SourceStatus::Current) {
        sourceBanner_->hide(); reloadButton_->hide();
    } else if (status == SctDocumentController::SourceStatus::Changed) {
        sourceBanner_->setText(tr("The dataset copy changed. This tab still shows its earlier immutable snapshot."));
        sourceBanner_->show(); reloadButton_->show();
    } else {
        sourceBanner_->setText(tr("The source asset is missing from the refreshed dataset. This tab retains its loaded snapshot."));
        sourceBanner_->show(); reloadButton_->hide();
    }
}

QTreeWidgetItem* SctDocumentWidget::addOutlineItem(
    QTreeWidgetItem* parent, const core::SctOutlineItem& item) {
    auto* widgetItem = parent == nullptr ? new QTreeWidgetItem(outline_) : new QTreeWidgetItem(parent);
    widgetItem->setText(0, QString::fromStdString(item.label));
    widgetItem->setText(1, QString::fromStdString(item.secondary));
    widgetItem->setData(0, KindRole, static_cast<int>(item.target.kind));
    widgetItem->setData(0, IdRole, QVariant::fromValue<qulonglong>(item.target.id));
    for (const auto& child : item.children) addOutlineItem(widgetItem, child);
    return widgetItem;
}

}  // namespace salsa::qt
