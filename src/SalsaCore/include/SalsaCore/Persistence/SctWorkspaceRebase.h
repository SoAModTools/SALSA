#pragma once

#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"
#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Sct/SctPatchRebase.h"

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace salsa::core {

struct SctStalePatchCandidate final {
    AssetLocator locator;
    SourceRevision oldSourceRevision;
    std::optional<SourceRevision> newSourceRevision{};
    std::filesystem::path patchPath{};
    std::string patchFileDigest{};
    std::string contextToken{};
    bool sourceMissing = false;
};

struct SctRebaseDiscovery final {
    std::vector<SctStalePatchCandidate> candidates{};
    std::vector<Diagnostic> diagnostics{};
};

struct SctWorkspaceRebasePlan final {
    SctStalePatchCandidate candidate;
    SourceAssetSnapshot newSource;
    std::shared_ptr<const SctDocumentSnapshot> newBaselineSnapshot{};
    PatchEnvelope oldEnvelope;
    SctPatchRebasePlan corePlan;
};

struct SctWorkspaceRebaseCommitResult final {
    bool committed = false;
    bool stale = false;
    WorkspaceTransactionResult transaction{};
    std::vector<AssetLocator> committedAssets{};
    std::vector<Diagnostic> diagnostics{};
};

struct SctWorkspaceRebaseCommitSelection final {
    const SctWorkspaceRebasePlan* plan = nullptr;
    const SctPatchRebasePreview* preview = nullptr;
    std::span<const SctMergeResolution> resolutions{};
};

class SctWorkspaceRebaseService final {
public:
    [[nodiscard]] static SctRebaseDiscovery discover(
        const LocalGameProject& project,
        const LocalSalsaWorkspace& workspace,
        std::stop_token stopToken = {});
    [[nodiscard]] static Result<SctWorkspaceRebasePlan> build(
        const LocalGameProject& project,
        const LocalSalsaWorkspace& workspace,
        const SctStalePatchCandidate& candidate,
        std::span<const SctReconciliationDecision> decisions = {},
        std::stop_token stopToken = {});
    [[nodiscard]] static SctWorkspaceRebaseCommitResult commit(
        const LocalGameProject& project,
        const LocalSalsaWorkspace& workspace,
        const SctWorkspaceRebasePlan& plan,
        const SctPatchRebasePreview& preview,
        std::span<const SctMergeResolution> resolutions);
    [[nodiscard]] static SctWorkspaceRebaseCommitResult commitSelected(
        const LocalGameProject& project,
        const LocalSalsaWorkspace& workspace,
        std::span<const SctWorkspaceRebaseCommitSelection> selections);
};

}  // namespace salsa::core
