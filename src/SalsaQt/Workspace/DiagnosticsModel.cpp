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

}  // namespace

DiagnosticsModel::DiagnosticsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void DiagnosticsModel::setDiagnostics(std::vector<core::Diagnostic> diagnostics) {
    beginResetModel();
    diagnostics_ = std::move(diagnostics);
    endResetModel();
}

void DiagnosticsModel::clear() {
    setDiagnostics({});
}

int DiagnosticsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(diagnostics_.size());
}

int DiagnosticsModel::columnCount(const QModelIndex&) const {
    return 4;
}

QVariant DiagnosticsModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || static_cast<std::size_t>(index.row()) >= diagnostics_.size()) {
        return {};
    }
    const auto& diagnostic = diagnostics_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return severityText(diagnostic.severity);
        case 1: return codeText(diagnostic.code);
        case 2: return QString::fromStdString(diagnostic.message);
        case 3:
            return diagnostic.path.has_value()
                ? QString::fromStdWString(diagnostic.path->wstring())
                : QString{};
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
        return QString::fromStdString(diagnostic.message);
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
