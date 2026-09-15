#pragma once
#include <QObject>
class QAction;
class QWidget;
namespace salsa::qt {
class PerformanceSettings final : public QObject {
public:
    explicit PerformanceSettings(QWidget *);
    QAction *action() const { return action_; }

private:
    void edit();
    QWidget *host_;
    QAction *action_;
};
} // namespace salsa::qt
