#include "SalsaCore/Sct/SctSemanticUsageIndex.h"

#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

namespace {
using namespace salsa::core;
using namespace spice::sct;

SctCanonicalExpression variable(
    const SctExpressionVariableKind kind,
    const std::uint32_t index) {
    const auto built = SctExpressionFactory::variable(kind, index);
    EXPECT_TRUE(built.expression.has_value());
    return *built.expression;
}

SctCanonicalExpression binary(
    const SctExpressionBinaryOperator operation,
    SctCanonicalExpression left,
    SctCanonicalExpression right) {
    const auto built = SctExpressionFactory::binaryOperator(
        operation, std::move(left), std::move(right));
    EXPECT_TRUE(built.expression.has_value());
    return *built.expression;
}

const SctParameterAddress& parameterOwner(const SctExpressionSite& site) {
    EXPECT_TRUE(std::holds_alternative<SctParameterAddress>(site.owner));
    return std::get<SctParameterAddress>(site.owner);
}

SctSemanticUsageIndex buildTemporaryIndex() {
    SctDocument document;
    const auto section = document.allocateSectionId();
    const auto instruction = document.allocateInstructionId();
    document.sections.push_back({ section, "TEMP",
        SctScriptSectionContent{{ SctDocumentInstruction{ instruction, 12 } }} });
    return SctSemanticUsageIndex::build(document);
}
}  // namespace

TEST(SctSemanticUsageIndex, EmptyAndNonScriptDocumentsProduceEmptyIndexes) {
    SctDocument empty;
    const auto emptyIndex = SctSemanticUsageIndex::build(empty);
    EXPECT_TRUE(emptyIndex.opcodeUsages().empty());
    EXPECT_TRUE(emptyIndex.referenceUsages().empty());
    EXPECT_TRUE(emptyIndex.variableUsages().empty());
    EXPECT_TRUE(emptyIndex.unresolvedReferences().empty());
    EXPECT_TRUE(emptyIndex.opaqueParameters().empty());
    EXPECT_TRUE(emptyIndex.opaqueExpressions().empty());

    SctDocument nonScript;
    nonScript.sections.push_back({ nonScript.allocateSectionId(), "LABEL",
        SctLabelSectionContent{} });
    nonScript.sections.push_back({ nonScript.allocateSectionId(), "OPAQUE",
        SctOpaqueSectionContent{} });
    const auto nonScriptIndex = SctSemanticUsageIndex::build(nonScript);
    EXPECT_TRUE(nonScriptIndex.opcodeUsages().empty());
    EXPECT_TRUE(nonScriptIndex.referenceUsages().empty());
}

TEST(SctSemanticUsageIndex, OpcodeInventoryAndQueriesFollowPhysicalDocumentOrder) {
    SctDocument document;
    const auto firstSection = document.allocateSectionId();
    const auto first = document.allocateInstructionId();
    const auto second = document.allocateInstructionId();
    const auto thirdSection = document.allocateSectionId();
    const auto third = document.allocateInstructionId();
    document.sections.push_back({ firstSection, "FIRST", SctScriptSectionContent{{
        SctDocumentInstruction{ first, 12 },
        SctDocumentInstruction{ second, 10 },
    }} });
    document.sections.push_back({ document.allocateSectionId(), "LABEL",
        SctLabelSectionContent{} });
    document.sections.push_back({ thirdSection, "THIRD", SctScriptSectionContent{{
        SctDocumentInstruction{ third, 12 },
    }} });

    const auto before = SctSemanticUsageIndex::build(document);
    ASSERT_EQ(before.opcodeUsages().size(), 3u);
    EXPECT_EQ(before.opcodeUsages()[0], (SctOpcodeUsage{ 12, first }));
    EXPECT_EQ(before.opcodeUsages()[1], (SctOpcodeUsage{ 10, second }));
    EXPECT_EQ(before.opcodeUsages()[2], (SctOpcodeUsage{ 12, third }));
    EXPECT_EQ(before.usagesForOpcode(12), (std::vector<SctOpcodeUsage>{
        { 12, first }, { 12, third },
    }));
    EXPECT_TRUE(before.usagesForOpcode(999).empty());

    auto& firstInstructions = std::get<SctScriptSectionContent>(
        document.sections.front().content).instructions;
    std::swap(firstInstructions[0], firstInstructions[1]);
    const auto moved = SctSemanticUsageIndex::build(document);
    EXPECT_EQ(moved.opcodeUsages()[0].instruction, second);
    EXPECT_EQ(moved.opcodeUsages()[1].instruction, first);
    EXPECT_EQ(before.opcodeUsages()[0].instruction, first);

    firstInstructions.erase(firstInstructions.begin() + 1);
    const auto deleted = SctSemanticUsageIndex::build(document);
    ASSERT_EQ(deleted.opcodeUsages().size(), 2u);
    EXPECT_EQ(deleted.opcodeUsages()[0].instruction, second);
    EXPECT_EQ(deleted.opcodeUsages()[1].instruction, third);
}

