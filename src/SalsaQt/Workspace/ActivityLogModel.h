#pragma once

#include "SalsaCore/Application/ActivityLifecycle.h"
#include "Workspace/DiagnosticsModel.h"

#include <QAbstractTableModel>
#include <QDateTime>

#include <optional>
#include <vector>

namespace salsa::qt {

enum class ActivityOutcome { Completed, Failed, Cancelled };

class ActivityLogModel final : public QAbstractTableModel {
public:
    enum class Event {
        IssueRaised,
        IssueResolved,
        IssueReraised,
        OperationCompleted,
        OperationFailed,
        OperationCancelled,
    };

    explicit ActivityLogModel(QObject* parent = nullptr);

    void observeIssues(const std::vector<DiagnosticRow>& current);
    void appendOperation(ActivityOutcome outcome, QString code,
        QString message, QString location = {});

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section, Qt::Orientation orientation, int role) const override;

private:
    struct Entry final {
        QDateTime time;
        Event event = Event::IssueRaised;
        std::optional<core::DiagnosticSeverity> severity{};
        QString code;
        QString message;
        QString location;
        QString tooltip;
    };

    void appendIssue(Event event, DiagnosticRow diagnostic);
    void appendEntry(Entry entry);
    core::IssueActivityTracker<DiagnosticRow> issueTracker_{};
    std::vector<Entry> entries_{};
};

}  // namespace salsa::qt
