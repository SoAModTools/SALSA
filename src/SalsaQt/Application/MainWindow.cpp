#include "Application/MainWindow.h"
#include "Application/ExclusiveOperationCoordinator.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SalsaCore/Sct/SctParameterAuthoring.h"
#include "Sct/SctDocumentController.h"
#include "Sct/SctDocumentWidget.h"
#include "Sct/SctRebaseDialog.h"
#include "Sct/SctExportDialog.h"
#include "Sct/SctMessageEditorWidget.h"
#include "Sct/SctScptEditorWidget.h"
#include "Sct/SctSemanticNavigatorWidget.h"
#include "Legacy/LegacyConversionDialog.h"
#include "Ui/UiConstants.h"
#include "Workspace/DiagnosticJournalModel.h"
#include "Workspace/DiagnosticsModel.h"
#include "Workspace/WorkspaceDetailsWidget.h"
#include "Workspace/WorkspaceModel.h"
#include "Workspace/WorkspaceOperationController.h"
#include "Workspace/WorkspaceMaintenanceController.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <QAction>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <sstream>

namespace salsa::qt {
namespace {

constexpr int SettingsStateVersion = 1;
constexpr qsizetype MaximumRecentDatasets = 10;

[[nodiscard]] QString draftWord(const std::uint32_t value) {
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

[[nodiscard]] QString draftValueText(
    const spice::sct::SctDocumentParameterValue& value,
    const std::optional<std::uint32_t> managedTerminator = std::nullopt) {
    return std::visit([managedTerminator](const auto& typed) -> QString {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>) {
            return draftWord(typed.value);
        } else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>) {
            const auto projected = core::SctExpressionLanguage::project(typed);
            return projected.text.empty()
                ? QObject::tr("Advanced SCPT program")
                : QString::fromStdString(projected.text);
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctTerminatedWordSequenceValue>) {
            QStringList words;
            auto displayEnd = typed.words.end();
            if (managedTerminator && !typed.words.empty()
                && typed.words.back() == *managedTerminator)
                --displayEnd;
            for (auto word = typed.words.begin(); word != displayEnd; ++word)
                words.push_back(draftWord(*word));
            return words.join(QLatin1Char(' '));
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
            return QObject::tr("Instruction %1").arg(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            return QObject::tr("Indexed string %1").arg(typed.target.value());
        } else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryReference>) {
            return QObject::tr("Footer entry %1").arg(typed.target.value());
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctUnresolvedReferenceValue>) {
            return QObject::tr("Choose a compatible target");
        } else {
            QStringList words;
            for (const auto word : typed.words) words.push_back(draftWord(word));
            return words.join(QLatin1Char(' '));
        }
    }, value);
}

[[nodiscard]] spice::sct::SctDocumentParameterValue referenceValue(
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([](const auto id) -> spice::sct::SctDocumentParameterValue {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return spice::sct::SctInstructionReference{id};
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return spice::sct::SctStringReference{id};
        else
            return spice::sct::SctFooterEntryReference{id};
    }, target);
}

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
    if (!changes.sections.empty() || !changes.instructions.empty()
        || !changes.footerEntries.empty() || changes.textValues.empty()) return false;
    return std::ranges::all_of(changes.modified, [](const auto target) {
        return target.kind == core::SctNavigationKind::String
            || target.kind == core::SctNavigationKind::FooterEntry;
    });
}

[[nodiscard]] bool isIncrementalStructuralTransition(
    const SctDocumentUpdate& update) {
    if (update.kind != SctDocumentUpdateKind::RevisionTransition
        || !update.transition.has_value()) return false;
    const auto& changes = update.transition->changes;
    return !changes.sections.empty() || !changes.instructions.empty()
        || !changes.footerEntries.empty();
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
    : MainWindow(Mode::Application, parent) {}

MainWindow::MainWindow(const Mode mode, QWidget* parent)
    : QMainWindow(parent), mode_(mode) {
    const auto name = core::applicationName();
    setWindowTitle(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    resize(1100, 700);

    buildUi();
    connectWorkspace();
    if (mode_ == Mode::Application) restoreApplicationSettings();
    syncWorkspace();
    syncDiagnostics();
    syncSemanticNavigator();
    syncActions();
    recordActiveNavigation();
    statusBar()->showMessage(tr("Ready"));
    if (mode_ == Mode::Application)
        QTimer::singleShot(0, this, &MainWindow::attemptRestoreDataset);
}

bool MainWindow::installSemanticCandidate(
    const core::AssetLocator& locator,
    std::shared_ptr<const core::SctDocumentSnapshot> provenanceSnapshot,
    const core::SctSemanticState& state) {
    return mode_ == Mode::IsolatedDocumentEditor
        && documentController_->installTransientDocument(
            locator, std::move(provenanceSnapshot), state);
}

std::optional<core::SctSemanticState> MainWindow::captureSemanticCandidate(
    const core::AssetLocator& locator) {
    if (mode_ != Mode::IsolatedDocumentEditor
        || !prepareScptEditor(locator) || !flushMessageEditor()) return std::nullopt;
    return documentController_->semanticState(locator);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (mode_ == Mode::IsolatedDocumentEditor) {
        QMainWindow::closeEvent(event);
        return;
    }
    if (exclusiveOperations_ && exclusiveOperations_->active()) {
        exclusiveOperations_->requestApplicationClose();
        event->ignore();
        return;
    }
    if (!prepareScptEditor() || !flushMessageEditor() || !confirmDiscardAll(
            tr("exit SALSA"), PendingLifecycle::Exit)) {
        event->ignore();
        return;
    }
    saveWorkspaceSession();
    saveApplicationSettings();
    controller_->cancel();
    documentController_->cancel();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
    controller_ = new WorkspaceController(this);
    exclusiveOperations_ = new ExclusiveOperationCoordinator(this);
    documentController_ = new SctDocumentController(this);
    workspaceModel_ = new WorkspaceModel(this);
    diagnosticsModel_ = new DiagnosticsModel(this);
    diagnosticJournalModel_ = new DiagnosticJournalModel(this);
    details_ = new WorkspaceDetailsWidget(this);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->addTab(details_, tr("Dataset Overview"));
    tabs_->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    setCentralWidget(tabs_);

    projectTree_ = new QTreeView(this);
    projectTree_->setModel(workspaceModel_);
    projectTree_->setIndentation(ui::TreeIndentation);
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

    activityLogView_ = new QTableView(this);
    activityLogView_->setModel(diagnosticJournalModel_);
    activityLogView_->setAlternatingRowColors(true);
    activityLogView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    activityLogView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    activityLogView_->horizontalHeader()->setStretchLastSection(false);
    activityLogView_->horizontalHeader()->resizeSection(0, 110);
    activityLogView_->horizontalHeader()->resizeSection(1, 90);
    activityLogView_->horizontalHeader()->resizeSection(2, 100);
    activityLogView_->horizontalHeader()->resizeSection(3, 180);
    activityLogView_->horizontalHeader()->resizeSection(4, 500);
    activityLogView_->horizontalHeader()->resizeSection(5, 320);
    activityLogDock_ = new QDockWidget(tr("Activity Log"), this);
    activityLogDock_->setObjectName(QStringLiteral("ActivityLogDock"));
    activityLogDock_->setWidget(activityLogView_);
    addDockWidget(Qt::BottomDockWidgetArea, activityLogDock_);
    tabifyDockWidget(diagnosticsDock_, activityLogDock_);
    diagnosticsDock_->raise();
    activityLogDock_->hide();

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
    messageEditor_->setPlainTextCommitHandler(
        [this](const core::AssetLocator& locator, const core::SctTextTarget& target,
            std::string utf8) {
            return documentController_->replacePlainText(locator, target, std::move(utf8));
        });
    messageEditor_->setTextValueCommitHandler(
        [this](const core::AssetLocator& locator, const core::SctTextTarget& target,
            spice::sct::SctTextValue value, std::string description,
            std::optional<core::SctTextRepairProvenance> repairProvenance) {
            return documentController_->replaceTextValue(
                locator, target, std::move(value), std::move(description),
                std::move(repairProvenance));
        });
    messageEditorDock_ = new QDockWidget(tr("SCT Text Editor"), this);
    messageEditorDock_->setObjectName(QStringLiteral("SctMessageEditorDock"));
    messageEditorDock_->setWidget(messageEditor_);
    addDockWidget(Qt::RightDockWidgetArea, messageEditorDock_);
    tabifyDockWidget(semanticNavigatorDock_, messageEditorDock_);
    semanticNavigatorDock_->raise();
    messageEditorDock_->hide();

    scptEditor_ = new SctScptEditorWidget(this);
    scptEditor_->setCommitHandler([this](const core::AssetLocator& locator,
            const spice::sct::SctParameterSite& site,
            const spice::sct::SctCanonicalExpression& expression) {
        return documentController_->replaceParameterValue(locator, site, expression);
    });
    scptEditorDock_ = new QDockWidget(tr("Advanced SCPT Editor"), this);
    scptEditorDock_->setObjectName(QStringLiteral("AdvancedScptEditorDock"));
    scptEditorDock_->setWidget(scptEditor_);
    addDockWidget(Qt::RightDockWidgetArea, scptEditorDock_);
    tabifyDockWidget(semanticNavigatorDock_, scptEditorDock_);
    scptEditorDock_->hide();

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    openAction_ = fileMenu->addAction(tr("&Open Dataset..."));
    openAction_->setShortcut(QKeySequence::Open);
    recentMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    convertLegacyProjectAction_ = fileMenu->addAction(
        tr("Convert &Legacy Project to Capsule..."));
    fileMenu->addSeparator();
    saveAction_ = fileMenu->addAction(tr("&Save Document"));
    saveAction_->setShortcut(QKeySequence::Save);
    exportAction_ = fileMenu->addAction(tr("&Export Active SCT..."));
    exportAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    closeWorkspaceAction_ = fileMenu->addAction(tr("&Close Dataset"));
    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);

    auto* projectMenu = menuBar()->addMenu(tr("&Project"));
    associatePatchWorkspaceAction_ = projectMenu->addAction(
        tr("Open or Create &Workspace..."));
    disconnectPatchWorkspaceAction_ = projectMenu->addAction(
        tr("Close Workspace"));
    projectMenu->addSeparator();
    rebasePatchesAction_ = projectMenu->addAction(
        tr("Rebase Stale Patches..."));
    cleanWorkspaceEvidenceAction_ = projectMenu->addAction(
        tr("Clean Workspace Recovery Evidence..."));
    projectMenu->addSeparator();
    refreshAction_ = projectMenu->addAction(tr("&Refresh Dataset"));
    refreshAction_->setShortcut(QKeySequence::Refresh);
    projectMenu->addSeparator();
    createScriptSectionAction_ = projectMenu->addAction(tr("New Script Section..."));
    createIndexedStringAction_ = projectMenu->addAction(tr("New Indexed String..."));
    renameSectionAction_ = projectMenu->addAction(tr("Rename Section..."));
    deleteSectionAction_ = projectMenu->addAction(tr("Delete Script Section"));
    moveSectionUpAction_ = projectMenu->addAction(tr("Move Section Up"));
    moveSectionDownAction_ = projectMenu->addAction(tr("Move Section Down"));
    projectMenu->addSeparator();
    createFooterMessageAction_ = projectMenu->addAction(tr("New Footer Message"));
    deleteTextAction_ = projectMenu->addAction(tr("Delete Text Entity"));

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    navigationBackAction_ = editMenu->addAction(tr("&Back"));
    navigationBackAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    navigationBackAction_->setIcon(QIcon::fromTheme(QStringLiteral("go-previous"),
        style()->standardIcon(QStyle::SP_ArrowBack)));
    navigationForwardAction_ = editMenu->addAction(tr("&Forward"));
    navigationForwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    navigationForwardAction_->setIcon(QIcon::fromTheme(QStringLiteral("go-next"),
        style()->standardIcon(QStyle::SP_ArrowForward)));
    editMenu->addSeparator();
    undoAction_ = editMenu->addAction(tr("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    undoAction_->setIcon(QIcon::fromTheme(QStringLiteral("edit-undo"),
        style()->standardIcon(QStyle::SP_ArrowBack)));
    redoAction_ = editMenu->addAction(tr("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    redoAction_->setIcon(QIcon::fromTheme(QStringLiteral("edit-redo"),
        style()->standardIcon(QStyle::SP_ArrowForward)));
    editMenu->addSeparator();
    editMessageAction_ = editMenu->addAction(tr("Edit &Text"));
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
    editToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    editToolbar->addAction(navigationBackAction_);
    editToolbar->addAction(navigationForwardAction_);
    editToolbar->addSeparator();
    editToolbar->addAction(undoAction_);
    editToolbar->addAction(redoAction_);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(projectDock_->toggleViewAction());
    viewMenu->addAction(diagnosticsDock_->toggleViewAction());
    viewMenu->addAction(activityLogDock_->toggleViewAction());
    viewMenu->addAction(semanticNavigatorDock_->toggleViewAction());
    viewMenu->addAction(messageEditorDock_->toggleViewAction());
    viewMenu->addAction(scptEditorDock_->toggleViewAction());
    viewMenu->addAction(editToolbar->toggleViewAction());

#if defined(_DEBUG)
    developerMenu_ = menuBar()->addMenu(tr("DEV"));
    logSctEditTimingsAction_ = developerMenu_->addAction(
        tr("Log SCT Edit Timings"));
    logSctEditTimingsAction_->setCheckable(true);
    logSctEditTimingsAction_->setStatusTip(
        tr("Log foreground edit and background verification phase timings."));
    connect(logSctEditTimingsAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            editTimingsEnabled_ = enabled;
            documentController_->setEditTimingsEnabled(enabled);
            statusBar()->showMessage(enabled
                ? tr("SCT edit timing logging enabled.")
                : tr("SCT edit timing logging disabled."), 5000);
        });
    developerMenu_->addSeparator();
    showStructuredBasicBlocksAction_ = developerMenu_->addAction(
        tr("Show Semantic Basic Blocks"));
    showStructuredBasicBlocksAction_->setCheckable(true);
    showStructuredBasicBlocksAction_->setStatusTip(
        tr("Show derived basic-block wrapper rows in Semantic views."));
    connect(showStructuredBasicBlocksAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            showStructuredBasicBlocks_ = enabled;
            for (int index = 1; index < tabs_->count(); ++index) {
                if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
                    widget->setStructuredDeveloperOptions(
                        enabled, showRejectedStructureEvidence_,
                        showSemanticControlFlowInstructions_);
                }
            }
        });
    showRejectedStructureEvidenceAction_ = developerMenu_->addAction(
        tr("Show Rejected Structure Evidence"));
    showRejectedStructureEvidenceAction_->setCheckable(true);
    showRejectedStructureEvidenceAction_->setStatusTip(
        tr("Show conservative structure-analysis issue rows and evidence."));
    connect(showRejectedStructureEvidenceAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            showRejectedStructureEvidence_ = enabled;
            for (int index = 1; index < tabs_->count(); ++index) {
                if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
                    widget->setStructuredDeveloperOptions(
                        showStructuredBasicBlocks_, enabled,
                        showSemanticControlFlowInstructions_);
                }
            }
        });
    showSemanticControlFlowInstructionsAction_ = developerMenu_->addAction(
        tr("Show Semantic Control-Flow Instructions"));
    showSemanticControlFlowInstructionsAction_->setCheckable(true);
    showSemanticControlFlowInstructionsAction_->setStatusTip(
        tr("Reveal managed and inferred jump scaffolding in the Semantic view."));
    connect(showSemanticControlFlowInstructionsAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            showSemanticControlFlowInstructions_ = enabled;
            for (int index = 1; index < tabs_->count(); ++index) {
                if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index))) {
                    widget->setStructuredDeveloperOptions(showStructuredBasicBlocks_,
                        showRejectedStructureEvidence_, enabled);
                }
            }
        });
    logStructureAnalysisAction_ = developerMenu_->addAction(
        tr("Log Structure Analysis Timings"));
    logStructureAnalysisAction_->setCheckable(true);
    logStructureAnalysisAction_->setStatusTip(
        tr("Log structured-control-flow timings and result statistics."));
    connect(logStructureAnalysisAction_, &QAction::toggled, this,
        [this](const bool enabled) {
            structureAnalysisTimingsEnabled_ = enabled;
            documentController_->setStructureTimingsEnabled(enabled);
            statusBar()->showMessage(enabled
                ? tr("Structure-analysis timing logging enabled.")
                : tr("Structure-analysis timing logging disabled."), 5000);
        });
