#include "Sct/SctDocumentWidget.h"
#include "Sct/SctDocumentController.h"

#include "SalsaCore/Sct/SctPresentation.h"

#include "SpiceSCT/SctDocumentIndex.h"

#include <QComboBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextFormat>
#include <QTreeWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>
#include <algorithm>
#include <ranges>

namespace salsa::qt {
namespace {

void expandAncestors(QTreeView& tree, QModelIndex index) {
    for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
        tree.setExpanded(parent, true);
}

void expandAncestors(QTreeWidgetItem* item) {
    for (auto* parent = item == nullptr ? nullptr : item->parent();
        parent != nullptr; parent = parent->parent()) {
        parent->setExpanded(true);
    }
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
    outline_ = new QTreeView(splitter);
    outlineModel_ = new SctOutlineModel(outline_);
    outline_->setModel(outlineModel_);
    outline_->setContextMenuPolicy(Qt::CustomContextMenu);
    outline_->header()->setSectionResizeMode(QHeaderView::Interactive);
    outline_->header()->setStretchLastSection(false);
    outline_->header()->resizeSection(0, 420);
    outline_->header()->resizeSection(1, 180);

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
    properties_->header()->setSectionResizeMode(QHeaderView::Interactive);
    properties_->header()->setStretchLastSection(false);
    properties_->header()->resizeSection(0, 180);
    properties_->header()->resizeSection(1, 240);
    properties_->header()->resizeSection(2, 280);
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

    connect(outline_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current) {
            const auto target = outlineModel_->target(current);
            if (!target.has_value()) return;
            currentTarget_ = *target;
            showTarget(*currentTarget_);
            emit becameActive(QString::fromStdString(locator_.identityKey()));
            emit editContextChanged();
        });
    connect(outline_, &QTreeView::activated, this,
        [this](const QModelIndex& modelIndex) {
            if (!modelIndex.isValid() || !snapshot_) return;
            const auto target = *outlineModel_->target(modelIndex);
            if (target.kind == core::SctNavigationKind::OpaqueAttachment) {
                const auto* attachment = index_
                    ? index_->find(spice::sct::SctOpaqueAttachmentId(target.id))
                    : nullptr;
                if (attachment != nullptr)
                    selectTarget(core::navigationTargetForOpaqueAnchor(attachment->anchor));
                return;
            }
            if (editingEnabled_ && selectedMessageTarget().has_value())
                emit editMessageRequested(QString::fromStdString(locator_.identityKey()));
        });
    connect(applyConventionButton_, &QPushButton::clicked, this, [this]() {
        emit textConventionRequested(QString::fromStdString(locator_.identityKey()),
            conventionCombo_->currentData().toInt());
    });
    connect(reloadButton_, &QPushButton::clicked, this, [this]() {
        emit reloadRequested(QString::fromStdString(locator_.identityKey()));
    });
    connect(outline_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& position) {
        QMenu menu(this);
        auto* editMessage = menu.addAction(tr("Edit Message"));
        editMessage->setEnabled(canEditSelectedMessage());
        menu.addSeparator();
        auto* insert = menu.addAction(tr("Insert Instruction..."));
        auto* remove = menu.addAction(tr("Delete Instruction"));
        menu.addSeparator();
        auto* moveUp = menu.addAction(tr("Move Up"));
        auto* moveDown = menu.addAction(tr("Move Down"));
        insert->setEnabled(editingEnabled_ && insertionContext().has_value());
        remove->setEnabled(editingEnabled_ && canDeleteSelected());
        moveUp->setEnabled(editingEnabled_
            && canMoveSelected(core::SctInstructionMoveDirection::Up));
        moveDown->setEnabled(editingEnabled_
            && canMoveSelected(core::SctInstructionMoveDirection::Down));
        connect(editMessage, &QAction::triggered, this, [this]() {
            emit editMessageRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(insert, &QAction::triggered, this, [this]() {
            emit insertInstructionRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(remove, &QAction::triggered, this, [this]() {
            emit deleteInstructionRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(moveUp, &QAction::triggered, this, [this]() {
            emit moveInstructionRequested(QString::fromStdString(locator_.identityKey()),
                static_cast<int>(core::SctInstructionMoveDirection::Up));
        });
        connect(moveDown, &QAction::triggered, this, [this]() {
            emit moveInstructionRequested(QString::fromStdString(locator_.identityKey()),
                static_cast<int>(core::SctInstructionMoveDirection::Down));
        });
        menu.exec(outline_->viewport()->mapToGlobal(position));
    });
}

const core::AssetLocator& SctDocumentWidget::locator() const noexcept { return locator_; }

void SctDocumentWidget::setSnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus) {
    snapshot_ = std::move(snapshot);
    index_.reset();
    if (snapshot_ && snapshot_->document)
        index_ = std::make_shared<const spice::sct::SctDocumentIndex>(
            spice::sct::SctDocumentIndex::build(*snapshot_->document));
    updateSourceBanner(sourceStatus);
    if (!snapshot_) return;
    const auto& assessment = snapshot_->provenance->inspection->textAssessment;
    const bool hasText = !assessment.records.empty();
    conventionBanner_->setVisible(hasText);
    conventionCombo_->setVisible(hasText);
    applyConventionButton_->setVisible(hasText);
    if (!hasText) {
        conventionBanner_->setText({});
    } else if (snapshot_->provenance->textConvention.has_value()) {
        conventionBanner_->setText(tr("Text is interpreted as %1.")
            .arg(QString::fromUtf8(
                core::sctTextConventionName(*snapshot_->provenance->textConvention).data(),
                static_cast<qsizetype>(core::sctTextConventionName(*snapshot_->provenance->textConvention).size()))));
        const auto comboIndex = conventionCombo_->findData(static_cast<int>(*snapshot_->provenance->textConvention));
        if (comboIndex >= 0) conventionCombo_->setCurrentIndex(comboIndex);
    } else if (assessment.viableConventions.empty()) {
        conventionBanner_->setText(tr("No convention decodes every text record. Text remains opaque; choose an interpretation to inspect it."));
    } else {
        conventionBanner_->setText(tr("Multiple text conventions are viable. Text remains opaque until you choose one."));
    }
    rebuildOutline();
}

void SctDocumentWidget::installVerifiedSnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    std::shared_ptr<const spice::sct::SctDocumentIndex> index,
    const int sourceStatus) {
    snapshot_ = std::move(snapshot);
    index_ = std::move(index);
    updateSourceBanner(sourceStatus);
    if (outlineReconciliationPending_) {
        rebuildOutline();
        outlineReconciliationPending_ = false;
    }
    if (currentTarget_.has_value()) showTarget(*currentTarget_);
}

void SctDocumentWidget::applyTextOnlySnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus,
    const core::SctEditChangeSet& changes) {
    Q_UNUSED(snapshot);
    updateSourceBanner(sourceStatus);
    if (!currentTarget_.has_value()) return;
    const bool selectedChanged = std::ranges::any_of(
        changes.modified, [this](const auto target) {
            return target == *currentTarget_;
        });
    Q_UNUSED(selectedChanged);
}

bool SctDocumentWidget::applyInstructionChanges(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus,
    const core::SctEditChangeSet& changes) {
    Q_UNUSED(snapshot);
    if (changes.instructions.empty() || !changes.modified.empty()) return false;
    const auto retained = currentTarget_;
    updateSourceBanner(sourceStatus);
    if (!outlineModel_->apply(changes)) {
        outlineReconciliationPending_ = true;
        return false;
    }

    const auto selected = retained.has_value()
        ? outlineModel_->indexForTarget(*retained) : QModelIndex{};
    if (selected.isValid()) {
        currentTarget_ = *retained;
        outline_->setCurrentIndex(selected);
    } else {
        currentTarget_.reset();
        title_->clear();
        subtitle_->clear();
        propertyLocations_.clear();
        properties_->clear();
        preview_->clear();
        preview_->hide();
    }
    return true;
}

void SctDocumentWidget::setSourceStatus(const int sourceStatus) {
    updateSourceBanner(sourceStatus);
}

void SctDocumentWidget::selectTarget(
    const core::SctNavigationTarget target,
    const bool reveal) {
    (void)selectLocation(core::SctInspectionLocation{ target }, reveal);
}

bool SctDocumentWidget::selectLocation(
    const core::SctInspectionLocation& location,
    const bool reveal) {
    const auto target = core::owningNavigationTarget(location);
    const auto found = outlineModel_->indexForTarget(target);
    if (!found.isValid()) return false;
    if (reveal) expandAncestors(*outline_, found);
    outline_->setCurrentIndex(found);
    if (!currentTarget_.has_value() || *currentTarget_ != target) {
        currentTarget_ = target;
        showTarget(target);
        emit becameActive(QString::fromStdString(locator_.identityKey()));
        emit editContextChanged();
    }
    if (reveal) outline_->scrollTo(found);

    if (std::holds_alternative<core::SctNavigationTarget>(location)) return true;
    const auto property = std::ranges::find_if(propertyLocations_,
        [&location](const auto& entry) { return entry.second == location; });
    if (property == propertyLocations_.end()) return false;
    if (reveal) expandAncestors(property->first);
    properties_->setCurrentItem(property->first);
    if (reveal) properties_->scrollToItem(property->first);
    return true;
}

void SctDocumentWidget::setEditingEnabled(const bool enabled) {
    if (editingEnabled_ == enabled) return;
    editingEnabled_ = enabled;
    emit editContextChanged();
}

std::optional<core::SctNavigationTarget> SctDocumentWidget::currentTarget() const noexcept {
    return currentTarget_;
}

std::optional<SctDocumentWidget::InstructionInsertionContext>
SctDocumentWidget::insertionContext() const {
    if (!currentTarget_.has_value()) return std::nullopt;
    if (currentTarget_->kind == core::SctNavigationKind::Instruction) {
        const auto instruction = spice::sct::SctInstructionId(currentTarget_->id);
        const auto* existing = outlineModel_->instruction(instruction);
        if (existing == nullptr || existing->opcode == 12u)
            return std::nullopt;
        return InstructionInsertionContext{ instruction,
            !outlineModel_->nextInstruction(instruction).has_value() };
    }
    return std::nullopt;
}

bool SctDocumentWidget::canDeleteSelected() const {
    const auto instruction = selectedInstruction();
    if (!instruction.has_value()) return false;
    const auto* existing = outlineModel_->instruction(*instruction);
    return existing != nullptr && !(existing->opcode == 9u
        && !outlineModel_->previousInstruction(*instruction).has_value());
}

std::optional<spice::sct::SctInstructionId> SctDocumentWidget::selectedInstruction() const {
    if (!currentTarget_.has_value()
        || currentTarget_->kind != core::SctNavigationKind::Instruction) return std::nullopt;
    return spice::sct::SctInstructionId(currentTarget_->id);
}

std::optional<core::SctMessageTarget> SctDocumentWidget::selectedMessageTarget() const {
    if (!snapshot_ || !currentTarget_.has_value()) return std::nullopt;
    if (!index_) return std::nullopt;
    const auto& index = *index_;
    if (currentTarget_->kind == core::SctNavigationKind::String) {
        const auto id = spice::sct::SctStringId(currentTarget_->id);
        const auto* string = index.find(id);
        if (string != nullptr && std::holds_alternative<spice::sct::SctMessage>(string->value))
            return core::SctMessageTarget{id};
    } else if (currentTarget_->kind == core::SctNavigationKind::FooterEntry) {
        const auto id = spice::sct::SctFooterEntryId(currentTarget_->id);
        const auto* entry = index.find(id);
        if (entry != nullptr && std::holds_alternative<spice::sct::SctMessage>(entry->value))
            return core::SctMessageTarget{id};
    }
    return std::nullopt;
}

bool SctDocumentWidget::canEditSelectedMessage() const {
    return editingEnabled_ && selectedMessageTarget().has_value();
}

bool SctDocumentWidget::canMoveSelected(const core::SctInstructionMoveDirection direction) const {
    const auto instruction = selectedInstruction();
    if (!instruction.has_value()) return false;
    const auto* current = outlineModel_->instruction(*instruction);
    const auto otherId = direction == core::SctInstructionMoveDirection::Up
        ? outlineModel_->previousInstruction(*instruction)
        : outlineModel_->nextInstruction(*instruction);
    const auto* other = otherId.has_value() ? outlineModel_->instruction(*otherId) : nullptr;
    if (current == nullptr || other == nullptr) return false;
    const auto opcode = current->opcode;
    const auto otherOpcode = other->opcode;
    return opcode != 9u && opcode != 12u && otherOpcode != 9u && otherOpcode != 12u;
}

void SctDocumentWidget::rebuildOutline() {
    const auto retained = currentTarget_;
    if (!snapshot_ || !snapshot_->document) return;
    outlineModel_->resetFrom(core::SctPresentationService::outline(*snapshot_),
        *snapshot_->document);
    outline_->collapseAll();
    auto selected = retained.has_value()
        ? outlineModel_->indexForTarget(*retained) : QModelIndex{};
    if (!selected.isValid() && outlineModel_->rowCount() != 0)
        selected = outlineModel_->index(0, 0);
    if (selected.isValid()) {
        currentTarget_ = outlineModel_->target(selected);
        outline_->setCurrentIndex(selected);
        showTarget(*currentTarget_);
    } else {
        currentTarget_.reset();
    }
}

void SctDocumentWidget::showTarget(const core::SctNavigationTarget target) {
    if (!snapshot_) return;
    if (target.kind == core::SctNavigationKind::Instruction
        && (!index_
            || index_->find(spice::sct::SctInstructionId(target.id)) == nullptr)) {
        const auto* instruction = outlineModel_->instruction(
            spice::sct::SctInstructionId(target.id));
        if (instruction != nullptr) {
            title_->setText(tr("Instruction %1").arg(target.id));
            subtitle_->setText(tr("Pending background verification"));
            propertyLocations_.clear();
            properties_->clear();
            auto* opcode = new QTreeWidgetItem(properties_);
            opcode->setText(0, tr("Opcode"));
            opcode->setText(1, QString::number(instruction->opcode));
            preview_->clear();
            preview_->hide();
            return;
        }
    }
    const auto presentation = index_
        ? core::SctPresentationService::describe(*snapshot_, target, *index_)
        : core::SctPresentationService::describe(*snapshot_, target);
    title_->setText(QString::fromStdString(presentation.title));
    subtitle_->setText(QString::fromStdString(presentation.subtitle));
    propertyLocations_.clear();
    properties_->clear();
    for (const auto& property : presentation.properties) addPropertyItem(nullptr, property);
    properties_->collapseAll();
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

QTreeWidgetItem* SctDocumentWidget::addPropertyItem(
    QTreeWidgetItem* parent,
    const core::SctPropertyItem& property) {
    auto* item = parent == nullptr
        ? new QTreeWidgetItem(properties_)
        : new QTreeWidgetItem(parent);
    item->setText(0, QString::fromStdString(property.name));
    item->setText(1, QString::fromStdString(property.value));
    item->setText(2, QString::fromStdString(property.notes));
    if (property.location.has_value())
        propertyLocations_.emplace_back(item, *property.location);
    for (const auto& child : property.children) addPropertyItem(item, child);
    return item;
}

}  // namespace salsa::qt
