#include "SctInterpretationDialog.h"
#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SalsaCore/Sct/SctWorkingState.h"
#include "SctStructuredOutlineModel.h"
#include "SpiceSCT/SctDocumentAnalysis.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeView>
#include <QVBoxLayout>
#include <regex>
#include <limits>
namespace salsa::qt {
namespace {
using namespace core;
namespace sct = spice::sct;
QString q(const std::string &s) {
    return QString::fromStdString(s);
}
class InterpretationDialog final : public QDialog {
  public:
    InterpretationDialog(SctDocumentController &c, AssetLocator loc,
                         std::function<void(AssetLocator, SctNavigationTarget)> nav, QWidget *parent)
        : QDialog(parent), controller(c), locator(std::move(loc)), navigate(std::move(nav)) {
        setWindowTitle(tr("Interactions and interpretations"));
        resize(1200, 820);
        auto *layout = new QVBoxLayout(this);
        auto *top = new QHBoxLayout;
        layout->addLayout(top);
        scripts = new QComboBox(this);
        top->addWidget(scripts, 1);
        friendly = new QCheckBox(tr("Friendly conditions"), this);
        friendly->setChecked(true);
        top->addWidget(friendly);
        allSections = new QCheckBox(tr("All script sections"), this);
        top->addWidget(allSections);
        auto *split = new QSplitter(this);
        layout->addWidget(split, 1);
        tree = new QTreeView(split);
        model = new SctStructuredOutlineModel(tree);
        tree->setModel(model);
        tree->setDragDropMode(QAbstractItemView::NoDragDrop);
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        auto *side = new QWidget(split);
        auto *right = new QVBoxLayout(side);
        right->addWidget(new QLabel(tr("Why this interpretation? Select a condition or named item."), side));
        records = new QListWidget(side);
        right->addWidget(records);
        details = new QTextEdit(side);
        details->setReadOnly(true);
        right->addWidget(details, 2);
        evidence = new QListWidget(side);
        right->addWidget(evidence);
        history = new QCheckBox(tr("Include interpretation history / debug observations"), side);
        right->addWidget(history);
        auto *buttons = new QHBoxLayout;
        layout->addLayout(buttons);
        add(buttons, tr("Analyze project"), [this] {
            if (!state)
                return;
            auto a = SctInterpretation::analyze(*state);
            apply("Record state interpretations", [a](const auto &s) { return SctInterpretation::record(s, a); });
        });
        add(buttons, tr("Name selected item…"), [this] { rename(); });
        add(buttons, tr("Name variable / value…"), [this] { nameVariable(); });
        add(buttons, tr("Accept interpretation"), [this] { decide(SctInterpretationDecision::Accepted); });
        add(buttons, tr("Reject interpretation"), [this] { decide(SctInterpretationDecision::Rejected); });
        add(buttons, tr("Edit in semantic editor"), [this] {
            if (auto t = model->target(tree->currentIndex())) {
                navigate(locator, *t);
                accept();
            }
        });
        auto *bottom = new QHBoxLayout;
        layout->addLayout(bottom);
        add(bottom, tr("Undo"), [this] {
            (void)controller.undo(locator);
            refresh();
        });
        add(bottom, tr("Redo"), [this] {
            (void)controller.redo(locator);
            refresh();
        });
        auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
        bottom->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(scripts, &QComboBox::currentIndexChanged, this, [this] { refreshTree(); });
        connect(friendly, &QCheckBox::toggled, this, [this] { refreshTree(); });
        connect(allSections, &QCheckBox::toggled, this, [this] { filter(); });
        connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { selection(); });
        connect(records, &QListWidget::currentRowChanged, this, [this] { explain(); });
        connect(history, &QCheckBox::toggled, this, [this] { selection(); });
        connect(evidence, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            auto row = evidence->row(item);
            if (row < 0 || row >= static_cast<int>(evidenceTargets.size()))
                return;
            const auto &t = evidenceTargets[row];
            auto index = scripts->findData(QVariant::fromValue<qulonglong>(t.script.value));
            if (index < 0)
                return;
            scripts->setCurrentIndex(index);
            if (t.site)
                tree->setCurrentIndex(
                    model->indexForTarget({SctNavigationKind::Instruction, t.site->instruction.value()}));
            else if (t.section)
                tree->setCurrentIndex(model->indexForTarget({SctNavigationKind::Section, t.section->value()}));
        });
        refresh();
    }

