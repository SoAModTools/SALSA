#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Application/ActivityLifecycle.h"

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

TEST(DiagnosticTest, CurrentIssueSeverityExcludesInformation) {
    EXPECT_FALSE(isCurrentDiagnosticSeverity(DiagnosticSeverity::Info));
    EXPECT_TRUE(isCurrentDiagnosticSeverity(DiagnosticSeverity::Warning));
    EXPECT_TRUE(isCurrentDiagnosticSeverity(DiagnosticSeverity::Error));
}

TEST(DiagnosticTest, EveryDeclaredCodeHasAStableDisplayName) {
    const auto last = static_cast<int>(DiagnosticCode::LegacyImportDestinationNotFresh);
    for (int ordinal = 0; ordinal <= last; ++ordinal) {
        const auto name = diagnosticCodeName(static_cast<DiagnosticCode>(ordinal));
        EXPECT_FALSE(name.empty()) << ordinal;
        EXPECT_NE(name, "Unknown") << ordinal;
    }
    EXPECT_EQ(diagnosticCodeName(static_cast<DiagnosticCode>(last + 1)), "Unknown");
}

TEST(ActivityLifecycleTest, ReportsRaisedResolvedAndReraisedIssues) {
    IssueActivityTracker<std::string> tracker;
    auto events = tracker.observe({"warning"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].transition, IssueActivityTransition::Raised);

    EXPECT_TRUE(tracker.observe({"warning"}).empty());
    events = tracker.observe({});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].transition, IssueActivityTransition::Resolved);

    events = tracker.observe({"warning"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].transition, IssueActivityTransition::Reraised);
}

}  // namespace
}  // namespace salsa::core
