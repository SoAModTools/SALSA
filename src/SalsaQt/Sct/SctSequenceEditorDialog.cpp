#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SctSequenceEditorDialog.h"
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace salsa::qt {
namespace {
namespace sct = spice::sct;
using namespace core;
QString text(const sct::SctCanonicalExpression& expression) {
    return QString::fromStdString(SctExpressionLanguage::project(expression).text);
}
class Editor final : public QDialog {
public:
    Editor(SctDocumentController& controller, const AssetLocator& locator,
        std::function<void(SctNavigationTarget)> navigate, QWidget* parent, std::optional<SctModuleId> module = {})
        : QDialog(parent), controller_(controller), locator_(locator), navigate_(std::move(navigate)), module_(module) {
        setWindowTitle(tr("Sequences and Conditions")); resize(920, 580);
        auto* layout = new QVBoxLayout(this);
        auto* tabs = new QTabWidget(this); layout->addWidget(tabs);
        auto* sequencePage = new QWidget(tabs); auto* sequenceLayout = new QVBoxLayout(sequencePage);
        sequenceLayout->addWidget(new QLabel(module_ ? tr("Edit this module's sequences. Shared helpers and caller preparation retain their ownership.")
            : tr("Name a section as an editing sequence. Calls, branch targets and ongoing scheduled work keep their existing behavior."), this));
        sequences_ = new QComboBox(this); sequenceLayout->addWidget(sequences_);
        auto* sequenceButtons = new QHBoxLayout; sequenceLayout->addLayout(sequenceButtons);
        if (!module_) button(sequenceButtons, tr("Name sequence…"), [this] { promote(); });
        button(sequenceButtons, tr("Rename"), [this] { rename(false); });
        if (!module_) button(sequenceButtons, tr("Remove definition"), [this] { remove(false); });
        else button(sequenceButtons, tr("Append action…"), [this] { append(); });
        actions_ = new QTableWidget(this); actions_->setColumnCount(5);
        actions_->setHorizontalHeaderLabels({tr("Action"), tr("Kind"), tr("Schedule"), tr("Skip refresh"), tr("Targets")});
        actions_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        actions_->horizontalHeader()->setStretchLastSection(true);
        actions_->setSelectionBehavior(QAbstractItemView::SelectRows); actions_->setSelectionMode(QAbstractItemView::SingleSelection);
        actions_->setEditTriggers(QAbstractItemView::NoEditTriggers); sequenceLayout->addWidget(actions_);
        auto* actionButtons = new QHBoxLayout; sequenceLayout->addLayout(actionButtons);
        button(actionButtons, tr("Edit parameter…"), [this] { parameter(); });
        button(actionButtons, tr("Edit timing…"), [this] { timing(); });
        button(actionButtons, tr("Arrival switch case…"), [this] { arrivalCase(); });
        button(actionButtons, tr("Go to action"), [this] {
            const auto row = actions_->currentRow(); if (row < 0 || row >= static_cast<int>(views_.size())) return;
            navigate_({SctNavigationKind::Instruction, views_[row].instruction.id.value()}); accept();
        });
        tabs->addTab(sequencePage, tr("Sequences"));
        auto* conditionsPage = new QWidget(tabs); auto* conditionLayout = new QVBoxLayout(conditionsPage);
        conditionLayout->addWidget(new QLabel(tr("Named conditions share an exact expression across explicitly selected uses. Editing updates every use in one undo step."), this));
        predicates_ = new QComboBox(this); conditionLayout->addWidget(predicates_);
        expression_ = new QLineEdit(this); expression_->setReadOnly(true); conditionLayout->addWidget(expression_);
        uses_ = new QListWidget(this); conditionLayout->addWidget(uses_);
        auto* conditionButtons = new QHBoxLayout; conditionLayout->addLayout(conditionButtons);
        button(conditionButtons, tr("Name condition…"), [this] { nameCondition(); });
        button(conditionButtons, tr("Rename"), [this] { rename(true); });
        button(conditionButtons, tr("Edit expression…"), [this] { editCondition(); });
        button(conditionButtons, tr("Arriving from…"), [this] { arrival(); });
        button(conditionButtons, tr("Remove definition"), [this] { remove(true); });
        button(conditionButtons, tr("Go to use"), [this] {
            const auto* predicate = selectedPredicate(); const auto row = uses_->currentRow();
            if (!predicate || row < 0 || row >= static_cast<int>(predicate->uses.size())) return;
            navigate_({SctNavigationKind::Instruction, predicate->uses[row].instruction.value()}); accept();
        });
        tabs->addTab(conditionsPage, tr("Conditions"));
        if (module_) {
            boundary_ = new QListWidget(this); tabs->addTab(boundary_, tr("Callers and dependencies"));
            connect(boundary_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
                if (!item->data(Qt::UserRole).isValid()) return;
                navigate_({SctNavigationKind::Instruction, item->data(Qt::UserRole).toULongLong()});
            });
        }
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this); layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(sequences_, &QComboBox::currentIndexChanged, this, [this] { refreshActions(); });
        connect(predicates_, &QComboBox::currentIndexChanged, this, [this] { refreshPredicate(); });
        refresh();
    }
private:
    void button(QHBoxLayout* layout, const QString& label, std::function<void()> action) {
        auto* result = new QPushButton(label, this); layout->addWidget(result);
        connect(result, &QPushButton::clicked, this, std::move(action));
    }
    void failure(const std::vector<Diagnostic>& diagnostics) {
        QString message;
        for (const auto& diagnostic : diagnostics) message += QString::fromStdString(diagnostic.message) + '\n';
        QMessageBox::warning(this, tr("Cannot apply change"), message);
    }
    void apply(std::string description, const SctAuthoringSession::Command& command) {
        if (!state_) return;
        auto result = controller_.executeAuthoringCommand(state_->project.revision, std::move(description), command);
        if (!result) failure(result.diagnostics());
        refresh();
    }
    void refresh() {
        const auto sequence = sequences_->currentData(); const auto predicate = predicates_->currentData();
        state_ = controller_.authoringState();
        sequences_->blockSignals(true); predicates_->blockSignals(true); sequences_->clear(); predicates_->clear(); script_ = {};
        if (state_) for (const auto& script : state_->project.scripts)
            if (state_->project.find(script.baseline)->source.locator == locator_) script_ = script.id;
        if (state_) {
            for (const auto& item : state_->project.sequences) if (item.script == script_ && belongs(item.section))
                sequences_->addItem(QString::fromStdString(item.name), QVariant::fromValue<qulonglong>(item.id.value));
            for (const auto& item : state_->project.predicates) if (item.script == script_ && (!module_ || std::ranges::any_of(item.uses, [this](const auto& use) { return ownsInstruction(use.instruction); })))
                predicates_->addItem(QString::fromStdString(item.name), QVariant::fromValue<qulonglong>(item.id.value));
        }
        if (module_ && state_) {
            const auto* module = state_->project.find(*module_);
            if (module) setWindowTitle(tr("Module: %1 — %2").arg(QString::fromStdString(module->name), QString::fromStdString(state_->project.find(module->script)->name)));
            boundary_->clear();
            auto view = SctModuleAuthoring::inspect(*state_, *module_);
            if (view) for (const auto& item : view.value().boundary) {
                const auto target = item.destination && ownsInstruction(item.instruction) ? *item.destination : item.instruction;
                auto* row = new QListWidgetItem(QString::fromStdString(item.description) + tr(" [instruction %1]").arg(target.value()), boundary_);
                row->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(target.value()));
            }
            if (module) for (const auto& evidence : module->evidence) boundary_->addItem(QString::fromStdString(evidence.description));
        }
        if (sequences_->findData(sequence) >= 0) sequences_->setCurrentIndex(sequences_->findData(sequence));
        if (predicates_->findData(predicate) >= 0) predicates_->setCurrentIndex(predicates_->findData(predicate));
        sequences_->blockSignals(false); predicates_->blockSignals(false); refreshActions(); refreshPredicate();
    }
    void refreshActions() {
        views_.clear(); actions_->setRowCount(0);
        if (!state_ || sequences_->currentIndex() < 0) return;
        auto projected = SctSequenceAuthoring::actions(*state_, {sequences_->currentData().toULongLong()});
        if (!projected) { failure(projected.diagnostics()); return; }
        views_ = std::move(projected).takeValue(); actions_->setRowCount(static_cast<int>(views_.size()));
        const QStringList kinds{tr("Action"), tr("Branch"), tr("Switch"), tr("Jump"), tr("Call"), tr("Return"), tr("Wait"), tr("Preserved")};
        for (int row = 0; row < static_cast<int>(views_.size()); ++row) {
            const auto& view = views_[row]; QStringList targets;
            for (const auto target : view.targets) targets << QString::number(target.value());
            auto label = view.label;
            if (view.instruction.opcode == 0 || view.instruction.opcode == 3) for (const auto& p : view.instruction.fixedParameters)
                if (p.schemaIndex == 0) if (const auto* e = std::get_if<sct::SctCanonicalExpression>(&p.value)) label += ": " + SctInterpretation::condition(*state_, script_, *e);
            const QStringList values{QString::fromStdString(label), kinds[static_cast<int>(view.kind)],
                view.instruction.scheduledExpression ? text(*view.instruction.scheduledExpression) : tr("Immediate"),
                view.instruction.skipRefresh ? tr("Yes") : tr("No"), targets.join(", ")};
            for (int column = 0; column < values.size(); ++column) actions_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }
    const SctNamedPredicate* selectedPredicate() const {
        return state_ && predicates_->currentIndex() >= 0 ? state_->project.find(SctPredicateId{predicates_->currentData().toULongLong()}) : nullptr;
    }
    void refreshPredicate() {
        expression_->clear(); uses_->clear(); const auto* predicate = selectedPredicate(); if (!predicate) return;
        auto expression = SctSequenceAuthoring::predicate(*state_, predicate->id);
        if (!expression) { failure(expression.diagnostics()); return; }
        expression_->setText(QString::fromStdString(SctInterpretation::condition(*state_, script_, expression.value())));
        expression_->setToolTip(text(expression.value()));
        for (const auto& use : predicate->uses) uses_->addItem(tr("Instruction %1 · condition").arg(use.instruction.value()));
        uses_->setCurrentRow(0);
    }
    void promote() {
        if (module_) return;
        if (!state_ || !script_.valid()) return;
        auto working = SctAuthoringMaterializer::workingState(state_->project, state_->programs, script_);
        if (!working) { failure(working.diagnostics()); return; }
        QStringList labels; std::vector<sct::SctSectionId> ids;
        for (const auto& section : working.value().document->sections) if (std::holds_alternative<sct::SctScriptSectionContent>(section.content)
            && !std::ranges::any_of(state_->project.sequences, [&](const auto& seq) { return seq.script == script_ && seq.section == section.id; })) {
            labels << QString::fromStdString(section.nameBytes) + tr(" [%1]").arg(section.id.value()); ids.push_back(section.id);
        }
        if (ids.empty()) return;
        bool ok = false; const auto chosen = QInputDialog::getItem(this, tr("Name sequence"), tr("Section"), labels, 0, false, &ok); if (!ok) return;
        const auto id = ids[labels.indexOf(chosen)]; const auto name = QInputDialog::getText(this, tr("Name sequence"), tr("Name"), QLineEdit::Normal, chosen, &ok); if (!ok) return;
        apply("Name sequence", [=, this](const auto& state) { return SctSequenceAuthoring::promoteSequence(state, script_, id, name.toStdString()); });
    }
    void rename(bool condition) {
        if (!state_ || (condition ? predicates_ : sequences_)->currentIndex() < 0) return;
        auto* combo = condition ? predicates_ : sequences_; const auto id = combo->currentData().toULongLong(); bool ok = false;
        const auto name = QInputDialog::getText(this, tr("Rename definition"), tr("Name"), QLineEdit::Normal, combo->currentText(), &ok); if (!ok) return;
        apply("Rename semantic definition", [=](const auto& state) {
            auto next = state;
            if (condition) std::ranges::find(next.project.predicates, SctPredicateId{id}, &SctNamedPredicate::id)->name = name.toStdString();
            else std::ranges::find(next.project.sequences, SctSequenceId{id}, &SctAuthoredSequence::id)->name = name.toStdString();
            return Result<SctAuthoringState>::success(std::move(next));
        });
    }
    void remove(bool condition) {
        if (module_ && !condition) return;
        auto* combo = condition ? predicates_ : sequences_; if (!state_ || combo->currentIndex() < 0) return;
        const auto id = combo->currentData().toULongLong();
        apply("Remove semantic definition", [=](const auto& state) {
            auto next = state;
            if (condition) std::erase_if(next.project.predicates, [=](const auto& p) { return p.id.value == id; });
            else std::erase_if(next.project.sequences, [=](const auto& s) { return s.id.value == id; });
            return Result<SctAuthoringState>::success(std::move(next));
        });
    }
    void append() {
        if (!module_ || !state_ || sequences_->currentIndex() < 0) return;
        QStringList names; std::vector<std::uint16_t> opcodes;
        for (const auto& choice : SctEditSession::insertableOpcodes()) {
            names << QString::fromStdString(choice.mnemonic); opcodes.push_back(choice.opcode);
        }
        bool ok = false; const auto choice = QInputDialog::getItem(this, tr("Append action"), tr("Action"), names, 0, false, &ok);
        if (!ok || names.indexOf(choice) < 0) return;
        const auto opcode = opcodes[names.indexOf(choice)]; const auto module = *module_;
        const SctSequenceId sequence{sequences_->currentData().toULongLong()};
        apply("Append module action", [=](const auto& state) { return SctModuleAuthoring::appendAction(state, module, sequence, opcode); });
    }
    bool belongs(sct::SctSectionId section) const {
        if (!module_) return true;
        const auto* module = state_->project.find(*module_);
        return module && std::ranges::find(module->sections, section) != module->sections.end();
    }
    bool ownsInstruction(sct::SctInstructionId instruction) const {
        if (!state_) return false;
        for (const auto& seq : state_->project.sequences) if (seq.script == script_ && belongs(seq.section))
            for (const auto& action : seq.actions) if (action.instruction == instruction) return true;
        return false;
    }
    void parameter() {
        const auto row = actions_->currentRow(); if (!state_ || row < 0 || row >= static_cast<int>(views_.size())) return;
        const auto instruction = views_[row].instruction;
        QStringList names; std::vector<sct::SctDocumentParameter> parameters;
        for (const auto& parameter : instruction.fixedParameters)
            if (const auto* value = std::get_if<sct::SctCanonicalExpression>(&parameter.value);
                value && SctExpressionLanguage::project(*value).availability == SctExpressionTextAvailability::Editable) {
                names << tr("Parameter %1: %2").arg(parameter.schemaIndex).arg(text(*value)); parameters.push_back(parameter);
            }
        if (parameters.empty()) { QMessageBox::information(this, tr("Parameters"), tr("This action has no editable fixed expression. Use Go to action for its other fields.")); return; }
        bool ok = false; const auto choice = QInputDialog::getItem(this, tr("Edit parameter"), tr("Expression"), names, 0, false, &ok); if (!ok) return;
        const auto parameter = parameters[names.indexOf(choice)];
        const auto input = QInputDialog::getText(this, tr("Edit parameter"), tr("Expression"), QLineEdit::Normal, text(std::get<sct::SctCanonicalExpression>(parameter.value)), &ok); if (!ok) return;
        auto parsed = SctExpressionLanguage::parse(input.toStdString());
        if (!parsed.expression) { QMessageBox::warning(this, tr("Expression"), tr("Invalid expression.")); return; }
        const auto replacement = *parsed.expression; const auto script = script_;
        apply("Edit module action parameter", [=](const auto& state) {
            auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
            if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
            auto edited = SctSemanticOperationService::apply(*working.value().document,
                {{SctReplaceParameterValueOperation{{instruction.id, {parameter.schemaIndex, {}}}, replacement}}});
            if (!edited.succeeded()) return Result<SctAuthoringState>::failure({DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, "Parameter edit failed validation."});
            working.value().document = edited.document;
            auto project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working.value());
            if (!project) return Result<SctAuthoringState>::failure(project.diagnostics());
            auto next = state; next.project = std::move(project).takeValue(); return Result<SctAuthoringState>::success(std::move(next));
        });
    }
    void timing() {
        const auto row = actions_->currentRow(); if (!state_ || row < 0 || row >= static_cast<int>(views_.size())) return;
        const auto view = views_[row]; const SctSequenceId sequence{sequences_->currentData().toULongLong()};
        QDialog dialog(this); dialog.setWindowTitle(tr("Action timing")); auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(tr("Schedule expression (empty means immediate)"), &dialog));
        auto* schedule = new QLineEdit(view.instruction.scheduledExpression ? text(*view.instruction.scheduledExpression) : QString{}, &dialog); layout->addWidget(schedule);
        auto* skip = new QCheckBox(tr("Skip refresh"), &dialog); skip->setChecked(view.instruction.skipRefresh); layout->addWidget(skip);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        std::optional<sct::SctCanonicalExpression> expression;
        if (!schedule->text().trimmed().isEmpty()) {
            // An untouched schedule retains the exact imported encoding.
            if (view.instruction.scheduledExpression && schedule->text() == text(*view.instruction.scheduledExpression)) expression = view.instruction.scheduledExpression;
            else { auto parsed = SctExpressionLanguage::parse(schedule->text().toStdString());
                if (!parsed.succeeded()) { QMessageBox::warning(this, tr("Invalid schedule"), QString::fromStdString(parsed.issues.front().message)); return; }
                expression = parsed.expression;
            }
        }
        apply("Edit action timing", [=](const auto& state) { return SctSequenceAuthoring::setTiming(state, sequence, view.id, expression, skip->isChecked()); });
    }
    void nameCondition() {
        if (!state_ || !script_.valid()) return;
        auto working = SctAuthoringMaterializer::workingState(state_->project, state_->programs, script_);
        if (!working) { failure(working.diagnostics()); return; }
        QDialog dialog(this); dialog.setWindowTitle(tr("Name condition uses")); dialog.resize(740, 440);
        auto* layout = new QVBoxLayout(&dialog); layout->addWidget(new QLabel(tr("Select uses with the same exact expression."), &dialog));
        auto* list = new QListWidget(&dialog); list->setSelectionMode(QAbstractItemView::ExtendedSelection); layout->addWidget(list);
        std::vector<sct::SctParameterSite> sites;
        for (const auto& section : working.value().document->sections) if (const auto* body = std::get_if<sct::SctScriptSectionContent>(&section.content); body && belongs(section.id))
            for (const auto& instruction : body->instructions) if (instruction.opcode == 0) {
                const sct::SctParameterSite site{instruction.id, {0, {}}};
                if (std::ranges::any_of(state_->project.predicates, [&](const auto& p) { return p.script == script_ && std::ranges::find(p.uses, site) != p.uses.end(); })) continue;
                for (const auto& parameter : instruction.fixedParameters) if (parameter.schemaIndex == 0)
                    if (const auto* expression = std::get_if<sct::SctCanonicalExpression>(&parameter.value); expression && SctExpressionLanguage::project(*expression).availability == SctExpressionTextAvailability::Editable) {
                        sites.push_back(site); list->addItem(QString::fromStdString(section.nameBytes) + tr(" · %1 · ").arg(instruction.id.value()) + text(*expression));
                    }
            }
        auto* name = new QLineEdit(&dialog); name->setPlaceholderText(tr("Condition name")); layout->addWidget(name);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        std::vector<sct::SctParameterSite> uses; for (auto* item : list->selectedItems()) uses.push_back(sites[list->row(item)]);
        apply("Name condition", [&, this](const auto& state) { return SctSequenceAuthoring::namePredicate(state, script_, uses, name->text().toStdString()); });
    }
    void arrivalCase() {
        const auto row = actions_->currentRow(); if (!state_ || row < 0 || row >= static_cast<int>(views_.size())) return;
        const auto view = views_[row]; if (view.kind != SctSequenceActionKind::Switch) return;
        QStringList cases; std::vector<std::uint32_t> ordinals;
        for (std::uint32_t i = 0; i < view.instruction.repeatedParameterGroups.size(); ++i)
            for (const auto& parameter : view.instruction.repeatedParameterGroups[i].parameters) if (parameter.schemaIndex == 2)
                if (const auto* value = std::get_if<sct::SctEncodedWordValue>(&parameter.value); value && value->value != UINT32_MAX) {
                    cases << tr("Case %1 · value %2").arg(i + 1).arg(value->value); ordinals.push_back(i);
                }
        if (cases.empty()) return;
        bool ok = false; const auto selected = QInputDialog::getItem(this, tr("Arrival switch case"), tr("Existing case"), cases, 0, false, &ok); if (!ok) return;
        const auto ordinal = ordinals[cases.indexOf(selected)];
        const auto stem = QInputDialog::getText(this, tr("Arriving from"), tr("Previous location (for example me033b)"), QLineEdit::Normal, {}, &ok); if (!ok) return;
        apply("Select switch arrival location", [=, this](const auto& state) { return SctSequenceAuthoring::selectArrivalCase(state, script_, view.instruction.id, ordinal, stem.toStdString()); });
    }
    void editCondition() {
        const auto* definition = selectedPredicate(); if (!definition) return; const auto id = definition->id;
        bool ok = false; const auto value = QInputDialog::getMultiLineText(this, tr("Edit named condition"), tr("Expression (updates every bound use)"), expression_->text(), &ok);
        if (!ok || value == expression_->text()) return;
        auto parsed = SctExpressionLanguage::parse(value.toStdString());
        if (!parsed.succeeded()) { QMessageBox::warning(this, tr("Invalid condition"), QString::fromStdString(parsed.issues.front().message)); return; }
        apply("Edit named condition", [=](const auto& state) { return SctSequenceAuthoring::editPredicate(state, id, *parsed.expression); });
    }
    void arrival() {
        const auto* definition = selectedPredicate(); if (!definition) return; const auto id = definition->id;
        QStringList names;
        for (const auto& script : state_->project.scripts) {
            const auto stem = state_->project.find(script.baseline)->source.locator.path().stem().string();
            if (SctSequenceAuthoring::arrivalLocation(stem)) names << QString::fromStdString(stem);
        }
        names.removeDuplicates(); names.sort(); bool ok = false;
        const auto location = QInputDialog::getItem(this, tr("Arriving from"), tr("Previous location (me000a–me999j; a–j avoid ambiguous encodings)"), names, 0, true, &ok);
        if (!ok) return;
        apply("Select previous location", [=](const auto& state) { return SctSequenceAuthoring::selectArrival(state, id, location.toStdString()); });
    }
    SctDocumentController& controller_; AssetLocator locator_; std::function<void(SctNavigationTarget)> navigate_;
    std::shared_ptr<const SctAuthoringState> state_; SctScriptId script_;
    QComboBox* sequences_{}; QComboBox* predicates_{}; QTableWidget* actions_{}; QLineEdit* expression_{}; QListWidget* uses_{};
    std::vector<SctSequenceActionView> views_;
    std::optional<SctModuleId> module_; QListWidget* boundary_{};
};
}
void showSctSequenceEditor(SctDocumentController& controller, const core::AssetLocator& locator,
    std::function<void(core::SctNavigationTarget)> navigate, QWidget* parent) {
    Editor dialog(controller, locator, std::move(navigate), parent); dialog.exec();
}
void showSctModuleBrowser(SctDocumentController& controller, const AssetLocator& locator,
    std::function<void(SctNavigationTarget)> navigate, QWidget* parent) {
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("Script Modules")); dialog.resize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QObject::tr("Modules retain their parent script and its resource context. Shared helpers and caller preparation remain external."), &dialog));
    auto* modules = new QListWidget(&dialog); layout->addWidget(modules);
    auto* buttons = new QHBoxLayout; layout->addLayout(buttons);
    auto* extract = new QPushButton(QObject::tr("Extract sections…"), &dialog); buttons->addWidget(extract);
    auto* empty = new QPushButton(QObject::tr("New empty module…"), &dialog); buttons->addWidget(empty);
    auto* open = new QPushButton(QObject::tr("Open module"), &dialog); buttons->addWidget(open);
    auto* rename = new QPushButton(QObject::tr("Rename…"), &dialog); buttons->addWidget(rename);
    const auto scriptOf = [&]() {
        const auto state = controller.authoringState();
        if (state) for (const auto& script : state->project.scripts) if (state->project.find(script.baseline)->source.locator == locator) return script.id;
        return SctScriptId{};
    };
    const auto refresh = [&]() {
        modules->clear(); const auto state = controller.authoringState(); if (!state) return;
        for (const auto& module : state->project.modules) if (module.script == scriptOf()) {
            auto* row = new QListWidgetItem(QString::fromStdString(module.name), modules);
            row->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(module.id.value));
        }
    };
    const auto run = [&](const SctAuthoringSession::Command& command) {
        auto state = controller.authoringState(); if (!state) return;
        auto result = controller.executeAuthoringCommand(state->project.revision, "Edit script module", command);
        if (!result) { QString text; for (const auto& d : result.diagnostics()) text += QString::fromStdString(d.message) + '\n'; QMessageBox::warning(&dialog, QObject::tr("Module"), text); }
        refresh();
    };
    QObject::connect(open, &QPushButton::clicked, &dialog, [&]() {
        if (!modules->currentItem()) return;
        Editor editor(controller, locator, navigate, &dialog, SctModuleId{modules->currentItem()->data(Qt::UserRole).toULongLong()}); editor.exec(); refresh();
    });
    QObject::connect(modules, &QListWidget::itemDoubleClicked, open, [open] { open->click(); });
    QObject::connect(rename, &QPushButton::clicked, &dialog, [&]() {
        if (!modules->currentItem()) return;
        const SctModuleId id{modules->currentItem()->data(Qt::UserRole).toULongLong()}; bool ok = false;
        auto name = QInputDialog::getText(&dialog, QObject::tr("Rename module"), QObject::tr("Name"), QLineEdit::Normal, modules->currentItem()->text(), &ok);
        if (ok && !name.isEmpty()) run([=](const auto& state) { auto next = state; std::ranges::find(next.project.modules, id, &SctAuthoringModule::id)->name = name.toStdString(); return Result<SctAuthoringState>::success(std::move(next)); });
    });
    QObject::connect(empty, &QPushButton::clicked, &dialog, [&]() {
        bool ok = false; const auto name = QInputDialog::getText(&dialog, QObject::tr("New module"), QObject::tr("Name"), QLineEdit::Normal, {}, &ok);
        const auto script = scriptOf(); if (ok && !name.isEmpty()) run([=](const auto& state) { return SctModuleAuthoring::createEmpty(state, script, name.toStdString()); });
    });
    QObject::connect(extract, &QPushButton::clicked, &dialog, [&]() {
        auto state = controller.authoringState(); const auto script = scriptOf(); if (!state || !script.valid()) return;
        auto working = SctAuthoringMaterializer::workingState(state->project, state->programs, script); if (!working) return;
        QDialog selection(&dialog); selection.setWindowTitle(QObject::tr("Extract module")); auto* content = new QVBoxLayout(&selection);
        auto* name = new QLineEdit(&selection); name->setPlaceholderText(QObject::tr("Module name")); content->addWidget(name);
        auto* sections = new QListWidget(&selection); sections->setSelectionMode(QAbstractItemView::ExtendedSelection); content->addWidget(sections);
        for (const auto& section : working.value().document->sections) if (std::holds_alternative<sct::SctScriptSectionContent>(section.content)) {
            bool owned = false; for (const auto& m : state->project.modules) if (m.script == script && std::ranges::find(m.sections, section.id) != m.sections.end()) owned = true;
            if (!owned) { auto* row = new QListWidgetItem(QString::fromStdString(section.nameBytes), sections); row->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(section.id.value())); }
        }
        auto* done = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &selection); content->addWidget(done);
        QObject::connect(done, &QDialogButtonBox::accepted, &selection, &QDialog::accept); QObject::connect(done, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
        if (selection.exec() != QDialog::Accepted || name->text().isEmpty()) return;
        std::vector<sct::SctSectionId> chosen; for (const auto* item : sections->selectedItems()) chosen.push_back(sct::SctSectionId{item->data(Qt::UserRole).toULongLong()});
        const auto label = name->text().toStdString(); run([=](const auto& current) { return SctModuleAuthoring::extract(current, script, chosen, label); });
    });
    refresh(); dialog.exec();
}

}
