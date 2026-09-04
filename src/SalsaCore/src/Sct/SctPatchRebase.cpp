#include "SalsaCore/Sct/SctPatchRebase.h"

#include "SalsaCore/Foundation/Hashing.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic rebaseError(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctReconciliation,
        std::move(message), std::nullopt};
}

[[nodiscard]] Result<std::string> digestBytes(
    const std::span<const std::byte> bytes) {
    const auto digest = sha256(bytes);
    if (!digest) return Result<std::string>::failure(digest.diagnostics());
    return Result<std::string>::success(digest.value().toHex());
}

[[nodiscard]] std::string decisionId(
    const std::string_view prefix, const std::size_t ordinal) {
    return std::string(prefix) + '-' + std::to_string(ordinal);
}

[[nodiscard]] SctReconciliationDecision pairAsset(
    const AssetLocator& locator, const std::string& incomingKey,
    const std::string& id) {
    return {id, SctReconciliationDecisionKind::PairAsset,
        locator, incomingKey, std::nullopt, std::nullopt};
}

[[nodiscard]] Result<SctReconciliationResult> reconcileCompleteShape(
    const AssetLocator& locator, const SourceRevision& revision,
    const SctSemanticState& baseline, const SctSemanticState& incoming,
    const std::string& scope, std::vector<SctReconciliationDecision> decisions,
    const bool acceptSingleSided) {
    const std::string incomingKey = scope + ":incoming";
    decisions.push_back(pairAsset(locator, incomingKey, scope + "-pair-asset"));
    SctReconciliationRequest request;
    request.decisionScopeId = scope;
    request.targetScopeKey = "patch-rebase";
    request.baselineAssets.push_back({locator, revision, baseline});
    request.incomingAssets.push_back({incomingKey, locator, incoming});
    request.decisions = decisions;
    auto first = SctDocumentReconciler::reconcile(request);
    if (!first || !acceptSingleSided) return first;
    if (first.value().scripts.size() != 1u)
        return Result<SctReconciliationResult>::failure(rebaseError(
            "Rebase reconciliation did not produce exactly one script."));

    std::size_t ordinal = 0;
    for (const auto& entity : first.value().scripts.front().entities) {
        if (entity.baseline && !entity.incoming) {
            request.decisions.push_back({decisionId(scope + "-remove", ordinal++),
                SctReconciliationDecisionKind::BaselineEntityRemoval,
                locator, incomingKey, entity.baseline, std::nullopt});
        } else if (!entity.baseline && entity.incoming) {
            request.decisions.push_back({decisionId(scope + "-add", ordinal++),
                SctReconciliationDecisionKind::IncomingEntityAddition,
                locator, incomingKey, std::nullopt, entity.incoming});
        }
    }
    return SctDocumentReconciler::reconcile(request);
}

[[nodiscard]] std::vector<SctReconciliationDecision> lineagePairs(
    const AssetLocator& locator, const std::string& incomingKey,
    const SctReconciliationResult& oldResult) {
    std::vector<SctReconciliationDecision> result;
    std::size_t ordinal = 0;
    for (const auto& entity : oldResult.scripts.front().entities) {
        if (!entity.candidate || !entity.incoming
            || entity.candidate->index() != entity.incoming->index()) continue;
        result.push_back({decisionId("patch-local-pair", ordinal++),
            SctReconciliationDecisionKind::PairEntity,
            locator, incomingKey, entity.candidate, entity.incoming});
    }
    return result;
}

[[nodiscard]] bool hasBlockingIdentity(const SctReconciliationResult& result) {
    if (result.scripts.size() != 1u || !result.scripts.front().candidate) return true;
    return std::ranges::any_of(result.scripts.front().entities, [](const auto& entity) {
        return entity.baseline && entity.incoming && !entity.resolved;
    });
}

}  // namespace