#endif

    progressBar_ = new QProgressBar(this);
    progressBar_->setTextVisible(true);
    progressBar_->setMinimumWidth(180);
    progressBar_->hide();
    statusBar()->addPermanentWidget(progressBar_);

    cancelButton_ = new QToolButton(this);
    cancelButton_->setText(tr("Cancel"));
    cancelButton_->hide();
    statusBar()->addPermanentWidget(cancelButton_);

    workspaceSessionSaveTimer_ = new QTimer(this);
    workspaceSessionSaveTimer_->setSingleShot(true);
    workspaceSessionSaveTimer_->setInterval(500);
    connect(workspaceSessionSaveTimer_, &QTimer::timeout,
        this, &MainWindow::saveWorkspaceSession);

    connect(openAction_, &QAction::triggered, this, &MainWindow::chooseDataset);
    connect(convertLegacyProjectAction_, &QAction::triggered,
        this, &MainWindow::convertLegacyProject);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::saveActiveDocument);
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportActiveDocument);
    connect(associatePatchWorkspaceAction_, &QAction::triggered,
        this, &MainWindow::associatePatchWorkspace);
    connect(disconnectPatchWorkspaceAction_, &QAction::triggered,
        this, &MainWindow::disconnectPatchWorkspace);
    connect(rebasePatchesAction_, &QAction::triggered,
        this, &MainWindow::rebaseStalePatches);
    connect(cleanWorkspaceEvidenceAction_, &QAction::triggered,
        this, &MainWindow::cleanWorkspaceEvidence);
    connect(closeWorkspaceAction_, &QAction::triggered, this, [this]() {
        if (!prepareScptEditor() || !flushMessageEditor() || !confirmDiscardAll(
                tr("close the dataset"), PendingLifecycle::CloseDataset)) return;
        scptEditor_->clear();
        messageEditor_->clear();
        saveWorkspaceSession();
        documentController_->closeAll();
        detachPatchWorkspace(false);
        controller_->closeWorkspace();
        statusBar()->showMessage(tr("Dataset closed."), 5000);
    });
    connect(refreshAction_, &QAction::triggered, this, [this]() {
        if (!flushMessageEditor()) return;
        (void)exclusiveOperations_->open(std::make_unique<WorkspaceOperationController>(
            controller_, WorkspaceController::Operation::Refreshing), this);
    });
    connect(cancelButton_, &QToolButton::clicked, this, [this]() {
        controller_->cancel();
        documentController_->cancel();
    });
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(exclusiveOperations_, &ExclusiveOperationCoordinator::activeChanged,
        this, [this] { syncActions(); });
    connect(undoAction_, &QAction::triggered, this, &MainWindow::undoActiveDocument);
    connect(redoAction_, &QAction::triggered, this, &MainWindow::redoActiveDocument);
    connect(navigationBackAction_, &QAction::triggered, this, &MainWindow::navigateBack);
    connect(navigationForwardAction_, &QAction::triggered, this, &MainWindow::navigateForward);
    connect(editMessageAction_, &QAction::triggered, this, &MainWindow::editSelectedMessage);
    connect(createScriptSectionAction_, &QAction::triggered,
        this, &MainWindow::createScriptSection);
    connect(createIndexedStringAction_, &QAction::triggered,
        this, &MainWindow::createIndexedString);
    connect(renameSectionAction_, &QAction::triggered,
        this, &MainWindow::renameSelectedSection);
    connect(deleteSectionAction_, &QAction::triggered,
        this, &MainWindow::deleteSelectedSection);
    connect(moveSectionUpAction_, &QAction::triggered, this, [this]() {
        moveSelectedSection(core::SctSectionMoveDirection::Up);
    });
    connect(moveSectionDownAction_, &QAction::triggered, this, [this]() {
        moveSelectedSection(core::SctSectionMoveDirection::Down);
    });
    connect(createFooterMessageAction_, &QAction::triggered, this, [this]() {
        createFooterText(core::SctCreatedFooterTextKind::Message);
    });
    connect(deleteTextAction_, &QAction::triggered, this, &MainWindow::deleteSelectedText);
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
            scheduleWorkspaceSessionSave();
        });
    connect(projectTree_, &QTreeView::expanded,
        this, [this](const QModelIndex&) { scheduleWorkspaceSessionSave(); });
    connect(projectTree_, &QTreeView::collapsed,
        this, [this](const QModelIndex&) { scheduleWorkspaceSessionSave(); });
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
        recordActiveNavigation();
        scheduleWorkspaceSessionSave();
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved,
        this, [this](int, int) { scheduleWorkspaceSessionSave(); });
    connect(diagnosticsView_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* row = diagnosticsModel_->rowAt(index.row());
        if (row == nullptr || !row->locator.has_value()) return;
        replayingNavigation_ = true;
        focusDocument(QString::fromStdString(row->locator->identityKey()));
        if (row->inspectionLocation.has_value()) {
            if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->currentWidget()))
                (void)widget->selectLocation(*row->inspectionLocation);
        }
        replayingNavigation_ = false;
        recordActiveNavigation();
    });
    connect(semanticNavigator_, &SctSemanticNavigatorWidget::navigationRequested,
        this, [this](const QString& identityKey, const core::SctInspectionLocation location) {
            replayingNavigation_ = true;
            focusDocument(identityKey);
            if (auto* widget = activeDocumentWidget();
                widget == nullptr || !widget->selectLocation(location)) {
                statusBar()->showMessage(
                    tr("That semantic location is not present in the current document revision."),
                    8000);
            }
            replayingNavigation_ = false;
            recordActiveNavigation();
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
        const bool showStatusProgress = busy
            && !(exclusiveOperations_ && exclusiveOperations_->active());
        progressBar_->setVisible(showStatusProgress);
        cancelButton_->setVisible(showStatusProgress);
        if (showStatusProgress) {
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
            if (exclusiveOperations_ && exclusiveOperations_->active()) return;
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
            if (scptEditor_->boundLocator().has_value()
                && QString::fromStdString(scptEditor_->boundLocator()->identityKey()) == identityKey)
                scptEditor_->clear();
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
            pruneNavigationHistory();
            syncDiagnostics();
            syncSemanticNavigator();
            syncActions();
            scheduleWorkspaceSessionSave();
        });
    connect(documentController_, &SctDocumentController::operationCompleted,
        this, [this](const QString& identityKey, bool success, bool cancelled,
                const QString& message) {
            if (!controller_->busy()) {
                progressBar_->hide();
                cancelButton_->hide();
            }
            statusBar()->showMessage(message, 8000);
            if (!success && !cancelled
                && !(exclusiveOperations_ && exclusiveOperations_->active())) {
                diagnosticsDock_->show();
                diagnosticsDock_->raise();
            }
            diagnosticJournalModel_->appendActivity(
                cancelled ? QStringLiteral("SctOperationCancelled")
                    : success ? QStringLiteral("SctOperationCompleted")
                        : QStringLiteral("SctOperationFailed"),
                message, identityKey);
            syncDiagnostics();
            syncActions();
            continueWorkspaceSessionRestore(
                identityKey, success, cancelled, message);
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
    connect(documentController_, &SctDocumentController::checkpointCompleted,
        this, [this](const QString& identityKey, const bool success, const bool cancelled,
            const QString& message) {
            diagnosticJournalModel_->appendActivity(
                cancelled ? QStringLiteral("CheckpointCancelled")
                    : success ? QStringLiteral("CheckpointCompleted")
                        : QStringLiteral("CheckpointFailed"),
                message, identityKey);
            statusBar()->showMessage(message, 8000);
            if (!success && !cancelled) {
                diagnosticsDock_->show();
                diagnosticsDock_->raise();
            }
            rebuildDocumentTabTitles();
            syncDiagnostics();
            syncActions();
            const bool anySaving = std::ranges::any_of(
                documentController_->openLocators(), [this](const auto& locator) {
                    return documentController_->isSaving(locator);
                });
            if (!anySaving && !controller_->busy() && !documentController_->busy()) {
                progressBar_->hide();
                cancelButton_->hide();
            }
            continuePendingLifecycle(identityKey, success, cancelled);
            scheduleWorkspaceSessionSave();
        });
    connect(documentController_, &SctDocumentController::publicationCompleted,
        this, [this](const QString& identityKey, const bool success,
            const bool cancelled, const QString& message, const bool replacedSource) {
            progressBar_->hide();
            cancelButton_->hide();
            statusBar()->showMessage(message, 10000);
            diagnosticJournalModel_->appendActivity(
                cancelled ? QStringLiteral("PublicationCancelled")
                    : success ? QStringLiteral("PublicationCompleted")
                        : QStringLiteral("PublicationFailed"),
                message, identityKey);
            syncDiagnostics();
            syncActions();
            if (!success && !cancelled) {
                diagnosticsDock_->show();
                diagnosticsDock_->raise();
                if (!documentController_->failureDiagnostics().empty()
                    && !(exclusiveOperations_ && exclusiveOperations_->active())) {
                    QMessageBox::warning(this, tr("SCT export failed"), message);
                }
            } else if (success && replacedSource) {
                statusBar()->showMessage(
                    tr("Source SCT replaced. Reload the document before saving another patch checkpoint."),
                    12000);
            }
        });
}

void MainWindow::convertLegacyProject() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    (void)exclusiveOperations_->open(
        std::make_unique<LegacyConversionController>(), this);
}

void MainWindow::chooseDataset() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
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
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    pendingDatasetRoot_ = rootPath;
    if (!prepareScptEditor() || !flushMessageEditor()
        || !confirmDiscardAll(tr("open another dataset"),
            PendingLifecycle::OpenDataset)) return;
    saveWorkspaceSession();
    scptEditor_->clear();
    auto operation = std::make_unique<WorkspaceOperationController>(
        controller_, WorkspaceController::Operation::Opening, rootPath);
    activeDatasetOperation_ = operation.get();
    if (!exclusiveOperations_->open(std::move(operation), this))
        activeDatasetOperation_.clear();
}

