#include "Editor/WorkspaceComponent.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>
namespace salsa::qt {
namespace ed = core::editor;
namespace {
QString q(const std::string &s) { return QString::fromStdString(s); }
QString label(ed::SourceState state) {
    switch (state) {
    case ed::SourceState::Unchanged:
        return QObject::tr("Matches imported source");
    case ed::SourceState::Changed:
        return QObject::tr("Source changed");
    case ed::SourceState::Missing:
        return QObject::tr("Source missing");
    case ed::SourceState::Added:
        return QObject::tr("Not imported");
    case ed::SourceState::Unqualified:
        return QObject::tr("No source qualification");
    default:
        return QObject::tr("Not checked");
    }
}
} // namespace
WorkspaceComponent::WorkspaceComponent(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    auto *buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    auto button = [&](QString text, auto callback) {
        auto *b = new QPushButton(text, this);
        buttons->addWidget(b);
        connect(b, &QPushButton::clicked, this, callback);
    };
    button(tr("Review source changes"), [this] {
        if (refreshRequested)
            refreshRequested();
    });
    button(tr("Reopen saved project"), [this] {
        if (reopenRequested)
            reopenRequested();
    });
    button(tr("Import into another workspace…"), [this] {
        if (importRequested)
            importRequested();
    });
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter scripts, paths or status"));
    layout->addWidget(filter_);
    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({tr("Source / script"), tr("Source status")});
    tree_->setUniformRowHeights(true);
    layout->addWidget(tree_, 1);
    details_ = new QPlainTextEdit(this);
    details_->setReadOnly(true);
    details_->setMaximumHeight(155);
    layout->addWidget(details_);
    connect(filter_, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](auto *item, int) {
        auto index = item->data(0, Qt::UserRole);
        if (!index.isValid())
            return;
        auto &row = view_.sources.at(index.toULongLong());
        if (row.script && navigate)
            navigate(*row.script);
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](auto *item, auto *) {
        details_->clear();
        if (!item)
            return;
        auto index = item->data(0, Qt::UserRole);
        if (!index.isValid())
            return;
        auto &row = view_.sources.at(index.toULongLong());
        details_->setPlainText(
            tr("%1\n%2\nImported: %3 bytes; SHA-256 %4\nInspected source: %5 bytes; SHA-256 %6\n%7")
                .arg(q(row.asset), label(row.state),
                     row.baselineBytes ? QString::number(*row.baselineBytes) : tr("unknown"),
                     q(row.baselineDigest),
                     row.currentBytes ? QString::number(*row.currentBytes) : tr("unavailable / not checked"),
                     q(row.currentDigest),
                     row.script ? tr("Activate this row to open the authored script. Source differences do "
                                     "not replace its content.")
                                : tr("Import this source into another workspace to inspect it.")));
    });
    clear();
}
void WorkspaceComponent::clear() {
    ++generation_;
    view_ = {};
    tree_->clear();
    details_->clear();
    summary_->setText(tr("No workspace open."));
}
void WorkspaceComponent::status(bool dirty, bool needsSave) {
    view_.dirty = dirty;
    view_.needsInitialSave = needsSave;
    if (view_.workspaceRoot.empty())
        return;
    summary_->setText(tr("Workspace: %1\nDataset: %2\nProject: %3\n%4 · %5 source entries · %6")
                          .arg(q(view_.workspaceRoot), q(view_.datasetRoot), q(view_.checkpointPath),
                               dirty || needsSave ? tr("Unsaved changes") : tr("Saved"),
                               QString::number(view_.sources.size()),
                               view_.inspected ? tr("Explicit source review; activate a script to navigate")
                                               : tr("Source files have not been rechecked")));
}
void WorkspaceComponent::present(ed::WorkspaceOverview value, std::function<void()> ready) {
    auto generation = ++generation_;
    view_ = std::move(value);
    tree_->clear();
    details_->clear();
    status(view_.dirty, view_.needsInitialSave);
    auto at = std::make_shared<std::size_t>(0);
    auto folders = std::make_shared<std::map<QString, QTreeWidgetItem *>>();
    auto batch = std::make_shared<std::function<void()>>();
    *batch = [this, generation, at, folders, weak = std::weak_ptr(batch), ready = std::move(ready)] {
        auto batch = weak.lock();
        if (!batch)
            return;
        if (generation != generation_) {
            *batch = {};
            if (ready)
                ready();
            return;
        }
        auto end = std::min(*at + 128, view_.sources.size());
        for (; *at < end; ++*at) {
            const auto &row = view_.sources[*at];
            auto path = q(row.asset).split('/');
            QTreeWidgetItem *parent = nullptr;
            QString key;
            for (int n = 0; n + 1 < path.size(); ++n) {
                key += '/' + path[n];
                auto &folder = (*folders)[key];
                if (!folder) {
                    folder = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
                    folder->setText(0, path[n]);
                    folder->setExpanded(true);
                }
                parent = folder;
            }
            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
            item->setText(0, q(row.name));
            item->setText(1, label(row.state));
            item->setToolTip(0, q(row.asset));
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(*at));
        }
        if (*at < view_.sources.size()) {
            QTimer::singleShot(0, this, [batch] {
                auto run = *batch;
                if (run)
                    run();
            });
            return;
        }
        *batch = {};
        filter();
        if (ready)
            ready();
    };
    QTimer::singleShot(0, this, [batch] {
        auto run = *batch;
        if (run)
            run();
    });
}
void WorkspaceComponent::filter() {
    const auto text = filter_->text();
    std::function<bool(QTreeWidgetItem *)> visit = [&](auto *item) {
        bool visible = false;
        for (int n = 0; n < item->childCount(); ++n)
            visible = visit(item->child(n)) || visible;
        if (item->data(0, Qt::UserRole).isValid())
            visible = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive) ||
                      item->text(1).contains(text, Qt::CaseInsensitive) ||
                      item->toolTip(0).contains(text, Qt::CaseInsensitive);
        item->setHidden(!visible);
        return visible;
    };
    for (int n = 0; n < tree_->topLevelItemCount(); ++n)
        visit(tree_->topLevelItem(n));
}
} // namespace salsa::qt
