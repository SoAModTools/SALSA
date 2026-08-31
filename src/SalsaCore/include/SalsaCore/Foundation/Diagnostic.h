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
    ReparsePointSkipped,
    HashInitializationFailed,
    HashUpdateFailed,
    HashFinalizationFailed,
    HashStateInvalid,
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
