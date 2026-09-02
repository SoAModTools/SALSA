#include "SpiceSctStructurePrototype/SctStructuredControlFlow.h"

#include "SpiceSCT/SctDocumentAnalysis.h"
#include "SpiceSCT/SctDocumentImporter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <vector>

namespace {

using namespace spice::sct;
using namespace salsa::spice_sct_prototype;

SctDocumentInstruction ordinary(SctDocument& document, const std::uint16_t opcode = 1u) {
    return {document.allocateInstructionId(), opcode};
}

SctDocumentInstruction branch(SctDocument& document, const SctInstructionId falseTarget) {
    auto result = ordinary(document, 0u);
    result.fixedParameters = {
        {0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{falseTarget}},
    };
    return result;
}

SctDocumentInstruction jump(SctDocument& document, const SctInstructionId target) {
    auto result = ordinary(document, 10u);
    result.fixedParameters = {{0u, SctInstructionReference{target}}};
    return result;
}

SctDocumentInstruction returnInstruction(SctDocument& document) {
    return ordinary(document, 12u);
}

SctDocumentInstruction switchInstruction(SctDocument& document,
    const std::vector<std::pair<std::int32_t, SctInstructionId>>& cases) {
    auto result = ordinary(document, 3u);
    result.fixedParameters = {
        {0u, SctEncodedWordValue{0u}},
        {1u, SctEncodedWordValue{static_cast<std::uint32_t>(cases.size())}},
    };
    for (const auto& [value, target] : cases) {
        result.repeatedParameterGroups.push_back({{{2u,
            SctEncodedWordValue{static_cast<std::uint32_t>(value)}},
            {3u, SctInstructionReference{target}}}});
    }
    return result;
}

SctDocument makeDocument(std::vector<SctDocumentInstruction> instructions) {
    SctDocument document;
    const auto section = document.allocateSectionId();
    document.sections.push_back({section, "test", SctScriptSectionContent{std::move(instructions)}});
    return document;
}

SctStructuredControlFlowAnalysis analyze(const SctDocument& document) {
    const auto canonical = SctDocumentAnalysis::build(document);
    return SctStructuredControlFlowAnalysis::build(document, canonical);
}

struct ImportedGapFixture final {
    SctDocument document;
    SctBoundImportEvidence evidence;
    SctInstructionId loopHeader;
    SctInstructionId historicalJump;
};

ImportedGapFixture importedBackwardGapFixture() {
    SctParseResult parsed;
    parsed.parseOk = true;
    parsed.file.detectedEndian = "big";
    parsed.file.originalPayloadBytes.resize(60u, 0u);
    parsed.file.originalPayloadBytes[11] = 1u;
    parsed.file.originalPayloadBytes[16] = 'M';
    parsed.file.originalPayloadBytes[17] = 'A';
    parsed.file.originalPayloadBytes[18] = 'I';
    parsed.file.originalPayloadBytes[19] = 'N';
    std::fill(parsed.file.originalPayloadBytes.begin() + 36,
        parsed.file.originalPayloadBytes.begin() + 52, 0xacu);

    SctInstruction header;
    header.offset = 0u;
    header.payloadOffset = 0u;
    header.opcode = 125u;
    header.rawWords = {125u};
    header.sizeBytes = 4u;
    header.decodeOk = true;

    SctInstruction backward;
    backward.offset = 20u;
    backward.payloadOffset = 20u;
    backward.opcode = 10u;
    backward.rawWords = {10u, 0xffffffe8u};
    backward.parameters = {{0u, "offset", SctParameterValueKind::Link,
        SctSemanticConfidence::Known, {0xffffffe8u}}};
    backward.sizeBytes = 8u;
    backward.decodeOk = true;

    SctSection section;
    section.id = {0u, "MAIN"};
    section.startOffset = 32u;
    section.endOffset = 60u;
    section.kind = SctSectionKind::Script;
    section.instructions = {header, backward};
    section.edges.push_back({SctEdgeType::Jump, SctSemanticConfidence::Known,
        20u, 0u, 20u, 0u, 10u, "jump"});
    parsed.file.sections.push_back(std::move(section));

    auto imported = SctDocumentImporter::import(parsed);
    EXPECT_TRUE(imported.document.has_value());
    auto evidence = imported.context.bind(imported.context.revisionProvenance());
    EXPECT_TRUE(evidence.has_value());
    auto document = std::move(*imported.document);
    const auto& instructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    EXPECT_EQ(instructions.size(), 2u);
    const auto loopHeader = instructions[0].id;
    const auto historicalJump = instructions[1].id;
    return {std::move(document), std::move(*evidence),
        loopHeader, historicalJump};
}

const SctStructuredRegion* regionOf(
    const SctStructuredControlFlowAnalysis& analysis, const SctStructuredRegionKind kind) {
    for (const auto& section : analysis.sections()) {
        const auto found = std::ranges::find(section.regions, kind,
            [](const auto& region) { return region.id.kind; });
        if (found != section.regions.end()) return &*found;
    }
    return nullptr;
}

void collectOutlineInstructions(const std::vector<SctStructuredOutlineItem>& items,
    std::vector<SctInstructionId>& result) {
    for (const auto& item : items) {
        if ((item.kind == SctStructuredOutlineItemKind::Instruction
                || item.kind == SctStructuredOutlineItemKind::Region)
            && item.instruction) {
            result.push_back(*item.instruction);
        }
        collectOutlineInstructions(item.children, result);
    }
}

TEST(SctStructuredControlFlow, EmptyAndNonScriptDocumentsProduceNoSections) {
    SctDocument document;
    document.sections.push_back({document.allocateSectionId(), "label", SctLabelSectionContent{}});
    EXPECT_TRUE(analyze(document).sections().empty());
}

TEST(SctStructuredControlFlow, BuildsStableBasicBlocksAndRetainsUnreachableInstructions) {
    SctDocument document;
    const auto first = document.allocateInstructionId();
    const auto unreachable = document.allocateInstructionId();
    const auto target = document.allocateInstructionId();
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        {first, 10u, false, std::nullopt, {{0u, SctInstructionReference{target}}}},
        {unreachable, 1u},
        {target, 12u},
    }}});
    const auto analysis = analyze(document);
    ASSERT_EQ(analysis.sections().size(), 1u);
    ASSERT_EQ(analysis.sections()[0].blocks.size(), 3u);
    EXPECT_TRUE(analysis.sections()[0].blocks[0].reachable);
    EXPECT_FALSE(analysis.sections()[0].blocks[1].reachable);
    EXPECT_TRUE(analysis.sections()[0].blocks[2].reachable);
    EXPECT_EQ(analysis.blockContaining(unreachable)->id.entryInstruction, unreachable);
}

