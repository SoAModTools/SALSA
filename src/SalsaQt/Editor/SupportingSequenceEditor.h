#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include <QWidget>
class QComboBox;
class QLabel;
class QAction;
namespace salsa::qt {
class EditorSessionPresenter;
class ScriptOutlineComponent;
class ScriptEditingActions;
// A scoped view of existing sequences, sharing selection, editing and history.
class SupportingSequenceEditor final : public QWidget {
  public:
    explicit SupportingSequenceEditor(EditorSessionPresenter *, QWidget *parent = nullptr);
  private:
    void refresh();
    void showPhase(int, bool select);
    EditorSessionPresenter *session_;
    ScriptOutlineComponent *outline_;
    ScriptEditingActions *editing_;
    QComboBox *phases_;
    QLabel *timing_;
    QAction *append_, *remove_, *selectPhase_, *ownerAction_;
    std::optional<core::editor::Subject> owner_;
    core::SctScriptId script_;
    std::vector<core::editor::SupportingSequence> sequences_;
};
} // namespace salsa::qt
