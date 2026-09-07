#include "SalsaCore/Sct/SctEditSession.h"
#include "SpiceSCT/SctDocumentAnalysis.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctDocumentValidator.h"

#include <gtest/gtest.h>

#include <array>
#include <functional>
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

const SctSemanticProjectionNode* findSemanticNode(
    const std::span<const SctSemanticProjectionNode> nodes,
    const std::function<bool(const SctSemanticProjectionNode&)>& predicate) {
    for (const auto& node : nodes) {
        if (predicate(node)) return &node;
        if (const auto* child = findSemanticNode(node.children, predicate)) return child;
    }
    return nullptr;
}

const SctSemanticProjectionNode* findAuthoredArmNode(
    const SctSemanticEditorProjection& projection, const SctAuthoredArmId arm) {
    return findSemanticNode(projection.roots(), [&](const auto& node) {
        return node.authoredArm == arm;
    });
}

SctSemanticFragment twoInstructionSemanticFragment() {
    SctSemanticFragment fragment;
    fragment.kind = SctFragmentKind::SemanticUnits;
    fragment.sourceAssetIdentity = "scripts/semantic_test.sct";
    SctDocumentInstruction first{SctInstructionId(100u), 10u};
    first.fixedParameters = {{0u, SctInstructionReference{SctInstructionId(101u)}}};
    SctDocumentInstruction second{SctInstructionId(101u), 1u};
    fragment.instructions = {first, second};
    for (const auto& value : fragment.instructions) {
        SctSemanticFragmentUnit unit;
        unit.kind = SctSemanticNodeKind::Instruction;
        unit.instructions.push_back(value.id);
        fragment.semanticUnits.push_back(std::move(unit));
    }
    return fragment;
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

TEST(SctStructuredAuthoring, ProjectionNormalizesSiblingsAndPlansSafeDestinations) {
    const auto fixture = ifWithoutElse();
    SctEditSession session(fixture.snapshot);
    const auto projection = session.semanticProjection();
    ASSERT_NE(projection, nullptr);
    ASSERT_TRUE(projection->current());
    const auto* region = findSemanticNode(projection->roots(), [](const auto& node) {
        return node.kind == SctSemanticNodeKind::Region
            && node.regionKind == SctStructuredRegionKind::If;
    });
    ASSERT_NE(region, nullptr);
    const auto arm = std::ranges::find_if(region->children, [](const auto& node) {
        return node.kind == SctSemanticNodeKind::Arm
            && node.armKind == SctStructuredArmKind::Then;
    });
    ASSERT_NE(arm, region->children.end());
    ASSERT_GE(arm->children.size(), 2u);
    const std::array reversed{arm->children[1].key, arm->children[0].key,
        arm->children[1].key};
    const auto normalized = SctSemanticCommandPlanner::normalizeSelection(
        *projection, reversed);
    ASSERT_TRUE(normalized);
    ASSERT_EQ(normalized.value().units.size(), 2u);
    EXPECT_EQ(normalized.value().units[0], arm->children[0].key);
    EXPECT_EQ(normalized.value().units[1], arm->children[1].key);

    const std::array firstOnly{arm->children[0].key};
    const auto selection = SctSemanticCommandPlanner::normalizeSelection(
        *projection, firstOnly);
    ASSERT_TRUE(selection);
    const auto moved = SctSemanticCommandPlanner::planMove(*projection,
        session.workingState(), selection.value(), SctSemanticMoveDirection::Down);
    ASSERT_TRUE(moved);
    ASSERT_TRUE(moved.value().anchorAfter);
    EXPECT_EQ(*moved.value().anchorAfter, arm->children[1].physicalInstructions.back());
    const auto destination = SctSemanticDestination{projection->workingRevision(),
        arm->key, SctSemanticDestinationPlacement::IntoEnd};
    const auto placed = SctSemanticCommandPlanner::planMove(*projection,
        session.workingState(), selection.value(), destination);
    ASSERT_TRUE(placed);
    EXPECT_EQ(placed.value().destinationController, arm->controller);

    const std::array wholeArm{arm->key};
    const auto armSelection = SctSemanticCommandPlanner::normalizeSelection(
        *projection, wholeArm);
    ASSERT_TRUE(armSelection);
    EXPECT_FALSE(SctSemanticCommandPlanner::planMove(*projection,
        session.workingState(), armSelection.value(), destination));
}

TEST(SctStructuredAuthoring, MultiInstructionPasteLowersEmptyElseAtomically) {
    const auto fixture = ifWithoutElse();
    SctEditSession session(fixture.snapshot);
    ASSERT_TRUE(session.addVirtualElse(fixture.controller).committed);
    const auto arm = session.structuredAuthoring().arms().front().id;
    const auto beforePaste = session.workingRevision();
    const auto* armNode = findAuthoredArmNode(*session.semanticProjection(), arm);
    ASSERT_NE(armNode, nullptr);
    const auto pasted = session.pasteFragment(twoInstructionSemanticFragment(),
        {session.semanticProjection()->workingRevision(), armNode->key,
            SctSemanticDestinationPlacement::IntoEnd});
    ASSERT_TRUE(pasted.committed) << (pasted.diagnostics.empty()
        ? "" : pasted.diagnostics.front().message);
    EXPECT_NE(session.workingRevision(), beforePaste);
    const auto* authored = session.structuredAuthoring().find(arm);
    ASSERT_NE(authored, nullptr);
    ASSERT_EQ(authored->members.size(), 2u);
    EXPECT_EQ(authored->realization, SctAuthoredArmRealization::Physical);
    const auto materialized = session.materializeRevision(session.workingRevision());
    ASSERT_TRUE(materialized);
    const auto index = SctDocumentIndex::build(**materialized);
    const auto* controller = index.find(**materialized, fixture.controller);
    ASSERT_NE(controller, nullptr);
    const auto falseTarget = std::ranges::find(controller->fixedParameters, 1u,
        &SctDocumentParameter::schemaIndex);
    ASSERT_NE(falseTarget, controller->fixedParameters.end());
    EXPECT_EQ(std::get<SctInstructionReference>(falseTarget->value).target,
        authored->members.front());
    const auto* first = index.find(**materialized, authored->members.front());
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(std::get<SctInstructionReference>(first->fixedParameters.front().value).target,
        authored->members.back());
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingRevision(), beforePaste);
    EXPECT_EQ(session.structuredAuthoring().find(arm)->realization,
        SctAuthoredArmRealization::Virtual);
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_EQ(session.structuredAuthoring().find(arm)->members.size(), 2u);
}

