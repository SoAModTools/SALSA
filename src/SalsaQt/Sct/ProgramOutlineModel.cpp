#include "Sct/ProgramOutlineModel.h"
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFutureWatcher>
#include <QPointer>
#include <QStyle>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
namespace salsa::qt {
namespace ir = core::editor;
ProgramOutlineModel::ProgramOutlineModel(QObject *parent) : QAbstractItemModel(parent) { nodes_.resize(1); }
std::size_t ProgramOutlineModel::slot(const QModelIndex &i) const { return i.isValid() ? i.internalId() : 0; }
QModelIndex ProgramOutlineModel::indexFor(std::size_t n) const {
    return n && n < nodes_.size() && nodes_[n].live ? createIndex(static_cast<int>(nodes_[n].ordinal), 0, n)
                                                    : QModelIndex{};
}
QModelIndex ProgramOutlineModel::index(int r, int c, const QModelIndex &p) const {
    auto n = slot(p);
    if (n >= nodes_.size() || r < 0 || c < 0 || c >= 2 || (p.isValid() && p.column() != 0) ||
        static_cast<std::size_t>(r) >= nodes_[n].children.size())
        return {};
    return createIndex(r, c, nodes_[n].children[r]);
}
QModelIndex ProgramOutlineModel::parent(const QModelIndex &i) const {
    auto n = slot(i);
    return n && n < nodes_.size() ? indexFor(nodes_[n].parent) : QModelIndex{};
}
int ProgramOutlineModel::rowCount(const QModelIndex &p) const {
    auto n = slot(p);
    return n < nodes_.size() && (!p.isValid() || !p.column()) ? static_cast<int>(nodes_[n].children.size())
                                                              : 0;
}
const ir::OutlineRow *ProgramOutlineModel::row(const QModelIndex &i) const {
    auto n = slot(i);
    return n && n < nodes_.size() && nodes_[n].live ? &nodes_[n].row : nullptr;
}
QVariant ProgramOutlineModel::data(const QModelIndex &i, int role) const {
    const auto *r = row(i);
    if (!r)
        return {};
    if (role == Qt::ForegroundRole && r->color)
        return QBrush(QColor::fromRgb(*r->color));
    if (role == Qt::DecorationRole && r->bookmarked)
        return QApplication::style()->standardIcon(QStyle::SP_DialogApplyButton);
    if (i.column() == 0 && !r->groupingIssue.empty()) {
        if (role == Qt::ToolTipRole)
            return QString::fromStdString(r->label + "\n" + r->groupingIssue);
        if (role == Qt::DecorationRole)
            return QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
    }
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        if (i.column() == 0)
            return QString::fromStdString(r->label);
        return std::visit([](auto id) { return QVariant::fromValue<qulonglong>(id.value); }, r->subject);
    }
    return {};
}
QVariant ProgramOutlineModel::headerData(int n, Qt::Orientation o, int role) const {
    return o == Qt::Horizontal && role == Qt::DisplayRole
               ? QVariant(n ? tr("Identity") : tr("Script content"))
               : QVariant{};
}
bool ProgramOutlineModel::canFetchMore(const QModelIndex &p) const {
    auto n = slot(p);
    if (!service_ || n >= nodes_.size() || (p.isValid() && p.column()))
        return false;
    const auto &node = nodes_[n];
    return node.live && node.row.expandable && !node.loading &&
           (!node.fetched || node.children.size() < node.total);
}
bool ProgramOutlineModel::hasChildren(const QModelIndex &p) const {
    auto n = slot(p);
    return n < nodes_.size() && (!p.isValid() || !p.column()) &&
           (nodes_[n].row.expandable || !nodes_[n].children.empty());
}
bool ProgramOutlineModel::pending(const QModelIndex &i) const {
    auto n = slot(i);
    return n < nodes_.size() && nodes_[n].loading;
}
void ProgramOutlineModel::bind(std::shared_ptr<ir::Backend> service, ir::Revision revision,
                               core::editor::Subject script) {
    beginResetModel();
    ++generation_;
    service_ = std::move(service);
    revision_ = revision;
    nodes_.clear();
    free_.clear();
    Node root;
    root.row = {script, {}, "", true};
    nodes_.push_back(std::move(root));
    endResetModel();
    fetchMore({});
}
void ProgramOutlineModel::fetchMore(const QModelIndex &parent) {
    if (!canFetchMore(parent))
        return;
    const auto n = slot(parent), generation = generation_, request = ++nextRequest_;
    auto &node = nodes_[n];
    node.loading = true;
    node.request = request;
    auto *watcher = new QFutureWatcher<core::Result<ir::OutlinePage>>(this);
    const auto revision = revision_;
    auto service = service_;
    ir::OutlineRequest input{node.row.subject, node.children.size(), 128, mode_, language_};
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, n, generation, revision, request] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (generation != generation_ || n >= nodes_.size() || !nodes_[n].live ||
            nodes_[n].request != request)
            return;
        nodes_[n].loading = false;
        if (revision != revision_) {
            fetchMore(indexFor(n));
            return;
        }
        if (!result) {
            nodes_[n].fetched = true;
            nodes_[n].total = nodes_[n].children.size();
            if (diagnostic)
                for (const auto &d : result.diagnostics())
                    diagnostic(QString::fromStdString(d.message));
            if (!n && ready)
                ready();
            if (pageReady)
                pageReady();
            return;
        }
        const auto &page = result.value();
        auto count = nodes_[n].children.size();
        if (!page.rows.empty())
            beginInsertRows(indexFor(n), static_cast<int>(count),
                            static_cast<int>(count + page.rows.size() - 1));
        for (const auto &row : page.rows) {
            Node added;
            added.row = row;
            added.parent = n;
            added.ordinal = nodes_[n].children.size();
            std::size_t at;
            if (free_.empty()) {
                at = nodes_.size();
                nodes_.push_back(std::move(added));
            } else {
                at = free_.back();
                free_.pop_back();
                nodes_[at] = std::move(added);
            }
            nodes_[n].children.push_back(at);
        }
        nodes_[n].fetched = true;
        nodes_[n].total = page.total;
        if (!page.rows.empty())
            endInsertRows();
        if (pageReady)
            pageReady();
        if (measured)
            measured(page.elapsedMicroseconds, page.rows.size());
        if (!n && ready) {
            auto complete = ready;
            complete();
        }
    });
    watcher->setFuture(
        QtConcurrent::run([service, revision, input] { return service->outline(revision, input); }));
}
QModelIndex ProgramOutlineModel::find(const ir::Subject &subject) const {
    for (std::size_t n = 1; n < nodes_.size(); ++n)
        if (nodes_[n].live && nodes_[n].row.subject == subject)
            return indexFor(n);
    return {};
}
void ProgramOutlineModel::discard(std::size_t n) {
    for (auto c : nodes_[n].children)
        discard(c);
    nodes_[n].children.clear();
    nodes_[n].live = false;
    nodes_[n].request = ++nextRequest_;
    free_.push_back(n);
}
void ProgramOutlineModel::resetChildren(std::size_t n) {
    if (!nodes_[n].children.empty()) {
        beginRemoveRows(indexFor(n), 0, static_cast<int>(nodes_[n].children.size() - 1));
        for (auto child : nodes_[n].children)
            discard(child);
        nodes_[n].children.clear();
        endRemoveRows();
    }
    nodes_[n].fetched = false;
    nodes_[n].loading = false;
    nodes_[n].request = ++nextRequest_;
    fetchMore(indexFor(n));
}
void ProgramOutlineModel::apply(const ir::Change &change) {
    revision_ = change.status.current;
    if (!nodes_.empty())
        if (auto *sequence = std::get_if<core::SctSequenceId>(&nodes_[0].row.subject))
            if (std::ranges::find(change.sequences, *sequence) != change.sequences.end())
                resetChildren(0);
    if (!nodes_.empty() && std::holds_alternative<core::SctModuleId>(nodes_[0].row.subject) &&
        change.presentationChanged) {
        resetChildren(0);
        return;
    }
    if (!nodes_.empty())
        if (auto *script = std::get_if<core::SctScriptId>(&nodes_[0].row.subject))
            if (std::ranges::find(change.scriptLayouts, *script) != change.scriptLayouts.end()) {
                resetChildren(0);
                return;
            }
    if (mode_ == ir::ViewMode::Semantic) {
        // Branches are sibling rows. A changed chain can add/remove rows in its
        // containing page, so refresh that page instead of just the If children.
        std::vector<std::size_t> parents;
        for (std::size_t n = 1; n < nodes_.size(); ++n)
            if (nodes_[n].live && nodes_[n].row.chain &&
                std::ranges::find(change.actions, *nodes_[n].row.chain) != change.actions.end())
                parents.push_back(nodes_[n].parent);
        std::ranges::sort(parents);
        parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
        for (auto n : parents)
            if (nodes_[n].live)
                resetChildren(n);
    }
    if (mode_ == ir::ViewMode::Semantic && !change.projectionSequences.empty()) {
        std::vector<std::size_t> affected;
        for (std::size_t n = 1; n < nodes_.size(); ++n)
            if (nodes_[n].live && nodes_[n].fetched &&
                (!std::holds_alternative<core::SctActionId>(nodes_[n].row.subject) || nodes_[n].row.chain ||
                 nodes_[n].row.condition) &&
                std::ranges::find(change.projectionSequences, nodes_[n].row.sequence) !=
                    change.projectionSequences.end())
                affected.push_back(n);
        for (auto n : affected)
            if (nodes_[n].live)
                resetChildren(n);
    }
    std::vector<std::size_t> orders, labels;
    for (std::size_t n = 1; n < nodes_.size(); ++n) {
        const auto &node = nodes_[n];
        if (!node.live)
            continue;
        const bool sequenceContainer =
            !std::holds_alternative<core::SctActionId>(node.row.subject) || node.row.chain ||
            (mode_ == ir::ViewMode::Semantic && node.row.condition);
        if (sequenceContainer && node.fetched &&
            std::ranges::find(change.sequences, node.row.sequence) != change.sequences.end())
            orders.push_back(n);
        std::visit(
            [&](auto id) {
                using T = decltype(id);
                bool changed = change.presentationChanged;
                changed |= node.row.condition && std::ranges::find(change.conditions, *node.row.condition) !=
                                                     change.conditions.end();
                changed |=
                    node.row.preparation &&
                    std::ranges::find(change.sequences, *node.row.preparation) != change.sequences.end();
                for (auto sequence : node.row.supportingSequences)
                    changed |= std::ranges::find(change.sequences, sequence) != change.sequences.end();
                if constexpr (std::is_same_v<T, core::SctActionId>)
                    changed |= std::ranges::find(change.actions, id) != change.actions.end();
                else if constexpr (std::is_same_v<T, core::program::SectionId>)
                    changed |= std::ranges::find(change.sections, id) != change.sections.end();
                if (changed)
                    labels.push_back(n);
            },
            node.row.subject);
    }
    for (auto n : orders)
        if (nodes_[n].live)
            resetChildren(n);
    // A parameter change updates its row in place; it does not discard siblings
    // or their expansion state. Structural edits refresh only the owning order.
    for (auto n : labels)
        if (nodes_[n].live) {
            auto parent = nodes_[n].parent;
            auto expected = nodes_[n].row.subject;
            auto generation = generation_;
            auto revision = revision_;
            auto service = service_;
            auto *watcher = new QFutureWatcher<core::Result<ir::OutlinePage>>(this);
            ir::OutlineRequest request{nodes_[parent].row.subject, nodes_[n].ordinal, 1, mode_, language_};
            connect(watcher, &QFutureWatcherBase::finished, this,
                    [this, watcher, n, expected, generation, revision,
                     changePresentation = change.presentationChanged] {
                        auto result = watcher->result();
                        watcher->deleteLater();
                        if (generation != generation_ || revision != revision_ || n >= nodes_.size() ||
                            !nodes_[n].live || nodes_[n].row.subject != expected)
                            return;
                        if (!result || result.value().rows.empty() ||
                            result.value().rows.front().subject != expected)
                            return;
                        const auto oldKind = nodes_[n].row.kind;
                        nodes_[n].row = result.value().rows.front();
                        emit dataChanged(indexFor(n), indexFor(n));
                        if (!changePresentation && nodes_[n].fetched &&
                            (oldKind != nodes_[n].row.kind ||
                             (nodes_[n].row.kind && *nodes_[n].row.kind != ir::RowKind::Instruction) ||
                             nodes_[n].row.condition))
                            resetChildren(n);
                    });
            watcher->setFuture(QtConcurrent::run(
                [service, revision, request] { return service->outline(revision, request); }));
        }
}
} // namespace salsa::qt
