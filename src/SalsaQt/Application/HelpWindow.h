#pragma once

#include <QWidget>

class QListWidget;
class QTextBrowser;

namespace salsa::qt {

class HelpWindow final : public QWidget {
public:
    explicit HelpWindow(QWidget* parent = nullptr);

    void showTopic(const QString& resourceName);

private:
    QTextBrowser* browser_ = nullptr;
    QListWidget* topics_ = nullptr;
};

}  // namespace salsa::qt
