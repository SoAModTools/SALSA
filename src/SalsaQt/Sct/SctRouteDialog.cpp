#include "SalsaCore/Authoring/SctPublicationSet.h"
#include "SalsaCore/Authoring/SctRouteAuthoring.h"
#include "SctInterpretationDialog.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <nlohmann/json.hpp>
#include <set>
namespace salsa::qt {
namespace {
using namespace core;
QString q(const std::string &s) {
    return QString::fromStdString(s);
}
QString diagnostics(const std::vector<Diagnostic> &ds) {
    QString s;
    for (const auto &d : ds)
        s += q(d.message) + "\n";
    return s;
}
class RouteDialog final : public QDialog {
  public:
    RouteDialog(SctDocumentController &c, QWidget *parent) : QDialog(parent), controller(c) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Executable routes and coordinated output"));
        resize(1150, 850);
        auto *layout = new QVBoxLayout(this);
        auto *tabs = new QTabWidget(this);
        layout->addWidget(tabs);
        auto *routeTab = new QWidget;
        auto *rl = new QVBoxLayout(routeTab);
        tabs->addTab(routeTab, tr("Routes"));
        rl->addWidget(new QLabel(tr("Select existing module or entrypoint action ports. Arrival conditions are "
                                    "selected in Entrypoints.\nNarrative steps remain separate; this operation edits "
                                    "executable instructions in both scripts.")));
        routes = new QComboBox;
        rl->addWidget(routes);
        auto *form = new QFormLayout;
        rl->addLayout(form);
        source = new QComboBox;
        arrival = new QComboBox;
        response = new QComboBox;
        form->addRow(tr("Source load / insertion anchor"), source);
        form->addRow(tr("Destination arrival"), arrival);
        form->addRow(tr("Destination module response"), response);
        transition = new QComboBox;
        transition->addItems({tr("Literal load"), tr("Warp (unqualified)"), tr("Saved-area return (unqualified)"),
                              tr("Battle (unqualified)"), tr("Ship-battle exit (unqualified)")});
        form->addRow(tr("Transition kind"), transition);
        draftOnly = new QCheckBox(tr("Save unresolved intent; retain current instructions"));
        form->addRow(draftOnly);
        draftReason = new QLineEdit;
        form->addRow(tr("Unresolved explanation"), draftReason);
        ordinal = new QSpinBox;
        ordinal->setRange(-1, 100000);
        ordinal->setSpecialValueText(tr("Comparison"));
        ordinal->setValue(-1);
        form->addRow(tr("Switch case ordinal"), ordinal);
        insert = new QCheckBox(tr("Insert a load after the selected source action"));
        form->addRow(insert);
        prerequisites = new QLineEdit;
        requirements = new QLineEdit;
        effects = new QLineEdit;
        continuation = new QLineEdit;
        form->addRow(tr("Source prerequisite action IDs (comma separated)"), prerequisites);
        form->addRow(tr("Destination requirement action IDs"), requirements);
        form->addRow(tr("Already authored source effect action IDs"), effects);
        form->addRow(tr("Source continuation action ID (optional)"), continuation);
        discriminator = new QLineEdit;
        discriminatorValue = new QCheckBox(tr("Require 1 (unchecked = 0)"));
        form->addRow(tr("Explicit bit discriminator (optional)"), discriminator);
        form->addRow(discriminatorValue);
        auto *rb = new QHBoxLayout;
        rl->addLayout(rb);
        button(rb, "Refresh endpoints", [this] { refresh(); });
        button(rb, "Review both endpoint edits", [this] { reviewRoute(); });
        apply = button(rb, "Apply reviewed route", [this] { applyRoute(); });
        apply->setEnabled(false);
        button(rb, "Detach selected route", [this] {
            auto state = controller.authoringState();
            auto id = SctConnectionId{routes->currentData().toULongLong()};
            if (!state || !id.valid())
                return;
            auto r =
                controller.executeAuthoringCommand(state->project.revision, "Detach route (retain instructions)",
                                                   [id](const auto &s) { return SctRouteAuthoring::remove(s, id); });
            detail->setPlainText(r ? tr("Route detached. Its executable instructions remain editable in sequences.")
                                   : diagnostics(r.diagnostics()));
            refresh();
        });
        detail = new QTextEdit;
        detail->setReadOnly(true);
        rl->addWidget(detail, 1);
        connect(routes, &QComboBox::currentIndexChanged, this, [this] { selectRoute(); });
        auto *outputTab = new QWidget;
        auto *ol = new QVBoxLayout(outputTab);
        tabs->addTab(outputTab, tr("Coordinated output"));
        ol->addWidget(new QLabel(tr("Select one seed script. The complete route dependency closure is required. Review "
                                    "encoding settings and generated instructions before installing.")));
        auto *ob = new QHBoxLayout;
        ol->addLayout(ob);
        seed = new QComboBox;
        ob->addWidget(seed, 1);
        button(ob, "Load dependency closure", [this] { loadTargets(); });
        button(ob, "Choose output directory", [this] {
            auto path = QFileDialog::getExistingDirectory(this, tr("Output directory"));
            if (!path.isEmpty())
                root->setText(path);
        });
        root = new QLineEdit;
        ol->addWidget(root);
        replace =
            new QCheckBox(tr("Authorize replacement of original sources only if their imported hashes still match"));
        ol->addWidget(replace);
        targets = new QTableWidget;
        targets->setColumnCount(6);
        targets->setHorizontalHeaderLabels({tr("Script"), tr("Relative output path"), tr("Platform"), tr("Byte order"),
                                            tr("Text convention"), tr("Compression")});
        ol->addWidget(targets, 2);
        auto *pb = new QHBoxLayout;
        ol->addLayout(pb);
        prepare = button(pb, "Prepare and review output set", [this] { prepareOutput(); });
        install = button(pb, "Install reviewed set", [this] { installOutput(); });
        install->setEnabled(false);
        button(pb, "Cancel output operation", [this] { stop.request_stop(); });
        report = new QTextEdit;
        report->setReadOnly(true);
        ol->addWidget(report, 2);
        connect(&prepareWatcher, &QFutureWatcher<Result<SctPreparedPublicationSet>>::finished, this, [this] {
            prepare->setEnabled(true);
            auto result = prepareWatcher.result();
            if (preparingGeneration != outputGeneration) {
                report->setPlainText(
                    tr("Output settings changed during preparation. Prepare the updated selection again."));
                return;
            }
            if (!result) {
                report->setPlainText(diagnostics(result.diagnostics()));
                return;
            }
            prepared = std::make_shared<const SctPreparedPublicationSet>(std::move(result).takeValue());
            install->setEnabled(true);
            QString text =
                tr("Captured revision %1. Every output is encoded and verified; installation has not started.\n")
                    .arg(prepared->review().revision.value);
            for (const auto &out : prepared->materialization().scripts) {
                text += tr("\nScript %1: %2 bytes, SHA256 %3\n")
                            .arg(out.script.value)
                            .arg(out.prepared->bytes.size())
                            .arg(q(out.prepared->digest.toHex()));
                std::set<spice::sct::SctInstructionId> selected;
                for (const auto &record : out.prepared->realization.records)
                    if (std::holds_alternative<SctConnectionId>(record.entity) ||
                        std::holds_alternative<SctEntrypointId>(record.entity))
                        for (const auto &location : record.locations) {
                            if (const auto *entity = std::get_if<spice::sct::SctDocumentEntityId>(&location)) {
                                if (const auto *id = std::get_if<spice::sct::SctInstructionId>(entity))
                                    selected.insert(*id);
                            } else if (const auto *site = std::get_if<spice::sct::SctParameterSite>(&location))
                                selected.insert(site->instruction);
                        }
                text += instructionReview(*out.prepared->document, out.script, prepared->review().revision, selected);
            }
            report->setPlainText(text);
        });
        connect(&installWatcher, &QFutureWatcher<SctPublicationSetResult>::finished, this, [this] {
            prepare->setEnabled(true);
            auto r = installWatcher.result();
            if (r.receipt)
                report->append(tr("Verified complete set installed for revision %1. Receipt: "
                                  "%2/.salsa-publication/receipts/%3.json\nNewer project edits remain unpublished.")
                                   .arg(r.receipt->revision.value)
                                   .arg(q(r.receipt->root.string()))
                                   .arg(q(r.receipt->transactionId)));
            else
                report->append((r.cancelled ? tr("Cancelled. ") : QString{}) +
                               tr("No successful publication receipt. Transaction status %1.\n")
                                   .arg(static_cast<unsigned>(r.status)) +
                               diagnostics(r.diagnostics));
        });
        auto invalidateRoute = [this] {
            reviewed.reset();
            apply->setEnabled(false);
        };
        for (auto *box : {source, arrival, response, transition})
            connect(box, &QComboBox::currentIndexChanged, this, invalidateRoute);
        connect(ordinal, &QSpinBox::valueChanged, this, invalidateRoute);
        connect(insert, &QCheckBox::toggled, this, invalidateRoute);
        connect(draftOnly, &QCheckBox::toggled, this, invalidateRoute);
        connect(draftReason, &QLineEdit::textChanged, this, invalidateRoute);
        connect(discriminatorValue, &QCheckBox::toggled, this, invalidateRoute);
        for (auto *field : {prerequisites, requirements, effects, continuation, discriminator})
            connect(field, &QLineEdit::textChanged, this, invalidateRoute);
        connect(root, &QLineEdit::textChanged, this, [this] { invalidateOutput(); });
        connect(replace, &QCheckBox::toggled, this, [this] { invalidateOutput(); });
        connect(seed, &QComboBox::currentIndexChanged, this, [this] { invalidateOutput(); });
        connect(targets, &QTableWidget::itemChanged, this, [this] { invalidateOutput(); });
        refresh();
        loadTargets();
    }
    ~RouteDialog() {
        stop.request_stop();
    }

