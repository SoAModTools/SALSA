#include "Application/ExclusiveOperationController.h"

namespace salsa::qt {

ExclusiveOperationController::ExclusiveOperationController(QObject* parent)
    : QObject(parent) {}

std::optional<ExclusiveOperationAction> ExclusiveOperationController::actionForEdge(
    std::string_view) const { return std::nullopt; }

bool ExclusiveOperationController::edgeEnabled(std::string_view) const { return true; }

void ExclusiveOperationController::handleEvent(const std::string_view event) {
    raiseEvent(event);
}

void ExclusiveOperationController::pageEntered(std::string_view) {}

void ExclusiveOperationController::requestCancel() { raiseEvent("cancelled"); }

bool ExclusiveOperationController::cancellable() const noexcept { return cancellable_; }
bool ExclusiveOperationController::finishing() const noexcept { return finishing_; }

void ExclusiveOperationController::raiseEvent(const std::string_view event) {
    lastRaisedEvent_ = event;
    emit eventRaised(QString::fromUtf8(event.data(), static_cast<qsizetype>(event.size())));
}

void ExclusiveOperationController::setCancellable(const bool value) {
    if (cancellable_ == value) return;
    cancellable_ = value;
    emit cancellationChanged(value);
}

void ExclusiveOperationController::setFinishing(const bool value) {
    if (finishing_ == value) return;
    finishing_ = value;
    emit finishingChanged(value);
}

void ExclusiveOperationController::reportProgress(const QString& phase,
    const std::uint64_t completed, const std::uint64_t total,
    const ExclusiveOperationProgressUnit unit, const QString& currentItem) {
    emit progressChanged(phase, completed, total, static_cast<int>(unit), currentItem);
}

void ExclusiveOperationController::reportDiagnostics(const QString& text) {
    emit diagnosticsChanged(text);
}

void ExclusiveOperationController::reportTerminalActivity(const QString& code,
    const QString& message, const QString& location) {
    auto outcome = ExclusiveOperationActivityOutcome::Failed;
    if (lastRaisedEvent_.find("cancel") != std::string::npos)
        outcome = ExclusiveOperationActivityOutcome::Cancelled;
    else if (lastRaisedEvent_ == "complete" || lastRaisedEvent_ == "committed"
        || lastRaisedEvent_ == "none" || lastRaisedEvent_ == "empty")
        outcome = ExclusiveOperationActivityOutcome::Completed;
    emit activityRaised(static_cast<int>(outcome), code, message, location);
}

void ExclusiveOperationController::requestDismissal() {
    setCancellable(false);
    emit dismissalRequested();
}

std::string_view ExclusiveOperationController::lastRaisedEvent() const noexcept {
    return lastRaisedEvent_;
}

}  // namespace salsa::qt
