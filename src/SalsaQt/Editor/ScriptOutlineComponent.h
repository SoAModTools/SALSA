#pragma once
#include "Sct/ProgramOutlineModel.h"
#include <QWidget>
class QTreeView;
class QTabBar;
namespace salsa::qt {
class EditorSessionPresenter;
class ScriptOutlineComponent final : public QWidget {
  public:
    explicit ScriptOutlineComponent(EditorSessionPresenter *, QWidget *parent = nullptr);
    ProgramOutlineModel *outlineModel() const { return model_; }
    void bind(core::SctScriptId);
    void bindModule(core::SctScriptId, core::SctModuleId);
    std::vector<core::editor::Subject> selectedItems() const;
    void activate();
    void setSemantic(bool);
    void reveal(core::editor::Subject, std::function<void()> = {});
    QModelIndex currentIndex() const;
    void setCurrentIndex(const QModelIndex &);
    void scrollTo(const QModelIndex &);
    void expand(const QModelIndex &);
    std::function<void()> selectionChanged, editRequested, modeChanged;

  private:
    EditorSessionPresenter *session_;
    ProgramOutlineModel *model_;
    QTreeView *tree_;
    QTabBar *modes_;
    core::SctScriptId script_;
    std::optional<core::SctModuleId> module_;
    std::uint64_t navigationGeneration_ = 0;
    bool restoring_ = false;
    std::vector<core::editor::Subject> restoreSelection_, restoreExpanded_;
    std::optional<core::editor::Subject> restoreCurrent_;
    void restoreVisible();
};
} // namespace salsa::qt
