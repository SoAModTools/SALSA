#pragma once

#include "Workspace/DiagnosticsModel.h"

#include <QAbstractTableModel>
#include <QDateTime>

#include <vector>

namespace salsa::qt {

class DiagnosticJournalModel final : public QAbstractTableModel {
public:
    enum class Event { Raised, Resolved, Reraised, Activity };

    explicit DiagnosticJournalModel(QObject* parent = nullptr);

    void observe(const std::vector<DiagnosticRow>& current);
    void appendActivity(QString code, QString message, QString location = {});

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section, Qt::Orientation orientation, int role) const override;

private:
    struct Entry final {
        QDateTime time;
        Event event = Event::Raised;
        DiagnosticRow diagnostic;
    };

    void append(Event event, DiagnosticRow diagnostic);
    [[nodiscard]] bool previouslySeen(const DiagnosticRow& row) const;

    std::vector<DiagnosticRow> current_{};
    std::vector<DiagnosticRow> seen_{};
    std::vector<Entry> entries_{};
};

}  // namespace salsa::qt
