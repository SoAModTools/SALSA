#include "Application/TransientNoticePresenter.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QStringList>
#include <QStyle>
#include <QWidget>

#include <algorithm>

namespace salsa::qt {

TransientNoticePresenter::TransientNoticePresenter(QWidget* owner)
    : QObject(owner), owner_(owner) {
    callout_ = new QFrame(nullptr, Qt::ToolTip | Qt::FramelessWindowHint
        | Qt::WindowDoesNotAcceptFocus);
    callout_->setAttribute(Qt::WA_ShowWithoutActivating);
    callout_->setFrameShape(QFrame::StyledPanel);
    callout_->setStyleSheet(QStringLiteral(
        "QFrame { background: #fff4ce; border: 1px solid #c89b18; border-radius: 5px; }"
        "QLabel { color: #5f4700; border: none; background: transparent; }"));
    auto* layout = new QHBoxLayout(callout_);
    layout->setContentsMargins(10, 8, 10, 8);
    auto* icon = new QLabel(callout_);
    icon->setPixmap(owner_->style()->standardIcon(QStyle::SP_MessageBoxWarning)
        .pixmap(20, 20));
    message_ = new QLabel(callout_);
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);
    message_->setMaximumWidth(420);
    layout->addWidget(icon, 0, Qt::AlignTop);
    layout->addWidget(message_, 1);
    callout_->installEventFilter(this);
    qApp->installEventFilter(this);

    dismissTimer_.setSingleShot(true);
    connect(&dismissTimer_, &QTimer::timeout, this, &TransientNoticePresenter::dismiss);
    trackingTimer_.setInterval(100);
    connect(&trackingTimer_, &QTimer::timeout, this, &TransientNoticePresenter::reposition);
}

TransientNoticePresenter::~TransientNoticePresenter() {
    if (qApp != nullptr) qApp->removeEventFilter(this);
    delete callout_;
}

void TransientNoticePresenter::showNotice(
    const InteractionNotice& notice, AnchorProvider anchor) {
    anchor_ = std::move(anchor);
    if (!anchor_ && notice.globalPosition) {
        const auto point = *notice.globalPosition;
        anchor_ = [point] { return QRect(point, QSize(1, 1)); };
    }
    QString text = notice.message;
    QStringList context;
    if (!notice.code.isEmpty()) context.push_back(notice.code);
    if (!notice.documentIdentity.isEmpty()) context.push_back(notice.documentIdentity);
    if (!context.isEmpty()) text += QStringLiteral("\n") + context.join(QStringLiteral(" · "));
    message_->setText(text);
    callout_->setAccessibleName(tr("Interaction notice"));
    callout_->setAccessibleDescription(notice.message);
    callout_->adjustSize();
    reposition();
    callout_->show();
    callout_->raise();
    dismissTimer_.start(notice.prominent ? 8000 : 5000);
    trackingTimer_.start();
}

void TransientNoticePresenter::dismiss() {
    dismissTimer_.stop();
    trackingTimer_.stop();
    anchor_ = {};
    callout_->hide();
}

bool TransientNoticePresenter::eventFilter(QObject* watched, QEvent* event) {
    if (callout_->isVisible() && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) dismiss();
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == callout_ || (widget != nullptr && callout_->isAncestorOf(widget)))
            dismiss();
    }
    return QObject::eventFilter(watched, event);
}

void TransientNoticePresenter::reposition() {
    if (callout_ == nullptr || owner_ == nullptr) return;
    QRect anchor = anchor_ ? anchor_() : QRect{};
    if (!anchor.isValid()) {
        const auto center = owner_->mapToGlobal(owner_->rect().center());
        anchor = QRect(center, QSize(1, 1));
    }
    auto position = QPoint(anchor.left(), anchor.bottom() + 8);
    auto* screen = QGuiApplication::screenAt(anchor.center());
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen != nullptr) {
        const auto available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right() - callout_->width() + 1)));
        if (position.y() + callout_->height() > available.bottom() + 1)
            position.setY(anchor.top() - callout_->height() - 8);
        position.setY(std::clamp(position.y(), available.top(),
            std::max(available.top(), available.bottom() - callout_->height() + 1)));
    }
    callout_->move(position);
}

}  // namespace salsa::qt