void MainWindow::associatePatchWorkspace() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    const auto* dataset = controller_->dataset();
    if (dataset == nullptr || !documentController_->openLocators().empty()) return;
    const auto datasetRoot = QString::fromStdWString(dataset->root.wstring());
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Open or create SALSA workspace"),
        patchWorkspace_ ? QString::fromStdWString(
            patchWorkspace_->descriptor().root.wstring()) : datasetRoot,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;

    const auto path = std::filesystem::path(selected.toStdWString());
    const auto datasetCopy = *dataset;
    auto operation = WorkspaceMaintenanceController::openWorkspace(
        path, datasetCopy, [this, selected](core::LocalSalsaWorkspace opened) {
            saveWorkspaceSession();
            detachPatchWorkspace(false);
            patchWorkspace_ = std::make_shared<const core::LocalSalsaWorkspace>(
                std::move(opened));
            documentController_->setWorkspace(patchWorkspace_);
            const auto* current = controller_->dataset();
            if (current) rememberPatchWorkspaceAssociation(
                QString::fromStdWString(current->root.wstring()), selected);
            syncActions();
            restoreWorkspaceSession();
            QTimer::singleShot(0, this, [this] {
                if (activeWorkspaceMaintenance_ && !restoringWorkspaceSession_)
                    activeWorkspaceMaintenance_->completeRestoration();
            });
            return tr("SALSA workspace opened: %1")
                .arg(QDir::toNativeSeparators(selected));
        });
    activeWorkspaceMaintenance_ = operation.get();
    if (!exclusiveOperations_->open(std::move(operation), this))
        activeWorkspaceMaintenance_.clear();
}

void MainWindow::rebaseStalePatches() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    if (patchWorkspace_ == nullptr || controller_->busy()
        || documentController_->busy() || documentController_->isPublishing()) return;
    if (!prepareScptEditor() || !flushMessageEditor()) return;
    const auto dirty = documentController_->dirtyLocators();
    if (!confirmDiscardAll(tr("rebase stale patches"),
            PendingLifecycle::RebasePatches)) return;
    // A true return with pre-existing dirty documents means the user chose
    // Discard. Close just those sessions so their uncheckpointed state cannot
    // leak into the independently reconstructed rebase inputs.
    for (const auto& locator : dirty) documentController_->closeDocument(locator);

    const auto project = controller_->projectSnapshot();
    if (!project) return;
    (void)exclusiveOperations_->open(std::make_unique<SctRebaseController>(
        *project, patchWorkspace_, [this, project](const auto& assets) {
            bool adoptedEveryOpenDocument = true;
            for (const auto& locator : assets) {
                if (!documentController_->contains(locator)) continue;
                adoptedEveryOpenDocument = documentController_->adoptRebasedDocument(
                    *project, locator) && adoptedEveryOpenDocument;
            }
            scheduleWorkspaceSessionSave();
            syncActions();
            return adoptedEveryOpenDocument
                ? tr("The selected stale patches were rebased.")
                : tr("The selected patches were committed, but one open document could not "
                     "adopt the result. Close and reopen that document to load the durable patch.");
        }), this);
}

void MainWindow::cleanWorkspaceEvidence() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    if (patchWorkspace_ == nullptr) return;
    (void)exclusiveOperations_->open(WorkspaceMaintenanceController::cleanup(
        patchWorkspace_->descriptor().root,
        patchWorkspace_->descriptor().components.transactions,
        [this] { syncActions(); }), this);
}

