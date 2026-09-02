#pragma once

#include "SalsaCore/Foundation/Diagnostic.h"
#include "SalsaCore/Sct/SctInspectionLocation.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"

#include <QAbstractTableModel>

#include <vector>

namespace salsa::qt {

struct DiagnosticRow final {
    core::DiagnosticSeverity severity = core::DiagnosticSeverity::Error;
    QString code{};
    QString message{};
    QString location{};
    std::optional<core::AssetLocator> locator{};
    std::optional<core::SctInspectionLocation> inspectionLocation{};
};

class DiagnosticsModel final : public QAbstractTableModel {
public:
    explicit DiagnosticsModel(QObject* parent = nullptr);

    void setDiagnostics(std::vector<core::Diagnostic> diagnostics);
    void setCombinedDiagnostics(
        const std::vector<core::Diagnostic>& workspace,
        const std::vector<core::SctPipelineDiagnostic>& document);
    void setRows(std::vector<DiagnosticRow> rows);
    void clear();
    [[nodiscard]] const DiagnosticRow* rowAt(int row) const noexcept;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role) const override;

private:
    std::vector<DiagnosticRow> rows_{};
};

}  // namespace salsa::qt
