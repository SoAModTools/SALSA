#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QWidget>
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QListWidget;
class QPushButton;
class QAction;
namespace salsa::qt {
class OrganizationComponent final : public QWidget {
  public:
    OrganizationComponent(EditorSessionPresenter *, QWidget *);
    const QList<QAction *> &actions() const { return actions_; }
    std::function<void()> showRequested;

  protected:
    void showEvent(QShowEvent *) override;

  private:
    void refresh(bool discard = false);
    void apply();
    void folders();
    void catalog();
    EditorSessionPresenter *session_;
    QLabel *target_;
    QLineEdit *alias_, *bookmark_;
    QPlainTextEdit *note_;
    QPushButton *color_, *apply_;
    QListWidget *bookmarks_;
    std::vector<core::editor::Subject> bookmarkTargets_;
    QList<QAction *> actions_;
    std::optional<core::editor::ItemNote> draft_;
    core::editor::Revision draftRevision_;
    std::uint64_t generation_ = 0;
    bool dirty_ = false, loading_ = false, applying_ = false;
};
} // namespace salsa::qt
