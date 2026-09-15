#pragma once
#include "Editor/AuthoringTools.h"
#include "Editor/EditorQueryWidget.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt::feature_ui {
namespace ed = core::editor;
inline void availability(QAction *action, EditorSessionPresenter *session) {
    auto refresh = [=] {
        action->setEnabled(session->backend() && session->inspection() &&
                           session->inspection()->script.valid() && !session->busy());
    };
    QObject::connect(session, &EditorSessionPresenter::inspectionChanged, action, refresh);
    QObject::connect(session, &EditorSessionPresenter::sessionChanged, action, refresh);
    QObject::connect(session, &EditorSessionPresenter::availabilityChanged, action, refresh);
    refresh();
}
inline QString q(const std::string &s) { return QString::fromStdString(s); }
template <class W, class F> void background(QObject *owner, W work, F done) {
    using R = decltype(work());
    auto *watcher = new QFutureWatcher<R>(owner);
    QObject::connect(watcher, &QFutureWatcherBase::finished, owner,
                     [watcher, done = std::move(done)]() mutable {
                         auto r = watcher->result();
                         watcher->deleteLater();
                         done(std::move(r));
                     });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}
template <class T> bool report(EditorSessionPresenter *s, const core::Result<T> &result) {
    if (result)
        return true;
    for (const auto &d : result.diagnostics())
        emit s->diagnostic(q(d.message));
    return false;
}
template <class F> void inventory(EditorSessionPresenter *s, QObject *owner, F done) {
    if (!s->backend() || !s->inspection() || s->busy())
        return;
    auto script = s->inspection()->script;
    auto r = s->revision();
    auto b = s->backend();
    background(
        owner, [=] { return b->features(r, script); },
        [=](auto result) mutable {
            if (r == s->revision() && report(s, result))
                done(std::move(result).takeValue());
        });
}
inline std::optional<std::string> name(QWidget *host, QString title, std::string current = {}) {
    bool ok = false;
    auto text =
        QInputDialog::getText(host, title, QObject::tr("Name"), QLineEdit::Normal, q(current), &ok).trimmed();
    return ok && !text.isEmpty() ? std::optional(text.toStdString()) : std::nullopt;
}
inline std::optional<std::vector<int>> choose(QWidget *host, QString title, const QStringList &labels,
                                              bool multiple = true) {
    QDialog dialog(host);
    dialog.setWindowTitle(title);
    dialog.resize(560, 420);
    auto *l = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    list->addItems(labels);
    list->setSelectionMode(multiple ? QAbstractItemView::ExtendedSelection
                                    : QAbstractItemView::SingleSelection);
    l->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    l->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    std::vector<int> out;
    for (auto *i : list->selectedItems())
        out.push_back(list->row(i));
    std::sort(out.begin(), out.end());
    return out;
}
inline std::optional<core::SctActionId> action(EditorSessionPresenter *s, QWidget *h, QString title) {
    ed::BrowseRequest request;
    request.kind = ed::BrowseKind::Actions;
    if (s->inspection())
        request.script = s->inspection()->script;
    QMessageBox::information(h, QObject::tr("Select action"), title);
    auto id = pickReference(s, request, h);
    return id && std::holds_alternative<core::SctActionId>(*id)
               ? std::optional(std::get<core::SctActionId>(*id))
               : std::nullopt;
}
inline std::optional<ed::FeatureSite> site(EditorSessionPresenter *s, QWidget *h, QString title) {
    auto id = action(s, h, title);
    if (!id)
        return {};
    bool ok;
    auto schema = QInputDialog::getInt(h, QObject::tr("Operand"), QObject::tr("Parameter schema index"), 0, 0,
                                       65535, 1, &ok);
    if (!ok)
        return {};
    auto group = QInputDialog::getInt(h, QObject::tr("Operand"),
                                      QObject::tr("Repeated group / switch-case ordinal (-1 for fixed)"), -1,
                                      -1, INT_MAX, 1, &ok);
    if (!ok)
        return {};
    return ed::FeatureSite{*id,
                           {static_cast<unsigned>(schema),
                            group < 0 ? std::nullopt : std::optional(static_cast<unsigned>(group))}};
}
inline QAction *menuAction(QMenu *menu, QString name, QObject *owner, std::function<void()> run) {
    auto *a = menu->addAction(name);
    QObject::connect(a, &QAction::triggered, owner, std::move(run));
    return a;
}
inline void commit(EditorSessionPresenter *s, QDialog &dialog, ed::Revision r, ed::FeatureEdit edit,
                   QString title) {
    dialog.accept();
    s->execute(title, std::move(edit), r);
}
} // namespace salsa::qt::feature_ui