TEST(SctStructuredControlFlow, CallsEndBlocksButRetainTheirLocalFallthrough) {
    SctDocument document;
    const auto callId = document.allocateInstructionId();
    const auto nextId = document.allocateInstructionId();
    const auto targetId = document.allocateInstructionId();
    auto call = SctDocumentInstruction{callId, 11u};
    call.fixedParameters = {{0u, SctInstructionReference{targetId}}};
    document.sections.push_back({document.allocateSectionId(), "test",
        SctScriptSectionContent{{call, {nextId, 1u}, {targetId, 12u}}}});
    const auto analysis = analyze(document);
    ASSERT_EQ(analysis.sections().size(), 1u);
    ASSERT_EQ(analysis.sections()[0].blocks.size(), 3u);
    const auto& successors = analysis.sections()[0].blocks[0].successors;
    EXPECT_TRUE(std::ranges::any_of(successors, [nextId](const auto& edge) {
        return edge.kind == SctControlFlowKind::Fallthrough && edge.target
            && edge.target->entryInstruction == nextId;
    }));
    EXPECT_TRUE(std::ranges::any_of(successors, [targetId](const auto& edge) {
        return edge.kind == SctControlFlowKind::Call && edge.target
            && edge.target->entryInstruction == targetId;
    }));
}

