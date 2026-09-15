#pragma once
#include "SalsaCore/Program/EditorWorkspace.h"
#include "Sct/ProgramOutlineModel.h"
#include <QMainWindow>
#include <QPointer>
#include <QTreeView>
class QTabWidget;
class QListWidget;
class QPlainTextEdit;
class QLabel;
class QAction;
class QMenu;
class QCloseEvent;
namespace salsa::qt {
class ExclusiveOperationCoordinator;
class ProgramOperation;
class EditorSessionPresenter;
class InstructionInspector;
class ScriptOutlineComponent;
class TextEditorComponent;
class ScriptEditingActions;
class NavigatorComponent;
class WorkspaceComponent;
// Composes the workspace, feature components and shared editor session.
// Remaining backend-specific advanced handlers are quarantined in
// ProgramWorkspaceAdvanced.cpp and are not the feature component contract.
class ProgramWorkspaceController final : public QObject {
  public:
    explicit ProgramWorkspaceController(QMainWindow *shell);
    ~ProgramWorkspaceController() override;

    void closeRequested(QCloseEvent *);

  private:
    QMainWindow *shell_;
    struct Tab {
        ScriptOutlineComponent *view = nullptr;
        ProgramOutlineModel *model = nullptr;
        core::SctScriptId script;
        std::optional<core::program::EditorSubject> selection;
    };
    void openDataset(QString root = {}, QString workspace = {}, bool legacy = false);
    void install(std::shared_ptr<core::program::EditorWorkspace>, ProgramOperation &);
    void restoreTabs(QPointer<ProgramOperation>, std::size_t = 0);
    void openScript(core::SctScriptId, std::function<void()> ready = {});
    void select(Tab &, const QModelIndex &);
    bool prepareSelection(std::function<void()>);
    void reveal(Tab &, core::program::EditorSubject, std::function<void()> = {});
    void editSelected();
    void renameSection();
    void edit(std::vector<core::program::Primitive>);
    void changed(const core::program::ChangeReceipt &);
    void refreshChanged(const core::editor::Change &);
    void save(std::function<void()> = {});
    void publish();
    void importLegacy();
    void promoteFlow();
    void editText();
    void inspectLowered();
    void updateTitle();
    void log(QString);
    bool readyToLeave();
    std::optional<core::program::ValueBuffer> chooseTarget(core::program::ValueKind);
    core::program::Presentation presentation() const;
    Tab *active();
    const core::program::ScriptSummary *script(core::SctScriptId) const;
    void recentMenu();
    void reviewSources();
    void markSession();
    std::shared_ptr<core::program::EditorWorkspace> workspace_;
    ExclusiveOperationCoordinator *operations_;
    QTabWidget *tabs_;
    QListWidget *scripts_;
    WorkspaceComponent *workspaceView_;
    InstructionInspector *inspector_;
    TextEditorComponent *textEditor_;
    ScriptEditingActions *editing_;
    EditorSessionPresenter *editor_;
    QPlainTextEdit *activity_;
    QLabel *status_;
    QMenu *recent_;
    std::vector<Tab> documents_;
    std::shared_ptr<const core::program::ReadScope> selected_;
    std::optional<core::program::EditorSubject> selectedSubject_;
    std::uint64_t selectionGeneration_ = 0, workspaceGeneration_ = 0;
    bool restoring_ = false, preserveSession_ = false, sessionDirty_ = false, closing_ = false;
    QStringList recents_, associations_;
};
} // namespace salsa::qt
