#pragma once

#include "SalsaCore/Application/ExclusiveOperationFlow.h"

#include <QObject>
#include <QPointer>

#include <memory>
#include <optional>

class QWidget;

namespace salsa::qt {

class ExclusiveOperationController;
class ExclusiveOperationDialog;

class ExclusiveOperationCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit ExclusiveOperationCoordinator(QObject* parent = nullptr);
    [[nodiscard]] bool open(std::unique_ptr<ExclusiveOperationController> controller,
        QWidget* parent);
    [[nodiscard]] bool active() const noexcept;
    void focusActive();
    void requestApplicationClose();

signals:
    void activeChanged(bool active);

private:
    core::ExclusiveOperationGate gate_{};
    std::optional<core::ExclusiveOperationGate::Lease> lease_{};
    QPointer<ExclusiveOperationDialog> dialog_{};
};

}  // namespace salsa::qt
