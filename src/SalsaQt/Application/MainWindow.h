#pragma once

#include "Workspace/WorkspaceController.h"

#include <QMainWindow>
#include <QStringList>

class QAction;
class QCloseEvent;
class QDockWidget;
class QMenu;
class QProgressBar;
class QTableView;
class QToolButton;
class QTreeView;

namespace salsa::qt {

class DiagnosticsModel;
class WorkspaceDetailsWidget;
class WorkspaceModel;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void connectWorkspace();
    void chooseDataset();
    void openDataset(const QString& rootPath);
    void syncWorkspace();
    void syncSelection();
    void syncDiagnostics();
    void syncActions();
    void rebuildRecentMenu();
    void recordRecentDataset(const QString& canonicalRoot);
    void restoreApplicationSettings();
    void saveApplicationSettings() const;
    void handleOperationCompleted(
        WorkspaceController::Operation operation,
        bool success,
        bool cancelled,
        const QString& message);

    WorkspaceController* controller_ = nullptr;
    WorkspaceModel* workspaceModel_ = nullptr;
    DiagnosticsModel* diagnosticsModel_ = nullptr;
    WorkspaceDetailsWidget* details_ = nullptr;
    QTreeView* projectTree_ = nullptr;
    QTableView* diagnosticsView_ = nullptr;
    QDockWidget* projectDock_ = nullptr;
    QDockWidget* diagnosticsDock_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QToolButton* cancelButton_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* closeWorkspaceAction_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QStringList recentDatasets_{};
};

}  // namespace salsa::qt
