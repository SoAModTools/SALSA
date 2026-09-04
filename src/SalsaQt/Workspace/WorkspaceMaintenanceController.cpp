#include "Workspace/WorkspaceMaintenanceController.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace {
QString firstDiagnostic(const std::vector<core::Diagnostic>& diagnostics,
    const QString& fallback) {
    return diagnostics.empty() ? fallback : QString::fromStdString(diagnostics.front().message);
}
}
WorkspaceMaintenanceController::WorkspaceMaintenanceController(const Mode mode)
    : ExclusiveOperationController(nullptr), mode_(mode) {
    connect(&workspaceAssessmentWatcher_, &QFutureWatcherBase::finished,
        this, &WorkspaceMaintenanceController::finishWorkspaceAssessment);
    connect(&cleanupAssessmentWatcher_, &QFutureWatcherBase::finished,
        this, &WorkspaceMaintenanceController::finishCleanupAssessment);
    connect(&workspaceCommitWatcher_, &QFutureWatcherBase::finished,
        this, &WorkspaceMaintenanceController::finishWorkspaceCommit);
    connect(&cleanupCommitWatcher_, &QFutureWatcherBase::finished,
        this, &WorkspaceMaintenanceController::finishCleanupCommit);
}
std::unique_ptr<WorkspaceMaintenanceController> WorkspaceMaintenanceController::openWorkspace(
    std::filesystem::path root, core::DatasetContext dataset, OpenHandler handler) {
    auto result = std::unique_ptr<WorkspaceMaintenanceController>(
        new WorkspaceMaintenanceController(Mode::Open));
    result->root_ = std::move(root); result->dataset_ = std::move(dataset);
    result->openHandler_ = std::move(handler); return result;
}
std::unique_ptr<WorkspaceMaintenanceController> WorkspaceMaintenanceController::cleanup(
    std::filesystem::path root, std::filesystem::path transactions, CleanupHandler handler) {
    auto result = std::unique_ptr<WorkspaceMaintenanceController>(
        new WorkspaceMaintenanceController(Mode::Cleanup));
    result->root_ = std::move(root); result->transactions_ = std::move(transactions);
    result->cleanupHandler_ = std::move(handler); return result;
}
QString WorkspaceMaintenanceController::title() const {
    return mode_ == Mode::Open ? tr("Open or Create Workspace")
                               : tr("Clean Workspace Recovery Evidence");
}
core::ExclusiveOperationFlowDefinition WorkspaceMaintenanceController::flowDefinition() const {
    using Role = core::ExclusiveOperationPageRole;
    return {"processing", {{"processing", Role::Processing}, {"review", Role::Review},
        {"commit", Role::Commit}, {"summary", Role::Summary}},
        {{"review", "processing", "review", "review"},
         {"empty", "processing", "empty", "summary"},
         {"failed_assess", "processing", "failed", "summary"},
         {"confirm", "review", "confirm", "commit"},
         {"cancel_review", "review", "cancelled", "summary"},
         {"complete", "commit", "complete", "summary"},
         {"failed_commit", "commit", "failed", "summary"}}};
}
QWidget* WorkspaceMaintenanceController::simplePage(QWidget* parent, QLabel** label) {
    auto* page = new QWidget(parent); auto* layout = new QVBoxLayout(page);
    *label = new QLabel(page); (*label)->setWordWrap(true); layout->addWidget(*label);
    layout->addStretch(); return page;
}
QWidget* WorkspaceMaintenanceController::createPage(const std::string_view id, QWidget* parent) {
    if (id == "processing") return simplePage(parent, &processing_);
    if (id == "review") {
        auto* page = simplePage(parent, &review_);
        if (mode_ == Mode::Open && workspaceAssessment_)
            review_->setText(QString::fromStdString(workspaceAssessment_->message));
        else if (mode_ == Mode::Cleanup && !cleanupCandidates_.empty()) {
            std::uintmax_t bytes = 0;
            for (const auto& item : cleanupCandidates_) bytes += item.byteSize;
            review_->setText(tr(
                "Remove %1 verified transaction journal(s) using %2 bytes? Active or unresolved transactions are excluded. This cannot be undone.")
                .arg(cleanupCandidates_.size()).arg(bytes));
        }
        return page;
    }
    if (id == "commit") return simplePage(parent, &commit_);
    auto* page = simplePage(parent, &summary_); summary_->setText(summaryText_); return page;
}
std::optional<ExclusiveOperationAction> WorkspaceMaintenanceController::actionForEdge(
    const std::string_view edge) const {
    if (edge == "confirm") return ExclusiveOperationAction{
        mode_ == Mode::Open ? tr("Open Workspace") : tr("Remove Verified Evidence"),
        mode_ == Mode::Open ? ExclusiveOperationActionRole::Primary
                            : ExclusiveOperationActionRole::Destructive};
    return std::nullopt;
}
bool WorkspaceMaintenanceController::edgeEnabled(std::string_view) const { return true; }
void WorkspaceMaintenanceController::handleEvent(const std::string_view event) {
    if (event == "confirm") { raiseEvent(event); startCommit(); }
    else ExclusiveOperationController::handleEvent(event);
}
void WorkspaceMaintenanceController::pageEntered(const std::string_view page) {
    if (page == "processing" && !started_) { started_ = true; startAssessment(); }
}
void WorkspaceMaintenanceController::requestCancel() {
    summaryText_ = tr("The operation was cancelled before durable changes began.");
    if (summary_) summary_->setText(summaryText_);
    raiseEvent("cancelled");
}
void WorkspaceMaintenanceController::startAssessment() {
    setCancellable(false); // These bounded filesystem assessments do not expose cancellation.
    reportProgress(mode_ == Mode::Open ? tr("Assessing workspace")
        : tr("Assessing recovery evidence"), 0, 0, ExclusiveOperationProgressUnit::Files);
    if (processing_) processing_->setText(mode_ == Mode::Open
        ? tr("Inspecting the workspace and its dataset binding…")
        : tr("Finding verified transaction evidence eligible for cleanup…"));
    if (mode_ == Mode::Open) {
        const auto root = root_; const auto dataset = *dataset_;
        workspaceAssessmentWatcher_.setFuture(QtConcurrent::run([root, dataset] {
            return core::LocalSalsaWorkspace::assess(root, dataset);
        }));
    } else {
        const auto root = root_; const auto transactions = transactions_;
        cleanupAssessmentWatcher_.setFuture(QtConcurrent::run([root, transactions] {
            return core::WorkspaceArtifactCleanupService::assess(root, transactions);
        }));
    }
}
void WorkspaceMaintenanceController::finishWorkspaceAssessment() {
    auto result = workspaceAssessmentWatcher_.result(); setCancellable(true);
    if (!result) {
        summaryText_ = firstDiagnostic(result.diagnostics(), tr("The workspace could not be assessed."));
        raiseEvent("failed"); return;
    }
    workspaceAssessment_ = std::move(result).takeValue();
    if (review_) review_->setText(QString::fromStdString(workspaceAssessment_->message));
    raiseEvent("review");
}
void WorkspaceMaintenanceController::finishCleanupAssessment() {
    auto result = cleanupAssessmentWatcher_.result(); setCancellable(true);
    if (!result) {
        summaryText_ = firstDiagnostic(result.diagnostics(), tr("Recovery evidence could not be assessed."));
        raiseEvent("failed"); return;
    }
    cleanupCandidates_ = std::move(result).takeValue();
    if (cleanupCandidates_.empty()) {
        summaryText_ = tr("No verified transaction recovery evidence is eligible for cleanup.");
        raiseEvent("empty"); return;
    }
    std::uintmax_t bytes = 0; for (const auto& item : cleanupCandidates_) bytes += item.byteSize;
    if (review_) review_->setText(tr(
        "Remove %1 verified transaction journal(s) using %2 bytes? Active or unresolved transactions are excluded. This cannot be undone.")
        .arg(cleanupCandidates_.size()).arg(bytes));
    raiseEvent("review");
}
void WorkspaceMaintenanceController::startCommit() {
    setCancellable(false); setFinishing(true);
    if (commit_) commit_->setText(tr("Finishing safely…"));
    if (mode_ == Mode::Open) {
        const auto root = root_; const auto dataset = *dataset_;
        const auto acceptance = workspaceAssessment_->requiresReassociation()
            ? core::WorkspaceDatasetAcceptance::UserConfirmedReassociation
            : core::WorkspaceDatasetAcceptance::ExactOnly;
        workspaceCommitWatcher_.setFuture(QtConcurrent::run([root, dataset, acceptance] {
            return core::LocalSalsaWorkspace::openOrCreate(root, dataset, acceptance);
        }));
    } else {
        const auto root = root_; const auto candidates = cleanupCandidates_;
        cleanupCommitWatcher_.setFuture(QtConcurrent::run([root, candidates] {
            return core::WorkspaceArtifactCleanupService::remove(root, candidates);
        }));
    }
}
void WorkspaceMaintenanceController::finishWorkspaceCommit() {
    auto result = workspaceCommitWatcher_.result(); setFinishing(false);
    if (!result) {
        summaryText_ = firstDiagnostic(result.diagnostics(), tr("The workspace could not be opened."));
        raiseEvent("failed"); return;
    }
    summaryText_ = openHandler_ ? openHandler_(std::move(result).takeValue())
                                : tr("The workspace was opened.");
    awaitingRestoration_ = true;
    reportProgress(tr("Restoring workspace session"), 0, 0,
        ExclusiveOperationProgressUnit::Assets);
}
void WorkspaceMaintenanceController::finishCleanupCommit() {
    auto result = cleanupCommitWatcher_.result(); setFinishing(false);
    if (!result) {
        summaryText_ = firstDiagnostic(result.diagnostics(), tr("Recovery evidence could not be removed."));
        raiseEvent("failed"); return;
    }
    if (cleanupHandler_) cleanupHandler_();
    summaryText_ = tr("Verified workspace recovery evidence was removed.");
    if (summary_) summary_->setText(summaryText_); raiseEvent("complete");
}
void WorkspaceMaintenanceController::completeRestoration(const QString& detail) {
    if (!awaitingRestoration_) return;
    awaitingRestoration_ = false;
    if (!detail.isEmpty()) summaryText_ += QStringLiteral("\n\n") + detail;
    if (summary_) summary_->setText(summaryText_);
    raiseEvent("complete");
}
void WorkspaceMaintenanceController::reportRestorationProgress(
    const std::size_t completed, const std::size_t total, const QString& currentItem) {
    if (!awaitingRestoration_) return;
    reportProgress(tr("Restoring workspace documents"), completed, total,
        ExclusiveOperationProgressUnit::Assets, currentItem);
}
}  // namespace salsa::qt
