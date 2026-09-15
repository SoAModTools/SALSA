#include "Editor/ScriptEditingActions.h"
#include "Editor/EditorQueryWidget.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "Sct/SctScptEditorWidget.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace ed = core::editor;
std::optional<spice::sct::SctCanonicalExpression> editExpression(QWidget *parent,
                                                                 const spice::sct::SctCanonicalExpression &value) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("SCPT expression"));
    dialog.resize(740, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *editor = new SctScptEditorWidget(&dialog);
    layout->addWidget(editor);
    auto draft = value;
    editor->bindValue(value, [&](const auto &next) {
        draft = next;
        return true;
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (editor->prepareToClear())
            dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? std::optional(draft) : std::nullopt;
}
ScriptEditingActions::ScriptEditingActions(EditorSessionPresenter *session, QWidget *host)
    : QObject(host), session_(session), host_(host) {
    insert_ = new QAction(tr("Insert instruction…"), this);
    insert_->setShortcut(QKeySequence(Qt::Key_Insert));
    structure_ = new QAction(tr("Insert If / loop / Switch…"), this);
    edit_ = new QAction(tr("Edit condition / branches…"), this);
    timing_ = new QAction(tr("Instruction timing…"), this);
    connect(insert_, &QAction::triggered, this, [this] { insert(); });
    connect(structure_, &QAction::triggered, this, [this] { createStructure(); });
    connect(edit_, &QAction::triggered, this, [this] { edit(); });
    connect(timing_, &QAction::triggered, this, [this] { timing(); });
    auto availability = [this] {
        const auto &i = session_->inspection();
        bool ready = i && !session_->busy();
        insert_->setEnabled(ready);
        structure_->setEnabled(ready);
        edit_->setEnabled(ready && (i->condition || i->structured));
        timing_->setEnabled(ready && i->instruction);
    };
    connect(session_, &EditorSessionPresenter::inspectionChanged, this, availability);
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, availability);
    availability();
}
void ScriptEditingActions::insert() {
    if (!session_->selection())
        return;
    auto anchor = *session_->selection();
    auto revision = session_->revision();
    auto catalog = session_->backend()->opcodes();
    QStringList choices;
    for (const auto &op : catalog)
        choices << QString("%1 — %2").arg(op.code).arg(QString::fromStdString(op.name));
    bool ok = false;
    auto choice = QInputDialog::getItem(host_, tr("Insert instruction"), tr("Opcode"), choices, 0, false, &ok);
    if (!ok)
        return;
    auto at = choices.indexOf(choice);
    if (at >= 0)
        session_->execute(tr("Insert instruction"), ed::InsertOpcode{anchor, catalog[at].code}, revision);
}
void ScriptEditingActions::createStructure() {
    if (!session_->selection())
        return;
    auto anchor = *session_->selection();
    auto revision = session_->revision();
    bool ok = false;
    auto kind = QInputDialog::getItem(host_, tr("Insert structure"), tr("Construct"),
                                      {"If", "While", "Do While", "For", "Switch", "Break", "Continue"}, 0, false, &ok);
    if (!ok)
        return;
    if (kind == "Break" || kind == "Continue") {
        ed::BrowseRequest query;
        query.kind = ed::BrowseKind::Actions;
        if (session_->inspection())
            query.script = session_->inspection()->script;
        auto target = pickReference(session_, query, host_);
        if (target && std::holds_alternative<core::SctActionId>(*target))
            session_->execute(tr("Insert loop/switch transfer"),
                              ed::InsertTransfer{anchor, std::get<core::SctActionId>(*target), kind == "Continue"},
                              revision);
        return;
    }
    auto condition = editExpression(host_, spice::sct::SctExpressionFactory::encodedDecimalLiteral(1));
    if (condition)
        session_->execute(tr("Insert structure"), ed::InsertStructure{anchor, kind.toStdString(), *condition},
                          revision);
}
void ScriptEditingActions::edit() {
    const auto &i = session_->inspection();
    if (!i)
        return;
    auto captured = *i;
    if (std::holds_alternative<core::program::ConditionId>(captured.subject) && captured.condition) {
        auto value = editExpression(host_, *captured.condition);
        if (value)
            session_->execute(tr("Edit condition"), ed::SetExpression{captured.subject, {}, *value}, captured.revision);
        return;
    }
    if (!captured.structured || !std::holds_alternative<core::SctActionId>(captured.subject))
        return;
    auto backend = session_->backend();
    auto *watcher = new QFutureWatcher<core::Result<ed::StructureDraft>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, captured] {
        auto r = watcher->result();
        watcher->deleteLater();
        if (r && session_->revision() == captured.revision)
            arms(std::move(r).takeValue());
    });
    watcher->setFuture(QtConcurrent::run([backend, captured] {
        return backend->structure(captured.revision, std::get<core::SctActionId>(captured.subject));
    }));
}
void ScriptEditingActions::timing() {
    if (!session_->inspection())
        return;
    auto i = *session_->inspection();
    QDialog dialog(host_);
    dialog.setWindowTitle(tr("Instruction timing"));
    auto *layout = new QFormLayout(&dialog);
    auto *skip = new QCheckBox(tr("Skip refresh"), &dialog);
    skip->setChecked(i.skipRefresh);
    layout->addRow(skip);
    auto *scheduled = new QCheckBox(tr("Scheduled"), &dialog);
    scheduled->setChecked(i.schedule.has_value());
    layout->addRow(scheduled);
    auto value = i.schedule.value_or(spice::sct::SctExpressionFactory::encodedDecimalLiteral(0));
    auto *edit = new QPushButton(tr("Edit schedule expression…"), &dialog);
    layout->addRow(edit);
    connect(edit, &QPushButton::clicked, &dialog, [&] {
        auto next = editExpression(&dialog, value);
        if (next)
            value = *next;
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        session_->execute(tr("Edit instruction timing"),
                          ed::SetTiming{std::get<core::SctActionId>(i.subject), skip->isChecked(),
                                        scheduled->isChecked() ? std::optional(value) : std::nullopt},
                          i.revision);
}
void ScriptEditingActions::arms(ed::StructureDraft draft) {
    if (draft.arms.empty()) {
        QMessageBox::information(
            host_, tr("Loop controls"),
            tr("Expand the loop to edit its Condition and insert instructions in Initialize, Body or Update."));
        return;
    }
    QDialog dialog(host_);
    dialog.setWindowTitle(tr("Branches and cases"));
    dialog.resize(670, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(0, 3, &dialog);
    table->setHorizontalHeaderLabels({tr("Condition / labels"), tr("Else / default"), tr("Fall through")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(table);
    auto render = [&] {
        table->setRowCount(static_cast<int>(draft.arms.size()));
        for (int n = 0; n < table->rowCount(); ++n) {
            const auto &a = draft.arms[n];
            QString text;
            if (draft.switchCases) {
                QStringList labels;
                for (auto x : a.labels)
                    labels << QString::number(x);
                text = labels.join(", ");
            } else if (a.condition)
                text = QString::fromStdString(core::SctExpressionLanguage::project(*a.condition).text);
            table->setItem(n, 0, new QTableWidgetItem(text));
            auto *fallback = new QTableWidgetItem;
            fallback->setCheckState((draft.switchCases ? a.includesDefault : !a.condition) ? Qt::Checked
                                                                                           : Qt::Unchecked);
            table->setItem(n, 1, fallback);
            auto *fall = new QTableWidgetItem;
            fall->setCheckState(a.fallthrough ? Qt::Checked : Qt::Unchecked);
            table->setItem(n, 2, fall);
        }
    };
    auto capture = [&]() -> bool {
        for (int n = 0; n < table->rowCount(); ++n) {
            auto &a = draft.arms[n];
            auto text = table->item(n, 0)->text();
            bool fallback = table->item(n, 1)->checkState() == Qt::Checked;
            if (draft.switchCases) {
                a.includesDefault = fallback;
                a.fallthrough = table->item(n, 2)->checkState() == Qt::Checked;
                a.labels.clear();
                for (auto part : text.split(',', Qt::SkipEmptyParts)) {
                    bool ok = false;
                    auto value = part.trimmed().toInt(&ok);
                    if (!ok)
                        return false;
                    a.labels.push_back(value);
                }
            } else if (fallback)
                a.condition.reset();
            else {
                if (a.condition && core::SctExpressionLanguage::project(*a.condition).text == text.toStdString())
                    continue;
                auto value = core::SctExpressionLanguage::parse(text.toStdString());
                if (!value.succeeded())
                    return false;
                a.condition = *value.expression;
            }
        }
        return true;
    };
    auto button = [&](QString label, auto callback) {
        auto *b = new QPushButton(label, &dialog);
        layout->addWidget(b);
        connect(b, &QPushButton::clicked, &dialog, callback);
    };
    button(tr("Add branch / case"), [&] {
        if (!capture())
            return;
        ed::ArmDraft a;
        if (draft.switchCases)
            a.labels = {0};
        else
            a.condition = spice::sct::SctExpressionFactory::encodedDecimalLiteral(1);
        draft.arms.push_back(a);
        render();
    });
    button(tr("Remove selected branch"), [&] {
        int n = table->currentRow();
        if (n < 0 || !capture())
            return;
        draft.arms.erase(draft.arms.begin() + n);
        render();
    });
    button(tr("Move selected branch up"), [&] {
        int n = table->currentRow();
        if (n <= 0 || !capture())
            return;
        std::swap(draft.arms[n], draft.arms[n - 1]);
        render();
        table->selectRow(n - 1);
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (capture())
            dialog.accept();
        else
            QMessageBox::warning(&dialog, tr("Invalid branch"), tr("Check the expression or integer case labels."));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    render();
    if (dialog.exec() == QDialog::Accepted)
        session_->execute(tr("Edit branches"), ed::WriteArms{draft}, draft.revision);
}
} // namespace salsa::qt