TEST(SctSemanticUsageIndex, TypedReferenceQueriesPreserveFixedAndRepeatedOrder) {
    SctDocument document;
    const auto section = document.allocateSectionId();
    const auto source = document.allocateInstructionId();
    const auto otherSource = document.allocateInstructionId();
    const auto instructionTarget = document.allocateInstructionId();
    const auto stringTarget = document.allocateStringId();
    const auto footerTarget = document.allocateFooterEntryId();

    SctDocumentInstruction first{ source, 3 };
    first.fixedParameters.push_back({ 5, SctInstructionReference{ instructionTarget } });
    first.fixedParameters.push_back({ 1, SctStringReference{ stringTarget } });
    first.repeatedParameterGroups.push_back({{{
        9, SctFooterEntryReference{ footerTarget },
    }}});
    first.repeatedParameterGroups.push_back({{{
        3, SctInstructionReference{ instructionTarget },
    }}});
    SctDocumentInstruction second{ otherSource, 10 };
    second.fixedParameters.push_back({ 0, SctInstructionReference{ instructionTarget } });
    document.sections.push_back({ section, "SCRIPT", SctScriptSectionContent{{
        first, second, SctDocumentInstruction{ instructionTarget, 12 },
    }} });

    const auto index = SctSemanticUsageIndex::build(document);
    ASSERT_EQ(index.referenceUsages().size(), 5u);
    EXPECT_EQ(index.referenceUsages()[0].source,
        (SctParameterSite{ source, { 5, std::nullopt } }));
    EXPECT_EQ(index.referenceUsages()[1].target,
        (SctDocumentReferenceTarget{ stringTarget }));
    EXPECT_EQ(index.referenceUsages()[2].source.parameter,
        (SctParameterAddress{ 9, 0u }));
    EXPECT_EQ(index.referenceUsages()[3].source.parameter,
        (SctParameterAddress{ 3, 1u }));

    const auto outbound = index.outboundReferences(source);
    ASSERT_EQ(outbound.size(), 4u);
    EXPECT_EQ(outbound.front(), index.referenceUsages().front());
    EXPECT_TRUE(index.outboundReferences(SctInstructionId{ 999 }).empty());

    const auto inboundInstruction = index.inboundReferences(
        SctDocumentReferenceTarget{ instructionTarget });
    ASSERT_EQ(inboundInstruction.size(), 3u);
    EXPECT_EQ(inboundInstruction[0].source.instruction, source);
    EXPECT_EQ(inboundInstruction[1].source.parameter.repeatedGroupOrdinal, 1u);
    EXPECT_EQ(inboundInstruction[2].source.instruction, otherSource);
    ASSERT_EQ(index.inboundReferences(SctDocumentReferenceTarget{ stringTarget }).size(), 1u);
    ASSERT_EQ(index.inboundReferences(SctDocumentReferenceTarget{ footerTarget }).size(), 1u);
    EXPECT_TRUE(index.inboundReferences(
        SctDocumentReferenceTarget{ SctInstructionId{ 999 } }).empty());
}

