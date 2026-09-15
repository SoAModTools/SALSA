#include "Application/PerformanceSettings.h"
#include "Application/ProgramWorkspaceController.h"
#include "Editor/EditorSessionPresenter.h"
#include "Editor/InstructionInspector.h"
#include "Editor/TextEditorComponent.h"
#include "Editor/ScriptEditingActions.h"
#include "Editor/EditorQueryWidget.h"
#include "Editor/ScriptOutlineComponent.h"
#include "SalsaCore/Editor/ProgramBackend.h"
#include "Application/ProgramOperation.h"
#include "Application/ImportOperation.h"
#include "Application/ExclusiveOperationCoordinator.h"
#include "Legacy/LegacyConversionDialog.h"
#include "Sct/ProgramValueDialog.h"
#include "Sct/SctMessageEditorWidget.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include <QApplication>
#include <QAbstractListModel>
#include <QListView>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <set>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QLineEdit>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
namespace salsa::qt {
namespace ir = core::program;
namespace {
class LazyListModel final : public QAbstractListModel {
  public:
    std::size_t count = 0;
    std::function<QString(std::size_t)> label;
    explicit LazyListModel(QObject *p) : QAbstractListModel(p) {}
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(count);
    }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        return index.isValid() && index.row() >= 0 && static_cast<std::size_t>(index.row()) < count &&
                       role == Qt::DisplayRole
                   ? QVariant(label(index.row()))
                   : QVariant{};
    }
};
QString q(std::string_view s) {
    return QString::fromUtf8(s.data(), s.size());
}
template <class T> core::Result<std::any> boxed(core::Result<T> r) {
    if (!r)
        return core::Result<std::any>::failure(r.diagnostics());
    auto diagnostics = r.diagnostics();
    return core::Result<std::any>::success(std::any(std::move(r).takeValue()), std::move(diagnostics));
}
template <class T, class F> void await(QObject *owner, std::future<core::Result<T>> future, F done) {
    auto *watcher = new QFutureWatcher<core::Result<T>>(owner);
    QObject::connect(watcher, &QFutureWatcherBase::finished, owner, [watcher, done = std::move(done)]() mutable {
        auto result = watcher->result();
        watcher->deleteLater();
        done(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run([future = std::move(future)]() mutable { return future.get(); }));
}
QString errors(const auto &result) {
    QStringList lines;
    for (const auto &d : result.diagnostics())
        lines << q(d.message);
    return lines.join('\n');
}
std::uint64_t identity(const ir::EditorSubject &s) {
    return std::visit([](auto id) { return id.value; }, s);
}
} // namespace
ProgramWorkspaceController::ProgramWorkspaceController(QMainWindow *shell) : QObject(shell), shell_(shell) {
    shell_->resize(1280, 800);
    shell_->setWindowTitle(tr("SALSA"));
    operations_ = new ExclusiveOperationCoordinator(this);
    tabs_ = new QTabWidget(shell_);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    shell_->setCentralWidget(tabs_);
    auto dock = [&](const QString &name, const char *key, QWidget *widget, Qt::DockWidgetArea area) {
        auto *d = new QDockWidget(name, shell_);
        d->setObjectName(QString::fromLatin1(key));
        d->setWidget(widget);
        shell_->addDockWidget(area, d);
        return d;
    };
    scripts_ = new QListWidget(shell_);
    dock(tr("Scripts"), "programScripts", scripts_, Qt::LeftDockWidgetArea);
    editor_ = new EditorSessionPresenter(operations_, shell_);
    inspector_ = new InstructionInspector(editor_, shell_);
    dock(tr("Instruction inspector"), "programDetails", inspector_, Qt::RightDockWidgetArea);
    connect(editor_, &EditorSessionPresenter::changed, this, &ProgramWorkspaceController::refreshChanged);
    connect(editor_, &EditorSessionPresenter::diagnostic, this, &ProgramWorkspaceController::log);
    editing_=new ScriptEditingActions(editor_,shell_);
    textEditor_=new TextEditorComponent(editor_,shell_);
    auto *textDock=dock(tr("Text editor and preview"),"programTextEditor",textEditor_,Qt::BottomDockWidgetArea);
    auto *navigator=new NavigatorComponent(editor_,shell_);
    dock(tr("Navigator"),"programNavigator",navigator,Qt::LeftDockWidgetArea)->hide();
    connect(editor_,&EditorSessionPresenter::navigationRequested,this,[this](auto subject){
        if(!workspace_)return;auto revision=workspace_->status.current;
        await(this,workspace_->service->locate(revision,subject),[this,subject,revision](auto result){
            if(!result||!workspace_||workspace_->status.current!=revision)return;
            auto id=result.value().script;openScript(id,[this,id,subject]{auto at=std::ranges::find(documents_,id,&Tab::script);if(at!=documents_.end())reveal(*at,subject);});
        });
    });
    activity_ = new QPlainTextEdit(shell_);
    activity_->setReadOnly(true);
    activity_->setMaximumBlockCount(1000);
    auto *activityDock = dock(tr("Activity"), "activityLogDock", activity_, Qt::BottomDockWidgetArea);
    activityDock->hide();
    status_ = new QLabel(shell_);
    shell_->statusBar()->addWidget(status_, 1);
    auto *file = shell_->menuBar()->addMenu(tr("&File"));
    file->addAction(tr("Open / Import Project..."), this, [this] { openDataset(); }, QKeySequence::Open);
    recent_ = file->addMenu(tr("Recent datasets"));
    file->addAction(tr("Import Python SALSA v7 projectâ€¦"), this, [this] { importLegacy(); });
    file->addAction(tr("Save project"), this, [this] { save(); }, QKeySequence::Save);
    file->addAction(tr("Export active scriptâ€¦"), this, [this] { publish(); });
    file->addSeparator();
    file->addAction(tr("Close workspace"), this, [this] {
        if (!readyToLeave())
            return;
        editor_->bind({}, {});
        workspace_.reset();
        ++workspaceGeneration_;
        ++selectionGeneration_;
        for (auto &tab : documents_)
            tab.view->deleteLater();
        documents_.clear();
        tabs_->clear();
        scripts_->clear();
        editor_->select({});
        selected_.reset();
        updateTitle();
    });
    file->addAction(tr("Exit"), shell_, &QWidget::close, QKeySequence::Quit);
    auto *editMenu = shell_->menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(editor_->undoAction());
    editMenu->addAction(editor_->redoAction());
    editMenu->addAction(inspector_->editParameterAction());
    editMenu->addAction(editing_->editAction());editMenu->addAction(editing_->timingAction());
    editMenu->addAction(editing_->structureAction());
    editMenu->addAction(textEditor_->createAction());editMenu->addAction(textEditor_->deleteAction());
    editMenu->addSeparator();
    editMenu->addAction(tr("Edit selected itemâ€¦"), this, [this] { editSelected(); }, QKeySequence(Qt::Key_Return));
    editMenu->addAction(editing_->insertAction());
    editMenu->addAction(tr("Delete selected action"), this, [this] { remove(); }, QKeySequence::Delete);
    editMenu->addAction(tr("Move up"), this, [this] { move(-1); });
    editMenu->addAction(tr("Move down"), this, [this] { move(1); });
    editMenu->addAction(tr("Promote explicit If / While flow…"), this, [this] { promoteFlow(); });
    editMenu->addAction(tr("Promote shared guards to route dispatch"), this, [this] {
        if (!selectedSubject_ || operations_->active())
            return;
        if (auto *header = std::get_if<core::SctActionId>(&*selectedSubject_))
            edit({ir::SemanticOperation{ir::PromoteDispatch{*header, {}}}});
    });
    editMenu->addSeparator();
    editMenu->addAction(tr("Add sectionâ€¦"), this, [this] { addSection(); });
    editMenu->addAction(tr("Rename sectionâ€¦"), this, [this] { renameSection(); });
    editMenu->addAction(tr("Alias and abstraction notesâ€¦"), this, [this] { annotate(); });
    auto *authoring = shell_->menuBar()->addMenu(tr("&Authoring"));
    authoring->addAction(tr("Modules, entrypoints and language groupsâ€¦"), this, [this] { tools(); });
    authoring->addAction(tr("Find variable usesâ€¦"), this, [this] { variableUses(); });
    authoring->addAction(tr("Edit selected textâ€¦"), this, [this,textDock] { textDock->show(); textDock->raise(); textEditor_->setFocus(); });
    authoring->addAction(tr("Inspect generated instructionsâ€¦"), this, [this] { inspectLowered(); });
    auto *settingsMenu = shell_->menuBar()->addMenu(tr("&Settings"));
    settingsMenu->addAction((new PerformanceSettings(shell_))->action());
    auto *view = shell_->menuBar()->addMenu(tr("&View"));
    view->addAction(editor_->backAction());view->addAction(editor_->forwardAction());
    for (auto *d : shell_->findChildren<QDockWidget *>())
        view->addAction(d->toggleViewAction());
    connect(scripts_, &QListWidget::itemActivated, this,
            [this](auto *item) { openScript(core::SctScriptId{item->data(Qt::UserRole).toULongLong()}); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (auto *t = active())
            select(*t, t->view->currentIndex());
        else {
            selected_.reset();
            selectedSubject_.reset();
            editor_->select({});
        }
        markSession();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        if (operations_->active())
            return;
        auto *w = tabs_->widget(index);
        std::erase_if(documents_, [&](auto &t) {
            if (t.view != w)
                return false;
            if (workspace_ && preserveSession_)
                std::erase_if(workspace_->presentation.tabs, [&](auto &saved) { return saved.script == t.script; });
            return true;
        });
        tabs_->removeTab(index);
        w->deleteLater();
        markSession();
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this, [this] { markSession(); });
    connect(operations_, &ExclusiveOperationCoordinator::activityRaised, this,
            [this](int, const auto &, const auto &message, const auto &) { log(message); });
    QSettings settings;
    recents_ = settings.value("workspace/recentDatasets").toStringList();
    associations_ = settings.value("workspace/patchAssociations").toStringList();
    shell_->restoreGeometry(settings.value("window/geometry").toByteArray());
    shell_->restoreState(settings.value("window/programState").toByteArray());
    recentMenu();
    // Startup deliberately performs no dataset or workspace I/O.
}
ProgramWorkspaceController::~ProgramWorkspaceController() {
    if (workspace_)
        workspace_->service->shutdown();
}
void ProgramWorkspaceController::log(QString message) {
    activity_->appendPlainText(message);
    status_->setText(message.section('\n', 0, 0));
}
ProgramWorkspaceController::Tab *ProgramWorkspaceController::active() {
    auto *w = tabs_->currentWidget();
    auto i = std::ranges::find_if(documents_, [&](auto &t) { return t.view == w; });
    return i == documents_.end() ? nullptr : &*i;
}
const ir::ScriptSummary *ProgramWorkspaceController::script(core::SctScriptId id) const {
    if (!workspace_)
        return nullptr;
    auto i = std::ranges::find(workspace_->catalog.scripts, id, &ir::ScriptSummary::id);
    return i == workspace_->catalog.scripts.end() ? nullptr : &*i;
}
void ProgramWorkspaceController::recentMenu() {
    recent_->clear();
    for (const auto &root : recents_)
        recent_->addAction(root, this, [this, root] { openDataset(root); });
}
void ProgramWorkspaceController::updateTitle() {
    shell_->setWindowTitle(
        workspace_
            ? tr("SALSA â€” %1%2")
                  .arg(QString::fromStdWString(workspace_->workspace->descriptor().root.filename().wstring()),
                       workspace_->status.dirty || sessionDirty_ || workspace_->needsInitialSave ? QStringLiteral(" *")
                                                                                                 : QString{})
            : tr("SALSA"));
}
void ProgramWorkspaceController::markSession() {
    if (!restoring_ && workspace_) {
        sessionDirty_ = true;
        updateTitle();
    }
}
bool ProgramWorkspaceController::readyToLeave() {
    if (operations_->active()) {
        operations_->focusActive();
        return false;
    }
    if(!textEditor_->prepareToLeave())return false;
    if (!workspace_ || !(workspace_->status.dirty || workspace_->needsInitialSave || sessionDirty_))
        return true;
    auto answer = QMessageBox::question(shell_, tr("Save project"), tr("Save changes before leaving this workspace?"),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save) {
        save();
        return false;
    }
    return answer == QMessageBox::Discard;
}
void ProgramWorkspaceController::closeRequested(QCloseEvent *e) {
    if (operations_->active()) {
        operations_->requestApplicationClose();
        e->ignore();
        return;
    }
    if (!readyToLeave()) {
        e->ignore();
        return;
    }
    closing_ = true;
    editor_->bind({}, {});
    ++workspaceGeneration_;
    QSettings s;
    s.setValue("window/geometry", shell_->saveGeometry());
    s.setValue("window/programState", shell_->saveState());
    e->accept();
}
void ProgramWorkspaceController::openDataset(QString root, QString destination, bool legacy) {
    if (!readyToLeave()) return;
    struct Choice {
        ir::EditorOpenRequest request;
        QLabel *status = nullptr;
        QFormLayout *overrides = nullptr;
        std::vector<core::Diagnostic> pending;
        std::set<std::string> unresolved;
    };
    auto choice = std::make_shared<Choice>();
    auto suggestedWorkspace = [associations = associations_](const QString &path) {
        for (int i = 0; i + 1 < associations.size(); i += 2)
            if (associations[i].compare(path, Qt::CaseInsensitive) == 0) return associations[i + 1];
        auto source = std::filesystem::path(path.toStdWString());
        return path.isEmpty() ? QString{} : QString::fromStdWString(
            (source.parent_path() / (source.filename().wstring() + L".salsa-workspace")).wstring());
    };
    if (destination.isEmpty()) destination = suggestedWorkspace(root);
    choice->request.dataset.datasetRoot = std::filesystem::path(root.toStdWString());
    choice->request.workspace = std::filesystem::path(destination.toStdWString());
    const auto generation = workspaceGeneration_;
    auto operation = std::make_unique<ProgramOperation>(tr("Open / Import Project"), ProgramOperation::Work{},
        [this, generation](std::any value, ProgramOperation &op) {
            if (closing_ || generation != workspaceGeneration_) {
                op.fail(tr("Workspace changed before opening finished."));
                return;
            }
            auto prepared = std::any_cast<std::shared_ptr<ir::EditorWorkspace>>(std::move(value));
            auto root = QString::fromStdWString(prepared->dataset->dataset().root.wstring());
            auto destination = QString::fromStdWString(prepared->workspace->descriptor().root.wstring());
            for (int i = associations_.size() - 2; i >= 0; i -= 2)
                if (associations_[i].compare(root, Qt::CaseInsensitive) == 0) {
                    associations_.removeAt(i + 1);
                    associations_.removeAt(i);
                }
            associations_.prepend(destination);
            associations_.prepend(root);
            recents_.removeAll(root);
            recents_.prepend(root);
            while (recents_.size() > 12)
                recents_.removeLast();
            QSettings s;
            s.setValue("workspace/patchAssociations", associations_);
            s.setValue("workspace/recentDatasets", recents_);
            recentMenu();
            install(std::move(prepared), op);
        },
        true);
    auto *native = operation.get();
    operation->prepareWork = [choice] {
        return [request = choice->request](auto stop, const auto &observer) {
            return boxed(ir::EditorWorkspaceIO::open(request, stop, observer));
        };
    };
    operation->configurationReady = [choice] {
        return !choice->request.dataset.datasetRoot.empty() && !choice->request.workspace.empty() && choice->unresolved.empty();
    };
    operation->configurationPage = [this, choice, root, destination, suggestedWorkspace, native](QWidget *parent) {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        auto *description = new QLabel(tr("Open an extraction folder or import a legacy v7 project. "
            "SPICE detects native byte order and assesses text per script. "
            "The authoring project is independent of platform and region."), page);
        description->setWordWrap(true); layout->addWidget(description);
        auto *form = new QFormLayout;
        auto pathRow = [&](QString initial, QString title, auto changed) {
            auto *row = new QWidget(page); auto *line = new QHBoxLayout(row); line->setContentsMargins(0,0,0,0);
            auto *edit = new QLineEdit(initial, row); auto *browse = new QPushButton(tr("Browse…"), row);
            line->addWidget(edit); line->addWidget(browse);
            connect(browse, &QPushButton::clicked, row, [edit, title, row] {
                auto path = QFileDialog::getExistingDirectory(row, title, edit->text());
                if (!path.isEmpty()) edit->setText(path);
            });
            connect(edit, &QLineEdit::textChanged, native, changed);
            form->addRow(title, row); return edit;
        };
        auto *source = pathRow(root, tr("Extraction folder:"), [choice, native](const QString &path) {
            choice->request.dataset.datasetRoot = std::filesystem::path(path.toStdWString());
            choice->request.textOverrides.clear(); choice->unresolved.clear(); choice->pending.clear();
            choice->request.acceptance = core::WorkspaceDatasetAcceptance::ExactOnly;
            if (choice->status) choice->status->clear();
            if (choice->overrides) while (choice->overrides->rowCount()) choice->overrides->removeRow(0);
            emit native->presentationChanged();
        });
        auto *workspace = pathRow(destination, tr("Authoring workspace:"), [choice, native](const QString &path) {
            choice->request.workspace = std::filesystem::path(path.toStdWString());
            choice->request.acceptance = core::WorkspaceDatasetAcceptance::ExactOnly; emit native->presentationChanged();
        });
        connect(source, &QLineEdit::textChanged, native, [workspace, suggestedWorkspace](const QString &path) {
            workspace->setText(suggestedWorkspace(path));
        });
        layout->addLayout(form);
        auto *advanced = new QCheckBox(tr("Override native source text detection"), page);
        auto *convention = new QComboBox(page);
        convention->addItem(tr("Automatic (SPICE assessment per script)"), -1);
        for (const auto &c : spice::sct::sctKnownTextConventions()) convention->addItem(q(c.stableName), static_cast<int>(c.convention));
        layout->addWidget(advanced); layout->addWidget(convention); convention->hide();
        connect(advanced, &QCheckBox::toggled, convention, &QWidget::setVisible);
        connect(advanced, &QCheckBox::toggled, native, [convention](bool enabled) { if (!enabled) convention->setCurrentIndex(0); });
        connect(convention, &QComboBox::currentIndexChanged, native, [choice, convention] {
            choice->request.textConvention.reset();
            if (convention->currentData().toInt() >= 0)
                choice->request.textConvention = static_cast<spice::sct::SctKnownTextConvention>(convention->currentData().toInt());
        });
        choice->status = new QLabel(page); choice->status->setWordWrap(true); layout->addWidget(choice->status);
        choice->overrides = new QFormLayout; layout->addLayout(choice->overrides);
        layout->addStretch(); return page;
    };
    operation->configurationEntered = [choice, native] {
        if (choice->pending.empty()) return;
        QStringList messages;
        for (const auto &d : choice->pending) {
            messages << q(d.message);
            if (!d.path) continue;
            auto key = d.path->generic_string();
            if (!choice->unresolved.insert(key).second) continue;
            auto *combo = new QComboBox;
            combo->addItem(tr("Choose source text convention…"), -1);
            for (const auto &c : spice::sct::sctKnownTextConventions()) combo->addItem(q(c.stableName), static_cast<int>(c.convention));
            choice->overrides->addRow(q(key), combo);
            connect(combo, &QComboBox::currentIndexChanged, native, [choice, native, combo, key] {
                if (combo->currentData().toInt() < 0) choice->unresolved.insert(key);
                else { choice->request.textOverrides[key] = static_cast<spice::sct::SctKnownTextConvention>(combo->currentData().toInt()); choice->unresolved.erase(key); }
                emit native->presentationChanged();
            });
        }
        choice->status->setText(messages.join('\n')); choice->pending.clear(); emit native->presentationChanged();
    };
    operation->recoverFailure = [choice](const auto &diagnostics, ProgramOperation &op) {
        for (const auto &d : diagnostics)
            if (d.code == core::DiagnosticCode::SctSourceTextSelectionRequired) choice->pending.push_back(d);
        if (!choice->pending.empty()) { op.configureAgain(); return true; }
        if (std::ranges::none_of(diagnostics, [](const auto &d) {
                return d.message.find("Explicit reassociation is required") != std::string::npos;
            })) return false;
        choice->request.acceptance = core::WorkspaceDatasetAcceptance::UserConfirmedReassociation;
        op.offerRetry(tr("This workspace was associated with a different dataset fingerprint. Open it with the selected dataset? "
                         "Existing authored content and import identities will be retained."), op.prepareWork());
        return true;
    };
    auto converted = [choice, native](QString source, QString workspace) {
        choice->request = {};
        choice->request.dataset.datasetRoot = std::filesystem::path(source.toStdWString());
        choice->request.workspace = std::filesystem::path(workspace.toStdWString());
        native->prepareOpening();
    };
    (void)operations_->open(std::make_unique<ImportOperation>(std::move(operation), std::move(converted), legacy), shell_);
}
void ProgramWorkspaceController::install(std::shared_ptr<ir::EditorWorkspace> prepared, ProgramOperation &op) {
    ++workspaceGeneration_;
    ++selectionGeneration_;
    restoring_ = true;
    preserveSession_ = false;
    sessionDirty_ = false;
    selected_.reset();
    selectedSubject_.reset();
    editor_->select({});
    for (auto &t : documents_)
        t.view->deleteLater();
    documents_.clear();
    tabs_->clear();
    scripts_->clear();
    workspace_ = std::move(prepared);
    editor_->bind(std::make_shared<core::editor::ProgramBackend>(workspace_->service), workspace_->status.current);
    updateTitle();
    log(tr("Workspace prepared in %1 ms; %2 resident scripts. Views request pages of at most 128 rows.")
            .arg(workspace_->openedMicroseconds / 1000.0, 0, 'f', 1)
            .arg(workspace_->catalog.scripts.size()));
    log(tr("Resident IR: %1 action pages, %2 value pages, %3 child references, %4 UTF-8 bytes. These counts exclude "
           "indexes and undo history.")
            .arg(workspace_->catalog.storage.actionPages)
            .arg(workspace_->catalog.storage.valuePages)
            .arg(workspace_->catalog.childReferences)
            .arg(workspace_->catalog.characterBytes));
    QPointer<ProgramOperation> operation = &op;
    op.cancelPresentation = [this, operation] { preserveSession_ = true; };
    auto at = std::make_shared<std::size_t>(0);
    auto populate = std::make_shared<std::function<void()>>();
    *populate = [this, operation, at, populate] {
        if (!operation) {
            *populate = {};
            return;
        }
        auto end = std::min(*at + 128, workspace_->catalog.scripts.size());
        for (; *at < end; ++*at) {
            const auto &s = workspace_->catalog.scripts[*at];
            auto *item = new QListWidgetItem(q(s.name), scripts_);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(s.id.value));
        }
        operation->progress(tr("Building script catalog"), *at, workspace_->catalog.scripts.size());
        if (*at < workspace_->catalog.scripts.size()) {
            QTimer::singleShot(0, this, *populate);
            return;
        }
        *populate = {};
        restoreTabs(operation);
    };
    QTimer::singleShot(0, this, *populate);
}
void ProgramWorkspaceController::restoreTabs(QPointer<ProgramOperation> op, std::size_t at) {
    if (!op)
        return;
    const auto saved = workspace_->presentation.tabs;
    if (op->stopped() || at >= saved.size()) {
        if (op->stopped())
            preserveSession_ = true;
        if (workspace_->presentation.activeTab)
            for (auto &t : documents_)
                if (t.script == *workspace_->presentation.activeTab)
                    tabs_->setCurrentWidget(t.view);
        restoring_ = false;
        updateTitle();
        op->finish(
            op->stopped()
                ? tr("Restoration stopped; the workspace and completed tabs remain open. Saved tabs were retained.")
                : tr("Workspace and saved tabs are ready."));
        return;
    }
    auto tab = saved[at];
    if (!script(tab.script)) {
        log(tr("Saved script %1 is unavailable.").arg(tab.script.value));
        restoreTabs(op, at + 1);
        return;
    }
    op->progress(tr("Restoring script %1 of %2").arg(at + 1).arg(saved.size()), at, saved.size(),
                 q(script(tab.script)->name));
    openScript(tab.script, [this, op, at, tab] {
        auto i = std::ranges::find(documents_, tab.script, &Tab::script);
        if (i != documents_.end() && tab.selection && op && !op->stopped())
            reveal(*i, *tab.selection, [this, op, at] { restoreTabs(op, at + 1); });
        else
            restoreTabs(op, at + 1);
    });
}
void ProgramWorkspaceController::openScript(core::SctScriptId id, std::function<void()> completion) {
    if (!workspace_ || !script(id))
        return;
    auto existing = std::ranges::find(documents_, id, &Tab::script);
    if (existing != documents_.end()) {
        tabs_->setCurrentWidget(existing->view);
        if (completion)
            completion();
        return;
    }
    if (!completion && !operations_->active()) {
        auto op = std::make_unique<ProgramOperation>(
            tr("Open resident script"),
            [](auto, const auto &) { return core::Result<std::any>::success(std::any(true)); },
            [this, id](std::any, ProgramOperation &op) {
                QPointer<ProgramOperation> pending = &op;
                op.progress(tr("Building script outline"));
                openScript(id, [pending] {
                    if (pending)
                        pending->finish();
                });
            });
        (void)operations_->open(std::move(op), shell_);
        return;
    }
    auto *tree = new ScriptOutlineComponent(editor_, tabs_);
    auto *model = tree->outlineModel();
    auto savedMode=std::ranges::find(workspace_->presentation.tabs,id,&ir::DocumentTab::script);
    tree->setSemantic(savedMode==workspace_->presentation.tabs.end() || savedMode->semanticView);
    tree->modeChanged=[this]{markSession();};
    documents_.push_back({tree, model, id, {}});
    tabs_->setCurrentIndex(tabs_->addTab(tree, q(script(id)->name)));
    model->diagnostic = [this](auto message) { log(message); };
    model->measured = [this](auto us, auto rows) {
        status_->setText(tr("Outline page: %1 rows, %2 ms").arg(rows).arg(us / 1000.0, 0, 'f', 2));
    };
    model->ready = [model, completion = std::move(completion)] {
        auto callback = completion;
        model->ready = {};
        if (callback)
            callback();
    };
    tree->selectionChanged = [this, tree] {
        auto i = std::ranges::find(documents_, tree, &Tab::view);
        if (i != documents_.end() && tabs_->currentWidget() == tree)
            select(*i, tree->currentIndex());
    };
    tree->editRequested = [this] { editSelected(); };
    tree->bind(id);
    markSession();
}

void ProgramWorkspaceController::reveal(Tab &tab, ir::EditorSubject subject, std::function<void()> finished) {
    tab.view->reveal(subject,std::move(finished));
}
void ProgramWorkspaceController::select(Tab &tab, const QModelIndex &index) {
    if (!workspace_)
        return;
    selected_.reset();
    selectedSubject_.reset();
    ++selectionGeneration_;
    auto *row = tab.model->row(index);
    if (!row) {
        editor_->select({});
        return;
    }
    tab.selection = row->subject;
    static_cast<ScriptOutlineComponent *>(tab.view)->activate();
    markSession();
    selectedSubject_ = row->subject;
    selected_.reset();
    // Legacy tools acquire their bounded draft only when invoked. Merely
    // selecting a row performs the single editor-contract inspection request.
}
bool ProgramWorkspaceController::prepareSelection(std::function<void()> continuation) {
    if (!workspace_ || operations_->active()) return false;
    if (selected_ || !selectedSubject_) return true;
    auto *activeTab = active();
    if (!activeTab) return false;
    auto &tab = *activeTab;
    const auto index = tab.view->currentIndex();
    const auto *row = tab.model->row(index);
    if (!row) return false;
    auto generation = ++selectionGeneration_;
    auto revision = workspace_->status.current;
    ir::ReadSelection selection;
    selection.descend = false;
    selection.referencedText = false;
    std::visit(
        [&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, core::SctActionId>)
                selection.actions.push_back(id);
            else if constexpr (std::is_same_v<T, ir::SectionId>)
                selection.sections.push_back(id);
            else if constexpr (std::is_same_v<T, ir::ConditionId>)
                selection.conditions.push_back(id);
            else if constexpr (std::is_same_v<T, core::SctSequenceId>)
                selection.sequences.push_back(id);
            else if constexpr (std::is_same_v<T, core::SctTextResourceId>)
                selection.texts.push_back(id);
            else if constexpr (std::is_same_v<T, ir::ArmId>) {
                if (auto *parent = tab.model->row(index.parent()))
                    if (auto *owner = std::get_if<core::SctActionId>(&parent->subject))
                        selection.actions.push_back(*owner);
            }
        },
        row->subject);
    await(this, workspace_->service->read(revision, std::move(selection)), [this, generation, revision, continuation = std::move(continuation)](auto result) mutable {
        if (generation != selectionGeneration_ || !workspace_ || revision != workspace_->status.current)
            return;
        if (!result) {
            log(errors(result));
            return;
        }
        selected_ = result.value();
        continuation();

    });
    return false;
}

