#include "Editor/EditorQueryWidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace salsa::qt {
namespace ed = core::editor;
EditorQueryWidget::EditorQueryWidget(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session) {
    auto *layout = new QVBoxLayout(this);
    list_ = new QListWidget(this);
    layout->addWidget(list_);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *buttons = new QHBoxLayout;
    previous_ = new QPushButton(tr("Previous"), this);
    next_ = new QPushButton(tr("Next"), this);
    buttons->addWidget(previous_);
    buttons->addWidget(next_);
    layout->addLayout(buttons);
    previous_->setEnabled(false);
    next_->setEnabled(false);
    connect(previous_, &QPushButton::clicked, this, [this] {
        query_.offset -= std::min(query_.offset, query_.limit);
        load();
    });
    connect(next_, &QPushButton::clicked, this, [this] {
        query_.offset += query_.limit;
        load();
    });
    connect(list_, &QListWidget::itemActivated, this, [this] {
        if (auto s = selected(); s && activated)
            activated(*s);
    });
    connect(session_, &EditorSessionPresenter::changed, this, [this] {
        stop_.request_stop();
        ++generation_;
        list_->clear();
        rows_.clear();
        status_->setText(tr("Content changed. Run the query again."));
        previous_->setEnabled(false);
        next_->setEnabled(false);
    });
    connect(session_, &EditorSessionPresenter::sessionChanged, this, [this] {
        stop_.request_stop();
        ++generation_;
        list_->clear();
        rows_.clear();
        previous_->setEnabled(false);
        next_->setEnabled(false);
    });
}
void EditorQueryWidget::query(ed::BrowseRequest query) {
    query_ = query;
    load();
}
std::optional<ed::Subject> EditorQueryWidget::selected() const {
    int n = list_->currentRow();
    return n >= 0 && n < static_cast<int>(rows_.size()) ? std::optional(rows_[n].subject) : std::nullopt;
}
void EditorQueryWidget::load() {
    stop_.request_stop();
    stop_ = std::stop_source{};
    auto stop = stop_.get_token();
    auto backend = session_->backend();
    if (!backend)
        return;
    auto revision = session_->revision();
    auto generation = ++generation_;
    auto query = query_;
    previous_->setEnabled(false);
    next_->setEnabled(false);
    list_->clear();
    rows_.clear();
    status_->setText(tr("Searching…"));
    auto *watcher = new QFutureWatcher<core::Result<ed::BrowsePage>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, revision, generation] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (generation != generation_ || revision != session_->revision())
            return;
        if (!result) {
            status_->setText(QString::fromStdString(result.diagnostics().front().message));
            return;
        }
        rows_ = std::move(result.value().rows);
        for (auto &r : rows_)
            list_->addItem(QString::fromStdString(r.label));
        status_->setText(tr("%1 results; showing %2–%3")
                             .arg(result.value().total)
                             .arg(rows_.empty() ? 0 : query_.offset + 1)
                             .arg(query_.offset + rows_.size()));
        previous_->setEnabled(query_.offset > 0);
        next_->setEnabled(query_.offset + rows_.size() < result.value().total);
    });
    watcher->setFuture(
        QtConcurrent::run([backend, revision, query, stop] { return backend->browse(revision, query, stop); }));
}
std::optional<ed::Subject> pickReference(EditorSessionPresenter *session, ed::BrowseRequest query, QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Choose authored target"));
    dialog.resize(560, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *results = new EditorQueryWidget(session, &dialog);
    layout->addWidget(results);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (results->selected())
            dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    results->activated = [&](auto) { dialog.accept(); };
    results->query(query);
    return dialog.exec() == QDialog::Accepted ? results->selected() : std::nullopt;
}
NavigatorComponent::NavigatorComponent(EditorSessionPresenter *session, QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *kind = new QComboBox(this);
    kind->addItems({tr("Opcode"), tr("Variable"), tr("Incoming references"), tr("Strings"), tr("Incomplete bindings"),
                    tr("Actions")});
    layout->addWidget(kind);
    auto *bank = new QComboBox(this);
    bank->addItems({tr("BitVar"), tr("ByteVar"), tr("IntVar"), tr("FloatVar")});
    layout->addWidget(bank);
    auto *number = new QSpinBox(this);
    number->setRange(0, 2147483647);
    layout->addWidget(number);
    auto *local = new QCheckBox(tr("Active script only"), this);
    local->setChecked(true);
    showVariableSearch=[kind,local,number]{kind->setCurrentIndex(1);local->setChecked(false);number->setFocus();};
    layout->addWidget(local);
    auto *search = new QPushButton(tr("Search"), this);
    layout->addWidget(search);
    auto *results = new EditorQueryWidget(session, this);
    layout->addWidget(results);
    results->activated = [session](auto subject) { session->navigate(subject); };
    connect(search, &QPushButton::clicked, this, [=] {
        ed::BrowseRequest query;
        query.kind = static_cast<ed::BrowseKind>(kind->currentIndex());
        query.number = number->value();
        query.bank = bank->currentIndex();
        if (auto &i = session->inspection(); i) {
            if (local->isChecked())
                query.script = i->script;
            query.target = i->text ? ed::Subject{*i->text} : i->subject;
        }
        results->query(query);
    });
}
} // namespace salsa::qt
