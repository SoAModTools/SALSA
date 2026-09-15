#include "Application/PerformanceSettings.h"
#include "Application/HelpWindow.h"
#include "Editor/WorkspaceComponent.h"
#include "Application/ProgramWorkspaceController.h"
#include "Editor/EditorSessionPresenter.h"
#include "Editor/InstructionInspector.h"
#include "Editor/TextEditorComponent.h"
#include "Editor/ScriptEditingActions.h"
#include "Editor/ProductivityActions.h"
#include "Editor/OrganizationComponent.h"
#include "Editor/AuthoringTools.h"
#include "Editor/StoryTools.h"
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
    workspaceView_ = new WorkspaceComponent(shell_);
    auto *workspaceDock = dock(tr("Workspace and sources"), "programWorkspace", workspaceView_, Qt::LeftDockWidgetArea);
    workspaceDock->hide();
    workspaceView_->navigate = [this](auto id) { openScript(id); };
    workspaceView_->refreshRequested = [this] { reviewSources(); };
    workspaceView_->reopenRequested = [this] {
        if(workspace_) openDataset(QString::fromStdWString(workspace_->dataset->dataset().root.wstring()),
                                  QString::fromStdWString(workspace_->workspace->descriptor().root.wstring()));
    };
    workspaceView_->importRequested = [this] { openDataset(); };
    editor_ = new EditorSessionPresenter(operations_, shell_);
    inspector_ = new InstructionInspector(editor_, shell_);
    dock(tr("Instruction inspector"), "programDetails", inspector_, Qt::RightDockWidgetArea);
    connect(editor_, &EditorSessionPresenter::changed, this, &ProgramWorkspaceController::refreshChanged);
    connect(editor_, &EditorSessionPresenter::diagnostic, this, &ProgramWorkspaceController::log);
    editing_=new ScriptEditingActions(editor_,shell_);
    auto *productivity=new ProductivityActions(editor_,shell_);
    auto *organization=new OrganizationComponent(editor_,shell_);
    auto *organizationDock=dock(tr("Notes and bookmarks"),"programOrganization",organization,Qt::RightDockWidgetArea);
    organizationDock->hide();organization->showRequested=[organizationDock]{organizationDock->show();organizationDock->raise();};
    textEditor_=new TextEditorComponent(editor_,shell_);
    auto *textDock=dock(tr("Text editor and preview"),"programTextEditor",textEditor_,Qt::BottomDockWidgetArea);
    auto *navigator=new NavigatorComponent(editor_,shell_);
    auto *navigatorDock=dock(tr("Navigator"),"programNavigator",navigator,Qt::LeftDockWidgetArea);navigatorDock->hide();
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
    file->addAction(tr("Review source changes"), this, [this] { reviewSources(); }, QKeySequence(Qt::Key_F5));
    file->addAction(tr("Import Python SALSA v7 projectâ€¦"), this, [this] { importLegacy(); });
    file->addAction(tr("Save project"), this, [this] { save(); }, QKeySequence::Save);
    file->addAction(tr("Export active script…"), this, [this] { publish(); }, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
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
        workspaceView_->clear();
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
    for(auto *action:productivity->actions())editMenu->addAction(action);
    editMenu->addAction(tr("Promote explicit If / While flow…"), this, [this] { promoteFlow(); });
    editMenu->addAction(tr("Promote shared guards to route dispatch"), this, [this] {
        if (!selectedSubject_ || operations_->active())
            return;
        if (auto *header = std::get_if<core::SctActionId>(&*selectedSubject_))
            edit({ir::SemanticOperation{ir::PromoteDispatch{*header, {}}}});
    });
    editMenu->addSeparator();
    for(auto *action:organization->actions())editMenu->addAction(action);
    auto *authoring = shell_->menuBar()->addMenu(tr("&Authoring"));
    auto* modules=new ModuleTools(editor_,shell_);for(auto* a:modules->actions)authoring->addAction(a);
    auto* languages=new LanguageTools(editor_,shell_);authoring->addAction(languages->action);
    auto* entries=new EntryResourceTools(editor_,shell_);for(auto* a:entries->actions)authoring->addAction(a);
    auto* selectors=new SelectorTools(editor_,shell_);authoring->addAction(selectors->action);
    auto *story=new StoryTools(editor_,shell_);for(auto *a:story->actions)authoring->addAction(a);
    auto *interactions=new InteractionTools(editor_,shell_);authoring->addAction(interactions->action);
    auto *routes=new RouteTools(editor_,shell_);for(auto *a:routes->actions)authoring->addAction(a);

    authoring->addAction(tr("Find variable uses…"), this, [navigator,navigatorDock] { navigator->showVariableSearch();navigatorDock->show();navigatorDock->raise(); });
    authoring->addAction(tr("Edit selected textâ€¦"), this, [this,textDock] { textDock->show(); textDock->raise(); textEditor_->setFocus(); });
    authoring->addAction(tr("Inspect generated instructionsâ€¦"), this, [this] { inspectLowered(); });
    auto *settingsMenu = shell_->menuBar()->addMenu(tr("&Settings"));
    settingsMenu->addAction((new PerformanceSettings(shell_))->action());
    auto *view = shell_->menuBar()->addMenu(tr("&View"));
    view->addAction(editor_->backAction());view->addAction(editor_->forwardAction());
    for (auto *d : shell_->findChildren<QDockWidget *>())
        view->addAction(d->toggleViewAction());
    const auto defaultLayout = shell_->saveState();
    view->addSeparator();
    view->addAction(tr("Reset layout"), this, [this, defaultLayout] { shell_->restoreState(defaultLayout); });
    auto *help = shell_->menuBar()->addMenu(tr("&Help"));
    auto showHelp = [shell](QString topic) { auto *window = new HelpWindow(shell);window->setAttribute(Qt::WA_DeleteOnClose);window->showTopic(topic);window->show(); };
    help->addAction(tr("Current editor guide"),this,[showHelp]{showHelp("WholeProgramIrEditor.md");},QKeySequence(Qt::Key_F1));
    help->addAction(tr("Keyboard shortcuts"),this,[showHelp]{showHelp("KeyboardShortcuts.md");});
    help->addAction(tr("Workspace and source recovery"),this,[showHelp]{showHelp("WorkspaceAndSources.md");});
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
    if(workspace_) workspaceView_->status(workspace_->status.dirty || sessionDirty_,workspace_->needsInitialSave);
    shell_->setWindowTitle(
        workspace_
            ? tr("SALSA â€” %1%2")
                  .arg(QString::fromStdWString(workspace_->workspace->descriptor().root.filename().wstring()),
                       workspace_->status.dirty || sessionDirty_ || workspace_->needsInitialSave ? QStringLiteral(" *")
                                                                                                 : QString{})
            : tr("SALSA"));
}
void ProgramWorkspaceController::reviewSources() {
    if(!workspace_ || operations_->active()) return;
    const auto captured = *workspace_; const auto generation = workspaceGeneration_;
    core::editor::WorkspaceReview review = [captured](auto revision, auto stop, const auto &observer) {
        return ir::EditorWorkspaceIO::reviewSources(captured,revision,stop,observer);
    };
    auto op = std::make_unique<ProgramOperation>(tr("Review workspace source files"),
        [review,captured](auto stop,const auto &observer){return boxed(review(captured.status.current,stop,observer));},
        [this,generation](std::any result,ProgramOperation &op) {
            auto overview=std::any_cast<core::editor::WorkspaceOverview>(std::move(result));
            if(!workspace_||closing_||generation!=workspaceGeneration_||overview.revision!=workspace_->status.current) {
                op.fail(tr("Workspace changed before source review completed."));return;
            }
            overview.dirty |= sessionDirty_;
            std::size_t changed=0,missing=0,added=0;
            for(const auto &source:overview.sources) {
                changed+=source.state==core::editor::SourceState::Changed;
                missing+=source.state==core::editor::SourceState::Missing;
                added+=source.state==core::editor::SourceState::Added;
            }
            log(tr("Source review: %1 changed, %2 missing, %3 not imported. Authored content and imported baselines are unchanged.").arg(changed).arg(missing).arg(added));
            if(auto *dock=qobject_cast<QDockWidget *>(workspaceView_->parentWidget())){dock->show();dock->raise();}
            QPointer<ProgramOperation> pending=&op;
            workspaceView_->present(std::move(overview),[pending]{if(pending)pending->finish();});
        });
    (void)operations_->open(std::move(op),shell_);
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
    workspaceView_->present(ir::EditorWorkspaceIO::overview(*workspace_));
    auto backend=std::make_shared<core::editor::ProgramBackend>(workspace_->service);
    backend->setResourceLoader([dataset=workspace_->dataset](std::string asset,smores::script::ResourceRole role,std::stop_token stop){
        auto locator=core::AssetLocator::fromRelativePath(asset);
        if(!locator)return core::Result<smores::script::Resource>::failure(locator.diagnostics());
        ir::EditorWorkspace context;context.dataset=dataset;
        return ir::EditorWorkspaceIO::inspectResource(context,locator.value(),role,stop);
    });
    editor_->bind(backend, workspace_->status.current);
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
        if (i != documents_.end() && tab.selection && !tab.instructions.current && !tab.semantic.current && op && !op->stopped())
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
    if(savedMode!=workspace_->presentation.tabs.end())tree->setViewStates(savedMode->instructions,savedMode->semantic);
    tree->modeChanged=[this]{markSession();};
    tree->presentationChanged=[this]{markSession();};
    documents_.push_back({tree, model, id, {}});
    tabs_->setCurrentIndex(tabs_->addTab(tree, q(script(id)->name)));
    model->diagnostic = [this](auto message) { log(message); };
    model->measured = [this](auto us, auto rows) {
        status_->setText(tr("Outline page: %1 rows, %2 ms").arg(rows).arg(us / 1000.0, 0, 'f', 2));
    };
    model->ready = [model, tree, completion = std::move(completion)] {
        auto callback = completion;
        model->ready = {};
        tree->restoreSession(std::move(callback));
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
            auto saved = std::ranges::find(out.tabs,it->script,&ir::DocumentTab::script);
            saved->instructions=it->view->viewState(core::editor::ViewMode::Instructions);
            saved->semantic=it->view->viewState(core::editor::ViewMode::Semantic);
            saved->selection = saved->semanticView ? saved->semantic.current : saved->instructions.current;
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
} // namespace salsa::qt
