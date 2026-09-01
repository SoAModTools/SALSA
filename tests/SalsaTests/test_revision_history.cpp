#include "SalsaCore/History/RevisionHistory.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

struct TestState final {
    int value = 0;
};

struct MoveOnlyState final {
    explicit MoveOnlyState(const int value)
        : value(std::make_unique<int>(value)) {}

    MoveOnlyState(const MoveOnlyState&) = delete;
    MoveOnlyState& operator=(const MoveOnlyState&) = delete;
    MoveOnlyState(MoveOnlyState&&) noexcept = default;
    MoveOnlyState& operator=(MoveOnlyState&&) noexcept = default;

    std::unique_ptr<int> value;
};

[[nodiscard]] std::shared_ptr<const TestState> state(const int value) {
    return std::make_shared<const TestState>(TestState{ value });
}

TEST(RevisionHistoryTest, StartsAtAValidCleanCheckpoint) {
    const auto initial = state(10);
    RevisionHistory<TestState> history(initial);

    const auto current = history.currentRevision();
    EXPECT_EQ(current.id, RevisionId{ 1 });
    EXPECT_TRUE(current.id.valid());
    EXPECT_EQ(current.state, initial);
    EXPECT_EQ(history.checkpointRevision(), current.id);
    EXPECT_EQ(history.checkpointState(), initial);
    EXPECT_FALSE(history.isDirty());
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
    EXPECT_FALSE(history.undoDescription().has_value());
    EXPECT_FALSE(history.redoDescription().has_value());
    EXPECT_FALSE(history.undo().has_value());
    EXPECT_FALSE(history.redo().has_value());
}

TEST(RevisionHistoryTest, CommitsAndNavigatesImmutableSnapshots) {
    RevisionHistory<TestState> history(state(0));
    const auto first = state(1);
    const auto second = state(2);

    const auto firstCommit = history.commit(first, "First edit");
    const auto secondCommit = history.commit(second, "Second edit");

    EXPECT_TRUE(firstCommit.created);
    EXPECT_EQ(firstCommit.revision, RevisionId{ 2 });
    EXPECT_TRUE(secondCommit.created);
    EXPECT_EQ(secondCommit.revision, RevisionId{ 3 });
    ASSERT_TRUE(history.undoDescription().has_value());
    EXPECT_EQ(*history.undoDescription(), "Second edit");

    const auto undo = history.undo();
    ASSERT_TRUE(undo.has_value());
    EXPECT_EQ(undo->direction, HistoryDirection::Undo);
    EXPECT_EQ(undo->from, RevisionId{ 3 });
    EXPECT_EQ(undo->to, RevisionId{ 2 });
    EXPECT_EQ(undo->state, first);
    EXPECT_EQ(undo->description, "Second edit");
    ASSERT_TRUE(history.redoDescription().has_value());
    EXPECT_EQ(*history.redoDescription(), "Second edit");

    const auto redo = history.redo();
    ASSERT_TRUE(redo.has_value());
    EXPECT_EQ(redo->direction, HistoryDirection::Redo);
    EXPECT_EQ(redo->from, RevisionId{ 2 });
    EXPECT_EQ(redo->to, RevisionId{ 3 });
    EXPECT_EQ(redo->state, second);
    EXPECT_EQ(redo->description, "Second edit");
}

TEST(RevisionHistoryTest, BranchingInvalidatesRedoWithoutReusingRevisionIds) {
    RevisionHistory<TestState> history(state(0));
    (void)history.commit(state(1), "First edit");
    const auto abandoned = state(2);
    (void)history.commit(abandoned, "Abandoned edit");
    ASSERT_TRUE(history.undo().has_value());
    ASSERT_TRUE(history.canRedo());

    const auto branch = history.commit(state(3), "Branch edit");

    EXPECT_TRUE(branch.created);
    EXPECT_EQ(branch.revision, RevisionId{ 4 });
    EXPECT_FALSE(history.canRedo());
    EXPECT_FALSE(history.redoDescription().has_value());
    EXPECT_EQ(history.currentRevision().state->value, 3);
}

