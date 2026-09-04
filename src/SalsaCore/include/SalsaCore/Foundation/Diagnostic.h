#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
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
};

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
