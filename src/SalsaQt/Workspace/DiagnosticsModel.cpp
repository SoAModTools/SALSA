#include "Workspace/DiagnosticsModel.h"

#include <QBrush>
#include <QColor>

#include <utility>

namespace salsa::qt {
namespace {

[[nodiscard]] QString severityText(const core::DiagnosticSeverity severity) {
    switch (severity) {
    case core::DiagnosticSeverity::Info:
        return DiagnosticsModel::tr("Information");
    case core::DiagnosticSeverity::Warning:
        return DiagnosticsModel::tr("Warning");
    case core::DiagnosticSeverity::Error:
        return DiagnosticsModel::tr("Error");
    }
    return DiagnosticsModel::tr("Unknown");
}

[[nodiscard]] QString codeText(const core::DiagnosticCode code) {
    const auto name = core::diagnosticCodeName(code);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

[[nodiscard]] std::optional<core::SctInspectionLocation> inspectionLocation(
    const core::SctPipelineDiagnostic& diagnostic) {
    if (!diagnostic.primaryLocation) {
        return diagnostic.target
            ? std::optional<core::SctInspectionLocation>{*diagnostic.target}
            : std::nullopt;
    }
    return core::inspectionLocationForDiagnostic(*diagnostic.primaryLocation);
}

}  // namespace

DiagnosticsModel::DiagnosticsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void DiagnosticsModel::setDiagnostics(std::vector<core::Diagnostic> diagnostics) {
    std::vector<DiagnosticRow> rows;
    rows.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
        if (!core::isCurrentDiagnosticSeverity(diagnostic.severity)) continue;
        rows.push_back({ diagnostic.severity, codeText(diagnostic.code),
            QString::fromStdString(diagnostic.message),
            diagnostic.path.has_value() ? QString::fromStdWString(diagnostic.path->wstring()) : QString{},
            QString::fromStdString(diagnostic.message),
            std::nullopt, std::nullopt });
    }
    setRows(std::move(rows));
}

void DiagnosticsModel::setRows(std::vector<DiagnosticRow> rows) {
    if (rows_ == rows) return;
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

void DiagnosticsModel::setCombinedDiagnostics(
    const std::vector<core::Diagnostic>& workspace,
    const std::vector<core::SctPipelineDiagnostic>& document) {
    setRows(rowsFor(workspace, document));
}

std::vector<DiagnosticRow> DiagnosticsModel::rowsFor(
    const std::vector<core::Diagnostic>& workspace,
    const std::vector<core::SctPipelineDiagnostic>& document) {
    std::vector<DiagnosticRow> rows;
    rows.reserve(workspace.size() + document.size());
    for (const auto& diagnostic : workspace) {
        if (!core::isCurrentDiagnosticSeverity(diagnostic.severity)) continue;
        rows.push_back({ diagnostic.severity, codeText(diagnostic.code),
            QString::fromStdString(diagnostic.message),
            diagnostic.path.has_value() ? QString::fromStdWString(diagnostic.path->wstring()) : QString{},
            QString::fromStdString(diagnostic.message),
            std::nullopt, std::nullopt });
    }
    for (const auto& diagnostic : document) {
        if (!core::isCurrentDiagnosticSeverity(diagnostic.severity)) continue;
        QString location;
        if (diagnostic.locator.has_value())
            location = QString::fromStdWString(diagnostic.locator->path().wstring());
        if (diagnostic.payloadOffset.has_value())
            location += QStringLiteral(" @ 0x%1").arg(*diagnostic.payloadOffset, 0, 16);
        if (diagnostic.primaryLocation)
            location += QStringLiteral(" — ") + QString::fromStdString(
                core::formatSctDiagnosticLocation(*diagnostic.primaryLocation));
        if (!diagnostic.relatedLocations.empty())
            location += tr(" (+%1 related)").arg(diagnostic.relatedLocations.size());
        QString tooltip = QString::fromStdString(diagnostic.message);
        for (const auto& related : diagnostic.relatedLocations)
            tooltip += QStringLiteral("\nRelated: ") + QString::fromStdString(
                core::formatSctDiagnosticLocation(related));
        rows.push_back({ diagnostic.severity, QString::fromStdString(diagnostic.code),
            QString::fromStdString(diagnostic.message), std::move(location),
            std::move(tooltip),
            diagnostic.locator, inspectionLocation(diagnostic) });
    }
    return rows;
}

void DiagnosticsModel::clear() {
    setRows({});
}

const DiagnosticRow* DiagnosticsModel::rowAt(const int row) const noexcept {
    return row >= 0 && static_cast<std::size_t>(row) < rows_.size()
        ? &rows_[static_cast<std::size_t>(row)] : nullptr;
}

int DiagnosticsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int DiagnosticsModel::columnCount(const QModelIndex&) const {
    return 4;
}

QVariant DiagnosticsModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const auto& diagnostic = rows_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return severityText(diagnostic.severity);
        case 1: return diagnostic.code;
        case 2: return diagnostic.message;
        case 3: return diagnostic.location;
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (diagnostic.severity) {
        case core::DiagnosticSeverity::Info: return QBrush(QColor(55, 100, 160));
        case core::DiagnosticSeverity::Warning: return QBrush(QColor(170, 105, 0));
        case core::DiagnosticSeverity::Error: return QBrush(QColor(180, 45, 45));
        }
    }
    if (role == Qt::ToolTipRole) {
        return diagnostic.tooltip.isEmpty() ? diagnostic.message : diagnostic.tooltip;
    }
    return {};
}

QVariant DiagnosticsModel::headerData(
    const int section,
    const Qt::Orientation orientation,
    const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case 0: return tr("Severity");
    case 1: return tr("Code");
    case 2: return tr("Message");
    case 3: return tr("Location");
    default: return {};
    }
}

}  // namespace salsa::qt
