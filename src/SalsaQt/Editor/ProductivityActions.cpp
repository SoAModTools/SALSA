#include "Editor/ProductivityActions.h"
#include "Editor/EditorQueryWidget.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace ed = core::editor;
namespace {
constexpr auto Mime = "application/x-salsa-ir-fragment";
std::vector<std::byte> bytes(const QByteArray &b) {
    auto data = std::as_bytes(std::span(b.constData(), static_cast<std::size_t>(b.size())));
    return {data.begin(), data.end()};
}
template <class Work, class Done> void background(QObject *owner, Work work, Done done) {
    using R = decltype(work());
    auto *w = new QFutureWatcher<R>(owner);
    QObject::connect(w, &QFutureWatcherBase::finished, owner, [w, done = std::move(done)]() mutable {
        auto r = w->result();
        w->deleteLater();
        done(std::move(r));
    });
    w->setFuture(QtConcurrent::run(std::move(work)));
}
} // namespace
ProductivityActions::ProductivityActions(EditorSessionPresenter *s, QWidget *host)
    : QObject(host), session_(s), host_(host) {
    auto action = [&](QString label, QKeySequence shortcut, std::function<void()> run) {
        auto *a = new QAction(label, this);
        a->setShortcut(shortcut);
        connect(a, &QAction::triggered, this, std::move(run));
        actions_.push_back(a);
        return a;
    };
    copy_ = action(tr("Copy selection"), QKeySequence::Copy, [this] { copy(); });
    cut_ = action(tr("Cut selection"), QKeySequence::Cut, [this] { copy(true); });
    paste_ = action(tr("Paste…"), QKeySequence::Paste,
                    [this] { paste(bytes(QApplication::clipboard()->mimeData()->data(Mime))); });
    duplicate_ =
        action(tr("Duplicate selection"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { copy(false, true); });
    remove_ = action(tr("Delete selection"), QKeySequence::Delete, [this] {
        session_->execute(tr("Delete selection"), ed::DeleteSelection{session_->selectionSet()});
    });
    move_ = action(tr("Move selection to…"), {}, [this] { move(); });
    snippets_ = action(tr("Snippet library…"), {}, [this] { snippets(); });
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, [this] { refresh(); });
    connect(session_, &EditorSessionPresenter::inspectionChanged, this, [this] { refresh(); });
    connect(session_, &EditorSessionPresenter::sessionChanged, this, [this] { refresh(); });
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] { refresh(); });
    refresh();
}
void ProductivityActions::refresh() {
    bool ready = session_->backend() && !session_->busy();
    bool selected = ready && !session_->selectionSet().empty();
    for (auto *a : {copy_, cut_, duplicate_, remove_, move_})
        a->setEnabled(selected);
    paste_->setEnabled(ready && session_->selection() &&
                       QApplication::clipboard()->mimeData()->hasFormat(Mime));
    snippets_->setEnabled(ready);
}
void ProductivityActions::copy(bool cut, bool duplicate) {
    auto b = session_->backend();
    if (!b || session_->selectionSet().empty())
        return;
    auto r = session_->revision();
    auto selection = session_->selectionSet();
    auto anchor = session_->selection();
    background(
        this, [b, r, selection] { return b->copy(r, {selection}); },
        [this, r, selection, anchor, cut, duplicate](auto result) {
            if (r != session_->revision())
                return;
            if (!result) {
                emit session_->diagnostic(QString::fromStdString(result.diagnostics().front().message));
                return;
            }
            if (duplicate) {
                if (anchor)
                    session_->execute(tr("Duplicate selection"),
                                      ed::PasteSelection{*anchor, std::move(result).takeValue(), {}}, r);
                return;
            }
            auto *mime = new QMimeData;
            mime->setData(Mime, QByteArray(reinterpret_cast<const char *>(result.value().data()),
                                           static_cast<qsizetype>(result.value().size())));
            QApplication::clipboard()->setMimeData(mime);
            if (cut)
                session_->execute(tr("Cut selection"), ed::DeleteSelection{selection}, r);
        });
}
void ProductivityActions::paste(std::vector<std::byte> content) {
    auto b = session_->backend();
    if (!b || !session_->selection())
        return;
    auto r = session_->revision();
    auto anchor = *session_->selection();
    auto reviewContent = content;
    background(
        this,
        [b, r, anchor, content = std::move(reviewContent)] { return b->reviewPaste(r, anchor, content); },
        [this, r, anchor, content = std::move(content)](auto result) mutable {
            if (r != session_->revision())
                return;
            if (!result) {
                emit session_->diagnostic(QString::fromStdString(result.diagnostics().front().message));
                return;
            }
            auto bindings = std::move(result.value().bindings);
            if (!bindings.empty()) {
                QDialog dialog(host_);
                dialog.setWindowTitle(tr("Review pasted references"));
                dialog.resize(650, 350);
                auto *layout = new QVBoxLayout(&dialog);
                layout->addWidget(new QLabel(tr("Internal references are remapped. Choose where each "
                                                "external reference should point."),
                                             &dialog));
                auto *table = new QTableWidget(static_cast<int>(bindings.size()), 2, &dialog);
                table->setHorizontalHeaderLabels({tr("Reference"), tr("Destination")});
                table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
                layout->addWidget(table);
                for (int i = 0; i < table->rowCount(); ++i) {
                    table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(bindings[i].label)));
                    auto *button = new QPushButton(
                        bindings[i].target ? tr("Keep current target") : tr("Choose target…"), table);
                    table->setCellWidget(i, 1, button);
                    connect(button, &QPushButton::clicked, &dialog, [&, i, button] {
                        ed::BrowseRequest q;
                        q.kind = std::holds_alternative<core::SctActionId>(bindings[i].source)
                                     ? ed::BrowseKind::Actions
                                     : ed::BrowseKind::Texts;
                        if (session_->inspection())
                            q.script = session_->inspection()->script;
                        if (auto target = pickReference(session_, q, &dialog)) {
                            bindings[i].target = *target;
                            button->setText(
                                tr("Target %1").arg(std::visit([](auto id) { return id.value; }, *target)));
                        }
                    });
                }
                auto *buttons =
                    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
                layout->addWidget(buttons);
                connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
                    for (auto &binding : bindings)
                        if (!binding.target) {
                            QMessageBox::information(
                                &dialog, tr("Choose targets"),
                                tr("Choose a destination for every external reference."));
                            return;
                        }
                    dialog.accept();
                });
                if (dialog.exec() != QDialog::Accepted)
                    return;
            }
            session_->execute(tr("Paste selection"),
                              ed::PasteSelection{anchor, std::move(content), std::move(bindings)}, r);
        });
}
void ProductivityActions::move() {
    if (!session_->inspection())
        return;
    auto items = session_->selectionSet();
    auto r = session_->revision();
    ed::BrowseRequest query;
    query.script = session_->inspection()->script;
    query.kind = !items.empty() && std::holds_alternative<core::program::SectionId>(items.front())
                     ? ed::BrowseKind::Sections
                     : ed::BrowseKind::Containers;
    if (auto target = pickReference(session_, query, host_))
        session_->execute(tr("Move selection"), ed::MoveSelection{std::move(items), *target}, r);
}
void ProductivityActions::snippets() {
    auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/IR snippets";
    QDir().mkpath(root);
    QDialog dialog(host_);
    dialog.setWindowTitle(tr("Snippet library"));
    dialog.resize(500, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list);
    auto refresh = [&] {
        list->clear();
        for (auto name : QDir(root).entryList({"*.salsafrg"}, QDir::Files, QDir::Name))
            list->addItem(name);
    };
    refresh();
    auto *save = new QPushButton(tr("Save copied selection…"), &dialog);
    auto *insert = new QPushButton(tr("Insert selected snippet"), &dialog);
    auto *remove = new QPushButton(tr("Delete selected snippet"), &dialog);
    layout->addWidget(save);
    layout->addWidget(insert);
    layout->addWidget(remove);
    save->setEnabled(QApplication::clipboard()->mimeData()->hasFormat(Mime));
    connect(save, &QPushButton::clicked, &dialog, [&] {
        bool ok = false;
        auto name = QInputDialog::getText(&dialog, tr("Save snippet"), tr("Name"), QLineEdit::Normal, {}, &ok)
                        .trimmed();
        if (!ok || name.isEmpty())
            return;
        if (name.contains('/') || name.contains('\\') || name.contains(':') || name == "..")
            return;
        auto path = root + "/" + name + ".salsafrg";
        if (QFile::exists(path) &&
            QMessageBox::question(&dialog, tr("Replace snippet"), tr("Replace the existing snippet?")) !=
                QMessageBox::Yes)
            return;
        auto content = QApplication::clipboard()->mimeData()->data(Mime);
        save->setEnabled(false);
        background(
            &dialog,
            [path, content] {
                QSaveFile file(path);
                return file.open(QIODevice::WriteOnly) && file.write(content) == content.size() &&
                       file.commit();
            },
            [&, save](bool ok) {
                save->setEnabled(true);
                if (!ok)
                    emit session_->diagnostic(tr("Cannot save snippet."));
                refresh();
            });
    });
    connect(insert, &QPushButton::clicked, &dialog, [&] {
        if (!list->currentItem())
            return;
        auto path = root + "/" + list->currentItem()->text();
        auto revision = session_->revision();
        insert->setEnabled(false);
        background(
            &dialog,
            [path] {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
                    return QByteArray{};
                auto content = file.readAll();
                return file.error() == QFileDevice::NoError ? content : QByteArray{};
            },
            [&, revision](QByteArray content) {
                insert->setEnabled(true);
                if (content.isEmpty()) {
                    emit session_->diagnostic(tr("Cannot read snippet or snippet is too large."));
                    return;
                }
                if (revision != session_->revision())
                    return;
                dialog.accept();
                paste(bytes(content));
            });
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (!list->currentItem())
            return;
        if (QMessageBox::question(&dialog, tr("Delete snippet"), tr("Delete this saved snippet?")) !=
            QMessageBox::Yes)
            return;
        if (!QFile::remove(root + "/" + list->currentItem()->text()))
            emit session_->diagnostic(tr("Cannot delete snippet."));
        refresh();
    });
    dialog.exec();
}
} // namespace salsa::qt
