#pragma once
#include <QWidget>
class QLabel;
class QTableWidget;
class QAction;
namespace salsa::qt {
class EditorSessionPresenter;
class InstructionInspector final : public QWidget {
  public:
    explicit InstructionInspector(EditorSessionPresenter *, QWidget *parent = nullptr);
    QAction *editParameterAction() const { return edit_; }
  private:
    void refresh();
    void availability();
    void editParameter();
    EditorSessionPresenter *session_;
    QLabel *title_, *description_;
    QTableWidget *parameters_;
    QAction *edit_;
};
}
