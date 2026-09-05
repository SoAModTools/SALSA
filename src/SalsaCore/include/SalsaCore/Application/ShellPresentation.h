#pragma once

#include <string>
#include <string_view>

namespace salsa::core {

enum class ShellAction {
    OpenDataset,
    OpenRecentDataset,
    ImportLegacyProject,
    CloseDataset,
    AssociateWorkspace,
    CloseWorkspace,
    RebasePatches,
    CleanWorkspaceEvidence,
    PromoteLegacyMetadata,
    EditPersonalCatalog,
    EditWorkspaceAppearance,
    RefreshDataset,
};

enum class ShellActionBlockReason {
    None,
    OperationActive,
    DatasetRequired,
    WorkspaceRequired,
    DocumentsMustClose,
    NoRecentDatasets,
};

struct ShellPresentationState final {
    bool operationActive = false;
    bool datasetLoaded = false;
    bool workspaceAssociated = false;
    bool documentsOpen = false;
    bool recentDatasetsAvailable = false;
};

[[nodiscard]] inline std::string composeShellWindowTitle(
    const std::string_view applicationName, const std::string_view datasetName = {},
    const std::string_view documentName = {}, const bool documentModified = false) {
    std::string result;
    const auto append = [&result](const std::string_view part) {
        if (part.empty()) return;
        if (!result.empty()) result += " — ";
        result += part;
    };
    if (!documentName.empty()) {
        if (documentModified) result += '*';
        result += documentName;
    }
    append(datasetName);
    append(applicationName);
    return result;
}

[[nodiscard]] constexpr ShellActionBlockReason shellActionBlockReason(
    const ShellAction action, const ShellPresentationState& state) noexcept {
    if (state.operationActive) return ShellActionBlockReason::OperationActive;
    switch (action) {
    case ShellAction::OpenDataset:
    case ShellAction::ImportLegacyProject:
    case ShellAction::EditPersonalCatalog:
        return ShellActionBlockReason::None;
    case ShellAction::OpenRecentDataset:
        return state.recentDatasetsAvailable ? ShellActionBlockReason::None
                                             : ShellActionBlockReason::NoRecentDatasets;
    case ShellAction::CloseDataset:
    case ShellAction::RefreshDataset:
        return state.datasetLoaded ? ShellActionBlockReason::None
                                   : ShellActionBlockReason::DatasetRequired;
    case ShellAction::AssociateWorkspace:
        if (!state.datasetLoaded) return ShellActionBlockReason::DatasetRequired;
        return state.documentsOpen ? ShellActionBlockReason::DocumentsMustClose
                                   : ShellActionBlockReason::None;
    case ShellAction::CloseWorkspace:
        if (!state.workspaceAssociated) return ShellActionBlockReason::WorkspaceRequired;
        return state.documentsOpen ? ShellActionBlockReason::DocumentsMustClose
                                   : ShellActionBlockReason::None;
    case ShellAction::RebasePatches:
        if (!state.datasetLoaded) return ShellActionBlockReason::DatasetRequired;
        return state.workspaceAssociated ? ShellActionBlockReason::None
                                         : ShellActionBlockReason::WorkspaceRequired;
    case ShellAction::CleanWorkspaceEvidence:
    case ShellAction::EditWorkspaceAppearance:
        return state.workspaceAssociated ? ShellActionBlockReason::None
                                         : ShellActionBlockReason::WorkspaceRequired;
    case ShellAction::PromoteLegacyMetadata:
        if (!state.workspaceAssociated) return ShellActionBlockReason::WorkspaceRequired;
        return state.documentsOpen ? ShellActionBlockReason::DocumentsMustClose
                                   : ShellActionBlockReason::None;
    }
    return ShellActionBlockReason::OperationActive;
}

enum class DatasetOperationPresentation {
    AwaitRestoration,
    Dismiss,
    ShowSummary,
};

[[nodiscard]] constexpr DatasetOperationPresentation datasetOperationPresentation(
    const bool opening, const bool success, const bool cancelled,
    const bool hasCurrentDiagnostics) noexcept {
    if (!success || cancelled || hasCurrentDiagnostics)
        return DatasetOperationPresentation::ShowSummary;
    return opening ? DatasetOperationPresentation::AwaitRestoration
                   : DatasetOperationPresentation::Dismiss;
}

[[nodiscard]] constexpr DatasetOperationPresentation restoredDatasetPresentation(
    const bool hasCurrentDiagnostics, const bool hasRestorationIssues) noexcept {
    return hasCurrentDiagnostics || hasRestorationIssues
        ? DatasetOperationPresentation::ShowSummary
        : DatasetOperationPresentation::Dismiss;
}

}  // namespace salsa::core
