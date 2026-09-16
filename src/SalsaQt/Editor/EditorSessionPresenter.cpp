#include "Editor/EditorSessionPresenter.h"
#include "Application/ExclusiveOperationCoordinator.h"
#include "Application/ProgramOperation.h"
#include <QAction>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
namespace salsa::qt {
namespace ed = core::editor;
EditorSessionPresenter::EditorSessionPresenter(ExclusiveOperationCoordinator *operations, QWidget *host)
    : QObject(host), operations_(operations), host_(host) {
    undo_ = new QAction(tr("Undo"), this);
    undo_->setShortcut(QKeySequence::Undo);
    redo_ = new QAction(tr("Redo"), this);
    redo_->setShortcut(QKeySequence::Redo);
    connect(undo_, &QAction::triggered, this, [this] { history(false); });
    connect(redo_, &QAction::triggered, this, [this] { history(true); });
    connect(operations_, &ExclusiveOperationCoordinator::activeChanged, this, [this] {
        undo_->setEnabled(backend_ && !busy());
        redo_->setEnabled(backend_ && !busy());
        emit availabilityChanged();
    });
    undo_->setEnabled(false);
    redo_->setEnabled(false);
    back_ = new QAction(tr("Back"), this);
    back_->setShortcut(QKeySequence::Back);
    forward_ = new QAction(tr("Forward"), this);
    forward_->setShortcut(QKeySequence::Forward);
    back_->setEnabled(false);
    forward_->setEnabled(false);
    connect(back_, &QAction::triggered, this, [this] {
        if (navigationAt_ > 0) {
            historyTarget_ = navigation_[--navigationAt_];
            navigate(*historyTarget_);
        }
    });
    connect(forward_, &QAction::triggered, this, [this] {
        if (navigationAt_ + 1 < navigation_.size()) {
            historyTarget_ = navigation_[++navigationAt_];
            navigate(*historyTarget_);
        }
    });
}
EditorSessionPresenter::~EditorSessionPresenter() { readStop_.request_stop(); }
EditorSessionPresenter *EditorSessionPresenter::createViewSession(QWidget *owner) {
    auto *view = new EditorSessionPresenter(operations_, host_);
    view->setParent(owner);
    view->changeSink_ = this;
    view->bind(backend_, revision_);
    // Share revision notifications without moving another view's selection.
    auto forward = [](EditorSessionPresenter *target, const ed::Change &change) {
        if (target->revision() == change.status.current)
            return;
        auto notification = change;
        notification.selected.clear();
        notification.suggestedSelection.reset();
        target->accept(notification);
    };
    connect(view, &EditorSessionPresenter::changed, this,
            [this, forward](const ed::Change &change) { forward(this, change); });
    connect(this, &EditorSessionPresenter::changed, view,
            [view, forward](const ed::Change &change) { forward(view, change); });
    connect(view, &EditorSessionPresenter::diagnostic, this, &EditorSessionPresenter::diagnostic);
    connect(this, &EditorSessionPresenter::sessionChanged, owner, &QWidget::close);
    return view;
}

bool EditorSessionPresenter::busy() const { return operations_->active(); }
void EditorSessionPresenter::bind(std::shared_ptr<ed::Backend> backend, ed::Revision revision) {
    ++sessionGeneration_;
    backend_ = std::move(backend);
    revision_ = revision;
    inspection_.reset();
    reading_ = false;
    navigation_.clear();
    navigationAt_ = 0;
    historyTarget_.reset();
    back_->setEnabled(false);
    forward_->setEnabled(false);
    emit sessionChanged();
    select({});
    undo_->setEnabled(backend_ && !busy());
    redo_->setEnabled(backend_ && !busy());
}
void EditorSessionPresenter::selectMany(std::vector<ed::Subject> items, std::optional<ed::Subject> current) {
    selectionSet_ = std::move(items);
    select(current);
    emit availabilityChanged();
}
void EditorSessionPresenter::select(std::optional<ed::Subject> subject) {
    if (!subject)
        selectionSet_.clear();
    else if (std::ranges::find(selectionSet_, *subject) == selectionSet_.end())
        selectionSet_ = {*subject};
    if (subject == selection_ && requestRevision_ == revision_ && (inspection_ || reading_))
        return;
    readStop_.request_stop();
    readStop_ = std::stop_source{};
    reading_ = false;
    requestRevision_ = revision_;
    auto generation = ++selectionGeneration_;
    if (subject && (navigation_.empty() || navigation_[navigationAt_] != *subject || historyTarget_)) {
        if (historyTarget_ && subject == historyTarget_)
            historyTarget_.reset();
        else {
            historyTarget_.reset();
            if (!navigation_.empty())
                navigation_.resize(navigationAt_ + 1);
            navigation_.push_back(*subject);
            navigationAt_ = navigation_.size() - 1;
        }
        back_->setEnabled(navigationAt_ > 0);
        forward_->setEnabled(navigationAt_ + 1 < navigation_.size());
    }
    selection_ = subject;
    inspection_.reset();
    emit inspectionChanged();
    if (!subject || !backend_)
        return;
    reading_ = true;
    const auto revision = revision_;
    auto backend = backend_;
    auto stop = readStop_.get_token();
    auto *watcher = new QFutureWatcher<core::Result<ed::Inspection>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, revision] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (generation != selectionGeneration_ || revision != revision_)
            return;
        reading_ = false;
        if (!result) {
            for (const auto &d : result.diagnostics())
                emit diagnostic(QString::fromStdString(d.message));
            return;
        }
        if (result.value().revision != revision || result.value().subject != selection_)
            return;
        inspection_ = std::move(result).takeValue();
        emit inspectionChanged();
    });
    watcher->setFuture(QtConcurrent::run(
        [backend, revision, subject = *subject, stop] { return backend->inspect(revision, subject, stop); }));
}
void EditorSessionPresenter::accept(const ed::Change &change) {
    if (change.status.current.project != revision_.project ||
        change.status.current.generation != revision_.generation ||
        change.status.current.revision < revision_.revision)
        return;
    revision_ = change.status.current;
    if (!change.selected.empty()) {
        selectionSet_ = change.selected;
        selection_ = change.selected.front();
    }
    emit changed(change);
    select(selection_);
    if (!change.selected.empty())
        navigate(change.selected.front());
    else if (change.suggestedSelection)
        navigate(*change.suggestedSelection);
}
void EditorSessionPresenter::submit(QString title,
                                    std::function<core::Result<ed::Change>(std::stop_token)> work) {
    if (!backend_ || busy() || !host_)
        return;
    auto generation = sessionGeneration_;
    auto operation = std::make_unique<ProgramOperation>(
        std::move(title),
        [work = std::move(work)](auto stop, const auto &) {
            auto result = work(stop);
            if (!result)
                return core::Result<std::any>::failure(result.diagnostics());
            return core::Result<std::any>::success(std::any(std::move(result).takeValue()));
        },
        [self = QPointer(this), sink = changeSink_, generation](std::any value, ProgramOperation &op) {
            auto change = std::any_cast<ed::Change>(std::move(value));
            if (!self) {
                if (sink) {
                    change.selected.clear();
                    change.suggestedSelection.reset();
                    sink->accept(change);
                }
                op.finish();
                return;
            }
            if (generation != self->sessionGeneration_) {
                op.fail(tr("The editor session changed."));
                return;
            }
            self->accept(change);
            op.finish();
        });
    (void)operations_->open(std::move(operation), host_);
}
void EditorSessionPresenter::replaceParameter(ed::ParameterKey key, std::string text) {
    if (!inspection_ || !std::holds_alternative<core::SctActionId>(inspection_->subject))
        return;
    auto backend = backend_;
    auto revision = inspection_->revision;
    ed::ReplaceParameter edit{std::get<core::SctActionId>(inspection_->subject), key, std::move(text)};
    submit(tr("Edit parameter"),
           [backend, revision, edit](auto stop) { return backend->replaceParameter(revision, edit, stop); });
}
void EditorSessionPresenter::navigate(ed::Subject subject) { emit navigationRequested(subject); }
void EditorSessionPresenter::execute(QString title, ed::EditIntent intent,
                                     std::optional<ed::Revision> expected) {
    auto backend = backend_;
    auto revision = expected.value_or(revision_);
    submit(std::move(title), [backend, revision, intent = std::move(intent)](auto stop) {
        return backend->execute(revision, intent, stop);
    });
}
void EditorSessionPresenter::runOperation(
    QString title,
    std::function<core::Result<std::string>(std::stop_token, const core::SctLoadingObserver &)> work,
    std::function<void()> completed) {
    if(!backend_ || busy() || !host_)return;
    auto operation = std::make_unique<ProgramOperation>(
        std::move(title),
        [work = std::move(work)](auto stop, const auto &progress) {
            auto result = work(stop, progress);
            if (!result)
                return core::Result<std::any>::failure(result.diagnostics());
            return core::Result<std::any>::success(std::any(std::move(result).takeValue()));
        },
        [self = QPointer(this), revision = revision_,
         completed = std::move(completed)](std::any value, ProgramOperation &op) {
            if (!self || self->revision_ != revision) {
                op.fail(QObject::tr("The editor session changed."));
                return;
            }
            auto text = QString::fromStdString(std::any_cast<std::string>(std::move(value)));
            emit self->diagnostic(text);
            op.finish(text);
            if (completed)
                QMetaObject::invokeMethod(
                    self,
                    [self, revision, completed] {
                        if (self && self->revision_ == revision)
                            completed();
                    },
                    Qt::QueuedConnection);
        });
    (void)operations_->open(std::move(operation),host_);
}
void EditorSessionPresenter::history(bool redo) {
    auto backend = backend_;
    auto revision = revision_;
    submit(redo ? tr("Redo") : tr("Undo"),
           [backend, revision, redo](auto stop) { return backend->history(revision, redo, stop); });
}
} // namespace salsa::qt
