#pragma once

#include "Workspace/WorkspaceController.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceSession.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctFragment.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include <QMainWindow>
#include <QByteArray>
#include <QPoint>
#include <QPointer>
#include <QStringList>

#include <optional>
#include <vector>

class QAction;
class QCloseEvent;
class QDockWidget;
class QLineEdit;
class QListWidget;
class QMenu;
class QProgressBar;
class QPushButton;
class QTableView;
class QTableWidget;
class QTabWidget;
class QToolButton;
class QToolBar;
class QTreeView;
class QTimer;

namespace salsa::qt {

class ExclusiveOperationCoordinator;
class DisabledActionHintPresenter;
class HelpWindow;
class DiagnosticsModel;
class ActivityLogModel;
struct InteractionNotice;
class TransientNoticePresenter;
class SctDocumentController;
struct SctDocumentUpdate;
class SctDocumentWidget;
class SctMessageEditorWidget;
class SctMetadataEditorWidget;
class SctScptEditorWidget;
class SctSemanticNavigatorWidget;
class DatasetOverviewWidget;
class WorkspaceModel;
class WorkspaceOperationController;
class WorkspaceMaintenanceController;

class MainWindow final : public QMainWindow {
public:
    enum class Mode { Application, IsolatedDocumentEditor };

    explicit MainWindow(QWidget* parent = nullptr);
    MainWindow(Mode mode, QWidget* parent);
    [[nodiscard]] bool installSemanticCandidate(
        const core::AssetLocator& locator,
        std::shared_ptr<const core::SctDocumentSnapshot> provenanceSnapshot,
        const core::SctSemanticState& state);
    [[nodiscard]] std::optional<core::SctSemanticState> captureSemanticCandidate(
        const core::AssetLocator& locator);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class PendingLifecycle {
        None,
        CloseDocument,
        CloseDataset,
        OpenDataset,
        RebasePatches,
        Exit,
    };
    struct NavigationEntry final {
        std::optional<core::AssetLocator> locator{};
        std::optional<core::SctNavigationTarget> target{};
        bool operator==(const NavigationEntry&) const = default;
    };
    struct ActionAvailability final {
        bool enabled = false;
        QString reason{};
    };

