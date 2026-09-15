#pragma once
#include "Editor/EditorSessionPresenter.h"
#include <QWidget>
class QListWidget;
class QLabel;
class QPushButton;
namespace salsa::qt {
// Shared paged results used by the navigator and reference selection dialogs.
class EditorQueryWidget final : public QWidget {
  public:
    explicit EditorQueryWidget(EditorSessionPresenter *, QWidget *parent = nullptr);
    ~EditorQueryWidget() override { stop_.request_stop(); }
    void query(core::editor::BrowseRequest);
    std::optional<core::editor::Subject> selected() const;
    std::function<void(core::editor::Subject)> activated;

  private:
    std::stop_source stop_;
    void load();
    EditorSessionPresenter *session_;
    core::editor::BrowseRequest query_;
    std::vector<core::editor::BrowseRow> rows_;
    std::uint64_t generation_ = 0;
    QListWidget *list_;
    QLabel *status_;
    QPushButton *previous_, *next_;
};
std::optional<core::editor::Subject> pickReference(EditorSessionPresenter *, core::editor::BrowseRequest, QWidget *);
class NavigatorComponent final : public QWidget {
  public:
    explicit NavigatorComponent(EditorSessionPresenter *, QWidget *parent = nullptr);
    std::function<void()> showVariableSearch;
};
} // namespace salsa::qt