bool MainWindow::openPatchWorkspace(
    const QString& workspaceRoot, const bool allowConfirmation) {
    const auto* dataset = controller_->dataset();
    if (dataset == nullptr || !documentController_->openLocators().empty()) return false;
    const auto path = std::filesystem::path(workspaceRoot.toStdWString());
    auto assessment = core::LocalSalsaWorkspace::assess(path, *dataset);
    if (!assessment) {
        const auto message = assessment.diagnostics().empty()
            ? tr("The SALSA workspace could not be assessed.")
            : QString::fromStdString(assessment.diagnostics().front().message);
        if (allowConfirmation)
            QMessageBox::warning(this, tr("Workspace could not be opened"), message);
        else statusBar()->showMessage(message, 10000);
        return false;
    }
    auto acceptance = core::WorkspaceDatasetAcceptance::ExactOnly;
    if (assessment.value().requiresReassociation()) {
        if (!allowConfirmation) {
            statusBar()->showMessage(tr(
                "The associated workspace needs explicit dataset reassociation; the dataset remains open."),
                12000);
            return false;
        }
        const auto previous = assessment.value().storedDatasetRoot
            ? QString::fromStdWString(assessment.value().storedDatasetRoot->wstring())
            : tr("Unknown");
        const auto current = QString::fromStdWString(dataset->root.wstring());
        const auto answer = QMessageBox::warning(this, tr("Reassociate workspace?"),
            tr("%1\n\nPrevious dataset:\n%2\n\nSelected dataset:\n%3\n\n"
               "Reassociation updates the workspace binding. Individual SCT patches still "
               "require their recorded source revision to match.")
                .arg(QString::fromStdString(assessment.value().message),
                    QDir::toNativeSeparators(previous),
                    QDir::toNativeSeparators(current)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return false;
        acceptance = core::WorkspaceDatasetAcceptance::UserConfirmedReassociation;
    }
    saveWorkspaceSession();
    auto opened = core::LocalSalsaWorkspace::openOrCreate(path, *dataset, acceptance);
    if (!opened) {
        const auto message = opened.diagnostics().empty()
            ? tr("The SALSA workspace could not be opened.")
            : QString::fromStdString(opened.diagnostics().front().message);
        if (allowConfirmation)
            QMessageBox::warning(this, tr("Workspace could not be opened"), message);
        else statusBar()->showMessage(message, 10000);
        return false;
    }
    detachPatchWorkspace(false);
    patchWorkspace_ = std::make_shared<const core::LocalSalsaWorkspace>(
        std::move(opened).takeValue());
    documentController_->setWorkspace(patchWorkspace_);
    rememberPatchWorkspaceAssociation(
        QString::fromStdWString(dataset->root.wstring()), workspaceRoot);
    statusBar()->showMessage(tr("SALSA workspace opened: %1")
        .arg(QDir::toNativeSeparators(workspaceRoot)), 8000);
    syncActions();
    restoreWorkspaceSession();
    return true;
}

void MainWindow::disconnectPatchWorkspace() {
    if (!documentController_->openLocators().empty()) return;
    detachPatchWorkspace(true);
    statusBar()->showMessage(tr("SALSA workspace disconnected."), 5000);
    syncActions();
}

void MainWindow::detachPatchWorkspace(const bool saveSession) {
    if (saveSession) saveWorkspaceSession();
    if (workspaceSessionSaveTimer_) workspaceSessionSaveTimer_->stop();
    restoringWorkspaceSession_ = false;
    restoringWorkspaceSessionState_.reset();
    restoringWorkspaceDocumentIndex_ = 0;
    workspaceRestoreMessages_.clear();
    documentController_->setWorkspace(nullptr);
    patchWorkspace_.reset();
}

void MainWindow::restorePatchWorkspaceAssociation() {
    documentController_->setWorkspace(nullptr);
    patchWorkspace_.reset();
    const auto* dataset = controller_->dataset();
    if (dataset == nullptr) return;
    const auto datasetRoot = normalizedRecentDatasetPath(
        QString::fromStdWString(dataset->root.wstring()));
    for (qsizetype index = 0; index + 1 < patchWorkspaceAssociations_.size(); index += 2) {
        if (!sameDatasetPath(patchWorkspaceAssociations_[index], datasetRoot)) continue;
        const auto workspaceRoot = patchWorkspaceAssociations_[index + 1];
        const QDir workspaceDirectory(workspaceRoot);
        const bool hasManifest = QFileInfo(workspaceDirectory.filePath(
            QStringLiteral("project.json"))).isFile();
        const bool hasRollback = QFileInfo(workspaceDirectory.filePath(
            QStringLiteral("project.json.schema1.rollback"))).isFile();
        if (!workspaceDirectory.exists()
            || (!hasManifest && !hasRollback)) {
            statusBar()->showMessage(tr(
                "The last associated SALSA workspace is missing; the dataset remains open."),
                10000);
            return;
        }
        (void)openPatchWorkspace(workspaceRoot, false);
        return;
    }
}

void MainWindow::rememberPatchWorkspaceAssociation(
    const QString& datasetRoot, const QString& workspaceRoot) {
    const auto normalizedDataset = normalizedRecentDatasetPath(datasetRoot);
    const auto normalizedWorkspace = normalizedRecentDatasetPath(workspaceRoot);
    for (qsizetype index = patchWorkspaceAssociations_.size() - 2; index >= 0; index -= 2) {
        if (sameDatasetPath(patchWorkspaceAssociations_[index], normalizedDataset)
            && sameDatasetPath(patchWorkspaceAssociations_[index + 1], normalizedWorkspace)) {
            patchWorkspaceAssociations_.removeAt(index + 1);
            patchWorkspaceAssociations_.removeAt(index);
        }
        if (index < 2) break;
    }
    patchWorkspaceAssociations_.prepend(normalizedWorkspace);
    patchWorkspaceAssociations_.prepend(normalizedDataset);
    QSettings{}.setValue(QStringLiteral("workspace/patchAssociations"),
        patchWorkspaceAssociations_);
}

void MainWindow::saveActiveDocument() {
    if (!flushMessageEditor()) return;
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    if (documentController_->saveDocument(widget->locator())) {
        statusBar()->showMessage(tr("Saving SCT patch checkpoint..."));
        progressBar_->setRange(0, 0);
        progressBar_->show();
        cancelButton_->show();
        rebuildDocumentTabTitles();
        syncActions();
    }
}

void MainWindow::exportActiveDocument() {
    if (exclusiveOperations_->active()) { exclusiveOperations_->focusActive(); return; }
    auto* widget = activeDocumentWidget();
    if (widget == nullptr || !prepareScptEditor(widget->locator())
        || !flushMessageEditor()) return;
    const auto project = controller_->projectSnapshot();
    const auto snapshot = documentController_->snapshot(widget->locator());
    if (!project || !snapshot || !snapshot->provenance) return;

    const auto sourcePath = project->dataset().root / widget->locator().path();
    const auto defaults = core::SctPublicationService::defaultsFor(
        project->dataset(), *snapshot);
    lastExportDirectory_ = QSettings{}.value(
        QStringLiteral("publication/lastDirectory"), lastExportDirectory_).toString();
    (void)exclusiveOperations_->open(std::make_unique<SctPublicationController>(
        documentController_, *project, widget->locator(),
        documentController_->workingRevision(widget->locator()), defaults,
        sourcePath, lastExportDirectory_), this);
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
    std::vector<core::SctPipelineDiagnostic> documentDiagnostics;
    if (auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->currentWidget())) {
        for (const auto& failure : documentController_->failurePipelineDiagnostics()) {
            if (!failure.locator || *failure.locator == widget->locator())
                documentDiagnostics.push_back(failure);
        }
        auto current = documentController_->currentDiagnostics(widget->locator());
        documentDiagnostics.insert(documentDiagnostics.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    } else {
        for (const auto& failure : documentController_->failurePipelineDiagnostics())
            if (!failure.locator) documentDiagnostics.push_back(failure);
    }
    diagnosticsModel_->setCombinedDiagnostics(workspaceDiagnostics, documentDiagnostics);

    auto globalDocumentDiagnostics = documentController_->failurePipelineDiagnostics();
    for (const auto& locator : documentController_->openLocators()) {
        auto current = documentController_->currentDiagnostics(locator);
        globalDocumentDiagnostics.insert(globalDocumentDiagnostics.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    }
    diagnosticJournalModel_->observe(DiagnosticsModel::rowsFor(
        workspaceDiagnostics, globalDocumentDiagnostics));
}

void MainWindow::queueDiagnosticsSync() {
    if (diagnosticsSyncPending_) return;
    diagnosticsSyncPending_ = true;
    QTimer::singleShot(0, this, [this] {
        diagnosticsSyncPending_ = false;
        QElapsedTimer timer;
        timer.start();
        syncDiagnostics();
        if (editTimingsEnabled_) {
            qInfo().noquote() << QStringLiteral(
                "SALSA edit timing: diagnostics-delivery=%1us")
                .arg(timer.nsecsElapsed() / 1000);
        }
    });
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
    const bool publishing = documentController_->isPublishing();
    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    const bool hasDataset = controller_->hasWorkspace();
    const bool hasOpenDocuments = !documentController_->openLocators().empty();
    openAction_->setEnabled(!busy && !publishing && !exclusive);
    recentMenu_->setEnabled(!busy && !publishing && !exclusive && !recentDatasets_.isEmpty());
    convertLegacyProjectAction_->setEnabled(!busy && !publishing && !exclusive);
    closeWorkspaceAction_->setEnabled(!exclusive && (hasDataset || busy || publishing));
    associatePatchWorkspaceAction_->setEnabled(
        hasDataset && !busy && !publishing && !exclusive && !hasOpenDocuments);
    disconnectPatchWorkspaceAction_->setEnabled(
        patchWorkspace_ != nullptr && !busy && !publishing && !exclusive && !hasOpenDocuments);
    rebasePatchesAction_->setEnabled(
        patchWorkspace_ != nullptr && hasDataset && !busy && !publishing && !exclusive);
    cleanWorkspaceEvidenceAction_->setEnabled(
        patchWorkspace_ != nullptr && !busy && !publishing && !exclusive);
    refreshAction_->setEnabled(hasDataset && !busy && !publishing && !exclusive);
    projectTree_->setEnabled(hasDataset && !busy && !publishing && !exclusive);
    messageEditor_->setEnabled(!busy && !exclusive);
    syncEditActions();
    if (mode_ == Mode::IsolatedDocumentEditor) {
        openAction_->setEnabled(false);
        recentMenu_->setEnabled(false);
        closeWorkspaceAction_->setEnabled(false);
        associatePatchWorkspaceAction_->setEnabled(false);
        disconnectPatchWorkspaceAction_->setEnabled(false);
        rebasePatchesAction_->setEnabled(false);
        cleanWorkspaceEvidenceAction_->setEnabled(false);
        refreshAction_->setEnabled(false);
        saveAction_->setEnabled(false);
        exportAction_->setEnabled(false);
        projectTree_->setEnabled(false);
    }
}

SctDocumentWidget* MainWindow::activeDocumentWidget() const {
    return qobject_cast<SctDocumentWidget*>(tabs_->currentWidget());
}

void MainWindow::recordActiveNavigation() {
    if (replayingNavigation_) return;
    if (tabs_->currentIndex() == 0) {
        recordNavigation({});
    } else if (auto* widget = activeDocumentWidget(); widget != nullptr) {
        if (const auto target = widget->currentTarget())
            recordNavigation({widget->locator(), *target});
    }
}

void MainWindow::recordNavigation(NavigationEntry entry) {
    if (replayingNavigation_ || !navigationEntryAvailable(entry)) return;
    pruneNavigationHistory();
    if (!navigationHistory_.empty()
        && navigationHistory_[navigationHistoryIndex_] == entry) {
        syncNavigationActions();
        return;
    }
    if (!navigationHistory_.empty()
        && navigationHistoryIndex_ + 1u < navigationHistory_.size()) {
        navigationHistory_.erase(
            navigationHistory_.begin() + static_cast<std::ptrdiff_t>(
                navigationHistoryIndex_ + 1u), navigationHistory_.end());
    }
    navigationHistory_.push_back(std::move(entry));
    navigationHistoryIndex_ = navigationHistory_.size() - 1u;
    syncNavigationActions();
    scheduleWorkspaceSessionSave();
}

bool MainWindow::navigationEntryAvailable(const NavigationEntry& entry) const {
    if (!entry.locator) return !entry.target;
    for (int index = 1; index < tabs_->count(); ++index) {
        const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
        if (widget != nullptr && widget->locator() == *entry.locator)
            return entry.target && widget->containsTarget(*entry.target);
    }
    return false;
}

void MainWindow::pruneNavigationHistory() {
    if (navigationHistory_.empty()) {
        syncNavigationActions();
        return;
    }
    std::vector<NavigationEntry> retained;
    retained.reserve(navigationHistory_.size());
    std::size_t retainedThroughCurrent = 0;
    for (std::size_t index = 0; index < navigationHistory_.size(); ++index) {
        if (!navigationEntryAvailable(navigationHistory_[index])) continue;
        retained.push_back(navigationHistory_[index]);
        if (index <= navigationHistoryIndex_) retainedThroughCurrent = retained.size();
    }
    navigationHistory_ = std::move(retained);
    if (navigationHistory_.empty()) navigationHistoryIndex_ = 0;
    else navigationHistoryIndex_ = retainedThroughCurrent == 0 ? 0
        : std::min(retainedThroughCurrent - 1u, navigationHistory_.size() - 1u);
    syncNavigationActions();
}

bool MainWindow::navigateTo(const NavigationEntry& entry) {
    if (!navigationEntryAvailable(entry)) return false;
    replayingNavigation_ = true;
    if (!entry.locator) {
        tabs_->setCurrentIndex(0);
    } else {
        focusDocument(QString::fromStdString(entry.locator->identityKey()));
        auto* widget = activeDocumentWidget();
        if (widget == nullptr || widget->locator() != *entry.locator
            || !entry.target || !widget->selectLocation(*entry.target)) {
            replayingNavigation_ = false;
            return false;
        }
    }
    replayingNavigation_ = false;
    syncNavigationActions();
    return true;
}

void MainWindow::navigateBack() {
    pruneNavigationHistory();
    while (!navigationHistory_.empty() && navigationHistoryIndex_ > 0u) {
        --navigationHistoryIndex_;
        if (navigateTo(navigationHistory_[navigationHistoryIndex_])) {
            scheduleWorkspaceSessionSave();
            return;
        }
    }
    syncNavigationActions();
}

void MainWindow::navigateForward() {
    pruneNavigationHistory();
    while (!navigationHistory_.empty()
        && navigationHistoryIndex_ + 1u < navigationHistory_.size()) {
        ++navigationHistoryIndex_;
        if (navigateTo(navigationHistory_[navigationHistoryIndex_])) {
            scheduleWorkspaceSessionSave();
            return;
        }
    }
    syncNavigationActions();
}

QString MainWindow::navigationEntryLabel(const NavigationEntry& entry) const {
    if (!entry.locator) return tr("Dataset Overview");
    auto label = QString::fromStdWString(entry.locator->path().filename().wstring());
    if (entry.target) {
        for (int index = 1; index < tabs_->count(); ++index) {
            const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
            if (widget == nullptr || widget->locator() != *entry.locator) continue;
            const auto target = widget->targetLabel(*entry.target);
            label += target.isEmpty() ? tr(" — item %1").arg(entry.target->id)
                                      : QStringLiteral(" — ") + target;
            break;
        }
    }
    return label;
}

void MainWindow::syncNavigationActions() {
    if (navigationBackAction_ == nullptr || navigationForwardAction_ == nullptr) return;
    const bool canBack = !navigationHistory_.empty() && navigationHistoryIndex_ > 0u;
    const bool canForward = !navigationHistory_.empty()
        && navigationHistoryIndex_ + 1u < navigationHistory_.size();
    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    navigationBackAction_->setEnabled(canBack && !exclusive);
    navigationForwardAction_->setEnabled(canForward && !exclusive);
    navigationBackAction_->setToolTip(canBack
        ? tr("Back to %1 (Alt+Left)").arg(
            navigationEntryLabel(navigationHistory_[navigationHistoryIndex_ - 1u]))
        : tr("Back (Alt+Left)"));
    navigationForwardAction_->setToolTip(canForward
        ? tr("Forward to %1 (Alt+Right)").arg(
            navigationEntryLabel(navigationHistory_[navigationHistoryIndex_ + 1u]))
        : tr("Forward (Alt+Right)"));
}

void MainWindow::syncEditActions() {
    auto* widget = activeDocumentWidget();
    const bool exclusive = exclusiveOperations_ && exclusiveOperations_->active();
    const bool available = widget != nullptr
        && !controller_->busy() && !documentController_->busy() && !exclusive;
    const bool editable = available
        && documentController_->structurallyValid(widget->locator());
    saveAction_->setEnabled(available && patchWorkspace_ != nullptr
        && documentController_->isDirty(widget->locator())
        && !documentController_->isSaving(widget->locator())
        && !documentController_->patchConflict(widget->locator())
        && !documentController_->isPublishing()
        && documentController_->sourceStatus(widget->locator())
            == SctDocumentController::SourceStatus::Current);
    exportAction_->setEnabled(editable && !documentController_->isPublishing()
        && !documentController_->isSaving(widget->locator()));
    for (int i = 1; i < tabs_->count(); ++i) {
        if (auto* document = qobject_cast<SctDocumentWidget*>(tabs_->widget(i))) {
            document->setEditingEnabled(!controller_->busy() && !documentController_->busy()
                && !exclusive
                && documentController_->structurallyValid(document->locator()));
        }
    }

    const auto undoDescription = available
        ? documentController_->undoDescription(widget->locator()) : std::nullopt;
    const auto redoDescription = available
        ? documentController_->redoDescription(widget->locator()) : std::nullopt;
    undoAction_->setEnabled(available && documentController_->canUndo(widget->locator()));
    redoAction_->setEnabled(available && documentController_->canRedo(widget->locator()));
    undoAction_->setText(tr("&Undo"));
    redoAction_->setText(tr("&Redo"));
    undoAction_->setToolTip(undoDescription.has_value()
        ? tr("Undo %1").arg(QString::fromStdString(*undoDescription)) : tr("Undo"));
    redoAction_->setToolTip(redoDescription.has_value()
        ? tr("Redo %1").arg(QString::fromStdString(*redoDescription)) : tr("Redo"));
    editMessageAction_->setEnabled(editable && widget->canEditSelectedMessage());
    createScriptSectionAction_->setEnabled(editable);
    createIndexedStringAction_->setEnabled(editable);
    const bool sectionSelected = editable && widget->selectedSection().has_value();
    renameSectionAction_->setEnabled(sectionSelected);
    deleteSectionAction_->setEnabled(sectionSelected);
    moveSectionUpAction_->setEnabled(sectionSelected);
    moveSectionDownAction_->setEnabled(sectionSelected);
    createFooterMessageAction_->setEnabled(editable);
    deleteTextAction_->setEnabled(editable && widget->selectedTextTarget().has_value());
    insertInstructionAction_->setEnabled(editable && widget->insertionContext().has_value());
    deleteInstructionAction_->setEnabled(editable && widget->canDeleteSelected());
    moveInstructionUpAction_->setEnabled(editable
        && widget->canMoveSelected(core::SctInstructionMoveDirection::Up));
    moveInstructionDownAction_->setEnabled(editable
        && widget->canMoveSelected(core::SctInstructionMoveDirection::Down));
    syncNavigationActions();
}

std::optional<core::SctInstructionAuthoringDraft>
MainWindow::chooseInstructionDraft(
    const core::AssetLocator& locator, const bool allowReturn) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Instruction"));
    dialog.resize(460, 520);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Choose an opcode. Required parameters are configured next."), &dialog));
    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(tr("Filter by opcode or mnemonic"));
    layout->addWidget(filter);
    auto* list = new QListWidget(&dialog);
    int firstEnabled = -1;
    for (const auto& schema : spice::sct::sctOpcodeSchemas()) {
        if (schema.opcode == 9u
            || schema.documentRole == spice::sct::SctOpcodeDocumentRole::FoldedModifier)
            continue;
        const auto mnemonic = schema.semantic.mnemonic.empty()
            ? tr("Opcode %1").arg(schema.opcode)
            : QString::fromUtf8(schema.semantic.mnemonic.data(),
                static_cast<qsizetype>(schema.semantic.mnemonic.size()));
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  %2").arg(schema.opcode, 3, 10, QLatin1Char('0'))
                .arg(mnemonic), list);
        item->setData(Qt::UserRole, schema.opcode);
        const auto draft = documentController_->createInstructionDraft(locator, schema.opcode);
        if ((schema.opcode == 12u && !allowReturn) || !draft.draft) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(schema.opcode == 12u && !allowReturn
                ? tr("Return can only be inserted at the end of a section.")
                : draft.diagnostics.empty()
                    ? tr("The frozen schema cannot safely construct this instruction.")
                    : QString::fromStdString(draft.diagnostics.front().message));
        } else if (firstEnabled < 0) {
            firstEnabled = list->count() - 1;
        }
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
            buttons->button(QDialogButtonBox::Ok)->setEnabled(current != nullptr
                && !current->isHidden() && (current->flags() & Qt::ItemIsEnabled));
        });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
        [&dialog](QListWidgetItem* item) {
            if (item != nullptr && (item->flags() & Qt::ItemIsEnabled))
                dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (firstEnabled >= 0) list->setCurrentRow(firstEnabled);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr
        || list->currentItem()->isHidden()
        || !(list->currentItem()->flags() & Qt::ItemIsEnabled)) return std::nullopt;
    const auto opcode = static_cast<std::uint16_t>(
        list->currentItem()->data(Qt::UserRole).toUInt());
    auto created = documentController_->createInstructionDraft(locator, opcode);
    if (!created.draft) {
        const auto message = created.diagnostics.empty()
            ? tr("This opcode cannot be authored safely.")
            : QString::fromStdString(created.diagnostics.front().message);
        QMessageBox::warning(this, tr("New Instruction"), message);
        return std::nullopt;
    }
    auto draft = std::move(*created.draft);

    struct DraftEditorRow final {
        std::size_t parameterIndex = 0;
        std::optional<std::size_t> ownedTextIndex{};
        QLineEdit* line = nullptr;
        QComboBox* choices = nullptr;
        QCheckBox* acceptSuggestion = nullptr;
        std::vector<core::SctReferenceCandidate> candidates{};
        bool newFooterMessageChoice = false;
        QString initialText{};
    };

    QDialog editor(this);
    const auto* selectedSchema = spice::sct::findSctOpcodeSchema(opcode);
    const auto selectedName = selectedSchema != nullptr
            && !selectedSchema->semantic.mnemonic.empty()
        ? QString::fromUtf8(selectedSchema->semantic.mnemonic.data(),
            static_cast<qsizetype>(selectedSchema->semantic.mnemonic.size()))
        : tr("Opcode %1").arg(opcode);
    editor.setWindowTitle(tr("Configure %1").arg(selectedName));
    editor.resize(820, 480);
    auto* editorLayout = new QVBoxLayout(&editor);
    auto* help = new QLabel(tr(
        "Resolve every required value. Suggested values must be explicitly accepted or replaced."),
        &editor);
    help->setWordWrap(true);
    editorLayout->addWidget(help);
    auto* table = new QTableWidget(
        static_cast<int>(draft.instruction.parameters.size()), 3, &editor);
    table->setHorizontalHeaderLabels({tr("Parameter"), tr("Value"), tr("Notes")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->resizeSection(0, 210);
    table->horizontalHeader()->resizeSection(1, 360);
    table->horizontalHeader()->resizeSection(2, 210);
    table->verticalHeader()->hide();
    std::vector<DraftEditorRow> rows;
    rows.reserve(draft.instruction.parameters.size());
    const auto* opcodeSchema = spice::sct::findSctOpcodeSchema(opcode);
    for (std::size_t i = 0; i < draft.instruction.parameters.size(); ++i) {
        auto& parameter = draft.instruction.parameters[i];
        DraftEditorRow row;
        row.parameterIndex = i;
        const auto* parameterSchema = opcodeSchema == nullptr ? nullptr
            : spice::sct::sctOpcodeParameterSchema(
                *opcodeSchema, parameter.address.schemaIndex);
        auto role = parameterSchema != nullptr && !parameterSchema->role.empty()
            ? QString::fromUtf8(parameterSchema->role.data(),
                static_cast<qsizetype>(parameterSchema->role.size()))
            : tr("Parameter %1").arg(parameter.address.schemaIndex);
        if (parameter.address.repeatedGroupOrdinal)
            role += tr(" (group %1)").arg(*parameter.address.repeatedGroupOrdinal + 1u);
        table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(role));

        const auto owned = std::ranges::find(draft.ownedFooterText,
            parameter.address, &core::SctOwnedFooterTextDraft::parameter);
        if (owned != draft.ownedFooterText.end()) {
            row.ownedTextIndex = static_cast<std::size_t>(
                std::distance(draft.ownedFooterText.begin(), owned));
            if (owned->kind == spice::sct::SctTextKind::PlainString) {
                row.line = new QLineEdit(&editor);
                row.line->setPlaceholderText(tr("Instruction-owned footer text"));
                if (const auto* plain = std::get_if<spice::sct::SctPlainText>(&owned->value))
                    row.line->setText(QString::fromStdString(plain->utf8));
                row.initialText = row.line->text();
                table->setCellWidget(static_cast<int>(i), 1, row.line);
                table->setItem(static_cast<int>(i), 2,
                    new QTableWidgetItem(tr("A private footer entry is created with the instruction.")));
            } else {
                row.choices = new QComboBox(&editor);
                row.choices->addItem(tr("Create a new default footer message"));
                row.candidates = documentController_->draftReferenceCandidates(
                    locator, opcode, parameter.address);
                for (const auto& candidate : row.candidates)
                    row.choices->addItem(QString::fromStdString(candidate.label));
                row.newFooterMessageChoice = true;
                table->setCellWidget(static_cast<int>(i), 1, row.choices);
                table->setItem(static_cast<int>(i), 2,
                    new QTableWidgetItem(tr("Choose an existing message or create a new default one.")));
            }
        } else {
            const auto* effective = parameter.value ? &*parameter.value
                : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
            const bool reference = parameterSchema != nullptr
                && parameterSchema->referenceKind
                    != spice::sct::SctOpcodeReferenceKind::None;
            if (reference) {
                row.choices = new QComboBox(&editor);
                row.candidates = documentController_->draftReferenceCandidates(
                    locator, opcode, parameter.address);
                for (const auto& candidate : row.candidates)
                    row.choices->addItem(QString::fromStdString(candidate.label));
                if (effective != nullptr) {
                    const auto target = std::visit([](const auto& typed)
                            -> std::optional<spice::sct::SctDocumentReferenceTarget> {
                        using T = std::decay_t<decltype(typed)>;
                        if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>
                            || std::is_same_v<T, spice::sct::SctStringReference>
                            || std::is_same_v<T, spice::sct::SctFooterEntryReference>)
                            return typed.target;
                        return std::nullopt;
                    }, *effective);
                    if (target) {
                        const auto found = std::ranges::find(row.candidates, *target,
                            &core::SctReferenceCandidate::target);
                        if (found != row.candidates.end()) row.choices->setCurrentIndex(
                            static_cast<int>(std::distance(row.candidates.begin(), found)));
                    }
                }
                table->setCellWidget(static_cast<int>(i), 1, row.choices);
            } else if (effective != nullptr) {
                const auto terminator = parameterSchema != nullptr
                        && parameterSchema->terminator
                    ? std::optional{parameterSchema->terminator->encodedWord}
                    : std::nullopt;
                row.line = new QLineEdit(
                    draftValueText(*effective, terminator), &editor);
                row.initialText = row.line->text();
                table->setCellWidget(static_cast<int>(i), 1, row.line);
            } else {
                auto* unavailable = new QTableWidgetItem(tr("No safe authoring value available"));
                unavailable->setFlags(unavailable->flags() & ~Qt::ItemIsEditable);
                table->setItem(static_cast<int>(i), 1, unavailable);
            }
            if (!parameter.value && parameter.suggestedValue) {
                row.acceptSuggestion = new QCheckBox(tr("Accept suggested value"), &editor);
                table->setCellWidget(static_cast<int>(i), 2, row.acceptSuggestion);
            } else {
                table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(
                    effective == nullptr ? tr("Required value") : tr("Factory default")));
            }
        }
        rows.push_back(std::move(row));
    }
    editorLayout->addWidget(table, 1);
    auto* editorButtons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
    editorButtons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    editorLayout->addWidget(editorButtons);
    connect(editorButtons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
    connect(editorButtons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);

    while (editor.exec() == QDialog::Accepted) {
        auto candidate = draft;
        std::vector<std::size_t> removeOwned;
        QString error;
        for (const auto& row : rows) {
            auto& parameter = candidate.instruction.parameters[row.parameterIndex];
            if (row.ownedTextIndex) {
                auto& ownedText = candidate.ownedFooterText[*row.ownedTextIndex];
                if (ownedText.kind == spice::sct::SctTextKind::PlainString) {
                    ownedText.value = spice::sct::SctPlainText{
                        row.line->text().toStdString()};
                } else if (row.choices->currentIndex() > 0) {
                    const auto selected = static_cast<std::size_t>(
                        row.choices->currentIndex() - 1);
                    parameter.value = referenceValue(row.candidates[selected].target);
                    removeOwned.push_back(*row.ownedTextIndex);
                }
                continue;
            }
            if (row.choices != nullptr) {
                if (row.choices->currentIndex() < 0 || row.candidates.empty()) {
                    error = tr("%1 requires a compatible reference target.")
                        .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                    break;
                }
                parameter.value = referenceValue(row.candidates[
                    static_cast<std::size_t>(row.choices->currentIndex())].target);
                continue;
            }
            const auto* current = parameter.value ? &*parameter.value
                : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
            if (current == nullptr || row.line == nullptr) {
                error = tr("%1 has no safe value yet.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            const bool changed = row.line->text() != row.initialText;
            if (!parameter.value && parameter.suggestedValue
                && !changed && (row.acceptSuggestion == nullptr
                    || !row.acceptSuggestion->isChecked())) {
                error = tr("Explicitly accept or replace the suggested value for %1.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            if (changed) {
                const auto parsed = core::SctParameterAuthoringService::parseDraftValue(
                    opcode, parameter.address, *current, row.line->text().toStdString());
                if (!parsed.succeeded()) {
                    error = QString::fromStdString(parsed.error);
                    break;
                }
                parameter.value = *parsed.value;
            } else {
                parameter.value = *current;
            }
        }
        if (error.isEmpty()) {
            std::ranges::sort(removeOwned, std::greater{});
            for (const auto index : removeOwned)
                candidate.ownedFooterText.erase(candidate.ownedFooterText.begin() + index);
            return candidate;
        }
        QMessageBox::warning(&editor, tr("Incomplete Instruction Draft"), error);
    }
    return std::nullopt;
}

void MainWindow::insertInstruction() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto context = widget->insertionContext();
    if (!context.has_value()) return;
    auto draft = chooseInstructionDraft(widget->locator(), context->allowReturn);
    if (!draft.has_value()) return;
    (void)documentController_->createInstructionAfter(
        widget->locator(), context->anchor, std::move(*draft));
}

std::optional<std::uint16_t> MainWindow::chooseSemanticArmOpcode() {
    QStringList choices;
    std::vector<std::uint16_t> opcodes;
    for (const auto& choice : core::SctEditSession::insertableOpcodes()) {
        if (choice.opcode == 12u) continue;
        choices.push_back(QStringLiteral("%1  %2")
            .arg(choice.opcode, 3, 10, QLatin1Char('0'))
            .arg(QString::fromStdString(choice.mnemonic)));
        opcodes.push_back(choice.opcode);
    }
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this,
        tr("First Semantic-Arm Instruction"),
        tr("Opcode (only instructions requiring no parameter draft are available here):"),
        choices, 0, false, &accepted);
    const auto index = choices.indexOf(selected);
    return accepted && index >= 0
        ? std::optional{opcodes[static_cast<std::size_t>(index)]}
        : std::nullopt;
}

std::optional<spice::sct::SctDocumentRepeatedParameterGroup>
MainWindow::configureRepeatedGroupDraft(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction,
    spice::sct::SctRepeatedParameterGroupDraft draft) {
    struct Row final {
        std::size_t parameterIndex = 0;
        QLineEdit* line = nullptr;
        QComboBox* choices = nullptr;
        QCheckBox* acceptSuggestion = nullptr;
        QString initialText;
        std::vector<core::SctReferenceCandidate> candidates;
    };
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Repeated Group — Instruction %1")
        .arg(instruction.value()));
    dialog.resize(760, 380);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr(
        "Resolve required values and explicitly accept or replace provisional suggestions."),
        &dialog));
    auto* table = new QTableWidget(static_cast<int>(draft.parameters.size()), 3, &dialog);
    table->setHorizontalHeaderLabels({tr("Parameter"), tr("Value"), tr("Notes")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->resizeSection(0, 190);
    table->horizontalHeader()->resizeSection(1, 350);
    table->horizontalHeader()->resizeSection(2, 190);
    table->verticalHeader()->hide();
    const auto* opcodeSchema = spice::sct::findSctOpcodeSchema(draft.opcode);
    std::vector<Row> rows;
    for (std::size_t i = 0; i < draft.parameters.size(); ++i) {
        auto& parameter = draft.parameters[i];
        Row row;
        row.parameterIndex = i;
        const spice::sct::SctParameterAddress address{parameter.schemaIndex, 0u};
        const auto* schema = opcodeSchema == nullptr ? nullptr
            : spice::sct::sctOpcodeParameterSchema(*opcodeSchema, parameter.schemaIndex);
        const auto role = schema != nullptr && !schema->role.empty()
            ? QString::fromUtf8(schema->role.data(),
                static_cast<qsizetype>(schema->role.size()))
            : tr("Parameter %1").arg(parameter.schemaIndex);
        table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(role));
        const auto* effective = parameter.value ? &*parameter.value
            : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
        const bool reference = schema != nullptr
            && schema->referenceKind != spice::sct::SctOpcodeReferenceKind::None;
        if (reference) {
            row.choices = new QComboBox(&dialog);
            row.candidates = documentController_->draftReferenceCandidates(
                locator, draft.opcode, address);
            for (const auto& candidate : row.candidates)
                row.choices->addItem(QString::fromStdString(candidate.label));
            table->setCellWidget(static_cast<int>(i), 1, row.choices);
        } else if (effective != nullptr) {
            const auto terminator = schema != nullptr && schema->terminator
                ? std::optional{schema->terminator->encodedWord}
                : std::nullopt;
            row.line = new QLineEdit(
                draftValueText(*effective, terminator), &dialog);
            row.initialText = row.line->text();
            table->setCellWidget(static_cast<int>(i), 1, row.line);
        } else {
            table->setItem(static_cast<int>(i), 1,
                new QTableWidgetItem(tr("No safe value available")));
        }
        if (!parameter.value && parameter.suggestedValue) {
            row.acceptSuggestion = new QCheckBox(tr("Accept suggested value"), &dialog);
            table->setCellWidget(static_cast<int>(i), 2, row.acceptSuggestion);
        } else {
            table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(
                effective == nullptr ? tr("Required value") : tr("Factory default")));
        }
        rows.push_back(std::move(row));
    }
    layout->addWidget(table, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    while (dialog.exec() == QDialog::Accepted) {
        auto candidate = draft;
        QString error;
        for (const auto& row : rows) {
            auto& parameter = candidate.parameters[row.parameterIndex];
            const spice::sct::SctParameterAddress address{parameter.schemaIndex, 0u};
            if (row.choices != nullptr) {
                if (row.choices->currentIndex() < 0 || row.candidates.empty()) {
                    error = tr("%1 requires a compatible reference target.")
                        .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                    break;
                }
                parameter.value = referenceValue(row.candidates[
                    static_cast<std::size_t>(row.choices->currentIndex())].target);
                continue;
            }
            const auto* current = parameter.value ? &*parameter.value
                : parameter.suggestedValue ? &*parameter.suggestedValue : nullptr;
            if (current == nullptr || row.line == nullptr) {
                error = tr("%1 is unresolved.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            const bool changed = row.line->text() != row.initialText;
            if (!parameter.value && parameter.suggestedValue && !changed
                && (row.acceptSuggestion == nullptr
                    || !row.acceptSuggestion->isChecked())) {
                error = tr("Explicitly accept or replace the suggested value for %1.")
                    .arg(table->item(static_cast<int>(row.parameterIndex), 0)->text());
                break;
            }
            if (changed) {
                const auto parsed = core::SctParameterAuthoringService::parseDraftValue(
                    candidate.opcode, address, *current,
                    row.line->text().toStdString());
                if (!parsed.succeeded()) {
                    error = QString::fromStdString(parsed.error);
                    break;
                }
                parameter.value = *parsed.value;
            } else parameter.value = *current;
        }
        if (error.isEmpty()) {
            const auto materialized =
                spice::sct::SctInstructionFactory::materializeRepeatedGroup(candidate);
            if (materialized.group) return *materialized.group;
            error = materialized.diagnostics.empty()
                ? tr("The repeated group is incomplete.")
                : QString::fromStdString(materialized.diagnostics.front().message);
        }
        QMessageBox::warning(&dialog, tr("Incomplete Repeated Group"), error);
    }
    return std::nullopt;
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

void MainWindow::createScriptSection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Script Section"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Name ([A-Za-z0-9_], 1-16 characters):"), &dialog));
    auto* name = new QLineEdit(&dialog);
    name->setMaxLength(16);
    layout->addWidget(name);
    auto* includeReturn = new QCheckBox(tr("End with Return (12)"), &dialog);
    includeReturn->setChecked(true);
    layout->addWidget(includeReturn);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    connect(name, &QLineEdit::textChanged, &dialog, [buttons](const QString& value) {
        static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_]{1,16}$"));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(pattern.match(value).hasMatch());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    (void)documentController_->createScriptSection(widget->locator(),
        name->text().toStdString(), widget->selectedSection(), includeReturn->isChecked());
}

