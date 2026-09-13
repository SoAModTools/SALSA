#include "Sct/SctDocumentController.h"
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
    if (awaitingRestoration_ && cancelRestoration) {
        setCancellable(false); reportProgress(tr("Cancelling; waiting for the current stage"), 0, 0, ExclusiveOperationProgressUnit::None);
        cancelRestoration(); return;
    }
    if (!started_) {
        summaryText_ = tr("The dataset operation was cancelled.");
        raiseEvent("cancelled");
        return;
    }
    workspace_->cancel();
    setCancellable(false);
    if (processing_) processing_->setText(tr("Cancelling and discarding partial results…"));
}
void WorkspaceOperationController::completeRestoration(const QString& detail, bool cancelled, bool failed) {
    setFinishing(false); setCancellable(false);
    if (!awaitingRestoration_) return;
    awaitingRestoration_ = false;
    if (!detail.isEmpty()) summaryText_ += QStringLiteral("\n\n") + detail;
    if (summary_) summary_->setText(summaryText_);
    if (cancelled || failed) {
        raiseEvent(cancelled ? "cancelled" : "failed");
        reportTerminalActivity(QStringLiteral("WorkspaceRestoration"), detail); return;
    }
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
SctLoadingOperationController::SctLoadingOperationController(SctDocumentController* documents, std::function<bool()> start)
    : documents_(documents), start_(std::move(start)) {
    connect(documents_, &SctDocumentController::loadingProgress, this,
        [this](const QString& phase, quint64 done, quint64 total, const QString& item) {
            if (!cancelling_) reportProgress(phase, done, total, ExclusiveOperationProgressUnit::Scripts, item);
        });
    connect(documents_, &SctDocumentController::operationCompleted, this,
        [this](const QString& item, bool success, bool cancelled, const QString& message) {
            if (!started_) return;
            result_ = message; setCancellable(false);
            reportProgress(tr("Finished"), 1, 1, ExclusiveOperationProgressUnit::Scripts, item);
            raiseEvent(cancelled ? "cancelled" : success ? "complete" : "failed");
            reportTerminalActivity(QStringLiteral("SctLoading"), message, item);
            if (success) requestDismissal();
        });
}
QString SctLoadingOperationController::title() const { return tr("Open SCT document"); }
core::ExclusiveOperationFlowDefinition SctLoadingOperationController::flowDefinition() const {
    using R = core::ExclusiveOperationPageRole; using P = core::ExclusiveOperationProgressVisibility;
    using L = core::ExclusiveOperationPageLayout;
    return {"processing", {{"processing",R::Processing,P::Visible,L::Compact},{"summary",R::Summary,P::Hidden,L::Compact}},
        {{"complete","processing","complete","summary"},{"failed","processing","failed","summary"},{"cancelled","processing","cancelled","summary"}}};
}
QWidget* SctLoadingOperationController::createPage(std::string_view id, QWidget* parent) {
    auto* page = new QWidget(parent); auto* layout = new QVBoxLayout(page);
    auto* text = new QLabel(id == "summary" ? result_ : tr("Preparing document loading…"), page);
    text->setWordWrap(true); layout->addWidget(text); if (id == "processing") message_ = text;
    return page;
}
void SctLoadingOperationController::pageEntered(std::string_view page) {
    if (page != "processing" || started_) return; started_ = true;
    if (!start_()) { result_ = tr("Document loading could not start."); setCancellable(false); raiseEvent("failed"); }
}
void SctLoadingOperationController::requestCancel() {
    cancelling_ = true; documents_->cancel(); setCancellable(false);
    reportProgress(tr("Cancelling; waiting for the current stage"), 0, 0, ExclusiveOperationProgressUnit::None);
}
}  // namespace salsa::qt