TEST(SctStructuredControlFlow, RecognizesLegacyIfWithoutElsePattern) {
    SctDocument document;
    const auto ifId = document.allocateInstructionId();
    const auto bodyId = document.allocateInstructionId();
    const auto jumpId = document.allocateInstructionId();
    const auto joinId = document.allocateInstructionId();
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        [&] { auto value = SctDocumentInstruction{ifId, 0u}; value.fixedParameters = {
            {0u, SctEncodedWordValue{0u}}, {1u, SctInstructionReference{joinId}}}; return value; }(),
        {bodyId, 1u},
        [&] { auto value = SctDocumentInstruction{jumpId, 10u}; value.fixedParameters = {
            {0u, SctInstructionReference{joinId}}}; return value; }(),
        {joinId, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* region = regionOf(analysis, SctStructuredRegionKind::If);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->id.headerInstruction, ifId);
    ASSERT_EQ(region->arms.size(), 1u);
    EXPECT_EQ(region->arms[0].kind, SctStructuredArmKind::Then);
    EXPECT_TRUE(std::ranges::any_of(region->evidence, [](const auto& evidence) {
        return evidence.kind == SctStructureEvidenceKind::PreTargetJump;
    }));
}

TEST(SctStructuredControlFlow, RecognizesLegacyIfElseAndCommonForwardExit) {
    SctDocument document;
    const auto ifId = document.allocateInstructionId();
    const auto thenId = document.allocateInstructionId();
    const auto jumpId = document.allocateInstructionId();
    const auto elseId = document.allocateInstructionId();
    const auto joinId = document.allocateInstructionId();
    auto conditional = SctDocumentInstruction{ifId, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{elseId}}};
    auto exit = SctDocumentInstruction{jumpId, 10u};
    exit.fixedParameters = {{0u, SctInstructionReference{joinId}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        conditional, {thenId, 1u}, exit, {elseId, 1u}, {joinId, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* region = regionOf(analysis, SctStructuredRegionKind::IfElse);
    ASSERT_NE(region, nullptr);
    ASSERT_EQ(region->arms.size(), 2u);
    EXPECT_EQ(region->arms[0].kind, SctStructuredArmKind::Then);
    EXPECT_EQ(region->arms[1].kind, SctStructuredArmKind::Else);
    EXPECT_EQ(region->join->entryInstruction, joinId);
}

TEST(SctStructuredControlFlow, RecognizesBackwardJumpAsWhile) {
    SctDocument document;
    const auto ifId = document.allocateInstructionId();
    const auto bodyId = document.allocateInstructionId();
    const auto jumpId = document.allocateInstructionId();
    const auto exitId = document.allocateInstructionId();
    auto conditional = SctDocumentInstruction{ifId, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{exitId}}};
    auto back = SctDocumentInstruction{jumpId, 10u};
    back.fixedParameters = {{0u, SctInstructionReference{ifId}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        conditional, {bodyId, 1u}, back, {exitId, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* region = regionOf(analysis, SctStructuredRegionKind::While);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->join->entryInstruction, exitId);
    EXPECT_TRUE(std::ranges::any_of(region->evidence, [](const auto& evidence) {
        return evidence.kind == SctStructureEvidenceKind::BackwardTerminatorJump;
    }));
}

TEST(SctStructuredControlFlow, RecognizesSwitchCasesSharedTargetsAndCommonExit) {
    SctDocument document;
    const auto switchId = document.allocateInstructionId();
    const auto caseOne = document.allocateInstructionId();
    const auto jumpOne = document.allocateInstructionId();
    const auto caseTwo = document.allocateInstructionId();
    const auto jumpTwo = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto selector = SctDocumentInstruction{switchId, 3u};
    selector.fixedParameters = {{0u, SctEncodedWordValue{0u}}, {1u, SctEncodedWordValue{3u}}};
    selector.repeatedParameterGroups = {
        {{{2u, SctEncodedWordValue{1u}}, {3u, SctInstructionReference{caseOne}}}},
        {{{2u, SctEncodedWordValue{2u}}, {3u, SctInstructionReference{caseOne}}}},
        {{{2u, SctEncodedWordValue{3u}}, {3u, SctInstructionReference{caseTwo}}}},
    };
    auto firstExit = SctDocumentInstruction{jumpOne, 10u};
    firstExit.fixedParameters = {{0u, SctInstructionReference{join}}};
    auto secondExit = SctDocumentInstruction{jumpTwo, 10u};
    secondExit.fixedParameters = {{0u, SctInstructionReference{join}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        selector, {caseOne, 1u}, firstExit, {caseTwo, 1u}, secondExit, {join, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* region = regionOf(analysis, SctStructuredRegionKind::Switch);
    ASSERT_NE(region, nullptr);
    ASSERT_EQ(region->arms.size(), 2u);
    EXPECT_EQ(region->arms[0].caseLabels.size(), 2u);
    EXPECT_EQ(region->arms[0].caseLabels[0].value, 1);
    EXPECT_EQ(region->arms[0].caseLabels[1].value, 2);
    EXPECT_EQ(region->join->entryInstruction, join);
    EXPECT_TRUE(std::ranges::any_of(region->evidence, [](const auto& evidence) {
        return evidence.kind == SctStructureEvidenceKind::CommonForwardExit;
    }));
}

TEST(SctStructuredControlFlow, AcceptsOnlyGraphConfirmedAdjacentCaseFallthrough) {
    SctDocument document;
    const auto switchId = document.allocateInstructionId();
    const auto caseOne = document.allocateInstructionId();
    const auto caseTwo = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto selector = switchInstruction(document, {{1, caseOne}, {2, caseTwo}});
    selector.id = switchId;
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        selector, {caseOne, 1u}, {caseTwo, 1u}, {join, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* region = regionOf(analysis, SctStructuredRegionKind::Switch);
    ASSERT_NE(region, nullptr);
    ASSERT_EQ(region->arms.size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(region->evidence, [](const auto& evidence) {
        return evidence.kind == SctStructureEvidenceKind::CaseFallthrough
            && evidence.source.has_value() && evidence.target.has_value();
    }));
}

TEST(SctStructuredControlFlow, RejectsSwitchWhoseCasesHaveDifferentTerminalExits) {
    SctDocument document;
    const auto switchId = document.allocateInstructionId();
    const auto caseOne = document.allocateInstructionId();
    const auto jumpOne = document.allocateInstructionId();
    const auto caseTwo = document.allocateInstructionId();
    const auto jumpTwo = document.allocateInstructionId();
    const auto exitOne = document.allocateInstructionId();
    const auto exitTwo = document.allocateInstructionId();
    auto selector = switchInstruction(document, {{1, caseOne}, {2, caseTwo}});
    selector.id = switchId;
    auto firstJump = SctDocumentInstruction{jumpOne, 10u};
    firstJump.fixedParameters = {{0u, SctInstructionReference{exitOne}}};
    auto secondJump = SctDocumentInstruction{jumpTwo, 10u};
    secondJump.fixedParameters = {{0u, SctInstructionReference{exitTwo}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        selector, {caseOne, 1u}, firstJump, {caseTwo, 1u}, secondJump,
        {exitOne, 12u}, {exitTwo, 12u},
    }}});
    const auto analysis = analyze(document);
    EXPECT_EQ(regionOf(analysis, SctStructuredRegionKind::Switch), nullptr);
    EXPECT_TRUE(std::ranges::any_of(analysis.sections()[0].issues, [](const auto& issue) {
        return issue.kind == SctStructureIssueKind::AmbiguousSwitchCases;
    }));
}

TEST(SctStructuredControlFlow, RejectsExplicitCrossCaseJumpAsFallthrough) {
    SctDocument document;
    const auto switchId = document.allocateInstructionId();
    const auto caseOne = document.allocateInstructionId();
    const auto crossJump = document.allocateInstructionId();
    const auto caseTwo = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto selector = switchInstruction(document, {{1, caseOne}, {2, caseTwo}});
    selector.id = switchId;
    auto cross = SctDocumentInstruction{crossJump, 10u};
    cross.fixedParameters = {{0u, SctInstructionReference{caseTwo}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        selector, {caseOne, 1u}, cross, {caseTwo, 1u}, {join, 12u},
    }}});
    const auto analysis = analyze(document);
    EXPECT_EQ(regionOf(analysis, SctStructuredRegionKind::Switch), nullptr);
}

TEST(SctStructuredControlFlow, NestsConditionalWithinSwitch) {
    SctDocument document;
    const auto switchId = document.allocateInstructionId();
    const auto nestedIf = document.allocateInstructionId();
    const auto nestedBody = document.allocateInstructionId();
    const auto nestedJoin = document.allocateInstructionId();
    const auto caseTwo = document.allocateInstructionId();
    const auto caseTwoExit = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto selector = SctDocumentInstruction{switchId, 3u};
    selector.fixedParameters = {{0u, SctEncodedWordValue{0u}}, {1u, SctEncodedWordValue{2u}}};
    selector.repeatedParameterGroups = {
        {{{2u, SctEncodedWordValue{1u}}, {3u, SctInstructionReference{nestedIf}}}},
        {{{2u, SctEncodedWordValue{2u}}, {3u, SctInstructionReference{caseTwo}}}},
    };
    auto conditional = SctDocumentInstruction{nestedIf, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{nestedJoin}}};
    auto firstExit = SctDocumentInstruction{nestedJoin, 10u};
    firstExit.fixedParameters = {{0u, SctInstructionReference{join}}};
    auto secondExit = SctDocumentInstruction{caseTwoExit, 10u};
    secondExit.fixedParameters = {{0u, SctInstructionReference{join}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        selector, conditional, {nestedBody, 1u}, firstExit,
        {caseTwo, 1u}, secondExit, {join, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* switchRegion = regionOf(analysis, SctStructuredRegionKind::Switch);
    const auto* ifRegion = regionOf(analysis, SctStructuredRegionKind::If);
    ASSERT_NE(switchRegion, nullptr);
    ASSERT_NE(ifRegion, nullptr);
    EXPECT_EQ(ifRegion->parent, switchRegion->id);
}

TEST(SctStructuredControlFlow, NestsWhileLoopInsideFinalSwitchCase) {
    SctDocument document;
    const auto switchId = document.allocateInstructionId();
    const auto caseOne = document.allocateInstructionId();
    const auto caseOneExit = document.allocateInstructionId();
    const auto loopHeader = document.allocateInstructionId();
    const auto loopBody = document.allocateInstructionId();
    const auto loopBack = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto selector = switchInstruction(document, {{1, caseOne}, {2, loopHeader}});
    selector.id = switchId;
    auto firstExit = SctDocumentInstruction{caseOneExit, 10u};
    firstExit.fixedParameters = {{0u, SctInstructionReference{join}}};
    auto conditional = SctDocumentInstruction{loopHeader, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{join}}};
    auto back = SctDocumentInstruction{loopBack, 10u};
    back.fixedParameters = {{0u, SctInstructionReference{loopHeader}}};
    document.sections.push_back({document.allocateSectionId(), "test", SctScriptSectionContent{{
        selector, {caseOne, 1u}, firstExit, conditional, {loopBody, 1u}, back,
        {join, 12u},
    }}});
    const auto analysis = analyze(document);
    const auto* switchRegion = regionOf(analysis, SctStructuredRegionKind::Switch);
    const auto* whileRegion = regionOf(analysis, SctStructuredRegionKind::While);
    ASSERT_NE(switchRegion, nullptr);
    ASSERT_NE(whileRegion, nullptr);
    EXPECT_EQ(whileRegion->parent, switchRegion->id);
}

TEST(SctStructuredControlFlow, ReportsMultipleEntryIrreducibleCycleWithoutGroupingIt) {
    SctDocument document;
    const auto branchId = document.allocateInstructionId();
    const auto left = document.allocateInstructionId();
    const auto right = document.allocateInstructionId();
    auto choose = SctDocumentInstruction{branchId, 0u};
    choose.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{right}}};
    auto leftJump = SctDocumentInstruction{left, 10u};
    leftJump.fixedParameters = {{0u, SctInstructionReference{right}}};
    auto rightJump = SctDocumentInstruction{right, 10u};
    rightJump.fixedParameters = {{0u, SctInstructionReference{left}}};
    document.sections.push_back({document.allocateSectionId(), "test",
        SctScriptSectionContent{{choose, leftJump, rightJump}}});
    const auto analysis = analyze(document);
    EXPECT_TRUE(std::ranges::any_of(analysis.sections()[0].issues, [](const auto& issue) {
        return issue.kind == SctStructureIssueKind::IrreducibleCycle;
    }));
    EXPECT_TRUE(analysis.sections()[0].regions.empty());
}

TEST(SctStructuredControlFlow, OpaqueGapHintCanOnlyProduceEvidenceLimitedLoop) {
    auto fixture = importedBackwardGapFixture();
    auto& instructions = std::get<SctScriptSectionContent>(
        fixture.document.sections.front().content).instructions;
    auto& historicalSource = instructions[1];
    historicalSource.opcode = 125u;
    historicalSource.fixedParameters.clear();
    historicalSource.repeatedParameterGroups.clear();
    const auto canonical = SctDocumentAnalysis::build(
        fixture.document, &fixture.evidence);
    const auto analysis = SctStructuredControlFlowAnalysis::build(
        fixture.document, canonical);
    const auto* region = regionOf(analysis, SctStructuredRegionKind::NaturalLoop);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->strength, SctStructureClaimStrength::EvidenceLimited);
    EXPECT_EQ(region->confidence, SctSemanticConfidence::Heuristic);
    EXPECT_TRUE(std::ranges::any_of(region->evidence, [](const auto& evidence) {
        return evidence.kind == SctStructureEvidenceKind::ImportedOpaqueControlFlowGap
            && evidence.opaqueAttachment.has_value();
    }));
}

TEST(SctStructuredControlFlow, CurrentEdgeWinsWhenImportedOpaqueEvidenceConflicts) {
    auto fixture = importedBackwardGapFixture();
    auto& instructions = std::get<SctScriptSectionContent>(
        fixture.document.sections.front().content).instructions;
    auto& jump = instructions[1];
    jump.fixedParameters = {{0u, SctInstructionReference{jump.id}}};
    const auto canonical = SctDocumentAnalysis::build(
        fixture.document, &fixture.evidence);
    const auto analysis = SctStructuredControlFlowAnalysis::build(
        fixture.document, canonical);
    ASSERT_EQ(analysis.sections().size(), 1u);
    EXPECT_TRUE(std::ranges::any_of(analysis.sections()[0].issues, [](const auto& issue) {
        return issue.kind == SctStructureIssueKind::HistoricalEdgeConflict;
    }));
    EXPECT_TRUE(std::ranges::none_of(analysis.sections()[0].regions, [](const auto& region) {
        return region.strength == SctStructureClaimStrength::EvidenceLimited;
    }));
}

TEST(SctStructuredControlFlow, CrossSectionBranchesRemainUnstructured) {
    SctDocument document;
    const auto branchId = document.allocateInstructionId();
    const auto local = document.allocateInstructionId();
    const auto remote = document.allocateInstructionId();
    auto conditional = SctDocumentInstruction{branchId, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{remote}}};
    document.sections.push_back({document.allocateSectionId(), "one",
        SctScriptSectionContent{{conditional, {local, 12u}}}});
    document.sections.push_back({document.allocateSectionId(), "two",
        SctScriptSectionContent{{{remote, 12u}}}});
    const auto analysis = analyze(document);
    EXPECT_EQ(regionOf(analysis, SctStructuredRegionKind::If), nullptr);
    EXPECT_TRUE(std::ranges::any_of(analysis.sections()[0].issues, [](const auto& issue) {
        return issue.kind == SctStructureIssueKind::CrossSectionControlFlow;
    }));
}

