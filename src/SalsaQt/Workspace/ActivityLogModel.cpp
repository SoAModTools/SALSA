#include "Workspace/ActivityLogModel.h"

#include <QBrush>
#include <QColor>

#include <utility>

namespace salsa::qt {
namespace {

QString severityText(const std::optional<core::DiagnosticSeverity> severity) {
    if (!severity) return {};
    switch (*severity) {
    case core::DiagnosticSeverity::Info: return QObject::tr("Information");
    case core::DiagnosticSeverity::Warning: return QObject::tr("Warning");
    case core::DiagnosticSeverity::Error: return QObject::tr("Error");
    }
    return {};
}

QString eventText(const ActivityLogModel::Event event) {
    switch (event) {
    case ActivityLogModel::Event::IssueRaised: return QObject::tr("Issue raised");
    case ActivityLogModel::Event::IssueResolved: return QObject::tr("Issue resolved");
    case ActivityLogModel::Event::IssueReraised: return QObject::tr("Issue re-raised");
    case ActivityLogModel::Event::OperationCompleted: return QObject::tr("Completed");
    case ActivityLogModel::Event::OperationFailed: return QObject::tr("Failed");
    case ActivityLogModel::Event::OperationCancelled: return QObject::tr("Cancelled");
    }
    return {};
}

}  // namespace

ActivityLogModel::ActivityLogModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void ActivityLogModel::appendEntry(Entry entry) {
    const auto row = static_cast<int>(entries_.size());
    beginInsertRows({}, row, row);
    entries_.push_back(std::move(entry));
    endInsertRows();
}

void ActivityLogModel::appendIssue(const Event event, DiagnosticRow diagnostic) {
    appendEntry({QDateTime::currentDateTime(), event, diagnostic.severity,
        std::move(diagnostic.code), std::move(diagnostic.message),
        std::move(diagnostic.location), std::move(diagnostic.tooltip)});
}

void ActivityLogModel::observeIssues(const std::vector<DiagnosticRow>& current) {
    for (auto& activity : issueTracker_.observe(current)) {
        auto event = Event::IssueRaised;
        switch (activity.transition) {
        case core::IssueActivityTransition::Raised: event = Event::IssueRaised; break;
        case core::IssueActivityTransition::Resolved: event = Event::IssueResolved; break;
        case core::IssueActivityTransition::Reraised: event = Event::IssueReraised; break;
        }
        appendIssue(event, std::move(activity.issue));
    }
}

void ActivityLogModel::appendOperation(const ActivityOutcome outcome,
    QString code, QString message, QString location) {
    Event event = Event::OperationCompleted;
    if (outcome == ActivityOutcome::Failed) event = Event::OperationFailed;
    else if (outcome == ActivityOutcome::Cancelled) event = Event::OperationCancelled;
    const auto tooltip = message;
    appendEntry({QDateTime::currentDateTime(), event, std::nullopt,
        std::move(code), std::move(message), std::move(location), tooltip});
}

int ActivityLogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int ActivityLogModel::columnCount(const QModelIndex&) const { return 6; }

QVariant ActivityLogModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0
        || static_cast<std::size_t>(index.row()) >= entries_.size()) return {};
    const auto& entry = entries_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return entry.time.toString(QStringLiteral("HH:mm:ss.zzz"));
        case 1: return eventText(entry.event);
        case 2: return severityText(entry.severity);
        case 3: return entry.code;
        case 4: return entry.message;
        case 5: return entry.location;
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) return entry.tooltip;
    if (role == Qt::ForegroundRole && entry.event == Event::IssueResolved)
        return QBrush(QColor(110, 110, 110));
    return {};
}

QVariant ActivityLogModel::headerData(const int section,
    const Qt::Orientation orientation, const int role) const {
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
