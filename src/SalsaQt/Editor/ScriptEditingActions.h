#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QObject>
class QWidget;
class QAction;
namespace salsa::qt {
std::optional<spice::sct::SctCanonicalExpression> editExpression(QWidget *, const spice::sct::SctCanonicalExpression &);
class ScriptEditingActions final : public QObject {
  public:
    ScriptEditingActions(EditorSessionPresenter *, QWidget *);
    QAction *insertAction() const { return insert_; }
    QAction *structureAction() const { return structure_; }
    QAction *editAction() const { return edit_; }
    QAction *timingAction() const { return timing_; }
    QAction *promoteAction() const { return promote_; }

  private:
    void insert();
    void createStructure();
    void edit();
    void timing();
    void arms(core::editor::StructureDraft);
    EditorSessionPresenter *session_;
    QWidget *host_;
    QAction *insert_, *structure_, *edit_, *timing_, *promote_;
};
} // namespace salsa::qt
