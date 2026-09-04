#include "Application/ExclusiveOperationCoordinator.h"

#include "Application/ExclusiveOperationController.h"
#include "Application/ExclusiveOperationDialog.h"

namespace salsa::qt {

ExclusiveOperationCoordinator::ExclusiveOperationCoordinator(QObject* parent)
    : QObject(parent) {}

bool ExclusiveOperationCoordinator::open(
    std::unique_ptr<ExclusiveOperationController> controller, QWidget* parent) {
    auto lease = gate_.tryAcquire();
    if (!lease) {
        focusActive();
        return false;
    }
    lease_ = std::move(*lease);
    auto* dialog = new ExclusiveOperationDialog(std::move(controller), parent);
    dialog_ = dialog;
    connect(dialog, &ExclusiveOperationDialog::sessionDone, this, [this] {
        lease_.reset();
        dialog_.clear();
        emit activeChanged(false);
    });
    connect(dialog, &QObject::destroyed, this, [this] {
        if (!lease_) return;
        dialog_.clear();
        lease_.reset();
        emit activeChanged(false);
    });
    emit activeChanged(true);
    dialog->open();
    return true;
}

bool ExclusiveOperationCoordinator::active() const noexcept { return gate_.active(); }

void ExclusiveOperationCoordinator::focusActive() {
    if (!dialog_) return;
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
}

void ExclusiveOperationCoordinator::requestApplicationClose() {
    if (dialog_) dialog_->requestApplicationClose();
}

}  // namespace salsa::qt