void MainWindow::createIndexedString() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("New Indexed String"),
        tr("Section name ([A-Za-z0-9_], 1-16 characters):"),
        QLineEdit::Normal, {}, &accepted);
    if (!accepted) return;
    (void)documentController_->createIndexedString(widget->locator(),
        name.toStdString(), widget->selectedSection());
}

void MainWindow::renameSelectedSection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto section = widget->selectedSection();
    if (!section) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Rename Section"),
        tr("New name ([A-Za-z0-9_], 1-16 characters):"),
        QLineEdit::Normal, {}, &accepted);
    if (accepted) (void)documentController_->renameSection(
        widget->locator(), *section, name.toStdString());
}

void MainWindow::deleteSelectedSection() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto section = widget->selectedSection();
    if (!section) return;
    if (QMessageBox::question(this, tr("Delete Script Section"),
            tr("Delete the selected script section and all instructions it owns?"))
        != QMessageBox::Yes) return;
    (void)documentController_->deleteSection(widget->locator(), *section);
}

void MainWindow::moveSelectedSection(const core::SctSectionMoveDirection direction) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    if (const auto section = widget->selectedSection())
        (void)documentController_->moveSection(widget->locator(), *section, direction);
}

void MainWindow::createFooterText(const core::SctCreatedFooterTextKind kind) {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    std::optional<spice::sct::SctFooterEntryId> after;
    if (const auto target = widget->selectedTextTarget()) {
        if (const auto* footer = std::get_if<spice::sct::SctFooterEntryId>(&*target))
            after = *footer;
    }
    (void)documentController_->createFooterText(widget->locator(), kind, after);
}

