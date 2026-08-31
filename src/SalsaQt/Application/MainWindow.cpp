#include "Application/MainWindow.h"

#include "SalsaCore/Application/ApplicationInfo.h"

#include <QAction>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QString>

namespace salsa::qt {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    const auto name = core::applicationName();
    setWindowTitle(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    resize(1100, 700);

    auto* placeholder = new QLabel(tr("No workspace is open."), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    statusBar()->showMessage(tr("Ready"));
}

}  // namespace salsa::qt