  private:
    void add(QHBoxLayout *row, const QString &label, std::function<void()> action) {
        auto *b = new QPushButton(label, this);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, std::move(action));
    }
    void fail(const std::vector<Diagnostic> &ds) {
        QString message;
        for (const auto &d : ds)
            message += q(d.message) + '\n';
        QMessageBox::warning(this, tr("Interpretation"), message);
    }
    void apply(std::string name, const SctAuthoringSession::Command &command) {
        auto r = controller.executeAuthoringCommand(state->project.revision, std::move(name), command);
        if (!r)
            fail(r.diagnostics());
        refresh();
    }
    void refresh() {
        auto selected = scripts->currentData();
        state = controller.authoringState();
        if (state) {
            analysis = SctInterpretation::analyze(*state);
            displayState = *state;
            std::erase_if(displayState.project.meanings, [&](const auto &m) {
                if (!m.explanation)
                    return false;
                auto i = std::ranges::find(state->project.explanations, *m.explanation, &SctExplanationRecord::id);
                return i == state->project.explanations.end() || i->decision != SctInterpretationDecision::Accepted ||
                       ((i->origin != SctEvidenceKind::UserAssertion || m.target.site) &&
                        !SctInterpretation::fresh(*state, *i));
            });
            for (auto &m : displayState.project.meanings)
                m.explanation.reset();
        }
        scripts->blockSignals(true);
        scripts->clear();
        if (state)
            for (const auto &s : state->project.scripts) {
                scripts->addItem(q(s.name), QVariant::fromValue<qulonglong>(s.id.value));
                if (state->project.find(s.baseline)->source.locator == locator && !selected.isValid())
                    selected = QVariant::fromValue<qulonglong>(s.id.value);
            }
        auto row = scripts->findData(selected);
        if (row >= 0)
            scripts->setCurrentIndex(row);
        scripts->blockSignals(false);
        refreshTree();
    }
    void refreshTree() {
        if (!state || scripts->currentIndex() < 0)
            return;
        script = {scripts->currentData().toULongLong()};
        locator = state->project.find(state->project.find(script)->baseline)->source.locator;
        auto w = SctAuthoringMaterializer::workingState(state->project, state->programs, script);
        if (!w) {
            fail(w.diagnostics());
            return;
        }
        auto program = std::ranges::find_if(
            state->programs, [&](const auto &p) { return p->baseline().id == state->project.find(script)->baseline; });
        if (program == state->programs.end())
            return;
        auto snap = SctAuthoringMaterializer::snapshot(**program, w.value().document);
        auto projection = std::make_shared<SctSemanticEditorProjection>(SctSemanticEditorProjection::build(
            snap->analysis->structuredControlFlow, SctWorkingState(w.value().document),
            SctStructuredAuthoringState(w.value().authoredArms, w.value().unboundReferences, w.value().aliases),
            state->project.revision, state->project.revision));
        labels.clear();
        for (const auto &sec : w.value().document->sections) {
            auto t = SctInterpretation::sectionTarget(state->project, script, sec.id);
            labels[{SctNavigationKind::Section, sec.id.value()}] =
                q(SctInterpretation::label(*state, t, sec.nameBytes));
            for (const auto &e : state->project.entrypoints)
                if (e.script == script && e.binding)
                    if (const auto *seq = state->project.find(e.binding->sequence); seq && seq->section == sec.id) {
                        SctMeaningTarget et;
                        et.script = script;
                        et.entity = e.id;
                        labels[{SctNavigationKind::Section, sec.id.value()}] =
                            q(SctInterpretation::label(*state, et, SctInterpretation::label(*state, t, e.name)));
                    }
            if (const auto *b = std::get_if<sct::SctScriptSectionContent>(&sec.content))
                for (const auto &i : b->instructions)
                    if (i.opcode == 0 || i.opcode == 3)
                        for (const auto &p : i.fixedParameters)
                            if (p.schemaIndex == 0)
                                if (const auto *e = std::get_if<sct::SctCanonicalExpression>(&p.value)) {
                                    auto target =
                                        SctInterpretation::siteTarget(state->project, script, {i.id, {0, {}}});
                                    auto text =
                                        SctInterpretation::condition(displayState, script, *e, friendly->isChecked());
                                    if (friendly->isChecked())
                                        text = SctInterpretation::label(*state, target, text);
                                    labels[{SctNavigationKind::Instruction, i.id.value()}] = q(text);
                                }
        }
        model->setLabelRenderer([this](SctNavigationTarget t, QString fallback) {
            auto i = labels.find(t);
            if (i == labels.end())
                return fallback;
            return t.kind == SctNavigationKind::Section ? i->second : fallback + ": " + i->second;
        });
        model->resetFrom(snap, projection);
        filter();
        selection();
    }
    void filter() {
        for (int row = 0; row < model->rowCount(); ++row) {
            auto t = model->target(model->index(row, 0));
            bool visible = allSections->isChecked();
            if (t && state) {
                auto target = SctInterpretation::sectionTarget(state->project, script, sct::SctSectionId{t->id});
                auto name = SctInterpretation::describe(*state, target);
                visible |= name.ends_with(": init") || name.ends_with(": loop") ||
                           std::regex_search(name, std::regex(": M[0-9]{5}$"));
                for (const auto &e : state->project.entrypoints)
                    if (e.script == script && e.binding)
                        if (const auto *seq = state->project.find(e.binding->sequence);
                            seq && seq->section.value() == t->id)
                            visible = true;
            }
            tree->setRowHidden(row, {}, !visible);
        }
    }
    std::optional<SctMeaningTarget> selected() const {
        if (!state)
            return {};
        auto t = model->target(tree->currentIndex());
        if (!t)
            return {};
        if (t->kind == SctNavigationKind::Section) {
            for (const auto &e : state->project.entrypoints)
                if (e.script == script && e.binding)
                    if (const auto *seq = state->project.find(e.binding->sequence);
                        seq && seq->section.value() == t->id) {
                        SctMeaningTarget r;
                        r.script = script;
                        r.entity = e.id;
                        return r;
                    }
            return SctInterpretation::sectionTarget(state->project, script, sct::SctSectionId{t->id});
        }
        return SctInterpretation::siteTarget(state->project, script, {sct::SctInstructionId{t->id}, {0, {}}});
    }
    void selection() {
        records->clear();
        recordIds.clear();
        evidence->clear();
        details->clear();
        auto target = selected();
        if (!target)
            return;
        details->setPlainText(q(SctInterpretation::describe(*state, *target)) + "\n" +
                              q(SctInterpretation::observe(*state, *target)));
        std::vector<SctVariableKey> variables;
        if (target->site) {
            for (const auto &x : analysis.accesses)
                if (x.target.script == script && x.target.site &&
                    x.target.site->instruction == target->site->instruction)
                    variables.push_back(x.variable);
        }
        for (const auto &e : state->project.explanations) {
            bool attached = false;
            for (const auto &t : e.subjects)
                if (t == *target || (t.variable && std::ranges::find(variables, *t.variable) != variables.end() &&
                                     (!t.script.valid() || t.script == script)))
                    attached = true;
            if (!attached || (!history->isChecked() && e.decision == SctInterpretationDecision::Superseded))
                continue;
            recordIds.push_back(e.id);
            records->addItem(q(e.interpretation) + (SctInterpretation::fresh(*state, e) ? "" : " [evidence changed]") +
                             q(e.decision == SctInterpretationDecision::Suggested    ? " [suggested]"
                               : e.decision == SctInterpretationDecision::Rejected   ? " [rejected]"
                               : e.decision == SctInterpretationDecision::Superseded ? " [history]"
                                                                                     : " [accepted]"));
        }
        if (!recordIds.empty())
            records->setCurrentRow(0);
    }
    void explain() {
        auto row = records->currentRow();
        if (row < 0 || row >= static_cast<int>(recordIds.size()))
            return;
        const auto i = std::ranges::find(state->project.explanations, recordIds[row], &SctExplanationRecord::id);
        if (i == state->project.explanations.end())
            return;
        const auto &e = *i;
        QString text = q(e.interpretation) + "\n\n" + q(e.reasoning) + "\n\n" + q(e.qualification) +
                       "\n\nRule: " + q(e.rule) + " / " + q(e.ruleVersion) +
                       "\nCaptured revision: " + QString::number(e.revision.value);
        if (history->isChecked())
            for (const auto &o : e.observations)
                text += "\n\nCaptured observation:\n" + q(o);
        details->setPlainText(text);
        evidence->clear();
        evidenceTargets = e.evidence;
        for (const auto &t : e.evidence)
            evidence->addItem(q(SctInterpretation::describe(*state, t)));
    }
    void rename() {
        auto t = selected();
        if (!t)
            return;
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("Display name"), q(SctInterpretation::describe(*state, *t)),
                                          QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty())
            apply("Name authored item",
                  [=](const auto &s) { return SctInterpretation::setName(s, *t, name.toStdString()); });
    }
    void nameVariable() {
        bool ok = false;
        auto kind = QInputDialog::getItem(this, tr("Variable"), tr("Storage"), {"Bit", "Byte", "Integer", "Float"}, 0,
                                          false, &ok);
        if (!ok)
            return;
        int index = QInputDialog::getInt(this, tr("Variable"), tr("Index"), 0, 0, std::numeric_limits<int>::max(), 1, &ok);
        if (!ok)
            return;
        auto value =
            QInputDialog::getText(this, tr("Value"), tr("Optional integer value; leave empty to name the variable"),
                                  QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        SctMeaningTarget t;
        t.script = script;
        t.variable = SctVariableKey{kind == "Bit"       ? SctVariableKind::Bit
                                    : kind == "Byte"    ? SctVariableKind::Byte
                                    : kind == "Integer" ? SctVariableKind::Integer
                                                        : SctVariableKind::Float,
                                    static_cast<std::uint32_t>(index)};
        if (!value.isEmpty()) {
            auto n = value.toLongLong(&ok);
            if (!ok)
                return;
            t.value = n;
        }
        auto scope =
            QInputDialog::getItem(this, tr("Scope"), tr("Definition scope"), {"This script", "Project"}, 0, false, &ok);
        if (!ok)
            return;
        if (scope == "Project")
            t.script = {};
        auto name = QInputDialog::getText(this, tr("Name"), q(SctInterpretation::describe(*state, t)),
                                          QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty())
            apply("Name state definition",
                  [=](const auto &s) { return SctInterpretation::setName(s, t, name.toStdString()); });
    }
    void decide(SctInterpretationDecision d) {
        auto row = records->currentRow();
        if (row < 0 || row >= static_cast<int>(recordIds.size()))
            return;
        auto id = recordIds[row];
        apply("Decide interpretation", [=](const auto &s) { return SctInterpretation::decide(s, id, d); });
    }
    SctDocumentController &controller;
    AssetLocator locator;
    std::function<void(AssetLocator, SctNavigationTarget)> navigate;
    SctInterpretationAnalysis analysis;
    SctAuthoringState displayState;
    std::shared_ptr<const SctAuthoringState> state;
    SctScriptId script;
    QComboBox *scripts;
    QCheckBox *friendly, *allSections, *history;
    QTreeView *tree;
    SctStructuredOutlineModel *model;
    QListWidget *records, *evidence;
    QTextEdit *details;
    std::map<SctNavigationTarget, QString> labels;
    std::vector<SctExplanationId> recordIds;
    std::vector<SctMeaningTarget> evidenceTargets;
};
} // namespace
void showSctInterpretationEditor(SctDocumentController &controller, const core::AssetLocator &locator,
                                 std::function<void(core::AssetLocator, core::SctNavigationTarget)> navigate,
                                 QWidget *parent) {
    InterpretationDialog dialog(controller, locator, std::move(navigate), parent);
    dialog.exec();
}
} // namespace salsa::qt
