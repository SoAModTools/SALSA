#pragma once
#include <QWidget>
#include <functional>
class QAction;
class QLabel;
class QPlainTextEdit;
class QStackedWidget;
namespace salsa::qt {
class EditorSessionPresenter;
class InstructionInspector;
class TextEditorComponent;
// Selection-specific components share one dock. Full editing is explicitly opened.
class ContextInspector final : public QWidget {
  public:
    explicit ContextInspector(EditorSessionPresenter *, QWidget *parent = nullptr);
    QAction *editParameterAction() const;
    QAction *editTextAction() const { return editText_; }
    QAction *createTextAction() const { return createText_; }
    QAction *deleteTextAction() const { return deleteText_; }
    bool prepareToLeave();
    std::function<void()> showRequested;

  private:
    void refresh();
    void availability();
    void ensureEditor();
    void editText();
    EditorSessionPresenter *session_;
    InstructionInspector *instructions_;
    QStackedWidget *pages_;
    QWidget *message_, *editing_ = nullptr;
    QLabel *name_, *language_;
    QPlainTextEdit *content_;
    TextEditorComponent *editor_ = nullptr;
    QAction *editText_, *createText_, *deleteText_;
};
} // namespace salsa::qt
