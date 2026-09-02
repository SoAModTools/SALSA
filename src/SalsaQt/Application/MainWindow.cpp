#include "Application/MainWindow.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "Sct/SctDocumentController.h"
#include "Sct/SctDocumentWidget.h"
#include "Sct/SctMessageEditorWidget.h"
#include "Sct/SctSemanticNavigatorWidget.h"
#include "Workspace/DiagnosticsModel.h"
#include "Workspace/WorkspaceDetailsWidget.h"
#include "Workspace/WorkspaceModel.h"

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <ranges>
#include <type_traits>

namespace salsa::qt {
namespace {

constexpr int SettingsStateVersion = 1;
constexpr qsizetype MaximumRecentDatasets = 10;

[[nodiscard]] QString normalizedRecentDatasetPath(const QString& path) {
    const auto cleanedInput = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    if (cleanedInput.isEmpty()) return {};
    const QFileInfo info(cleanedInput);
    auto normalized = info.canonicalFilePath();
    if (normalized.isEmpty()) normalized = info.absoluteFilePath();
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
    return QDir::toNativeSeparators(normalized);
}

[[nodiscard]] bool sameDatasetPath(const QString& left, const QString& right) {
    return normalizedRecentDatasetPath(left).compare(
        normalizedRecentDatasetPath(right), Qt::CaseInsensitive) == 0;
}

[[nodiscard]] bool isTextOnlyTransition(const SctDocumentUpdate& update) {
    if (update.kind != SctDocumentUpdateKind::RevisionTransition
        || !update.transition.has_value()) return false;
    const auto& changes = update.transition->changes;
    if (!changes.created.empty() || !changes.removed.empty()
        || !changes.moved.empty() || changes.modified.empty()) return false;
    return std::ranges::all_of(changes.modified, [](const auto target) {
        return target.kind == core::SctNavigationKind::String
            || target.kind == core::SctNavigationKind::FooterEntry;
    });
}

[[nodiscard]] bool affectsMessageTarget(
    const SctDocumentUpdate& update,
    const core::SctMessageTarget& target) {
    if (update.kind == SctDocumentUpdateKind::SourceStatus) return false;
    if (!update.transition.has_value()) return true;
    const auto navigation = std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return core::SctNavigationTarget{core::SctNavigationKind::String, id.value()};
        else
            return core::SctNavigationTarget{core::SctNavigationKind::FooterEntry, id.value()};
    }, target);
    return std::ranges::find(update.transition->changes.modified, navigation)
        != update.transition->changes.modified.end();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    const auto name = core::applicationName();
    setWindowTitle(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    resize(1100, 700);

    buildUi();
    connectWorkspace();
    restoreApplicationSettings();
    syncWorkspace();
    syncDiagnostics();
    syncSemanticNavigator();
    syncActions();
    statusBar()->showMessage(tr("Ready"));
    QTimer::singleShot(0, this, &MainWindow::attemptRestoreDataset);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!flushMessageEditor() || !confirmDiscardAll(tr("exit SALSA"))) {
        event->ignore();
        return;
    }
    saveApplicationSettings();
    controller_->cancel();
    documentController_->cancel();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
    controller_ = new WorkspaceController(this);
    documentController_ = new SctDocumentController(this);
    workspaceModel_ = new WorkspaceModel(this);
    diagnosticsModel_ = new DiagnosticsModel(this);
    details_ = new WorkspaceDetailsWidget(this);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->addTab(details_, tr("Dataset Overview"));
    tabs_->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    setCentralWidget(tabs_);

