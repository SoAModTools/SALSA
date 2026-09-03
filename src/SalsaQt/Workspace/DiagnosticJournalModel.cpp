#include "Workspace/DiagnosticJournalModel.h"

#include <QBrush>
#include <QColor>

#include <algorithm>
#include <ranges>

namespace salsa::qt {
namespace {

[[nodiscard]] QString severityText(const core::DiagnosticSeverity severity) {
    switch (severity) {
    case core::DiagnosticSeverity::Info: return QObject::tr("Information");
    case core::DiagnosticSeverity::Warning: return QObject::tr("Warning");
    case core::DiagnosticSeverity::Error: return QObject::tr("Error");
    }
    return QObject::tr("Unknown");
}

[[nodiscard]] QString eventText(const DiagnosticJournalModel::Event event) {
    switch (event) {
    case DiagnosticJournalModel::Event::Raised: return QObject::tr("Raised");
    case DiagnosticJournalModel::Event::Resolved: return QObject::tr("Resolved");
    case DiagnosticJournalModel::Event::Reraised: return QObject::tr("Re-raised");
    case DiagnosticJournalModel::Event::Activity: return QObject::tr("Activity");
    }
    return {};
}

}  // namespace

DiagnosticJournalModel::DiagnosticJournalModel(QObject* parent)
    : QAbstractTableModel(parent) {}

bool DiagnosticJournalModel::previouslySeen(const DiagnosticRow& row) const {
    return std::ranges::find(seen_, row) != seen_.end();
}

void DiagnosticJournalModel::append(const Event event, DiagnosticRow diagnostic) {
    const auto row = static_cast<int>(entries_.size());
    beginInsertRows({}, row, row);
    entries_.push_back({QDateTime::currentDateTime(), event, std::move(diagnostic)});
    endInsertRows();
}

void DiagnosticJournalModel::observe(const std::vector<DiagnosticRow>& current) {
    std::vector<bool> previousMatched(current_.size(), false);
    std::vector<bool> currentMatched(current.size(), false);
    for (std::size_t next = 0; next < current.size(); ++next) {
        for (std::size_t previous = 0; previous < current_.size(); ++previous) {
            if (!previousMatched[previous] && current[next] == current_[previous]) {
                previousMatched[previous] = true;
                currentMatched[next] = true;
                break;
            }
        }
    }
    for (std::size_t previous = 0; previous < current_.size(); ++previous) {
        if (!previousMatched[previous]) append(Event::Resolved, current_[previous]);
    }
    for (std::size_t next = 0; next < current.size(); ++next) {
        if (currentMatched[next]) continue;
        const auto event = previouslySeen(current[next]) ? Event::Reraised : Event::Raised;
        append(event, current[next]);
        if (!previouslySeen(current[next])) seen_.push_back(current[next]);
    }
    current_ = current;
}

void DiagnosticJournalModel::appendActivity(
    QString code, QString message, QString location) {
    DiagnosticRow row;
    row.severity = core::DiagnosticSeverity::Info;
    row.code = std::move(code);
    row.message = std::move(message);
    row.location = std::move(location);
    row.tooltip = row.message;
    append(Event::Activity, std::move(row));
}

int DiagnosticJournalModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int DiagnosticJournalModel::columnCount(const QModelIndex&) const { return 6; }

QVariant DiagnosticJournalModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0
        || static_cast<std::size_t>(index.row()) >= entries_.size()) return {};
    const auto& entry = entries_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return entry.time.toString(QStringLiteral("HH:mm:ss.zzz"));
        case 1: return eventText(entry.event);
        case 2: return severityText(entry.diagnostic.severity);
        case 3: return entry.diagnostic.code;
        case 4: return entry.diagnostic.message;
        case 5: return entry.diagnostic.location;
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) return entry.diagnostic.tooltip;
    if (role == Qt::ForegroundRole && entry.event == Event::Resolved)
        return QBrush(QColor(110, 110, 110));
    return {};
}

QVariant DiagnosticJournalModel::headerData(
    const int section, const Qt::Orientation orientation, const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return tr("Time");
    case 1: return tr("Event");
    case 2: return tr("Severity");
    case 3: return tr("Code");
    case 4: return tr("Message");
    case 5: return tr("Location");
    default: return {};
    }
}

}  // namespace salsa::qt
