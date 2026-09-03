#pragma once

#include "Workspace/WorkspaceController.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceSession.h"
#include "SalsaCore/Sct/SctEditSession.h"

#include <QMainWindow>
#include <QStringList>

#include <optional>
#include <vector>

class QAction;
class QCloseEvent;
class QDockWidget;
class QMenu;
class QProgressBar;
class QTableView;
class QTabWidget;
class QToolButton;
class QTreeView;
class QTimer;

namespace salsa::qt {

class DiagnosticsModel;
class DiagnosticJournalModel;
class SctDocumentController;
struct SctDocumentUpdate;
class SctDocumentWidget;
class SctMessageEditorWidget;
class SctScptEditorWidget;
class SctSemanticNavigatorWidget;
class WorkspaceDetailsWidget;
class WorkspaceModel;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class PendingLifecycle { None, CloseDocument, CloseDataset, Exit };
    struct NavigationEntry final {
        std::optional<core::AssetLocator> locator{};
        std::optional<core::SctNavigationTarget> target{};
        bool operator==(const NavigationEntry&) const = default;
    };

    void buildUi();
    void connectWorkspace();
    void chooseDataset();
    void openDataset(const QString& rootPath);
    void associatePatchWorkspace();
    [[nodiscard]] bool openPatchWorkspace(
        const QString& workspaceRoot, bool allowConfirmation);
    void disconnectPatchWorkspace();
    void detachPatchWorkspace(bool saveSession);
    void restorePatchWorkspaceAssociation();
    void rememberPatchWorkspaceAssociation(
        const QString& datasetRoot, const QString& workspaceRoot);
    void saveActiveDocument();
    void exportActiveDocument();
    void syncWorkspace();
    void syncSelection();
    void syncDiagnostics();
    void queueDiagnosticsSync();
    void syncActions();
    void activateSelectedAsset();
    void syncDocument(const QString& identityKey, const SctDocumentUpdate& update);
    void syncSemanticNavigator();
    void focusDocument(const QString& identityKey);
    void closeDocumentTab(int index);
    void rebuildDocumentTabTitles();
    void syncEditActions();
    void recordActiveNavigation();
    void recordNavigation(NavigationEntry entry);
    void pruneNavigationHistory();
    void navigateBack();
    void navigateForward();
    [[nodiscard]] bool navigateTo(const NavigationEntry& entry);
    [[nodiscard]] bool navigationEntryAvailable(const NavigationEntry& entry) const;
    [[nodiscard]] QString navigationEntryLabel(const NavigationEntry& entry) const;
    void syncNavigationActions();
    void insertInstruction();
    void deleteInstruction();
    void moveInstruction(core::SctInstructionMoveDirection direction);
    void editSelectedMessage();
    void createScriptSection();
    void createIndexedString();
    void renameSelectedSection();
    void deleteSelectedSection();
    void moveSelectedSection(core::SctSectionMoveDirection direction);
    void createFooterText(core::SctCreatedFooterTextKind kind);
    void deleteSelectedText();
    [[nodiscard]] bool flushMessageEditor();
    [[nodiscard]] bool prepareScptEditor(
        const std::optional<core::AssetLocator>& locator = std::nullopt);
    void undoActiveDocument();
    void redoActiveDocument();
    [[nodiscard]] SctDocumentWidget* activeDocumentWidget() const;
    [[nodiscard]] std::optional<core::SctInstructionAuthoringDraft>
        chooseInstructionDraft(const core::AssetLocator& locator, bool allowReturn);
    [[nodiscard]] std::optional<std::uint16_t> chooseSemanticArmOpcode();
    [[nodiscard]] std::optional<spice::sct::SctDocumentRepeatedParameterGroup>
        configureRepeatedGroupDraft(
            const core::AssetLocator& locator,
            spice::sct::SctInstructionId instruction,
            spice::sct::SctRepeatedParameterGroupDraft draft);
    [[nodiscard]] bool confirmDiscardDocument(
        const core::AssetLocator& locator, const QString& action,
        PendingLifecycle pending = PendingLifecycle::None);
    [[nodiscard]] bool confirmDiscardAll(
        const QString& action, PendingLifecycle pending = PendingLifecycle::None);
    void continuePendingLifecycle(
        const QString& identityKey, bool success, bool cancelled);
    void rebuildRecentMenu();
    void recordRecentDataset(const QString& canonicalRoot);
    void attemptRestoreDataset();
    void restoreApplicationSettings();
    void saveApplicationSettings() const;
    void scheduleWorkspaceSessionSave();
    void saveWorkspaceSession();
    [[nodiscard]] std::optional<core::WorkspaceSessionState>
        captureWorkspaceSession() const;
    void restoreWorkspaceSession();
    void restoreNextWorkspaceDocument();
    void continueWorkspaceSessionRestore(
        const QString& identityKey, bool success, bool cancelled,
        const QString& message);
    void finishWorkspaceSessionRestore();
    void restoreProjectTreeState(const core::WorkspaceSessionState& state);
    void handleOperationCompleted(
        WorkspaceController::Operation operation,
        bool success,
        bool cancelled,
        const QString& message);

