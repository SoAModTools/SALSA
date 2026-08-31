#include "SalsaCore/Foundation/Result.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace salsa::core {
namespace {

TEST(ResultTest, CarriesAValueAndNonErrorDiagnostics) {
    std::vector<Diagnostic> diagnostics{
        { DiagnosticSeverity::Warning, DiagnosticCode::ReparsePointSkipped, "warning", std::nullopt },
    };

    auto result = Result<std::string>::success("value", diagnostics);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "value");
    ASSERT_EQ(result.diagnostics().size(), 1U);
    EXPECT_EQ(result.diagnostics().front().severity, DiagnosticSeverity::Warning);
}

TEST(ResultTest, CarriesMoveOnlyValues) {
    auto result = Result<std::unique_ptr<int>>::success(std::make_unique<int>(42));

    ASSERT_TRUE(result);
    auto value = std::move(result).takeValue();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
}

TEST(ResultTest, RepresentsTypedFailure) {
    auto result = Result<int>::failure(Diagnostic{
        DiagnosticSeverity::Error,
        DiagnosticCode::AssetNotFound,
        "missing",
        std::nullopt,
    });

    EXPECT_FALSE(result);
    EXPECT_FALSE(result.hasValue());
    ASSERT_EQ(result.diagnostics().size(), 1U);
    EXPECT_EQ(result.diagnostics().front().code, DiagnosticCode::AssetNotFound);
}

TEST(ResultTest, SupportsVoidSuccessAndFailure) {
    EXPECT_TRUE(Result<void>::success());
    EXPECT_FALSE(Result<void>::failure(Diagnostic{
        DiagnosticSeverity::Error,
        DiagnosticCode::Cancelled,
        "cancelled",
        std::nullopt,
    }));
}

}  // namespace
}  // namespace salsa::core
