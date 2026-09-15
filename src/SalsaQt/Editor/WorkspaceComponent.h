#pragma once
#include "SalsaCore/Editor/WorkspaceFeatures.h"
#include <QWidget>
class QLabel;
class QTreeWidget;
class QPlainTextEdit;
class QLineEdit;
namespace salsa::qt {
// Workspace presentation has no document, service, filesystem or storage dependency.
class WorkspaceComponent final : public QWidget {
  public:
    explicit WorkspaceComponent(QWidget *);
    void present(core::editor::WorkspaceOverview, std::function<void()> ready = {});
    void clear();
    void status(bool dirty, bool needsSave);
    std::function<void()> refreshRequested, reopenRequested, importRequested;
    std::function<void(core::SctScriptId)> navigate;

  private:
    core::editor::WorkspaceOverview view_;
    std::uint64_t generation_ = 0;
    QLabel *summary_;
    QLineEdit *filter_;
    QTreeWidget *tree_;
    QPlainTextEdit *details_;
    void filter();
};
} // namespace salsa::qt
