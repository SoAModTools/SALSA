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
    switch (code) {
    case core::DiagnosticCode::Cancelled: return QStringLiteral("Cancelled");
    case core::DiagnosticCode::InvalidDatasetRoot: return QStringLiteral("InvalidDatasetRoot");
    case core::DiagnosticCode::DatasetEnumerationFailed: return QStringLiteral("DatasetEnumerationFailed");
    case core::DiagnosticCode::NoSctAssets: return QStringLiteral("NoSctAssets");
    case core::DiagnosticCode::InvalidAssetLocator: return QStringLiteral("InvalidAssetLocator");
    case core::DiagnosticCode::DuplicateAssetLocator: return QStringLiteral("DuplicateAssetLocator");
    case core::DiagnosticCode::AssetOutsideDataset: return QStringLiteral("AssetOutsideDataset");
    case core::DiagnosticCode::AssetNotFound: return QStringLiteral("AssetNotFound");
    case core::DiagnosticCode::AssetReadFailed: return QStringLiteral("AssetReadFailed");
    case core::DiagnosticCode::SourceChanged: return QStringLiteral("SourceChanged");
    case core::DiagnosticCode::SctParseFailed: return QStringLiteral("SctParseFailed");
    case core::DiagnosticCode::SctImportFailed: return QStringLiteral("SctImportFailed");
    case core::DiagnosticCode::ReparsePointSkipped: return QStringLiteral("ReparsePointSkipped");
    case core::DiagnosticCode::HashInitializationFailed: return QStringLiteral("HashInitializationFailed");
    case core::DiagnosticCode::HashUpdateFailed: return QStringLiteral("HashUpdateFailed");
    case core::DiagnosticCode::HashFinalizationFailed: return QStringLiteral("HashFinalizationFailed");
    case core::DiagnosticCode::HashStateInvalid: return QStringLiteral("HashStateInvalid");
    case core::DiagnosticCode::MalformedPersistenceJson: return QStringLiteral("MalformedPersistenceJson");
    case core::DiagnosticCode::InvalidPatchEnvelope: return QStringLiteral("InvalidPatchEnvelope");
    case core::DiagnosticCode::UnsupportedPersistenceSchemaVersion: return QStringLiteral("UnsupportedPersistenceSchemaVersion");
    case core::DiagnosticCode::PatchPayloadCorrupt: return QStringLiteral("PatchPayloadCorrupt");
    case core::DiagnosticCode::PersistenceReadFailed: return QStringLiteral("PersistenceReadFailed");
    case core::DiagnosticCode::PersistenceWriteFailed: return QStringLiteral("PersistenceWriteFailed");
    case core::DiagnosticCode::PersistenceReplaceFailed: return QStringLiteral("PersistenceReplaceFailed");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] std::optional<core::SctInspectionLocation> inspectionLocation(
    const core::SctPipelineDiagnostic& diagnostic) {
    if (!diagnostic.target.has_value()) return std::nullopt;
    if (diagnostic.target->kind != core::SctNavigationKind::Instruction)
        return core::SctInspectionLocation{ *diagnostic.target };

    const auto instruction = spice::sct::SctInstructionId(diagnostic.target->id);
    if (diagnostic.schemaIndex.has_value()) {
        const spice::sct::SctParameterAddress parameter{
            *diagnostic.schemaIndex, diagnostic.repeatedGroupOrdinal };
        if (!diagnostic.expressionChildPath.empty()) {
            return core::SctInspectionLocation{ core::SctExpressionSite{
                instruction, parameter, diagnostic.expressionChildPath } };
        }
        return core::SctInspectionLocation{
            core::SctParameterSite{ instruction, parameter } };
    }
    if (!diagnostic.expressionChildPath.empty()) {
        return core::SctInspectionLocation{ core::SctExpressionSite{
            instruction, core::SctScheduledExpressionSite{},
            diagnostic.expressionChildPath } };
    }
    return core::SctInspectionLocation{ *diagnostic.target };
}

}  // namespace

DiagnosticsModel::DiagnosticsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void DiagnosticsModel::setDiagnostics(std::vector<core::Diagnostic> diagnostics) {
    std::vector<DiagnosticRow> rows;
    rows.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
        rows.push_back({ diagnostic.severity, codeText(diagnostic.code),
            QString::fromStdString(diagnostic.message),
            diagnostic.path.has_value() ? QString::fromStdWString(diagnostic.path->wstring()) : QString{},
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
    std::vector<DiagnosticRow> rows;
    rows.reserve(workspace.size() + document.size());
    for (const auto& diagnostic : workspace) {
        rows.push_back({ diagnostic.severity, codeText(diagnostic.code),
            QString::fromStdString(diagnostic.message),
            diagnostic.path.has_value() ? QString::fromStdWString(diagnostic.path->wstring()) : QString{},
            std::nullopt, std::nullopt });
    }
    for (const auto& diagnostic : document) {
        QString location;
        if (diagnostic.locator.has_value())
            location = QString::fromStdWString(diagnostic.locator->path().wstring());
        if (diagnostic.payloadOffset.has_value())
            location += QStringLiteral(" @ 0x%1").arg(*diagnostic.payloadOffset, 0, 16);
        if (diagnostic.schemaIndex.has_value())
            location += QStringLiteral(" parameter %1").arg(*diagnostic.schemaIndex);
        if (diagnostic.repeatedGroupOrdinal.has_value())
            location += QStringLiteral(" group %1").arg(*diagnostic.repeatedGroupOrdinal);
        if (!diagnostic.expressionChildPath.empty()) {
            location += QStringLiteral(" expression");
            for (const auto child : diagnostic.expressionChildPath)
                location += QStringLiteral("/%1").arg(child);
        }
        if (diagnostic.textOffset.has_value())
            location += QStringLiteral(" text[%1..%2)").arg(*diagnostic.textOffset)
                .arg(*diagnostic.textOffset + diagnostic.textSize.value_or(0));
        rows.push_back({ diagnostic.severity, QString::fromStdString(diagnostic.code),
            QString::fromStdString(diagnostic.message), std::move(location),
            diagnostic.locator, inspectionLocation(diagnostic) });
    }
    setRows(std::move(rows));
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
        return diagnostic.message;
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
    case 3: return tr("Path");
    default: return {};
    }
}

}  // namespace salsa::qt
