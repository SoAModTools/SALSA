#include "Editor/TextEditorComponent.h"
#include "Sct/SctMessageEditorWidget.h"
#include <QAction>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTextBrowser>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace ed = core::editor;
TextEditorComponent::TextEditorComponent(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session) {
    auto *layout = new QVBoxLayout(this);
    status_ = new QLabel(tr("Select a string or text reference"), this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *bar = new QToolBar(this);
    layout->addWidget(bar);
    apply_ = bar->addAction(tr("Apply"));
    discard_ = bar->addAction(tr("Discard"));
    create_ = bar->addAction(tr("New string…"));
    remove_ = bar->addAction(tr("Delete string"));
    editor_ = new SctMessageEditorWidget(this);
    layout->addWidget(editor_);
    preview_ = new QTextBrowser(this);
    preview_->setMaximumHeight(130);
    layout->addWidget(preview_);
    connect(apply_, &QAction::triggered, this, [this] { apply(); });
    connect(discard_, &QAction::triggered, this, [this] { discard(); });
    connect(create_, &QAction::triggered, this, [this] { create(); });
    connect(remove_, &QAction::triggered, this, [this] {
        if (editor_->flushPending() && draft_ && !dirty_)
            session_->execute(tr("Delete string"), ed::DeleteItem{draft_->id});
    });
    connect(session_, &EditorSessionPresenter::inspectionChanged, this, [this] { follow(); });
    connect(session_, &EditorSessionPresenter::sessionChanged, this, [this] {
        ++generation_;
        draft_.reset();
        dirty_ = false;
        applying_ = false;
        stale_ = false;
        editor_->clear();
        preview_->clear();
    });
    connect(session_, &EditorSessionPresenter::changed, this, [this](const ed::Change &change) {
        if (!draft_ || !editor_->flushPending())
            return;
        bool affected = std::ranges::find(change.texts, draft_->id) != change.texts.end();
        if (applying_ && affected) {
            dirty_ = false;
            applying_ = false;
            stale_ = false;
            draft_->revision = change.status.current;
            status_->setText(tr("Text saved to project history"));
        } else if (dirty_ && affected) {
            stale_ = true;
            status_->setText(tr("This string changed. Your draft is retained; discard to reload."));
            return;
        } else if (!stale_)
            draft_->revision = change.status.current;
        if (affected && !dirty_)
            open(draft_->id);
    });
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, [this] {
        editor_->setEnabled(!session_->busy());
        apply_->setEnabled(!session_->busy());
        create_->setEnabled(!session_->busy());
        remove_->setEnabled(!session_->busy());
        if (!session_->busy())
            applying_ = false;
    });
}
void TextEditorComponent::follow() {
    if (!session_->inspection())
        return;
    if (!editor_->flushPending())
        return;
    const auto &i = *session_->inspection();
    std::optional<core::SctTextResourceId> text = i.text;
    if (!text)
        for (const auto &p : i.parameters)
            if (p.target)
                if (auto id = std::get_if<core::SctTextResourceId>(&*p.target)) {
                    text = *id;
                    break;
                }
    if (dirty_) {
        if (text && text != draft_->id)
            status_->setText(tr("Draft pinned to this string. Apply or discard before following another selection."));
        return;
    }
    if (!text) {
        ++generation_;
        draft_.reset();
        editor_->clear();
        preview_->clear();
        status_->setText(tr("Select a string or text reference"));
        return;
    }
    if (text && (!draft_ || draft_->id != *text))
        open(*text);
}
void TextEditorComponent::open(core::SctTextResourceId id) {
    if (dirty_ || !session_->backend())
        return;
    auto backend = session_->backend();
    auto revision = session_->revision();
    auto generation = ++generation_;
    auto *watcher = new QFutureWatcher<core::Result<ed::TextDraft>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, revision, generation] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (generation != generation_ || revision != session_->revision() || dirty_)
            return;
        if (!result) {
            status_->setText(QString::fromStdString(result.diagnostics().front().message));
            draft_.reset();
            editor_->clear();
            return;
        }
        editor_->clear();
        draft_ = std::move(result).takeValue();
        stale_ = false;
        status_->setText(QString::fromStdString(draft_->name));
        editor_->bindValue(draft_->value, draft_->kind,
                           draft_->footer ? spice::sct::SctTextStorage::Footer
                                          : spice::sct::SctTextStorage::IndexedSection,
                           [this](auto value) {
                               if (!draft_)
                                   return false;
                               draft_->value = std::move(value);
                               dirty_ = true;
                               status_->setText(tr("Unapplied text draft"));
                               preview();
                               return true;
                           });
        preview();
    });
    watcher->setFuture(QtConcurrent::run([backend, revision, id] { return backend->text(revision, id); }));
}
void TextEditorComponent::apply() {
    if (!editor_->flushPending() || !dirty_ || !draft_ || session_->busy())
        return;
    if (stale_) {
        status_->setText(tr("The string changed after this draft was made. The draft is retained; discard to reload."));
        return;
    }
    applying_ = true;
    session_->execute(tr("Apply text draft"), ed::WriteText{*draft_}, draft_->revision);
}
void TextEditorComponent::discard() {
    ++generation_;
    auto id = draft_ ? std::optional(draft_->id) : std::nullopt;
    dirty_ = false;
    applying_ = false;
    stale_ = false;
    editor_->clear();
    draft_.reset();
    if (id)
        open(*id);
}
bool TextEditorComponent::prepareToLeave() {
    if (!editor_->flushPending())
        return false;
    if (!dirty_)
        return true;
    auto answer = QMessageBox::question(this, tr("Text draft"),
                                        tr("Discard the unapplied text draft? Choose Cancel to return and Apply it."),
                                        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard)
        return false;
    discard();
    return true;
}
void TextEditorComponent::create() {
    if (!session_->inspection() || session_->busy())
        return;
    bool ok = false;
    auto name =
        QInputDialog::getText(this, tr("New string"), tr("Section name or footer label"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty())
        return;
    auto storage =
        QInputDialog::getItem(this, tr("Text storage"), tr("Storage"),
                              {tr("Indexed dialogue"), tr("Footer dialogue"), tr("Footer plain text")}, 0, false, &ok);
    if (!ok)
        return;
    ed::CreateText input;
    input.script = session_->inspection()->script;
    input.name = name.toStdString();
    input.footer = storage != tr("Indexed dialogue");
    if (storage == tr("Footer plain text")) {
        input.kind = spice::sct::SctTextKind::PlainString;
        input.value = spice::sct::SctPlainText{""};
    } else {
        core::SctMessageDraft draft;
        draft.completion = core::SctMessageClose{};
        auto value = core::SctMessageAuthoringProfile::materialize(draft);
        if (!value.succeeded())
            return;
        input.value = *value.message;
    }
    session_->execute(tr("Create string"), std::move(input));
}
void TextEditorComponent::preview() {
    if (!draft_)
        return;
    QString html;
    if (auto *plain = std::get_if<spice::sct::SctPlainText>(&draft_->value))
        html = QString::fromStdString(plain->utf8).toHtmlEscaped();
    else if (auto *message = std::get_if<spice::sct::SctMessage>(&draft_->value)) {
        auto projection = core::SctMessageAuthoringProfile::project(*message);
        if (projection.draft) {
            if (projection.draft->headerUtf8)
                html = "<b>" + QString::fromStdString(*projection.draft->headerUtf8).toHtmlEscaped() + "</b><br>";
            for (const auto &run : projection.draft->body) {
                QString style;
                if (run.style.doubleScale)
                    style += "font-size:200%;";
                if (run.style.color) {
                    const auto c = *run.style.color;
                    style += QString("color:rgb(%1,%2,%3);").arg(c.red).arg(c.green).arg(c.blue);
                }
                html += "<span style='" + style + "'>" +
                        QString::fromStdString(run.utf8).toHtmlEscaped().replace("\n", "<br>") + "</span>";
            }
        } else
            html = tr("This message contains controls outside the formatted preview profile.");
    }
    preview_->setHtml(html);
}
} // namespace salsa::qt
