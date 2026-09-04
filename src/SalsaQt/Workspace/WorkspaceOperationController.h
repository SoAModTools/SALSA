#pragma once
#include "Application/ExclusiveOperationController.h"
#include "Workspace/WorkspaceController.h"
#include <QString>
class QLabel;
namespace salsa::qt {
class WorkspaceOperationController final : public ExclusiveOperationController {
    Q_OBJECT
public:
    WorkspaceOperationController(WorkspaceController* workspace,
        WorkspaceController::Operation operation, QString rootPath = {},
        QObject* parent = nullptr);
    [[nodiscard]] QString title() const override;
    [[nodiscard]] core::ExclusiveOperationFlowDefinition flowDefinition() const override;
    [[nodiscard]] QWidget* createPage(std::string_view pageId, QWidget* parent) override;
    void pageEntered(std::string_view pageId) override;
    void requestCancel() override;
    void completeRestoration(const QString& detail = {});
    void reportRestorationProgress(std::size_t completed, std::size_t total,
        const QString& currentItem = {});
private:
    WorkspaceController* workspace_ = nullptr;
    WorkspaceController::Operation operation_ = WorkspaceController::Operation::None;
    QString rootPath_{};
    QLabel* processing_ = nullptr;
    QLabel* summary_ = nullptr;
    QString summaryText_{};
    bool started_ = false;
    bool awaitingRestoration_ = false;
};
}  // namespace salsa::qt