void MainWindow::deleteSelectedText() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr) return;
    const auto target = widget->selectedTextTarget();
    if (!target) return;
    if (QMessageBox::question(this, tr("Delete Text Entity"),
            tr("Delete the selected text entity?")) != QMessageBox::Yes) return;
    (void)documentController_->deleteTextEntity(widget->locator(), *target);
}

void MainWindow::editSelectedMessage() {
    auto* widget = activeDocumentWidget();
    if (widget == nullptr || !widget->canEditSelectedMessage()) return;
    const auto target = widget->selectedTextTarget();
    const auto snapshot = documentController_->snapshot(widget->locator());
    const auto value = target ? documentController_->workingText(widget->locator(), *target)
        : std::nullopt;
    if (!target.has_value() || snapshot == nullptr || !value) return;
    if (!messageEditor_->isBoundTo(widget->locator(), *target)
        ) {
        spice::sct::SctTextKind kind = std::holds_alternative<spice::sct::SctMessage>(*value)
            ? spice::sct::SctTextKind::SctString : spice::sct::SctTextKind::PlainString;
        spice::sct::SctTextStorage storage = std::holds_alternative<spice::sct::SctStringId>(*target)
            ? spice::sct::SctTextStorage::IndexedSection : spice::sct::SctTextStorage::Footer;
        if (snapshot->analysis && snapshot->document) {
            std::visit([&](const auto id) {
                if (const auto* entity = snapshot->analysis->entities.find(*snapshot->document, id))
                    kind = entity->kind;
            }, *target);
        }
        if (!messageEditor_->bindText(widget->locator(), *target, *value, kind, storage)) {
            statusBar()->showMessage(tr("The selected text could not be opened for editing."), 8000);
            return;
        }
    }
    messageEditorDock_->show();
    messageEditorDock_->raise();
    messageEditor_->focusEditor();
}

bool MainWindow::flushMessageEditor() {
    return messageEditor_ == nullptr || messageEditor_->flushPending();
}

bool MainWindow::prepareScptEditor(
    const std::optional<core::AssetLocator>& locator) {
    if (scptEditor_ == nullptr || !scptEditor_->boundLocator()) return true;
    if (locator && *scptEditor_->boundLocator() != *locator) return true;
    return scptEditor_->prepareToClear();
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
    const core::AssetLocator& locator, const QString& action,
    const PendingLifecycle pending) {
    if (!documentController_->isDirty(locator)) return true;
    QMessageBox message(this);
    message.setIcon(QMessageBox::Warning);
    message.setWindowTitle(tr("Save document changes?"));
    message.setText(tr("%1 has changes that are not in a patch checkpoint.")
        .arg(QString::fromStdWString(locator.path().wstring())));
    message.setInformativeText(patchWorkspace_ && !documentController_->patchConflict(locator)
        ? tr("Save a patch checkpoint, discard the changes, or cancel before you %1?").arg(action)
        : tr("No writable SALSA workspace is available. Discard the changes or cancel before you %1?")
            .arg(action));
    auto buttons = QMessageBox::Discard | QMessageBox::Cancel;
    if (patchWorkspace_ && !documentController_->patchConflict(locator))
        buttons |= QMessageBox::Save;
    message.setStandardButtons(buttons);
    message.setDefaultButton(QMessageBox::Cancel);
    const auto choice = message.exec();
    if (choice == QMessageBox::Discard) return true;
    if (choice != QMessageBox::Save) return false;
    pendingLifecycle_ = pending;
    pendingLifecycleDocument_ = pending == PendingLifecycle::CloseDocument
        ? std::optional<core::AssetLocator>{locator} : std::nullopt;
    pendingLifecycleSaves_ = {locator};
    if (!documentController_->isSaving(locator)
        && !documentController_->saveDocument(locator)) {
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleDocument_.reset();
        pendingLifecycleSaves_.clear();
        statusBar()->showMessage(tr("The document checkpoint could not be started."), 8000);
    }
    return false;
}

bool MainWindow::confirmDiscardAll(
    const QString& action, const PendingLifecycle pending) {
    const auto dirty = documentController_->dirtyLocators();
    if (dirty.empty()) return true;
    QMessageBox message(this);
    message.setIcon(QMessageBox::Warning);
    message.setWindowTitle(tr("Save document changes?"));
    message.setText(dirty.size() == 1
        ? tr("One open SCT document has changes outside its patch checkpoint.")
        : tr("%1 open SCT documents have changes outside their patch checkpoints.")
            .arg(dirty.size()));
    const bool canSaveAll = patchWorkspace_ && std::ranges::none_of(
        dirty, [this](const auto& locator) {
            return documentController_->patchConflict(locator);
        });
    message.setInformativeText(canSaveAll
        ? tr("Save patch checkpoints, discard all changes, or cancel before you %1?").arg(action)
        : tr("Not every document has a writable, conflict-free SALSA workspace. Discard all changes or cancel before you %1?")
            .arg(action));
    auto buttons = QMessageBox::Discard | QMessageBox::Cancel;
    if (canSaveAll) buttons |= QMessageBox::Save;
    message.setStandardButtons(buttons);
    message.setDefaultButton(QMessageBox::Cancel);
    const auto choice = message.exec();
    if (choice == QMessageBox::Discard) return true;
    if (choice != QMessageBox::Save) return false;
    pendingLifecycle_ = pending;
    pendingLifecycleDocument_.reset();
    pendingLifecycleSaves_.clear();
    bool failedToStart = false;
    for (const auto& locator : dirty) {
        pendingLifecycleSaves_.push_back(locator);
        if (!documentController_->isSaving(locator))
            failedToStart = !documentController_->saveDocument(locator) || failedToStart;
    }
    if (failedToStart) {
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleSaves_.clear();
        statusBar()->showMessage(tr(
            "One or more document checkpoints could not be started."), 8000);
    } else {
        progressBar_->setRange(0, 0);
        progressBar_->show();
        cancelButton_->show();
    }
    return false;
}

