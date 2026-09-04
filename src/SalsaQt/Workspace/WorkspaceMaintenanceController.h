#pragma once
#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"
#include <QFutureWatcher>
#include <functional>
#include <optional>
class QLabel;
namespace salsa::qt {
class WorkspaceMaintenanceController final : public ExclusiveOperationController {
    Q_OBJECT
public:
    enum class Mode { Open, Cleanup };
    using OpenHandler = std::function<QString(core::LocalSalsaWorkspace)>;
    using CleanupHandler = std::function<void()>;
    static std::unique_ptr<WorkspaceMaintenanceController> openWorkspace(
        std::filesystem::path root, core::DatasetContext dataset, OpenHandler handler);
    static std::unique_ptr<WorkspaceMaintenanceController> cleanup(
        std::filesystem::path root, std::filesystem::path transactions,
        CleanupHandler handler);
    [[nodiscard]] QString title() const override;
    [[nodiscard]] core::ExclusiveOperationFlowDefinition flowDefinition() const override;
    [[nodiscard]] QWidget* createPage(std::string_view pageId, QWidget* parent) override;
    [[nodiscard]] std::optional<ExclusiveOperationAction> actionForEdge(
        std::string_view edgeId) const override;
    [[nodiscard]] bool edgeEnabled(std::string_view edgeId) const override;
    void handleEvent(std::string_view event) override;
    void pageEntered(std::string_view pageId) override;
    void requestCancel() override;
    void completeRestoration(const QString& detail = {});
    void reportRestorationProgress(std::size_t completed, std::size_t total,
        const QString& currentItem = {});
private:
    explicit WorkspaceMaintenanceController(Mode mode);
    void startAssessment();
    void finishWorkspaceAssessment();
    void finishCleanupAssessment();
    void startCommit();
    void finishWorkspaceCommit();
    void finishCleanupCommit();
    QWidget* simplePage(QWidget* parent, QLabel** label);
    Mode mode_;
    std::filesystem::path root_{};
    std::filesystem::path transactions_{};
    std::optional<core::DatasetContext> dataset_{};
    OpenHandler openHandler_{};
    CleanupHandler cleanupHandler_{};
    std::optional<core::WorkspaceOpenAssessment> workspaceAssessment_{};
    std::vector<core::WorkspaceCleanupCandidate> cleanupCandidates_{};
    QFutureWatcher<core::Result<core::WorkspaceOpenAssessment>> workspaceAssessmentWatcher_{};
    QFutureWatcher<core::Result<std::vector<core::WorkspaceCleanupCandidate>>> cleanupAssessmentWatcher_{};
    QFutureWatcher<core::Result<core::LocalSalsaWorkspace>> workspaceCommitWatcher_{};
    QFutureWatcher<core::Result<void>> cleanupCommitWatcher_{};
    QLabel* processing_ = nullptr;
    QLabel* review_ = nullptr;
    QLabel* commit_ = nullptr;
    QLabel* summary_ = nullptr;
    QString summaryText_{};
    bool started_ = false;
    bool awaitingRestoration_ = false;
};
}  // namespace salsa::qt
