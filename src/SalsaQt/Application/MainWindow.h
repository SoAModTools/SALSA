#pragma once

#include "Workspace/WorkspaceController.h"
#include "SalsaCore/Sct/SctEditSession.h"

#include <QMainWindow>
#include <QStringList>

#include <optional>

class QAction;
class QCloseEvent;
class QDockWidget;
class QMenu;
class QProgressBar;
class QTableView;
class QTabWidget;
class QToolButton;
class QTreeView;

namespace salsa::qt {

class DiagnosticsModel;
class SctDocumentController;
class SctDocumentWidget;
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
    void activateSelectedAsset();
    void syncDocument(const QString& identityKey);
    void focusDocument(const QString& identityKey);
    void closeDocumentTab(int index);
    void rebuildDocumentTabTitles();
    void syncEditActions();
    void insertInstruction();
    void deleteInstruction();
    void moveInstruction(core::SctInstructionMoveDirection direction);
    void undoActiveDocument();
    void redoActiveDocument();
    [[nodiscard]] SctDocumentWidget* activeDocumentWidget() const;
    [[nodiscard]] std::optional<std::uint16_t> chooseInsertableOpcode(bool allowReturn);
    [[nodiscard]] bool confirmDiscardDocument(
        const core::AssetLocator& locator, const QString& action);
    [[nodiscard]] bool confirmDiscardAll(const QString& action);
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
    SctDocumentController* documentController_ = nullptr;
    WorkspaceModel* workspaceModel_ = nullptr;
    DiagnosticsModel* diagnosticsModel_ = nullptr;
    WorkspaceDetailsWidget* details_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeView* projectTree_ = nullptr;
    QTableView* diagnosticsView_ = nullptr;
    QDockWidget* projectDock_ = nullptr;
    QDockWidget* diagnosticsDock_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QToolButton* cancelButton_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* closeWorkspaceAction_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* insertInstructionAction_ = nullptr;
    QAction* deleteInstructionAction_ = nullptr;
    QAction* moveInstructionUpAction_ = nullptr;
    QAction* moveInstructionDownAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QStringList recentDatasets_{};
};

}  // namespace salsa::qt
