#include "SalsaCore/Persistence/SctReconciliationDecisionStore.h"
#include "SalsaCore/Sct/SctReconciliation.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <stop_token>
#include <tuple>

namespace salsa::core {
namespace {
using namespace spice::sct;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-reconciliation-" + std::to_wstring(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
private:
    std::filesystem::path path_;
};

[[nodiscard]] Sha256Digest digest(const std::string_view value) {
    return sha256(std::as_bytes(std::span{value.data(), value.size()})).value();
}

[[nodiscard]] AssetLocator locator(const std::string_view path) {
    return AssetLocator::fromRelativePath(std::filesystem::path(path)).value();
}

[[nodiscard]] SctDocument document(const std::size_t skippedIds = 0,
    const std::initializer_list<std::uint16_t> opcodes = {9u, 125u, 12u}) {
    SctDocumentBuilder builder;
    for (std::size_t index = 0; index < skippedIds; ++index) {
        (void)builder.allocateSectionId();
        (void)builder.document().allocateInstructionId();
        (void)builder.allocateFooterEntryId();
    }
    SctScriptSectionContent script;
    for (const auto opcode : opcodes) {
        const auto draft = SctInstructionFactory::createDraft({opcode});
        EXPECT_TRUE(draft.draft.has_value());
        const auto built = SctInstructionFactory::materialize(
            builder.document(), *draft.draft);
        EXPECT_TRUE(built.instruction.has_value());
        script.instructions.push_back(*built.instruction);
    }
    builder.document().sections.push_back(
        {builder.allocateSectionId(), "SCRIPT", std::move(script)});
    builder.document().footerEntries.push_back({builder.allocateFooterEntryId(),
        SctTextKind::PlainString, SctPlainText{"message"}});
    return std::move(builder).finish();
}

[[nodiscard]] SctSemanticState state(const SctDocument& value) {
    return {std::make_shared<const SctDocument>(value)};
}

[[nodiscard]] SctSemanticState state(const SctDocument& value,
    std::vector<SctAuthoredArm> arms) {
    return {std::make_shared<const SctDocument>(value), std::move(arms)};
}

[[nodiscard]] SctReconciliationRequest request(const SctDocument& baseline,
    const SctDocument& incoming) {
    const auto asset = locator("scripts/a001a.sct");
    SctReconciliationRequest value;
    value.decisionScopeId = "legacy-project";
    value.targetScopeKey = "workspace";
    value.baselineAssets.push_back(
        {asset, SourceRevision{digest("baseline")}, state(baseline)});
    value.incomingAssets.push_back({"legacy:a001a", asset, state(incoming)});
    return value;
}

[[nodiscard]] const SctChangeUnit* instructionUnit(
    const SctReconciliationResult& result, const std::uint64_t id) {
    if (!result.changePlan || result.changePlan->scripts.empty()) return nullptr;
    const auto& units = result.changePlan->scripts.front().units;
    const auto found = std::ranges::find_if(units, [&](const auto& unit) {
        return unit.entityKind == SctChangeEntityKind::Instruction
            && unit.target && unit.target->id == id;
    });
    return found == units.end() ? nullptr : &*found;
}

TEST(SctReconciliationTest, ReidentifiesEquivalentIndependentDocumentsWithoutChanges) {
    const auto baseline = document();
    const auto incoming = document(8);
    const auto reconciled = SctDocumentReconciler::reconcile(
        request(baseline, incoming));
    ASSERT_TRUE(reconciled);
    ASSERT_EQ(reconciled.value().assets.size(), 1u);
    EXPECT_EQ(reconciled.value().assets.front().status,
        SctIdentityMatchStatus::Exact);
    ASSERT_EQ(reconciled.value().scripts.size(), 1u);
    ASSERT_TRUE(reconciled.value().scripts.front().candidate.has_value());
    const auto difference = SalsaScriptPatchService::diff(state(baseline),
        *reconciled.value().scripts.front().candidate, std::nullopt);
    ASSERT_TRUE(difference);
    EXPECT_TRUE(difference.value().empty());
}

TEST(SctReconciliationTest, StrongInstructionMatchAnnotatesItsChangeUnit) {
    const auto baseline = document();
    auto incoming = document(4);
    auto& incomingInstructions = std::get<SctScriptSectionContent>(
        incoming.sections.front().content).instructions;
    incomingInstructions[1].skipRefresh = true;
    const auto reconciled = SctDocumentReconciler::reconcile(
        request(baseline, incoming));
    ASSERT_TRUE(reconciled);
    const auto baselineId = std::get<SctScriptSectionContent>(
        baseline.sections.front().content).instructions[1].id;
    const auto* unit = instructionUnit(reconciled.value(), baselineId.value());
    ASSERT_NE(unit, nullptr);
    ASSERT_TRUE(unit->identity.has_value());
    EXPECT_EQ(unit->identity->status, SctIdentityMatchStatus::Strong);
    EXPECT_TRUE(unit->selectable);
}

TEST(SctReconciliationTest, AmbiguousIncomingInstructionsStayProvisionalUntilConfirmed) {
    const auto baseline = document(0, {9u});
    const auto incoming = document(3, {9u, 9u});
    auto initialRequest = request(baseline, incoming);
    const auto initial = SctDocumentReconciler::reconcile(initialRequest);
    ASSERT_TRUE(initial);
    const auto provisionalCount = std::ranges::count_if(
        initial.value().scripts.front().entities,
        [](const auto& entity) { return entity.provisional; });
    EXPECT_EQ(provisionalCount, 2u);
    ASSERT_TRUE(initial.value().changePlan.has_value());
    EXPECT_EQ(std::ranges::count_if(initial.value().changePlan->scripts.front().units,
        [](const auto& unit) {
            return unit.entityKind == SctChangeEntityKind::Instruction
                && !unit.selectable
                && unit.disposition == SctChangeDisposition::Conflict;
        }), 2u);

    const auto incomingId = std::get<SctScriptSectionContent>(
        incoming.sections.front().content).instructions.front().id;
    initialRequest.decisions.push_back({"accept-new",
        SctReconciliationDecisionKind::IncomingEntityAddition,
        initialRequest.baselineAssets.front().locator,
        initialRequest.incomingAssets.front().key, std::nullopt, incomingId});
    const auto confirmed = SctDocumentReconciler::reconcile(initialRequest);
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(std::ranges::count_if(confirmed.value().scripts.front().entities,
        [](const auto& entity) {
            return entity.incoming && std::holds_alternative<SctInstructionId>(
                *entity.incoming) && entity.resolved && !entity.provisional;
        }), 1u);
}

TEST(SctReconciliationTest, CrossSectionInstructionMoveRemainsAddAndRemove) {
    auto baseline = document(0, {9u});
    baseline.sections.push_back(
        {baseline.allocateSectionId(), "SECOND", SctScriptSectionContent{}});
    auto incoming = baseline;
    auto moved = std::get<SctScriptSectionContent>(
        incoming.sections.front().content).instructions.front();
    std::get<SctScriptSectionContent>(incoming.sections.front().content)
        .instructions.clear();
    std::get<SctScriptSectionContent>(incoming.sections.back().content)
        .instructions.push_back(moved);
    auto movedRequest = request(baseline, incoming);
    movedRequest.decisions.push_back({"try-cross-section-pair",
        SctReconciliationDecisionKind::PairEntity,
        movedRequest.baselineAssets.front().locator,
        movedRequest.incomingAssets.front().key, moved.id, moved.id});
    const auto reconciled = SctDocumentReconciler::reconcile(movedRequest);
    ASSERT_TRUE(reconciled);
    const auto& entities = reconciled.value().scripts.front().entities;
    EXPECT_TRUE(std::ranges::any_of(entities, [&](const auto& entity) {
        return entity.baseline == SctReconciliationEntityId{moved.id}
            && !entity.incoming;
    }));
    EXPECT_TRUE(std::ranges::any_of(entities, [&](const auto& entity) {
        return !entity.baseline
            && entity.incoming == SctReconciliationEntityId{moved.id}
            && entity.provisional;
    }));
}

TEST(SctReconciliationTest, ReconcilesAuthoringMetadataAfterPhysicalIdentity) {
    const auto baseline = document();
    const auto incoming = document(5);
    const auto& baselineScript = std::get<SctScriptSectionContent>(
        baseline.sections.front().content);
    const auto& incomingScript = std::get<SctScriptSectionContent>(
        incoming.sections.front().content);
    const SctAuthoredArm baselineArm{SctAuthoredArmId{1},
        {baseline.sections.front().id, baselineScript.instructions.front().id},
        SctStructuredArmKind::Then, std::nullopt, std::nullopt,
        {baselineScript.instructions[1].id}, {}};
    const SctAuthoredArm incomingArm{SctAuthoredArmId{31},
        {incoming.sections.front().id, incomingScript.instructions.front().id},
        SctStructuredArmKind::Then, std::nullopt, std::nullopt,
        {incomingScript.instructions[1].id}, {}};
    auto authoredRequest = request(baseline, incoming);
    authoredRequest.baselineAssets.front().state = state(baseline, {baselineArm});
    authoredRequest.incomingAssets.front().state = state(incoming, {incomingArm});
    const auto reconciled = SctDocumentReconciler::reconcile(authoredRequest);
    ASSERT_TRUE(reconciled);
    ASSERT_TRUE(reconciled.value().scripts.front().candidate.has_value());
    EXPECT_EQ(reconciled.value().scripts.front().candidate->authoredArms,
        (std::vector<SctAuthoredArm>{baselineArm}));
    EXPECT_TRUE(std::ranges::any_of(reconciled.value().scripts.front().entities,
        [&](const auto& entity) {
            return entity.baseline == SctReconciliationEntityId{baselineArm.id}
                && entity.incoming == SctReconciliationEntityId{incomingArm.id};
        }));
}

TEST(SctReconciliationTest, MatchesAReorderedBatchByUniqueSemanticEvidence) {
    const auto firstBaseline = document(0, {9u});
    const auto secondBaseline = document(0, {12u});
    const auto firstIncoming = document(5, {9u});
    const auto secondIncoming = document(7, {12u});
    SctReconciliationRequest batch;
    batch.decisionScopeId = "legacy-project";
    batch.targetScopeKey = "workspace";
    batch.baselineAssets = {
        {locator("scripts/a001a.sct"), SourceRevision{digest("a")},
            state(firstBaseline)},
        {locator("scripts/a002a.sct"), SourceRevision{digest("b")},
            state(secondBaseline)}};
    batch.incomingAssets = {
        {"legacy:second", locator("legacy/renamed-two.sct"),
            state(secondIncoming)},
        {"legacy:first", locator("legacy/renamed-one.sct"),
            state(firstIncoming)}};
    const auto reconciled = SctDocumentReconciler::reconcile(batch);
    ASSERT_TRUE(reconciled);
    ASSERT_EQ(reconciled.value().assets.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(reconciled.value().assets,
        [](const auto& asset) {
            return asset.status == SctIdentityMatchStatus::Exact
                && asset.resolved;
        }));
    const auto firstLocator = locator("scripts/a001a.sct");
    const auto first = std::ranges::find_if(reconciled.value().assets,
        [&](const auto& asset) {
            return asset.baseline && *asset.baseline == firstLocator;
        });
    ASSERT_NE(first, reconciled.value().assets.end());
    EXPECT_EQ(first->incomingKey, "legacy:first");
}

TEST(SctReconciliationTest, ReferenceGraphDisambiguatesRepeatedInstructionShapes) {
    const auto makeGraph = [](const std::size_t skippedIds, const bool reverse) {
        auto value = document(skippedIds, {9u, 12u});
        auto& script = std::get<SctScriptSectionContent>(
            value.sections.front().content);
        SctDocumentInstruction first{value.allocateInstructionId(), 10u};
        first.fixedParameters.push_back(
            {0u, SctInstructionReference{script.instructions[0].id}});
        SctDocumentInstruction second{value.allocateInstructionId(), 10u};
        second.fixedParameters.push_back(
            {0u, SctInstructionReference{script.instructions[1].id}});
        if (reverse) std::swap(first, second);
        script.instructions.push_back(first);
        script.instructions.push_back(second);
        return value;
    };
    const auto baseline = makeGraph(0, false);
    const auto incoming = makeGraph(5, true);
    const auto reconciled = SctDocumentReconciler::reconcile(
        request(baseline, incoming));
    ASSERT_TRUE(reconciled);
    const auto& baselineInstructions = std::get<SctScriptSectionContent>(
        baseline.sections.front().content).instructions;
    const auto& incomingInstructions = std::get<SctScriptSectionContent>(
        incoming.sections.front().content).instructions;
    const auto expectedBaseline = baselineInstructions[2].id;
    const auto expectedIncoming = incomingInstructions[3].id;
    EXPECT_TRUE(std::ranges::any_of(reconciled.value().scripts.front().entities,
        [&](const auto& entity) {
            return entity.baseline == SctReconciliationEntityId{expectedBaseline}
                && entity.incoming == SctReconciliationEntityId{expectedIncoming}
                && entity.status == SctIdentityMatchStatus::Exact
                && std::ranges::any_of(entity.evidence, [](const auto& evidence) {
                    return evidence.code == "reference-graph";
                });
        }));
}

TEST(SctReconciliationTest, ReportsResolvedWholeAssetDecisionsWithoutDocuments) {
    const auto baseline = document();
    const auto incoming = document(2);
    SctReconciliationRequest wholeAssets;
    wholeAssets.decisionScopeId = "legacy-project";
    wholeAssets.targetScopeKey = "workspace";
    wholeAssets.baselineAssets.push_back({locator("scripts/old.sct"),
        SourceRevision{digest("old")}, state(baseline)});
    wholeAssets.incomingAssets.push_back(
        {"legacy:new", locator("scripts/new.sct"), state(incoming)});
    wholeAssets.decisions.push_back({"remove-old",
        SctReconciliationDecisionKind::BaselineAssetRemoval,
        wholeAssets.baselineAssets.front().locator});
    wholeAssets.decisions.push_back({"add-new",
        SctReconciliationDecisionKind::IncomingAssetAddition, std::nullopt,
        wholeAssets.incomingAssets.front().key});
    const auto reconciled = SctDocumentReconciler::reconcile(wholeAssets);
    ASSERT_TRUE(reconciled);
    ASSERT_EQ(reconciled.value().assets.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(reconciled.value().assets,
        [](const auto& asset) { return asset.resolved; }));
    EXPECT_TRUE(reconciled.value().scripts.empty());
    EXPECT_FALSE(reconciled.value().changePlan.has_value());
}

TEST(SctReconciliationTest, NonidenticalOpaqueContentIsPreservedAndReported) {
    auto baseline = document();
    auto incoming = baseline;
    const auto target = std::get<SctScriptSectionContent>(
        baseline.sections.front().content).instructions.front().id;
    baseline.opaqueAttachments.push_back({baseline.allocateOpaqueAttachmentId(),
        {0xaa}, target, SctOpaquePlacement::After, std::nullopt, 1,
        SctOpaqueRelocationSupport::Relocatable, SctOpaqueReason::Gap});
    incoming.opaqueAttachments.push_back({incoming.allocateOpaqueAttachmentId(),
        {0xbb}, target, SctOpaquePlacement::After, std::nullopt, 1,
        SctOpaqueRelocationSupport::Relocatable, SctOpaqueReason::Gap});
    const auto reconciled = SctDocumentReconciler::reconcile(
        request(baseline, incoming));
    ASSERT_TRUE(reconciled);
    ASSERT_TRUE(reconciled.value().scripts.front().candidate.has_value());
    EXPECT_EQ(reconciled.value().scripts.front().candidate->document
        ->opaqueAttachments.front().bytes, (std::vector<std::uint8_t>{0xaa}));
    EXPECT_TRUE(std::ranges::any_of(
        reconciled.value().scripts.front().diagnostics,
        [](const auto& diagnostic) {
            return diagnostic.code == "OpaqueEquivalenceUnavailable"
                && diagnostic.disposition == SctChangeDisposition::Conflict;
        }));
}

TEST(SctReconciliationTest, PersistedDecisionsAreBoundAndInvalidatedPerAsset) {
    const auto baseline = document(0, {9u});
    const auto incoming = document(3, {9u, 9u});
    auto firstRequest = request(baseline, incoming);
    const auto incomingId = std::get<SctScriptSectionContent>(
        incoming.sections.front().content).instructions.front().id;
    firstRequest.decisions.push_back({"accept-new",
        SctReconciliationDecisionKind::IncomingEntityAddition,
        firstRequest.baselineAssets.front().locator,
        firstRequest.incomingAssets.front().key, std::nullopt, incomingId});
    const auto first = SctDocumentReconciler::reconcile(firstRequest);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first.value().boundDecisions.has_value());