TEST(SctStructuredControlFlow, OutlineContainsEachInstructionExactlyOnce) {
    SctDocument document;
    const auto ifId = document.allocateInstructionId();
    const auto body = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto conditional = SctDocumentInstruction{ifId, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{join}}};
    document.sections.push_back({document.allocateSectionId(), "test",
        SctScriptSectionContent{{conditional, {body, 1u}, {join, 12u}}}});
    const auto analysis = analyze(document);
    std::vector<SctInstructionId> found;
    collectOutlineInstructions(analysis.sections()[0].outline, found);
    std::ranges::sort(found);
    EXPECT_EQ(found, (std::vector<SctInstructionId>{ifId, body, join}));
}

TEST(SctStructuredControlFlow, AnalysisOwnsIdsAfterDocumentDestruction) {
    std::optional<SctStructuredControlFlowAnalysis> retained;
    SctInstructionId instructionId;
    {
        SctDocument document;
        instructionId = document.allocateInstructionId();
        document.sections.push_back({document.allocateSectionId(), "test",
            SctScriptSectionContent{{{instructionId, 12u}}}});
        retained = analyze(document);
    }
    ASSERT_TRUE(retained.has_value());
    ASSERT_NE(retained->blockContaining(instructionId), nullptr);
    EXPECT_EQ(retained->blockContaining(instructionId)->instructions.front(), instructionId);
}

