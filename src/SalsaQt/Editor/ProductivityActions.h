#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QObject>
class QAction;
namespace salsa::qt {
class ProductivityActions final : public QObject {
  public:
    ProductivityActions(EditorSessionPresenter *, QWidget *);
    const QList<QAction *> &actions() const { return actions_; }

  private:
    void copy(bool cut = false, bool duplicate = false);
    void paste(std::vector<std::byte>);
    void snippets();
    void move();
    void refresh();
    EditorSessionPresenter *session_;
    QWidget *host_;
    QList<QAction *> actions_;
    QAction *copy_, *cut_, *paste_, *duplicate_, *remove_, *move_, *snippets_;
};
} // namespace salsa::qt
