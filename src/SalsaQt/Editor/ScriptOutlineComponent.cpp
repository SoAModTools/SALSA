#include "Editor/ScriptOutlineComponent.h"
#include "Editor/EditorSessionPresenter.h"
#include <QFutureWatcher>
#include <QHeaderView>
#include <QTabBar>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace ed = core::editor;
ScriptOutlineComponent::ScriptOutlineComponent(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session), model_(new ProgramOutlineModel(this)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    modes_ = new QTabBar(this);
    modes_->addTab(tr("Instructions"));
    modes_->addTab(tr("Semantic"));
    layout->addWidget(modes_);
    tree_ = new QTreeView(this);
    layout->addWidget(tree_);
    tree_->setModel(model_);
    tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setColumnHidden(1, true);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (selectionChanged)
            selectionChanged();
    });
    connect(tree_, &QTreeView::doubleClicked, this, [this] {
        if (editRequested)
            editRequested();
    });
    connect(session_, &EditorSessionPresenter::changed, model_, &ProgramOutlineModel::apply);
    connect(modes_, &QTabBar::currentChanged, this, [this](int mode) {
        auto selected = session_->selection();
        model_->setMode(mode ? ed::ViewMode::Semantic : ed::ViewMode::Instructions);
        if (script_.valid()) {
            bind(script_);
            if (selected)
                reveal(*selected);
        }
        if (modeChanged)
            modeChanged();
    });
}
void ScriptOutlineComponent::setSemantic(bool semantic) { modes_->setCurrentIndex(semantic ? 1 : 0); }
void ScriptOutlineComponent::bind(core::SctScriptId script) {
    script_ = script;
    model_->bind(session_->backend(), session_->revision(), script);
}
QModelIndex ScriptOutlineComponent::currentIndex() const { return tree_->currentIndex(); }
void ScriptOutlineComponent::setCurrentIndex(const QModelIndex &i) { tree_->setCurrentIndex(i); }
void ScriptOutlineComponent::scrollTo(const QModelIndex &i) { tree_->scrollTo(i); }
void ScriptOutlineComponent::expand(const QModelIndex &i) { tree_->expand(i); }
void ScriptOutlineComponent::activate() {
    auto *row = model_->row(currentIndex());
    session_->select(row ? std::optional(row->subject) : std::nullopt);
}
void ScriptOutlineComponent::reveal(ed::Subject subject, std::function<void()> complete) {
    const auto revision = session_->revision();
    auto backend = session_->backend();
    auto mode = model_->mode();
    auto generation = ++navigationGeneration_;
    if (!backend) {
        if (complete)
            complete();
        return;
    }
    auto *watcher = new QFutureWatcher<core::Result<ed::NavigationPath>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, revision, generation, complete = std::move(complete)]() mutable {
                auto result = watcher->result();
                watcher->deleteLater();
                if (!result || revision != session_->revision() || generation != navigationGeneration_) {
                    if (!result && revision == session_->revision() && generation == navigationGeneration_)
                        for (const auto &d : result.diagnostics())
                            emit session_->diagnostic(QString::fromStdString(d.message));
                    if (complete)
                        complete();
                    return;
                }
                auto path = std::move(result).takeValue();
                auto *timer = new QTimer(this);
                timer->setInterval(16);
                connect(timer, &QTimer::timeout, this,
                        [this, timer, path = std::move(path), revision, generation, at = std::size_t{1},
                         parent = QModelIndex{}, complete = std::move(complete)]() mutable {
                            auto finish = [&] {
                                timer->stop();
                                timer->deleteLater();
                                if (complete)
                                    complete();
                            };
                            if (revision != session_->revision() || generation != navigationGeneration_) {
                                finish();
                                return;
                            }
                            if (at >= path.subjects.size()) {
                                setCurrentIndex(parent);
                                scrollTo(parent);
                                activate();
                                finish();
                                return;
                            }
                            auto next = model_->find(path.subjects[at]);
                            if (next.isValid()) {
                                parent = next;
                                ++at;
                                if (at < path.subjects.size())
                                    expand(next);
                                return;
                            }
                            if (model_->canFetchMore(parent))
                                model_->fetchMore(parent);
                            else if (!model_->pending(parent)) {
                                emit session_->diagnostic(tr("The selected item is no longer present in this view."));
                                finish();
                            }
                        });
                timer->start();
            });
    watcher->setFuture(
        QtConcurrent::run([backend, revision, subject, mode] { return backend->locate(revision, subject, mode); }));
}
} // namespace salsa::qt
