#include "Editor/EditorSessionPresenter.h"
#include "Application/ExclusiveOperationCoordinator.h"
#include "Application/ProgramOperation.h"
#include <QAction>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace ed = core::editor;
EditorSessionPresenter::EditorSessionPresenter(ExclusiveOperationCoordinator *operations, QWidget *host)
    : QObject(host), operations_(operations), host_(host) {
    undo_ = new QAction(tr("Undo"), this); undo_->setShortcut(QKeySequence::Undo);
    redo_ = new QAction(tr("Redo"), this); redo_->setShortcut(QKeySequence::Redo);
    connect(undo_, &QAction::triggered, this, [this] { history(false); });
    connect(redo_, &QAction::triggered, this, [this] { history(true); });
    connect(operations_, &ExclusiveOperationCoordinator::activeChanged, this, [this] {
        undo_->setEnabled(backend_ && !busy()); redo_->setEnabled(backend_ && !busy());
        emit availabilityChanged();
    });
    undo_->setEnabled(false); redo_->setEnabled(false);
}
EditorSessionPresenter::~EditorSessionPresenter() { readStop_.request_stop(); }
bool EditorSessionPresenter::busy() const { return operations_->active(); }
void EditorSessionPresenter::bind(std::shared_ptr<ed::Backend> backend, ed::Revision revision) {
    ++sessionGeneration_; backend_ = std::move(backend); revision_ = revision;
    inspection_.reset(); reading_ = false;
    select({});
    undo_->setEnabled(backend_ && !busy()); redo_->setEnabled(backend_ && !busy());
}
void EditorSessionPresenter::select(std::optional<ed::Subject> subject) {
    if (subject == selection_ && requestRevision_ == revision_ && (inspection_ || reading_)) return;
    readStop_.request_stop(); readStop_ = std::stop_source{};
    reading_ = false; requestRevision_ = revision_;
    auto generation = ++selectionGeneration_;
    selection_ = subject; inspection_.reset(); emit inspectionChanged();
    if (!subject || !backend_) return;
    reading_ = true;
    const auto revision = revision_;
    auto backend = backend_; auto stop = readStop_.get_token();
    auto *watcher = new QFutureWatcher<core::Result<ed::Inspection>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, revision] {
        auto result = watcher->result(); watcher->deleteLater();
        if (generation != selectionGeneration_ || revision != revision_) return;
        reading_ = false;
        if (!result) {
            for (const auto &d : result.diagnostics()) emit diagnostic(QString::fromStdString(d.message));
            return;
        }
        if (result.value().revision != revision || result.value().subject != selection_) return;
        inspection_ = std::move(result).takeValue(); emit inspectionChanged();
    });
    watcher->setFuture(QtConcurrent::run([backend, revision, subject = *subject, stop] {
        return backend->inspect(revision, subject, stop);
    }));
}
void EditorSessionPresenter::accept(const ed::Change &change) {
    if (change.status.current.project != revision_.project ||
        change.status.current.generation != revision_.generation ||
        change.status.current.revision < revision_.revision) return;
    revision_ = change.status.current;
    emit changed(change);
    select(selection_);
}
void EditorSessionPresenter::submit(QString title,
    std::function<core::Result<ed::Change>(std::stop_token)> work) {
    if (!backend_ || busy() || !host_) return;
    auto generation = sessionGeneration_;
    auto operation = std::make_unique<ProgramOperation>(std::move(title),
        [work = std::move(work)](auto stop, const auto &) {
            auto result = work(stop);
            if (!result) return core::Result<std::any>::failure(result.diagnostics());
            return core::Result<std::any>::success(std::any(std::move(result).takeValue()));
        }, [this, generation](std::any value, ProgramOperation &op) {
            if (generation != sessionGeneration_) { op.fail(tr("The editor session changed.")); return; }
            accept(std::any_cast<ed::Change>(std::move(value))); op.finish();
        });
    (void)operations_->open(std::move(operation), host_);
}
void EditorSessionPresenter::replaceParameter(ed::ParameterKey key, std::string text) {
    if (!inspection_ || !std::holds_alternative<core::SctActionId>(inspection_->subject)) return;
    auto backend = backend_; auto revision = inspection_->revision;
    ed::ReplaceParameter edit{std::get<core::SctActionId>(inspection_->subject), key, std::move(text)};
    submit(tr("Edit parameter"), [backend, revision, edit](auto stop) {
        return backend->replaceParameter(revision, edit, stop);
    });
}
void EditorSessionPresenter::history(bool redo) {
    auto backend = backend_; auto revision = revision_;
    submit(redo ? tr("Redo") : tr("Undo"), [backend, revision, redo](auto stop) {
        return backend->history(revision, redo, stop);
    });
}
}
