#pragma once

#include "Application/InteractionNotice.h"

#include <QObject>
#include <QRect>
#include <QTimer>

#include <functional>

class QFrame;
class QLabel;
class QWidget;

namespace salsa::qt {

class TransientNoticePresenter final : public QObject {
public:
    using AnchorProvider = std::function<QRect()>;

    explicit TransientNoticePresenter(QWidget* owner);
    ~TransientNoticePresenter() override;

    void showNotice(const InteractionNotice& notice, AnchorProvider anchor = {});
    void dismiss();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reposition();

    QWidget* owner_ = nullptr;
    QFrame* callout_ = nullptr;
    QLabel* message_ = nullptr;
    QTimer dismissTimer_{};
    QTimer trackingTimer_{};
    AnchorProvider anchor_{};
};

}  // namespace salsa::qt
