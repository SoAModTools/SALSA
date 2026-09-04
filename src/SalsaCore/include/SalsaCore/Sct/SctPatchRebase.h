#pragma once

#include "SalsaCore/Sct/SctMerge.h"
#include "SalsaCore/Sct/SctReconciliation.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {

struct SctPatchRebaseRequest final {
    AssetLocator locator;
    SourceRevision oldSourceRevision;
    SourceRevision newSourceRevision;
    SctSemanticState oldBaseline{};
    SalsaScriptPatch oldPatch{};
    SctSemanticState newBaseline{};
    std::string workspaceId{};
    std::string contextToken{};
    std::vector<SctReconciliationDecision> reconciliationDecisions{};
};

struct SctPatchRebasePlan final {
    std::string id{};
    std::string contextToken{};
    SourceRevision oldSourceRevision;
    SourceRevision newSourceRevision;
    std::string oldPatchDigest{};
    SctSemanticState reconstructedLocal{};
    SctSemanticState alignedOldBaseline{};
    SctSemanticState alignedLocal{};
    SctReconciliationResult baselineReconciliation{};
    SctReconciliationResult localReconciliation{};
    SctMergePlan mergePlan;
};

struct SctPatchRebasePreview final {
    SctMergePreview merge{};
    std::optional<SalsaScriptPatch> rebasedPatch{};
    std::vector<std::byte> serializedPatch{};
};

class SctPatchRebaseService final {
public:
    [[nodiscard]] static Result<SctPatchRebasePlan> build(
        const SctPatchRebaseRequest& request);
    [[nodiscard]] static SctPatchRebasePreview preview(
        const SctPatchRebasePlan& plan,
        std::span<const SctMergeResolution> resolutions,
        std::string_view currentContextToken);
};

}  // namespace salsa::core
