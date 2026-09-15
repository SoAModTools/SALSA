#pragma once
#include <QMainWindow>
namespace salsa::qt {
class ProgramWorkspaceController;
// Application shell. Feature components own UI and editing; the workspace
// coordinator composes the session, menus, docks and existing migration tools.
class ProgramWindow final : public QMainWindow {
  public:
    explicit ProgramWindow(QWidget *parent = nullptr);
    ~ProgramWindow() override;
  protected:
    void closeEvent(QCloseEvent *) override;
  private:
    ProgramWorkspaceController *workspace_;
};
}
