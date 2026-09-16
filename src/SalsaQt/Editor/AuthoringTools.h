#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QAction>
namespace salsa::qt {
class ModuleTools final : public QObject {
  public:
    ModuleTools(EditorSessionPresenter *, QWidget *);
    QList<QAction *> actions;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void open();
    void definitions();
    void editModule(core::editor::ModuleItem);
};
class LanguageTools final : public QObject {
  public:
    LanguageTools(EditorSessionPresenter *, QWidget *);
    QAction *action;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void open();
    void batch(std::vector<core::SctTextResourceId>);
    void localizations();
};
class EntryResourceTools final : public QObject {
  public:
    EntryResourceTools(EditorSessionPresenter *, QWidget *);
    QList<QAction *> actions;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void entries();
    void resources();
};
class SelectorTools final : public QObject {
  public:
    SelectorTools(EditorSessionPresenter *, QWidget *);
    QAction *action;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void open();
};
} // namespace salsa::qt
