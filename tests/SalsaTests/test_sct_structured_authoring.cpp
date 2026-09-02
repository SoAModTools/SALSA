#include "SalsaCore/Sct/SctEditSession.h"
#include "SpiceSCT/SctDocumentAnalysis.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctDocumentValidator.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <ranges>

namespace {
using namespace salsa::core;
using namespace spice::sct;

Sha256Digest zeroDigest() {
    return Sha256Digest(std::array<std::byte, Sha256Digest::Size>{});
}

std::shared_ptr<const SctDocumentProvenance> provenance() {
    auto locator = AssetLocator::fromRelativePath("scripts/semantic_test.sct");
    EXPECT_TRUE(locator);
    auto inspection = std::make_shared<SctSourceInspection>(SctSourceInspection{
        SourceAssetSnapshot{{locator.value(), 0u, SourceRevision{zeroDigest()}}, {}},
        DatasetFingerprint{zeroDigest()},
        std::make_shared<SctParseResult>(), {}, {}});
    auto result = std::make_shared<SctDocumentProvenance>();
    result->inspection = std::move(inspection);
    return result;
}

SctDocumentInstruction instruction(SctDocument& document, const std::uint16_t opcode) {
    return {document.allocateInstructionId(), opcode};
}

SctDocumentInstruction jump(SctDocument& document, const SctInstructionId target) {
    auto result = instruction(document, 10u);
    result.fixedParameters = {{0u, SctInstructionReference{target}}};
    return result;
}

std::shared_ptr<const SctDocumentSnapshot> snapshotFor(SctDocument document) {
    auto result = std::make_shared<SctDocumentSnapshot>();
    result->provenance = provenance();
    result->document = std::make_shared<const SctDocument>(std::move(document));
    result->analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*result->document));
    // These focused fixtures model already-admitted working documents. Their
    // deliberately small placeholder opcodes are not a parser-valid corpus.
    result->readiness = SctDocumentReadiness::StructurallyValid;
    return result;
}

struct IfFixture final {
    std::shared_ptr<const SctDocumentSnapshot> snapshot;
    SctInstructionId controller;
};

IfFixture ifWithoutElse() {
    SctDocument document;
    auto branch = instruction(document, 0u);
    auto body = instruction(document, 1u);
    const auto joinId = document.allocateInstructionId();
    auto exit = jump(document, joinId);
    SctDocumentInstruction join{joinId, 12u};
    branch.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{joinId}}};
    document.sections.push_back({document.allocateSectionId(), "TEST",
        SctScriptSectionContent{{branch, body, exit, join}}});
    return {snapshotFor(std::move(document)), branch.id};
}

struct SwitchFixture final {
    std::shared_ptr<const SctDocumentSnapshot> snapshot;
    SctInstructionId controller;
};

SwitchFixture switchWithCases() {
    SctDocument document;
    auto selector = instruction(document, 3u);
    auto first = instruction(document, 1u);
    auto second = instruction(document, 1u);
    const auto joinId = document.allocateInstructionId();
    auto firstExit = jump(document, joinId);
    auto secondExit = jump(document, joinId);
    SctDocumentInstruction join{joinId, 12u};
    selector.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctEncodedWordValue{2u}}};
    selector.repeatedParameterGroups = {
        {{{2u, SctEncodedWordValue{1u}}, {3u, SctInstructionReference{first.id}}}},
        {{{2u, SctEncodedWordValue{2u}}, {3u, SctInstructionReference{second.id}}}},
    };
    document.sections.push_back({document.allocateSectionId(), "TEST",
        SctScriptSectionContent{{selector, first, firstExit,
            second, secondExit, join}}});
    return {snapshotFor(std::move(document)), selector.id};
}

std::uint16_t simpleOpcode() {
    const auto found = std::ranges::find_if(SctEditSession::insertableOpcodes(),
        [](const auto& item) { return item.opcode != 9u && item.opcode != 12u; });
    EXPECT_NE(found, SctEditSession::insertableOpcodes().end());
    return found == SctEditSession::insertableOpcodes().end() ? 1u : found->opcode;
}

