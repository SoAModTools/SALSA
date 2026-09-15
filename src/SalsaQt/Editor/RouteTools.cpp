#include "Editor/StoryUi.h"
#include <QFileDialog>
namespace salsa::qt {
using namespace story_ui;
RouteTools::RouteTools(EditorSessionPresenter *s, QWidget *h) : QObject(h), session_(s), host_(h) {
    auto *r = new QAction(tr("Route endpoints…"), this);
    auto *o = new QAction(tr("Coordinated output…"), this);
    actions << r << o;
    connect(r, &QAction::triggered, this, [this] { open(); });
    connect(o, &QAction::triggered, this, [this] { output(); });
    for (auto *a : actions)
        availability(a, s);
}
void RouteTools::open() {
    catalog(session_, this, [this](ed::StoryCatalog data) {
        QDialog dialog(host_);
        dialog.setWindowTitle(tr("Route endpoints"));
        dialog.resize(780, 500);
        auto *l = new QVBoxLayout(&dialog);
        auto *list = new QListWidget(&dialog);
        for (auto &r : data.routes)
            list->addItem(tr("Connection %1: slot %2 → %3")
                              .arg(r.id.value)
                              .arg(r.source ? r.source->value : 0)
                              .arg(r.destination ? r.destination->value : 0));
        l->addWidget(list);
        auto *bar = new QToolBar(&dialog);
        l->addWidget(bar);
        bar->addAction(tr("Create slot…"), [&] {
            QStringList labels;
            for (auto &m : data.modules)
                labels << q(m.name);
            auto chosen = choose(&dialog, tr("Owning module"), labels, false);
            if (!chosen || chosen->empty())
                return;
            auto m = data.modules[chosen->front()];
            auto label = name(&dialog, tr("Slot name"));
            if (!label)
                return;
            auto role = choose(&dialog, tr("Slot role"), {tr("Entry"), tr("Exit")}, false);
            if (!role || role->empty())
                return;
            ed::SlotItem slot;
            slot.owner = m.id;
            slot.name = *label;
            slot.role = role->front() ? ed::SlotRole::Exit : ed::SlotRole::Entry;
            QStringList bodies;
            for (auto id : m.bodies)
                bodies << tr("Body %1").arg(id.value);
            auto body = choose(&dialog, tr("Body anchor"), bodies, false);
            if (!body || body->empty())
                return;
            slot.sequence = m.bodies[body->front()];
            if (slot.role == ed::SlotRole::Exit) {
                auto kind = choose(&dialog, tr("Exit boundary"),
                                   {tr("End of body"), tr("Existing target operand"),
                                    tr("Physical fallthrough"), tr("Terminal return / jump / call")},
                                   false);
                if (!kind || kind->empty())
                    return;
                if (kind->front() != 0) {
                    auto action = pickAction(session_, &dialog, m.script);
                    if (!action)
                        return;
                    slot.action = *action;
                    slot.sequence.reset();
                    slot.boundary = kind->front() == 1   ? ed::SlotBoundary::Operand
                                    : kind->front() == 2 ? ed::SlotBoundary::Fallthrough
                                                         : ed::SlotBoundary::Terminal;
                    if (kind->front() == 1) {
                        bool ok;
                        int index = QInputDialog::getInt(&dialog, tr("Operand"), tr("Parameter schema index"),
                                                         0, 0, 65535, 1, &ok);
                        if (!ok)
                            return;
                        int ordinal = QInputDialog::getInt(&dialog, tr("Operand"),
                                                           tr("Repeated group ordinal (-1 for fixed)"), -1,
                                                           -1, INT_MAX, 1, &ok);
                        if (!ok)
                            return;
                        slot.operand = spice::sct::SctParameterAddress{
                            static_cast<unsigned>(index),
                            ordinal < 0 ? std::nullopt : std::optional(static_cast<unsigned>(ordinal))};
                    }
                }
            }
            dialog.accept();
            session_->execute(tr("Create route slot"), ed::StoryEdit{ed::WriteSlot{slot}}, data.revision);
        });
        auto editRoute = [&](bool reconnect) {
            ed::RouteItem route;
            if (reconnect) {
                int i = list->currentRow();
                if (i < 0)
                    return;
                route = data.routes[i];
            }
            auto pick = [&](ed::SlotRole role) -> std::optional<core::SctPortId> {
                QStringList labels;
                std::vector<core::SctPortId> ids;
                for (auto &s : data.endpoints)
                    if (s.role == role) {
                        std::string owner = std::to_string(
                            number(std::visit([](auto id) -> ed::Subject { return id; }, s.owner)));
                        for (auto &m : data.modules)
                            if (s.owner == core::SctPortOwner(m.id))
                                owner = m.name;
                        labels << q(owner + ": " + s.name);
                        ids.push_back(s.id);
                    }
                auto chosen =
                    choose(&dialog,
                           role == ed::SlotRole::Exit ? tr("Source exit slot") : tr("Destination entry slot"),
                           labels, false);
                return chosen && !chosen->empty() ? std::optional(ids[chosen->front()]) : std::nullopt;
            };
            if (!route.source) {
                route.source = pick(ed::SlotRole::Exit);
                if (!route.source)
                    return;
            }
            auto outcome = choose(&dialog, tr("Route outcome"),
                                  {tr("Local transfer"), tr("Return to caller"), tr("Resume loop"),
                                   tr("Script transition"), tr("Scheduled call")},
                                  false);
            if (!outcome || outcome->empty())
                return;
            route.outcome = static_cast<ed::RouteOutcome>(outcome->front() + 1);
            if (route.outcome == ed::RouteOutcome::LocalTransfer ||
                route.outcome == ed::RouteOutcome::ScriptTransition ||
                route.outcome == ed::RouteOutcome::ScheduledCall) {
                route.destination = pick(ed::SlotRole::Entry);
                if (!route.destination)
                    return;
            } else
                route.destination.reset();
            ed::ConnectRoute command{route};
            if (route.outcome == ed::RouteOutcome::ScheduledCall) {
                bool ok;
                auto expr = QInputDialog::getText(&dialog, tr("Schedule"), tr("Schedule expression"),
                                                  QLineEdit::Normal, "0", &ok);
                if (!ok)
                    return;
                auto parsed = core::SctExpressionLanguage::parse(expr.toStdString());
                if (!parsed.succeeded())
                    return;
                command.schedule = *parsed.expression;
            }
            if (route.outcome == ed::RouteOutcome::ScriptTransition) {
                auto slot = std::ranges::find(data.endpoints, *route.destination, &ed::SlotItem::id);
                core::SctScriptId script;
                for (auto &m : data.modules)
                    if (slot->owner == core::SctPortOwner(m.id))
                        script = m.script;
                if (!script.valid())
                    return;
                auto b = session_->backend();
                auto r = data.revision;
                dialog.accept();
                background(
                    this, [=] { return b->features(r, script); },
                    [=, this](auto result) mutable {
                        if (r != session_->revision() || !report(session_, result))
                            return;
                        QStringList labels;
                        std::vector<core::SctEntrypointId> ids;
                        for (auto &e : result.value().entries)
                            if (e.kind == core::SctEntrypointKind::Arrival) {
                                labels << q(e.name);
                                ids.push_back(e.id);
                            }
                        auto chosen =
                            choose(host_, tr("Destination arrival handler with existing response operand"),
                                   labels, false);
                        if (!chosen || chosen->empty())
                            return;
                        command.route.arrival = ids[chosen->front()];
                        session_->execute(tr("Connect script transition"), ed::StoryEdit{command}, r);
                    });
                return;
            }
            dialog.accept();
            session_->execute(tr("Connect route"), ed::StoryEdit{command}, data.revision);
        };
        bar->addAction(tr("Connect…"), [&] { editRoute(false); });
        bar->addAction(tr("Reconnect…"), [&] { editRoute(true); });
        bar->addAction(tr("Detach binding"), [&] {
            int i = list->currentRow();
            if (i < 0)
                return;
            if (QMessageBox::question(&dialog, tr("Detach binding"),
                                      tr("Remove this connection record? Its ordinary executable "
                                         "instructions remain editable and unchanged.")) != QMessageBox::Yes)
                return;
            auto id = data.routes[i].id;
            dialog.accept();
            session_->execute(tr("Detach route binding"), ed::StoryEdit{ed::DetachRoute{id}}, data.revision);
        });
        bar->addAction(tr("Inspect implementation"), [&] {
            int i = list->currentRow();
            if (i >= 0 && data.routes[i].implementation)
                session_->navigate(*data.routes[i].implementation);
        });
        bar->addAction(tr("Interpretation…"), [&] {
            int i = list->currentRow();
            if (i >= 0)
                decision(
                    session_, &dialog, data.routes[i].id,
                    "Authored route connection receipt; inspect current instructions to review changes.");
        });
        bar->addAction(tr("Bind route-owned requirement…"), [&] {
            int i = list->currentRow();
            if (i < 0 || !data.routes[i].source)
                return;
            auto route = data.routes[i];
            auto slot = std::ranges::find(data.endpoints, *route.source, &ed::SlotItem::id);
            core::SctScriptId script;
            for (const auto &m : data.modules)
                if (slot->owner == core::SctPortOwner(m.id))
                    script = m.script;
            if (!script.valid())
                return;
            auto b = session_->backend();
            auto r = data.revision;
            dialog.accept();
            background(
                this, [=] { return b->interaction(r, script); },
                [=, this](auto result) {
                    if (r != session_->revision() || !report(session_, result))
                        return;
                    QStringList labels;
                    std::vector<core::program::ArmId> arms;
                    for (const auto &c : result.value().conditions)
                        if (c.route) {
                            labels << q(c.friendly);
                            arms.push_back(*c.route);
                        }
                    auto selected = choose(host_, tr("Existing route-owned condition"), labels, false);
                    if (selected && !selected->empty())
                        session_->execute(
                            tr("Bind route requirement"),
                            ed::StoryEdit{ed::BindRouteRequirement{route.id, arms[selected->front()]}}, r);
                });
        });
        bar->addAction(tr("Edit atomic requirement…"), [&] {
            int i = list->currentRow();
            if (i < 0 || !data.routes[i].requirement) {
                QMessageBox::information(&dialog, tr("Requirement"),
                                         tr("Use Script interactions to recover a supported shared dispatch "
                                            "and select a route-owned condition."));
                return;
            }
            auto arm = *data.routes[i].requirement;
            auto b = session_->backend();
            auto r = data.revision;
            dialog.accept();
            background(
                this, [=] { return b->inspect(r, arm); },
                [=, this](auto result) {
                    if (r != session_->revision() || !report(session_, result))
                        return;
                    auto script = result.value().script;
                    background(
                        this, [=] { return b->interaction(r, script); },
                        [=, this](auto view) {
                            if (r != session_->revision() || !report(session_, view))
                                return;
                            for (auto &c : view.value().conditions)
                                if (c.route == arm) {
                                    editCondition(session_, host_, c, r);
                                    return;
                                }
                        });
                });
        });
        bar->addAction(tr("Remove unused slot…"), [&] {
            QStringList labels;
            for (auto &s : data.endpoints)
                labels << q(s.name);
            auto chosen = choose(&dialog, tr("Remove slot definition"), labels, false);
            if (chosen && !chosen->empty()) {
                auto id = data.endpoints[chosen->front()].id;
                dialog.accept();
                session_->execute(tr("Remove slot"), ed::StoryEdit{ed::RemoveSlot{id}}, data.revision);
            }
        });
        dialog.exec();
    });
}
void RouteTools::output() {
    catalog(session_, this, [this](auto data) {
        QStringList labels;
        for (auto &s : data.scripts)
            labels << q(s.second);
        auto chosen = choose(host_, tr("Select output roots"), labels);
        if (!chosen || chosen->empty())
            return;
        std::vector<core::SctScriptId> roots;
        for (int i : *chosen)
            roots.push_back(data.scripts[i].first);
        auto b = session_->backend();
        auto r = data.revision;
        background(
            this, [=] { return b->reviewOutput(r, roots); },
            [this, b, r](auto result) {
                if (r != session_->revision() || !report(session_, result))
                    return;
                auto review = result.value();
                QDialog dialog(host_);
                dialog.setWindowTitle(tr("Review coordinated output"));
                dialog.resize(700, 600);
                auto *form = new QFormLayout(&dialog);
                auto *files = new QPlainTextEdit(&dialog);
                files->setReadOnly(true);
                QStringList lines;
                for (auto &item : review.items)
                    lines << q(item.filename + " — " + item.reason);
                for (auto &d : review.diagnostics)
                    lines << q(d.message);
                files->setPlainText(lines.join('\n'));
                form->addRow(files);
                auto *platform = new QComboBox(&dialog);
                platform->addItems({"GameCube", "Dreamcast"});
                form->addRow(tr("Output platform"), platform);
                auto *endian = new QComboBox(&dialog);
                endian->addItems({tr("Big endian"), tr("Little endian")});
                form->addRow(tr("Byte order"), endian);
                auto *encoding = new QComboBox(&dialog);
                for (auto &c : spice::sct::sctKnownTextConventions())
                    encoding->addItem(q(std::string(c.stableName)));
                form->addRow(tr("Text convention"), encoding);
                auto *compression = new QComboBox(&dialog);
                compression->addItems({"Raw SCT", "AKLZ"});
                form->addRow(tr("Wrapper"), compression);
                auto *buttons =
                    new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
                form->addRow(buttons);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                if (dialog.exec() != QDialog::Accepted)
                    return;
                auto parent =
                    QFileDialog::getExistingDirectory(host_, tr("Parent directory for new output set"));
                if (parent.isEmpty())
                    return;
                auto name = feature_ui::name(host_, tr("New output directory name"), "salsa-output");
                if (!name)
                    return;
                std::filesystem::path leaf(*name);
                if (leaf != leaf.filename() || leaf == "." || leaf == "..") {
                    QMessageBox::information(host_, tr("Output directory"),
                                             tr("Enter a directory name without a path."));
                    return;
                }
                spice::sct::SctDocumentExportOptions options{
                    platform->currentIndex() ? spice::sct::SctPlatform::Dreamcast
                                             : spice::sct::SctPlatform::GameCube,
                    spice::sct::kSctKnownTextConventions[encoding->currentIndex()].encoding};
                options.byteOrder = endian->currentIndex()
                                        ? spice::sct::SctDocumentOutputByteOrder::LittleEndian
                                        : spice::sct::SctDocumentOutputByteOrder::BigEndian;
                options.wrapper = compression->currentIndex() ? spice::sct::SctDocumentOutputWrapper::Aklz
                                                              : spice::sct::SctDocumentOutputWrapper::Raw;
                ed::OutputRequest request{review, options,
                                          std::filesystem::path(parent.toStdWString()) / leaf};
                session_->runOperation(tr("Publish coordinated output"), [b, request](auto stop,
                                                                                      const auto &progress) {
                    auto result = b->publishOutput(request, stop, progress);
                    if (!result)
                        return core::Result<std::string>::failure(result.diagnostics());
                    return core::Result<std::string>::success("Published " +
                                                              std::to_string(result.value().files.size()) +
                                                              " scripts to " + request.directory.string());
                });
            });
    });
}
} // namespace salsa::qt
