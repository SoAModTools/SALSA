#pragma once
#include "Application/ExclusiveOperationController.h"
#include "Workspace/WorkspaceController.h"
#include <QString>
#include <functional>
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
    std::function<void()> cancelRestoration;
    bool loadingSafe = true;
    void loadingSafety(bool safe) {
        loadingSafe = safe; setFinishing(!safe); setCancellable(safe && static_cast<bool>(cancelRestoration));
    }
    void loadingProgress(const QString& phase, quint64 completed, quint64 total, const QString& item) {
        setCancellable(loadingSafe && static_cast<bool>(cancelRestoration));
        reportProgress(phase, completed, total, ExclusiveOperationProgressUnit::Scripts, item);
    }
    void completeRestoration(const QString& detail = {}, bool cancelled = false, bool failed = false);
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
class SctDocumentController;
class SctLoadingOperationController final : public ExclusiveOperationController {
public:
    SctLoadingOperationController(SctDocumentController*, std::function<bool()> start);
    QString title() const override;
    core::ExclusiveOperationFlowDefinition flowDefinition() const override;
    QWidget* createPage(std::string_view, QWidget*) override;
    void pageEntered(std::string_view) override;
    void requestCancel() override;
private:
    SctDocumentController* documents_;
    std::function<bool()> start_;
    QLabel* message_ = nullptr;
    QString result_;
    bool started_ = false, cancelling_ = false;
};
}  // namespace salsa::qt
