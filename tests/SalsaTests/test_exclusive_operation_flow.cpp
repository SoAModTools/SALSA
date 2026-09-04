#include "SalsaCore/Application/ExclusiveOperationFlow.h"

#include <gtest/gtest.h>

namespace salsa::core {
namespace {

ExclusiveOperationFlowDefinition cyclicFlow() {
    return {"configure",
        {{"configure", ExclusiveOperationPageRole::Configuration},
         {"work", ExclusiveOperationPageRole::Processing},
         {"review", ExclusiveOperationPageRole::Review},
         {"summary", ExclusiveOperationPageRole::Summary}},
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
    EXPECT_TRUE(flow.dispatch("prepared"));
    EXPECT_TRUE(flow.dispatch("finish"));
    EXPECT_EQ(flow.currentPage(), "summary");
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
    definition.pages.push_back({"dead-end", ExclusiveOperationPageRole::Review});
    definition.edges.push_back({"strand", "review", "strand", "dead-end"});
    ExclusiveOperationFlow flow(std::move(definition));

    EXPECT_FALSE(flow.validationErrors().empty());
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