    projectTree_ = new QTreeView(this);
    projectTree_->setModel(workspaceModel_);
    projectTree_->setAlternatingRowColors(true);
    projectTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    projectTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    projectDock_ = new QDockWidget(tr("Project Explorer"), this);
    projectDock_->setObjectName(QStringLiteral("ProjectExplorerDock"));
    projectDock_->setWidget(projectTree_);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock_);

    diagnosticsView_ = new QTableView(this);
    diagnosticsView_->setModel(diagnosticsModel_);
    diagnosticsView_->setAlternatingRowColors(true);
    diagnosticsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    diagnosticsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    diagnosticsView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    diagnosticsView_->horizontalHeader()->setStretchLastSection(false);
    diagnosticsView_->horizontalHeader()->resizeSection(0, 90);
    diagnosticsView_->horizontalHeader()->resizeSection(1, 180);
    diagnosticsView_->horizontalHeader()->resizeSection(2, 500);
    diagnosticsView_->horizontalHeader()->resizeSection(3, 320);

    diagnosticsDock_ = new QDockWidget(tr("Diagnostics"), this);
    diagnosticsDock_->setObjectName(QStringLiteral("DiagnosticsDock"));
    diagnosticsDock_->setWidget(diagnosticsView_);
    addDockWidget(Qt::BottomDockWidgetArea, diagnosticsDock_);

    semanticNavigator_ = new SctSemanticNavigatorWidget(this);
    semanticNavigatorDock_ = new QDockWidget(tr("Semantic Navigator"), this);
    semanticNavigatorDock_->setObjectName(QStringLiteral("SemanticNavigatorDock"));
    semanticNavigatorDock_->setWidget(semanticNavigator_);
    addDockWidget(Qt::RightDockWidgetArea, semanticNavigatorDock_);

    messageEditor_ = new SctMessageEditorWidget(this);
    messageEditor_->setCommitHandler(
        [this](const core::AssetLocator& locator,
            const core::SctMessageTarget& target,
            const core::SctMessageDraft& draft,
            const core::SctMessageEditKind kind) {
            return documentController_->replaceMessage(locator, target, draft, kind);
        });
    messageEditorDock_ = new QDockWidget(tr("SctMessage Editor"), this);
    messageEditorDock_->setObjectName(QStringLiteral("SctMessageEditorDock"));
    messageEditorDock_->setWidget(messageEditor_);
    addDockWidget(Qt::RightDockWidgetArea, messageEditorDock_);
    tabifyDockWidget(semanticNavigatorDock_, messageEditorDock_);
    semanticNavigatorDock_->raise();
    messageEditorDock_->hide();

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    openAction_ = fileMenu->addAction(tr("&Open Dataset..."));
    openAction_->setShortcut(QKeySequence::Open);
    recentMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    closeWorkspaceAction_ = fileMenu->addAction(tr("&Close Dataset"));
    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);

    auto* projectMenu = menuBar()->addMenu(tr("&Project"));
    refreshAction_ = projectMenu->addAction(tr("&Refresh Dataset"));
    refreshAction_->setShortcut(QKeySequence::Refresh);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = editMenu->addAction(tr("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = editMenu->addAction(tr("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    editMessageAction_ = editMenu->addAction(tr("Edit &Message"));
    editMenu->addSeparator();
    insertInstructionAction_ = editMenu->addAction(tr("&Insert Instruction..."));
    insertInstructionAction_->setShortcut(QKeySequence(Qt::Key_Insert));
    deleteInstructionAction_ = editMenu->addAction(tr("&Delete Instruction"));
    deleteInstructionAction_->setShortcut(QKeySequence::Delete);
    moveInstructionUpAction_ = editMenu->addAction(tr("Move Instruction &Up"));
    moveInstructionUpAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    moveInstructionDownAction_ = editMenu->addAction(tr("Move Instruction &Down"));
    moveInstructionDownAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));

    auto* editToolbar = addToolBar(tr("Editing"));
    editToolbar->setObjectName(QStringLiteral("EditingToolbar"));
    editToolbar->addAction(undoAction_);
    editToolbar->addAction(redoAction_);
    editToolbar->addSeparator();
    editToolbar->addAction(insertInstructionAction_);
    editToolbar->addAction(deleteInstructionAction_);
    editToolbar->addAction(moveInstructionUpAction_);
    editToolbar->addAction(moveInstructionDownAction_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(projectDock_->toggleViewAction());
    viewMenu->addAction(diagnosticsDock_->toggleViewAction());
    viewMenu->addAction(semanticNavigatorDock_->toggleViewAction());
    viewMenu->addAction(messageEditorDock_->toggleViewAction());
    viewMenu->addAction(editToolbar->toggleViewAction());

    progressBar_ = new QProgressBar(this);
    progressBar_->setTextVisible(true);
    progressBar_->setMinimumWidth(180);
    progressBar_->hide();
    statusBar()->addPermanentWidget(progressBar_);

    cancelButton_ = new QToolButton(this);
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->hide();
    statusBar()->addPermanentWidget(cancelButton_);

    connect(openAction_, &QAction::triggered, this, &MainWindow::chooseDataset);
    connect(closeWorkspaceAction_, &QAction::triggered, this, [this]() {
        if (!flushMessageEditor() || !confirmDiscardAll(tr("close the dataset"))) return;
        messageEditor_->clear();
        documentController_->closeAll();
        controller_->closeWorkspace();
        statusBar()->showMessage(tr("Workspace closed."), 5000);
    });
    connect(refreshAction_, &QAction::triggered, this, [this]() {
        if (!flushMessageEditor()) return;
        if (controller_->refresh()) {
            statusBar()->showMessage(tr("Refreshing dataset..."));
        }
    });
    connect(cancelButton_, &QToolButton::clicked, this, [this]() {
        controller_->cancel();
        documentController_->cancel();
    });
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(undoAction_, &QAction::triggered, this, &MainWindow::undoActiveDocument);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::redoActiveDocument);
    connect(editMessageAction_, &QAction::triggered, this, &MainWindow::editSelectedMessage);
    connect(insertInstructionAction_, &QAction::triggered, this, &MainWindow::insertInstruction);
    connect(deleteInstructionAction_, &QAction::triggered, this, &MainWindow::deleteInstruction);
    connect(moveInstructionUpAction_, &QAction::triggered, this, [this]() {
        moveInstruction(core::SctInstructionMoveDirection::Up);
    });
    connect(moveInstructionDownAction_, &QAction::triggered, this, [this]() {
        moveInstruction(core::SctInstructionMoveDirection::Down);
    });

    connect(
        projectTree_->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current) {
            const auto asset = workspaceModel_->assetAt(current);
            controller_->selectAsset(asset.has_value()
                ? std::optional<core::AssetLocator>(asset->locator)
                : std::nullopt);
        });
    connect(projectTree_, &QTreeView::activated, this, [this](const QModelIndex&) {
        activateSelectedAsset();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeDocumentTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        if (!restoringTabAfterCommitFailure_ && messageEditor_->hasBinding()) {
            auto* active = activeDocumentWidget();
            const auto& bound = messageEditor_->boundLocator();
            if (active == nullptr || !bound.has_value() || active->locator() != *bound) {
                if (!flushMessageEditor()) {
                    if (bound.has_value()) {
                        restoringTabAfterCommitFailure_ = true;
                        focusDocument(QString::fromStdString(bound->identityKey()));
                        restoringTabAfterCommitFailure_ = false;
                    }
                    return;
                }
                messageEditor_->clear();
            }
        }
        syncDiagnostics();
        syncSemanticNavigator();
        syncEditActions();
    });
    connect(diagnosticsView_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* row = diagnosticsModel_->rowAt(index.row());
        if (row == nullptr || !row->locator.has_value()) return;
        focusDocument(QString::fromStdString(row->locator->identityKey()));
        if (row->inspectionLocation.has_value()) {
            if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->currentWidget()))
                (void)widget->selectLocation(*row->inspectionLocation);
        }
    });
    connect(semanticNavigator_, &SctSemanticNavigatorWidget::navigationRequested,
        this, [this](const QString& identityKey, const core::SctInspectionLocation location) {
            focusDocument(identityKey);
            if (auto* widget = activeDocumentWidget();
                widget == nullptr || !widget->selectLocation(location)) {
                statusBar()->showMessage(
                    tr("That semantic location is not present in the current document revision."),
                    8000);
            }
        });
    connect(semanticNavigator_, &SctSemanticNavigatorWidget::statusMessageRequested,
        this, [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(messageEditor_, &SctMessageEditorWidget::statusMessageRequested,
        this, [this](const QString& message) { statusBar()->showMessage(message, 8000); });
    connect(messageEditorDock_, &QDockWidget::visibilityChanged, this, [this](const bool visible) {
        if (!visible && !flushMessageEditor()) {
            messageEditorDock_->show();
            messageEditorDock_->raise();
            messageEditor_->focusEditor();
        }
    });
}

void MainWindow::connectWorkspace() {
    connect(controller_, &WorkspaceController::workspaceChanged, this, &MainWindow::syncWorkspace);
    connect(controller_, &WorkspaceController::selectionChanged, this, &MainWindow::syncSelection);
    connect(controller_, &WorkspaceController::diagnosticsChanged, this, &MainWindow::syncDiagnostics);
    connect(controller_, &WorkspaceController::operationStateChanged, this, [this]() {
        const bool busy = controller_->busy();
        progressBar_->setVisible(busy);
        cancelButton_->setVisible(busy);
        if (busy) {
            progressBar_->setRange(0, 0);
            progressBar_->setValue(0);
            statusBar()->showMessage(
                controller_->operation() == WorkspaceController::Operation::Opening
                    ? tr("Inspecting dataset...")
                    : tr("Refreshing dataset..."));
        }
        syncActions();
    });
    connect(
        controller_,
        &WorkspaceController::progressChanged,
        this,
        [this](
            const bool determinate,
            const int completed,
            const int total,
            const QString& currentPath) {
            if (determinate) {
                progressBar_->setRange(0, total);
                progressBar_->setValue(completed);
            } else {
                progressBar_->setRange(0, 0);
            }
            const auto fileName = QFileInfo(currentPath).fileName();
            statusBar()->showMessage(
                determinate
                    ? tr("Hashing %1 (%2 of %3)").arg(fileName).arg(completed).arg(total)
                    : tr("Discovering SCT assets: %1").arg(fileName));
        });
    connect(
        controller_,
        &WorkspaceController::operationCompleted,
        this,
        &MainWindow::handleOperationCompleted);
    connect(
        controller_,
        &WorkspaceController::datasetOpened,
        this,
        &MainWindow::recordRecentDataset);

    connect(documentController_, &SctDocumentController::busyChanged, this, [this]() {
        const bool busy = documentController_->busy();
        if (busy) {
            progressBar_->setRange(0, 0);
            progressBar_->show();
            cancelButton_->show();
            statusBar()->showMessage(tr("Decoding SCT document..."));
        } else if (!controller_->busy()) {
            progressBar_->hide();
            cancelButton_->hide();
        }
        syncActions();
    });
    connect(documentController_, &SctDocumentController::documentChanged,
        this, &MainWindow::syncDocument);
    connect(documentController_, &SctDocumentController::focusRequested,
        this, &MainWindow::focusDocument);
    connect(documentController_, &SctDocumentController::selectionRequested,
        this, [this](const QString& identityKey, const int kind, const qulonglong id) {
            focusDocument(identityKey);
            if (auto* widget = activeDocumentWidget())
                widget->selectTarget(
                    { static_cast<core::SctNavigationKind>(kind), id }, false);
        });
    connect(documentController_, &SctDocumentController::documentClosed,
        this, [this](const QString& identityKey) {
            if (messageEditor_->boundLocator().has_value()
                && QString::fromStdString(messageEditor_->boundLocator()->identityKey()) == identityKey)
                messageEditor_->clear();
            for (int i = 1; i < tabs_->count(); ++i) {
                auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(i));
                if (widget != nullptr
                    && QString::fromStdString(widget->locator().identityKey()) == identityKey) {
                    tabs_->removeTab(i);
                    widget->deleteLater();
                    break;
                }
            }
            rebuildDocumentTabTitles();
            syncDiagnostics();
            syncSemanticNavigator();
        });
    connect(documentController_, &SctDocumentController::operationCompleted,
        this, [this](const QString&, bool success, bool cancelled, const QString& message) {
            if (!controller_->busy()) {
                progressBar_->hide();
                cancelButton_->hide();
            }
            statusBar()->showMessage(message, 8000);
            if (!success && !cancelled) {
                diagnosticsDock_->show();
                diagnosticsDock_->raise();
            }
            syncDiagnostics();
            syncActions();
        });
    connect(documentController_, &SctDocumentController::editCompleted,
        this, [this](const QString&, const bool success, const QString& message) {
            statusBar()->showMessage(message, 8000);
            if (!success) {
                diagnosticsDock_->show();
                diagnosticsDock_->raise();
                syncDiagnostics();
            }
        });
}

