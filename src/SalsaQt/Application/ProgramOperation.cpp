#include "Application/ProgramOperation.h"
#include <QLabel>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
ProgramOperation::ProgramOperation(QString name, Work work, Install install, bool discardCancelled)
    : name_(std::move(name)), work_(std::move(work)), install_(std::move(install)),
      discardCancelled_(discardCancelled) {
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, [this] { drain(); });
    connect(&watcher_, &QFutureWatcherBase::finished, this, [this] {
        drain();
        timer_.stop();
        auto result = watcher_.result();
        if (stop_.stop_requested() && discardCancelled_) {
            summary_ = tr("Cancelled. The previous workspace is unchanged.");
            done_ = true;
            setCancellable(false);
            raiseEvent("cancelled");
            return;
        }
        if (!result) {
            if (stop_.stop_requested()) {
                summary_ = tr("Operation cancelled.");
                done_ = true;
                setCancellable(false);
                raiseEvent("cancelled");
                return;
            }
            if (recoverFailure && recoverFailure(result.diagnostics(), *this))
                return;
            QStringList lines;
            for (const auto &d : result.diagnostics())
                lines << QString::fromStdString(d.message);
            fail(lines.join('\n'));
            return;
        }
        for (const auto &d : result.diagnostics()) {
            const auto message = QString::fromStdString(d.message);
            if (diagnosticLog && d.severity != core::DiagnosticSeverity::Error)
                diagnosticLog(d.severity == core::DiagnosticSeverity::Warning
                                  ? tr("Warning: %1").arg(message) : message);
            if (!compactOpening || !diagnosticLog || d.severity == core::DiagnosticSeverity::Error)
                reportDiagnostics(message);
        }
        preparing_ = true;
        setCancellable(true);
        try {
            // Installation may offer a subsequent stage and replace install_.
            auto install = install_;
            install(std::move(result).takeValue(), *this);
        } catch (const std::exception &e) {
            fail(QString::fromUtf8(e.what()));
        }
    });
}
ProgramOperation::~ProgramOperation() { stop_.request_stop(); }
QString ProgramOperation::title() const { return name_; }
core::ExclusiveOperationFlowDefinition ProgramOperation::flowDefinition() const {
    using R = core::ExclusiveOperationPageRole;
    using P = core::ExclusiveOperationProgressVisibility;
    using L = core::ExclusiveOperationPageLayout;
    return {configurationPage ? "configure" : "working",
            {{"configure", R::Configuration, P::Hidden, L::Standard},
             {"working", R::Processing, P::Visible, configurationPage ? L::Standard : L::Compact},
             {"review", R::Review, P::Hidden, L::Standard},
             {"next", R::Review, P::Hidden, L::Standard},
             {"summary", R::Summary, P::Hidden, L::Compact}},
            {{"start", "configure", "start", "working"},
             {"reconfigure", "working", "configure", "configure"},
             {"cancel_configure", "configure", "cancelled", "summary"},
             {"review", "working", "review", "review"},
             {"retry", "review", "retry", "working"},
             {"cancel_review", "review", "cancelled", "summary"},
             {"offer_next", "working", "next", "next"},
             {"continue_next", "next", "continue_next", "working"},
             {"finish_next", "next", "complete", "summary"},
             {"complete", "working", "complete", "summary"},
             {"failed", "working", "failed", "summary"},
             {"cancelled", "working", "cancelled", "summary"}}};
}
QWidget *ProgramOperation::createPage(std::string_view id, QWidget *parent) {
    if (id == "next") {
        nextContainer_ = new QWidget(parent);
        new QVBoxLayout(nextContainer_);
        return nextContainer_;
    }
    if (id == "configure" && configurationPage)
        return configurationPage(parent);
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *label = new QLabel(
        (id == "summary" || id == "review") ? summary_ : tr("Preparing the requested operation…"), page);
    label->setWordWrap(true);
    layout->addWidget(label);
    if (id == "summary")
        reportTerminalActivity(QStringLiteral("ProgramOperation"), summary_);
    return page;
}
void ProgramOperation::pageEntered(std::string_view page) {
    choosingNext_ = page == "next";
    if (choosingNext_) {
        setCancellable(true);
        auto *layout = nextContainer_->layout();
        while (auto *item = layout->takeAt(0)) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        if (nextPage_) layout->addWidget(nextPage_(nextContainer_));
    }
    configuring_ = page == "configure";
    if (configuring_) {
        setCancellable(true);
        if (configurationEntered)
            configurationEntered();
    }
    if (page != "working" || started_)
        return;
    started_ = true;
    timer_.start();
    auto work = std::move(work_);
    auto pending = pending_;
    auto stop = stop_.get_token();
    watcher_.setFuture(QtConcurrent::run([work, pending, stop] {
        core::SctLoadingObserver observer = [pending](const auto &p) {
            std::lock_guard lock(pending->mutex);
            auto now = std::chrono::steady_clock::now();
            if (p.phase != pending->phase || (p.total && p.completed == p.total) ||
                now - pending->last > std::chrono::milliseconds(100)) {
                pending->updates.push_back(p);
                pending->phase = p.phase;
                pending->last = now;
            }
        };
        try {
            return work(stop, observer);
        } catch (const std::exception &e) {
            return core::Result<std::any>::failure(
                {core::DiagnosticSeverity::Error, core::DiagnosticCode::InvalidSctAuthoringProject, e.what()});
        }
    }));
}
void ProgramOperation::drain() {
    std::vector<core::SctLoadingProgress> updates;
    {
        std::lock_guard lock(pending_->mutex);
        updates.swap(pending_->updates);
    }
    for (const auto &p : updates) {
        setCancellable(p.cancellable && !stop_.stop_requested());
        if (!stop_.stop_requested())
            progress(QString::fromStdString(p.phase), p.completed, p.total, QString::fromStdString(p.item));
    }
}
void ProgramOperation::progress(QString phase, quint64 n, quint64 total, QString item) {
    reportProgress(phase, n, total, ExclusiveOperationProgressUnit::Steps, item);
}
std::optional<ExclusiveOperationAction> ProgramOperation::actionForEdge(std::string_view edge) const {
    if (edge == "continue_next")
        return ExclusiveOperationAction{nextAction_, ExclusiveOperationActionRole::Secondary};
    if (edge == "finish_next")
        return ExclusiveOperationAction{tr("Finish"), ExclusiveOperationActionRole::Primary};
    if (edge == "start")
        return ExclusiveOperationAction{tr("Import project"), ExclusiveOperationActionRole::Primary};
    if (edge == "retry")
        return ExclusiveOperationAction{tr("Open with selected dataset"), ExclusiveOperationActionRole::Primary};
    return {};
}
bool ProgramOperation::edgeEnabled(std::string_view edge) const {
    return edge != "start" || !configurationReady || configurationReady();
}
void ProgramOperation::configureAgain() {
    started_ = false;
    raiseEvent("configure");
}
void ProgramOperation::offerRetry(QString message, Work work) {
    summary_ = message;
    work_ = std::move(work);
    reviewing_ = true;
    setCancellable(true);
    raiseEvent("review");
}
void ProgramOperation::offerNext(QString action, std::function<Work()> prepare, Install install,
                                std::function<QWidget *(QWidget *)> page, bool discardCancelled) {
    nextAction_ = std::move(action);
    nextWork_ = std::move(prepare);
    install_ = std::move(install);
    nextPage_ = std::move(page);
    discardCancelled_ = discardCancelled;
    preparing_ = false;
    cancelPresentation = {};
    recoverFailure = {};
    raiseEvent("next");
}
void ProgramOperation::handleEvent(std::string_view event) {
    if (event == "complete" && choosingNext_) { finish(); return; }
    if (event == "continue_next") {
        work_ = nextWork_();
        preparing_ = false;
        started_ = false;
        stop_ = std::stop_source{};
    }
    if (event == "start" && prepareWork)
        work_ = prepareWork();
    if (event == "retry") {
        reviewing_ = false;
        started_ = false;
        stop_ = std::stop_source{};
    }
    raiseEvent(event);
}
void ProgramOperation::requestCancel() {
    if (done_ || !cancellable())
        return;
    if (reviewing_ || configuring_ || choosingNext_) {
        done_ = true;
        summary_ = tr("Workspace opening cancelled.");
        setCancellable(false);
        raiseEvent("cancelled");
        return;
    }
    stop_.request_stop();
    setCancellable(false);
    progress(tr("Cancellation pending; waiting for the current stage to finish safely"));
    if (preparing_ && cancelPresentation)
        cancelPresentation();
}
void ProgramOperation::finish(QString message) {
    if (done_)
        return;
    done_ = true;
    setCancellable(false);
    summary_ = message.isEmpty() ? tr("Ready") : message;
    if (stop_.stop_requested()) { raiseEvent("cancelled"); return; }
    if (compactOpening) {
        // A saved-project open stays progress-only through its last presentation
        // phase. Do not briefly construct/resize a summary page before dismissal.
        emit activityRaised(static_cast<int>(ExclusiveOperationActivityOutcome::Completed),
                            QStringLiteral("ProgramOperation"), summary_, {});
        requestDismissal();
        return;
    }
    raiseEvent("complete");
    requestDismissal();
}
void ProgramOperation::fail(QString message) {
    if (done_)
        return;
    done_ = true;
    setCancellable(false);
    summary_ = message;
    reportDiagnostics(message);
    raiseEvent("failed");
}
} // namespace salsa::qt