    auto persistedRequest = request(baseline, incoming);
    persistedRequest.persistedDecisions = std::cref(*first.value().boundDecisions);
    const auto reused = SctDocumentReconciler::reconcile(persistedRequest);
    ASSERT_TRUE(reused);
    EXPECT_TRUE(std::ranges::any_of(reused.value().scripts.front().entities,
        [](const auto& entity) {
            return entity.resolved && entity.provenance
                == SctIdentityMatchProvenance::PersistedUserDecision;
        }));

    auto changedIncoming = incoming;
    std::get<SctScriptSectionContent>(changedIncoming.sections.front().content)
        .instructions.back().skipRefresh = true;
    auto invalidatedRequest = request(baseline, changedIncoming);
    invalidatedRequest.persistedDecisions =
        std::cref(*first.value().boundDecisions);
    const auto invalidated = SctDocumentReconciler::reconcile(invalidatedRequest);
    ASSERT_TRUE(invalidated);
    EXPECT_TRUE(std::ranges::any_of(invalidated.value().diagnostics,
        [](const auto& diagnostic) {
            return diagnostic.code == "PersistedDecisionInvalidated";
        }));
}

TEST(SctReconciliationTest, CancellationReturnsNoPartialCandidateOrPlan) {
    const auto baseline = document();
    const auto incoming = document(2);
    auto cancelledRequest = request(baseline, incoming);
    std::stop_source stop;
    stop.request_stop();
    cancelledRequest.stopToken = stop.get_token();
    const auto reconciled = SctDocumentReconciler::reconcile(cancelledRequest);
    ASSERT_TRUE(reconciled);
    EXPECT_TRUE(reconciled.value().cancelled);
    EXPECT_TRUE(reconciled.value().scripts.empty());
    EXPECT_FALSE(reconciled.value().changePlan.has_value());
}

