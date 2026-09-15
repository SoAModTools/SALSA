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
        for (const auto &d : result.diagnostics())
            reportDiagnostics(QString::fromStdString(d.message));
        preparing_ = true;
        setCancellable(true);
        try {
            install_(std::move(result).takeValue(), *this);
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
             {"working", R::Processing, P::Visible, L::Compact},
             {"review", R::Review, P::Hidden, L::Standard},
             {"summary", R::Summary, P::Hidden, L::Compact}},
            {{"start", "configure", "start", "working"},
             {"reconfigure", "working", "configure", "configure"},
             {"cancel_configure", "configure", "cancelled", "summary"},
             {"review", "working", "review", "review"},
             {"retry", "review", "retry", "working"},
             {"cancel_review", "review", "cancelled", "summary"},
             {"complete", "working", "complete", "summary"},
             {"failed", "working", "failed", "summary"},
             {"cancelled", "working", "cancelled", "summary"}}};
}
QWidget *ProgramOperation::createPage(std::string_view id, QWidget *parent) {
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
    if (edge == "start")
        return ExclusiveOperationAction{tr("Open project"), ExclusiveOperationActionRole::Primary};
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
void ProgramOperation::handleEvent(std::string_view event) {
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
    if (reviewing_ || configuring_) {
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
