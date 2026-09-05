#include "Workspace/WorkspaceOperationController.h"
#include "SalsaCore/Foundation/Diagnostic.h"
#include "SalsaCore/Application/ShellPresentation.h"
#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>
#include <ranges>
namespace salsa::qt {
WorkspaceOperationController::WorkspaceOperationController(
    WorkspaceController* workspace, const WorkspaceController::Operation operation,
    QString rootPath, QObject* parent)
    : ExclusiveOperationController(parent), workspace_(workspace), operation_(operation),
      rootPath_(std::move(rootPath)) {
    connect(workspace_, &WorkspaceController::progressChanged, this,
        [this](const bool determinate, const int completed, const int total,
            const QString& path) {
            reportProgress(operation_ == WorkspaceController::Operation::Opening
                    ? tr("Inspecting dataset") : tr("Refreshing dataset"),
                completed, determinate ? total : 0, ExclusiveOperationProgressUnit::Files,
                QFileInfo(path).fileName());
        });
    connect(workspace_, &WorkspaceController::operationCompleted, this,
        [this](const WorkspaceController::Operation operation, const bool success,
            const bool cancelled, const QString& message) {
            if (!started_ || operation != operation_) return;
            summaryText_ = message;
            if (summary_) summary_->setText(summaryText_);
            setCancellable(false);
            const bool hasCurrentDiagnostics = std::ranges::any_of(
                workspace_->diagnostics(), [](const auto& diagnostic) {
                    return core::isCurrentDiagnosticSeverity(diagnostic.severity);
                });
            const auto presentation = core::datasetOperationPresentation(
                operation_ == WorkspaceController::Operation::Opening,
                success, cancelled, hasCurrentDiagnostics);
            if (presentation == core::DatasetOperationPresentation::AwaitRestoration) {
                awaitingRestoration_ = true;
                reportProgress(tr("Restoring associated workspace and session"), 0, 0,
                    ExclusiveOperationProgressUnit::Assets);
                return;
            }
            if (presentation == core::DatasetOperationPresentation::Dismiss) {
                requestDismissal();
                return;
            }
            raiseEvent(cancelled ? "cancelled" : success ? "complete" : "failed");
        });
}
QString WorkspaceOperationController::title() const {
    return operation_ == WorkspaceController::Operation::Opening
        ? tr("Open Dataset") : tr("Refresh Dataset");
}
core::ExclusiveOperationFlowDefinition WorkspaceOperationController::flowDefinition() const {
    using Role = core::ExclusiveOperationPageRole;
    using Progress = core::ExclusiveOperationProgressVisibility;
    using Layout = core::ExclusiveOperationPageLayout;
    return {"processing", {{"processing", Role::Processing, Progress::Visible, Layout::Compact},
        {"summary", Role::Summary, Progress::Hidden, Layout::Compact}},
        {{"complete", "processing", "complete", "summary"},
         {"failed", "processing", "failed", "summary"},
         {"cancelled", "processing", "cancelled", "summary"}}};
}
QWidget* WorkspaceOperationController::createPage(
    const std::string_view id, QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    if (id == "processing") {
        processing_ = new QLabel(operation_ == WorkspaceController::Operation::Opening
            ? tr("Inspecting the selected dataset…") : tr("Refreshing the dataset…"), page);
        processing_->setWordWrap(true);
        layout->addWidget(processing_);
    } else {
        summary_ = new QLabel(summaryText_, page);
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
    }
    layout->addStretch();
    return page;
}
void WorkspaceOperationController::pageEntered(const std::string_view page) {
    if (page != "processing" || started_) return;
    started_ = true;
    const bool began = operation_ == WorkspaceController::Operation::Opening
        ? workspace_->openDataset(rootPath_) : workspace_->refresh();
    if (!began) {
        summaryText_ = tr("The dataset operation could not be started.");
        setCancellable(false);
        raiseEvent("failed");
    }
}
void WorkspaceOperationController::requestCancel() {
    if (!started_) {
        summaryText_ = tr("The dataset operation was cancelled.");
        raiseEvent("cancelled");
        return;
    }
    workspace_->cancel();
    setCancellable(false);
    if (processing_) processing_->setText(tr("Cancelling and discarding partial results…"));
}
void WorkspaceOperationController::completeRestoration(const QString& detail) {
    if (!awaitingRestoration_) return;
    awaitingRestoration_ = false;
    if (!detail.isEmpty()) summaryText_ += QStringLiteral("\n\n") + detail;
    if (summary_) summary_->setText(summaryText_);
    const bool hasCurrentDiagnostics = std::ranges::any_of(
        workspace_->diagnostics(), [](const auto& diagnostic) {
            return core::isCurrentDiagnosticSeverity(diagnostic.severity);
        });
    if (core::restoredDatasetPresentation(hasCurrentDiagnostics, !detail.isEmpty())
            == core::DatasetOperationPresentation::Dismiss)
        requestDismissal();
    else
        raiseEvent("complete");
}
void WorkspaceOperationController::reportRestorationProgress(
    const std::size_t completed, const std::size_t total, const QString& currentItem) {
    if (!awaitingRestoration_) return;
    reportProgress(tr("Restoring workspace documents"), completed, total,
        ExclusiveOperationProgressUnit::Assets, currentItem);
}
}  // namespace salsa::qt