TEST(SctStructuredControlFlow, RepeatedAnalysisIsDeterministicAndQueriesUseStableIds) {
    SctDocument document;
    const auto ifId = document.allocateInstructionId();
    const auto body = document.allocateInstructionId();
    const auto join = document.allocateInstructionId();
    auto conditional = SctDocumentInstruction{ifId, 0u};
    conditional.fixedParameters = {{0u, SctEncodedWordValue{0u}},
        {1u, SctInstructionReference{join}}};
    document.sections.push_back({document.allocateSectionId(), "test",
        SctScriptSectionContent{{conditional, {body, 1u}, {join, 12u}}}});
    const auto first = analyze(document);
    const auto second = analyze(document);
    ASSERT_EQ(first.sections().size(), second.sections().size());
    EXPECT_EQ(first.sections()[0], second.sections()[0]);
    ASSERT_EQ(first.sections()[0].blocks.size(), second.sections()[0].blocks.size());
    ASSERT_EQ(first.sections()[0].regions.size(), second.sections()[0].regions.size());
    for (std::size_t index = 0; index < first.sections()[0].blocks.size(); ++index)
        EXPECT_EQ(first.sections()[0].blocks[index].id, second.sections()[0].blocks[index].id);
    ASSERT_NE(first.blockContaining(body), nullptr);
    const auto containing = first.regionsContaining(body);
    ASSERT_EQ(containing.size(), 1u);
    EXPECT_EQ(first.findRegion(containing.front().id)->id, containing.front().id);
}

}  // namespace