TEST(SctReconciliationTest, ProgressIsDeterministicAndCanCancelBetweenPhases) {
    const auto baseline = document();
    const auto incoming = document(2);
    using ProgressKey = std::tuple<SctReconciliationProgressPhase,
        std::size_t, std::size_t>;
    const auto run = [&](std::vector<ProgressKey>& observed) {
        auto observedRequest = request(baseline, incoming);
        observedRequest.progress = [&](const auto& progress) {
            observed.emplace_back(progress.phase, progress.completedAssets,
                progress.totalAssets);
        };
        return SctDocumentReconciler::reconcile(observedRequest);
    };
    std::vector<ProgressKey> firstProgress, secondProgress;
    ASSERT_TRUE(run(firstProgress));
    ASSERT_TRUE(run(secondProgress));
    EXPECT_EQ(firstProgress, secondProgress);
    EXPECT_TRUE(std::ranges::any_of(firstProgress, [](const auto& progress) {
        return std::get<0>(progress)
            == SctReconciliationProgressPhase::GraphRefinement;
    }));

    std::stop_source stop;
    auto cancellingRequest = request(baseline, incoming);
    cancellingRequest.stopToken = stop.get_token();
    cancellingRequest.progress = [&](const auto& progress) {
        if (progress.phase == SctReconciliationProgressPhase::GraphRefinement)
            stop.request_stop();
    };
    const auto cancelled = SctDocumentReconciler::reconcile(cancellingRequest);
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled.value().cancelled);
    EXPECT_TRUE(cancelled.value().scripts.empty());
    EXPECT_FALSE(cancelled.value().changePlan.has_value());
}