TEST(SctSemanticUsageIndex, VariablesRetainKindOwnerAndPreorderExpressionPath) {
    SctDocument document;
    const auto section = document.allocateSectionId();
    const auto instructionId = document.allocateInstructionId();
    SctDocumentInstruction instruction{ instructionId, 100 };
    instruction.scheduledExpression = variable(SctExpressionVariableKind::Byte, 7);
    instruction.fixedParameters.push_back({ 0, binary(
        SctExpressionBinaryOperator::Add,
        variable(SctExpressionVariableKind::Integer, 7),
        variable(SctExpressionVariableKind::Bit, 7)) });
    instruction.fixedParameters.push_back({ 1, SctExpressionFactory::decimalLiteral(7) });
    instruction.repeatedParameterGroups.push_back({{{
        2, variable(SctExpressionVariableKind::Float, 7),
    }}});

    SctCanonicalExpressionNode secondary{
        SctCanonicalExpressionNodeKind::SecondaryValue, 0x50000007u, {}, {} };
    instruction.fixedParameters.push_back({ 3,
        SctCanonicalExpression{ secondary, SctExpressionTermination::StopCode } });
    SctCanonicalExpressionNode raw{
        SctCanonicalExpressionNodeKind::RawValue, 0x20000007u, {}, {} };
    instruction.fixedParameters.push_back({ 4,
        SctCanonicalExpression{ raw, SctExpressionTermination::StopCode } });
    document.sections.push_back({ section, "SCRIPT",
        SctScriptSectionContent{{ instruction }} });

    const auto index = SctSemanticUsageIndex::build(document);
    ASSERT_EQ(index.variableUsages().size(), 4u);
    const auto& scheduled = index.variableUsages()[0];
    EXPECT_EQ(scheduled.variable, (SctVariableIdentity{ SctVariableKind::Byte, 7 }));
    EXPECT_TRUE(std::holds_alternative<SctScheduledExpressionSite>(scheduled.source.owner));
    EXPECT_TRUE(scheduled.source.childPath.empty());

    EXPECT_EQ(index.variableUsages()[1].variable,
        (SctVariableIdentity{ SctVariableKind::Integer, 7 }));
    EXPECT_EQ(index.variableUsages()[1].source.childPath,
        (std::vector<std::uint32_t>{ 0 }));
    EXPECT_EQ(parameterOwner(index.variableUsages()[1].source),
        (SctParameterAddress{ 0, std::nullopt }));
    EXPECT_EQ(index.variableUsages()[2].variable,
        (SctVariableIdentity{ SctVariableKind::Bit, 7 }));
    EXPECT_EQ(index.variableUsages()[2].source.childPath,
        (std::vector<std::uint32_t>{ 1 }));
    EXPECT_EQ(index.variableUsages()[3].variable,
        (SctVariableIdentity{ SctVariableKind::Float, 7 }));
    EXPECT_EQ(parameterOwner(index.variableUsages()[3].source),
        (SctParameterAddress{ 2, 0u }));

    EXPECT_EQ(index.usagesForVariable({ SctVariableKind::Integer, 7 }).size(), 1u);
    EXPECT_EQ(index.usagesForVariable({ SctVariableKind::Float, 7 }).size(), 1u);
    EXPECT_EQ(index.usagesForVariable({ SctVariableKind::Bit, 7 }).size(), 1u);
    EXPECT_EQ(index.usagesForVariable({ SctVariableKind::Byte, 7 }).size(), 1u);
    EXPECT_TRUE(index.usagesForVariable({ SctVariableKind::Integer, 8 }).empty());
}

