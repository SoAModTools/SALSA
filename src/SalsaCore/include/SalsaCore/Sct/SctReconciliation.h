#pragma once

#include "SalsaCore/Sct/SctChangePlan.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace salsa::core {

using SctReconciliationEntityId = std::variant<
    spice::sct::SctSectionId,
    spice::sct::SctInstructionId,
    spice::sct::SctStringId,
    spice::sct::SctFooterEntryId,
    spice::sct::SctOpaqueAttachmentId,
    SctAuthoredArmId>;

enum class SctReconciliationDecisionKind {
    PairAsset,
    IncomingAssetAddition,
    BaselineAssetRemoval,
    PairEntity,
    IncomingEntityAddition,
    BaselineEntityRemoval,
};

struct SctReconciliationDecision final {
    std::string id{};
    SctReconciliationDecisionKind kind =
        SctReconciliationDecisionKind::PairEntity;
    std::optional<AssetLocator> baselineAsset{};
    std::optional<std::string> incomingAssetKey{};
    std::optional<SctReconciliationEntityId> baselineEntity{};
    std::optional<SctReconciliationEntityId> incomingEntity{};
    auto operator<=>(const SctReconciliationDecision&) const = default;
};

struct SctReconciliationBaselineAsset final {
    AssetLocator locator;
    SourceRevision revision;
    SctSemanticState state{};
};

struct SctReconciliationIncomingAsset final {
    std::string key{};
    std::optional<AssetLocator> proposedLocator{};
    SctSemanticState state{};
};

enum class SctReconciliationProgressPhase {
    Preflight,
    AssetMatching,
    EntityMatching,
    GraphRefinement,
    CandidateConstruction,
    ChangePlan,
};

struct SctReconciliationProgress final {
    SctReconciliationProgressPhase phase =
        SctReconciliationProgressPhase::Preflight;
    std::size_t completedAssets = 0;
    std::size_t totalAssets = 0;
    std::optional<AssetLocator> baselineAsset{};
    std::optional<std::string> incomingAssetKey{};
};

using SctReconciliationProgressObserver =
    std::function<void(const SctReconciliationProgress&)>;

struct SctReconciliationDecisionBinding final {
    std::optional<AssetLocator> baselineAsset{};
    std::optional<SourceRevision> baselineRevision{};
    std::optional<std::string> incomingAssetKey{};
    std::optional<std::string> incomingFingerprint{};
    std::vector<SctReconciliationDecision> decisions{};
};

struct SctReconciliationDecisionArtifact final {
    std::string decisionScopeId{};
    std::string targetScopeKey{};
    std::uint32_t reconciliationContractVersion = 1;
    std::vector<SctReconciliationDecisionBinding> assets{};
};

struct SctReconciliationRequest final {
    std::string decisionScopeId{};
    std::string targetScopeKey{};
    std::vector<SctReconciliationBaselineAsset> baselineAssets{};
    std::vector<SctReconciliationIncomingAsset> incomingAssets{};
    std::vector<SctReconciliationDecision> decisions{};
    std::optional<std::reference_wrapper<const SctReconciliationDecisionArtifact>>
        persistedDecisions{};
    std::stop_token stopToken{};
    SctReconciliationProgressObserver progress{};
};

struct SctEntityCorrespondence final {
    std::optional<SctReconciliationEntityId> baseline{};
    std::optional<SctReconciliationEntityId> incoming{};
    std::optional<SctReconciliationEntityId> candidate{};
    SctIdentityMatchStatus status = SctIdentityMatchStatus::Unmatched;
    SctIdentityMatchProvenance provenance =
        SctIdentityMatchProvenance::Automatic;
    std::vector<SctIdentityEvidence> evidence{};
    bool resolved = false;
    bool provisional = false;
    bool representable = true;
};

struct SctAssetCorrespondence final {
    std::optional<AssetLocator> baseline{};
    std::optional<std::string> incomingKey{};
    SctIdentityMatchStatus status = SctIdentityMatchStatus::Unmatched;
    SctIdentityMatchProvenance provenance =
        SctIdentityMatchProvenance::Automatic;
    std::vector<SctIdentityEvidence> evidence{};
    bool resolved = false;
};

struct SctReconciledScript final {
    AssetLocator locator;
    std::string incomingKey{};
    std::vector<SctEntityCorrespondence> entities{};
    std::optional<SctSemanticState> candidate{};
    std::vector<SctChangeDiagnostic> diagnostics{};
};

struct SctReconciliationResult final {
    std::string id{};
    bool cancelled = false;
    std::vector<SctAssetCorrespondence> assets{};
    std::vector<SctReconciledScript> scripts{};
    std::optional<SctChangePlan> changePlan{};
    std::vector<SctChangeDiagnostic> diagnostics{};
    std::optional<SctReconciliationDecisionArtifact> boundDecisions{};
};

class SctDocumentReconciler final {
public:
    static constexpr std::uint32_t ContractVersion = 1;

    [[nodiscard]] static Result<SctReconciliationResult> reconcile(
        const SctReconciliationRequest& request);
    [[nodiscard]] static Result<std::string> semanticFingerprint(
        const SctSemanticState& state);
};

}  // namespace salsa::core
