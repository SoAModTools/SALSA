#pragma once
#include "Application/ExclusiveOperationController.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include <QFutureWatcher>
#include <QTimer>
#include <any>
#include <mutex>
namespace salsa::qt {
// One operation remains open through worker completion and lazy presentation.
class ProgramOperation final : public ExclusiveOperationController {
  public:
    using Work = std::function<core::Result<std::any>(std::stop_token, const core::SctLoadingObserver &)>;
    using Install = std::function<void(std::any, ProgramOperation &)>;
    ProgramOperation(QString, Work, Install, bool discardCancelled = false);
    ~ProgramOperation() override;
    QString title() const override;
    core::ExclusiveOperationFlowDefinition flowDefinition() const override;
    QWidget *createPage(std::string_view, QWidget *) override;
    bool compactOpening = false;
    // Nonblocking notices from successful compact opens belong in the host log,
    // not in the dialog's expanded diagnostic presentation.
    std::function<void(QString)> diagnosticLog;
    bool progressOnlyPage(std::string_view id) const override { return compactOpening && id == "working"; }
    void pageEntered(std::string_view) override;
    void requestCancel() override;
    std::optional<ExclusiveOperationAction> actionForEdge(std::string_view) const override;
    void handleEvent(std::string_view) override;
    void offerRetry(QString, Work);
    void offerDecision(QString action, std::function<Work()> prepare, std::function<QWidget *(QWidget *)> page);
    // Continue an already successful import without opening a second operation.
    void offerNext(QString action, std::function<Work()> prepare, Install,
                   std::function<QWidget *(QWidget *)> page, bool discardCancelled = true);
    // Optional selection stage, inside the same exclusive operation.
    std::function<QWidget *(QWidget *)> configurationPage;
    std::function<Work()> prepareWork;
    std::function<bool()> configurationReady;
    std::function<void()> configurationEntered;
    bool edgeEnabled(std::string_view) const override;
    void configureAgain();
    void prepareOpening() {
        if (prepareWork)
            work_ = prepareWork();
    }
    std::function<bool(const std::vector<core::Diagnostic> &, ProgramOperation &)> recoverFailure;
    void finish(QString message = {});
    void fail(QString);
    void progress(QString, quint64 = 0, quint64 = 0, QString = {});
    bool stopped() const { return stop_.stop_requested(); }
    std::function<void()> cancelPresentation;

  private:
    struct Pending {
        std::mutex mutex;
        std::vector<core::SctLoadingProgress> updates;
        std::string phase;
        std::chrono::steady_clock::time_point last;
    };
    void drain();
    QString name_, summary_;
    Work work_;
    Install install_;
    std::stop_source stop_;
    std::shared_ptr<Pending> pending_ = std::make_shared<Pending>();
    QFutureWatcher<core::Result<std::any>> watcher_;
    QTimer timer_;
    QString retryAction_;
    std::function<Work()> retryWork_;
    std::function<QWidget *(QWidget *)> reviewPage_;
    QWidget *reviewContainer_ = nullptr;
    QString nextAction_;
    std::function<Work()> nextWork_;
    std::function<QWidget *(QWidget *)> nextPage_;
    QWidget *nextContainer_ = nullptr;
    bool choosingNext_ = false;
    bool started_ = false, done_ = false, discardCancelled_ = false, preparing_ = false, reviewing_ = false,
         configuring_ = false;
};
} // namespace salsa::qt