TEST(RevisionHistoryTest, SamePointerCommitIsANoOpAndPreservesRedo) {
    const auto initial = state(0);
    RevisionHistory<TestState> history(initial);
    const auto edited = state(1);
    (void)history.commit(edited, "Edit");
    ASSERT_TRUE(history.undo().has_value());
    ASSERT_TRUE(history.canRedo());

    const auto noOp = history.commit(initial, "No-op edit");

    EXPECT_FALSE(noOp.created);
    EXPECT_EQ(noOp.revision, RevisionId{ 1 });
    EXPECT_TRUE(history.canRedo());
    ASSERT_TRUE(history.redo().has_value());
    EXPECT_EQ(history.currentRevision().id, RevisionId{ 2 });
}

TEST(RevisionHistoryTest, DirtyStateUsesExactCheckpointRevision) {
    RevisionHistory<TestState> history(state(7));
    (void)history.commit(state(8), "Edit");
    EXPECT_TRUE(history.isDirty());

    history.markCheckpoint();
    EXPECT_FALSE(history.isDirty());
    EXPECT_EQ(history.checkpointRevision(), RevisionId{ 2 });

    ASSERT_TRUE(history.undo().has_value());
    EXPECT_TRUE(history.isDirty());
    ASSERT_TRUE(history.redo().has_value());
    EXPECT_FALSE(history.isDirty());

    ASSERT_TRUE(history.undo().has_value());
    const auto semanticallyEqual = state(8);
    const auto replacement = history.commit(semanticallyEqual, "Equivalent edit");
    EXPECT_EQ(replacement.revision, RevisionId{ 3 });
    EXPECT_TRUE(history.isDirty());
}

TEST(RevisionHistoryTest, RetainsCheckpointAfterItsHistoryBranchIsDiscarded) {
    RevisionHistory<TestState> history(state(0));
    (void)history.commit(state(1), "First edit");
    auto checkpointed = state(2);
    std::weak_ptr<const TestState> checkpointLifetime = checkpointed;
    (void)history.commit(checkpointed, "Checkpointed edit");
    history.markCheckpoint();
    checkpointed.reset();

    ASSERT_TRUE(history.undo().has_value());
    (void)history.commit(state(3), "Replacement branch");

    EXPECT_TRUE(history.isDirty());
    EXPECT_EQ(history.checkpointRevision(), RevisionId{ 3 });
    EXPECT_FALSE(checkpointLifetime.expired());
    EXPECT_EQ(history.checkpointState()->value, 2);

    history.markCheckpoint();
    EXPECT_TRUE(checkpointLifetime.expired());
    EXPECT_FALSE(history.isDirty());
}

TEST(RevisionHistoryTest, ReleasesDiscardedSnapshotsNotRetainedElsewhere) {
    RevisionHistory<TestState> history(state(0));
    (void)history.commit(state(1), "First edit");
    auto abandoned = state(2);
    std::weak_ptr<const TestState> lifetime = abandoned;
    (void)history.commit(abandoned, "Abandoned edit");
    abandoned.reset();

    ASSERT_TRUE(history.undo().has_value());
    (void)history.commit(state(3), "Replacement branch");

    EXPECT_TRUE(lifetime.expired());
}

TEST(RevisionHistoryTest, CompoundCandidateCreatesOneAtomicRevision) {
    RevisionHistory<TestState> history(state(0));
    const auto completedCandidate = state(30);

    const auto commit = history.commit(completedCandidate, "Apply compound repair");

    EXPECT_EQ(commit.revision, RevisionId{ 2 });
    ASSERT_TRUE(history.undo().has_value());
    EXPECT_EQ(history.currentRevision().id, RevisionId{ 1 });
    EXPECT_FALSE(history.canUndo());
    ASSERT_TRUE(history.redo().has_value());
    EXPECT_EQ(history.currentRevision().state->value, 30);
    EXPECT_FALSE(history.canRedo());
}

TEST(RevisionHistoryTest, SupportsMoveOnlyStateAndMovingTheHistory) {
    static_assert(!std::is_copy_constructible_v<RevisionHistory<MoveOnlyState>>);
    static_assert(std::is_move_constructible_v<RevisionHistory<MoveOnlyState>>);

    auto initial = std::make_shared<const MoveOnlyState>(1);
    RevisionHistory<MoveOnlyState> source(initial);
    auto edited = std::make_shared<const MoveOnlyState>(2);
    (void)source.commit(edited, "Move-only edit");

    auto history = std::move(source);

    EXPECT_EQ(*history.currentRevision().state->value, 2);
    ASSERT_TRUE(history.undo().has_value());
    EXPECT_EQ(*history.currentRevision().state->value, 1);
}

}  // namespace
}  // namespace salsa::core
