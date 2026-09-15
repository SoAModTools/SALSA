#include "Editor/ScriptOutlineComponent.h"
#include "Editor/EditorSessionPresenter.h"
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QMimeData>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
namespace salsa::qt {
namespace ed = core::editor;
namespace {
class SelectionTree final : public QTreeView {
  public:
    EditorSessionPresenter *session;
    std::function<std::vector<ed::Subject>()> selected;
    std::vector<ed::Subject> dragging;
    ed::Revision dragRevision;
    SelectionTree(EditorSessionPresenter *s, QWidget *p) : QTreeView(p), session(s) {
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDefaultDropAction(Qt::MoveAction);
    }

  protected:
    void startDrag(Qt::DropActions) override {
        if (session->busy())
            return;
        dragging = selected();
        if (dragging.empty())
            return;
        dragRevision = session->revision();
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData;
        mime->setData("application/x-salsa-selection", "move");
        drag->setMimeData(mime);
        drag->exec(Qt::MoveAction);
        dragging.clear();
        drag->deleteLater();
    }
    void dragEnterEvent(QDragEnterEvent *e) override {
        if (e->mimeData()->hasFormat("application/x-salsa-selection"))
            e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *e) override {
        if (e->mimeData()->hasFormat("application/x-salsa-selection") &&
            indexAt(e->position().toPoint()).isValid())
            e->acceptProposedAction();
        else
            e->ignore();
    }
    void dropEvent(QDropEvent *e) override {
        auto *source = dynamic_cast<SelectionTree *>(e->source());
        auto *m = static_cast<ProgramOutlineModel *>(model());
        auto *row = m->row(indexAt(e->position().toPoint()));
        if (!source || !row || source->session != session || source->dragRevision != session->revision() ||
            session->busy()) {
            e->ignore();
            return;
        }
        session->execute(tr("Move selection"), ed::MoveSelection{source->dragging, row->subject},
                         source->dragRevision);
        e->acceptProposedAction();
    }
};
} // namespace
ScriptOutlineComponent::ScriptOutlineComponent(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session), model_(new ProgramOutlineModel(this)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    modes_ = new QTabBar(this);
    modes_->addTab(tr("Instructions"));
    modes_->addTab(tr("Semantic"));
    layout->addWidget(modes_);
    auto *selectionTree = new SelectionTree(session, this);
    tree_ = selectionTree;
    selectionTree->selected = [this] { return selectedItems(); };
    layout->addWidget(tree_);
    tree_->setModel(model_);
    tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setColumnHidden(1, true);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (isVisible() && window()->isActiveWindow() && !restoring_ && !restoringSession_ &&
            selectionChanged)
            selectionChanged();
    });
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (!restoring_)
            activate();
    });
    auto presentationChange = [this] {
        if (!restoring_ && !restoringSession_ && presentationChanged)
            presentationChanged();
    };
    connect(tree_, &QTreeView::expanded, this, presentationChange);
    connect(tree_, &QTreeView::collapsed, this, presentationChange);
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, presentationChange);
    connect(tree_, &QTreeView::doubleClicked, this, [this] {
        if (editRequested)
            editRequested();
    });
    connect(session_, &EditorSessionPresenter::changed, this, [this](const ed::Change &change) {
        restoreSelection_ = change.selected.empty() || !isVisible() ? selectedItems() : change.selected;
        auto *row = model_->row(currentIndex());
        restoreCurrent_ = !change.selected.empty() && isVisible() ? std::optional(change.selected.front())
                          : row                                   ? std::optional(row->subject)
                                                                  : std::nullopt;
        restoreExpanded_.clear();
        std::function<void(QModelIndex)> collect = [&](QModelIndex parent) {
            for (int i = 0; i < model_->rowCount(parent); ++i) {
                auto index = model_->index(i, 0, parent);
                if (tree_->isExpanded(index)) {
                    if (auto *r = model_->row(index))
                        restoreExpanded_.push_back(r->subject);
                    collect(index);
                }
            }
        };
        collect({});
        restoring_ = true;
        model_->apply(change);
        restoreVisible();
    });
    model_->pageReady = [this] { restoreVisible(); };

    connect(modes_, &QTabBar::currentChanged, this, [this](int mode) {
        const auto oldMode = model_->mode();
        viewStates_[oldMode == ed::ViewMode::Semantic ? 1 : 0] = viewState(oldMode);
        model_->setMode(mode ? ed::ViewMode::Semantic : ed::ViewMode::Instructions);
        if (script_.valid()) {
            bind(script_);
            restoreSession();
        }
        if (modeChanged)
            modeChanged();
    });
}
void ScriptOutlineComponent::setSemantic(bool semantic) { modes_->setCurrentIndex(semantic ? 1 : 0); }
void ScriptOutlineComponent::bind(core::SctScriptId script) {
    script_ = script;
    model_->bind(session_->backend(), session_->revision(),
                 module_ ? ed::Subject{*module_} : ed::Subject{script});
}
void ScriptOutlineComponent::bindModule(core::SctScriptId script, core::SctModuleId module) {
    module_ = module;
    bind(script);
}
QModelIndex ScriptOutlineComponent::currentIndex() const { return tree_->currentIndex(); }
void ScriptOutlineComponent::setCurrentIndex(const QModelIndex &i) { tree_->setCurrentIndex(i); }
void ScriptOutlineComponent::scrollTo(const QModelIndex &i) { tree_->scrollTo(i); }
void ScriptOutlineComponent::expand(const QModelIndex &i) { tree_->expand(i); }
std::vector<ed::Subject> ScriptOutlineComponent::selectedItems() const {
    std::vector<ed::Subject> items;
    for (const auto &i : tree_->selectionModel()->selectedRows())
        if (auto *row = model_->row(i))
            items.push_back(row->subject);
    return items;
}
ed::OutlineViewState ScriptOutlineComponent::viewState(ed::ViewMode mode) const {
    if (mode != model_->mode() || restoringSession_)
        return viewStates_[mode == ed::ViewMode::Semantic ? 1 : 0];
    ed::OutlineViewState state;
    state.selected = selectedItems();
    if (auto *row = model_->row(currentIndex()))
        state.current = row->subject;
    std::function<void(QModelIndex)> collect = [&](auto parent) {
        for (int n = 0; n < model_->rowCount(parent); ++n) {
            auto index = model_->index(n, 0, parent);
            if (tree_->isExpanded(index)) {
                if (auto *row = model_->row(index))
                    state.expanded.push_back(row->subject);
                collect(index);
            }
        }
    };
    collect({});
    return state;
}
void ScriptOutlineComponent::setViewStates(ed::OutlineViewState instructions, ed::OutlineViewState semantic) {
    viewStates_[0] = std::move(instructions);
    viewStates_[1] = std::move(semantic);
}
void ScriptOutlineComponent::restoreSession(std::function<void()> ready) {
    auto state = viewStates_[model_->mode() == ed::ViewMode::Semantic ? 1 : 0];
    restoringSession_ = true;
    auto generation = ++sessionRestoreGeneration_;
    auto mode = model_->mode();
    auto revision = session_->revision();
    auto targets = state.expanded;
    targets.insert(targets.end(), state.selected.begin(), state.selected.end());
    if (state.current)
        targets.push_back(*state.current);
    auto next = std::make_shared<std::function<void()>>();
    auto cursor = std::make_shared<std::size_t>(0);
    *next = [this, mode, revision, generation, state = std::move(state), targets = std::move(targets), cursor,
             weak = std::weak_ptr(next), ready = std::move(ready)]() mutable {
        auto next = weak.lock();
        if (!next)
            return;
        if (generation != sessionRestoreGeneration_ || mode != model_->mode() ||
            revision != session_->revision()) {
            if (generation == sessionRestoreGeneration_)
                restoringSession_ = false;
            *next = {};
            if (ready)
                ready();
            return;
        }
        if (*cursor < targets.size()) {
            auto subject = targets[(*cursor)++];
            auto expandItem = *cursor <= state.expanded.size();
            reveal(subject, [this, subject, expandItem, next, generation, mode, revision] {
                if (expandItem && generation == sessionRestoreGeneration_ && mode == model_->mode() &&
                    revision == session_->revision()) {
                    auto index = model_->find(subject);
                    if (index.isValid())
                        tree_->expand(index);
                }
                QTimer::singleShot(0, this, [next] {
                    auto run = *next;
                    if (run)
                        run();
                });
            });
            return;
        }
        {
            QSignalBlocker blocker(tree_->selectionModel());
            tree_->selectionModel()->clearSelection();
            for (auto subject : state.selected) {
                auto index = model_->find(subject);
                if (index.isValid())
                    tree_->selectionModel()->select(index,
                                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
            if (state.current) {
                auto index = model_->find(*state.current);
                if (index.isValid()) {
                    tree_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                    scrollTo(index);
                }
            }
        }
        restoringSession_ = false;
        *next = {};
        activate();
        if (selectionChanged)
            selectionChanged();
        if (ready)
            ready();
    };
    QTimer::singleShot(0, this, [next] {
        auto run = *next;
        if (run)
            run();
    });
}
void ScriptOutlineComponent::activate() {
    if (restoring_ || restoringSession_ || !isVisible() || !window()->isActiveWindow())
        return;
    auto *row = model_->row(currentIndex());
    session_->selectMany(selectedItems(), row ? std::optional(row->subject) : std::nullopt);
}
void ScriptOutlineComponent::restoreVisible() {
    if (!restoring_)
        return;
    for (auto subject : restoreExpanded_) {
        auto i = model_->find(subject);
        if (i.isValid())
            tree_->expand(i);
    }
    QSignalBlocker block(tree_->selectionModel());
    for (auto subject : restoreSelection_) {
        auto i = model_->find(subject);
        if (i.isValid())
            tree_->selectionModel()->select(i, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    if (restoreCurrent_) {
        auto i = model_->find(*restoreCurrent_);
        if (i.isValid())
            tree_->selectionModel()->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
    }
    // User selection resumes after all loaded parents have finished their local refresh.
    bool pending = model_->pending({});
    std::function<void(QModelIndex)> check = [&](QModelIndex parent) {
        for (int n = 0; n < model_->rowCount(parent); ++n) {
            auto i = model_->index(n, 0, parent);
            pending |= model_->pending(i);
            if (model_->rowCount(i))
                check(i);
        }
    };
    check({});
    if (!pending) {
        restoring_ = false;
        block.unblock();
        activate();
    }
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
                                QSignalBlocker selectionBlock(tree_->selectionModel());
                                auto desiredSelection = session_->selectionSet();
                                setCurrentIndex(parent);
                                for (auto subject : desiredSelection) {
                                    auto i = model_->find(subject);
                                    if (i.isValid())
                                        tree_->selectionModel()->select(i, QItemSelectionModel::Select |
                                                                               QItemSelectionModel::Rows);
                                }
                                scrollTo(parent);
                                selectionBlock.unblock();
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
                                emit session_->diagnostic(
                                    tr("The selected item is no longer present in this view."));
                                finish();
                            }
                        });
                timer->start();
            });
    watcher->setFuture(QtConcurrent::run([backend, revision, subject, mode, module = module_] {
        auto located = backend->locate(revision, subject, mode);
        if (!located || !module)
            return located;
        auto detail = backend->module(revision, *module);
        if (!detail)
            return core::Result<ed::NavigationPath>::failure(detail.diagnostics());
        // Module roots show their existing body identities directly, omitting
        // the containing script/section/construct ancestors in the full outline.
        auto path = located.value();
        std::optional<ed::NavigationPath> scoped;
        std::size_t longest = 0;
        for (auto body : detail.value().module.bodies) {
            auto ancestry = backend->locate(revision, body, mode);
            if (!ancestry)
                return core::Result<ed::NavigationPath>::failure(ancestry.diagnostics());
            auto prefix = ancestry.value().subjects;
            prefix.pop_back();
            if (prefix.size() < longest || prefix.size() > path.subjects.size() ||
                !std::equal(prefix.begin(), prefix.end(), path.subjects.begin()))
                continue;
            auto start = prefix.size();
            if (start < path.subjects.size() && path.subjects[start] == ed::Subject(body))
                ++start;
            auto projected = path;
            projected.subjects = {ed::Subject(*module), ed::Subject(body)};
            projected.subjects.insert(projected.subjects.end(), path.subjects.begin() + start,
                                      path.subjects.end());
            longest = prefix.size();
            scoped = std::move(projected);
        }
        if (scoped)
            return core::Result<ed::NavigationPath>::success(std::move(*scoped));
        return core::Result<ed::NavigationPath>::failure(
            {{core::DiagnosticSeverity::Info, core::DiagnosticCode::InvalidSctAuthoringProject,
              "This target is outside the module; open it from the script outline."}});
    }));
}
} // namespace salsa::qt