TEST(SctReconciliationDecisionCodecTest, IsStrictCanonicalAndRoundTrips) {
    const auto baseline = document();
    const auto incoming = document(1);
    auto sourceRequest = request(baseline, incoming);
    sourceRequest.decisions.push_back({"pair",
        SctReconciliationDecisionKind::PairAsset,
        sourceRequest.baselineAssets.front().locator,
        sourceRequest.incomingAssets.front().key});
    const auto reconciled = SctDocumentReconciler::reconcile(sourceRequest);
    ASSERT_TRUE(reconciled);
    ASSERT_TRUE(reconciled.value().boundDecisions.has_value());
    const auto encoded = SctReconciliationDecisionCodec::serialize(
        *reconciled.value().boundDecisions);
    ASSERT_TRUE(encoded);
    const auto decoded = SctReconciliationDecisionCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    const auto encodedAgain = SctReconciliationDecisionCodec::serialize(decoded.value());
    ASSERT_TRUE(encodedAgain);
    EXPECT_EQ(encoded.value(), encodedAgain.value());

    auto mismatchedBinding = decoded.value();
    mismatchedBinding.assets.front().decisions.front().incomingAssetKey =
        "different-asset";
    EXPECT_FALSE(SctReconciliationDecisionCodec::serialize(mismatchedBinding));
    auto missingEvidence = decoded.value();
    missingEvidence.assets.front().incomingFingerprint.reset();
    EXPECT_FALSE(SctReconciliationDecisionCodec::serialize(missingEvidence));

    auto unknown = encoded.value();
    const auto location = unknown.find("\"schemaVersion\": 1");
    ASSERT_NE(location, std::string::npos);
    unknown.replace(location, std::string("\"schemaVersion\": 1").size(),
        "\"schemaVersion\": 2");
    EXPECT_FALSE(SctReconciliationDecisionCodec::deserialize(unknown));
}