void MainWindow::chooseDataset() {
    const auto initial = recentDatasets_.isEmpty() ? QDir::homePath() : recentDatasets_.front();
    const auto root = QFileDialog::getExistingDirectory(
        this,
        tr("Open extracted game-data dataset"),
        initial,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!root.isEmpty()) {
        openDataset(root);
    }
}

void MainWindow::openDataset(const QString& rootPath) {
    if (!flushMessageEditor() || !confirmDiscardAll(tr("open another dataset"))) return;
    if (controller_->openDataset(rootPath)) {
        statusBar()->showMessage(tr("Inspecting dataset..."));
    }
}

void MainWindow::syncWorkspace() {
    const auto* dataset = controller_->dataset();
    const auto* catalog = controller_->catalog();
    if (dataset == nullptr || catalog == nullptr) {
        workspaceModel_->clear();
        details_->clear();
        projectTree_->setEnabled(false);
    } else {
        workspaceModel_->setSnapshot(*catalog);
        details_->setWorkspace(*dataset, *catalog);
        projectTree_->setEnabled(true);
        projectTree_->collapseAll();
        if (controller_->selectedLocator().has_value()) {
            const auto index = workspaceModel_->indexForLocator(*controller_->selectedLocator());
            if (index.isValid()) {
                projectTree_->setCurrentIndex(index);
            }
        }
    }
    syncSelection();
    syncActions();
}

