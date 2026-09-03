#include "SalsaCore/Sct/SctExpressionLanguage.h"

#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctScptEncoding.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <variant>

namespace {
using namespace salsa::core;
using namespace spice::sct;

std::vector<std::uint32_t> parseWords(const std::string& text) {
    const auto parsed = SctExpressionLanguage::parse(text);
    EXPECT_TRUE(parsed.succeeded()) << (parsed.issues.empty()
        ? std::string{} : parsed.issues.front().message);
    return parsed.expression
        ? encodeSctCanonicalExpressionWords(*parsed.expression)
        : std::vector<std::uint32_t>{};
}
} // namespace

TEST(SctExpressionLanguage, ParsesEveryDocumentedExpression) {
    // Keep this list synchronized with the valid examples in Docs/Parameters.md.
    constexpr std::array examples{
        std::string_view{"3"},
        std::string_view{"3.5"},
        std::string_view{"-2.25"},
        std::string_view{"3f"},
        std::string_view{"3.5f"},
        std::string_view{"1e-3f"},
        std::string_view{"ByteVar[87]"},
        std::string_view{"BitVar[4]"},
        std::string_view{"FloatVar[12]"},
        std::string_view{"IntVar[24]"},
        std::string_view{"NegatedIntVar[87]"},
        std::string_view{"Low16IntVar[15]"},
        std::string_view{"IntInput[87]"},
        std::string_view{"Gold"},
        std::string_view{"VyseCurrentHP"},
        std::string_view{"InlineValue[0x7F7FFFFF]"},
        std::string_view{"ByteVar[87] == 3"},
        std::string_view{"(IntVar[24] + 2) * 4"},
        std::string_view{"IntVar[24] == 1 && ByteVar[87] != 0"},
    };
    for (const auto example : examples) {
        const auto parsed = SctExpressionLanguage::parse(std::string(example));
        EXPECT_TRUE(parsed.succeeded()) << example << ": "
            << (parsed.issues.empty() ? std::string{} : parsed.issues.front().message);
    }
}

TEST(SctExpressionLanguage, AppliesCStylePrecedenceAndLeftAssociativity) {
    const auto parsed = SctExpressionLanguage::parse(
        "IntVar[24] + 2 * 3 == 7 && ByteVar[87] != 0");
    ASSERT_TRUE(parsed.succeeded());
    const auto projected = SctExpressionLanguage::project(*parsed.expression);
    ASSERT_EQ(projected.availability, SctExpressionTextAvailability::Editable);
    EXPECT_EQ(projected.text,
        "IntVar[24] + 2 * 3 == 7 && ByteVar[87] != 0");

    const auto grouped = SctExpressionLanguage::parse("8 - (4 - 2)");
    ASSERT_TRUE(grouped.succeeded());
    EXPECT_EQ(SctExpressionLanguage::project(*grouped.expression).text,
        "8 - (4 - 2)");
}

TEST(SctExpressionLanguage, DistinguishesExactFixedDecimalsFromSuffixedFloats) {
    EXPECT_TRUE(SctExpressionLanguage::parse("0.5").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("0.1").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("0.1f").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("1e-3f").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("1e-3").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("8388608").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("nanf").succeeded());

    const auto fixed = parseWords("3");
    const auto floating = parseWords("3f");
    EXPECT_NE(fixed, floating);
}

TEST(SctExpressionLanguage, UsesEachFactorySpecificVariableDomain) {
    EXPECT_TRUE(SctExpressionLanguage::parse("ByteVar[268435455]").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("ByteVar[268435456]").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("FloatVar[268435455]").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("FloatVar[268435456]").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("BitVar[536870911]").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("BitVar[536870912]").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("NegatedIntVar[87]").succeeded());
    EXPECT_FALSE(SctExpressionLanguage::parse("NegatedIntVar[24]").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("IntInput[24]").succeeded());
    EXPECT_TRUE(SctExpressionLanguage::parse("IntInput[87]").succeeded());
}

TEST(SctExpressionLanguage, SupportsAllConfirmedInlineAndNamedValues) {
    for (const auto text : {"InlineValue[0x7F7FFFFF]",
            "InlineValue[0x00800000]", "InlineValue[0x7FFFFFFF]",
            "InlineValue[0x7FFFFFFE]", "Gold", "Reputation",
            "VyseCurrentHP", "AikaCurrentHP", "FinaCurrentHP",
            "DrachmaCurrentHP", "EnriqueCurrentHP", "GilderCurrentHP",
            "VyseLevel"}) {
        EXPECT_TRUE(SctExpressionLanguage::parse(text).succeeded()) << text;
    }
    EXPECT_FALSE(SctExpressionLanguage::parse(
        "InlineValue[0x7FFFFFFD]").succeeded());
}

TEST(SctExpressionLanguage, ReportsPreciseSyntaxSpans) {
    const auto parsed = SctExpressionLanguage::parse("ByteVar[87] + )");
    ASSERT_FALSE(parsed.succeeded());
    ASSERT_FALSE(parsed.issues.empty());
    EXPECT_EQ(parsed.issues.front().offset, 14u);
    EXPECT_GE(parsed.issues.front().length, 1u);
}

TEST(SctExpressionLanguage, ClassifiesExactAndNonconventionalPrograms) {
    auto left = SctExpressionFactory::byteVariable(87).expression.value();
    auto right = SctExpressionFactory::scaledDecimalLiteral(256).expression.value();
    const auto conventional = SctExpressionFactory::binaryOperator(
        SctExpressionBinaryOperator::Equal, std::move(left), std::move(right));
    ASSERT_TRUE(conventional.expression.has_value());
    EXPECT_EQ(SctExpressionLanguage::project(*conventional.expression).text,
        "ByteVar[87] == 1");

    auto operations = std::get<SctTypedScptProgram>(
        conventional.expression->body).operations;
    operations.push_back(SctExpressionFactory::stackOverwritePreviousWithTop());
    const auto stackProgram = SctExpressionFactory::program(std::move(operations));
    ASSERT_TRUE(stackProgram.expression.has_value());
    EXPECT_NE(SctExpressionLanguage::project(*stackProgram.expression).availability,
        SctExpressionTextAvailability::Editable);

    auto alternateOperations = std::get<SctTypedScptProgram>(
        conventional.expression->body).operations;
    auto& alternateOperation = std::get<SctScptBinaryOperation>(
        alternateOperations.back());
    alternateOperation.encodingWord = 0x15u;
    alternateOperation.kind = SctScptBinaryOperationKind::Arithmetic;
    const auto alternate = SctExpressionFactory::program(std::move(alternateOperations));
    ASSERT_TRUE(alternate.expression.has_value());
    const auto alternateProjection = SctExpressionLanguage::project(*alternate.expression);
    EXPECT_EQ(alternateProjection.availability, SctExpressionTextAvailability::Editable);
    EXPECT_TRUE(alternateProjection.usesAlternateOperatorEncoding);
}
