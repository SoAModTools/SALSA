#pragma once

#include <QMainWindow>

namespace salsa::qt {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
};

}  // namespace salsa::qt