void MainWindow::syncSelection() {
    details_->setSelectedAsset(controller_->selectedAsset());
    if (!controller_->selectedLocator().has_value()) {
        projectTree_->clearSelection();
        projectTree_->setCurrentIndex({});
    }
}

void MainWindow::syncDiagnostics() {
    auto workspaceDiagnostics = controller_->diagnostics();
    workspaceDiagnostics.insert(workspaceDiagnostics.end(),
        documentController_->failureDiagnostics().begin(),
        documentController_->failureDiagnostics().end());
    auto documentDiagnostics = documentController_->failurePipelineDiagnostics();
    if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->currentWidget())) {
        if (const auto snapshot = documentController_->snapshot(widget->locator()))
            documentDiagnostics.insert(documentDiagnostics.end(),
                snapshot->diagnostics.begin(), snapshot->diagnostics.end());
    }
    diagnosticsModel_->setCombinedDiagnostics(workspaceDiagnostics, documentDiagnostics);
}

void MainWindow::syncSemanticNavigator() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) {
        semanticNavigator_->clear();
        return;
    }
    const auto snapshot = documentController_->snapshot(widget->locator());
    if (snapshot == nullptr || snapshot->document == nullptr) {
        semanticNavigator_->clear();
        return;
    }
    semanticNavigator_->setDocument(widget->locator(), *snapshot);
}

