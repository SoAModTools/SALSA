#include "Editor/FeatureUi.h"
#include "Editor/ScriptEditingActions.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
namespace salsa::qt {
using namespace feature_ui;
EntryResourceTools::EntryResourceTools(EditorSessionPresenter *s, QWidget *h)
    : QObject(h), session_(s), host_(h) {
    auto *e = new QAction(tr("Entrypoints and activation…"), this);
    actions << e;
    connect(e, &QAction::triggered, this, [this] { entries(); });
    auto *r = new QAction(tr("Optional MLD resources…"), this);
    actions << r;
    connect(r, &QAction::triggered, this, [this] { resources(); });
    for (auto *a : actions)
        availability(a, session_);
}
void EntryResourceTools::entries() {
    inventory(session_, this, [this](ed::FeatureInventory data) {
        QDialog dialog(host_);
        dialog.setWindowTitle(tr("Entrypoints and activation"));
        dialog.resize(710, 470);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(tr("init runs once; loop remains the engine root. Optional routes "
                                        "bypass their entire selected block when disabled."),
                                     &dialog));
        auto *list = new QListWidget(&dialog);
        for (const auto &e : data.entries) {
            auto *item = new QListWidgetItem(q(e.name) + (e.required() ? tr(" — required")
                                                          : e.enabled  ? tr(" — enabled")
                                                                       : tr(" — disabled")),
                                             list);
            QString detail = tr("Body %1").arg(e.body.value);
            if (auto *named = std::get_if<core::SctNamedActivation>(&e.activation))
                detail += tr("\nExternal name: %1; original entry %2")
                              .arg(q(named->externalName))
                              .arg(named->body.value);
            if (auto *local = std::get_if<core::SctLocalActivation>(&e.activation))
                detail += tr("\nIncoming %1; block start %2; continuation %3")
                              .arg(local->source.value)
                              .arg(local->start.value)
                              .arg(local->continuation.value);
            if (auto *unresolved = std::get_if<core::SctUnresolvedBinding>(&e.activation))
                detail += "\n" + q(unresolved->explanation);
            if (e.response)
                detail += tr("\nResponse port %1").arg(e.response->value);
            if (e.provider)
                if (auto *provider = std::get_if<smores::script::Entry>(&*e.provider))
                    detail += tr("\nEvidence: %1 entry %2; tblId %3")
                                  .arg(q(provider->resource.asset))
                                  .arg(provider->tableIndex)
                                  .arg(provider->tableId);
            item->setToolTip(detail);
        }
        layout->addWidget(list);
        auto current = [&]() -> std::optional<ed::EntryItem> {
            int i = list->currentRow();
            return i >= 0 ? std::optional(data.entries[i]) : std::nullopt;
        };
        auto *menu = new QMenu(&dialog);
        auto *bar = new QToolBar(&dialog);
        bar->addAction(tr("Entry actions"), [=] { menu->exec(bar->mapToGlobal(QPoint(0, bar->height()))); });
        layout->addWidget(bar);
        menuAction(menu, tr("Open body"), &dialog, [&] {
            if (auto e = current())
                session_->navigate(e->body);
        });
        auto *toggle = menuAction(menu, tr("Enable / disable"), &dialog, [&] {
            if (auto e = current(); e && !e->required()) {
                e->enabled = !e->enabled;
                commit(session_, dialog, data.revision, ed::WriteEntry{*e}, tr("Change entry activation"));
            }
        });
        toggle->setEnabled(false);
        connect(list, &QListWidget::currentRowChanged, &dialog, [&] {
            auto e = current();
            toggle->setEnabled(e && !e->required());
        });
        menuAction(menu, tr("Declare named or M handler / arrival / signal…"), &dialog, [&] {
            QStringList labels;
            for (const auto &b : data.bodies)
                labels << q(b.name);
            auto chosen = choose(&dialog, tr("Existing activation body"), labels, false);
            if (!chosen || chosen->empty())
                return;
            auto body = data.bodies[chosen->front()];
            auto label = name(&dialog, tr("Entrypoint name"), body.name);
            if (!label)
                return;
            auto kind = choose(&dialog, tr("Entry method"),
                               {tr("Named section"), tr("Direct M handler"),
                                tr("Arrival (previous location / battle return)"), tr("Signal consumer")},
                               false);
            if (!kind || kind->empty())
                return;
            ed::EntryItem e;
            e.script = data.script;
            e.name = *label;
            e.body = body.id;
            e.kind = kind->front() == 0   ? core::SctEntrypointKind::NamedSection
                     : kind->front() == 1 ? core::SctEntrypointKind::DirectHandler
                     : kind->front() == 2 ? core::SctEntrypointKind::Arrival
                                          : core::SctEntrypointKind::SignalConsumer;
            if (kind->front() < 2) {
                auto first = feature_ui::action(session_, &dialog,
                                                tr("Original body entry for the external section name"));
                if (!first)
                    return;
                e.activation = core::SctNamedActivation{body.name, *first};
            }
            commit(session_, dialog, data.revision, ed::WriteEntry{e}, tr("Declare entrypoint"));
        });
        menuAction(menu, tr("Rename / remove optional entry…"), &dialog, [&] {
            auto e = current();
            if (!e || e->required())
                return;
            auto choice =
                choose(&dialog, tr("Optional entry"), {tr("Rename"), tr("Remove definition")}, false);
            if (!choice || choice->empty())
                return;
            if (choice->front() == 1) {
                commit(session_, dialog, data.revision, ed::RemoveEntry{e->id},
                       tr("Remove entry definition"));
                return;
            }
            if (auto label = name(&dialog, tr("Entry name"), e->name)) {
                e->name = *label;
                commit(session_, dialog, data.revision, ed::WriteEntry{*e}, tr("Rename entry"));
            }
        });
        menuAction(menu, tr("Repair missing init / loop binding…"), &dialog, [&] {
            QStringList labels;
            std::vector<core::program::SectionId> ids;
            for (const auto &b : data.bodies)
                if (b.section) {
                    labels << q(b.name);
                    ids.push_back(*b.section);
                }
            auto selected = choose(&dialog, tr("Existing root body"), labels, false);
            if (!selected || selected->empty())
                return;
            auto root = choose(&dialog, tr("Required root"), {"init", "loop"}, false);
            if (root && !root->empty())
                commit(session_, dialog, data.revision,
                       ed::RepairRoot{ids[selected->front()], root->front() ? "loop" : "init"},
                       tr("Repair root binding"));
        });
        menuAction(menu, tr("Suggested whole-block boundaries…"), &dialog, [&] {
            auto entry = current();
            if (!entry || entry->required())
                return;
            auto b = session_->backend();
            background(
                &dialog, [=] { return b->activationSuggestions(data.revision, data.script); },
                [&, entry](auto result) mutable {
                    if (data.revision != session_->revision() || !report(session_, result))
                        return;
                    QStringList labels;
                    for (const auto &a : result.value())
                        labels << tr("Incoming %1 → block %2 → continuation %3 (includes preparation, "
                                     "response and consumption)")
                                      .arg(a.source.value)
                                      .arg(a.start.value)
                                      .arg(a.continuation.value);
                    auto chosen = choose(&dialog, tr("Review structured block suggestions"), labels, false);
                    if (!chosen || chosen->empty())
                        return;
                    entry->activation = result.value()[chosen->front()];
                    commit(session_, dialog, data.revision, ed::WriteEntry{*entry},
                           tr("Select activation block"));
                });
        });
        menuAction(menu, tr("Set manual boundary / unresolved draft…"), &dialog, [&] {
            auto e = current();
            if (!e || e->required())
                return;
            auto choice = choose(&dialog, tr("Boundary type"),
                                 {tr("Local incoming edge and whole block"), tr("Named section wrapper"),
                                  tr("Unresolved draft")},
                                 false);
            if (!choice || choice->empty())
                return;
            if (choice->front() == 2)
                e->activation = core::SctUnresolvedBinding{
                    "User boundary review", "Activation boundary not selected", {}};
            else if (choice->front() == 1) {
                auto label = name(&dialog, tr("External section name"), e->name);
                if (!label)
                    return;
                auto first = feature_ui::action(session_, &dialog, tr("Original body entry"));
                if (!first)
                    return;
                e->activation = core::SctNamedActivation{*label, *first};
            } else {
                auto incoming =
                    choose(&dialog, tr("Incoming edge"),
                           {tr("Physical fallthrough"), tr("Target operand / switch case")}, false);
                if (!incoming || incoming->empty())
                    return;
                core::SctLocalActivation boundary;
                if (incoming->front()) {
                    auto selected = site(session_, &dialog, tr("Incoming edge source and operand"));
                    if (!selected)
                        return;
                    boundary.source = selected->action;
                    boundary.operand = selected->operand;
                } else {
                    auto source =
                        feature_ui::action(session_, &dialog, tr("Incoming physical fallthrough source"));
                    if (!source)
                        return;
                    boundary.source = *source;
                }
                auto start =
                    feature_ui::action(session_, &dialog, tr("Whole block start, including preparation"));
                if (!start)
                    return;
                auto end = feature_ui::action(session_, &dialog,
                                              tr("Bypass continuation after response and consumption"));
                if (!end)
                    return;
                boundary.start = *start;
                boundary.continuation = *end;
                e->activation = boundary;
            }
            commit(session_, dialog, data.revision, ed::WriteEntry{*e}, tr("Edit activation boundary"));
        });
        menuAction(menu, tr("Connect existing response operand to module…"), &dialog, [&] {
            auto e = current();
            if (!e)
                return;
            QStringList labels;
            std::vector<core::SctSequenceId> bodies;
            for (const auto &m : data.modules)
                for (auto body : m.bodies) {
                    labels << q(m.name) + tr(" / body %1").arg(body.value);
                    bodies.push_back(body);
                }
            auto chosen = choose(&dialog, tr("Response module body"), labels, false);
            if (!chosen || chosen->empty())
                return;
            auto target =
                site(session_, &dialog, tr("Existing call or branch operand that invokes the response"));
            if (target)
                commit(session_, dialog, data.revision, ed::WriteEntry{*e, *target, bodies[chosen->front()]},
                       tr("Connect module response"));
        });
        menuAction(menu, tr("Edit existing arrival comparison…"), &dialog, [&] {
            QStringList labels;
            std::vector<core::program::ConditionId> ids;
            for (const auto &def : data.definitions)
                if (auto *id = std::get_if<core::program::ConditionId>(&def.subject)) {
                    labels << q(def.name);
                    ids.push_back(*id);
                }
            auto chosen = choose(&dialog, tr("Existing previous-location condition"), labels, false);
            if (!chosen || chosen->empty())
                return;
            bool ok = false;
            auto source = QInputDialog::getText(&dialog, tr("Arrival source"),
                                                tr("Script stem (me033b) or explicit encoded value"),
                                                QLineEdit::Normal, {}, &ok)
                              .trimmed();
            if (!ok || source.isEmpty())
                return;
            ed::SetArrival edit;
            edit.condition = ids[chosen->front()];
            auto numeric = source.toUInt(&ok);
            if (ok)
                edit.encoded = numeric;
            else
                edit.stem = source.toStdString();
            commit(session_, dialog, data.revision, edit, tr("Edit arrival comparison"));
        });
        menuAction(menu, tr("Select / reselect MLD entry evidence…"), &dialog, [&] {
            auto entry = current();
            if (!entry)
                return;
            auto asset = name(&dialog, tr("Dataset-relative MLD path"));
            if (!asset)
                return;
            auto b = session_->backend();
            background(
                &dialog,
                [=] {
                    return b->inspectResource(data.revision, *asset,
                                              smores::script::ResourceRole::Unclassified);
                },
                [&, entry](auto result) {
                    if (data.revision != session_->revision() || !report(session_, result))
                        return;
                    QStringList labels;
                    for (const auto &e : result.value().entries)
                        labels << tr("Entry %1 — tblId %2 — %3")
                                      .arg(e.tableIndex)
                                      .arg(e.tableId)
                                      .arg(q(e.function));
                    auto chosen = choose(&dialog, tr("Select exact MLD entry index"), labels, false);
                    if (chosen && !chosen->empty())
                        commit(session_, dialog, data.revision,
                               ed::SelectResourceEntry{entry->id, result.value(),
                                                       result.value().entries[chosen->front()].tableIndex},
                               tr("Select resource evidence"));
                });
        });
        dialog.exec();
    });
}
void EntryResourceTools::resources() {
    inventory(session_, this, [this](ed::FeatureInventory data) {
        QDialog dialog(host_);
        dialog.setWindowTitle(tr("Optional MLD context"));
        dialog.resize(620, 400);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(
            tr("Context is advisory. Missing or changed resources do not block editing, saving or export."),
            &dialog));
        auto *list = new QListWidget(&dialog);
        for (const auto &r : data.resources)
            list->addItem(q(r.resource.asset) +
                          tr(" — retained revision %1").arg(q(r.resource.revision.substr(0, 12))));
        layout->addWidget(list);
        auto *bar = new QToolBar(&dialog);
        layout->addWidget(bar);
        bar->addAction(tr("Remove association"), [&] {
            int i = list->currentRow();
            if (i < 0)
                return;
            auto rows = data.resources;
            rows.erase(rows.begin() + i);
            commit(session_, dialog, data.revision, ed::SetResources{data.script, rows},
                   tr("Remove optional resource"));
        });
        bar->addAction(tr("Inspect / associate…"), [&] {
            auto asset = name(&dialog, tr("Dataset-relative MLD path"));
            if (!asset)
                return;
            auto choice = choose(&dialog, tr("Resource role"),
                                 {tr("Base"), tr("Auxiliary"), tr("Streamed tile")}, false);
            if (!choice || choice->empty())
                return;
            auto role = choice->front() == 0   ? smores::script::ResourceRole::Base
                        : choice->front() == 1 ? smores::script::ResourceRole::Auxiliary
                                               : smores::script::ResourceRole::StreamedTile;
            auto b = session_->backend();
            background(
                &dialog, [=] { return b->inspectResource(data.revision, *asset, role); },
                [&, role](auto result) {
                    if (data.revision != session_->revision() || !report(session_, result))
                        return;
                    auto rows = data.resources;
                    auto found = std::ranges::find_if(rows, [&](const auto &x) {
                        return x.resource.asset == result.value().identity.asset;
                    });
                    core::SctScriptContext::ResourceAssociation association{result.value().identity, role};
                    if (found == rows.end())
                        rows.push_back(association);
                    else
                        *found = association;
                    commit(session_, dialog, data.revision, ed::SetResources{data.script, rows},
                           tr("Associate optional MLD"));
                });
        });
        dialog.exec();
    });
}
} // namespace salsa::qt
