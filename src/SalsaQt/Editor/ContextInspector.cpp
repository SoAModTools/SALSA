#include "Editor/ContextInspector.h"
#include "Editor/EditorSessionPresenter.h"
#include "Editor/InstructionInspector.h"
#include "Editor/TextEditorComponent.h"
#include <QAction>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
namespace salsa::qt {
ContextInspector::ContextInspector(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_);
    instructions_ = new InstructionInspector(session, pages_);
    pages_->addWidget(instructions_);
    message_ = new QWidget(pages_);
    auto *messageLayout = new QVBoxLayout(message_);
    name_ = new QLabel(message_);
    name_->setTextFormat(Qt::PlainText);
    name_->setWordWrap(true);
    language_ = new QLabel(message_);
    language_->setTextFormat(Qt::PlainText);
    language_->setWordWrap(true);
    content_ = new QPlainTextEdit(message_);
    content_->setReadOnly(true);
    messageLayout->addWidget(name_);
    messageLayout->addWidget(language_);
    messageLayout->addWidget(content_, 1);
    editText_ = new QAction(tr("Edit string…"), this);
    auto *edit = new QPushButton(editText_->text(), message_);
    messageLayout->addWidget(edit);
    connect(edit, &QPushButton::clicked, editText_, &QAction::trigger);
    connect(editText_, &QAction::changed, edit, [this, edit] { edit->setEnabled(editText_->isEnabled()); });
    connect(editText_, &QAction::triggered, this, [this] { editText(); });
    pages_->addWidget(message_);
    createText_ = new QAction(tr("New string…"), this);
    deleteText_ = new QAction(tr("Delete string"), this);
    connect(createText_, &QAction::triggered, this, [this] {
        if (!prepareToLeave()) return;
        ensureEditor();
        editor_->release();
        pages_->setCurrentWidget(editing_);
        if (showRequested) showRequested();
        editor_->createAction()->trigger();
    });
    connect(deleteText_, &QAction::triggered, this, [this] {
        if (!prepareToLeave()) return;
        const auto &i = session_->inspection();
        if (i && i->text) session_->execute(tr("Delete string"), core::editor::DeleteItem{*i->text});
    });
    connect(session, &EditorSessionPresenter::inspectionChanged, this, [this] { refresh(); });
    connect(session, &EditorSessionPresenter::availabilityChanged, this, [this] { availability(); });
    connect(session, &EditorSessionPresenter::sessionChanged, this, [this] {
        if (editor_) editor_->release();
        pages_->setCurrentWidget(instructions_);
        refresh();
    });
    refresh();
}
QAction *ContextInspector::editParameterAction() const { return instructions_->editParameterAction(); }
bool ContextInspector::prepareToLeave() {
    return !session_->busy() && (!editor_ || editor_->prepareToLeave());
}
void ContextInspector::ensureEditor() {
    if (editor_) return;
    editing_ = new QWidget(pages_);
    auto *layout = new QVBoxLayout(editing_);
    auto *back = new QPushButton(tr("Back to inspector"), editing_);
    layout->addWidget(back);
    editor_ = new TextEditorComponent(session_, editing_);
    layout->addWidget(editor_, 1);
    pages_->addWidget(editing_);
    connect(back, &QPushButton::clicked, this, [this] {
        if (!prepareToLeave()) return;
        editor_->release();
        pages_->setCurrentWidget(instructions_);
        refresh();
    });
}
void ContextInspector::editText() {
    const auto &i = session_->inspection();
    if (!i || !i->text || session_->busy()) return;
    const auto id = *i->text;
    const auto revision = session_->revision();
    if (!prepareToLeave() || revision != session_->revision() ||
        !session_->inspection() || session_->inspection()->text != id) return;
    ensureEditor();
    editor_->release();
    pages_->setCurrentWidget(editing_);
    editor_->open(id);
    if (showRequested) showRequested();
}
void ContextInspector::refresh() {
    const auto &i = session_->inspection();
    if (i && i->message) {
        name_->setText(QString::fromStdString(i->message->name));
        QStringList languages;
        for (const auto &name : i->message->languages) languages << QString::fromStdString(name);
        language_->setText(!i->text ? tr("Selected localization unavailable")
                         : languages.empty() ? tr("Localization: unassigned")
                                             : tr("Localization: %1").arg(languages.join(", ")));
        content_->setPlainText(QString::fromStdString(i->message->content));
    }
    // An explicitly opened draft remains pinned until Apply/Discard and Back.
    if (!editing_ || pages_->currentWidget() != editing_)
        pages_->setCurrentWidget(i && i->message ? message_ : instructions_);
    availability();
}
void ContextInspector::availability() {
    const auto &i = session_->inspection();
    const bool idle = !session_->busy();
    editText_->setEnabled(idle && i && i->text.has_value());
    createText_->setEnabled(idle && i && i->script.valid());
    deleteText_->setEnabled(idle && i && i->message && i->text);
}
} // namespace salsa::qt