void MainWindow::continuePendingLifecycle(
    const QString& identityKey, const bool success, const bool cancelled) {
    if (pendingLifecycleSaves_.empty()) return;
    if (!success || cancelled) {
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleDocument_.reset();
        pendingLifecycleSaves_.clear();
        return;
    }
    std::erase_if(pendingLifecycleSaves_, [&identityKey](const auto& locator) {
        return QString::fromStdString(locator.identityKey()) == identityKey;
    });
    if (!pendingLifecycleSaves_.empty()) return;

    if (pendingLifecycleDocument_.has_value()
        && documentController_->isDirty(*pendingLifecycleDocument_)) {
        statusBar()->showMessage(tr(
            "The document changed while its checkpoint was being saved; it remains open."),
            8000);
        pendingLifecycle_ = PendingLifecycle::None;
        pendingLifecycleDocument_.reset();
        return;
    }
    if (pendingLifecycle_ != PendingLifecycle::CloseDocument
        && !documentController_->dirtyLocators().empty()) {
        statusBar()->showMessage(tr(
            "A document changed while checkpoints were being saved; the requested action was cancelled."),
            8000);
        pendingLifecycle_ = PendingLifecycle::None;
        return;
    }

    const auto action = pendingLifecycle_;
    const auto document = pendingLifecycleDocument_;
    pendingLifecycle_ = PendingLifecycle::None;
    pendingLifecycleDocument_.reset();
    if (action == PendingLifecycle::CloseDocument && document.has_value()) {
        documentController_->closeDocument(*document);
    } else if (action == PendingLifecycle::CloseDataset) {
        scptEditor_->clear();
        messageEditor_->clear();
        saveWorkspaceSession();
        documentController_->closeAll();
        detachPatchWorkspace(false);
        controller_->closeWorkspace();
        statusBar()->showMessage(tr("Dataset closed."), 5000);
    } else if (action == PendingLifecycle::OpenDataset) {
        const auto root = std::exchange(pendingDatasetRoot_, {});
        QTimer::singleShot(0, this, [this, root] { openDataset(root); });
    } else if (action == PendingLifecycle::RebasePatches) {
        QTimer::singleShot(0, this, [this]() { rebaseStalePatches(); });
    } else if (action == PendingLifecycle::Exit) {
        QTimer::singleShot(0, this, &QWidget::close);
    }
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
        widget->setParameterPresentationProvider([this, widget](const auto instruction) {
            return documentController_->parameterPresentation(
                widget->locator(), instruction);
        });
        widget->setParameterCommitHandler([this, widget](const auto& site,
                std::string text) {
            return documentController_->editParameterText(
                widget->locator(), site, std::move(text));
        });
        widget->setStructuredDeveloperOptions(
            showStructuredBasicBlocks_, showRejectedStructureEvidence_,
            showSemanticControlFlowInstructions_);
        tabs_->addTab(widget, QString::fromStdWString(found->path().filename().wstring()));
        connect(widget, &SctDocumentWidget::textConventionRequested, this,
            [this, widget](const QString&, const int convention) {
                if (!prepareScptEditor(widget->locator()) || !flushMessageEditor()
                    || !confirmDiscardDocument(widget->locator(), tr("change text interpretation"))) return;
                scptEditor_->clear();
                messageEditor_->clear();
                (void)documentController_->selectTextConvention(widget->locator(),
                    static_cast<spice::sct::SctKnownTextConvention>(convention));
            });
        connect(widget, &SctDocumentWidget::reloadRequested, this,
            [this, widget](const QString&) {
                if (!prepareScptEditor(widget->locator()) || !flushMessageEditor()
                    || !confirmDiscardDocument(widget->locator(), tr("reload the document"))) return;
                scptEditor_->clear();
                messageEditor_->clear();
                if (const auto project = controller_->projectSnapshot(); project.has_value())
                    (void)documentController_->reloadDocument(*project, widget->locator());
            });
        connect(widget, &SctDocumentWidget::becameActive, this,
            [this](const QString&) { syncDiagnostics(); syncEditActions(); });
        connect(widget, &SctDocumentWidget::navigationChanged, this,
            [this, widget](const QString&, const int kind, const qulonglong id) {
                recordNavigation({widget->locator(), core::SctNavigationTarget{
                    static_cast<core::SctNavigationKind>(kind), id}});
            });
        connect(widget, &SctDocumentWidget::activeViewChanged, this,
            [this](const QString&, int) { scheduleWorkspaceSessionSave(); });
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
        connect(widget, &SctDocumentWidget::parameterNavigationRequested,
            this, [this, widget](const QString&, const int kind, const qulonglong id) {
                const core::SctNavigationTarget target{
                    static_cast<core::SctNavigationKind>(kind), id};
                widget->selectTarget(target);
                if (target.kind == core::SctNavigationKind::String
                    || target.kind == core::SctNavigationKind::FooterEntry)
                    editSelectedMessage();
            });
        connect(widget, &SctDocumentWidget::advancedScptRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 schemaIndex, const int repeatedOrdinal) {
                spice::sct::SctParameterSite site{
                    spice::sct::SctInstructionId(instruction),
                    {schemaIndex, repeatedOrdinal < 0
                        ? std::optional<std::uint32_t>{}
                        : std::optional{static_cast<std::uint32_t>(repeatedOrdinal)}}};
                const auto expression = documentController_->workingParameterExpression(
                    widget->locator(), site);
                if (!expression) return;
                if (scptEditor_->bind(widget->locator(), site, *expression)) {
                    scptEditorDock_->show();
                    scptEditorDock_->raise();
                }
            });
        connect(widget, &SctDocumentWidget::changeParameterReferenceRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 schemaIndex, const int repeatedOrdinal) {
                const spice::sct::SctParameterSite site{
                    spice::sct::SctInstructionId(instruction),
                    {schemaIndex, repeatedOrdinal < 0
                        ? std::optional<std::uint32_t>{}
                        : std::optional{static_cast<std::uint32_t>(repeatedOrdinal)}}};
                const auto candidates = documentController_->referenceCandidates(
                    widget->locator(), site);
                QStringList labels;
                for (const auto& candidate : candidates)
                    labels.push_back(QString::fromStdString(candidate.label));
                bool accepted = false;
                const auto choice = QInputDialog::getItem(this,
                    tr("Select Reference Target"), tr("Compatible target:"),
                    labels, 0, false, &accepted);
                const auto selected = labels.indexOf(choice);
                if (accepted && selected >= 0)
                    (void)documentController_->retargetParameter(
                        widget->locator(), site,
                        candidates[static_cast<std::size_t>(selected)].target);
            });
        connect(widget, &SctDocumentWidget::replaceOpaqueParameterRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 schemaIndex, const int repeatedOrdinal,
                    const int editorKind) {
                const spice::sct::SctParameterSite site{
                    spice::sct::SctInstructionId(instruction),
                    {schemaIndex, repeatedOrdinal < 0
                        ? std::optional<std::uint32_t>{}
                        : std::optional{static_cast<std::uint32_t>(repeatedOrdinal)}}};
                const auto kind = static_cast<core::SctInlineParameterEditorKind>(
                    editorKind);
                if (kind == core::SctInlineParameterEditorKind::Reference) {
                    const auto candidates = documentController_->referenceCandidates(
                        widget->locator(), site);
                    QStringList labels;
                    for (const auto& candidate : candidates)
                        labels.push_back(QString::fromStdString(candidate.label));
                    bool accepted = false;
                    const auto choice = QInputDialog::getItem(this,
                        tr("Replace With Typed Reference"), tr("Compatible target:"),
                        labels, 0, false, &accepted);
                    const auto selected = labels.indexOf(choice);
                    if (accepted && selected >= 0)
                        (void)documentController_->retargetParameter(
                            widget->locator(), site,
                            candidates[static_cast<std::size_t>(selected)].target);
                    return;
                }
                if (kind == core::SctInlineParameterEditorKind::AdvancedScpt) {
                    const auto expression =
                        spice::sct::SctExpressionFactory::encodedDecimalLiteral(0);
                    if (scptEditor_->bind(widget->locator(), site, expression)) {
                        scptEditorDock_->show();
                        scptEditorDock_->raise();
                    }
                    return;
                }
                bool accepted = false;
                const auto text = QInputDialog::getText(this,
                    tr("Replace With Typed Value"),
                    kind == core::SctInlineParameterEditorKind::TerminatedWords
                        ? tr("Word list (the terminator is managed automatically):")
                        : tr("32-bit value:"),
                    QLineEdit::Normal, {}, &accepted);
                if (accepted)
                    (void)documentController_->editParameterText(
                        widget->locator(), site, text.toStdString());
            });
        connect(widget, &SctDocumentWidget::addRepeatedGroupRequested,
            this, [this, widget](const QString&, const qulonglong instructionId,
                    const quint32 ordinal) {
                const auto instruction = documentController_->workingInstruction(
                    widget->locator(), spice::sct::SctInstructionId(instructionId));
                if (!instruction) return;
                auto draftResult = spice::sct::SctInstructionFactory::createRepeatedGroupDraft(
                    instruction->opcode);
                if (!draftResult.draft) {
                    QMessageBox::warning(this, tr("Add Repeated Group"),
                        tr("The opcode schema cannot construct a repeated group draft."));
                    return;
                }
                const auto group = configureRepeatedGroupDraft(widget->locator(),
                    instruction->id, std::move(*draftResult.draft));
                if (!group) return;
                (void)documentController_->insertRepeatedGroup(widget->locator(),
                    instruction->id, ordinal, *group);
            });
        connect(widget, &SctDocumentWidget::deleteRepeatedGroupRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 ordinal) {
                (void)documentController_->deleteRepeatedGroup(widget->locator(),
                    spice::sct::SctInstructionId(instruction), ordinal);
            });
        connect(widget, &SctDocumentWidget::moveRepeatedGroupRequested,
            this, [this, widget](const QString&, const qulonglong instruction,
                    const quint32 ordinal, const int direction) {
                (void)documentController_->moveRepeatedGroup(widget->locator(),
                    spice::sct::SctInstructionId(instruction), ordinal,
                    static_cast<core::SctRepeatedGroupMoveDirection>(direction));
            });
        connect(widget, &SctDocumentWidget::createScriptSectionRequested,
            this, [this](const QString&) { createScriptSection(); });
        connect(widget, &SctDocumentWidget::createIndexedStringRequested,
            this, [this](const QString&) { createIndexedString(); });
        connect(widget, &SctDocumentWidget::renameSectionRequested,
            this, [this](const QString&) { renameSelectedSection(); });
        connect(widget, &SctDocumentWidget::deleteSectionRequested,
            this, [this](const QString&) { deleteSelectedSection(); });
        connect(widget, &SctDocumentWidget::moveSectionRequested,
            this, [this](const QString&, const int direction) {
                moveSelectedSection(static_cast<core::SctSectionMoveDirection>(direction));
            });
        connect(widget, &SctDocumentWidget::createFooterTextRequested,
            this, [this](const QString&, const int kind) {
                createFooterText(static_cast<core::SctCreatedFooterTextKind>(kind));
            });
        connect(widget, &SctDocumentWidget::deleteTextRequested,
            this, [this](const QString&) { deleteSelectedText(); });
        connect(widget, &SctDocumentWidget::addElseRequested,
            this, [this, widget](const QString&, const qulonglong controller) {
                (void)documentController_->addVirtualElse(widget->locator(),
                    spice::sct::SctInstructionId(controller));
            });
        connect(widget, &SctDocumentWidget::addCaseRequested,
            this, [this, widget](const QString&, const qulonglong controller) {
                (void)documentController_->addVirtualCase(widget->locator(),
                    spice::sct::SctInstructionId(controller));
            });
        connect(widget, &SctDocumentWidget::setCaseValueRequested,
            this, [this, widget](const QString&, const qulonglong arm) {
                bool accepted = false;
                const auto value = QInputDialog::getInt(this, tr("Switch Case Value"),
                    tr("Signed case value:"), 0,
                    (std::numeric_limits<int>::lowest)(),
                    (std::numeric_limits<int>::max)(), 1, &accepted);
                if (accepted) {
                    (void)documentController_->setVirtualCaseValue(widget->locator(),
                        core::SctAuthoredArmId{arm}, static_cast<std::int32_t>(value));
                }
            });
        connect(widget, &SctDocumentWidget::removeSemanticArmRequested,
            this, [this, widget](const QString&, const qulonglong arm) {
                (void)documentController_->removeVirtualArm(
                    widget->locator(), core::SctAuthoredArmId{arm});
            });
        connect(widget, &SctDocumentWidget::insertIntoSemanticArmRequested,
            this, [this, widget](const QString&, const qulonglong arm) {
                const auto opcode = chooseSemanticArmOpcode();
                if (opcode) {
                    (void)documentController_->insertInstructionIntoAuthoredArm(
                        widget->locator(), core::SctAuthoredArmId{arm}, *opcode);
                }
            });
        connect(widget, &SctDocumentWidget::insertIntoStructuredArmRequested,
            this, [this, widget](const QString&, const qulonglong controller,
                const int armKind) {
                const auto opcode = chooseSemanticArmOpcode();
                if (opcode) {
                    (void)documentController_->insertInstructionIntoStructuredArm(
                        widget->locator(), spice::sct::SctInstructionId(controller),
                        static_cast<spice::sct::SctStructuredArmKind>(armKind),
                        *opcode);
                }
            });
        connect(widget, &SctDocumentWidget::returnSemanticArmToEmptyRequested,
            this, [this, widget](const QString&, const qulonglong arm,
                const qulonglong instruction) {
                (void)documentController_->deleteOnlyInstructionFromAuthoredArm(
                    widget->locator(), core::SctAuthoredArmId{arm},
                    spice::sct::SctInstructionId(instruction));
            });
    }
    const auto snapshot = update.snapshot != nullptr
        ? update.snapshot : documentController_->snapshot(*found);
    const auto sourceStatus = static_cast<int>(documentController_->sourceStatus(*found));
    const bool textOnly = !createdWidget && isTextOnlyTransition(update);
    const bool instructionDelta = !createdWidget
        && isIncrementalStructuralTransition(update);
    const bool authoringOnly = !createdWidget && update.transition
        && !update.transition->changes.structuredAuthoring.empty()
        && !update.transition->changes.documentChanged;
    bool incrementalInstructionApplied = false;
    if (update.kind == SctDocumentUpdateKind::VerifiedMaterialization) {
        widget->installVerifiedSnapshot(snapshot, sourceStatus);
    } else if (authoringOnly) {
        widget->setSourceStatus(sourceStatus);
    } else if (createdWidget || update.kind == SctDocumentUpdateKind::Replacement
        || (update.kind == SctDocumentUpdateKind::RevisionTransition
            && !textOnly && !instructionDelta)) {
        widget->setSnapshot(snapshot, sourceStatus);
    } else if (instructionDelta) {
        incrementalInstructionApplied = widget->applyInstructionChanges(
            snapshot, sourceStatus, update.transition->changes);
        if (!incrementalInstructionApplied) widget->setSourceStatus(sourceStatus);
    } else if (textOnly) {
        widget->applyTextOnlySnapshot(snapshot, sourceStatus, update.transition->changes);
    } else {
        widget->setSourceStatus(sourceStatus);
    }
    widget->setSemanticProjection(update.semanticProjection != nullptr
        ? update.semanticProjection : documentController_->semanticProjection(*found));
    if (!messageEditor_->isCommitting() && messageEditor_->boundLocator().has_value()
        && *messageEditor_->boundLocator() == *found
        && messageEditor_->boundTarget().has_value()
        && affectsMessageTarget(update, *messageEditor_->boundTarget())) {
        if (const auto text = documentController_->workingText(
                widget->locator(), *messageEditor_->boundTarget()); text.has_value()) {
            (void)messageEditor_->refreshText(*text);
        }
    }
    if (update.transition && scptEditor_->boundLocator()
        && *scptEditor_->boundLocator() == *found && scptEditor_->boundSite()) {
        scptEditor_->refreshOrMarkStale(*found, update.transition->changes,
            documentController_->workingParameterExpression(
                *found, *scptEditor_->boundSite()));
    }
    rebuildDocumentTabTitles();
    queueDiagnosticsSync();
    if (widget == activeDocumentWidget()
        && (createdWidget || update.kind == SctDocumentUpdateKind::Replacement
            || update.kind == SctDocumentUpdateKind::VerifiedMaterialization
            || (update.kind == SctDocumentUpdateKind::RevisionTransition && !textOnly))) {
        if (update.kind == SctDocumentUpdateKind::VerifiedMaterialization) {
            semanticNavigator_->installVerifiedDocument(widget->locator(), *snapshot);
        } else if (instructionDelta) {
            (void)semanticNavigator_->applyInstructionChanges(
                widget->locator(), *snapshot, update.transition->changes);
        } else if (!incrementalInstructionApplied) {
            syncSemanticNavigator();
        }
    }
    pruneNavigationHistory();
    syncEditActions();
    if (createdWidget) scheduleWorkspaceSessionSave();
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
        if (scptEditor_->boundLocator().has_value()
            && *scptEditor_->boundLocator() == widget->locator()
            && !scptEditor_->prepareToClear()) return;
        if (!flushMessageEditor()
            || !confirmDiscardDocument(widget->locator(), tr("close the document"),
                PendingLifecycle::CloseDocument)) return;
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
        if (documentController_->isSaving(widget->locator())) title += tr(" [Saving]");
        if (documentController_->patchConflict(widget->locator())) title += tr(" [Patch conflict]");
        tabs_->setTabText(tabIndex, title);
        tabs_->setTabToolTip(tabIndex, path);
    }
}

void MainWindow::scheduleWorkspaceSessionSave() {
    if (restoringWorkspaceSession_ || patchWorkspace_ == nullptr
        || workspaceSessionSaveTimer_ == nullptr) return;
    workspaceSessionSaveTimer_->start();
}

