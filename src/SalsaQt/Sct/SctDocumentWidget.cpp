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
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>
#include <algorithm>
#include <ranges>

namespace salsa::qt {
namespace {

std::optional<core::SctNavigationTarget> navigationForEntity(
    const spice::sct::SctDocumentEntityId& entity) {
    return std::visit([](const auto& id) -> std::optional<core::SctNavigationTarget> {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, std::monostate>) return std::nullopt;
        else if constexpr (std::is_same_v<T, spice::sct::SctSectionId>)
            return core::SctNavigationTarget{core::SctNavigationKind::Section, id.value()};
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return core::SctNavigationTarget{core::SctNavigationKind::Instruction, id.value()};
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return core::SctNavigationTarget{core::SctNavigationKind::String, id.value()};
        else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryId>)
            return core::SctNavigationTarget{core::SctNavigationKind::FooterEntry, id.value()};
        else return core::SctNavigationTarget{
            core::SctNavigationKind::OpaqueAttachment, id.value()};
    }, entity);
}

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
    auto* outlinePane = new QWidget(splitter);
    auto* outlineLayout = new QVBoxLayout(outlinePane);
    outlineLayout->setContentsMargins(0, 0, 0, 0);
    outlineTabs_ = new QTabWidget(outlinePane);

    auto* physicalTab = new QWidget(outlineTabs_);
    auto* physicalLayout = new QVBoxLayout(physicalTab);
    physicalLayout->setContentsMargins(0, 0, 0, 0);
    outline_ = new QTreeView(physicalTab);
    outlineModel_ = new SctOutlineModel(outline_);
    outline_->setModel(outlineModel_);
    outline_->setContextMenuPolicy(Qt::CustomContextMenu);
    outline_->header()->setSectionResizeMode(QHeaderView::Interactive);
    outline_->header()->setStretchLastSection(false);
    outline_->header()->resizeSection(0, 420);
    outline_->header()->resizeSection(1, 180);
    physicalLayout->addWidget(outline_);
    outlineTabs_->addTab(physicalTab, tr("Physical"));

    auto* structuredTab = new QWidget(outlineTabs_);
    auto* structuredLayout = new QVBoxLayout(structuredTab);
    structuredLayout->setContentsMargins(0, 0, 0, 0);
    structuredBanner_ = new QLabel(structuredTab);
    structuredBanner_->setWordWrap(true);
    structuredBanner_->hide();
    structuredOutline_ = new QTreeView(structuredTab);
    structuredOutlineModel_ = new SctStructuredOutlineModel(structuredOutline_);
    structuredOutline_->setModel(structuredOutlineModel_);
    structuredOutline_->setContextMenuPolicy(Qt::CustomContextMenu);
    structuredOutline_->header()->setSectionResizeMode(QHeaderView::Interactive);
    structuredOutline_->header()->setStretchLastSection(false);
    structuredOutline_->header()->resizeSection(0, 420);
    structuredOutline_->header()->resizeSection(1, 180);
    structuredLayout->addWidget(structuredBanner_);
    structuredLayout->addWidget(structuredOutline_, 1);
    outlineTabs_->addTab(structuredTab, tr("Semantic"));
    outlineLayout->addWidget(outlineTabs_);

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
    parameterTable_ = new QTreeView(details);
    parameterTableModel_ = new SctParameterTableModel(parameterTable_);
    parameterTable_->setModel(parameterTableModel_);
    parameterTable_->setItemDelegate(new SctParameterItemDelegate(parameterTable_));
    parameterTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    parameterTable_->setEditTriggers(QAbstractItemView::DoubleClicked
        | QAbstractItemView::EditKeyPressed);
    parameterTable_->header()->setSectionResizeMode(QHeaderView::Interactive);
    parameterTable_->header()->setStretchLastSection(false);
    parameterTable_->header()->resizeSection(0, 220);
    parameterTable_->header()->resizeSection(1, 260);
    parameterTable_->header()->resizeSection(2, 360);
    parameterTable_->hide();
    parameterTableModel_->setCommitHandler([this](const auto& row,
            const QString& text) -> std::optional<QString> {
        if (!editingEnabled_ || !parameterCommitHandler_)
            return tr("Parameter editing is currently unavailable.");
        return parameterCommitHandler_(row.site, text.toStdString())
            ? std::nullopt : std::optional{tr(
                "The edit was rejected. See Diagnostics for details.")};
    });
    preview_ = new QTextEdit(details);
    preview_->setReadOnly(true);
    preview_->setPlaceholderText(tr("No visual text preview is available for this entity."));
    preview_->setMaximumHeight(170);
    detailsLayout->addWidget(title_);
    detailsLayout->addWidget(subtitle_);
    detailsLayout->addWidget(properties_, 1);
    detailsLayout->addWidget(parameterTable_, 1);
    detailsLayout->addWidget(preview_);
    splitter->addWidget(outlinePane);
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
    connect(structuredOutline_->selectionModel(), &QItemSelectionModel::currentChanged,
        this, [this](const QModelIndex& current) {
            const auto target = structuredOutlineModel_->target(current);
            if (!target) return;
            currentTarget_ = *target;
            showTarget(*target);
            const auto physical = outlineModel_->indexForTarget(*target);
            if (physical.isValid()) {
                const QSignalBlocker blocker(outline_->selectionModel());
                outline_->setCurrentIndex(physical);
            }
            emit becameActive(QString::fromStdString(locator_.identityKey()));
            emit editContextChanged();
        });
    connect(structuredOutline_, &QTreeView::activated, this,
        [this](const QModelIndex& current) {
            const auto target = structuredOutlineModel_->target(current);
            if (!target) return;
            currentTarget_ = *target;
            showTarget(*target);
        });
    connect(structuredOutline_, &QTreeView::customContextMenuRequested,
        this, [this](const QPoint& position) {
            const auto index = structuredOutline_->indexAt(position);
            const auto target = structuredOutlineModel_->target(index);
            const auto context = structuredOutlineModel_->editContext(index);
            QMenu menu(this);
            auto* showPhysical = menu.addAction(tr("Show in Physical Outline"));
            showPhysical->setEnabled(target.has_value());
            connect(showPhysical, &QAction::triggered, this, [this, target] {
                if (!target) return;
                outlineTabs_->setCurrentIndex(0);
                selectTarget(*target);
            });
            if (context && context->verified && context->controller) {
                menu.addSeparator();
                if (context->regionKind
                    == spice::sct::SctStructuredRegionKind::If
                    || context->regionKind
                        == spice::sct::SctStructuredRegionKind::IfElse) {
                    auto* insertThen = menu.addAction(tr("Insert Instruction into Then..."));
                    connect(insertThen, &QAction::triggered, this,
                        [this, controller = *context->controller] {
                            emit insertIntoStructuredArmRequested(
                                QString::fromStdString(locator_.identityKey()),
                                controller.value(), static_cast<int>(
                                    spice::sct::SctStructuredArmKind::Then));
                        });
                }
                if (context->regionKind
                    == spice::sct::SctStructuredRegionKind::While
                    || context->regionKind
                        == spice::sct::SctStructuredRegionKind::NaturalLoop) {
                    auto* insertBody = menu.addAction(tr("Insert Instruction into Body..."));
                    connect(insertBody, &QAction::triggered, this,
                        [this, controller = *context->controller] {
                            emit insertIntoStructuredArmRequested(
                                QString::fromStdString(locator_.identityKey()),
                                controller.value(), static_cast<int>(
                                    spice::sct::SctStructuredArmKind::LoopBody));
                        });
                }
                if (context->regionKind
                    == spice::sct::SctStructuredRegionKind::If) {
                    auto* addElse = menu.addAction(tr("Add Empty Else"));
                    connect(addElse, &QAction::triggered, this,
                        [this, controller = *context->controller] {
                            emit addElseRequested(
                                QString::fromStdString(locator_.identityKey()),
                                controller.value());
                        });
                }
                if (context->regionKind
                    == spice::sct::SctStructuredRegionKind::Switch) {
                    auto* addCase = menu.addAction(tr("Add Empty Case"));
                    connect(addCase, &QAction::triggered, this,
                        [this, controller = *context->controller] {
                            emit addCaseRequested(
                                QString::fromStdString(locator_.identityKey()),
                                controller.value());
                        });
                }
            }
            if (context && context->authoredArm) {
                menu.addSeparator();
                if (context->armKind
                    == spice::sct::SctStructuredArmKind::SwitchCase) {
                    auto* setValue = menu.addAction(tr("Set Case Value..."));
                    connect(setValue, &QAction::triggered, this,
                        [this, arm = *context->authoredArm] {
                            emit setCaseValueRequested(
                                QString::fromStdString(locator_.identityKey()), arm.value);
                        });
                }
                auto* insert = menu.addAction(tr("Insert First Instruction..."));
                insert->setEnabled(context->virtualArm && !context->needsValue);
                connect(insert, &QAction::triggered, this,
                    [this, arm = *context->authoredArm] {
                        emit insertIntoSemanticArmRequested(
                            QString::fromStdString(locator_.identityKey()), arm.value);
                    });
                auto* remove = menu.addAction(tr("Remove Empty Arm"));
                remove->setEnabled(context->virtualArm);
                connect(remove, &QAction::triggered, this,
                    [this, arm = *context->authoredArm] {
                        emit removeSemanticArmRequested(
                            QString::fromStdString(locator_.identityKey()), arm.value);
                    });
                if (context->canReturnToEmpty && target
                    && target->kind == core::SctNavigationKind::Instruction) {
                    auto* returnToEmpty = menu.addAction(tr("Delete and Keep Empty Arm"));
                    connect(returnToEmpty, &QAction::triggered, this,
                        [this, arm = *context->authoredArm, instruction = target->id] {
                            emit returnSemanticArmToEmptyRequested(
                                QString::fromStdString(locator_.identityKey()),
                                arm.value, instruction);
                        });
                }
            }
            if (context && !context->authoredArm && context->verified
                && context->controller && context->armKind
                && *context->armKind != spice::sct::SctStructuredArmKind::Then
                && *context->armKind != spice::sct::SctStructuredArmKind::LoopBody) {
                auto* insert = menu.addAction(tr("Insert Instruction into Arm..."));
                connect(insert, &QAction::triggered, this,
                    [this, controller = *context->controller, arm = *context->armKind] {
                        emit insertIntoStructuredArmRequested(
                            QString::fromStdString(locator_.identityKey()),
                            controller.value(), static_cast<int>(arm));
                    });
            }
            menu.exec(structuredOutline_->viewport()->mapToGlobal(position));
        });
    connect(parameterTable_, &QTreeView::activated, this,
        [this](const QModelIndex& index) {
            const auto* row = parameterTableModel_->parameter(index);
            if (row == nullptr) return;
            if (row->navigation) {
                emit parameterNavigationRequested(
                    QString::fromStdString(locator_.identityKey()),
                    static_cast<int>(row->navigation->kind), row->navigation->id);
            } else if (row->editor == core::SctInlineParameterEditorKind::AdvancedScpt) {
                emit advancedScptRequested(
                    QString::fromStdString(locator_.identityKey()),
                    row->site.instruction.value(), row->site.parameter.schemaIndex,
                    row->site.parameter.repeatedGroupOrdinal
                        ? static_cast<int>(*row->site.parameter.repeatedGroupOrdinal) : -1);
            }
        });
    connect(parameterTable_, &QTreeView::customContextMenuRequested, this,
        [this](const QPoint& position) {
            const auto index = parameterTable_->indexAt(position);
            const auto* row = parameterTableModel_->parameter(index);
            const auto group = parameterTableModel_->groupOrdinal(index);
            const auto& presentation = parameterTableModel_->presentation();
            QMenu menu(this);
            if (row != nullptr && row->navigation) {
                auto* open = menu.addAction(row->navigation->kind
                        == core::SctNavigationKind::Instruction
                    ? tr("Go to Target") : tr("Open in Text Editor"));
                connect(open, &QAction::triggered, this, [this, navigation = *row->navigation] {
                    emit parameterNavigationRequested(
                        QString::fromStdString(locator_.identityKey()),
                        static_cast<int>(navigation.kind), navigation.id);
                });
            }
            if (row != nullptr
                && row->editor == core::SctInlineParameterEditorKind::Reference) {
                auto* changeReference = menu.addAction(
                    row->value.starts_with("0x")
                        ? tr("Repair Reference...") : tr("Change Reference..."));
                changeReference->setEnabled(editingEnabled_);
                connect(changeReference, &QAction::triggered, this,
                    [this, site = row->site] {
                        emit changeParameterReferenceRequested(
                            QString::fromStdString(locator_.identityKey()),
                            site.instruction.value(), site.parameter.schemaIndex,
                            site.parameter.repeatedGroupOrdinal
                                ? static_cast<int>(*site.parameter.repeatedGroupOrdinal) : -1);
                    });
            }
            if (row != nullptr && row->replacementEditor) {
                auto* replace = menu.addAction(tr("Replace With Typed Value..."));
                replace->setEnabled(editingEnabled_);
                connect(replace, &QAction::triggered, this,
                    [this, site = row->site, kind = *row->replacementEditor] {
                        emit replaceOpaqueParameterRequested(
                            QString::fromStdString(locator_.identityKey()),
                            site.instruction.value(), site.parameter.schemaIndex,
                            site.parameter.repeatedGroupOrdinal
                                ? static_cast<int>(*site.parameter.repeatedGroupOrdinal) : -1,
                            static_cast<int>(kind));
                    });
            }
            if (row != nullptr
                && (row->editor == core::SctInlineParameterEditorKind::AdvancedScpt
                    || row->editor == core::SctInlineParameterEditorKind::ConventionalScpt)) {
                auto* advanced = menu.addAction(tr("Open Advanced SCPT Editor"));
                advanced->setEnabled(true);
                connect(advanced, &QAction::triggered, this, [this, site = row->site] {
                    emit advancedScptRequested(
                        QString::fromStdString(locator_.identityKey()),
                        site.instruction.value(), site.parameter.schemaIndex,
                        site.parameter.repeatedGroupOrdinal
                            ? static_cast<int>(*site.parameter.repeatedGroupOrdinal) : -1);
                });
            }
            if (presentation.supportsRepeatedGroups) {
                if (!menu.isEmpty()) menu.addSeparator();
                const auto insertion = group.value_or(
                    static_cast<std::uint32_t>(presentation.repeatedGroups.size()));
                auto* addAbove = menu.addAction(tr("Add Group Above"));
                auto* addBelow = menu.addAction(tr("Add Group Below"));
                addAbove->setEnabled(editingEnabled_
                    && !presentation.repeatedGroupsManagedBySemanticEditor);
                addBelow->setEnabled(addAbove->isEnabled());
                connect(addAbove, &QAction::triggered, this,
                    [this, instruction = presentation.instruction, insertion] {
                        emit addRepeatedGroupRequested(
                            QString::fromStdString(locator_.identityKey()),
                            instruction.value(), insertion);
                    });
                connect(addBelow, &QAction::triggered, this,
                    [this, instruction = presentation.instruction, insertion, group] {
                        emit addRepeatedGroupRequested(
                            QString::fromStdString(locator_.identityKey()),
                            instruction.value(), insertion + (group ? 1u : 0u));
                    });
                if (group) {
                    auto* remove = menu.addAction(tr("Remove Group"));
                    auto* moveUp = menu.addAction(tr("Move Group Up"));
                    auto* moveDown = menu.addAction(tr("Move Group Down"));
                    const bool manageable = editingEnabled_
                        && !presentation.repeatedGroupsManagedBySemanticEditor;
                    remove->setEnabled(manageable
                        && presentation.repeatedGroups.size()
                            > presentation.minimumRepeatedGroups);
                    moveUp->setEnabled(manageable && *group > 0u);
                    moveDown->setEnabled(manageable
                        && *group + 1u < presentation.repeatedGroups.size());
                    connect(remove, &QAction::triggered, this,
                        [this, instruction = presentation.instruction, ordinal = *group] {
                            emit deleteRepeatedGroupRequested(
                                QString::fromStdString(locator_.identityKey()),
                                instruction.value(), ordinal);
                        });
                    connect(moveUp, &QAction::triggered, this,
                        [this, instruction = presentation.instruction, ordinal = *group] {
                            emit moveRepeatedGroupRequested(
                                QString::fromStdString(locator_.identityKey()),
                                instruction.value(), ordinal,
                                static_cast<int>(core::SctRepeatedGroupMoveDirection::Up));
                        });
                    connect(moveDown, &QAction::triggered, this,
                        [this, instruction = presentation.instruction, ordinal = *group] {
                            emit moveRepeatedGroupRequested(
                                QString::fromStdString(locator_.identityKey()),
                                instruction.value(), ordinal,
                                static_cast<int>(core::SctRepeatedGroupMoveDirection::Down));
                        });
                }
            }
            if (!menu.isEmpty()) menu.exec(parameterTable_->viewport()->mapToGlobal(position));
        });
    connect(outlineTabs_, &QTabWidget::currentChanged, this, [this](const int index) {
        if (!currentTarget_) {
            emit editContextChanged();
            return;
        }
        auto* view = index == 0 ? outline_ : structuredOutline_;
        const auto found = index == 0
            ? outlineModel_->indexForTarget(*currentTarget_)
            : structuredOutlineModel_->indexForTarget(*currentTarget_);
        if (found.isValid()) {
            expandAncestors(*view, found);
            view->setCurrentIndex(found);
            view->scrollTo(found);
        }
        emit editContextChanged();
    });
    connect(outline_, &QTreeView::activated, this,
        [this](const QModelIndex& modelIndex) {
            if (!modelIndex.isValid() || !snapshot_) return;
            const auto target = *outlineModel_->target(modelIndex);
            if (target.kind == core::SctNavigationKind::OpaqueAttachment) {
                const auto* attachment = index_
                    ? index_->find(*snapshot_->document,
                        spice::sct::SctOpaqueAttachmentId(target.id))
                    : nullptr;
                if (attachment == nullptr) return;
                if (const auto* direct = std::get_if<spice::sct::SctInstructionId>(
                        &attachment->anchor)) {
                    selectTarget({core::SctNavigationKind::Instruction, direct->value()});
                    return;
                }
                const auto* context = snapshot_->analysis->opaqueContext.find(attachment->id);
                if (context != nullptr && !context->crossingImportedEdges.empty()) {
                    const auto source = context->crossingImportedEdges.front().sourceInstruction;
                    if (index_->find(*snapshot_->document, source) != nullptr) {
                        selectTarget({core::SctNavigationKind::Instruction, source.value()});
                        return;
                    }
                }
                if (context != nullptr && context->nextSemanticEntity) {
                    const auto imported = spice::sct::SctImportedSourceTarget{
                        *context->nextSemanticEntity};
                    const auto* status = snapshot_->analysis->importedSites
                        ? snapshot_->analysis->importedSites->find(imported) : nullptr;
                    const auto next = navigationForEntity(*context->nextSemanticEntity);
                    if ((status == nullptr || status->addressability
                            != spice::sct::SctImportedSiteAddressability::MissingEntity)
                        && next) {
                        selectTarget(*next);
                        return;
                    }
                }
                if (context != nullptr && context->previousSemanticEntity) {
                    const auto imported = spice::sct::SctImportedSourceTarget{
                        *context->previousSemanticEntity};
                    const auto* status = snapshot_->analysis->importedSites
                        ? snapshot_->analysis->importedSites->find(imported) : nullptr;
                    const auto previous = navigationForEntity(
                        *context->previousSemanticEntity);
                    if ((status == nullptr || status->addressability
                            != spice::sct::SctImportedSiteAddressability::MissingEntity)
                        && previous) {
                        selectTarget(*previous);
                        return;
                    }
                }
                selectTarget(core::navigationTargetForOpaqueAnchor(attachment->anchor));
                return;
            }
            if (editingEnabled_ && selectedTextTarget().has_value())
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
        auto* editMessage = menu.addAction(tr("Edit Text"));
        editMessage->setEnabled(canEditSelectedMessage());
        auto* createScript = menu.addAction(tr("New Script Section..."));
        auto* createString = menu.addAction(tr("New Indexed String..."));
        auto* renameSection = menu.addAction(tr("Rename Section..."));
        auto* deleteSection = menu.addAction(tr("Delete Script Section"));
        auto* moveSectionUp = menu.addAction(tr("Move Section Up"));
        auto* moveSectionDown = menu.addAction(tr("Move Section Down"));
        auto* createFooterMessage = menu.addAction(tr("New Footer Message"));
        auto* deleteText = menu.addAction(tr("Delete Text Entity"));
        const bool hasSection = selectedSection().has_value();
        const bool hasText = selectedTextTarget().has_value();
        createScript->setEnabled(editingEnabled_);
        createString->setEnabled(editingEnabled_);
        renameSection->setEnabled(editingEnabled_ && hasSection);
        deleteSection->setEnabled(editingEnabled_ && hasSection);
        moveSectionUp->setEnabled(editingEnabled_ && hasSection);
        moveSectionDown->setEnabled(editingEnabled_ && hasSection);
        createFooterMessage->setEnabled(editingEnabled_);
        deleteText->setEnabled(editingEnabled_ && hasText);
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
        connect(createScript, &QAction::triggered, this, [this]() {
            emit createScriptSectionRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(createString, &QAction::triggered, this, [this]() {
            emit createIndexedStringRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(renameSection, &QAction::triggered, this, [this]() {
            emit renameSectionRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(deleteSection, &QAction::triggered, this, [this]() {
            emit deleteSectionRequested(QString::fromStdString(locator_.identityKey()));
        });
        connect(moveSectionUp, &QAction::triggered, this, [this]() {
            emit moveSectionRequested(QString::fromStdString(locator_.identityKey()),
                static_cast<int>(core::SctSectionMoveDirection::Up));
        });
        connect(moveSectionDown, &QAction::triggered, this, [this]() {
            emit moveSectionRequested(QString::fromStdString(locator_.identityKey()),
                static_cast<int>(core::SctSectionMoveDirection::Down));
        });
        connect(createFooterMessage, &QAction::triggered, this, [this]() {
            emit createFooterTextRequested(QString::fromStdString(locator_.identityKey()),
                static_cast<int>(core::SctCreatedFooterTextKind::Message));
        });
        connect(deleteText, &QAction::triggered, this, [this]() {
            emit deleteTextRequested(QString::fromStdString(locator_.identityKey()));
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

void SctDocumentWidget::setParameterPresentationProvider(
    ParameterPresentationProvider provider) {
    parameterPresentationProvider_ = std::move(provider);
}

void SctDocumentWidget::setParameterCommitHandler(
    ParameterCommitHandler handler) {
    parameterCommitHandler_ = std::move(handler);
}

void SctDocumentWidget::setSnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus) {
    snapshot_ = std::move(snapshot);
    index_ = snapshot_ && snapshot_->analysis ? &snapshot_->analysis->entities : nullptr;
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
    } else {
        using Status = spice::sct::SctSourceTextRecommendationStatus;
        switch (assessment.recommendation.status) {
        case Status::Ambiguous:
            conventionBanner_->setText(tr("Indexed text supports multiple conventions. Text remains opaque until you choose one."));
            break;
        case Status::Conflicting:
            conventionBanner_->setText(tr("Indexed text records conflict about their encoding. Text remains opaque until you choose an interpretation."));
            break;
        case Status::InsufficientEvidence:
            conventionBanner_->setText(tr("There is not enough indexed-text evidence to recommend an encoding. Text remains opaque until you choose one."));
            break;
        case Status::Unique:
            conventionBanner_->setText(tr("The recommended text convention could not be applied automatically."));
            break;
        }
    }
    rebuildOutline();
    rebuildStructuredOutline(true);
}

void SctDocumentWidget::installVerifiedSnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus) {
    snapshot_ = std::move(snapshot);
    index_ = snapshot_ && snapshot_->analysis ? &snapshot_->analysis->entities : nullptr;
    updateSourceBanner(sourceStatus);
    if (outlineReconciliationPending_) {
        rebuildOutline();
        outlineReconciliationPending_ = false;
    }
    structuredOutlinePending_ = false;
    structuredBanner_->hide();
    structuredOutline_->setEnabled(true);
    rebuildStructuredOutline(false);
    if (currentTarget_.has_value()) showTarget(*currentTarget_);
}

void SctDocumentWidget::applyTextOnlySnapshot(
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
    const int sourceStatus,
    const core::SctEditChangeSet& changes) {
    Q_UNUSED(snapshot);
    updateSourceBanner(sourceStatus);
    if (core::hasInvalidation(changes.invalidations,
            core::SctDerivedAnalysisInvalidation::StructuredControlFlow)) {
        markStructuredOutlinePending();
    }
    if (currentTarget_ && currentTarget_->kind == core::SctNavigationKind::Instruction
        && !changes.textValues.empty()) {
        showParameterTable(spice::sct::SctInstructionId(currentTarget_->id), &changes);
    }
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
    if (changes.sections.empty() && changes.instructions.empty()
        && changes.footerEntries.empty()) return false;
    const auto retained = currentTarget_;
    updateSourceBanner(sourceStatus);
    if (!outlineModel_->apply(changes)) {
        outlineReconciliationPending_ = true;
        return false;
    }
    if (core::hasInvalidation(changes.invalidations,
            core::SctDerivedAnalysisInvalidation::StructuredControlFlow)) {
        markStructuredOutlinePending();
    }

    const auto selected = retained.has_value()
        ? outlineModel_->indexForTarget(*retained) : QModelIndex{};
    if (selected.isValid()) {
        currentTarget_ = *retained;
        outline_->setCurrentIndex(selected);
        if (currentTarget_->kind == core::SctNavigationKind::Instruction
            && std::ranges::any_of(changes.instructions, [this](const auto& change) {
                return change.instruction.value() == currentTarget_->id;
            })) {
            showParameterTable(
                spice::sct::SctInstructionId(currentTarget_->id), &changes);
        }
    } else {
        currentTarget_.reset();
        title_->clear();
        subtitle_->clear();
        propertyLocations_.clear();
        properties_->clear();
        preview_->clear();
        preview_->hide();
        parameterTable_->hide();
        properties_->show();
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
    // Diagnostic, property, and semantic navigation always reveal the
    // authoritative physical representation.
    outlineTabs_->setCurrentIndex(0);
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

void SctDocumentWidget::setSemanticProjection(
    std::shared_ptr<const core::SctSemanticEditorProjection> projection) {
    semanticProjection_ = std::move(projection);
    rebuildStructuredOutline(false);
}

void SctDocumentWidget::setStructuredDeveloperOptions(
    const bool showBasicBlocks, const bool showRejectedEvidence,
    const bool showControlFlowInstructions) {
    showStructuredBasicBlocks_ = showBasicBlocks;
    showRejectedStructureEvidence_ = showRejectedEvidence;
    showSemanticControlFlowInstructions_ = showControlFlowInstructions;
    structuredOutlineModel_->setDeveloperOptions(
        showBasicBlocks, showRejectedEvidence, showControlFlowInstructions);
    structuredOutline_->collapseAll();
}

std::optional<core::SctNavigationTarget> SctDocumentWidget::currentTarget() const noexcept {
    return currentTarget_;
}

std::optional<SctDocumentWidget::InstructionInsertionContext>
SctDocumentWidget::insertionContext() const {
    if (outlineTabs_->currentIndex() != 0) return std::nullopt;
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
    if (outlineTabs_->currentIndex() != 0) return false;
    const auto instruction = selectedInstruction();
    if (!instruction.has_value()) return false;
    const auto* existing = outlineModel_->instruction(*instruction);
    return existing != nullptr && !(existing->opcode == 9u
        && !outlineModel_->previousInstruction(*instruction).has_value());
}

std::optional<spice::sct::SctInstructionId> SctDocumentWidget::selectedInstruction() const {
    if (outlineTabs_->currentIndex() != 0) return std::nullopt;
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
        const auto* string = index.find(*snapshot_->document, id);
        if (string != nullptr && std::holds_alternative<spice::sct::SctMessage>(string->value))
            return core::SctMessageTarget{id};
    } else if (currentTarget_->kind == core::SctNavigationKind::FooterEntry) {
        const auto id = spice::sct::SctFooterEntryId(currentTarget_->id);
        const auto* entry = index.find(*snapshot_->document, id);
        if (entry != nullptr && std::holds_alternative<spice::sct::SctMessage>(entry->value))
            return core::SctMessageTarget{id};
    }
    return std::nullopt;
}

std::optional<core::SctTextTarget> SctDocumentWidget::selectedTextTarget() const {
    if (!currentTarget_) return std::nullopt;
    if (currentTarget_->kind == core::SctNavigationKind::String)
        return core::SctTextTarget{spice::sct::SctStringId(currentTarget_->id)};
    if (currentTarget_->kind == core::SctNavigationKind::FooterEntry)
        return core::SctTextTarget{spice::sct::SctFooterEntryId(currentTarget_->id)};
    return std::nullopt;
}

std::optional<spice::sct::SctSectionId> SctDocumentWidget::selectedSection() const {
    if (!currentTarget_ || currentTarget_->kind != core::SctNavigationKind::Section)
        return std::nullopt;
    return spice::sct::SctSectionId(currentTarget_->id);
}

bool SctDocumentWidget::canEditSelectedMessage() const {
    return editingEnabled_ && selectedTextTarget().has_value();
}

bool SctDocumentWidget::canMoveSelected(const core::SctInstructionMoveDirection direction) const {
    if (outlineTabs_->currentIndex() != 0) return false;
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

void SctDocumentWidget::rebuildStructuredOutline(const bool initialLoad) {
    const auto retained = currentTarget_;
    structuredOutlineModel_->setDeveloperOptions(
        showStructuredBasicBlocks_, showRejectedStructureEvidence_,
        showSemanticControlFlowInstructions_);
    structuredOutlineModel_->resetFrom(snapshot_, semanticProjection_);
    if (initialLoad) structuredOutline_->collapseAll();
    if (!retained) return;
    const auto selected = structuredOutlineModel_->indexForTarget(*retained);
    if (!selected.isValid()) return;
    structuredOutline_->setCurrentIndex(selected);
    if (!initialLoad) expandAncestors(*structuredOutline_, selected);
}

void SctDocumentWidget::markStructuredOutlinePending() {
    structuredOutlinePending_ = true;
    structuredBanner_->setText(tr(
        "Semantic structure is updating. Verified regions remain available while the latest document revision is checked."));
    structuredBanner_->show();
    structuredOutline_->setEnabled(true);
}

void SctDocumentWidget::showTarget(const core::SctNavigationTarget target) {
    if (!snapshot_) return;
    if (target.kind == core::SctNavigationKind::Instruction
        && (!index_
            || index_->find(*snapshot_->document,
                spice::sct::SctInstructionId(target.id)) == nullptr)) {
        const auto* instruction = outlineModel_->instruction(
            spice::sct::SctInstructionId(target.id));
        if (instruction != nullptr) {
            title_->setText(tr("Instruction %1").arg(target.id));
            subtitle_->setText(tr("Pending background verification"));
            propertyLocations_.clear();
            properties_->clear();
            properties_->show();
            auto* opcode = new QTreeWidgetItem(properties_);
            opcode->setText(0, tr("Opcode"));
            opcode->setText(1, QString::number(instruction->opcode));
            parameterTable_->show();
            showParameterTable(instruction->id);
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
    const bool instruction = target.kind == core::SctNavigationKind::Instruction;
    for (const auto& property : presentation.properties) {
        if (instruction && (property.name == "Fixed parameters"
                || property.name == "Repeated groups")) continue;
        addPropertyItem(nullptr, property);
    }
    properties_->show();
    parameterTable_->setVisible(instruction);
    if (instruction) showParameterTable(
        spice::sct::SctInstructionId(target.id));
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

void SctDocumentWidget::showParameterTable(
    const spice::sct::SctInstructionId instruction,
    const core::SctEditChangeSet* changes) {
    if (!parameterPresentationProvider_) return;
    auto presentation = parameterPresentationProvider_(instruction);
    const core::SctEditChangeSet noChanges;
    if (parameterTableModel_->presentation().instruction == instruction
        && parameterTableModel_->apply(presentation,
            changes == nullptr ? noChanges : *changes)) return;
    parameterTableModel_->resetFrom(std::move(presentation));
    parameterTable_->collapseAll();
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
