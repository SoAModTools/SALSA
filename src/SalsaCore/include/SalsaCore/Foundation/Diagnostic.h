#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

enum class DiagnosticSeverity {
    Info,
    Warning,
    Error,
};

enum class DiagnosticCode {
    Cancelled,
    InvalidDatasetRoot,
    DatasetEnumerationFailed,
    NoSctAssets,
    InvalidAssetLocator,
    DuplicateAssetLocator,
    AssetOutsideDataset,
    AssetNotFound,
    AssetReadFailed,
    SourceChanged,
    SctParseFailed,
    SctImportFailed,
    ReparsePointSkipped,
    HashInitializationFailed,
    HashUpdateFailed,
    HashFinalizationFailed,
    HashStateInvalid,
    MalformedPersistenceJson,
    InvalidPatchEnvelope,
    UnsupportedPersistenceSchemaVersion,
    PatchPayloadCorrupt,
    PersistenceReadFailed,
    PersistenceWriteFailed,
    PersistenceReplaceFailed,
    InvalidSalsaWorkspace,
    WorkspaceDatasetReassociationRequired,
    InvalidWorkspaceSession,
    UnsupportedSctPatchSchema,
    InvalidSctPatch,
    UnsupportedSctFragmentSchema,
    InvalidSctFragment,
    SctFragmentTooLarge,
    SctPatchSourceMismatch,
    SctPatchApplyFailed,
    SctPatchVerificationFailed,
    SctBaselineMissing,
    SctBaselineCorrupt,
    InvalidSctReconciliation,
    InvalidWorkspaceTransaction,
    WorkspaceRecoveryRequired,
    WorkspaceCleanupFailed,
    SctExportFailed,
    PublicationSourceChanged,
    PublicationSourceReplacementNotConfirmed,
    PublicationWriteFailed,
    LegacyConversionNotTrusted,
    LegacyConversionInvalidRequest,
    LegacyConversionFailed,
    LegacyConversionCancelled,
    LegacyConversionResourceLimit,
    LegacyConversionIsolationFailed,
    LegacyCapsuleInvalid,
    LegacyCapsuleUnsupportedSchema,
    LegacyCapsuleIntegrityFailed,
    LegacyImportInvalidRequest,
    LegacyImportDestinationNotFresh,
    InvalidSctAuthoringProject,
    UnresolvedSctAuthoringBinding,
    InvalidSctAuthoringPresentation,
    SctSourceArtifactOmitted,
};

[[nodiscard]] constexpr bool isCurrentDiagnosticSeverity(
    const DiagnosticSeverity severity) noexcept {
    return severity == DiagnosticSeverity::Warning
        || severity == DiagnosticSeverity::Error;
}

