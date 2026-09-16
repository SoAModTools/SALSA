#include "Editor/StoryUi.h"
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <queue>
#include <set>
namespace salsa::qt {
using namespace story_ui;
namespace {
class StoryWindow final : public QDialog {
    EditorSessionPresenter *session;
    ed::StoryRequest request;
    ed::StoryView data;
    std::stop_source stop;
    std::uint64_t generation = 0;
    QListWidget *modules;
    QTableWidget *effects;
    QGraphicsScene *scene;
    QLabel *status;
    QCheckBox *bits, *all;
    QComboBox *scriptFilter;
    std::vector<ed::ModuleItem> listed;
    QAction *analyzeAction, *disableAction, *pathAction, *narrativeAction;
    QTimer *refresh;
    std::size_t shown = 0;
    std::vector<ed::StoryEffect> visible;
    std::optional<ed::ModuleItem> selected() {
        int i = modules->currentRow();
        return i >= 0 && i < int(listed.size()) ? std::optional(listed[i]) : std::nullopt;
    }
    void page() {
        auto end = std::min(shown + 200, visible.size());
        for (; shown < end; ++shown) {
            const auto &e = visible[shown];
            int row = effects->rowCount();
            effects->insertRow(row);
            effects->setItem(row, 0, new QTableWidgetItem(q(e.meaning)));
            effects->setItem(row, 1, new QTableWidgetItem(q(e.access)));
            effects->setItem(row, 2, new QTableWidgetItem(q(e.expression)));
        }
        if (!data.enabled) {
            status->setText(tr("Script editing mode. Choose Analyze and organize whole project to enable "
                               "story authoring. Existing organization is retained."));
            return;
        }
        status->setText(
            tr("Whole project: %1 scripts, %2 modules, %3 candidate relationships. %4 unorganized code "
               "sections. Showing %5 of %6 selected effects; graph displays up to 100 filtered modules.")
                .arg(data.scripts.size())
                .arg(data.modules.size())
                .arg(data.links.size())
                .arg(data.unorganizedSections)
                .arg(shown)
                .arg(visible.size()));
    }
    void select() {
        visible.clear();
        effects->setRowCount(0);
        shown = 0;
        auto m = selected();
        if (m)
            for (auto &e : data.effects)
                if (e.module == m->id)
                    visible.push_back(e);
        page();
    }
    void draw(std::optional<core::SctModuleId> selectedId = {}) {
        modules->clear();
        listed.clear();
        auto filter = scriptFilter->currentData().toULongLong();
        for (auto &m : data.modules)
            if (data.enabled && (!filter || m.script.value == filter)) {
                listed.push_back(m);
                modules->addItem(q(m.name));
            }
        scene->clear();
        std::map<core::SctModuleId, QPointF> points;
        std::vector<ed::ModuleItem> ordered = listed;
        std::map<core::SctModuleId, std::size_t> rank;
        for (std::size_t i = 0; i < data.narrative.size(); ++i)
            rank[data.narrative[i]] = i;
        std::stable_sort(ordered.begin(), ordered.end(), [&](auto &a, auto &b) {
            return (rank.contains(a.id) ? rank[a.id] : SIZE_MAX) <
                   (rank.contains(b.id) ? rank[b.id] : SIZE_MAX);
        });
        for (std::size_t i = 0; i < std::min<std::size_t>(100, ordered.size()); ++i) {
            QPointF pos((i % 4) * 255, (i / 4) * 115);
            points[ordered[i].id] = pos;
            auto *box = scene->addRect(QRectF(pos, QSizeF(225, 65)), QPen(palette().color(QPalette::Mid)),
                                       QBrush(palette().color(QPalette::AlternateBase)));
            box->setToolTip(
                tr("Narrative layout is user-authored. Lines show variable write/read candidates."));
            auto *text = scene->addText(q(ordered[i].name));
            text->setDefaultTextColor(palette().color(QPalette::Text));
            text->setTextWidth(215);
            text->setPos(pos + QPointF(5, 5));
        }
        std::set<std::pair<core::SctModuleId, core::SctModuleId>> seen;
        for (auto &link : data.links)
            if (auto target = std::get_if<core::SctModuleId>(&link.destination);
                target && points.contains(link.source) && points.contains(*target) &&
                seen.emplace(link.source, *target).second) {
                auto *line = scene->addLine(
                    QLineF(points[link.source] + QPointF(112, 65), points[*target] + QPointF(112, 0)),
                    QPen(palette().color(QPalette::Highlight)));
                line->setToolTip(q(link.explanation));
                line->setZValue(-1);
            }
        if (!listed.empty()) {
            auto found =
                selectedId ? std::ranges::find(listed, *selectedId, &ed::ModuleItem::id) : listed.end();
            modules->setCurrentRow(found == listed.end() ? 0 : static_cast<int>(found - listed.begin()));
        } else
            select();
    }
    void install(ed::StoryView next) {
        auto previous = selected();
        const auto filter = scriptFilter->currentData();
        data = std::move(next);
        {
            QSignalBlocker block(scriptFilter);
            scriptFilter->clear();
            scriptFilter->addItem(tr("All scripts (view filter)"), QVariant::fromValue<qulonglong>(0));
            for (const auto &[id, name] : data.scripts)
                scriptFilter->addItem(q(name), QVariant::fromValue<qulonglong>(id.value));
            auto index = scriptFilter->findData(filter);
            if (index >= 0)
                scriptFilter->setCurrentIndex(index);
        }
        pathAction->setEnabled(data.enabled);
        narrativeAction->setEnabled(data.enabled);
        disableAction->setEnabled(data.enabled);
        draw(previous ? std::optional(previous->id) : std::nullopt);
    }
    void review(ed::StoryView result) {
        if (result.revision != session->revision())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Whole-project organization review"));
        dialog.resize(720, 500);
        auto *layout = new QVBoxLayout(&dialog);
        auto *explanation = new QLabel(
            tr("Reviewed all %1 scripts. Existing modules are retained. Select unorganized code sections to "
               "add as modules; section boundaries are suggestions, not cutscene meanings. Unselected code "
               "remains editable and its readers remain available as links.")
                .arg(result.scripts.size()),
            &dialog);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        auto *choices = new QListWidget(&dialog);
        choices->setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (const auto &candidate : result.candidates) {
            auto script = std::ranges::find(result.scripts, candidate.script,
                                            &std::pair<core::SctScriptId, std::string>::first);
            choices->addItem((script == result.scripts.end() ? QString::number(candidate.script.value)
                                                             : q(script->second)) +
                             ": " + q(candidate.name));
        }
        layout->addWidget(choices);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Enable story authoring"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        ed::OrganizeStory edit;
        for (auto *item : choices->selectedItems())
            edit.modules.push_back(result.candidates.at(choices->row(item)));
        session->execute(tr("Enable and organize story authoring"), ed::StoryEdit{std::move(edit)},
                         result.revision);
    }
    void load(bool analyze = false) {
        if (!session->backend())
            return;
        auto b = session->backend();
        auto r = session->revision();
        request.unclassifiedBits = bits->isChecked();
        request.allEffects = all->isChecked();
        auto query = request;
        query.analyze = analyze;
        stop.request_stop();
        stop = std::stop_source{};
        auto token = stop.get_token();
        auto serial = ++generation;
        if (analyze) {
            refresh->stop();
            auto result = std::make_shared<std::optional<ed::StoryView>>();
            session->runOperation(
                tr("Analyze and organize whole project"),
                [b, r, query, result](auto token, const auto &progress) mutable {
                    query.progress = progress;
                    auto analyzed = b->story(r, query, token);
                    if (!analyzed)
                        return core::Result<std::string>::failure(analyzed.diagnostics());
                    *result = std::move(analyzed).takeValue();
                    return core::Result<std::string>::success(
                        "Whole-project story analysis ready for organization review.");
                },
                [self = QPointer(this), r, serial, result] {
                    if (self && self->generation == serial && self->session->revision() == r && *result)
                        self->review(std::move(**result));
                });
            return;
        }
        status->setText(tr("Restoring story organization and updating project relationships…"));
        background(
            this, [=] { return b->story(r, query, token); },
            [this, r, serial](auto result) {
                if (serial != generation || r != session->revision())
                    return;
                if (!report(session, result)) {
                    status->setText(tr("Story query failed; see activity log."));
                    return;
                }
                install(std::move(result).takeValue());
            });
    }
    void path() {
        const auto captured = data.revision;
        QStringList labels;
        for (auto &m : data.modules)
            labels << q(m.name);
        auto start = choose(this, tr("Path start"), labels, false);
        if (!start || start->empty())
            return;
        auto finish = choose(this, tr("Path end"), labels, false);
        if (!finish || finish->empty())
            return;
        auto from = data.modules[start->front()].id, to = data.modules[finish->front()].id;
        std::queue<core::SctModuleId> pending;
        std::map<core::SctModuleId, core::SctModuleId> previous;
        previous[from] = {};
        pending.push(from);
        while (!pending.empty() && !previous.contains(to)) {
            auto id = pending.front();
            pending.pop();
            for (auto &l : data.links)
                if (l.source == id)
                    if (auto dest = std::get_if<core::SctModuleId>(&l.destination);
                        dest && !previous.contains(*dest)) {
                        previous[*dest] = id;
                        pending.push(*dest);
                    }
        }
        if (!previous.contains(to)) {
            QMessageBox::information(
                this, tr("Path proposal"),
                tr("No writer/reader or control-flow path connects these modules in this project."));
            return;
        }
        std::vector<core::SctModuleId> order;
        for (auto id = to; id.valid(); id = previous.at(id))
            order.push_back(id);
        std::reverse(order.begin(), order.end());
        QStringList route;
        for (auto id : order)
            for (auto &m : data.modules)
                if (m.id == id)
                    route << q(m.name);
        if (QMessageBox::question(
                this, tr("Adopt narrative proposal"),
                route.join(" → ") +
                    tr("\n\nThis is a writer/reader or control-flow path, not a proven playthrough. Append "
                       "it to your narrative order?")) != QMessageBox::Yes)
            return;
        auto narrative = data.narrative;
        for (auto id : order)
            std::erase(narrative, id);
        narrative.insert(narrative.end(), order.begin(), order.end());
        session->execute(tr("Adopt narrative path"), ed::StoryEdit{ed::WriteNarrative{narrative}}, captured);
    }
    void narrative() {
        const auto captured = data.revision;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Narrative order (presentation only)"));
        auto *l = new QVBoxLayout(&dialog);
        auto *list = new QListWidget(&dialog);
        std::vector<core::SctModuleId> ids = data.narrative;
        for (auto &m : data.modules)
            if (std::ranges::find(ids, m.id) == ids.end())
                ids.push_back(m.id);
        for (auto id : ids) {
            std::string label = std::to_string(id.value);
            for (auto &m : data.modules)
                if (m.id == id)
                    label = m.name;
            list->addItem(q(label));
        }
        l->addWidget(list);
        auto *bar = new QToolBar(&dialog);
        l->addWidget(bar);
        for (int direction : {-1, 1})
            bar->addAction(direction < 0 ? tr("Up") : tr("Down"), [&, direction] {
                int i = list->currentRow(), j = i + direction;
                if (i < 0 || j < 0 || j >= list->count())
                    return;
                std::swap(ids[i], ids[j]);
                auto *item = list->takeItem(i);
                list->insertItem(j, item);
                list->setCurrentRow(j);
            });
        bar->addAction(tr("Remove from narrative"), [&] {
            int i = list->currentRow();
            if (i >= 0) {
                ids.erase(ids.begin() + i);
                delete list->takeItem(i);
            }
        });
        bar->addAction(tr("Apply"), [&] {
            dialog.accept();
            session->execute(tr("Set narrative order"), ed::StoryEdit{ed::WriteNarrative{ids}}, captured);
        });
        dialog.exec();
    }

