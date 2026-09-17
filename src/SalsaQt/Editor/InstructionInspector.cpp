#include "Editor/InstructionInspector.h"
#include "Editor/EditorQueryWidget.h"
#include "Editor/EditorSessionPresenter.h"
#include "Editor/ScriptEditingActions.h"
#include "Editor/SupportingSequenceEditor.h"
#include "Sct/SctMessageEditorWidget.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QAction>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
namespace salsa::qt {
InstructionInspector::InstructionInspector(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session) {
    auto *layout = new QVBoxLayout(this);
    title_ = new QLabel(this);
    title_->setWordWrap(true);
    layout->addWidget(title_);
    description_ = new QLabel(this);
    description_->setWordWrap(true);
    layout->addWidget(description_);
    parameters_ = new QTableWidget(0, 2, this);
    parameters_->setHorizontalHeaderLabels({tr("Parameter"), tr("Value")});
    parameters_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameters_->setSelectionMode(QAbstractItemView::SingleSelection);
    parameters_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(parameters_);
    edit_ = new QAction(tr("Edit parameter…"), this);
    auto *toolbar = new QToolBar(this);
    toolbar->addAction(edit_);
    layout->addWidget(toolbar);
    layout->addWidget(new SupportingSequenceEditor(session, this), 1);
    follow_ = toolbar->addAction(tr("Follow reference"));
    connect(follow_, &QAction::triggered, this, [this] {
        const auto &i = session_->inspection();
        auto n = parameters_->currentRow();
        if (i && n >= 0 && n < static_cast<int>(i->parameters.size()) && i->parameters[n].target)
            session_->navigate(*i->parameters[n].target);
    });
    connect(edit_, &QAction::triggered, this, [this] { editParameter(); });
    connect(parameters_, &QTableWidget::itemDoubleClicked, this, [this] { editParameter(); });
    connect(parameters_, &QTableWidget::itemSelectionChanged, this, [this] { availability(); });
    connect(session_, &EditorSessionPresenter::inspectionChanged, this, [this] { refresh(); });
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, [this] { availability(); });
    refresh();
}
void InstructionInspector::refresh() {
    const auto &inspection = session_->inspection();
    const int selected = parameters_->currentRow();
    parameters_->setRowCount(0);
    title_->setText(inspection ? QString::fromStdString(inspection->title) : tr("Select an instruction"));
    description_->setText(inspection ? QString::fromStdString(inspection->description + "\n" + inspection->evidence)
                                     : QString{});
    if (inspection) {
        parameters_->setRowCount(static_cast<int>(inspection->parameters.size()));
        for (int i = 0; i < parameters_->rowCount(); ++i) {
            const auto &p = inspection->parameters[i];
            parameters_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(p.name)));
            auto *value = new QTableWidgetItem(QString::fromStdString(p.value));
            value->setToolTip(QString::fromStdString(p.unavailableReason));
            parameters_->setItem(i, 1, value);
        }
        if (parameters_->rowCount())
            parameters_->selectRow(std::clamp(selected, 0, parameters_->rowCount() - 1));
    }
    parameters_->setVisible(parameters_->rowCount() != 0);
    availability();
}
void InstructionInspector::availability() {
    const auto &i = session_->inspection();
    int row = parameters_->currentRow();
    bool enabled = i && row >= 0 && row < static_cast<int>(i->parameters.size()) && !session_->busy() &&
                   i->parameters[row].kind != core::editor::ParameterKind::ReadOnly;
    edit_->setEnabled(enabled);
    follow_->setEnabled(i && row >= 0 && row < static_cast<int>(i->parameters.size()) &&
                        i->parameters[row].target.has_value() && i->parameters[row].kind != core::editor::ParameterKind::FooterText);
    edit_->setToolTip(enabled ? tr("Apply one parameter edit through project history")
                      : i && row >= 0 && row < static_cast<int>(i->parameters.size()) && !session_->busy()
                          ? QString::fromStdString(i->parameters[row].unavailableReason)
                          : tr("Select an editable parameter when the editor is idle"));
}
void InstructionInspector::editParameter() {
    if (!edit_->isEnabled() || !session_->inspection())
        return;
    // Own only this parameter draft. Cancelling leaves the committed value intact.
    const auto p = session_->inspection()->parameters.at(parameters_->currentRow());
    const auto revision = session_->revision();
    const auto subject = session_->inspection()->subject;
    if (p.expression) {
        auto value = editExpression(this, *p.expression);
        if (value)
            session_->execute(tr("Edit SCPT parameter"), core::editor::SetExpression{subject, p.key, *value}, revision);
        return;
    }
    if (p.kind == core::editor::ParameterKind::FooterText) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Edit string parameter: %1").arg(QString::fromStdString(p.name)));
        auto *layout = new QVBoxLayout(&dialog);
        auto *editor = new SctMessageEditorWidget(&dialog);
        layout->addWidget(editor);
        auto draft = p.textValue.value_or(p.textKind == spice::sct::SctTextKind::PlainString
            ? spice::sct::SctTextValue{spice::sct::SctPlainText{}}
            : spice::sct::SctTextValue{spice::sct::SctMessage{}});
        if (!p.textValue && p.textKind == spice::sct::SctTextKind::SctString) {
            core::SctMessageDraft empty;
            empty.completion = core::SctMessageClose{};
            auto value = core::SctMessageAuthoringProfile::materialize(empty);
            if (value.succeeded()) draft = *value.message;
        }
        editor->bindValue(draft, p.textKind, spice::sct::SctTextStorage::Footer,
                          [&](auto value) { draft = std::move(value); return true; });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            if (editor->flushPending()) dialog.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(650, 420);
        if (dialog.exec() == QDialog::Accepted)
            session_->execute(tr("Edit string parameter"), core::editor::WriteStringParameter{
                std::get<core::SctActionId>(subject), p.key, std::move(draft)}, revision);
        return;
    }
    if (p.kind == core::editor::ParameterKind::ActionTarget || p.kind == core::editor::ParameterKind::IndexedText) {
        core::editor::BrowseRequest query;
        query.kind = p.kind == core::editor::ParameterKind::ActionTarget ? core::editor::BrowseKind::Actions
                                                                         : core::editor::BrowseKind::Texts;
        query.script = session_->inspection()->script;
        auto target = pickReference(session_, query, this);
        if (target)
            session_->execute(tr("Bind reference"),
                              core::editor::BindReference{std::get<core::SctActionId>(subject), p.key, *target},
                              revision);
        return;
    }
    bool ok = false;
    auto text = QInputDialog::getText(this, tr("Edit parameter"), QString::fromStdString(p.name), QLineEdit::Normal,
                                      QString::fromStdString(p.value), &ok);
    if (ok && revision == session_->revision() && session_->inspection() &&
        session_->inspection()->subject == subject && text.toStdString() != p.value)
        session_->replaceParameter(p.key, text.toStdString());
}
} // namespace salsa::qt