    WorkspaceController* controller_ = nullptr;
    SctDocumentController* documentController_ = nullptr;
    WorkspaceModel* workspaceModel_ = nullptr;
    DiagnosticsModel* diagnosticsModel_ = nullptr;
    DiagnosticJournalModel* diagnosticJournalModel_ = nullptr;
    WorkspaceDetailsWidget* details_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeView* projectTree_ = nullptr;
    QTableView* diagnosticsView_ = nullptr;
    QTableView* activityLogView_ = nullptr;
    QDockWidget* projectDock_ = nullptr;
    QDockWidget* diagnosticsDock_ = nullptr;
    QDockWidget* activityLogDock_ = nullptr;
    QDockWidget* semanticNavigatorDock_ = nullptr;
    QDockWidget* messageEditorDock_ = nullptr;
    QDockWidget* scptEditorDock_ = nullptr;
    SctSemanticNavigatorWidget* semanticNavigator_ = nullptr;
    SctMessageEditorWidget* messageEditor_ = nullptr;
    SctScptEditorWidget* scptEditor_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QToolButton* cancelButton_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* closeWorkspaceAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* associatePatchWorkspaceAction_ = nullptr;
    QAction* disconnectPatchWorkspaceAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* navigationBackAction_ = nullptr;
    QAction* navigationForwardAction_ = nullptr;
    QAction* editMessageAction_ = nullptr;
    QAction* createScriptSectionAction_ = nullptr;
    QAction* createIndexedStringAction_ = nullptr;
    QAction* renameSectionAction_ = nullptr;
    QAction* deleteSectionAction_ = nullptr;
    QAction* moveSectionUpAction_ = nullptr;
    QAction* moveSectionDownAction_ = nullptr;
    QAction* createFooterMessageAction_ = nullptr;
    QAction* deleteTextAction_ = nullptr;
    QAction* insertInstructionAction_ = nullptr;
    QAction* deleteInstructionAction_ = nullptr;
    QAction* moveInstructionUpAction_ = nullptr;
    QAction* moveInstructionDownAction_ = nullptr;
    QAction* logSctEditTimingsAction_ = nullptr;
    QAction* showStructuredBasicBlocksAction_ = nullptr;
    QAction* showRejectedStructureEvidenceAction_ = nullptr;
    QAction* showSemanticControlFlowInstructionsAction_ = nullptr;
    QAction* logStructureAnalysisAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* developerMenu_ = nullptr;
    QStringList recentDatasets_{};
    QStringList patchWorkspaceAssociations_{};
    QString lastDataset_{};
    QString lastExportDirectory_{};
    std::shared_ptr<const core::LocalSalsaWorkspace> patchWorkspace_{};
    QTimer* workspaceSessionSaveTimer_ = nullptr;
    std::optional<core::WorkspaceSessionState> restoringWorkspaceSessionState_{};
    std::size_t restoringWorkspaceDocumentIndex_ = 0;
    QStringList workspaceRestoreMessages_{};
    bool restoringDataset_ = false;
    bool restoringWorkspaceSession_ = false;
    bool diagnosticsSyncPending_ = false;
    bool editTimingsEnabled_ = false;
    bool showStructuredBasicBlocks_ = false;
    bool showRejectedStructureEvidence_ = false;
    bool showSemanticControlFlowInstructions_ = false;
    bool structureAnalysisTimingsEnabled_ = false;
    bool restoringTabAfterCommitFailure_ = false;
    bool replayingNavigation_ = false;
    std::vector<NavigationEntry> navigationHistory_{};
    std::size_t navigationHistoryIndex_ = 0;
    PendingLifecycle pendingLifecycle_ = PendingLifecycle::None;
    std::optional<core::AssetLocator> pendingLifecycleDocument_{};
    std::vector<core::AssetLocator> pendingLifecycleSaves_{};
};

}  // namespace salsa::qt
