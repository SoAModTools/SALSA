#include "SalsaCore/Application/ExclusiveOperationFlow.h"

#include <gtest/gtest.h>

namespace salsa::core {
namespace {

ExclusiveOperationFlowDefinition cyclicFlow() {
    using Progress = ExclusiveOperationProgressVisibility;
    using Layout = ExclusiveOperationPageLayout;
    return {"configure",
        {{"configure", ExclusiveOperationPageRole::Configuration, Progress::Hidden, Layout::Standard},
         {"work", ExclusiveOperationPageRole::Processing, Progress::Visible, Layout::Compact},
         {"review", ExclusiveOperationPageRole::Review, Progress::Hidden, Layout::Expanded},
         {"summary", ExclusiveOperationPageRole::Summary, Progress::Hidden, Layout::Compact}},
        {{"start", "configure", "start", "work"},
         {"prepared", "work", "prepared", "review"},
         {"retry", "review", "retry", "work"},
         {"finish", "review", "finish", "summary"}}};
}

TEST(ExclusiveOperationFlowTest, TraversesDeclaredCyclesAndGuardsEdges) {
    ExclusiveOperationFlow flow(cyclicFlow());
    EXPECT_TRUE(flow.validationErrors().empty());
    EXPECT_TRUE(flow.dispatch("start"));
    EXPECT_TRUE(flow.dispatch("prepared"));
    EXPECT_FALSE(flow.dispatch("finish", [](std::string_view id) { return id != "finish"; }));
    EXPECT_TRUE(flow.dispatch("retry"));
    EXPECT_EQ(flow.currentNode()->progress, ExclusiveOperationProgressVisibility::Visible);
    EXPECT_EQ(flow.currentNode()->layout, ExclusiveOperationPageLayout::Compact);
    EXPECT_TRUE(flow.dispatch("prepared"));
    EXPECT_EQ(flow.currentNode()->progress, ExclusiveOperationProgressVisibility::Hidden);
    EXPECT_EQ(flow.currentNode()->layout, ExclusiveOperationPageLayout::Expanded);
    EXPECT_TRUE(flow.dispatch("finish"));
    EXPECT_EQ(flow.currentPage(), "summary");
    EXPECT_EQ(flow.currentNode()->progress, ExclusiveOperationProgressVisibility::Hidden);
}

TEST(ExclusiveOperationFlowTest, RejectsAmbiguousAndNonTerminalDefinitions) {
    auto definition = cyclicFlow();
    definition.edges.push_back({"again", "configure", "start", "summary"});
    definition.edges.push_back({"escape", "summary", "escape", "configure"});
    ExclusiveOperationFlow flow(std::move(definition));
    EXPECT_FALSE(flow.validationErrors().empty());
}

TEST(ExclusiveOperationFlowTest, RejectsAReachableBranchThatCannotFinish) {
    auto definition = cyclicFlow();
    definition.pages.emplace_back("dead-end", ExclusiveOperationPageRole::Review,
        ExclusiveOperationProgressVisibility::Hidden,
        ExclusiveOperationPageLayout::Standard);
    definition.edges.push_back({"strand", "review", "strand", "dead-end"});
    ExclusiveOperationFlow flow(std::move(definition));

    EXPECT_FALSE(flow.validationErrors().empty());
}

TEST(ExclusiveOperationFlowTest, DialogSizingGrowsMonotonicallyAndClampsToScreen) {
    EXPECT_EQ(resolveExclusiveOperationSize(ExclusiveOperationPageLayout::Compact,
        {}, {1920, 1080}), (ExclusiveOperationLogicalSize{560, 260}));
    EXPECT_EQ(resolveExclusiveOperationSize(ExclusiveOperationPageLayout::Standard,
        {900, 650}, {1920, 1080}, ExclusiveOperationLogicalSize{1000, 700}),
        (ExclusiveOperationLogicalSize{1000, 700}));
    EXPECT_EQ(resolveExclusiveOperationSize(ExclusiveOperationPageLayout::Expanded,
        {1500, 1000}, {1200, 800}),
        (ExclusiveOperationLogicalSize{1080, 720}));
}

TEST(ExclusiveOperationGateTest, LeaseExcludesAndMovePreservesOwnership) {
    ExclusiveOperationGate gate;
    auto first = gate.tryAcquire();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(gate.active());
    EXPECT_FALSE(gate.tryAcquire().has_value());
    const auto generation = first->generation();
    ExclusiveOperationGate::Lease moved = std::move(*first);
    EXPECT_EQ(moved.generation(), generation);
    moved.release();
    EXPECT_FALSE(gate.active());
    EXPECT_TRUE(gate.tryAcquire().has_value());
}

}  // namespace
}  // namespace salsa::core
