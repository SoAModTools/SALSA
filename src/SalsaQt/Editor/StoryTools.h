#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QAction>
namespace salsa::qt {
class StoryTools final : public QObject {
  public:
    StoryTools(EditorSessionPresenter *, QWidget *);
    QList<QAction *> actions;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void open();
    void meanings();
};
class InteractionTools final : public QObject {
  public:
    InteractionTools(EditorSessionPresenter *, QWidget *);
    QAction *action;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void open();
};
class RouteTools final : public QObject {
  public:
    RouteTools(EditorSessionPresenter *, QWidget *);
    QList<QAction *> actions;

  private:
    EditorSessionPresenter *session_;
    QWidget *host_;
    void open();
    void output();
};
} // namespace salsa::qt
