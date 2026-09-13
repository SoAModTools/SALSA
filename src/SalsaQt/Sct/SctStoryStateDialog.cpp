#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SalsaCore/Authoring/SctStoryPaths.h"
#include "SctInterpretationDialog.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <map>
#include <set>
namespace salsa::qt {
namespace {
using namespace core;
namespace sct = spice::sct;
// Graphics items do not inherit widget palette roles. Keep their fills and text
// together so the graph remains readable regardless of the system palette.
namespace storyColors {
const QColor background("#20252b");
const QColor cutscene("#243b53");
const QColor variable("#493d26");
const QColor reader("#263f35");
const QColor border("#6b8299");
const QColor selected("#60a5fa");
const QColor text("#f1f5f9");
const QColor connection("#8b9bad");
const QColor caption("#cbd5e1");
} // namespace storyColors
QString q(const std::string &s) {
    return QString::fromStdString(s);
}
QString operation(SctStateAccessKind kind) {
    switch (kind) {
    case SctStateAccessKind::Set:
        return "Set to 1";
    case SctStateAccessKind::Clear:
        return "Clear to 0";
    case SctStateAccessKind::Toggle:
        return "Toggle";
    case SctStateAccessKind::Assign:
        return "Assign";
    default:
        return "Read";
    }
}
class StoryNode final : public QGraphicsRectItem {
  public:
    std::function<void()> moved;

  protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        auto result = QGraphicsRectItem::itemChange(change, value);
        if (change == ItemSelectedHasChanged)
            setPen(QPen(value.toBool() ? storyColors::selected : storyColors::border, 2.0));
        if (change == ItemPositionHasChanged && moved)
            moved();
        return result;
    }
};
class StoryDialog final : public QDialog {
  public:
    StoryDialog(SctDocumentController &c, AssetLocator loc, std::function<void(AssetLocator, SctNavigationTarget)> nav,
                QWidget *parent)
        : QDialog(parent), controller(c), locator(std::move(loc)), navigate(std::move(nav)) {
        setWindowTitle(tr("Story state and cutscene effects"));
        resize(1400, 900);
        auto *layout = new QVBoxLayout(this);
        auto *bar = new QHBoxLayout;
        layout->addLayout(bar);
        bar->addWidget(new QLabel(tr("Cutscene / module:")));
        roots = new QComboBox(this);
        bar->addWidget(roots, 1);
        button(bar, "Executable routes / export set", [this] { showSctRouteEditor(controller,this); });
        button(bar, "Refresh", [this] { refresh(); });
        button(bar, "Save layout", [this] { saveLayout(); });
        button(bar, "Suggest all paths", [this] { suggest(false); });
        button(bar, "Suggest from selected", [this] { suggest(true); });
        auto *split = new QSplitter(this);
        layout->addWidget(split, 1);
        auto *left = new QWidget(split);
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->addWidget(new QLabel(tr("Whole bodies and referenced helpers. Arrows describe state writes and "
                                            "reads; they do not create execution routes.")));
        graph = new QGraphicsScene(this);
        graph->setBackgroundBrush(storyColors::background);
        view = new QGraphicsView(graph, left);
        view->setDragMode(QGraphicsView::ScrollHandDrag);
        leftLayout->addWidget(view, 3);
        tabs = new QTabWidget(left);
        leftLayout->addWidget(tabs, 2);
        writes = new QListWidget(tabs);
        tabs->addTab(writes, tr("Every write"));
        readers = new QListWidget(tabs);
        tabs->addTab(readers, tr("Readers"));
        instructions = new QTextEdit(tabs);
        instructions->setReadOnly(true);
        tabs->addTab(instructions, tr("Generated instructions"));
        auto *proposals = new QWidget(tabs);
        auto *pl = new QVBoxLayout(proposals);
        pathChoices = new QComboBox(proposals);
        pl->addWidget(pathChoices);
        proposedSteps = new QListWidget(proposals);
        proposedSteps->setSelectionMode(QAbstractItemView::ExtendedSelection);
        pl->addWidget(proposedSteps);
        auto *pb = new QHBoxLayout;
        pl->addLayout(pb);
        button(pb, "Adopt selected steps", [this] { adopt(false); });
        button(pb, "Reject selected suggestion", [this] { adopt(true); });
        proposalTab = proposals;
        tabs->addTab(proposals, tr("Suggested paths"));
        connect(pathChoices, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { showPath(); });
        connect(proposedSteps, &QListWidget::currentRowChanged, this, [this](int n) {
            if (!proposal || n < 0 || n >= static_cast<int>(proposalNodes.size()))
                return;
            auto id = proposalNodes[n];
            QString text;
            for (const auto &edge : proposal->graph.at("edges"))
                if (edge.at("from") == id || edge.at("to") == id)
                    text += q(edge.dump(2)) + "\n";
            why->setPlainText(text);
            if (proposal->targets.contains(id))
                select(proposal->targets.at(id), false);
            why->append(text);
        });
        auto *right = new QWidget(split);
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->addWidget(new QLabel(tr("Storyline (user-defined order)")));
        stories = new QComboBox(right);
        rightLayout->addWidget(stories);
        steps = new QListWidget(right);
        rightLayout->addWidget(steps, 2);
        auto *sb = new QHBoxLayout;
        rightLayout->addLayout(sb);
        button(sb, "New", [this] { newStory(); });
        button(sb, "Overview", [this] { drawStory(); });
        button(sb, "Name step", [this] { nameStep(); });
        button(sb, "Add cutscene", [this] { editStory(0); });
        button(sb, "Remove", [this] { editStory(1); });
        button(sb, "Up", [this] { editStory(-1); });
        button(sb, "Down", [this] { editStory(2); });
        rightLayout->addWidget(new QLabel(tr("Selected item: meaning and attached explanations")));
        why = new QTextEdit(right);
        why->setReadOnly(true);
        rightLayout->addWidget(why, 3);
        auto *actions = new QHBoxLayout;
        layout->addLayout(actions);
        button(actions, "Name selected item", [this] { name(); });
        button(actions, "Inspect generated instructions", [this] { generated(); });
        button(actions, "Open in semantic editor", [this] { open(); });
        button(actions, "Undo", [this] {
            (void)controller.undo(locator);
            refresh();
        });
        button(actions, "Redo", [this] {
            (void)controller.redo(locator);
            refresh();
        });
        auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
        actions->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(roots, &QComboBox::currentIndexChanged, this, [this] { draw(); });
        connect(stories, &QComboBox::currentIndexChanged, this, [this] { story(); });
        connect(graph, &QGraphicsScene::selectionChanged, this, [this] {
            const auto items = graph->selectedItems();
            if (items.empty())
                return;
            const auto n = items.front()->data(0).toInt();
            if (n >= 0 && n < static_cast<int>(targets.size()))
                select(targets[n]);
        });
        connect(writes, &QListWidget::currentRowChanged, this, [this](int n) {
            if (n >= 0 && n < static_cast<int>(writeTargets.size()))
                select(writeTargets[n]);
        });
        connect(readers, &QListWidget::currentRowChanged, this, [this](int n) {
            if (n >= 0 && n < static_cast<int>(readerTargets.size()))
                select(readerTargets[n], false);
        });
        connect(steps, &QListWidget::currentRowChanged, this, [this](int n) {
            const auto *s = currentStory();
            if (s && n >= 0 && n < static_cast<int>(s->steps.size()))
                select(s->steps[n].target);
        });
        refresh();
    }

    ~StoryDialog() override {
        graph->blockSignals(true);
        lines.clear();
        for (auto *item : graph->items())
            if (auto *node = dynamic_cast<StoryNode *>(item))
                node->moved = {};
        graph->clear();
    }