std::optional<core::WorkspaceSessionState> MainWindow::captureWorkspaceSession() const {
    if (patchWorkspace_ == nullptr) return std::nullopt;
    core::WorkspaceSessionState state;
    state.workspaceId = patchWorkspace_->descriptor().workspaceId;
    for (int index = 1; index < tabs_->count(); ++index) {
        const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
        if (widget == nullptr) continue;
        const bool checkpointed = !documentController_->isDirty(widget->locator());
        state.documents.push_back({widget->locator(), widget->activeView(),
            checkpointed ? widget->currentTarget()
                         : std::optional<core::SctNavigationTarget>{}});
    }
    if (const auto* active = activeDocumentWidget()) state.activeDocument = active->locator();
    state.selectedProjectAsset = controller_->selectedLocator();

    std::function<void(const QModelIndex&)> collectExpanded;
    collectExpanded = [this, &state, &collectExpanded](const QModelIndex& parent) {
        for (int row = 0; row < workspaceModel_->rowCount(parent); ++row) {
            const auto index = workspaceModel_->index(row, 0, parent);
            if (!index.isValid()) continue;
            if (projectTree_->isExpanded(index)) {
                if (const auto path = workspaceModel_->logicalDirectoryAt(index))
                    state.expandedProjectDirectories.push_back(*path);
            }
            collectExpanded(index);
        }
    };
    collectExpanded({});

    std::size_t retainedThroughCurrent = 0;
    for (std::size_t index = 0; index < navigationHistory_.size(); ++index) {
        const auto& entry = navigationHistory_[index];
        bool include = !entry.locator && !entry.target;
        if (entry.locator && entry.target) {
            include = !documentController_->isDirty(*entry.locator)
                && navigationEntryAvailable(entry);
        }
        if (!include) continue;
        state.navigation.push_back({entry.locator, entry.target});
        if (index <= navigationHistoryIndex_) retainedThroughCurrent = state.navigation.size();
    }
    state.navigationIndex = state.navigation.empty() || retainedThroughCurrent == 0u
        ? 0u : retainedThroughCurrent - 1u;
    return state;
}

void MainWindow::saveWorkspaceSession() {
    if (restoringWorkspaceSession_ || patchWorkspace_ == nullptr) return;
    if (workspaceSessionSaveTimer_) workspaceSessionSaveTimer_->stop();
    const auto state = captureWorkspaceSession();
    if (!state) return;
    core::WorkspaceSessionStore store;
    const auto saved = store.checkpoint(patchWorkspace_->sessionPath(), *state);
    if (!saved) {
        const auto message = saved.diagnostics().empty()
            ? tr("The workspace session could not be saved.")
            : QString::fromStdString(saved.diagnostics().front().message);
        statusBar()->showMessage(message, 10000);
    }
}

void MainWindow::restoreWorkspaceSession() {
    if (patchWorkspace_ == nullptr) return;
    if (workspaceSessionSaveTimer_) workspaceSessionSaveTimer_->stop();
    core::WorkspaceSessionStore store;
    auto loaded = store.load(patchWorkspace_->sessionPath(),
        patchWorkspace_->descriptor().workspaceId);
    navigationHistory_.clear();
    navigationHistoryIndex_ = 0;
    if (!loaded) {
        const auto message = loaded.diagnostics().empty()
            ? tr("The optional workspace session was ignored.")
            : QString::fromStdString(loaded.diagnostics().front().message);
        statusBar()->showMessage(tr("%1 Durable workspace content remains available.")
            .arg(message), 12000);
        scheduleWorkspaceSessionSave();
        return;
    }
    if (!loaded.value()) {
        recordActiveNavigation();
        scheduleWorkspaceSessionSave();
        return;
    }
    restoringWorkspaceSession_ = true;
    replayingNavigation_ = true;
    restoringWorkspaceSessionState_ = std::move(*loaded.value());
    restoringWorkspaceDocumentIndex_ = 0;
    workspaceRestoreMessages_.clear();
    restoreNextWorkspaceDocument();
}

void MainWindow::restoreNextWorkspaceDocument() {
    if (!restoringWorkspaceSession_ || !restoringWorkspaceSessionState_) return;
    const auto& documents = restoringWorkspaceSessionState_->documents;
    if (activeDatasetOperation_)
        activeDatasetOperation_->reportRestorationProgress(
            restoringWorkspaceDocumentIndex_, documents.size(),
            restoringWorkspaceDocumentIndex_ < documents.size()
                ? QString::fromStdWString(
                    documents[restoringWorkspaceDocumentIndex_].locator.path().wstring())
                : QString{});
    if (activeWorkspaceMaintenance_)
        activeWorkspaceMaintenance_->reportRestorationProgress(
            restoringWorkspaceDocumentIndex_, documents.size(),
            restoringWorkspaceDocumentIndex_ < documents.size()
                ? QString::fromStdWString(
                    documents[restoringWorkspaceDocumentIndex_].locator.path().wstring())
                : QString{});
    if (restoringWorkspaceDocumentIndex_ >= documents.size()) {
        finishWorkspaceSessionRestore();
        return;
    }
    const auto& document = documents[restoringWorkspaceDocumentIndex_];
    const auto* catalog = controller_->catalog();
    const bool exists = catalog != nullptr && std::ranges::any_of(
        catalog->assets, [&document](const auto& asset) {
            return asset.locator == document.locator;
        });
    const auto project = controller_->projectSnapshot();
    if (!exists || !project) {
        workspaceRestoreMessages_.push_back(tr("Missing document: %1")
            .arg(QString::fromStdWString(document.locator.path().wstring())));
        ++restoringWorkspaceDocumentIndex_;
        QTimer::singleShot(0, this, &MainWindow::restoreNextWorkspaceDocument);
        return;
    }
    if (!documentController_->openDocument(*project, document.locator)) {
        workspaceRestoreMessages_.push_back(tr("Could not start restoration of %1")
            .arg(QString::fromStdWString(document.locator.path().wstring())));
        ++restoringWorkspaceDocumentIndex_;
        QTimer::singleShot(0, this, &MainWindow::restoreNextWorkspaceDocument);
    }
}

void MainWindow::continueWorkspaceSessionRestore(
    const QString& identityKey, const bool success, const bool cancelled,
    const QString& message) {
    if (!restoringWorkspaceSession_ || !restoringWorkspaceSessionState_) return;
    const auto& documents = restoringWorkspaceSessionState_->documents;
    if (restoringWorkspaceDocumentIndex_ >= documents.size()) return;
    const auto& state = documents[restoringWorkspaceDocumentIndex_];
    if (QString::fromStdString(state.locator.identityKey()) != identityKey) return;
    if (success) {
        SctDocumentWidget* widget = nullptr;
        for (int index = 1; index < tabs_->count(); ++index) {
            auto* candidate = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
            if (candidate && candidate->locator() == state.locator) {
                widget = candidate;
                break;
            }
        }
        if (widget != nullptr) {
            if (state.selection && widget->containsTarget(*state.selection))
                widget->selectTarget(*state.selection, false);
            else if (state.selection)
                workspaceRestoreMessages_.push_back(tr("Stale selection in %1")
                    .arg(QString::fromStdWString(state.locator.path().wstring())));
            widget->setActiveView(state.view);
        }
    } else {
        workspaceRestoreMessages_.push_back(message);
    }
    ++restoringWorkspaceDocumentIndex_;
    if (cancelled) restoringWorkspaceDocumentIndex_ = documents.size();
    QTimer::singleShot(0, this, &MainWindow::restoreNextWorkspaceDocument);
}

void MainWindow::restoreProjectTreeState(
    const core::WorkspaceSessionState& state) {
    for (const auto& path : state.expandedProjectDirectories) {
        const auto index = workspaceModel_->indexForLogicalDirectory(path);
        if (index.isValid()) projectTree_->setExpanded(index, true);
    }
    if (state.selectedProjectAsset) {
        const auto index = workspaceModel_->indexForLocator(*state.selectedProjectAsset);
        if (index.isValid()) projectTree_->setCurrentIndex(index);
    }
}

void MainWindow::finishWorkspaceSessionRestore() {
    if (!restoringWorkspaceSessionState_) return;
    const auto state = std::move(*restoringWorkspaceSessionState_);
    restoringWorkspaceSessionState_.reset();
    restoreProjectTreeState(state);

    navigationHistory_.clear();
    std::size_t retainedThroughCurrent = 0;
    for (std::size_t index = 0; index < state.navigation.size(); ++index) {
        NavigationEntry entry{state.navigation[index].locator,
            state.navigation[index].target};
        if (!navigationEntryAvailable(entry)) continue;
        navigationHistory_.push_back(std::move(entry));
        if (index <= state.navigationIndex) retainedThroughCurrent = navigationHistory_.size();
    }
    navigationHistoryIndex_ = navigationHistory_.empty() || retainedThroughCurrent == 0u
        ? 0u : retainedThroughCurrent - 1u;

    bool activated = false;
    if (state.activeDocument) {
        for (int index = 1; index < tabs_->count(); ++index) {
            const auto* widget = qobject_cast<SctDocumentWidget*>(tabs_->widget(index));
            if (widget && widget->locator() == *state.activeDocument) {
                tabs_->setCurrentIndex(index);
                activated = true;
                break;
            }
        }
    }
    if (!activated) tabs_->setCurrentIndex(0);
    replayingNavigation_ = false;
    restoringWorkspaceSession_ = false;
    restoringWorkspaceDocumentIndex_ = 0;
    syncNavigationActions();
    syncDiagnostics();
    syncSemanticNavigator();
    syncEditActions();
    if (workspaceRestoreMessages_.isEmpty())
        statusBar()->showMessage(tr("Workspace session restored."), 8000);
    else
        statusBar()->showMessage(tr("Workspace restored with %1 stale or unavailable item(s).")
            .arg(workspaceRestoreMessages_.size()), 12000);
    const auto restorationSummary = workspaceRestoreMessages_.isEmpty()
        ? tr("Workspace session restored.")
        : tr("Workspace restored with %1 stale or unavailable item(s).")
            .arg(workspaceRestoreMessages_.size());
    if (activeDatasetOperation_)
        activeDatasetOperation_->completeRestoration(restorationSummary);
    if (activeWorkspaceMaintenance_)
        activeWorkspaceMaintenance_->completeRestoration(restorationSummary);
    workspaceRestoreMessages_.clear();
    scheduleWorkspaceSessionSave();
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
        && !(exclusiveOperations_ && exclusiveOperations_->active())
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
    auto operation = std::make_unique<WorkspaceOperationController>(
        controller_, WorkspaceController::Operation::Opening, lastDataset_);
    activeDatasetOperation_ = operation.get();
    if (!exclusiveOperations_->open(std::move(operation), this)) {
        activeDatasetOperation_.clear();
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
    scptEditorDock_->hide();

    const auto stored = settings.value(QStringLiteral("workspace/recentDatasets")).toStringList();
    const auto storedLastDataset = settings.value(
        QStringLiteral("workspace/lastDataset"),
        stored.isEmpty() ? QString{} : stored.front()).toString();
    lastDataset_ = normalizedRecentDatasetPath(storedLastDataset);
    lastExportDirectory_ = normalizedRecentDatasetPath(
        settings.value(QStringLiteral("publication/lastDirectory")).toString());
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
    const auto storedAssociations = settings.value(
        QStringLiteral("workspace/patchAssociations")).toStringList();
    for (qsizetype index = 0; index + 1 < storedAssociations.size(); index += 2) {
        const auto datasetRoot = normalizedRecentDatasetPath(storedAssociations[index]);
        const auto workspaceRoot = normalizedRecentDatasetPath(storedAssociations[index + 1]);
        if (!datasetRoot.isEmpty() && !workspaceRoot.isEmpty()) {
            patchWorkspaceAssociations_.push_back(datasetRoot);
            patchWorkspaceAssociations_.push_back(workspaceRoot);
        }
    }
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    settings.setValue(QStringLiteral("workspace/patchAssociations"),
        patchWorkspaceAssociations_);
    rebuildRecentMenu();
}

void MainWindow::saveApplicationSettings() const {
    QSettings settings{};
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState(SettingsStateVersion));
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    settings.setValue(QStringLiteral("workspace/lastDataset"), lastDataset_);
    settings.setValue(QStringLiteral("workspace/patchAssociations"),
        patchWorkspaceAssociations_);
    settings.setValue(QStringLiteral("publication/lastDirectory"),
        lastExportDirectory_);
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
    diagnosticJournalModel_->appendActivity(
        cancelled ? QStringLiteral("DatasetOperationCancelled")
            : success ? QStringLiteral("DatasetOperationCompleted")
                : QStringLiteral("DatasetOperationFailed"),
        message);
    syncActions();

    if (!success && !cancelled) {
        diagnosticsDock_->show();
        diagnosticsDock_->raise();
        if (operation == WorkspaceController::Operation::Opening && !wasRestoreAttempt
            && !(exclusiveOperations_ && exclusiveOperations_->active())) {
            QMessageBox::warning(this, tr("Dataset could not be opened"), message);
        }
    }
    if (success && operation == WorkspaceController::Operation::Opening) {
        documentController_->closeAll();
        detachPatchWorkspace(false);
        restorePatchWorkspaceAssociation();
        QTimer::singleShot(0, this, [this]() {
            if (!restoringWorkspaceSession_ && activeDatasetOperation_)
                activeDatasetOperation_->completeRestoration();
        });
    }
    if (success && operation == WorkspaceController::Operation::Refreshing) {
        if (const auto* catalog = controller_->catalog())
            documentController_->synchronizeCatalog(*catalog);
    }
}

}  // namespace salsa::qt