TEST(SctStructuredAuthoring, EmptyArmMoveRejectsSourceControllerDismantling) {
    const auto fixture = ifWithoutElse();
    SctEditSession session(fixture.snapshot);
    ASSERT_TRUE(session.addVirtualElse(fixture.controller).committed);
    const auto projection = session.semanticProjection();
    ASSERT_TRUE(projection);
    const auto arm = session.structuredAuthoring().arms().front().id;
    const auto* destination = findAuthoredArmNode(*projection, arm);
    ASSERT_NE(destination, nullptr);
    const auto* source = findSemanticNode(projection->roots(), [](const auto& node) {
        return node.kind == SctSemanticNodeKind::Instruction
            && !node.hiddenByDefault;
    });
    ASSERT_NE(source, nullptr);
    const std::array key{source->key};
    const auto selection = SctSemanticCommandPlanner::normalizeSelection(
        *projection, key);
    ASSERT_TRUE(selection);
    const auto before = session.workingRevision();
    const auto moved = session.moveSemanticUnits(selection.value(),
        {projection->workingRevision(), destination->key,
            SctSemanticDestinationPlacement::IntoEnd});
    EXPECT_FALSE(moved.committed);
    EXPECT_EQ(session.workingRevision(), before);
}

TEST(SctStructuredAuthoring, MultiInstructionCaseRequiresValueAndLowersInOneRevision) {
    const auto fixture = switchWithCases();
    SctEditSession session(fixture.snapshot);
    ASSERT_TRUE(session.addVirtualCase(fixture.controller).committed);
    const auto arm = session.structuredAuthoring().arms().front().id;
    const auto* emptyNode = findAuthoredArmNode(*session.semanticProjection(), arm);
    ASSERT_NE(emptyNode, nullptr);
    const auto emptyKey = emptyNode->key;
    const auto rejectedAt = session.workingRevision();
    const auto rejected = session.pasteFragment(twoInstructionSemanticFragment(),
        {session.semanticProjection()->workingRevision(), emptyKey,
            SctSemanticDestinationPlacement::IntoEnd});
    EXPECT_FALSE(rejected.committed);
    EXPECT_EQ(session.workingRevision(), rejectedAt);
    ASSERT_TRUE(session.setVirtualCaseValue(arm, -7).committed);
    const auto beforePaste = session.workingRevision();
    const auto stale = session.pasteFragment(twoInstructionSemanticFragment(),
        {rejectedAt, emptyKey, SctSemanticDestinationPlacement::IntoEnd});
    EXPECT_FALSE(stale.committed);
    EXPECT_EQ(session.workingRevision(), beforePaste);
    const auto* valuedNode = findAuthoredArmNode(*session.semanticProjection(), arm);
    ASSERT_NE(valuedNode, nullptr);
    const auto pasted = session.pasteFragment(twoInstructionSemanticFragment(),
        {session.semanticProjection()->workingRevision(), valuedNode->key,
            SctSemanticDestinationPlacement::IntoEnd});
    ASSERT_TRUE(pasted.committed) << (pasted.diagnostics.empty()
        ? "" : pasted.diagnostics.front().message);
    const auto* authored = session.structuredAuthoring().find(arm);
    ASSERT_NE(authored, nullptr);
    ASSERT_EQ(authored->members.size(), 2u);
    ASSERT_EQ(authored->managedScaffolding.size(), 1u);
    const auto materialized = session.materializeRevision(session.workingRevision());
    ASSERT_TRUE(materialized);
    const auto index = SctDocumentIndex::build(**materialized);
    const auto* controller = index.find(**materialized, fixture.controller);
    ASSERT_NE(controller, nullptr);
    ASSERT_EQ(controller->repeatedParameterGroups.size(), 3u);
    EXPECT_EQ(std::get<SctInstructionReference>(
        controller->repeatedParameterGroups.back().parameters.back().value).target,
        authored->members.front());
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_EQ(session.workingRevision(), beforePaste);
    EXPECT_EQ(session.structuredAuthoring().find(arm)->realization,
        SctAuthoredArmRealization::Virtual);
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

TEST(SctStructuredAuthoring, MetadataEditsAreTypedUndoableAndValidated) {
    const auto fixture = ifWithoutElse();
    SctEditSession session(fixture.snapshot);
    ASSERT_TRUE(session.setVariableAlias(
        {salsa::core::SctVariableKind::Byte, 4}, "DoorState").committed);
    EXPECT_FALSE(session.setVariableAlias(
        {salsa::core::SctVariableKind::Bit, 8}, "doorstate").committed);

    const SctAuthoringTarget target{SctAuthoringTargetKind::Instruction,
        fixture.controller.value()};
    ASSERT_TRUE(session.setAnnotation({target, "Triggers the scene.",
        std::string{}, 0x224466u}).committed);
    ASSERT_EQ(session.annotations().size(), 1u);
    EXPECT_EQ(session.annotations().front().note, "Triggers the scene.");
    EXPECT_TRUE(session.annotations().front().bookmarkLabel.has_value());
    EXPECT_FALSE(session.setAnnotation({
        {SctAuthoringTargetKind::Variable, 0u,
            static_cast<salsa::core::SctVariableKind>(0xffu)},
        "Invalid variable kind"}).committed);
    ASSERT_TRUE(session.undo().has_value());
    EXPECT_TRUE(session.annotations().empty());
    ASSERT_TRUE(session.redo().has_value());
    EXPECT_EQ(session.annotations().front().colorRgb, 0x224466u);
}

TEST(SctStructuredAuthoring, SectionFoldersRequireContiguousPhysicalRanges) {
    SctDocument document;
    const auto first = document.allocateSectionId();
    const auto second = document.allocateSectionId();
    const auto third = document.allocateSectionId();
    document.sections.push_back({first, "A", SctScriptSectionContent{}});
    document.sections.push_back({second, "B", SctScriptSectionContent{}});
    document.sections.push_back({third, "C", SctScriptSectionContent{}});
    SctEditSession session(snapshotFor(std::move(document)));

    const std::array split{first, third};
    const std::array all{first, second, third};
    const std::array opening{first, second};
    const std::array ending{third};
    EXPECT_FALSE(session.createSectionFolder("Split", split).committed);
    ASSERT_TRUE(session.createSectionFolder("All", all).committed);
    ASSERT_TRUE(session.createSectionFolder("Opening", opening,
        SctSectionFolderId{1}).committed);
    ASSERT_TRUE(session.createSectionFolder("Ending", ending,
        SctSectionFolderId{1}).committed);
    ASSERT_EQ(session.folders().size(), 3u);
    EXPECT_EQ(session.folders()[1].parent, SctSectionFolderId{1});
    auto cycle = session.folders().front();
    cycle.parent = SctSectionFolderId{2};
    EXPECT_FALSE(session.updateSectionFolder(std::move(cycle)).committed);
    EXPECT_FALSE(session.moveSection(second, SctSectionMoveDirection::Down).committed);
}

}  // namespace