  private:
    SctDocumentController &controller;
    QComboBox *routes, *source, *arrival, *response, *seed, *transition;
    QCheckBox *draftOnly;
    QLineEdit *draftReason;
    QSpinBox *ordinal;
    QCheckBox *insert, *replace, *discriminatorValue;
    QLineEdit *discriminator;
    QLineEdit *prerequisites, *requirements, *effects, *continuation, *root;
    QTextEdit *detail, *report;
    QTableWidget *targets;
    QPushButton *apply, *prepare, *install;
    std::shared_ptr<const SctAuthoringState> reviewed, outputCapture;
    RevisionId reviewedRevision;
    std::shared_ptr<const SctPreparedPublicationSet> prepared;
    QFutureWatcher<Result<SctPreparedPublicationSet>> prepareWatcher;
    QFutureWatcher<SctPublicationSetResult> installWatcher;
    std::stop_source stop;
    std::uint64_t outputGeneration = 0, preparingGeneration = 0;
    void invalidateOutput() {
        ++outputGeneration;
        prepared.reset();
        install->setEnabled(false);
    }
    template <class F> QPushButton *button(QHBoxLayout *l, const char *label, F f) {
        auto *b = new QPushButton(tr(label));
        l->addWidget(b);
        connect(b, &QPushButton::clicked, this, f);
        return b;
    }
    static QString instructionReview(const spice::sct::SctDocument &d, SctScriptId script, RevisionId revision,
                                     const std::set<spice::sct::SctInstructionId> &selected) {
        QString result = tr("Script %1, captured revision %2. Selected endpoint/generated instructions:\n")
                             .arg(script.value)
                             .arg(revision.value);
        const auto index = spice::sct::SctDocumentIndex::build(d);
        for (auto id : selected) {
            const auto *instruction = index.find(d, id);
            if (!instruction)
                continue;
            result += tr("Instruction %1, opcode %2\n").arg(id.value()).arg(instruction->opcode);
            auto parameters = [&](const auto &ps) {
                for (const auto &p : ps)
                    if (const auto *expression = std::get_if<spice::sct::SctCanonicalExpression>(&p.value))
                        result += tr("  Parameter %1: %2\n")
                                      .arg(p.schemaIndex)
                                      .arg(q(SctExpressionLanguage::project(*expression).text));
            };
            parameters(instruction->fixedParameters);
            for (const auto &group : instruction->repeatedParameterGroups)
                parameters(group.parameters);
            auto observation = SctProgramStateCodec::inspectInstruction(d, id);
            if (observation)
                result += q(nlohmann::json::parse(observation.value()).dump(2)) + "\n";
        }
        return result;
    }
    void refresh() {
        auto state = controller.authoringState();
        if (!state)
            return;
        reviewed.reset();
        apply->setEnabled(false);
        routes->blockSignals(true);
        routes->clear();
        routes->addItem(tr("New route"), qulonglong(0));
        source->clear();
        arrival->clear();
        response->clear();
        seed->clear();
        for (const auto &c : state->project.connections)
            if (c.route)
                routes->addItem(tr("Route %1: %2 -> %3%4")
                                    .arg(c.id.value)
                                    .arg(q(c.route->sourceStem))
                                    .arg(q(c.route->targetFilename))
                                    .arg(c.route->unresolved ? tr(" (unresolved)") : QString{}),
                                qulonglong(c.id.value));
        for (const auto &script : state->project.scripts)
            seed->addItem(q(script.name) + tr(" [%1]").arg(script.id.value), qulonglong(script.id.value));
        for (const auto &p : state->project.ports)
            if (p.action) {
                auto script =
                    std::visit([&](auto id) { return state->project.effectiveScript(SctContentOwner{id}); }, p.owner);
                auto name = script ? q(state->project.find(*script)->name) : QString{};
                auto label =
                    name + " / " + q(p.name) + tr(" [action %1, port %2]").arg(p.action->value).arg(p.id.value);
                source->addItem(label, qulonglong(p.id.value));
                if (std::holds_alternative<SctModuleId>(p.owner))
                    response->addItem(label, qulonglong(p.id.value));
            }
        for (const auto &e : state->project.entrypoints)
            if (e.binding && e.binding->kind == SctEntrypointKind::Arrival)
                arrival->addItem(q(state->project.find(e.script)->name) + " / " + q(e.name) +
                                     (e.enabled ? QString{} : tr(" (disabled)")),
                                 qulonglong(e.id.value));
        routes->blockSignals(false);
    }
    void selectRoute() {
        auto state = controller.authoringState();
        if (!state)
            return;
        auto *c = state->project.find(SctConnectionId{routes->currentData().toULongLong()});
        if (!c || !c->route)
            return;
        if (auto *id = std::get_if<SctPortId>(&c->source))
            source->setCurrentIndex(source->findData(qulonglong(id->value)));
        if (auto *id = std::get_if<SctPortId>(&c->destination))
            response->setCurrentIndex(response->findData(qulonglong(id->value)));
        arrival->setCurrentIndex(arrival->findData(qulonglong(c->route->arrival.value)));
        ordinal->setValue(c->route->arrivalCase ? static_cast<int>(*c->route->arrivalCase) : -1);
        insert->setChecked(false);
        transition->setCurrentIndex(static_cast<int>(c->route->kind));
        draftOnly->setChecked(c->route->unresolved.has_value());
        draftReason->setText(c->route->unresolved ? q(c->route->unresolved->explanation) : QString{});
        discriminator->setText(c->route->discriminatorBit ? QString::number(*c->route->discriminatorBit) : QString{});
        discriminatorValue->setChecked(c->route->discriminatorValue);
        prerequisites->clear();
        requirements->clear();
        effects->clear();
        continuation->clear();
        for (const auto &o : c->route->observations) {
            QLineEdit *field = o.role == SctRouteObservationRole::Prerequisite   ? prerequisites
                               : o.role == SctRouteObservationRole::Requirement  ? requirements
                               : o.role == SctRouteObservationRole::Effect       ? effects
                               : o.role == SctRouteObservationRole::Continuation ? continuation
                                                                                 : nullptr;
            if (field)
                field->setText(field->text() + (field->text().isEmpty() ? "" : ",") + QString::number(o.action.value));
        }
        detail->setPlainText(c->route->unresolved ? q(c->route->unresolved->explanation)
                                                  : tr("Resolved endpoint observations. Review before reconnecting."));
        reviewed.reset();
        apply->setEnabled(false);
    }
    static std::vector<SctActionId> ids(QLineEdit *field) {
        std::vector<SctActionId> result;
        for (const auto &text : field->text().split(',', Qt::SkipEmptyParts)) {
            bool ok = false;
            auto id = text.trimmed().toULongLong(&ok);
            if (!ok || !id)
                throw std::runtime_error("Action IDs must be nonzero decimal integers.");
            result.push_back({id});
        }
        return result;
    }
    void reviewRoute() {
        try {
            auto state = controller.authoringState();
            if (!state)
                return;
            SctRouteRequest r{{source->currentData().toULongLong()},
                              {response->currentData().toULongLong()},
                              {arrival->currentData().toULongLong()}};
            if (ordinal->value() >= 0)
                r.arrivalCase = ordinal->value();
            if (auto id = routes->currentData().toULongLong())
                r.replace = SctConnectionId{id};
            r.insertLoadAfterSource = insert->isChecked();
            r.prerequisites = ids(prerequisites);
            r.requirements = ids(requirements);
            r.effects = ids(effects);
            auto cs = ids(continuation);
            if (cs.size() > 1)
                throw std::runtime_error("Select at most one continuation.");
            if (!cs.empty())
                r.continuation = cs.front();
            if (!discriminator->text().trimmed().isEmpty()) {
                bool ok = false;
                auto bit = discriminator->text().toUInt(&ok);
                if (!ok)
                    throw std::runtime_error("Invalid discriminator bit index.");
                r.discriminatorBit = bit;
                r.discriminatorValue = discriminatorValue->isChecked();
            }
            auto result =
                draftOnly->isChecked() || transition->currentIndex() != 0
                    ? SctRouteAuthoring::draft(*state, r, static_cast<SctTransitionKind>(transition->currentIndex()),
                                               "Selected source port " + std::to_string(r.source.value),
                                               draftReason->text().toStdString())
                    : SctRouteAuthoring::connect(*state, r);
            if (!result) {
                detail->setPlainText(diagnostics(result.diagnostics()));
                apply->setEnabled(false);
                return;
            }
            reviewedRevision = state->project.revision;
            reviewed = std::make_shared<const SctAuthoringState>(std::move(result).takeValue());
            const auto *c = r.replace ? reviewed->project.find(*r.replace) : &reviewed->project.connections.back();
            QString text = tr("Review revision %1. This changes both endpoint instructions in one undoable command. No "
                              "state effects are invented.\n")
                               .arg(reviewedRevision.value);
            for (auto script : {c->route->sourceScript, c->route->destinationScript}) {
                auto working = SctAuthoringMaterializer::workingState(reviewed->project, reviewed->programs, script);
                std::set<spice::sct::SctInstructionId> selected;
                for (const auto &o : c->route->observations)
                    if (o.script == script)
                        if (const auto *action = reviewed->project.find(o.action))
                            selected.insert(action->instruction);
                if (working)
                    text += tr("\nScript %1\n").arg(script.value) +
                            instructionReview(*working.value().document, script, reviewedRevision, selected);
            }
            detail->setPlainText(text);
            apply->setEnabled(true);
        } catch (const std::exception &e) {
            detail->setPlainText(e.what());
        }
    }
    void applyRoute() {
        if (!reviewed)
            return;
        auto snapshot = reviewed;
        auto r =
            controller.executeAuthoringCommand(reviewedRevision, "Author cross-script route", [snapshot](const auto &) {
                return Result<SctAuthoringState>::success(*snapshot);
            });
        detail->setPlainText(r ? tr("Both endpoints committed. Undo restores both.") : diagnostics(r.diagnostics()));
        refresh();
    }
    QComboBox *combo(int row, int column, QStringList values, int selected) {
        auto *box = new QComboBox;
        box->addItems(values);
        box->setCurrentIndex(selected);
        targets->setCellWidget(row, column, box);
        connect(box, &QComboBox::currentIndexChanged, this, [this] { invalidateOutput(); });
        return box;
    }
    void loadTargets() {
        outputCapture = controller.authoringState();
        if (!outputCapture)
            return;
        std::array seeds{SctScriptId{seed->currentData().toULongLong()}};
        auto closure = SctRouteAuthoring::closure(outputCapture->project, seeds);
        targets->setRowCount(static_cast<int>(closure.size()));
        for (int row = 0; row < static_cast<int>(closure.size()); ++row) {
            auto *s = outputCapture->project.find(closure[row]);
            if (!s)
                continue;
            auto *b = outputCapture->project.find(s->baseline);
            auto *label = new QTableWidgetItem(q(s->name));
            label->setData(Qt::UserRole, qulonglong(s->id.value));
            label->setFlags(label->flags() & ~Qt::ItemIsEditable);
            targets->setItem(row, 0, label);
            targets->setItem(row, 1, new QTableWidgetItem(q(b->source.locator.path().filename().string())));
            combo(row, 2, {"GameCube", "Dreamcast"}, s->platform == GamePlatform::Dreamcast ? 1 : 0);
            combo(row, 3, {"Big endian", "Little endian"}, s->outputProfile && !s->outputProfile->bigEndian ? 1 : 0);
            combo(row, 4, {"Windows-1252 / 7F", "Shift-JIS / 7F", "Shift-JIS / 8140"},
                  static_cast<int>(b->textConvention.value_or(spice::sct::SctKnownTextConvention::ShiftJisByte7F)));
            combo(row, 5, {"Raw", "AKLZ"}, s->outputProfile && s->outputProfile->compressed ? 1 : 0);
        }
        targets->resizeColumnsToContents();
        prepared.reset();
        install->setEnabled(false);
    }
    void prepareOutput() {
        if (prepareWatcher.isRunning() || installWatcher.isRunning() || !outputCapture)
            return;
        outputCapture = controller.authoringState();
        SctPublicationSetRequest r{outputCapture,
                                   outputCapture->project.revision,
                                   std::filesystem::path(root->text().toStdWString()),
                                   {{seed->currentData().toULongLong()}}};
        for (int row = 0; row < targets->rowCount(); ++row) {
            auto selected = [&](int col) {
                return static_cast<QComboBox *>(targets->cellWidget(row, col))->currentIndex();
            };
            SctPublicationTarget target;
            target.script = {targets->item(row, 0)->data(Qt::UserRole).toULongLong()};
            target.relativePath = targets->item(row, 1)->text().toStdWString();
            target.options.platform =
                selected(2) ? spice::sct::SctPlatform::Dreamcast : spice::sct::SctPlatform::GameCube;
            target.options.byteOrder = selected(3) ? spice::sct::SctDocumentOutputByteOrder::LittleEndian
                                                   : spice::sct::SctDocumentOutputByteOrder::BigEndian;
            target.options.textEncoding =
                *spice::sct::sctTextEncodingFor(static_cast<spice::sct::SctKnownTextConvention>(selected(4)));
            target.options.wrapper =
                selected(5) ? spice::sct::SctDocumentOutputWrapper::Aklz : spice::sct::SctDocumentOutputWrapper::Raw;
            target.originalSource = controller.authoringSourcePath(target.script);
            target.allowSourceReplacement = replace->isChecked();
            r.targets.push_back(target);
        }
        stop = std::stop_source{};
        preparingGeneration = outputGeneration;
        auto token = stop.get_token();
        prepare->setEnabled(false);
        install->setEnabled(false);
        report->setPlainText(tr("Preparing captured output set…"));
        prepareWatcher.setFuture(
            QtConcurrent::run([r = std::move(r), token] { return SctPublicationSet::prepare(r, token); }));
    }
    void installOutput() {
        if (!prepared || installWatcher.isRunning())
            return;
        auto set = prepared;
        stop = std::stop_source{};
        auto token = stop.get_token();
        install->setEnabled(false);
        prepare->setEnabled(false);
        installWatcher.setFuture(QtConcurrent::run([set, token] { return SctPublicationSet::install(*set, token); }));
    }
};
} // namespace
void showSctRouteEditor(SctDocumentController &controller, QWidget *parent) {
    auto *dialog = new RouteDialog(controller, parent);
    dialog->show();
}
} // namespace salsa::qt
