#pragma once

#include <QObject>
#include <QPointer>

class QAction;
class QMenu;
class QTimer;

namespace salsa::qt {

class DisabledActionHintPresenter final : public QObject {
public:
    explicit DisabledActionHintPresenter(QObject* parent = nullptr);

    void registerMenu(QMenu* menu);
    static void setReason(QAction* action, const QString& reason);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setHoveredAction(QMenu* menu, QAction* action);
    void clear();

    QTimer* timer_ = nullptr;
    QPointer<QMenu> menu_{};
    QPointer<QAction> action_{};
};

}  // namespace salsa::qt