void MainWindow::syncActions() {
    const bool busy = controller_->busy() || documentController_->busy();
    openAction_->setEnabled(!busy);
    recentMenu_->setEnabled(!busy && !recentDatasets_.isEmpty());
    closeWorkspaceAction_->setEnabled(controller_->hasWorkspace() || busy);
    refreshAction_->setEnabled(controller_->hasWorkspace() && !busy);
    projectTree_->setEnabled(controller_->hasWorkspace() && !busy);
    messageEditor_->setEnabled(!busy);
    syncEditActions();
}

SctDocumentWidget* MainWindow::activeDocumentWidget() const {
    return qobject_cast<SctDocumentWidget*>(tabs_->currentWidget());
}

void MainWindow::syncEditActions() {
    auto* widget = activeDocumentWidget();
    const bool available = widget != nullptr
        && !controller_->busy() && !documentController_->busy();
    const bool editable = available
        && documentController_->structurallyValid(widget->locator());
    for (int i = 1; i < tabs_->count(); ++i) {
        if (auto* document = qobject_cast<SctDocumentWidget*>(tabs_->widget(i))) {
            document->setEditingEnabled(!controller_->busy() && !documentController_->busy()
                && documentController_->structurallyValid(document->locator()));
        }
    }

    const auto undoDescription = available
        ? documentController_->undoDescription(widget->locator()) : std::nullopt;
    const auto redoDescription = available
        ? documentController_->redoDescription(widget->locator()) : std::nullopt;
    undoAction_->setEnabled(available && documentController_->canUndo(widget->locator()));
    redoAction_->setEnabled(available && documentController_->canRedo(widget->locator()));
    undoAction_->setText(undoDescription.has_value()
        ? tr("Undo %1").arg(QString::fromStdString(*undoDescription)) : tr("Undo"));
    redoAction_->setText(redoDescription.has_value()
        ? tr("Redo %1").arg(QString::fromStdString(*redoDescription)) : tr("Redo"));
    editMessageAction_->setEnabled(editable && widget->canEditSelectedMessage());
    insertInstructionAction_->setEnabled(editable && widget->insertionContext().has_value());
    deleteInstructionAction_->setEnabled(editable && widget->canDeleteSelected());
    moveInstructionUpAction_->setEnabled(editable
        && widget->canMoveSelected(core::SctInstructionMoveDirection::Up));
    moveInstructionDownAction_->setEnabled(editable
        && widget->canMoveSelected(core::SctInstructionMoveDirection::Down));
}

std::optional<std::uint16_t> MainWindow::chooseInsertableOpcode(const bool allowReturn) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Insert Instruction"));
    dialog.resize(460, 520);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Choose an opcode that can be created without parameter input."), &dialog));
    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Filter by opcode or mnemonic"));
    layout->addWidget(filter);
    auto* list = new QListWidget(&dialog);
    for (const auto& choice : core::SctEditSession::insertableOpcodes()) {
        if (choice.opcode == 12u && !allowReturn) continue;
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(choice.opcode, 3, 10, QLatin1Char('0'))
                .arg(QString::fromStdString(choice.mnemonic)), list);
        item->setData(Qt::UserRole, choice.opcode);
    }
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    connect(filter, &QLineEdit::textChanged, &dialog, [list](const QString& text) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(list, &QListWidget::currentItemChanged, &dialog,
        [buttons](QListWidgetItem* current) {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(current != nullptr && !current->isHidden());
        });
    connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem*) {
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (list->count() > 0) list->setCurrentRow(0);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr
        || list->currentItem()->isHidden()) return std::nullopt;
    return static_cast<std::uint16_t>(list->currentItem()->data(Qt::UserRole).toUInt());
}

