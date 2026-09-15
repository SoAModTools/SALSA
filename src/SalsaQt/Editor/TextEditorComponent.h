#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QWidget>
class QLabel;
class QAction;
class QTextBrowser;
namespace salsa::qt {
class SctMessageEditorWidget;
class TextEditorComponent final : public QWidget {
  public:
    explicit TextEditorComponent(EditorSessionPresenter *, QWidget *parent = nullptr);
    bool prepareToLeave();
    QAction *createAction() const { return create_; }
    QAction *deleteAction() const { return remove_; }
    void open(core::SctTextResourceId);

  private:
    void follow();
    void apply();
    void discard();
    void create();
    void preview();
    EditorSessionPresenter *session_;
    SctMessageEditorWidget *editor_;
    QTextBrowser *preview_;
    QLabel *status_;
    QAction *create_, *remove_, *apply_, *discard_;
    std::optional<core::editor::TextDraft> draft_;
    bool dirty_ = false, applying_ = false, stale_ = false;
    std::uint64_t generation_ = 0;
};
} // namespace salsa::qt