TEST(SctSemanticUsageIndex, IncompleteEvidenceIsReportedWithoutInventingSemantics) {
    SctDocument document;
    const auto section = document.allocateSectionId();
    const auto instructionId = document.allocateInstructionId();
    SctDocumentInstruction instruction{ instructionId, 200 };
    instruction.scheduledExpression = SctCanonicalExpression{
        SctOpaqueExpression{{ 0x50000005u }}, SctExpressionTermination::StopCode };
    instruction.fixedParameters.push_back({ 1, SctUnresolvedReferenceValue{
        SctExpectedReferenceTarget{ SctReferenceTargetStorage::IndexedString,
            SctTextKind::SctString },
        { 0x12345678u, 0x9abcdef0u },
    } });
    instruction.fixedParameters.push_back({ 2,
        SctOpaqueParameterValue{{ 0x50000005u, 0x20000006u, 0x0000001du }} });
    instruction.fixedParameters.push_back({ 3, SctCanonicalExpression{
        SctOpaqueExpression{{ 0x10000007u, 0x0000001du }},
        SctExpressionTermination::StopCode } });
    instruction.fixedParameters.push_back({ 4,
        SctTerminatedWordSequenceValue{{ 0x40000008u, 0x0000001du }} });
    document.sections.push_back({ section, "SCRIPT",
        SctScriptSectionContent{{ instruction }} });

    const auto index = SctSemanticUsageIndex::build(document);
    EXPECT_TRUE(index.referenceUsages().empty());
    EXPECT_TRUE(index.variableUsages().empty());
    ASSERT_EQ(index.unresolvedReferences().size(), 1u);
    EXPECT_EQ(index.unresolvedReferences()[0].source.parameter,
        (SctParameterAddress{ 1, std::nullopt }));
    EXPECT_EQ(index.unresolvedReferences()[0].expectedTarget,
        (SctExpectedReferenceTarget{ SctReferenceTargetStorage::IndexedString,
            SctTextKind::SctString }));
    EXPECT_EQ(index.unresolvedReferences()[0].encodedWordCount, 2u);
    ASSERT_EQ(index.opaqueParameters().size(), 1u);
    EXPECT_EQ(index.opaqueParameters()[0].source.parameter.schemaIndex, 2u);
    EXPECT_EQ(index.opaqueParameters()[0].wordCount, 3u);
    ASSERT_EQ(index.opaqueExpressions().size(), 2u);
    EXPECT_TRUE(std::holds_alternative<SctScheduledExpressionSite>(
        index.opaqueExpressions()[0].source.owner));
    EXPECT_EQ(index.opaqueExpressions()[0].wordCount, 1u);
    EXPECT_EQ(parameterOwner(index.opaqueExpressions()[1].source).schemaIndex, 3u);
    EXPECT_TRUE(index.opaqueExpressions()[1].source.childPath.empty());
    EXPECT_EQ(index.opaqueExpressions()[1].wordCount, 2u);
}

TEST(SctSemanticUsageIndex, OwnsResultsAndBestEffortPreservesInvalidPhysicalClaims) {
    const auto temporary = buildTemporaryIndex();
    ASSERT_EQ(temporary.opcodeUsages().size(), 1u);
    EXPECT_EQ(temporary.opcodeUsages().front().opcode, 12u);

    SctDocument invalid;
    invalid.sections.push_back({ SctSectionId{}, "INVALID", SctScriptSectionContent{{
        SctDocumentInstruction{ SctInstructionId{}, 4 },
        SctDocumentInstruction{ SctInstructionId{}, 5 },
    }} });
    const auto bestEffort = SctSemanticUsageIndex::build(invalid);
    ASSERT_EQ(bestEffort.opcodeUsages().size(), 2u);
    EXPECT_EQ(bestEffort.opcodeUsages()[0],
        (SctOpcodeUsage{ 4, SctInstructionId{} }));
    EXPECT_EQ(bestEffort.opcodeUsages()[1],
        (SctOpcodeUsage{ 5, SctInstructionId{} }));
}