void ProgramWorkspaceController::edit(std::vector<ir::Primitive> changes) {
    if (!workspace_ || operations_->active())
        return;
    auto service = workspace_->service;
    auto revision = workspace_->status.current;
    auto generation = workspaceGeneration_;
    auto op = std::make_unique<ProgramOperation>(
        tr("Apply authoring change"),
        [service, request = ir::EditRequest{revision, std::move(changes)}](auto stop, const auto &observer) {
            observer({"Applying local authoring change", 0, 0, {}});
            return boxed(service->edit(request, stop).get());
        },
        [this, generation](std::any result, ProgramOperation &op) {
            if (generation != workspaceGeneration_) {
                op.fail(tr("Workspace changed."));
                return;
            }
            changed(std::any_cast<ir::ChangeReceipt>(std::move(result)));
            op.finish();
        });
    (void)operations_->open(std::move(op), shell_);
}
void ProgramWorkspaceController::changed(const ir::ChangeReceipt &receipt) {
    editor_->accept(core::editor::ProgramBackend::describe(receipt));
}
void ProgramWorkspaceController::refreshChanged(const core::editor::Change &receipt) {
    workspace_->status = receipt.status;
    selected_.reset();
    ++selectionGeneration_;
    updateTitle();
    for (const auto &d : receipt.diagnostics)
        log(q(d.message));
    if (auto *t = active())
        if (t->selection)
            reveal(*t, *t->selection);
    if (receipt.catalogChanged) {
        auto revision = receipt.status.current;
        await(this, workspace_->service->catalog(revision), [this, revision](auto result) {
            if (!result || !workspace_ || workspace_->status.current != revision)
                return;
            workspace_->catalog = std::move(result).takeValue();
        });
    }
}
ir::Presentation ProgramWorkspaceController::presentation() const {
    auto out = workspace_->presentation;
    if (!preserveSession_)
        out.tabs.clear();
    out.activeTab.reset();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto it = std::ranges::find(documents_, tabs_->widget(i), &Tab::view);
        if (it != documents_.end()) {
            auto previous = std::ranges::find(out.tabs, it->script, &ir::DocumentTab::script);
            if (previous == out.tabs.end())
                out.tabs.push_back({it->script, it->model->mode()==core::editor::ViewMode::Semantic, it->selection});
            else {
                previous->selection = it->selection;
                previous->semanticView=it->model->mode()==core::editor::ViewMode::Semantic;
            }
            if (i == tabs_->currentIndex())
                out.activeTab = it->script;
        }
    }
    return out;
}
void ProgramWorkspaceController::save(std::function<void()> continuation) {
    if (!workspace_ || operations_->active())
        return;
    auto session = *workspace_;
    auto pres = presentation();
    auto op = std::make_unique<ProgramOperation>(
        tr("Save binary authoring project"),
        [session, pres](auto stop, const auto &observer) {
            return boxed(ir::EditorWorkspaceIO::save(session, session.status.current, pres, stop, observer));
        },
        [this, pres, continuation = std::move(continuation)](std::any result, ProgramOperation &op) {
            auto saved = std::any_cast<ir::EditorSaveResult>(std::move(result));
            workspace_->status = saved.status;
            workspace_->checkpointDigest = saved.digest;
            workspace_->presentation = pres;
            workspace_->needsInitialSave = false;
            sessionDirty_ = false;
            updateTitle();
            log(tr("Binary project saved in %1 ms.").arg(saved.elapsedMicroseconds / 1000.0, 0, 'f', 1));
            op.finish();
            if (continuation)
                QTimer::singleShot(0, this, continuation);
        });
    (void)operations_->open(std::move(op), shell_);
}
void ProgramWorkspaceController::publish() {
    auto *tab = active();
    if (!workspace_ || !tab || operations_->active())
        return;
    auto *s = script(tab->script);
    auto destination =
        QFileDialog::getSaveFileName(shell_, tr("Export native SCT"), q(s->name) + ".sct", tr("SCT scripts (*.sct)"));
    if (destination.isEmpty())
        return;
    spice::sct::SctDocumentExportOptions options{s->context.platform == core::GamePlatform::Dreamcast
                                                     ? spice::sct::SctPlatform::Dreamcast
                                                     : spice::sct::SctPlatform::GameCube,
                                                 *spice::sct::sctTextEncodingFor(s->textConvention.value_or(
                                                     spice::sct::SctKnownTextConvention::ShiftJisByte7F))};
    if (s->context.outputProfile) {
        options.byteOrder = s->context.outputProfile->bigEndian ? spice::sct::SctDocumentOutputByteOrder::BigEndian
                                                                : spice::sct::SctDocumentOutputByteOrder::LittleEndian;
        options.wrapper = s->context.outputProfile->compressed ? spice::sct::SctDocumentOutputWrapper::Aklz
                                                               : spice::sct::SctDocumentOutputWrapper::Raw;
    }
    QDialog profile(shell_);
    profile.setWindowTitle(tr("Native SCT output"));
    auto *form = new QFormLayout(&profile);
    auto *endian = new QComboBox(&profile);
    endian->addItems({tr("Big endian"), tr("Little endian")});
    endian->setCurrentIndex(options.byteOrder == spice::sct::SctDocumentOutputByteOrder::LittleEndian);
    form->addRow(tr("Byte order"), endian);
    auto *compression = new QComboBox(&profile);
    compression->addItems({tr("Raw SCT"), tr("AKLZ")});
    compression->setCurrentIndex(options.wrapper == spice::sct::SctDocumentOutputWrapper::Aklz);
    form->addRow(tr("Compression"), compression);
    auto *encoding = new QComboBox(&profile);
    for (const auto &c : spice::sct::sctKnownTextConventions())
        encoding->addItem(q(c.stableName));
    encoding->setCurrentIndex(
        static_cast<int>(s->textConvention.value_or(spice::sct::SctKnownTextConvention::ShiftJisByte7F)));
    form->addRow(tr("Text convention"), encoding);
    auto *replaceSource = new QCheckBox(tr("Allow replacing files inside the source dataset"), &profile);
    form->addRow(replaceSource);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &profile);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &profile, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &profile, &QDialog::reject);
    if (profile.exec() != QDialog::Accepted)
        return;
    options.byteOrder = endian->currentIndex() ? spice::sct::SctDocumentOutputByteOrder::LittleEndian
                                               : spice::sct::SctDocumentOutputByteOrder::BigEndian;
    options.wrapper = compression->currentIndex() ? spice::sct::SctDocumentOutputWrapper::Aklz
                                                  : spice::sct::SctDocumentOutputWrapper::Raw;
    options.textEncoding = spice::sct::kSctKnownTextConventions[encoding->currentIndex()].encoding;
    const bool replace = replaceSource->isChecked();
    auto session = *workspace_;
    auto id = tab->script;
    auto op = std::make_unique<ProgramOperation>(
        tr("Export native SCT"),
        [session, id, options, replace,
         path = std::filesystem::path(destination.toStdWString())](auto stop, const auto &observer) {
            return boxed(ir::EditorWorkspaceIO::publish(session, session.status.current, id, options, path, replace,
                                                        stop, observer));
        },
        [this, destination](std::any result, ProgramOperation &op) {
            auto exported = std::any_cast<ir::ExportedScript>(std::move(result));
            log(tr("Exported %1 bytes to %2").arg(exported.bytes.size()).arg(destination));
            op.finish();
        });
    (void)operations_->open(std::move(op), shell_);
}
void ProgramWorkspaceController::importLegacy() { openDataset({}, {}, true); }
namespace {
std::optional<std::pair<core::SctSequenceId, std::size_t>> insertion(const ir::ReadScope &scope,
                                                                     const ir::EditorSubject &subject) {
    core::SctSequenceId sequence;
    std::optional<core::SctActionId> after;
    std::visit(
        [&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, core::SctActionId>) {
                auto i = std::ranges::find(scope.actions, id, &ir::Action::id);
                if (i != scope.actions.end()) {
                    sequence = i->owner;
                    after = id;
                }
            } else if constexpr (std::is_same_v<T, ir::SectionId>) {
                auto i = std::ranges::find(scope.sections, id, &ir::Section::id);
                if (i != scope.sections.end())
                    sequence = i->body;
            } else if constexpr (std::is_same_v<T, core::SctSequenceId>)
                sequence = id;
            else if constexpr (std::is_same_v<T, ir::ArmId>) {
                auto i = std::ranges::find(scope.arms, id, &ir::Arm::id);
                if (i != scope.arms.end())
                    sequence = i->body;
            } else if constexpr (std::is_same_v<T, ir::ConditionId>) {
                auto i = std::ranges::find(scope.conditions, id, &ir::ConditionRegion::id);
                if (i != scope.conditions.end())
                    sequence = i->preparation;
            }
        },
        subject);
    auto i = std::ranges::find(scope.sequences, sequence, &ir::Sequence::id);
    if (!sequence.valid() || i == scope.sequences.end())
        return {};
    auto position = i->order->size();
    if (after) {
        auto at = std::ranges::find(*i->order, *after);
        if (at == i->order->end())
            return {};
        position = std::distance(i->order->begin(), at) + 1;
    }
    return std::pair(sequence, position);
}
std::optional<ir::ValueBuffer> conditionInput(QWidget *parent, const QString &initial = QStringLiteral("1")) {
    for (;;) {
        bool ok = false;
        auto text = QInputDialog::getText(parent, QObject::tr("Condition"), QObject::tr("SCPT expression"),
                                          QLineEdit::Normal, initial, &ok);
        if (!ok)
            return {};
        auto parsed = core::SctExpressionLanguage::parse(text.toStdString());
        if (parsed.succeeded())
            return ir::ValueBuffer::expression(*parsed.expression).value();
        QMessageBox::warning(parent, QObject::tr("Invalid expression"),
                             parsed.issues.empty() ? QObject::tr("Cannot parse this expression.")
                                                   : q(parsed.issues.front().message));
    }
}
} // namespace
void ProgramWorkspaceController::editSelected() {
    if (!prepareSelection([this] { editSelected(); })) return;
    if (!selected_ || !selectedSubject_ || operations_->active())
        return;
    if (!selected_->texts.empty()) {
        editText();
        return;
    }
    ir::ValueId value;
    std::optional<core::SctActionId> action;
    std::optional<ir::ConditionId> condition;
    auto editSubject = *selectedSubject_;
    if (auto *arm = std::get_if<ir::ArmId>(&editSubject)) {
        auto found = std::ranges::find(selected_->arms, *arm, &ir::Arm::id);
        if (found != selected_->arms.end())
            editSubject = found->owner;
    }
    if (auto *id = std::get_if<core::SctActionId>(&editSubject)) {
        auto a = std::ranges::find(selected_->actions, *id, &ir::Action::id);
        if (a == selected_->actions.end())
            return;
        if (a->kind == ir::ActionKind::Instruction) {
            action = *id;
            value = a->value;
        } else if (a->kind == ir::ActionKind::Dispatch) {
            QMessageBox::information(shell_, tr("Route dispatch"),
                                     tr("Expand a route and select its Condition to edit the complete entry "
                                        "requirement. Instructions belong in that route body."));
            return;
        } else if (a->kind == ir::ActionKind::Conditional || a->kind == ir::ActionKind::Switch) {
            std::vector<ir::ArmEdit> arms;
            for (const auto &arm : selected_->arms) {
                ir::ArmEdit e;
                e.id = arm.id;
                e.includesDefault = arm.includesDefault;
                e.exit = arm.caseExit;
                auto labels = std::span(selected_->caseLabels).subspan(arm.firstLabel, arm.labelCount);
                e.labels.assign(labels.begin(), labels.end());
                if (arm.condition.valid()) {
                    auto c = std::ranges::find(selected_->conditions, arm.condition, &ir::ConditionRegion::id);
                    e.condition = ir::ProgramValues::slice(selected_->payload, c->result).value();
                }
                arms.push_back(std::move(e));
            }
            QDialog branches(shell_);
            branches.setWindowTitle(tr("Branches"));
            auto *layout = new QVBoxLayout(&branches);
            auto *list = new QListWidget(&branches);
            layout->addWidget(list);
            const bool isSwitch = a->kind == ir::ActionKind::Switch;
            auto refresh = [&] {
                auto index = list->currentRow();
                list->clear();
                for (std::size_t n = 0; n < arms.size(); ++n) {
                    const auto &arm = arms[n];
                    QString label;
                    if (isSwitch) {
                        QStringList values;
                        for (auto x : arm.labels)
                            values << QString::number(x);
                        if (arm.includesDefault)
                            values << tr("default");
                        label = values.join(", ") +
                                (arm.exit == ir::CaseExit::Fallthrough ? tr(" — fall through") : tr(" — exit switch"));
                    } else if (!arm.condition)
                        label = tr("Else");
                    else {
                        auto expression = ir::ProgramValues::expression(*arm.condition, arm.condition->root);
                        label = (n ? tr("Else If ") : tr("If ")) +
                                (expression ? q(core::SctExpressionLanguage::project(expression.value()).text)
                                            : tr("(unavailable)"));
                    }
                    list->addItem(label);
                }
                list->setCurrentRow(arms.empty() ? -1 : std::clamp(index, 0, static_cast<int>(arms.size()) - 1));
            };
            auto button = [&](QString text, auto f) {
                auto *b = new QPushButton(text, &branches);
                layout->addWidget(b);
                connect(b, &QPushButton::clicked, &branches, f);
            };
            auto changeArm = [&](ir::ArmEdit &arm) -> bool {
                if (!isSwitch) {
                    auto expression = conditionInput(&branches);
                    if (!expression)
                        return false;
                    arm.condition = std::move(*expression);
                    return true;
                }
                bool ok;
                QStringList values;
                for (auto n : arm.labels)
                    values << QString::number(n);
                if (arm.includesDefault)
                    values << QStringLiteral("default");
                auto text = QInputDialog::getText(&branches, tr("Case labels"),
                                                  tr("Comma-separated values; 'default' may share this body"),
                                                  QLineEdit::Normal, values.join(", "), &ok);
                if (!ok)
                    return false;
                std::vector<std::int32_t> parsed;
                bool fallback = false;
                for (auto part : text.split(',', Qt::SkipEmptyParts)) {
                    part = part.trimmed();
                    if (part == QStringLiteral("default")) {
                        fallback = true;
                        continue;
                    }
                    auto n = part.toInt(&ok);
                    if (!ok) {
                        QMessageBox::warning(&branches, tr("Case labels"), tr("Enter integer labels or default."));
                        return false;
                    }
                    parsed.push_back(n);
                }
                auto exit = QInputDialog::getItem(&branches, tr("Case continuation"), tr("After this body"),
                                                  {tr("Exit switch"), tr("Fall through to next case")},
                                                  arm.exit == ir::CaseExit::Fallthrough ? 1 : 0, false, &ok);
                if (!ok)
                    return false;
                arm.labels = std::move(parsed);
                arm.includesDefault = fallback;
                arm.exit = exit == tr("Exit switch") ? ir::CaseExit::ExitSwitch : ir::CaseExit::Fallthrough;
                return true;
            };
            button(tr("Edit selected branch"), [&] {
                auto n = list->currentRow();
                if (n < 0)
                    return;
                auto copy = arms[n];
                if (changeArm(copy)) {
                    arms[n] = std::move(copy);
                    refresh();
                }
            });
            button(isSwitch ? tr("Add case") : tr("Add Else If"), [&] {
                ir::ArmEdit arm;
                if (!changeArm(arm))
                    return;
                auto at = arms.end();
                if (!isSwitch && !arms.empty() && !arms.back().condition)
                    --at;
                arms.insert(at, std::move(arm));
                refresh();
            });
            if (!isSwitch)
                button(tr("Add Else"), [&] {
                    if (arms.empty() || arms.back().condition) {
                        arms.push_back({});
                        refresh();
                    }
                });
            button(tr("Remove selected empty branch"), [&] {
                auto n = list->currentRow();
                if (n >= 0) {
                    arms.erase(arms.begin() + n);
                    refresh();
                }
            });
            button(tr("Move selected branch up"), [&] {
                auto n = list->currentRow();
                if (n > 0) {
                    std::swap(arms[n], arms[n - 1]);
                    refresh();
                    list->setCurrentRow(n - 1);
                }
            });
            button(tr("Move selected branch down"), [&] {
                auto n = list->currentRow();
                if (n >= 0 && n + 1 < arms.size()) {
                    std::swap(arms[n], arms[n + 1]);
                    refresh();
                    list->setCurrentRow(n + 1);
                }
            });
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &branches);
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &branches, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &branches, &QDialog::reject);
            refresh();
            if (branches.exec() != QDialog::Accepted)
                return;
            edit({ir::SemanticOperation{ir::ReplaceArms{*id, std::move(arms)}}});
            return;
        } else if (a->kind == ir::ActionKind::Loop) {
            auto &loop = selected_->loops.at(a->detail);
            condition = loop.condition;
            value = std::ranges::find(selected_->conditions, *condition, &ir::ConditionRegion::id)->result;
        } else {
            log(tr("Break and Continue targets are selected when inserting the transfer."));
            return;
        }
    } else if (auto *id = std::get_if<ir::ConditionId>(&*selectedSubject_)) {
        auto c = std::ranges::find(selected_->conditions, *id, &ir::ConditionRegion::id);
        if (c == selected_->conditions.end())
            return;
        condition = *id;
        value = c->result;
    } else if (std::holds_alternative<ir::SectionId>(*selectedSubject_)) {
        renameSection();
        return;
    } else
        return;
    auto detached = ir::ProgramValues::slice(selected_->payload, value);
    if (!detached) {
        log(errors(detached));
        return;
    }
    ProgramValueDialog dialog(std::move(detached).takeValue(), [this](auto kind) { return chooseTarget(kind); }, shell_);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (action)
        edit({ir::ReplaceInstruction{*action, dialog.value()}});
    else if (condition)
        edit({ir::ReplaceCondition{*condition, dialog.value()}});
}
void ProgramWorkspaceController::insert() {
    if (!prepareSelection([this] { insert(); })) return;
    if (!selected_ || !selectedSubject_ || operations_->active())
        return;
    auto place = insertion(*selected_, *selectedSubject_);
    if (!place) {
        log(tr("Select an action or a code body to insert into."));
        return;
    }
    bool ok = false;
    auto type = QInputDialog::getItem(shell_, tr("Insert"), tr("Content"),
                                      {tr("Instruction"), tr("If / Else If / Else"), tr("While"), tr("Do While"),
                                       tr("For"), tr("Switch"), tr("Break"), tr("Continue")},
                                      0, false, &ok);
    if (!ok)
        return;
    if (type == tr("Instruction")) {
        QStringList names;
        std::vector<std::uint16_t> opcodes;
        for (unsigned i = 0; i < 256; ++i)
            if (auto *schema = spice::sct::findSctOpcodeSchema(i)) {
                names << QStringLiteral("%1 — %2").arg(i).arg(q(schema->semantic.mnemonic));
                opcodes.push_back(i);
            }
        auto choice = QInputDialog::getItem(shell_, tr("Instruction"), tr("Opcode"), names, 0, false, &ok);
        if (!ok)
            return;
        spice::sct::SctInstructionFactoryRequest request;
        request.opcode = opcodes.at(names.indexOf(choice));
        auto value = ir::ProgramValues::instruction(request);
        if (!value) {
            log(errors(value));
            return;
        }
        ProgramValueDialog dialog(std::move(value).takeValue(), [this](auto k) { return chooseTarget(k); }, shell_);
        if (dialog.exec() == QDialog::Accepted)
            edit({ir::InsertInstruction{place->first, place->second, dialog.value()}});
        return;
    }
    if (type == tr("Break") || type == tr("Continue")) {
        auto target = chooseTarget(ir::ValueKind::ActionReference);
        if (!target)
            return;
        auto value = target->values.at(target->root.value).data;
        edit({ir::SemanticOperation{ir::InsertTransfer{
            place->first, place->second, type == tr("Break") ? ir::TransferKind::Break : ir::TransferKind::Continue,
            core::SctActionId{value}}}});
        return;
    }
    auto condition = conditionInput(shell_);
    if (!condition)
        return;
    ir::InsertConstruct construct;
    construct.sequence = place->first;
    construct.position = place->second;
    construct.condition = *condition;
    if (type == tr("If / Else If / Else")) {
        construct.kind = ir::ActionKind::Conditional;
        ir::ArmEdit arm;
        arm.condition = *condition;
        construct.arms.push_back(std::move(arm));
    } else if (type == tr("Switch")) {
        construct.kind = ir::ActionKind::Switch;
        ir::ArmEdit arm;
        arm.includesDefault = true;
        construct.arms.push_back(std::move(arm));
    } else {
        construct.kind = ir::ActionKind::Loop;
        construct.loopKind = type == tr("For")        ? ir::LoopKind::For
                             : type == tr("Do While") ? ir::LoopKind::DoWhile
                                                      : ir::LoopKind::While;
    }
    edit({ir::SemanticOperation{std::move(construct)}});
}
void ProgramWorkspaceController::remove() {
    if (!prepareSelection([this] { remove(); })) return;
    if (!selectedSubject_ || operations_->active())
        return;
    if (selected_ && !selected_->texts.empty()) {
        edit({ir::RemoveText{selected_->texts.front().id}});
        return;
    }
    if (auto *id = std::get_if<core::SctActionId>(&*selectedSubject_))
        edit({ir::SemanticOperation{ir::EraseAction{*id}}});
    else if (auto *id = std::get_if<ir::SectionId>(&*selectedSubject_))
        edit({ir::EraseEmptySection{*id}});
}
void ProgramWorkspaceController::move(int direction) {
    if (!prepareSelection([this, direction] { move(direction); })) return;
    if (!selected_ || !selectedSubject_ || operations_->active())
        return;
    auto *id = std::get_if<core::SctActionId>(&*selectedSubject_);
    if (!id)
        return;
    auto place = insertion(*selected_, *selectedSubject_);
    if (!place)
        return;
    auto index = place->second - 1;
    if (direction < 0 && index == 0)
        return;
    auto seq = std::ranges::find(selected_->sequences, place->first, &ir::Sequence::id);
    if (direction > 0 && index + 1 >= seq->order->size())
        return;
    edit({ir::SemanticOperation{
        ir::MoveAction{*id, place->first, static_cast<std::size_t>(static_cast<std::ptrdiff_t>(index) + direction)}}});
}
void ProgramWorkspaceController::addSection() {
    auto *tab = active();
    if (!tab)
        return;
    bool ok = false;
    auto name = QInputDialog::getText(shell_, tr("Add section"), tr("Section name"), QLineEdit::Normal, {}, &ok);
    if (ok)
        edit({ir::AddSection{tab->script, name.toStdString(), script(tab->script)->sections}});
}
void ProgramWorkspaceController::renameSection() {
    if (!prepareSelection([this] { renameSection(); })) return;
    if (!selectedSubject_)
        return;
    auto *id = std::get_if<ir::SectionId>(&*selectedSubject_);
    if (!id)
        return;
    bool ok = false;
    auto name = QInputDialog::getText(
        shell_, tr("Rename section"), tr("Section name"), QLineEdit::Normal,
        selected_ && !selected_->sections.empty() ? q(selected_->sections.front().name) : QString{}, &ok);
    if (ok)
        edit({ir::RenameSection{*id, name.toStdString()}});
}
std::optional<ir::ValueBuffer> ProgramWorkspaceController::chooseTarget(ir::ValueKind kind) {
    auto *tab = active();
    if (!tab || !workspace_)
        return {};
    if (kind == ir::ValueKind::Unresolved) {
        bool ok = false;
        auto choice = QInputDialog::getItem(shell_, tr("Bind reference"), tr("Target type"),
                                            {tr("Action"), tr("Indexed string"), tr("Footer text")}, 0, false, &ok);
        if (!ok)
            return {};
        kind = choice == tr("Action")           ? ir::ValueKind::ActionReference
               : choice == tr("Indexed string") ? ir::ValueKind::IndexedTextReference
                                                : ir::ValueKind::SupplementaryTextReference;
    }
    QDialog dialog(shell_);
    dialog.setWindowTitle(tr("Select target in this script"));
    dialog.resize(600, 550);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tree = new QTreeView(&dialog);
    auto *model = new ProgramOutlineModel(tree);
    tree->setModel(model);
    tree->setUniformRowHeights(true);
    layout->addWidget(tree);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::optional<ir::ValueBuffer> result;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto *row = model->row(tree->currentIndex());
        if (!row)
            return;
        if (auto *id = std::get_if<core::SctActionId>(&row->subject); id && kind == ir::ValueKind::ActionReference) {
            ir::ValueBuffer b;
            b.add(kind, id->value);
            result = std::move(b);
            dialog.accept();
            return;
        }
        ir::ReadSelection selection;
        selection.descend = false;
        selection.referencedText = false;
        if (auto *id = std::get_if<ir::SectionId>(&row->subject))
            selection.sections.push_back(*id);
        else if (auto *id = std::get_if<core::SctTextResourceId>(&row->subject))
            selection.texts.push_back(*id);
        else
            return;
        buttons->setEnabled(false);
        await(&dialog, workspace_->service->read(workspace_->status.current, selection), [&](auto read) {
            buttons->setEnabled(true);
            if (!read || read.value()->texts.empty())
                return;
            const auto &text = read.value()->texts.front();
            if (text.supplementary != (kind == ir::ValueKind::SupplementaryTextReference))
                return;
            ir::ValueBuffer b;
            b.add(kind, text.id.value);
            result = std::move(b);
            dialog.accept();
        });
    });
    model->bind(editor_->backend(), workspace_->status.current, tab->script);
    dialog.exec();
    return result;
}
void ProgramWorkspaceController::editText() {
    if (!prepareSelection([this] { editText(); })) return;
    if (!selected_ || selected_->texts.empty() || operations_->active())
        return;
    const auto text = selected_->texts.front();
    auto decoded = ir::ProgramValues::text(selected_->payload, text.value);
    if (!decoded) {
        log(errors(decoded));
        return;
    }
    auto *tab = active();
    if (!tab)
        return;
    auto *s = script(tab->script);
    if (!s || !s->locator)
        return;
    QDialog dialog(shell_);
    dialog.setWindowTitle(tr("Edit text"));
    dialog.resize(720, 480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *editor = new SctMessageEditorWidget(&dialog);
    layout->addWidget(editor);
    auto value = decoded.value();
    editor->setCommitHandler([&](const auto &, const auto &, const core::SctMessageDraft &draft, auto) {
        auto typed = core::SctMessageAuthoringProfile::materialize(draft);
        if (!typed.succeeded())
            return false;
        value = *typed.message;
        return true;
    });
    editor->setPlainTextCommitHandler([&](const auto &, const auto &, std::string utf8) {
        value = spice::sct::SctPlainText{std::move(utf8)};
        return true;
    });
    editor->setTextValueCommitHandler([&](const auto &, const auto &, auto typed, auto, auto) {
        value = std::move(typed);
        return true;
    });
    // Reused widget IDs are disposable display bindings. The command uses the
    // captured authored TextResourceId, never a SPICE document lookup.
    core::SctTextTarget target = text.supplementary
                                     ? core::SctTextTarget(spice::sct::SctSupplementaryTextId{text.id.value})
                                     : core::SctTextTarget(spice::sct::SctStringId{text.id.value});
    if (!editor->bindText(*s->locator, target, value, text.kind,
                          text.supplementary ? spice::sct::SctTextStorage::Footer
                                             : spice::sct::SctTextStorage::IndexedSection))
        return;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (editor->flushPending())
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        auto b = ir::ProgramValues::text(value);
        if (b)
            edit({ir::ReplaceText{text.id, std::move(b).takeValue()}});
        else
            log(errors(b));
    }
}
void ProgramWorkspaceController::annotate() {
    if (!workspace_ || !selectedSubject_ || operations_->active())
        return;
    auto subject = *selectedSubject_;
    auto revision = workspace_->status.current;
    await(this, workspace_->service->explain(revision, subject), [this, subject, revision](auto result) {
        if (!result || !workspace_ || workspace_->status.current != revision) {
            if (!result)
                log(errors(result));
            return;
        }
        QDialog dialog(shell_);
        dialog.setWindowTitle(tr("Alias and abstraction evidence"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *alias = new QLineEdit(
            result.value().label == std::to_string(identity(subject)) ? QString{} : q(result.value().label), &dialog);
        layout->addWidget(new QLabel(tr("Display alias"), &dialog));
        layout->addWidget(alias);
        auto *evidence = new QPlainTextEdit(&dialog);
        QStringList lines;
        for (const auto &record : result.value().records)
            lines << q(record.description);
        evidence->setPlainText(lines.join("\n\n"));
        evidence->setReadOnly(true);
        layout->addWidget(evidence);
        auto *note = new QPlainTextEdit(&dialog);
        note->setPlaceholderText(tr("Add a user assertion or explanation"));
        layout->addWidget(note);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(650, 450);
        if (dialog.exec() != QDialog::Accepted)
            return;
        ir::Annotation annotation{subject, alias->text().toStdString(), result.value().records};
        if (!note->toPlainText().isEmpty())
            annotation.evidence.push_back({core::SctEvidenceKind::UserAssertion, note->toPlainText().toStdString()});
        edit({ir::SemanticOperation{ir::PutAnnotation{std::move(annotation)}}});
    });
}
void ProgramWorkspaceController::variableUses() {
    if (!workspace_ || operations_->active())
        return;
    bool ok = false;
    auto bank = QInputDialog::getItem(shell_, tr("Variable connections"), tr("Variable bank"),
                                      {tr("BitVar"), tr("ByteVar"), tr("IntVar"), tr("FloatVar")}, 0, false, &ok);
    if (!ok)
        return;
    auto index = QInputDialog::getInt(shell_, tr("Variable connections"), tr("Index"), 0, 0, INT_MAX, 1, &ok);
    if (!ok)
        return;
    ir::Variable variable{bank == tr("BitVar")    ? ir::Bank::Bit
                          : bank == tr("ByteVar") ? ir::Bank::Byte
                          : bank == tr("IntVar")  ? ir::Bank::Integer
                                                  : ir::Bank::Float,
                          static_cast<unsigned>(index)};
    auto revision = workspace_->status.current;
    await(this, workspace_->service->uses(revision, variable), [this, revision, bank, index](auto result) {
        if (!result || !workspace_ || workspace_->status.current != revision) {
            if (!result)
                log(errors(result));
            return;
        }
        auto *dialog = new QDialog(shell_);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("%1[%2] — all script uses").arg(bank).arg(index));
        dialog->resize(680, 500);
        auto *layout = new QVBoxLayout(dialog);
        auto *list = new QListView(dialog);
        layout->addWidget(list);
        auto *model = new LazyListModel(list);
        auto uses = std::make_shared<const std::vector<ir::VariableUse>>(std::move(result.value().uses));
        model->count = uses->size();
        model->label = [uses](std::size_t i) {
            static const char *access[] = {"Read", "Set", "Clear", "Assign", "Modify", "Computed write"};
            const auto &use = (*uses)[i];
            return tr("%1 — action %2")
                .arg(QString::fromLatin1(access[static_cast<unsigned>(use.access)]))
                .arg(use.site.action.value);
        };
        list->setModel(model);
        list->setUniformItemSizes(true);
        connect(list, &QListView::activated, this, [this, dialog, revision, uses](auto index) {
            if (!workspace_ || workspace_->status.current != revision) {
                log(tr("This connection view is from an earlier revision. Run the query again."));
                return;
            }
            auto id = uses->at(index.row()).site.action;
            await(dialog, workspace_->service->locate(revision, id), [this, id](auto path) {
                if (!path) {
                    log(errors(path));
                    return;
                }
                openScript(path.value().script, [this, id] {
                    if (auto *t = active())
                        reveal(*t, id);
                });
            });
        });
        dialog->show();
    });
}
void ProgramWorkspaceController::tools() {
    if (!prepareSelection([this] { tools(); })) return;
    auto *tab = active();
    if (!workspace_ || !tab || operations_->active())
        return;
    auto id = tab->script;
    auto revision = workspace_->status.current;
    await(this, workspace_->service->interactions(revision, id), [this, id, revision](auto result) {
        if (!result || !workspace_ || workspace_->status.current != revision) {
            if (!result)
                log(errors(result));
            return;
        }
        auto data = std::move(result).takeValue();
        QDialog dialog(shell_);
        dialog.setWindowTitle(tr("Script authoring"));
        dialog.resize(720, 540);
        auto *layout = new QVBoxLayout(&dialog);
        auto *list = new QListWidget(&dialog);
        layout->addWidget(list);
        for (std::size_t i = 0; i < data.entrypoints.size(); ++i) {
            const auto &e = data.entrypoints[i];
            auto *row = new QListWidgetItem(tr("Entrypoint: %1 — %2")
                                                .arg(q(e.name), e.required() ? tr("required")
                                                                : e.enabled  ? tr("enabled")
                                                                             : tr("disabled")),
                                            list);
            row->setData(Qt::UserRole, static_cast<int>(i));
            row->setData(Qt::UserRole + 1, 0);
        }
        for (std::size_t i = 0; i < data.languages.size(); ++i) {
            auto *row = new QListWidgetItem(tr("Language group: %1").arg(q(data.languages[i].name)), list);
            row->setData(Qt::UserRole, static_cast<int>(i));
            row->setData(Qt::UserRole + 1, 1);
        }
        std::vector<ir::Module> modules;
        for (const auto &m : workspace_->catalog.modules)
            if (m.script == id) {
                modules.push_back(m);
                auto *row = new QListWidgetItem(tr("Module: %1").arg(q(m.name)), list);
                row->setData(Qt::UserRole, static_cast<int>(modules.size() - 1));
                row->setData(Qt::UserRole + 1, 2);
            }
        std::vector<ir::Primitive> changes;
        auto button = [&](QString label, auto f) {
            auto *b = new QPushButton(label, &dialog);
            layout->addWidget(b);
            connect(b, &QPushButton::clicked, &dialog, f);
            return b;
        };
        auto *activationButton = button(tr("Enable / disable selected entrypoint"), [&] {
            auto *row = list->currentItem();
            if (!row || row->data(Qt::UserRole + 1).toInt() != 0)
                return;
            auto entry = data.entrypoints.at(row->data(Qt::UserRole).toInt());
            entry.enabled = !entry.enabled;
            changes.push_back(ir::SemanticOperation{ir::PutMetadata{std::move(entry)}});
            dialog.accept();
        });
        activationButton->setEnabled(false);
        connect(list, &QListWidget::currentRowChanged, &dialog, [&](int) {
            auto *row = list->currentItem();
            activationButton->setEnabled(row && row->data(Qt::UserRole + 1).toInt() == 0 &&
                                         !data.entrypoints.at(row->data(Qt::UserRole).toInt()).required());
        });
        button(tr("Select whole activation block for selected entrypoint"), [&] {
            auto *row = list->currentItem();
            if (!row || row->data(Qt::UserRole + 1).toInt() != 0)
                return;
            auto entry = data.entrypoints.at(row->data(Qt::UserRole).toInt());
            if (entry.required())
                return;
            auto choose = [&](QString title) -> std::optional<core::SctActionId> {
                QMessageBox::information(&dialog, tr("Activation boundary"), title);
                auto selected = chooseTarget(ir::ValueKind::ActionReference);
                if (!selected)
                    return {};
                return core::SctActionId{selected->values[selected->root.value].data};
            };
            auto source = choose(tr("Select the instruction containing the incoming activation edge."));
            if (!source)
                return;
            auto start =
                choose(tr("Select the first instruction of the whole activation block, including preparation."));
            if (!start)
                return;
            auto continuation =
                choose(tr("Select the bypass continuation after preparation, response and consumption."));
            if (!continuation)
                return;
            bool ok = false;
            auto edge = QInputDialog::getItem(&dialog, tr("Incoming edge"), tr("Edge type"),
                                              {tr("Physical fallthrough"), tr("Target operand")}, 0, false, &ok);
            if (!ok)
                return;
            core::SctLocalActivation boundary{*source, {}, *start, *continuation, false};
            if (edge == tr("Target operand")) {
                auto parameter = QInputDialog::getInt(&dialog, tr("Target operand"), tr("Parameter schema index"), 0, 0,
                                                      255, 1, &ok);
                if (!ok)
                    return;
                auto group = QInputDialog::getInt(&dialog, tr("Switch or repeated target"),
                                                  tr("Repeated group / switch-case ordinal (-1 for fixed parameter)"),
                                                  -1, -1, INT_MAX, 1, &ok);
                if (!ok)
                    return;
                boundary.operand = spice::sct::SctParameterAddress{
                    static_cast<unsigned>(parameter),
                    group < 0 ? std::nullopt : std::optional(static_cast<unsigned>(group))};
            }
            entry.activation = boundary;
            entry.evidence.push_back({core::SctEvidenceKind::UserAssertion,
                                      "Explicit whole-block activation boundary selected in the editor"});
            changes.push_back(ir::SemanticOperation{ir::PutMetadata{entry}});
            dialog.accept();
        });
        button(tr("Rename selected item"), [&] {
            auto *row = list->currentItem();
            if (!row)
                return;
            bool ok = false;
            auto name = QInputDialog::getText(&dialog, tr("Rename"), tr("Display name"), QLineEdit::Normal, {}, &ok);
            if (!ok)
                return;
            auto i = row->data(Qt::UserRole).toInt();
            switch (row->data(Qt::UserRole + 1).toInt()) {
            case 0: {
                auto e = data.entrypoints.at(i);
                e.name = name.toStdString();
                changes.push_back(ir::SemanticOperation{ir::PutMetadata{e}});
                break;
            }
            case 1: {
                auto e = data.languages.at(i);
                e.name = name.toStdString();
                changes.push_back(ir::SemanticOperation{ir::PutMetadata{e}});
                break;
            }
            default: {
                auto e = modules.at(i);
                e.name = name.toStdString();
                changes.push_back(ir::SemanticOperation{ir::PutMetadata{e}});
            }
            }
            dialog.accept();
        });
        button(tr("Create module from selected body"), [&] {
            if (!selected_ || !selectedSubject_)
                return;
            auto place = insertion(*selected_, *selectedSubject_);
            if (!place)
                return;
            bool ok = false;
            auto name = QInputDialog::getText(&dialog, tr("Module"), tr("Name"), QLineEdit::Normal, {}, &ok);
            if (!ok)
                return;
            ir::Module module{{},
                              id,
                              name.toStdString(),
                              {place->first},
                              {{core::SctEvidenceKind::UserAssertion, "User selected this body as a module"}}};
            changes.push_back(ir::SemanticOperation{ir::PutMetadata{std::move(module)}});
            dialog.accept();
        });
        button(tr("Declare named entrypoint from selected section"), [&] {
            if (!selected_ || selected_->sections.empty())
                return;
            const auto &section = selected_->sections.front();
            auto seq = std::ranges::find(selected_->sequences, section.body, &ir::Sequence::id);
            if (seq == selected_->sequences.end() || seq->order->empty())
                return;
            ir::Entrypoint entry;
            entry.script = id;
            entry.name = section.name;
            entry.body = section.body;
            entry.activation = core::SctNamedActivation{section.name, seq->order->front()};
            entry.evidence.push_back(
                {core::SctEvidenceKind::UserAssertion, "Declared by the user without requiring MLD evidence"});
            changes.push_back(ir::SemanticOperation{ir::PutMetadata{std::move(entry)}});
            dialog.accept();
        });
        button(tr("Repair lifecycle binding from selected section"), [&] {
            if (!selected_ || selected_->sections.empty())
                return;
            bool ok = false;
            auto name = QInputDialog::getItem(&dialog, tr("Required lifecycle root"), tr("Bind this existing body as"),
                                              {QStringLiteral("init"), QStringLiteral("loop")}, 0, false, &ok);
            if (ok) {
                changes.push_back(
                    ir::SemanticOperation{ir::BindLifecycle{selected_->sections.front().id, name.toStdString()}});
                dialog.accept();
            }
        });
        button(tr("Create language group"), [&] {
            bool ok = false;
            auto name = QInputDialog::getText(&dialog, tr("Language group"), tr("Name"), QLineEdit::Normal, {}, &ok);
            if (ok) {
                changes.push_back(
                    ir::SemanticOperation{ir::PutMetadata{core::SctLanguageGroup{{}, name.toStdString(), {}}}});
                dialog.accept();
            }
        });
        button(tr("Assign selected text to selected language group"), [&] {
            auto *row = list->currentItem();
            if (!row || row->data(Qt::UserRole + 1).toInt() != 1 || !selected_ || selected_->texts.empty())
                return;
            changes.push_back(ir::SemanticOperation{ir::AssignLanguages{
                selected_->texts.front().id, {data.languages.at(row->data(Qt::UserRole).toInt()).id}}});
            dialog.accept();
        });
        button(tr("Inspect selected module's story effects"), [&] {
            auto *row = list->currentItem();
            if (!row || row->data(Qt::UserRole + 1).toInt() != 2)
                return;
            auto module = modules.at(row->data(Qt::UserRole).toInt());
            await(&dialog, workspace_->service->summarize(revision, module.id), [&](auto summary) {
                if (!summary) {
                    QMessageBox::warning(&dialog, tr("Module"), errors(summary));
                    return;
                }
                QStringList lines;
                for (const auto &effect : summary.value().summary->effects) {
                    if (effect.variable &&
                        (effect.variable->bank == ir::Bank::Bit || effect.variable->bank == ir::Bank::Byte))
                        lines << tr("%1[%2]: action %3, access %4")
                                     .arg(effect.variable->bank == ir::Bank::Bit ? tr("BitVar") : tr("ByteVar"))
                                     .arg(effect.variable->index)
                                     .arg(effect.site.action.value)
                                     .arg(static_cast<int>(effect.access));
                }
                auto *report = new QDialog(&dialog);
                report->setAttribute(Qt::WA_DeleteOnClose);
                auto *l = new QVBoxLayout(report);
                auto *text = new QPlainTextEdit(report);
                text->setReadOnly(true);
                text->setPlainText(lines.join('\n'));
                l->addWidget(text);
                report->resize(680, 450);
                report->show();
            });
        });
        auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted && !changes.empty())
            edit(std::move(changes));
    });
}
void ProgramWorkspaceController::inspectLowered() {
    auto *tab = active();
    if (!workspace_ || !tab || operations_->active())
        return;
    auto service = workspace_->service;
    auto revision = workspace_->status.current;
    auto id = tab->script;
    using Preview = std::pair<ir::Revision, std::shared_ptr<const std::vector<std::string>>>;
    auto op = std::make_unique<ProgramOperation>(
        tr("Inspect generated instructions"),
        [service, revision, id](auto stop, const auto &observer) {
            observer({"Lowering selected script for inspection", 0, 0, {}});
            auto checkpoint = service->checkpoint(revision, stop).get();
            if (!checkpoint)
                return core::Result<std::any>::failure(checkpoint.diagnostics());
            auto uuid = core::generateSctAuthoringUuid();
            if (!uuid)
                return core::Result<std::any>::failure(uuid.diagnostics());
            ir::ProgramMaterializer materializer;
            auto lowered = materializer.materialize(checkpoint.value(), revision, id, {uuid.value()}, stop);
            if (!lowered)
                return core::Result<std::any>::failure(lowered.diagnostics());
            auto lines = std::make_shared<std::vector<std::string>>();
            auto parameter = [](const spice::sct::SctDocumentParameter &p) {
                return std::visit(
                    [](const auto &v) -> std::string {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>)
                            return core::SctExpressionLanguage::project(v).text;
                        else if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>)
                            return std::to_string(v.value);
                        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference> ||
                                           std::is_same_v<T, spice::sct::SctStringReference> ||
                                           std::is_same_v<T, spice::sct::SctSupplementaryTextReference>)
                            return "target " + std::to_string(v.target.value());
                        else
                            return "ordered/unsupported payload";
                    },
                    p.value);
            };
            for (const auto &section : lowered.value().document->sections) {
                if (stop.stop_requested())
                    return core::Result<std::any>::failure({core::DiagnosticSeverity::Error,
                                                            core::DiagnosticCode::InvalidSctAuthoringProject,
                                                            "Inspection cancelled"});
                lines->push_back(section.nameBytes);
                if (auto *code = std::get_if<spice::sct::SctScriptSectionContent>(&section.content))
                    for (const auto &instruction : code->instructions) {
                        auto *schema = spice::sct::findSctOpcodeSchema(instruction.opcode);
                        std::string row =
                            "    " + std::to_string(instruction.id.value()) + ": " +
                            (schema ? std::string(schema->semantic.mnemonic) : std::to_string(instruction.opcode));
                        for (const auto &p : instruction.fixedParameters)
                            row += " | " + std::to_string(p.schemaIndex) + ": " + parameter(p);
                        for (std::size_t group = 0; group < instruction.repeatedParameterGroups.size(); ++group)
                            for (const auto &p : instruction.repeatedParameterGroups[group].parameters)
                                row += " | group " + std::to_string(group) + "/" + std::to_string(p.schemaIndex) +
                                       ": " + parameter(p);
                        if (instruction.scheduledExpression)
                            row += " | schedule: " +
                                   core::SctExpressionLanguage::project(*instruction.scheduledExpression).text;
                        if (instruction.skipRefresh)
                            row += " | skip refresh";
                        lines->push_back(std::move(row));
                    }
            }
            return core::Result<std::any>::success(std::any(Preview{revision, std::move(lines)}));
        },
        [this](std::any value, ProgramOperation &op) {
            auto preview = std::any_cast<Preview>(std::move(value));
            auto *dialog = new QDialog(shell_);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(tr("Generated instructions — captured revision %1").arg(preview.first.revision));
            auto *layout = new QVBoxLayout(dialog);
            auto *list = new QListView(dialog);
            auto *model = new LazyListModel(list);
            model->count = preview.second->size();
            model->label = [rows = preview.second](std::size_t i) { return q((*rows)[i]); };
            list->setModel(model);
            list->setUniformItemSizes(true);
            layout->addWidget(list);
            dialog->resize(900, 600);
            dialog->show();
            op.finish();
        },
        true);
    (void)operations_->open(std::move(op), shell_);
}

