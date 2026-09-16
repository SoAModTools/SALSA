#pragma once
#include "Sct/ProgramOutlineModel.h"
#include <QWidget>
class QTreeView;
class QTabBar;
class QComboBox;
namespace salsa::qt {
class EditorSessionPresenter;
class ScriptOutlineComponent final : public QWidget {
  public:
    explicit ScriptOutlineComponent(EditorSessionPresenter *, QWidget *parent = nullptr);
    ProgramOutlineModel *outlineModel() const { return model_; }
    void bind(core::SctScriptId);
    void bindModule(core::SctScriptId, core::SctModuleId);
    void bindSequence(core::SctScriptId, core::SctSequenceId);
    std::vector<core::editor::Subject> selectedItems() const;
    core::editor::OutlineViewState viewState(core::editor::ViewMode) const;
    void setViewStates(core::editor::OutlineViewState instructions, core::editor::OutlineViewState semantic);
    void restoreSession(std::function<void()> = {});
    void activate();
    void setSemantic(bool);
    void reveal(core::editor::Subject, std::function<void()> = {});
    void cancelReveal() { ++navigationGeneration_; }
    QModelIndex currentIndex() const;
    void setCurrentIndex(const QModelIndex &);
    void scrollTo(const QModelIndex &);
    void expand(const QModelIndex &);
    std::function<void()> selectionChanged, editRequested, modeChanged, presentationChanged;

  private:
    EditorSessionPresenter *session_;
    ProgramOutlineModel *model_;
    QTreeView *tree_;
    QTabBar *modes_;
    QComboBox *localization_;
    void refreshLanguages();
    core::SctScriptId script_;
    std::optional<core::SctModuleId> module_;
    std::optional<core::SctSequenceId> sequence_;
    std::uint64_t navigationGeneration_ = 0, sessionRestoreGeneration_ = 0;
    bool restoring_ = false, restoringSession_ = false;
    core::editor::OutlineViewState viewStates_[2];
    std::vector<core::editor::Subject> restoreSelection_, restoreExpanded_;
    std::optional<core::editor::Subject> restoreCurrent_;
    void restoreVisible();
};
} // namespace salsa::qt
