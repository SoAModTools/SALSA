#include "Application/DisabledActionHintPresenter.h"

#include <QAction>
#include <QEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolTip>

namespace salsa::qt {
namespace {

constexpr auto DisabledReasonProperty = "salsa.disabledReason";

}  // namespace

DisabledActionHintPresenter::DisabledActionHintPresenter(QObject* parent)
    : QObject(parent), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (!menu_ || !action_ || action_->isEnabled()) return;
        const auto reason = action_->property(DisabledReasonProperty).toString();
        if (reason.isEmpty()) return;
        const auto geometry = menu_->actionGeometry(action_);
        QToolTip::showText(menu_->mapToGlobal(geometry.bottomLeft()), reason, menu_, geometry);
    });
}

void DisabledActionHintPresenter::registerMenu(QMenu* menu) {
    if (menu == nullptr) return;
    menu->setMouseTracking(true);
    menu->installEventFilter(this);
    connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        if (menu_ == menu) clear();
    });
}

void DisabledActionHintPresenter::setReason(QAction* action, const QString& reason) {
    if (action == nullptr) return;
    action->setProperty(DisabledReasonProperty, reason);
    action->setStatusTip(reason);
}

bool DisabledActionHintPresenter::eventFilter(QObject* watched, QEvent* event) {
    auto* menu = qobject_cast<QMenu*>(watched);
    if (menu == nullptr) return QObject::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        auto* action = menu->actionAt(mouse->position().toPoint());
        if (action != nullptr && (!action->isEnabled()
                && !action->property(DisabledReasonProperty).toString().isEmpty()))
            setHoveredAction(menu, action);
        else
            clear();
        break;
    }
    case QEvent::ToolTip:
        if (action_ && !action_->isEnabled()) return true;
        break;
    case QEvent::Leave:
    case QEvent::Hide:
        clear();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void DisabledActionHintPresenter::setHoveredAction(QMenu* menu, QAction* action) {
    if (menu_ == menu && action_ == action) return;
    QToolTip::hideText();
    menu_ = menu;
    action_ = action;
    timer_->start();
}

void DisabledActionHintPresenter::clear() {
    timer_->stop();
    menu_.clear();
    action_.clear();
    QToolTip::hideText();
}

}  // namespace salsa::qt