TEST(SctReconciliationDecisionStoreTest, CheckpointsAndLoadsByScope) {
    const auto baseline = document();
    const auto incoming = document(1);
    auto sourceRequest = request(baseline, incoming);
    sourceRequest.decisions.push_back({"pair",
        SctReconciliationDecisionKind::PairAsset,
        sourceRequest.baselineAssets.front().locator,
        sourceRequest.incomingAssets.front().key});
    const auto reconciled = SctDocumentReconciler::reconcile(sourceRequest);
    ASSERT_TRUE(reconciled);
    ASSERT_TRUE(reconciled.value().boundDecisions.has_value());

    TemporaryDirectory directory;
    DirectorySctReconciliationDecisionStore store(directory.path() / L"nested");
    ASSERT_TRUE(store.checkpointReconciliationDecisions(
        *reconciled.value().boundDecisions));
    const auto loaded = store.loadReconciliationDecisions(
        sourceRequest.decisionScopeId, sourceRequest.targetScopeKey);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(loaded.value()->decisionScopeId, sourceRequest.decisionScopeId);
    EXPECT_EQ(loaded.value()->assets.front().decisions.front().id, "pair");
    const auto absent = store.loadReconciliationDecisions(
        sourceRequest.decisionScopeId, "other-workspace");
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent.value().has_value());
}

}  // namespace
}  // namespace salsa::core