void ProgramWorkspaceController::promoteFlow() {
    if (!selectedSubject_ || operations_->active())
        return;
    auto *header = std::get_if<core::SctActionId>(&*selectedSubject_);
    if (!header)
        return;
    const auto id = *header;
    auto continuation = chooseTarget(ir::ValueKind::ActionReference);
    if (continuation)
        edit({ir::SemanticOperation{
            ir::PromoteFlow{id, core::SctActionId{continuation->values[continuation->root.value].data}}}});
}
void ProgramWorkspaceController::variableMeanings() {
    auto *tab = active();
    if (!workspace_ || !tab || operations_->active())
        return;
    const auto scriptId = tab->script;
    const auto revision = workspace_->status.current;
    await(this, workspace_->service->interactions(revision, scriptId), [this, scriptId, revision](auto result) {
        if (!result || !workspace_ || workspace_->status.current != revision)
            return;
        auto definitions = std::move(result).takeValue().meanings;
        QDialog dialog(shell_);
        dialog.setWindowTitle(tr("Variable meanings"));
        auto *form = new QFormLayout(&dialog);
        auto *existing = new QComboBox(&dialog);
        existing->addItem(tr("New meaning"));
        for (const auto &item : definitions)
            existing->addItem(q(item.name));
        form->addRow(tr("Definition"), existing);
        auto *bank = new QComboBox(&dialog);
        bank->addItems({tr("BitVar"), tr("ByteVar"), tr("IntVar"), tr("FloatVar")});
        form->addRow(tr("Variable bank"), bank);
        auto *index = new QLineEdit(QStringLiteral("0"), &dialog);
        form->addRow(tr("Index"), index);
        auto *value = new QLineEdit(&dialog);
        form->addRow(tr("Specific value (blank for any)"), value);
        auto *name = new QLineEdit(&dialog);
        form->addRow(tr("Meaning"), name);
        auto *local = new QCheckBox(tr("Only shell_ script"), &dialog);
        local->setChecked(true);
        form->addRow(local);
        connect(existing, &QComboBox::currentIndexChanged, &dialog, [&](int i) {
            if (!i)
                return;
            const auto &item = definitions.at(i - 1);
            bank->setCurrentIndex(static_cast<int>(item.variable.kind));
            index->setText(QString::number(item.variable.index));
            value->setText(item.value ? QString::number(*item.value) : QString{});
            name->setText(q(item.name));
            local->setChecked(item.script.valid());
        });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        std::optional<ir::VariableMeaning> authored;
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            bool ok;
            auto numericIndex = index->text().toUInt(&ok);
            if (!ok)
                return;
            std::optional<std::int64_t> numericValue;
            if (!value->text().trimmed().isEmpty()) {
                numericValue = value->text().toLongLong(&ok);
                if (!ok)
                    return;
            }
            if (name->text().trimmed().isEmpty())
                return;
            ir::VariableMeaning item;
            if (existing->currentIndex())
                item = definitions.at(existing->currentIndex() - 1);
            item.script = local->isChecked() ? scriptId : core::SctScriptId{};
            item.variable = {static_cast<core::SctVariableKind>(bank->currentIndex()), numericIndex};
            item.value = numericValue;
            item.name = name->text().toStdString();
            item.evidence.push_back({core::SctEvidenceKind::UserAssertion, "Variable meaning supplied by the user"});
            authored = std::move(item);
            dialog.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted && authored)
            edit({ir::SemanticOperation{ir::PutMetadata{std::move(*authored)}}});
    });
}
void ProgramWorkspaceController::resources() {
    auto *tab = active();
    if (!workspace_ || !tab || operations_->active())
        return;
    const auto id = tab->script;
    const auto context = script(id)->context;
    const auto revision = workspace_->status.current;
    QDialog dialog(shell_);
    dialog.setWindowTitle(tr("Optional MLD context"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(
        new QLabel(tr("Selected evidence is advisory. Reselect explicitly after a file changes."), &dialog));
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list);
    for (const auto &item : context.resources)
        list->addItem(q(item.resource.asset));
    auto *remove = new QPushButton(tr("Remove selected association"), &dialog);
    layout->addWidget(remove);
    auto *inspect = new QPushButton(tr("Inspect and select an MLD…"), &dialog);
    layout->addWidget(inspect);
    std::optional<int> erase;
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() >= 0) {
            erase = list->currentRow();
            dialog.accept();
        }
    });
    connect(inspect, &QPushButton::clicked, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (erase) {
        auto associations = context.resources;
        associations.erase(associations.begin() + *erase);
        edit({ir::SemanticOperation{
            ir::ScriptSettings{id, context.name, context.outputProfile, std::move(associations)}}});
        return;
    }
    auto path = QFileDialog::getOpenFileName(shell_, tr("Select MLD in the dataset"),
                                             QString::fromStdWString(workspace_->dataset->dataset().root.wstring()),
                                             tr("MLD files (*.mld *.MLD)"));
    if (path.isEmpty())
        return;
    auto relative = std::filesystem::path(path.toStdWString()).lexically_relative(workspace_->dataset->dataset().root);
    auto locator = core::AssetLocator::fromRelativePath(relative.generic_string());
    if (!locator) {
        log(errors(locator));
        return;
    }
    bool ok;
    auto roleName = QInputDialog::getItem(shell_, tr("Resource role"), tr("Association"),
                                          {tr("Base"), tr("Auxiliary"), tr("Streamed tile")}, 0, false, &ok);
    if (!ok)
        return;
    auto role = roleName == tr("Base")        ? smores::script::ResourceRole::Base
                : roleName == tr("Auxiliary") ? smores::script::ResourceRole::Auxiliary
                                              : smores::script::ResourceRole::StreamedTile;
    auto session = workspace_;
    auto op = std::make_unique<ProgramOperation>(
        tr("Inspect optional resource"),
        [session, locator = locator.value(), role](auto stop, const auto &observer) {
            observer({"Inspecting selected MLD", 0, 0, locator.identityKey()});
            return boxed(ir::EditorWorkspaceIO::inspectResource(*session, locator, role, stop));
        },
        [this, session, context, revision, id](std::any result, ProgramOperation &op) {
            if (workspace_ != session || session->status.current != revision) {
                op.fail(tr("Workspace changed."));
                return;
            }
            auto resource = std::any_cast<smores::script::Resource>(std::move(result));
            auto associations = context.resources;
            auto found = std::ranges::find_if(
                associations, [&](const auto &r) { return r.resource.asset == resource.identity.asset; });
            core::SctScriptContext::ResourceAssociation association{resource.identity, resource.role};
            if (found == associations.end())
                associations.push_back(association);
            else
                *found = association;
            // Resource association never rewrites a handler name, response or baseline.
            std::vector<ir::Primitive> commands{ir::SemanticOperation{
                ir::ScriptSettings{id, context.name, context.outputProfile, std::move(associations)}}};
            QPointer<ProgramOperation> operation(&op);
            await(&op, session->service->edit({revision, std::move(commands)}),
                  [this, session, operation, count = resource.entries.size()](auto changedResult) {
                      if (!operation)
                          return;
                      if (!changedResult) {
                          operation->fail(errors(changedResult));
                          return;
                      }
                      if (workspace_ != session) {
                          operation->fail(tr("Workspace changed."));
                          return;
                      }
                      changed(changedResult.value());
                      operation->finish(
                          tr("Selected resource evidence retained (%1 entries). Script targets are unchanged.")
                              .arg(count));
                  });
        },
        true);
    (void)operations_->open(std::move(op), shell_);
}
} // namespace salsa::qt