    void buildUi();
    void applyActionAvailability(QAction* action, ActionAvailability availability);
    void syncDatasetOverview();
    void syncWindowTitle();
    void showHelpTopic(const QString& topic);
    void resetWindowLayout();
    void connectWorkspace();
    void chooseDataset();
    void convertLegacyProject();
    void openDataset(const QString& rootPath);
    void associatePatchWorkspace();
    void rebaseStalePatches();
    void cleanWorkspaceEvidence();
    void promoteLegacyMetadata();
    void editInstructionCatalog();
    void editProjectOpcodeColors();
    void addVariableAlias(bool projectScope);
    void removeSelectedVariableAlias();
    void editSelectedVariableMetadata();
    void reloadAuthoringDocks();
    [[nodiscard]] bool flushMetadataEditor();
    [[nodiscard]] bool syncMetadataEditor();
    [[nodiscard]] bool saveWorkspaceAuthoring();
    void loadWorkspaceAuthoring();
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
    void showInteractionNotice(InteractionNotice notice);
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
    [[nodiscard]] std::optional<core::SctSemanticFragment>
        captureSelectedFragment(bool reportFailure = true);
    [[nodiscard]] bool copyFragmentToClipboard(
        const core::SctSemanticFragment& fragment);
    void copySelection();
    void cutSelection();
    void pasteSelection();
    void duplicateSelection();
    void deleteSelection();
    void saveSelectionAsSnippet();
    void pasteSelectedSnippet();
    void deleteSelectedSnippet();
    void reloadSnippets();
    [[nodiscard]] bool pasteFragment(const core::SctSemanticFragment& fragment,
        bool duplicate = false);
    void editSelectedMessage();
    void createScriptSection();
    void createIndexedString();
    void renameSelectedSection();
    void deleteSelectedSection();
    void moveSelectedSection(core::SctSectionMoveDirection direction);
    void createSupplementaryText(core::SctCreatedSupplementaryTextKind kind);
    void deleteSelectedText();
    [[nodiscard]] bool flushMessageEditor();
    [[nodiscard]] bool flushPendingEditors();
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
    void closeDataset();
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
    ExclusiveOperationCoordinator* exclusiveOperations_ = nullptr;
    QPointer<WorkspaceOperationController> activeDatasetOperation_{};
    QPointer<WorkspaceMaintenanceController> activeWorkspaceMaintenance_{};
    SctDocumentController* documentController_ = nullptr;
    WorkspaceModel* workspaceModel_ = nullptr;
    DiagnosticsModel* diagnosticsModel_ = nullptr;
    ActivityLogModel* activityLogModel_ = nullptr;
    TransientNoticePresenter* noticePresenter_ = nullptr;
    DisabledActionHintPresenter* actionHints_ = nullptr;
    HelpWindow* helpWindow_ = nullptr;
    DatasetOverviewWidget* details_ = nullptr;
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
    QDockWidget* snippetLibraryDock_ = nullptr;
    QDockWidget* aliasDock_ = nullptr;
    QDockWidget* bookmarkDock_ = nullptr;
    QDockWidget* metadataDock_ = nullptr;
    SctSemanticNavigatorWidget* semanticNavigator_ = nullptr;
    SctMessageEditorWidget* messageEditor_ = nullptr;
    SctMetadataEditorWidget* metadataEditor_ = nullptr;
    SctScptEditorWidget* scptEditor_ = nullptr;
    QLineEdit* snippetSearch_ = nullptr;
    QListWidget* snippetList_ = nullptr;
    QTableWidget* aliasTable_ = nullptr;
    QTableWidget* bookmarkTable_ = nullptr;
    QPushButton* editAliasMetadataButton_ = nullptr;
    QPushButton* pasteSnippetButton_ = nullptr;
    QPushButton* deleteSnippetButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QToolButton* cancelButton_ = nullptr;
    QToolBar* editingToolbar_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* convertLegacyProjectAction_ = nullptr;
    QAction* closeWorkspaceAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* associatePatchWorkspaceAction_ = nullptr;
    QAction* disconnectPatchWorkspaceAction_ = nullptr;
    QAction* rebasePatchesAction_ = nullptr;
    QAction* cleanWorkspaceEvidenceAction_ = nullptr;
    QAction* promoteLegacyMetadataAction_ = nullptr;
    QAction* editInstructionCatalogAction_ = nullptr;
    QAction* editProjectOpcodeColorsAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    QAction* duplicateAction_ = nullptr;
    QAction* saveSnippetAction_ = nullptr;
    QAction* navigationBackAction_ = nullptr;
    QAction* navigationForwardAction_ = nullptr;
    QAction* datasetOverviewAction_ = nullptr;
    QAction* editMessageAction_ = nullptr;
    QAction* createScriptSectionAction_ = nullptr;
    QAction* createIndexedStringAction_ = nullptr;
    QAction* renameSectionAction_ = nullptr;
    QAction* deleteSectionAction_ = nullptr;
    QAction* moveSectionUpAction_ = nullptr;
    QAction* moveSectionDownAction_ = nullptr;
    QAction* createSupplementaryTextMessageAction_ = nullptr;
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
    QByteArray defaultWindowState_{};
    QStringList recentDatasets_{};
    QStringList patchWorkspaceAssociations_{};
    QString lastDataset_{};
    QString lastExportDirectory_{};
    std::shared_ptr<const core::LocalSalsaWorkspace> patchWorkspace_{};
    core::SctPersonalCatalog personalCatalog_{};
    std::filesystem::path personalCatalogPath_{};
    core::SctWorkspaceAuthoringState workspaceAuthoring_{};
    struct LoadedSnippet final {
        bool workspace = false;
        core::SctSnippet snippet{};
    };
    std::vector<LoadedSnippet> loadedSnippets_{};
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
    bool restoringMetadataTransition_ = false;
    bool replayingNavigation_ = false;
    std::vector<NavigationEntry> navigationHistory_{};
    std::size_t navigationHistoryIndex_ = 0;
    PendingLifecycle pendingLifecycle_ = PendingLifecycle::None;
    std::optional<core::AssetLocator> pendingLifecycleDocument_{};
    std::vector<core::AssetLocator> pendingLifecycleSaves_{};
    QString pendingDatasetRoot_{};
    std::optional<QPoint> pendingInteractionPosition_{};
    Mode mode_ = Mode::Application;
};

}  // namespace salsa::qt
