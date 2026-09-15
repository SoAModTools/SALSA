#include "Editor/FeatureUi.h"
#include "Editor/InstructionInspector.h"
#include "Editor/ProductivityActions.h"
#include "Editor/ScriptEditingActions.h"
#include "Editor/ScriptOutlineComponent.h"
#include <QSplitter>
namespace salsa::qt {
using namespace feature_ui;
ModuleTools::ModuleTools(EditorSessionPresenter *s, QWidget *h) : QObject(h), session_(s), host_(h) {
    auto *m = new QAction(tr("Modules…"), this);
    actions << m;
    connect(m, &QAction::triggered, this, [this] { open(); });
    auto *d = new QAction(tr("Named bodies and conditions…"), this);
    actions << d;
    connect(d, &QAction::triggered, this, [this] { definitions(); });
    for (auto *a : actions)
        availability(a, session_);
}
void ModuleTools::open() {
    inventory(session_, this, [this](ed::FeatureInventory data) {
        QDialog dialog(host_);
        dialog.setWindowTitle(tr("Modules"));
        dialog.resize(560, 430);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(
            new QLabel(tr("A module owns its script context and references existing bodies."), &dialog));
        auto *list = new QListWidget(&dialog);
        for (const auto &m : data.modules)
            list->addItem(q(m.name));
        layout->addWidget(list);
        auto *menu = new QMenu(&dialog);
        auto *bar = new QToolBar(&dialog);
        bar->addAction(tr("Module actions"),
                       [menu, bar] { menu->exec(bar->mapToGlobal(QPoint(0, bar->height()))); });
        layout->addWidget(bar);
        menuAction(menu, tr("Open module editor"), &dialog, [&] {
            int i = list->currentRow();
            if (i >= 0) {
                auto m = data.modules[i];
                dialog.accept();
                editModule(m);
            }
        });
        menuAction(menu, tr("Extract selected bodies / sections…"), &dialog, [&] {
            QStringList labels;
            for (const auto &b : data.bodies)
                labels << q(b.name);
            auto chosen = choose(&dialog, tr("Module bodies (existing code stays in place)"), labels);
            if (!chosen || chosen->empty())
                return;
            auto label = name(&dialog, tr("Module name"));
            if (!label)
                return;
            ed::ModuleItem m{{}, data.script, *label};
            for (int i : *chosen)
                m.bodies.push_back(data.bodies[i].id);
            commit(session_, dialog, data.revision, ed::WriteModule{m}, tr("Extract module"));
        });
        menuAction(menu, tr("Create empty module…"), &dialog, [&] {
            if (auto label = name(&dialog, tr("Empty module")))
                commit(session_, dialog, data.revision, ed::WriteModule{{{}, data.script, *label, {}}, true},
                       tr("Create empty module"));
        });
        menuAction(menu, tr("Rename / change bodies…"), &dialog, [&] {
            int i = list->currentRow();
            if (i < 0)
                return;
            auto m = data.modules[i];
            auto label = name(&dialog, tr("Module"), m.name);
            if (!label)
                return;
            QStringList labels;
            for (const auto &b : data.bodies)
                labels << q(b.name);
            auto chosen = choose(&dialog, tr("Select all module bodies"), labels);
            if (!chosen || chosen->empty())
                return;
            m.name = *label;
            m.bodies.clear();
            for (int n : *chosen)
                m.bodies.push_back(data.bodies[n].id);
            commit(session_, dialog, data.revision, ed::WriteModule{m}, tr("Edit module"));
        });
        menuAction(menu, tr("Remove module definition"), &dialog, [&] {
            int i = list->currentRow();
            if (i >= 0)
                commit(session_, dialog, data.revision, ed::RemoveModule{data.modules[i].id},
                       tr("Remove module definition"));
        });
        dialog.exec();
    });
}
void ModuleTools::editModule(ed::ModuleItem module) {
    auto *dialog = new QDialog(host_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Module: %1").arg(q(module.name)));
    dialog->resize(1050, 650);
    auto *view = session_->createViewSession(dialog);
    auto *layout = new QVBoxLayout(dialog);
    auto *bar = new QToolBar(dialog);
    layout->addWidget(bar);
    auto *editing = new ScriptEditingActions(view, dialog);
    auto *productivity = new ProductivityActions(view, dialog);
    for (auto *a : {editing->insertAction(), editing->structureAction(), editing->editAction(),
                    editing->timingAction()})
        bar->addAction(a);
    bar->addAction(view->undoAction());
    bar->addAction(view->redoAction());
    auto *more = new QMenu(dialog);
    for (auto *a : productivity->actions())
        more->addAction(a);
    bar->addAction(tr("Selection actions"),
                   [more, bar] { more->exec(bar->mapToGlobal(QPoint(0, bar->height()))); });
    auto *split = new QSplitter(dialog);
    auto *outline = new ScriptOutlineComponent(view, split);
    outline->bindModule(module.script, module.id);
    outline->setSemantic(true);
    split->addWidget(outline);
    auto *inspector = new InstructionInspector(view, split);
    split->addWidget(inspector);
    layout->addWidget(split);
    outline->editRequested = [editing] { editing->editAction()->trigger(); };
    bar->addAction(tr("Callers and dependencies…"), [this, dialog, module] {
        auto b = session_->backend();
        auto r = session_->revision();
        background(
            dialog, [=] { return b->module(r, module.id); },
            [this, dialog, r](auto result) {
                if (r != session_->revision() || !report(session_, result))
                    return;
                auto detail = result.value();
                QStringList labels;
                std::vector<ed::Subject> targets;
                for (auto id : detail.callers) {
                    labels << tr("Caller %1").arg(std::visit([](auto id) { return id.value; }, id));
                    targets.push_back(id);
                }
                for (auto id : detail.dependencies) {
                    labels << tr("Dependency %1").arg(std::visit([](auto id) { return id.value; }, id));
                    targets.push_back(id);
                }
                auto chosen = choose(dialog, tr("Navigate to related content"), labels, false);
                if (chosen && !chosen->empty())
                    session_->navigate(targets[chosen->front()]);
            });
    });
    connect(view, &EditorSessionPresenter::navigationRequested, outline,
            [outline](const ed::Subject &subject) { outline->reveal(subject); });
    dialog->show();
    if (!module.bodies.empty())
        view->navigate(module.bodies.front());
}
void ModuleTools::definitions() {
    inventory(session_, this, [this](ed::FeatureInventory data) {
        QStringList labels;
        for (const auto &def : data.definitions)
            labels << q(def.name) + tr(" — %1 uses").arg(def.uses.size());
        auto chosen = choose(host_, tr("Named bodies and conditions"), labels, false);
        if (!chosen || chosen->empty())
            return;
        auto def = data.definitions[chosen->front()];
        QStringList options{tr("Open definition"), tr("Rename definition"), tr("Inspect uses")};
        auto action = choose(host_, tr("Definition"), options, false);
        if (!action || action->empty())
            return;
        if (action->front() == 0)
            session_->navigate(def.subject);
        else if (action->front() == 1) {
            if (auto label = name(host_, tr("Definition alias"), def.name)) {
                auto r = session_->revision();
                auto b = session_->backend();
                background(
                    this, [=] { return b->organization(r, data.script); },
                    [=, this](auto result) {
                        if (r != session_->revision() || !report(session_, result))
                            return;
                        ed::ItemNote note{def.subject};
                        for (const auto &n : result.value().notes)
                            if (n.subject == def.subject)
                                note = n;
                        note.alias = *label;
                        session_->execute(tr("Name definition"), ed::WriteNote{note}, r);
                    });
            }
        } else {
            QStringList uses;
            for (auto use : def.uses)
                uses << tr("Item %1").arg(std::visit([](auto id) { return id.value; }, use));
            auto target = choose(host_, tr("Definition uses"), uses, false);
            if (target && !target->empty())
                session_->navigate(def.uses[target->front()]);
        }
    });
}
} // namespace salsa::qt
