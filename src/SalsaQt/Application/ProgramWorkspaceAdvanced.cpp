// Quarantined advanced program adapter. These reachable full-value editing,
// explicit promotion and native-output preview paths have no complete generic
// component replacements yet. Keep them out of shell composition; do not remove
// their capabilities merely because the older dialogs look similar to new ones.
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
} // namespace
namespace {
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
} // namespace salsa::qt
