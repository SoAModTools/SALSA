#include "Application/ProgramWindow.h"
#include "Application/ProgramWorkspaceController.h"
namespace salsa::qt {
ProgramWindow::ProgramWindow(QWidget *parent) : QMainWindow(parent),
    workspace_(new ProgramWorkspaceController(this)) {}
ProgramWindow::~ProgramWindow() = default;
void ProgramWindow::closeEvent(QCloseEvent *event) { workspace_->closeRequested(event); }
}