TEST(SctStructuredAuthoring, StateAppliesAtomicallyAndNeverReusesIds) {
    SctStructuredAuthoringState state;
    const SctAuthoredArm arm{{1u}, {SctSectionId(1u), SctInstructionId(2u)},
        SctStructuredArmKind::Else};
    const auto created = state.apply({{{arm.id, std::nullopt, arm}}});
    ASSERT_TRUE(created.succeeded());
    EXPECT_EQ(state.nextId().value, 2u);
    auto changed = arm;
    changed.realization = SctAuthoredArmRealization::Physical;
    const auto failed = state.apply({{
        {arm.id, SctAuthoredArm{}, changed},
        {{2u}, std::nullopt, SctAuthoredArm{{2u}}},
    }});
    EXPECT_FALSE(failed.succeeded());
    EXPECT_EQ(*state.find(arm.id), arm);
    ASSERT_TRUE(state.apply(created.inverse).succeeded());
    EXPECT_TRUE(state.arms().empty());
    EXPECT_EQ(state.nextId().value, 2u);
}

TEST(SctStructuredAuthoring, VirtualElseIsDirtyUndoableAndDocumentFree) {
    const auto fixture = ifWithoutElse();
    ASSERT_EQ(fixture.snapshot->readiness, SctDocumentReadiness::StructurallyValid);
    SctEditSession session(fixture.snapshot);
    const auto baseline = session.verifiedSnapshot()->document;
    const auto added = session.addVirtualElse(fixture.controller);
    ASSERT_TRUE(added.committed) << (added.diagnostics.empty()
        ? "" : added.diagnostics.front().message);
    EXPECT_FALSE(added.changes.documentChanged);
    EXPECT_TRUE(session.isDirty());
    ASSERT_EQ(session.structuredAuthoring().arms().size(), 1u);
    EXPECT_EQ(session.verifiedSnapshot()->document, baseline);
    ASSERT_TRUE(session.semanticProjection());
    EXPECT_EQ(session.semanticProjection()->authoredArms().front().status,
        SctSemanticArmStatus::Virtual);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_TRUE(session.structuredAuthoring().arms().empty());
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_EQ(session.structuredAuthoring().arms().size(), 1u);
}

TEST(SctStructuredAuthoring, FirstElseInstructionLowersAsOneCombinedRevision) {
    const auto fixture = ifWithoutElse();
    SctEditSession session(fixture.snapshot);
    ASSERT_TRUE(session.addVirtualElse(fixture.controller).committed);
    const auto arm = session.structuredAuthoring().arms().front().id;
    const auto inserted = session.insertInstructionIntoAuthoredArm(arm, simpleOpcode());
    ASSERT_TRUE(inserted.committed) << (inserted.diagnostics.empty()
        ? "" : inserted.diagnostics.front().message);
    EXPECT_TRUE(inserted.changes.documentChanged);
    const auto* authored = session.structuredAuthoring().find(arm);
    ASSERT_NE(authored, nullptr);
    ASSERT_EQ(authored->members.size(), 1u);
    EXPECT_EQ(authored->realization, SctAuthoredArmRealization::Physical);
    const auto materialized = session.materializeRevision(session.workingRevision());
    ASSERT_TRUE(materialized.has_value());
    const auto index = SctDocumentIndex::build(**materialized);
    const auto* controller = index.find(**materialized, fixture.controller);
    ASSERT_NE(controller, nullptr);
    const auto falseTarget = std::ranges::find(controller->fixedParameters, 1u,
        &SctDocumentParameter::schemaIndex);
    ASSERT_NE(falseTarget, controller->fixedParameters.end());
    EXPECT_EQ(std::get<SctInstructionReference>(falseTarget->value).target,
        authored->members.front());
    const auto emptied = session.deleteOnlyInstructionFromAuthoredArm(
        arm, authored->members.front());
    ASSERT_TRUE(emptied.committed) << (emptied.diagnostics.empty()
        ? "" : emptied.diagnostics.front().message);
    EXPECT_EQ(session.structuredAuthoring().find(arm)->realization,
        SctAuthoredArmRealization::Virtual);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.structuredAuthoring().find(arm)->realization,
        SctAuthoredArmRealization::Physical);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.structuredAuthoring().find(arm)->realization,
        SctAuthoredArmRealization::Virtual);
}