void MainWindow::insertInstruction() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto context = widget->insertionContext();
    if (!context.has_value()) return;
    const auto opcode = chooseInsertableOpcode(context->allowReturn);
    if (!opcode.has_value()) return;
    (void)documentController_->insertInstructionAfter(
        widget->locator(), context->anchor, *opcode);
}

void MainWindow::deleteInstruction() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto instruction = widget->selectedInstruction();
    if (instruction.has_value())
        (void)documentController_->deleteInstruction(widget->locator(), *instruction);
}

void MainWindow::moveInstruction(const core::SctInstructionMoveDirection direction) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto instruction = widget->selectedInstruction();
    if (instruction.has_value())
        (void)documentController_->moveInstruction(widget->locator(), *instruction, direction);
}

void MainWindow::editSelectedMessage() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr || !widget->canEditSelectedMessage()) return;
    const auto target = widget->selectedMessageTarget();
    const auto snapshot = documentController_->snapshot(widget->locator());
    if (!target.has_value() || snapshot == nullptr) return;
    if (!messageEditor_->isBoundTo(widget->locator(), *target)
        && !messageEditor_->bindMessage(widget->locator(), snapshot, *target)) {
        statusBar()->showMessage(tr("The selected message could not be opened for editing."), 8000);
        return;
    }
    messageEditorDock_->show();
    messageEditorDock_->raise();
    messageEditor_->focusEditor();
}

bool MainWindow::flushMessageEditor() {
    return messageEditor_ == nullptr || messageEditor_->flushPending();
}

void MainWindow::undoActiveDocument() {
    if (!flushMessageEditor()) return;
    if (auto* widget = activeDocumentWidget())
        (void)documentController_->undo(widget->locator());
}

void MainWindow::redoActiveDocument() {
    if (!flushMessageEditor()) return;
    if (auto* widget = activeDocumentWidget())
        (void)documentController_->redo(widget->locator());
}

bool MainWindow::confirmDiscardDocument(
    const core::AssetLocator& locator, const QString& action) {
    if (!documentController_->isDirty(locator)) return true;
    QMessageBox message(this);
    message.setIcon(QMessageBox::Warning);
    message.setWindowTitle(tr("Discard document changes?"));
    message.setText(tr("%1 has uncommitted in-memory changes.")
        .arg(QString::fromStdWString(locator.path().wstring())));
    message.setInformativeText(tr("Discard those changes and %1? This editor slice cannot save them yet.")
        .arg(action));
    message.setStandardButtons(QMessageBox::Discard | QMessageBox::Cancel);
    message.setDefaultButton(QMessageBox::Cancel);
    return message.exec() == QMessageBox::Discard;
}

bool MainWindow::confirmDiscardAll(const QString& action) {
    const auto dirty = documentController_->dirtyLocators();
    if (dirty.empty()) return true;
    QMessageBox message(this);
    message.setIcon(QMessageBox::Warning);
    message.setWindowTitle(tr("Discard document changes?"));
    message.setText(dirty.size() == 1
        ? tr("One open SCT document has uncommitted in-memory changes.")
        : tr("%1 open SCT documents have uncommitted in-memory changes.").arg(dirty.size()));
    message.setInformativeText(tr("Discard all of those changes and %1? This editor slice cannot save them yet.")
        .arg(action));
    message.setStandardButtons(QMessageBox::Discard | QMessageBox::Cancel);
    message.setDefaultButton(QMessageBox::Cancel);
    return message.exec() == QMessageBox::Discard;
}

void MainWindow::activateSelectedAsset() {
    const auto locator = controller_->selectedLocator();
    const auto project = controller_->projectSnapshot();
    if (!locator.has_value() || !project.has_value()
        || controller_->busy() || documentController_->busy()) return;
    if (documentController_->openDocument(*project, *locator))
        statusBar()->showMessage(tr("Decoding %1...")
            .arg(QString::fromStdWString(locator->path().filename().wstring())));
}