  public:
    ~StoryWindow() override { stop.request_stop(); }
    StoryWindow(EditorSessionPresenter *s, QWidget *host) : QDialog(host), session(s) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Story workspace"));
        resize(1150, 730);
        auto *l = new QVBoxLayout(this);
        auto *bar = new QToolBar(this);
        l->addWidget(bar);
        bits = new QCheckBox(tr("Include unclassified bits"), this);
        all = new QCheckBox(tr("Inspect all variable effects"), this);
        bar->addWidget(bits);
        bar->addWidget(all);
        bits->setChecked(true);
        analyzeAction = bar->addAction(tr("Analyze and organize whole project…"), [this] { load(true); });
        availability(analyzeAction, session);
        disableAction = bar->addAction(tr("Use script editing only"), [this] {
            session->execute(tr("Disable story authoring (retain organization)"),
                             ed::StoryEdit{ed::OrganizeStory{{}, false}});
        });
        pathAction = bar->addAction(tr("Propose path…"), [this] { path(); });
        narrativeAction = bar->addAction(tr("Narrative order…"), [this] { narrative(); });
        pathAction->setEnabled(false);
        narrativeAction->setEnabled(false);
        disableAction->setEnabled(false);
        bar->addAction(tr("Module alias…"), [this] {
            if (auto m = selected())
                alias(session, this, m->script, m->id);
        });
        bar->addAction(tr("Explain module…"), [this] {
            if (auto m = selected())
                decision(session, this, m->id,
                         "Module membership and effects are observations, not proof of narrative meaning.");
        });
        scriptFilter = new QComboBox(this);
        scriptFilter->addItem(tr("All scripts (view filter)"), QVariant::fromValue<qulonglong>(0));
        l->addWidget(scriptFilter);
        connect(scriptFilter, &QComboBox::currentIndexChanged, this, [this] { draw(); });
        auto *split = new QSplitter(this);
        modules = new QListWidget(split);
        auto *view = new QGraphicsView(split);
        scene = new QGraphicsScene(view);
        view->setScene(scene);
        view->setBackgroundBrush(palette().color(QPalette::Base));
        split->addWidget(modules);
        split->addWidget(view);
        split->setStretchFactor(1, 1);
        l->addWidget(split, 2);
        effects = new QTableWidget(0, 3, this);
        effects->setHorizontalHeaderLabels({tr("State / meaning"), tr("Access"), tr("Expression")});
        effects->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        effects->setEditTriggers(QAbstractItemView::NoEditTriggers);
        l->addWidget(effects, 1);
        status = new QLabel(this);
        l->addWidget(status);
        auto *details = new QToolBar(this);
        l->addWidget(details);
        details->addAction(tr("More effects"), [this] { page(); });
        details->addAction(tr("Readers / downstream interactions…"), [this] {
            auto m = selected();
            if (!m)
                return;
            QStringList labels;
            std::vector<core::SctActionId> targets;
            for (auto &link : data.links)
                if (link.source == m->id) {
                    labels << q(link.explanation) + tr(" — reader %1").arg(link.reader.value);
                    targets.push_back(link.reader);
                }
            auto chosen = choose(this, tr("Navigate to reader"), labels, false);
            if (chosen && !chosen->empty())
                session->navigate(targets[chosen->front()]);
        });
        details->addAction(tr("Entry / exit details…"), [this] {
            auto m = selected();
            if (!m)
                return;
            auto b = session->backend();
            auto r = session->revision();
            background(
                this, [=] { return b->routes(r, m->id); },
                [this, r](auto result) {
                    if (r != session->revision() || !report(session, result))
                        return;
                    QStringList labels;
                    for (auto &row : result.value().rows)
                        labels << q(row.second);
                    auto chosen =
                        choose(this, tr("Observed routes and all underlying effects"), labels, false);
                    if (chosen && !chosen->empty())
                        session->navigate(result.value().rows[chosen->front()].first);
                });
        });
        connect(modules, &QListWidget::currentRowChanged, this, [this] { select(); });
        connect(modules, &QListWidget::itemDoubleClicked, this, [this] {
            if (auto m = selected(); m && !m->bodies.empty())
                session->navigate(m->bodies.front());
        });
        connect(effects, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (row >= 0 && row < int(visible.size()))
                session->navigate(visible[row].action);
        });
        refresh = new QTimer(this);
        refresh->setSingleShot(true);
        refresh->setInterval(80);
        connect(refresh, &QTimer::timeout, this, [this] { load(); });
        connect(session, &EditorSessionPresenter::changed, this, [this] { refresh->start(); });
        connect(session, &EditorSessionPresenter::sessionChanged, this, &QDialog::close);
        connect(bits, &QCheckBox::toggled, this, [this] { load(); });
        connect(all, &QCheckBox::toggled, this, [this] { load(); });
        load();
    }
};
} // namespace
StoryTools::StoryTools(EditorSessionPresenter *s, QWidget *h) : QObject(h), session_(s), host_(h) {
    auto *story = new QAction(tr("Story workspace…"), this);
    auto *meaning = new QAction(tr("Variable meanings…"), this);
    actions << story << meaning;
    connect(story, &QAction::triggered, this, [this] { open(); });
    connect(meaning, &QAction::triggered, this, [this] { meanings(); });
    for (auto *a : actions)
        availability(a, s);
}
void StoryTools::open() {
    if (session_->backend() && !session_->busy())
        (new StoryWindow(session_, host_))->show();
}
void StoryTools::meanings() {
    catalog(session_, this, [this](auto data) {
        QDialog dialog(host_);
        dialog.setWindowTitle(tr("Variable and value meanings"));
        auto *form = new QFormLayout(&dialog);
        auto *existing = new QComboBox(&dialog);
        existing->addItem(tr("New meaning"));
        for (auto &m : data.meanings)
            existing->addItem(q(m.name));
        form->addRow(tr("Definition"), existing);
        auto *scope = new QComboBox(&dialog);
        scope->addItem(tr("Project-wide"));
        for (auto &s : data.scripts)
            scope->addItem(q(s.second));
        form->addRow(tr("Scope"), scope);
        auto *bank = new QComboBox(&dialog);
        bank->addItems({"BitVar", "ByteVar", "IntVar", "FloatVar"});
        form->addRow(tr("Variable"), bank);
        auto *index = new QSpinBox(&dialog);
        index->setRange(0, INT_MAX);
        form->addRow(tr("Index"), index);
        auto *value = new QLineEdit(&dialog);
        value->setPlaceholderText(tr("Blank for the variable; integer for one value"));
        form->addRow(tr("Value"), value);
        auto *label = new QLineEdit(&dialog);
        form->addRow(tr("Meaning"), label);
        auto *story = new QCheckBox(tr("Show in story state"), &dialog);
        form->addRow(story);
        connect(existing, &QComboBox::currentIndexChanged, &dialog, [&](int i) {
            if (i <= 0)
                return;
            auto &m = data.meanings[i - 1];
            scope->setCurrentIndex(0);
            for (int n = 0; n < int(data.scripts.size()); ++n)
                if (data.scripts[n].first == m.script)
                    scope->setCurrentIndex(n + 1);
            bank->setCurrentIndex(int(m.variable.kind));
            index->setValue(m.variable.index);
            value->setText(m.value ? QString::number(*m.value) : QString{});
            label->setText(q(m.name));
            story->setChecked(m.storyState);
        });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        auto *remove = buttons->addButton(tr("Delete definition"), QDialogButtonBox::DestructiveRole);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(remove, &QPushButton::clicked, &dialog, [&] {
            if (existing->currentIndex() > 0) {
                auto id = data.meanings[existing->currentIndex() - 1].id;
                dialog.accept();
                session_->execute(tr("Delete meaning"), ed::StoryEdit{ed::RemoveMeaning{id}}, data.revision);
            }
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            if (label->text().trimmed().isEmpty())
                return;
            ed::Meaning m;
            if (existing->currentIndex() > 0)
                m = data.meanings[existing->currentIndex() - 1];
            m.script =
                scope->currentIndex() ? data.scripts[scope->currentIndex() - 1].first : core::SctScriptId{};
            m.variable = {static_cast<core::SctVariableKind>(bank->currentIndex()),
                          static_cast<unsigned>(index->value())};
            m.value.reset();
            if (!value->text().trimmed().isEmpty()) {
                bool ok;
                auto n = value->text().toLongLong(&ok);
                if (!ok)
                    return;
                m.value = n;
            }
            m.name = label->text().toStdString();
            m.storyState = story->isChecked();
            m.evidence.push_back({core::SctEvidenceKind::UserAssertion,
                                  "User-defined variable meaning and story classification"});
            dialog.accept();
            session_->execute(tr("Define variable meaning"), ed::StoryEdit{ed::WriteMeaning{m}},
                              data.revision);
        });
        dialog.exec();
    });
}
} // namespace salsa::qt
