#pragma once

#include "SalsaCore/Foundation/Diagnostic.h"

#include <QAbstractTableModel>

#include <vector>

namespace salsa::qt {

class DiagnosticsModel final : public QAbstractTableModel {
public:
    explicit DiagnosticsModel(QObject* parent = nullptr);

    void setDiagnostics(std::vector<core::Diagnostic> diagnostics);
    void clear();

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;

private:
    std::vector<core::Diagnostic> diagnostics_{};
};

}  // namespace salsa::qt