void MainWindow::syncDocument(
    const QString& identityKey,
    const SctDocumentUpdate& update) {
    const auto locators = documentController_->openLocators();
    const auto found = std::ranges::find_if(locators, [&identityKey](const auto& locator) {
        return QString::fromStdString(locator.identityKey()) == identityKey;
    });
    if (found == locators.end()) return;
    SctDocumentWidget* widget = nullptr;
    for (int i = 1; i < tabs_->count(); ++i) {
        auto* candidate = qobject_cast<SctDocumentWidget*>(tabs_->widget(i));
        if (candidate != nullptr && candidate->locator() == *found) {
            widget = candidate;
            break;
        }
    }
    const bool createdWidget = widget == nullptr;
    if (createdWidget) {
        widget = new SctDocumentWidget(*found, tabs_);
        tabs_->addTab(widget, QString::fromStdWString(found->path().filename().wstring()));
        connect(widget, &SctDocumentWidget::textConventionRequested, this,
            [this, widget](const QString&, const int convention) {
                if (!flushMessageEditor()
                    || !confirmDiscardDocument(widget->locator(), tr("change text interpretation"))) return;
                messageEditor_->clear();
                (void)documentController_->selectTextConvention(widget->locator(),
                    static_cast<spice::sct::SctKnownTextConvention>(convention));
            });
        connect(widget, &SctDocumentWidget::reloadRequested, this,
            [this, widget](const QString&) {
                if (!flushMessageEditor()
                    || !confirmDiscardDocument(widget->locator(), tr("reload the document"))) return;
                messageEditor_->clear();
                if (const auto project = controller_->projectSnapshot(); project.has_value())
                    (void)documentController_->reloadDocument(*project, widget->locator());
            });
        connect(widget, &SctDocumentWidget::becameActive, this,
            [this](const QString&) { syncDiagnostics(); syncEditActions(); });
        connect(widget, &SctDocumentWidget::editContextChanged,
            this, &MainWindow::syncEditActions);
        connect(widget, &SctDocumentWidget::insertInstructionRequested,
            this, [this](const QString&) { insertInstruction(); });
        connect(widget, &SctDocumentWidget::deleteInstructionRequested,
            this, [this](const QString&) { deleteInstruction(); });
        connect(widget, &SctDocumentWidget::moveInstructionRequested,
            this, [this](const QString&, const int direction) {
                moveInstruction(static_cast<core::SctInstructionMoveDirection>(direction));
            });
        connect(widget, &SctDocumentWidget::editMessageRequested,
            this, [this](const QString&) { editSelectedMessage(); });
    }
    const auto snapshot = update.snapshot != nullptr
        ? update.snapshot : documentController_->snapshot(*found);
    const auto sourceStatus = static_cast<int>(documentController_->sourceStatus(*found));
    const bool textOnly = !createdWidget && isTextOnlyTransition(update);
    if (createdWidget || update.kind == SctDocumentUpdateKind::Replacement
        || (update.kind == SctDocumentUpdateKind::RevisionTransition && !textOnly)) {
        widget->setSnapshot(snapshot, sourceStatus);
    } else if (textOnly) {
        widget->applyTextOnlySnapshot(snapshot, sourceStatus, update.transition->changes);
    } else {
        widget->setSourceStatus(sourceStatus);
    }
    if (!messageEditor_->isCommitting() && messageEditor_->boundLocator().has_value()
        && *messageEditor_->boundLocator() == *found
        && messageEditor_->boundTarget().has_value()
        && affectsMessageTarget(update, *messageEditor_->boundTarget())) {
        (void)messageEditor_->refresh(snapshot);
    }
    rebuildDocumentTabTitles();
    syncDiagnostics();
    if (createdWidget || update.kind == SctDocumentUpdateKind::Replacement
        || (update.kind == SctDocumentUpdateKind::RevisionTransition && !textOnly)) {
        syncSemanticNavigator();
    }
    syncEditActions();
}

void MainWindow::focusDocument(const QString& identityKey) {
    for (int i = 1; i < tabs_->count(); ++i) {
        auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(i));
        if (widget != nullptr
            && QString::fromStdString(widget->locator().identityKey()) == identityKey) {
            tabs_->setCurrentIndex(i);
            return;
        }
    }
}

void MainWindow::closeDocumentTab(const int index) {
    if (index <= 0 || index >= tabs_->count()) return;
    if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
        if (!flushMessageEditor()
            || !confirmDiscardDocument(widget->locator(), tr("close the document"))) return;
        documentController_->closeDocument(widget->locator());
    }
}

