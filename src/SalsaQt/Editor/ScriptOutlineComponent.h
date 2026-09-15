#pragma once
#include "Sct/ProgramOutlineModel.h"
#include <QTreeView>
namespace salsa::qt {
class EditorSessionPresenter;
// Hosts its own view/model and selection wiring, independently of the shell.
class ScriptOutlineComponent final : public QTreeView {
  public:
    explicit ScriptOutlineComponent(EditorSessionPresenter *, QWidget *parent = nullptr);
    ProgramOutlineModel *outlineModel() const { return model_; }
    void bind(core::SctScriptId);
    void activate();
    std::function<void()> selectionChanged, editRequested;
  private:
    EditorSessionPresenter *session_;
    ProgramOutlineModel *model_;
};
}