Result<SctPatchRebasePlan> SctPatchRebaseService::build(
    const SctPatchRebaseRequest& request) {
    if (!request.oldBaseline.document || !request.newBaseline.document
        || request.workspaceId.empty() || request.contextToken.empty())
        return Result<SctPatchRebasePlan>::failure(rebaseError(
            "A rebase requires complete baselines and exact workspace context."));

    auto serializedOldPatch = SalsaScriptPatchCodec::serialize(request.oldPatch);
    if (!serializedOldPatch)
        return Result<SctPatchRebasePlan>::failure(serializedOldPatch.diagnostics());
    auto oldPatchDigest = digestBytes(serializedOldPatch.value());
    if (!oldPatchDigest)
        return Result<SctPatchRebasePlan>::failure(oldPatchDigest.diagnostics());
    auto reconstructed = SalsaScriptPatchService::apply(
        request.oldBaseline, request.oldPatch);
    if (!reconstructed)
        return Result<SctPatchRebasePlan>::failure(reconstructed.diagnostics());

    auto oldAligned = reconcileCompleteShape(request.locator,
        request.newSourceRevision, request.newBaseline, request.oldBaseline,
        "patch-rebase-old", request.reconciliationDecisions, true);
    if (!oldAligned)
        return Result<SctPatchRebasePlan>::failure(oldAligned.diagnostics());
    if (hasBlockingIdentity(oldAligned.value()))
        return Result<SctPatchRebasePlan>::failure(rebaseError(
            "The old and new source lineages require an explicit entity mapping."));
    const auto alignedOld = *oldAligned.value().scripts.front().candidate;

    const std::string localIncomingKey = "patch-rebase-local:incoming";
    auto localDecisions = lineagePairs(
        request.locator, localIncomingKey, oldAligned.value());
    auto localAligned = reconcileCompleteShape(request.locator,
        request.newSourceRevision, alignedOld, reconstructed.value(),
        "patch-rebase-local", std::move(localDecisions), true);
    if (!localAligned)
        return Result<SctPatchRebasePlan>::failure(localAligned.diagnostics());
    if (hasBlockingIdentity(localAligned.value()))
        return Result<SctPatchRebasePlan>::failure(rebaseError(
            "The saved local patch requires an explicit entity mapping."));
    const auto alignedLocal = *localAligned.value().scripts.front().candidate;

    auto merge = SctMergePlanService::build({SctMergeMode::TrueThreeWay,
        request.locator, request.newSourceRevision, alignedOld, alignedLocal,
        request.newBaseline, request.oldPatch.sourceTextConvention,
        request.contextToken});
    if (!merge) return Result<SctPatchRebasePlan>::failure(merge.diagnostics());

    SctPatchRebasePlan plan{"", request.contextToken,
        request.oldSourceRevision, request.newSourceRevision,
        oldPatchDigest.value(), reconstructed.value(), alignedOld, alignedLocal,
        std::move(oldAligned).takeValue(), std::move(localAligned).takeValue(),
        std::move(merge).takeValue()};
    const std::string seed = request.workspaceId + '|' + request.locator.identityKey()
        + '|' + request.oldSourceRevision.digest.toHex() + '|'
        + request.newSourceRevision.digest.toHex() + '|' + plan.oldPatchDigest
        + '|' + plan.mergePlan.id + '|' + request.contextToken;
    const auto id = digestBytes(std::as_bytes(std::span{seed.data(), seed.size()}));
    if (!id) return Result<SctPatchRebasePlan>::failure(id.diagnostics());
    plan.id = id.value();
    return Result<SctPatchRebasePlan>::success(std::move(plan));
}

SctPatchRebasePreview SctPatchRebaseService::preview(
    const SctPatchRebasePlan& plan,
    const std::span<const SctMergeResolution> resolutions,
    const std::string_view currentContextToken) {
    SctPatchRebasePreview result;
    result.merge = SctMergePlanService::preview(
        plan.mergePlan, resolutions, currentContextToken);
    if (result.merge.status != SctMergePreviewStatus::Ready
        || !result.merge.candidate) return result;
    auto patch = SalsaScriptPatchService::diff(plan.mergePlan.request.incoming,
        *result.merge.candidate,
        plan.mergePlan.request.sourceTextConvention);
    if (!patch) {
        result.merge.status = SctMergePreviewStatus::Invalid;
        result.merge.diagnostics.push_back({SctChangeDisposition::InvalidResult,
            "RebasedPatchDiffFailed", "The rebased result could not be represented as a patch."});
        return result;
    }
    auto bytes = SalsaScriptPatchCodec::serialize(patch.value());
    if (!bytes) {
        result.merge.status = SctMergePreviewStatus::Invalid;
        result.merge.diagnostics.push_back({SctChangeDisposition::InvalidResult,
            "RebasedPatchSerializationFailed", "The rebased patch could not be serialized."});
        return result;
    }
    auto decoded = SalsaScriptPatchCodec::deserialize(bytes.value());
    if (!decoded) {
        result.merge.status = SctMergePreviewStatus::Invalid;
        result.merge.diagnostics.push_back({SctChangeDisposition::InvalidResult,
            "RebasedPatchRoundTripFailed", "The rebased patch could not be decoded."});
        return result;
    }
    auto reapplied = SalsaScriptPatchService::apply(
        plan.mergePlan.request.incoming, decoded.value());
    auto reproduction = reapplied
        ? SalsaScriptPatchService::diff(reapplied.value(),
            *result.merge.candidate, std::nullopt)
        : Result<SalsaScriptPatch>::failure(reapplied.diagnostics());
    if (!reapplied || !reapplied.value().document || !result.merge.candidate->document
        || !reproduction || !reproduction.value().empty()) {
        result.merge.status = SctMergePreviewStatus::Invalid;
        result.merge.diagnostics.push_back({SctChangeDisposition::InvalidResult,
            "RebasedPatchReproductionFailed",
            "The serialized rebased patch did not reproduce the reviewed result."});
        return result;
    }
    result.rebasedPatch = std::move(patch).takeValue();
    result.serializedPatch = std::move(bytes).takeValue();
    return result;
}

}  // namespace salsa::core