[[nodiscard]] constexpr std::string_view diagnosticCodeName(
    const DiagnosticCode code) noexcept {
    switch (code) {
    case DiagnosticCode::Cancelled: return "Cancelled";
    case DiagnosticCode::InvalidDatasetRoot: return "InvalidDatasetRoot";
    case DiagnosticCode::DatasetEnumerationFailed: return "DatasetEnumerationFailed";
    case DiagnosticCode::NoSctAssets: return "NoSctAssets";
    case DiagnosticCode::InvalidAssetLocator: return "InvalidAssetLocator";
    case DiagnosticCode::DuplicateAssetLocator: return "DuplicateAssetLocator";
    case DiagnosticCode::AssetOutsideDataset: return "AssetOutsideDataset";
    case DiagnosticCode::AssetNotFound: return "AssetNotFound";
    case DiagnosticCode::AssetReadFailed: return "AssetReadFailed";
    case DiagnosticCode::SourceChanged: return "SourceChanged";
    case DiagnosticCode::SctParseFailed: return "SctParseFailed";
    case DiagnosticCode::SctImportFailed: return "SctImportFailed";
    case DiagnosticCode::ReparsePointSkipped: return "ReparsePointSkipped";
    case DiagnosticCode::HashInitializationFailed: return "HashInitializationFailed";
    case DiagnosticCode::HashUpdateFailed: return "HashUpdateFailed";
    case DiagnosticCode::HashFinalizationFailed: return "HashFinalizationFailed";
    case DiagnosticCode::HashStateInvalid: return "HashStateInvalid";
    case DiagnosticCode::MalformedPersistenceJson: return "MalformedPersistenceJson";
    case DiagnosticCode::InvalidPatchEnvelope: return "InvalidPatchEnvelope";
    case DiagnosticCode::UnsupportedPersistenceSchemaVersion: return "UnsupportedPersistenceSchemaVersion";
    case DiagnosticCode::PatchPayloadCorrupt: return "PatchPayloadCorrupt";
    case DiagnosticCode::PersistenceReadFailed: return "PersistenceReadFailed";
    case DiagnosticCode::PersistenceWriteFailed: return "PersistenceWriteFailed";
    case DiagnosticCode::PersistenceReplaceFailed: return "PersistenceReplaceFailed";
    case DiagnosticCode::InvalidSalsaWorkspace: return "InvalidSalsaWorkspace";
    case DiagnosticCode::WorkspaceDatasetReassociationRequired: return "WorkspaceDatasetReassociationRequired";
    case DiagnosticCode::InvalidWorkspaceSession: return "InvalidWorkspaceSession";
    case DiagnosticCode::UnsupportedSctPatchSchema: return "UnsupportedSctPatchSchema";
    case DiagnosticCode::InvalidSctPatch: return "InvalidSctPatch";
    case DiagnosticCode::UnsupportedSctFragmentSchema: return "UnsupportedSctFragmentSchema";
    case DiagnosticCode::InvalidSctFragment: return "InvalidSctFragment";
    case DiagnosticCode::SctFragmentTooLarge: return "SctFragmentTooLarge";
    case DiagnosticCode::SctPatchSourceMismatch: return "SctPatchSourceMismatch";
    case DiagnosticCode::SctPatchApplyFailed: return "SctPatchApplyFailed";
    case DiagnosticCode::SctPatchVerificationFailed: return "SctPatchVerificationFailed";
    case DiagnosticCode::SctBaselineMissing: return "SctBaselineMissing";
    case DiagnosticCode::SctBaselineCorrupt: return "SctBaselineCorrupt";
    case DiagnosticCode::InvalidSctReconciliation: return "InvalidSctReconciliation";
    case DiagnosticCode::InvalidWorkspaceTransaction: return "InvalidWorkspaceTransaction";
    case DiagnosticCode::WorkspaceRecoveryRequired: return "WorkspaceRecoveryRequired";
    case DiagnosticCode::WorkspaceCleanupFailed: return "WorkspaceCleanupFailed";
    case DiagnosticCode::SctExportFailed: return "SctExportFailed";
    case DiagnosticCode::PublicationSourceChanged: return "PublicationSourceChanged";
    case DiagnosticCode::PublicationSourceReplacementNotConfirmed: return "PublicationSourceReplacementNotConfirmed";
    case DiagnosticCode::PublicationWriteFailed: return "PublicationWriteFailed";
    case DiagnosticCode::LegacyConversionNotTrusted: return "LegacyConversionNotTrusted";
    case DiagnosticCode::LegacyConversionInvalidRequest: return "LegacyConversionInvalidRequest";
    case DiagnosticCode::LegacyConversionFailed: return "LegacyConversionFailed";
    case DiagnosticCode::LegacyConversionCancelled: return "LegacyConversionCancelled";
    case DiagnosticCode::LegacyConversionResourceLimit: return "LegacyConversionResourceLimit";
    case DiagnosticCode::LegacyConversionIsolationFailed: return "LegacyConversionIsolationFailed";
    case DiagnosticCode::LegacyCapsuleInvalid: return "LegacyCapsuleInvalid";
    case DiagnosticCode::LegacyCapsuleUnsupportedSchema: return "LegacyCapsuleUnsupportedSchema";
    case DiagnosticCode::LegacyCapsuleIntegrityFailed: return "LegacyCapsuleIntegrityFailed";
    case DiagnosticCode::LegacyImportInvalidRequest: return "LegacyImportInvalidRequest";
    case DiagnosticCode::LegacyImportDestinationNotFresh: return "LegacyImportDestinationNotFresh";
    case DiagnosticCode::InvalidSctAuthoringProject: return "InvalidSctAuthoringProject";
    case DiagnosticCode::UnresolvedSctAuthoringBinding: return "UnresolvedSctAuthoringBinding";
    case DiagnosticCode::InvalidSctAuthoringPresentation: return "InvalidSctAuthoringPresentation";
    case DiagnosticCode::SctSourceArtifactOmitted: return "SctSourceArtifactOmitted";
    }
    return "Unknown";
}

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    DiagnosticCode code = DiagnosticCode::InvalidDatasetRoot;
    std::string message{};
    std::optional<std::filesystem::path> path{};
};

[[nodiscard]] inline bool hasErrors(const std::vector<Diagnostic>& diagnostics) noexcept {
    return std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

}  // namespace salsa::core