void MainWindow::rebuildDocumentTabTitles() {
    std::vector<SctDocumentWidget*> widgets;
    for (int i = 1; i < tabs_->count(); ++i)
        if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(i))) widgets.push_back(widget);
    for (const auto* widget : widgets) {
        const auto path = QString::fromStdWString(widget->locator().path().wstring());
        const auto parts = QDir::fromNativeSeparators(path).split('/', Qt::SkipEmptyParts);
        QString title = parts.isEmpty() ? path : parts.back();
        for (qsizetype depth = 2; depth <= parts.size(); ++depth) {
            const bool collision = std::ranges::any_of(widgets, [widget, depth, &title](const auto* other) {
                if (other == widget) return false;
                const auto otherParts = QDir::fromNativeSeparators(
                    QString::fromStdWString(other->locator().path().wstring())).split('/', Qt::SkipEmptyParts);
                if (otherParts.isEmpty()) return false;
                const auto start = std::max<qsizetype>(0, otherParts.size() - (depth - 1));
                return otherParts.mid(start).join('/') == title;
            });
            if (!collision) break;
            const auto start = std::max<qsizetype>(0, parts.size() - depth);
            title = parts.mid(start).join('/');
        }
        const auto tabIndex = tabs_->indexOf(const_cast<SctDocumentWidget*>(widget));
        if (documentController_->isDirty(widget->locator())) title += QLatin1Char('*');
        tabs_->setTabText(tabIndex, title);
        tabs_->setTabToolTip(tabIndex, path);
    }
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    for (const auto& root : recentDatasets_) {
        auto* action = recentMenu_->addAction(root);
        action->setToolTip(root);
        connect(action, &QAction::triggered, this, [this, root]() {
            openDataset(root);
        });
    }
    recentMenu_->setEnabled(!controller_->busy() && !documentController_->busy()
        && !recentDatasets_.isEmpty());
}

void MainWindow::recordRecentDataset(const QString& canonicalRoot) {
    const auto normalizedRoot = normalizedRecentDatasetPath(canonicalRoot);
    if (normalizedRoot.isEmpty()) return;
    lastDataset_ = normalizedRoot;
    recentDatasets_.erase(
        std::remove_if(
            recentDatasets_.begin(),
            recentDatasets_.end(),
            [&normalizedRoot](const QString& existing) {
                return sameDatasetPath(existing, normalizedRoot);
            }),
        recentDatasets_.end());
    recentDatasets_.push_front(normalizedRoot);
    while (recentDatasets_.size() > MaximumRecentDatasets) {
        recentDatasets_.removeLast();
    }
    QSettings settings{};
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    rebuildRecentMenu();
}

void MainWindow::attemptRestoreDataset() {
    if (lastDataset_.isEmpty() || controller_->busy() || controller_->hasWorkspace()) return;
    restoringDataset_ = true;
    if (controller_->openDataset(lastDataset_)) {
        statusBar()->showMessage(tr("Restoring the previous dataset..."));
    } else {
        restoringDataset_ = false;
    }
}

void MainWindow::restoreApplicationSettings() {
    QSettings settings{};
    const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    const auto state = settings.value(QStringLiteral("window/state")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    if (!state.isEmpty()) {
        restoreState(state, SettingsStateVersion);
    }
    messageEditorDock_->hide();

    const auto stored = settings.value(QStringLiteral("workspace/recentDatasets")).toStringList();
    const auto storedLastDataset = settings.value(
        QStringLiteral("workspace/lastDataset"),
        stored.isEmpty() ? QString{} : stored.front()).toString();
    lastDataset_ = normalizedRecentDatasetPath(storedLastDataset);
    for (const auto& root : stored) {
        const auto normalizedRoot = normalizedRecentDatasetPath(root);
        if (!normalizedRoot.isEmpty() && QFileInfo(normalizedRoot).isDir()
            && !std::ranges::any_of(
                recentDatasets_,
                [&normalizedRoot](const QString& existing) {
                    return sameDatasetPath(existing, normalizedRoot);
                })) {
            recentDatasets_.push_back(normalizedRoot);
            if (recentDatasets_.size() == MaximumRecentDatasets) {
                break;
            }
        }
    }
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    rebuildRecentMenu();
}

void MainWindow::saveApplicationSettings() const {
    QSettings settings{};
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState(SettingsStateVersion));
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
}

void MainWindow::handleOperationCompleted(
    const WorkspaceController::Operation operation,
    const bool success,
    const bool cancelled,
    const QString& message) {
    const bool wasRestoreAttempt = restoringDataset_
        && operation == WorkspaceController::Operation::Opening;
    if (operation == WorkspaceController::Operation::Opening)
        restoringDataset_ = false;
    progressBar_->hide();
    cancelButton_->hide();
    statusBar()->showMessage(message, 8000);
    syncActions();

    if (!success && !cancelled) {
        diagnosticsDock_->show();
        diagnosticsDock_->raise();
        if (operation == WorkspaceController::Operation::Opening && !wasRestoreAttempt) {
            QMessageBox::warning(this, tr("Dataset could not be opened"), message);
        }
    }
    if (success && operation == WorkspaceController::Operation::Opening) {
        documentController_->closeAll();
    }
    if (success && operation == WorkspaceController::Operation::Refreshing) {
        if (const auto* catalog = controller_->catalog())
            documentController_->synchronizeCatalog(*catalog);
    }
}

}  // namespace salsa::qt
