#include "Application/MainWindow.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "Workspace/DiagnosticsModel.h"
#include "Workspace/WorkspaceDetailsWidget.h"
#include "Workspace/WorkspaceModel.h"

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <optional>
#include <ranges>

namespace salsa::qt {
namespace {

constexpr int SettingsStateVersion = 1;
constexpr qsizetype MaximumRecentDatasets = 10;

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
    syncActions();
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveApplicationSettings();
    controller_->cancel();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
    controller_ = new WorkspaceController(this);
    workspaceModel_ = new WorkspaceModel(this);
    diagnosticsModel_ = new DiagnosticsModel(this);
    details_ = new WorkspaceDetailsWidget(this);
    setCentralWidget(details_);

    projectTree_ = new QTreeView(this);
    projectTree_->setModel(workspaceModel_);
    projectTree_->setAlternatingRowColors(true);
    projectTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    projectTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    projectTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    projectDock_ = new QDockWidget(tr("Project Explorer"), this);
    projectDock_->setObjectName(QStringLiteral("ProjectExplorerDock"));
    projectDock_->setWidget(projectTree_);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock_);

    diagnosticsView_ = new QTableView(this);
    diagnosticsView_->setModel(diagnosticsModel_);
    diagnosticsView_->setAlternatingRowColors(true);
    diagnosticsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    diagnosticsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    diagnosticsView_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    diagnosticsView_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    diagnosticsView_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    diagnosticsView_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    diagnosticsDock_ = new QDockWidget(tr("Diagnostics"), this);
    diagnosticsDock_->setObjectName(QStringLiteral("DiagnosticsDock"));
    diagnosticsDock_->setWidget(diagnosticsView_);
    addDockWidget(Qt::BottomDockWidgetArea, diagnosticsDock_);

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

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(projectDock_->toggleViewAction());
    viewMenu->addAction(diagnosticsDock_->toggleViewAction());

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
        controller_->closeWorkspace();
        statusBar()->showMessage(tr("Workspace closed."), 5000);
    });
    connect(refreshAction_, &QAction::triggered, this, [this]() {
        if (controller_->refresh()) {
            statusBar()->showMessage(tr("Refreshing dataset..."));
        }
    });
    connect(cancelButton_, &QToolButton::clicked, controller_, &WorkspaceController::cancel);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

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
        projectTree_->expandToDepth(0);
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
    diagnosticsModel_->setDiagnostics(controller_->diagnostics());
}

void MainWindow::syncActions() {
    const bool busy = controller_->busy();
    openAction_->setEnabled(!busy);
    recentMenu_->setEnabled(!busy && !recentDatasets_.isEmpty());
    closeWorkspaceAction_->setEnabled(controller_->hasWorkspace() || busy);
    refreshAction_->setEnabled(controller_->hasWorkspace() && !busy);
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
    recentMenu_->setEnabled(!controller_->busy() && !recentDatasets_.isEmpty());
}

void MainWindow::recordRecentDataset(const QString& canonicalRoot) {
    recentDatasets_.erase(
        std::remove_if(
            recentDatasets_.begin(),
            recentDatasets_.end(),
            [&canonicalRoot](const QString& existing) {
                return existing.compare(canonicalRoot, Qt::CaseInsensitive) == 0;
            }),
        recentDatasets_.end());
    recentDatasets_.push_front(canonicalRoot);
    while (recentDatasets_.size() > MaximumRecentDatasets) {
        recentDatasets_.removeLast();
    }
    QSettings settings{};
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    rebuildRecentMenu();
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

    const auto stored = settings.value(QStringLiteral("workspace/recentDatasets")).toStringList();
    for (const auto& root : stored) {
        if (QFileInfo(root).isDir() && !std::ranges::any_of(
                recentDatasets_,
                [&root](const QString& existing) {
                    return existing.compare(root, Qt::CaseInsensitive) == 0;
                })) {
            recentDatasets_.push_back(QDir::cleanPath(root));
            if (recentDatasets_.size() == MaximumRecentDatasets) {
                break;
            }
        }
    }
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
    rebuildRecentMenu();
}

void MainWindow::saveApplicationSettings() const {
    QSettings settings{};
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState(SettingsStateVersion));
    settings.setValue(QStringLiteral("workspace/recentDatasets"), recentDatasets_);
}

void MainWindow::handleOperationCompleted(
    const WorkspaceController::Operation operation,
    const bool success,
    const bool cancelled,
    const QString& message) {
    progressBar_->hide();
    cancelButton_->hide();
    statusBar()->showMessage(message, 8000);
    syncActions();

    if (!success && !cancelled) {
        diagnosticsDock_->show();
        diagnosticsDock_->raise();
        if (operation == WorkspaceController::Operation::Opening) {
            QMessageBox::warning(this, tr("Dataset could not be opened"), message);
        }
    }
}

}  // namespace salsa::qt