TEST(SctStructuredAuthoring, VirtualCaseRequiresUniqueValueBeforeRealization) {
    const auto fixture = switchWithCases();
    ASSERT_EQ(fixture.snapshot->readiness, SctDocumentReadiness::StructurallyValid);
    SctEditSession session(fixture.snapshot);
    const auto added = session.addVirtualCase(fixture.controller);
    ASSERT_TRUE(added.committed) << (added.diagnostics.empty()
        ? "" : added.diagnostics.front().message);
    const auto arm = session.structuredAuthoring().arms().front().id;
    EXPECT_EQ(session.semanticProjection()->find(arm)->status,
        SctSemanticArmStatus::NeedsValue);
    EXPECT_FALSE(session.insertInstructionIntoAuthoredArm(arm, simpleOpcode()).committed);
    EXPECT_FALSE(session.setVirtualCaseValue(arm, 1).committed);
    ASSERT_TRUE(session.setVirtualCaseValue(arm, -7).committed);
    const auto inserted = session.insertInstructionIntoAuthoredArm(arm, simpleOpcode());
    ASSERT_TRUE(inserted.committed) << (inserted.diagnostics.empty()
        ? "" : inserted.diagnostics.front().message);
    const auto materialized = session.materializeRevision(session.workingRevision());
    ASSERT_TRUE(materialized.has_value());
    const auto index = SctDocumentIndex::build(**materialized);
    const auto* controller = index.find(**materialized, fixture.controller);
    ASSERT_NE(controller, nullptr);
    ASSERT_EQ(controller->repeatedParameterGroups.size(), 3u);
    const auto& value = controller->repeatedParameterGroups.back().parameters.front().value;
    EXPECT_EQ(std::get<SctEncodedWordValue>(value).value,
        static_cast<std::uint32_t>(-7));
}

TEST(SctStructuredAuthoring, InstructionReplacementRequiresSameIdentityAndOpcode) {
    const auto fixture = ifWithoutElse();
    const auto* source = fixture.snapshot->analysis->entities.find(
        *fixture.snapshot->document, fixture.controller);
    ASSERT_NE(source, nullptr);
    auto replacement = *source;
    replacement.fixedParameters.front().value = SctEncodedWordValue{42u};
    const auto applied = SctSemanticOperationService::apply(*fixture.snapshot->document,
        {{{SctReplaceInstructionOperation{fixture.controller, replacement}}}});
    ASSERT_TRUE(applied.succeeded());
    EXPECT_TRUE(applied.forwardChanges.documentChanged);
    replacement.opcode = 1u;
    const auto rejected = SctSemanticOperationService::apply(*fixture.snapshot->document,
        {{{SctReplaceInstructionOperation{fixture.controller, replacement}}}});
    EXPECT_FALSE(rejected.succeeded());
}

TEST(SctStructuredAuthoring, MaterializerRejectsAnUnmetAuthoredArmPostcondition) {
    const auto fixture = ifWithoutElse();
    ASSERT_FALSE(fixture.snapshot->analysis->structuredControlFlow.sections().empty());
    ASSERT_FALSE(fixture.snapshot->analysis->structuredControlFlow.sections().front().regions.empty());
    const auto* region =
        &fixture.snapshot->analysis->structuredControlFlow.sections().front().regions.front();
    ASSERT_TRUE(region->join.has_value());
    SctAuthoredArm expected{{1u},
        {region->id.section, fixture.controller}, SctStructuredArmKind::Else};
    expected.expectedJoin = region->join->entryInstruction;
    expected.members = {SctInstructionId(999u)};
    expected.realization = SctAuthoredArmRealization::Physical;
    SctMaterializationRequest request;
    request.generation = 1u;
    request.baseRevision = RevisionId{1u};
    request.targetRevision = RevisionId{1u};
    request.baseDocument = fixture.snapshot->document;
    request.expectedStructuredArms = {expected};
    const auto result = SctDocumentMaterializer::materialize(request);
    EXPECT_TRUE(std::ranges::any_of(result.operationIssues, [](const auto& issue) {
        return issue.code == "StructuredPostconditionFailed";
    }));
}

}  // namespace