  private:
    void button(QHBoxLayout *row, const QString &label, std::function<void()> fn) {
        auto *b = new QPushButton(label, this);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, std::move(fn));
    }
    void fail(const std::vector<Diagnostic> &ds) {
        QString text;
        for (const auto &d : ds)
            text += q(d.message) + '\n';
        QMessageBox::warning(this, tr("Story state"), text);
    }
    void apply(std::string label, SctAuthoringSession::Command fn) {
        auto r = controller.executeAuthoringCommand(state->project.revision, std::move(label), fn);
        if (!r)
            fail(r.diagnostics());
        refresh();
    }
    QString label(const SctMeaningTarget &t) const {
        auto name = SctInterpretation::label(*state, t, "");
        if (name.empty() && t.variable) {
            auto global = t;
            global.script = {};
            name = SctInterpretation::label(*state, global, "");
        }
        if (name.empty())
            name = SctInterpretation::describe(*state, t);
        if (t.variable && t.variable->kind == SctVariableKind::Bit && !t.value)
            for (int value = 0; value <= 1; ++value) {
                auto named = t;
                named.value = value;
                auto alias = SctInterpretation::label(*state, named, "");
                if (alias.empty()) {
                    named.script = {};
                    alias = SctInterpretation::label(*state, named, "");
                }
                if (!alias.empty())
                    name += "\n" + std::to_string(value) + ": " + alias;
            }
        return q(name);
    }
    SctMeaningTarget sectionTarget(SctScriptId script, sct::SctSectionId section) const {
        for (const auto &e : state->project.entrypoints)
            if (e.script == script && e.binding)
                if (const auto *sequence = state->project.find(e.binding->sequence);
                    sequence && sequence->section == section) {
                    SctMeaningTarget t;
                    t.script = script;
                    t.entity = e.id;
                    return t;
                }
        return SctInterpretation::sectionTarget(state->project, script, section);
    }
    const SctStateEffectScope *scope() const {
        const auto n = roots->currentIndex();
        return n < 0 || n >= static_cast<int>(analysis.effects.size()) ? nullptr : &analysis.effects[n];
    }
    const SctStoryline *currentStory() const {
        const auto id = stories->currentData().toULongLong();
        for (const auto &s : state->project.storylines)
            if (s.id.value == id)
                return &s;
        return nullptr;
    }
    void refresh() {
        std::optional<SctMeaningTarget> old;
        if (const auto *s = scope())
            old = s->root;
        const auto oldStory = stories->currentData();
        state = controller.authoringState();
        if (!state)
            return;
        analysis = SctInterpretation::analyze(*state);
        locations.clear();
        for (const auto &script : state->project.scripts) {
            auto w = SctAuthoringMaterializer::workingState(state->project, state->programs, script.id);
            if (w)
                for (const auto &sec : w.value().document->sections)
                    if (const auto *body = std::get_if<sct::SctScriptSectionContent>(&sec.content))
                        for (const auto &i : body->instructions)
                            locations[{script.id, i.id}] = sec.id;
        }
        roots->blockSignals(true);
        roots->clear();
        for (const auto &e : analysis.effects)
            roots->addItem(label(e.root));
        if (old)
            for (std::size_t i = 0; i < analysis.effects.size(); ++i)
                if (analysis.effects[i].root == *old)
                    roots->setCurrentIndex(static_cast<int>(i));
        roots->blockSignals(false);
        stories->blockSignals(true);
        stories->clear();
        for (const auto &s : state->project.storylines)
            stories->addItem(q(s.name), QVariant::fromValue<qulonglong>(s.id.value));
        const auto n = stories->findData(oldStory);
        if (n >= 0)
            stories->setCurrentIndex(n);
        stories->blockSignals(false);
        prepared.reset();
        instructions->clear();
        story();
        draw();
    }
    QGraphicsRectItem *node(SctMeaningTarget target, QPointF position, QColor color,
                            std::optional<SctStoryStepId> occurrence = {}) {
        for (const auto &p : controller.authoringPresentation().storyPlacements)
            if (p.target == target && p.occurrence == occurrence)
                position = {p.x, p.y};
        auto *rect = new StoryNode;
        rect->setRect(0, 0, 240, 68);
        rect->setPen(QPen(storyColors::border, 2.0));
        rect->setBrush(QBrush(color));
        graph->addItem(rect);
        rect->setPos(position);
        rect->moved = [this] { updateEdges(); };
        rect->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable |
                       QGraphicsItem::ItemSendsGeometryChanges);
        rect->setData(0, static_cast<int>(targets.size()));
        targets.push_back(target);
        if (occurrence)
            rect->setData(1, QVariant::fromValue<qulonglong>(occurrence->value));
        auto *text = new QGraphicsTextItem(label(target), rect);
        text->setDefaultTextColor(storyColors::text);
        text->setTextWidth(230);
        text->setAcceptedMouseButtons(Qt::NoButton);
        rect->setToolTip(q(SctInterpretation::describe(*state, target)));
        return rect;
    }
    void edge(QGraphicsRectItem *from, QGraphicsRectItem *to, const QString &text) {
        auto *line = graph->addLine(QLineF(), QPen(storyColors::connection, 1.5));
        line->setZValue(-1);
        auto *caption = graph->addText(text + QStringLiteral(" \u2192"));
        caption->setDefaultTextColor(storyColors::caption);
        caption->setZValue(-1);
        lines.push_back({from, to, line, caption});
        updateEdges();
    }
    void updateEdges() {
        for (const auto &e : lines) {
            auto a = e.from->pos() + QPointF(240, 34), b = e.to->pos() + QPointF(0, 34);
            e.line->setLine(QLineF(a, b));
            e.caption->setPos((a + b) / 2);
        }
    }
    void drawStory() {
        const auto *story = currentStory();
        if (!story)
            return;
        graph->blockSignals(true);
        lines.clear();
        graph->clear();
        targets.clear();
        QGraphicsRectItem *previous = nullptr;
        for (std::size_t n = 0; n < story->steps.size(); ++n) {
            const auto &step = story->steps[n];
            auto *current = node(step.target, {static_cast<qreal>((n % 3) * 380), static_cast<qreal>((n / 3) * 150)},
                                 storyColors::cutscene, step.id);
            if (!step.name.empty())
                for (auto *child : current->childItems())
                    if (auto *text = dynamic_cast<QGraphicsTextItem *>(child))
                        text->setPlainText(q(step.name));
            if (previous)
                edge(previous, current, "Story order " + QString::number(n));
            previous = current;
        }
        graph->blockSignals(false);
    }
    void draw() {
        graph->blockSignals(true);
        lines.clear();
        graph->clear();
        targets.clear();
        writeTargets.clear();
        writes->clear();
        readers->clear();
        readerTargets.clear();
        selected.reset();
        why->clear();
        const auto *e = scope();
        if (!e) {
            graph->blockSignals(false);
            return;
        }
        auto *root = node(e->root, {0, 0}, storyColors::cutscene);
        std::map<SctVariableKey, QGraphicsRectItem *> flags;
        std::map<SctVariableKey, std::set<QString>> operations;
        std::map<SctMeaningTarget, QGraphicsRectItem *> consumers;
        for (auto n : e->writes) {
            const auto &w = analysis.accesses[n];
            writes->addItem(operation(w.kind) + "  " +
                            q(w.variable.kind == SctVariableKind::Bit ? "BitVar" : "Variable") + "[" +
                            QString::number(w.variable.index) + QStringLiteral("]  \u2014  ") + label(w.target));
            writeTargets.push_back(w.target);
            operations[w.variable].insert(operation(w.kind));
            if (!flags.contains(w.variable)) {
                SctMeaningTarget t;
                t.script = e->root.script;
                t.variable = w.variable;
                flags[w.variable] = node(t, {380, static_cast<qreal>(flags.size() * 110)}, storyColors::variable);
            }
        }
        for (const auto &[v, item] : flags) {
            QStringList names;
            for (const auto &n : operations[v])
                names << n;
            edge(root, item, names.join(" / "));
            for (const auto &access : analysis.accesses)
                if (access.kind == SctStateAccessKind::Read && access.variable == v) {
                    const auto loc = locations.find({access.target.script, access.target.site->instruction});
                    if (loc == locations.end())
                        continue;
                    auto target = sectionTarget(access.target.script, loc->second);
                    if (!consumers.contains(target))
                        consumers[target] =
                            node(target, {800, static_cast<qreal>(consumers.size() * 100)}, storyColors::reader);
                    const auto pair = std::pair{v, target};
                    if (drawn.insert(pair).second)
                        edge(item, consumers[target], "Read");
                }
        }
        drawn.clear();
        for (auto n : e->unresolvedWrites) {
            const auto &w = analysis.unresolvedWrites[n];
            writes->addItem(operation(w.kind) + " BitVar[" + q(w.expression) + QStringLiteral("] \u2014 destination expression"));
            writeTargets.push_back(w.target);
        }
        graph->blockSignals(false);
        select(e->root);
        view->resetTransform();
        view->scale(controller.authoringPresentation().storyZoom, controller.authoringPresentation().storyZoom);
    }
    void select(SctMeaningTarget t, bool updateReaders = true) {
        selected = t;
        QString text = label(t) + "\n" + q(SctInterpretation::describe(*state, t));
        if (t.site) {
            const auto w = SctAuthoringMaterializer::workingState(state->project, state->programs, t.script);
            if (w) {
                const auto index = sct::SctDocumentIndex::build(*w.value().document);
                if (const auto *i = index.find(*w.value().document, t.site->instruction))
                    text += "\n" + instructionText(*i);
            }
        }
        if (const auto *e = scope(); e && t == e->root) {
            text += "\n\nAnalysis: all instructions in these sections, following parsed control-flow references into "
                    "helpers:\n";
            for (const auto &sec : e->sections)
                text += label(sec) + '\n';
        }
        for (const auto &record : state->project.explanations) {
            bool attached = std::ranges::find(record.subjects, t) != record.subjects.end();
            if (t.variable)
                for (const auto &subject : record.subjects)
                    if (subject.variable == t.variable && (!subject.script.valid() || subject.script == t.script))
                        attached = true;
            if (attached)
                text += "\n\n" + q(record.interpretation) + "\n" + q(record.reasoning) + "\nRule: " + q(record.rule) +
                        " / " + q(record.ruleVersion) + "\n" + q(record.qualification);
        }
        if (t.site)
            for (const auto &access : analysis.accesses)
                if (access.target == t && access.kind != SctStateAccessKind::Read)
                    text += "\n\nEffect recognition: " + operation(access.kind) +
                            " at this instruction. Included independently of naming, bank category, and position in "
                            "the cutscene.";
        why->setPlainText(text);
        if (updateReaders) {
            readers->blockSignals(true);
            readers->clear();
            readerTargets.clear();
            std::set<SctVariableKey> vars;
            if (t.variable)
                vars.insert(*t.variable);
            for (const auto &a : analysis.accesses)
                if (a.target == t)
                    vars.insert(a.variable);
            if (const auto *e = scope(); e && e->root == t)
                for (auto n : e->writes)
                    vars.insert(analysis.accesses[n].variable);
            for (const auto &a : analysis.accesses)
                if (a.kind == SctStateAccessKind::Read && vars.contains(a.variable)) {
                    readers->addItem(label(a.target) + "  " + q(a.expression));
                    readerTargets.push_back(a.target);
                }
            readers->blockSignals(false);
        }
    }
    QString instructionText(const sct::SctDocumentInstruction &i) const {
        QString text = "Instruction " + QString::number(i.id.value()) + QStringLiteral(" \u2014 opcode ") + QString::number(i.opcode);
        if (i.skipRefresh)
            text += " [skip refresh]";
        if (i.scheduledExpression)
            text += " [scheduled: " + q(SctExpressionLanguage::project(*i.scheduledExpression).text) + "]";
        const auto params = [&](const auto &ps, QString group) {
            for (const auto &p : ps) {
                text += "\n  " + group + "parameter " + QString::number(p.schemaIndex) + ": ";
                std::visit(
                    [&](const auto &value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, sct::SctCanonicalExpression>)
                            text += q(SctExpressionLanguage::project(value).text);
                        else if constexpr (requires { value.target; })
                            text += "target " + QString::number(value.target.value());
                        else if constexpr (requires { value.words; })
                            for (auto word : value.words)
                                text += QString::number(word, 16) + " ";
                        else if constexpr (requires { value.encodedWords; })
                            for (auto word : value.encodedWords)
                                text += QString::number(word, 16) + " ";
                        else
                            text += QString::number(value.value);
                    },
                    p.value);
            }
        };
        params(i.fixedParameters, {});
        for (std::size_t g = 0; g < i.repeatedParameterGroups.size(); ++g)
            params(i.repeatedParameterGroups[g].parameters, "group " + QString::number(g) + " ");
        return text;
    }
    void saveLayout() {
        auto p = controller.authoringPresentation();
        for (auto *item : graph->items())
            if (item->flags().testFlag(QGraphicsItem::ItemIsMovable)) {
                auto n = item->data(0).toInt();
                if (n < 0 || n >= static_cast<int>(targets.size()))
                    continue;
                std::optional<SctStoryStepId> occurrence;
                if (item->data(1).isValid())
                    occurrence = SctStoryStepId{item->data(1).toULongLong()};
                auto it = std::ranges::find_if(p.storyPlacements, [&](const auto &position) {
                    return position.target == targets[n] && position.occurrence == occurrence;
                });
                SctStoryPlacement value{targets[n], item->pos().x(), item->pos().y(), occurrence};
                if (it == p.storyPlacements.end())
                    p.storyPlacements.push_back(value);
                else
                    *it = value;
            }
        auto r = controller.setAuthoringPresentation(std::move(p));
        if (!r)
            fail(r.diagnostics());
    }
    void name() {
        if (!selected)
            return;
        bool ok = false;
        auto value = QInputDialog::getText(this, tr("Display name"), label(*selected), QLineEdit::Normal, {}, &ok);
        if (ok && !value.trimmed().isEmpty()) {
            auto t = *selected;
            apply("Name story item",
                  [=](const auto &s) { return SctInterpretation::setName(s, t, value.toStdString()); });
        }
    }
    void story() {
        steps->clear();
        if (const auto *s = currentStory())
            for (const auto &t : s->steps)
                steps->addItem(t.name.empty() ? label(t.target) : q(t.name));
    }
    void newStory() {
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("Storyline"), tr("Name"), QLineEdit::Normal, "Main storyline", &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        apply("Create storyline", [=](const auto &s) {
            auto next = s;
            auto id = next.project.template allocate<SctStorylineId>();
            if (!id)
                return Result<SctAuthoringState>::failure(id.diagnostics());
            next.project.storylines.push_back({id.value(), name.toStdString(), {}});
            return Result<SctAuthoringState>::success(std::move(next));
        });
    }
    void editStory(int operation) {
        const auto *s = currentStory();
        if (!s)
            return;
        auto id = s->id;
        auto row = steps->currentRow();
        auto values = s->steps;
        if (operation == 0) {
            if (!scope())
                return;
            values.push_back({{}, scope()->root, {}});
        } else {
            if (row < 0 || row >= static_cast<int>(values.size()))
                return;
            if (operation == 1)
                values.erase(values.begin() + row);
            else {
                auto other = row + (operation == -1 ? -1 : 1);
                if (other < 0 || other >= static_cast<int>(values.size()))
                    return;
                std::swap(values[row], values[other]);
            }
        }
        apply("Edit storyline order", [=](const auto &state) {
            auto next = state;
            for (auto &story : next.project.storylines)
                if (story.id == id) {
                    story.steps = values;
                    for (auto &step : story.steps)
                        if (!step.id.valid()) {
                            auto allocated = next.project.template allocate<SctStoryStepId>();
                            if (!allocated)
                                return Result<SctAuthoringState>::failure(allocated.diagnostics());
                            step.id = allocated.value();
                        }
                }
            return Result<SctAuthoringState>::success(std::move(next));
        });
    }
    void nameStep() {
        const auto *story = currentStory();
        auto row = steps->currentRow();
        if (!story || row < 0 || row >= static_cast<int>(story->steps.size()))
            return;
        auto storyId = story->id;
        auto stepId = story->steps[row].id;
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("Story occurrence"), tr("Name"), QLineEdit::Normal,
                                          q(story->steps[row].name), &ok);
        if (!ok)
            return;
        apply("Name story occurrence", [=](const auto &state) {
            auto next = state;
            for (auto &s : next.project.storylines)
                if (s.id == storyId)
                    for (auto &step : s.steps)
                        if (step.id == stepId)
                            step.name = name.toStdString();
            return Result<SctAuthoringState>::success(std::move(next));
        });
    }
    void suggest(bool anchored) {
        SctStoryPathOptions options;
        auto initial = SctStoryPaths::analyze(*state);
        if (!initial) {
            fail(initial.diagnostics());
            return;
        }
        if (anchored && scope())
            for (const auto &[id, target] : initial.value().targets)
                if (target == scope()->root) {
                    options.start = id;
                    break;
                }
        if (anchored && !options.start) {
            why->setPlainText(tr("Choose an available cutscene/module anchor."));
            return;
        }
        auto result = anchored ? SctStoryPaths::analyze(*state, options) : std::move(initial);
        if (!result) {
            fail(result.diagnostics());
            return;
        }
        proposal = std::move(result.value());
        pathChoices->blockSignals(true);
        pathChoices->clear();
        for (std::size_t n = 0; n < proposal->graph.at("paths").size(); ++n)
            pathChoices->addItem(tr("Candidate path %1").arg(n + 1));
        pathChoices->blockSignals(false);
        showPath();
        tabs->setCurrentWidget(proposalTab);
        why->setPlainText(
            tr("Suggested partial ordering. Cycles stay grouped; select individual occurrences in the desired order. "
               "Accepting changes narrative metadata only.\n") +
            (proposal->graph.at("truncated") == true ? tr("Enumeration reached its configured limit.\n") : QString{}) +
            q(proposal->graph.at("gaps").dump(2)));
    }
    void showPath() {
        proposedSteps->clear();
        proposalNodes.clear();
        if (!proposal)
            return;
        auto n = pathChoices->currentIndex();
        if (n < 0 || n >= static_cast<int>(proposal->graph.at("paths").size()))
            return;
        std::map<std::string, SctConnectivityJson> nodes;
        for (const auto &node : proposal->graph.at("nodes"))
            nodes[node.at("id")] = node;
        for (const auto &componentId : proposal->graph.at("paths")[n]) {
            const auto &component = proposal->graph.at("components").at(std::stoull(componentId.get<std::string>()));
            for (const auto &node : component.at("members")) {
                auto id = node.get<std::string>();
                auto t = proposal->targets.find(id);
                QString text = t == proposal->targets.end() ? q(nodes.at(id).value("name", id)) : label(t->second);
                if (component.at("cycle") == true)
                    text = tr("Cycle / unordered: ") + text;
                auto *item = new QListWidgetItem(text, proposedSteps);
                if (t == proposal->targets.end())
                    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
                proposalNodes.push_back(id);
            }
        }
    }
    void adopt(bool reject) {
        if (!proposal)
            return;
        const auto *story = currentStory();
        if (!reject && !story) {
            why->setPlainText(tr("Create or choose a storyline first."));
            return;
        }
        std::vector<std::string> nodes;
        for (int i = 0; i < proposedSteps->count(); ++i)
            if (proposedSteps->item(i)->isSelected())
                nodes.push_back(proposalNodes[i]);
        if (nodes.empty())
            return;
        auto captured = *proposal;
        auto id = story ? story->id : SctStorylineId{};
        apply(reject ? "Reject suggested story order" : "Adopt suggested story order", [=](const auto &state) {
            return reject ? SctStoryPaths::reject(state, captured, nodes)
                          : SctStoryPaths::adopt(state, captured, id, nodes);
        });
        proposal.reset();
        pathChoices->clear();
        proposedSteps->clear();
    }
    void open() {
        if (!selected)
            return;
        auto t = *selected;
        auto *script = state->project.find(t.script);
        if (!script)
            return;
        if (t.entity)
            if (const auto *m = std::get_if<SctModuleId>(&*t.entity))
                if (const auto *module = state->project.find(*m); module && !module->sections.empty())
                    t.section = module->sections.front();
        if (t.entity)
            if (const auto *id = std::get_if<SctEntrypointId>(&*t.entity))
                if (const auto *entry = state->project.find(*id); entry && entry->binding)
                    if (const auto *sequence = state->project.find(entry->binding->sequence))
                        t.section = sequence->section;
        auto loc = state->project.find(script->baseline)->source.locator;
        if (t.site)
            navigate(loc, {SctNavigationKind::Instruction, t.site->instruction.value()});
        else if (t.section)
            navigate(loc, {SctNavigationKind::Section, t.section->value()});
        if (t.site || t.section)
            accept();
    }
    void generated() {
        if (!selected)
            return;
        auto current = controller.authoringState();
        if (!current || current->project.id != state->project.id ||
            current->project.revision != state->project.revision) {
            refresh();
            return;
        }
        auto t = *selected;
        if (t.variable) {
            instructions->setPlainText(
                tr("Choose a write, reader, module or section to inspect its generated instructions."));
            tabs->setCurrentWidget(instructions);
            return;
        }
        if (!prepared || prepared->realization.key.script != t.script) {
            auto result = SctAuthoringMaterializer::materialize(
                {std::make_shared<const SctAuthoringProject>(state->project), state->programs, {t.script}});
            if (!result.succeeded()) {
                QString text;
                for (const auto &d : result.diagnostics)
                    text += q(d.message) + '\n';
                for (const auto &s : result.scripts) {
                    for (const auto &d : s.infrastructureDiagnostics)
                        text += q(d.message) + '\n';
                    for (const auto &d : s.diagnostics)
                        text += q(d.message) + '\n';
                }
                instructions->setPlainText(text);
                tabs->setCurrentWidget(instructions);
                return;
            }
            prepared = std::move(result.scripts.front().prepared);
        }
        auto key = prepared->realization.key;
        auto ids = SctInterpretation::generatedInstructions(*state, t, *prepared, key);
        if (!ids) {
            fail(ids.diagnostics());
            return;
        }
        if (const auto *scopeValue = scope(); scopeValue && scopeValue->root == t) {
            std::set<sct::SctInstructionId> included(ids.value().begin(), ids.value().end());
            for (const auto &section : scopeValue->sections) {
                auto part = SctInterpretation::generatedInstructions(*state, section, *prepared, key);
                if (!part) {
                    fail(part.diagnostics());
                    return;
                }
                included.insert(part.value().begin(), part.value().end());
            }
            ids.value().clear();
            for (const auto &section : prepared->document->sections)
                if (const auto *body = std::get_if<sct::SctScriptSectionContent>(&section.content))
                    for (const auto &instruction : body->instructions)
                        if (included.contains(instruction.id))
                            ids.value().push_back(instruction.id);
        }
        QString text = label(t) + "\nProject " + q(key.project.value) + "\nSemantic revision " +
                       QString::number(key.revision.value) + "\nRealization " + q(key.realization.value) + "\n\n";
        const auto index = sct::SctDocumentIndex::build(*prepared->document);
        for (auto id : ids.value())
            if (const auto *i = index.find(*prepared->document, id)) {
                if (prepared->layout)
                    for (const auto &location : prepared->layout->instructions)
                        if (location.id == id)
                            text += "Payload offset " + QString::number(location.span.offset) + "\n";
                text += instructionText(*i) + "\n\n";
            }
        instructions->setPlainText(text);
        tabs->setCurrentWidget(instructions);
    }
    SctDocumentController &controller;
    AssetLocator locator;
    std::function<void(AssetLocator, SctNavigationTarget)> navigate;
    std::shared_ptr<const SctAuthoringState> state;
    SctInterpretationAnalysis analysis;
    std::optional<SctProjectStoryPaths> proposal;
    std::vector<std::string> proposalNodes;
    QComboBox *pathChoices;
    QListWidget *proposedSteps;
    QWidget *proposalTab;
    std::optional<SctMeaningTarget> selected;
    std::optional<SctPreparedAuthoringOutput> prepared;
    std::vector<SctMeaningTarget> targets, writeTargets, readerTargets;
    struct Edge {
        QGraphicsRectItem *from, *to;
        QGraphicsLineItem *line;
        QGraphicsTextItem *caption;
    };
    std::vector<Edge> lines;
    std::map<std::pair<SctScriptId, sct::SctInstructionId>, sct::SctSectionId> locations;
    std::set<std::pair<SctVariableKey, SctMeaningTarget>> drawn;
    QComboBox *roots, *stories;
    QListWidget *writes, *readers, *steps;
    QTextEdit *instructions, *why;
    QTabWidget *tabs;
    QGraphicsScene *graph;
    QGraphicsView *view;
};
} // namespace
void showSctStoryStateEditor(SctDocumentController &controller, const core::AssetLocator &locator,
                             std::function<void(core::AssetLocator, core::SctNavigationTarget)> navigate,
                             QWidget *parent) {
    StoryDialog dialog(controller, locator, std::move(navigate), parent);
    dialog.exec();
}
} // namespace salsa::qt
