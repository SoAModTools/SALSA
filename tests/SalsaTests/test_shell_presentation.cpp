#include "SalsaCore/Application/ShellPresentation.h"

#include <gtest/gtest.h>

namespace salsa::core {

TEST(ShellPresentationTest, ShellActionsExplainTheirNearestPrerequisite) {
    ShellPresentationState state;
    EXPECT_EQ(shellActionBlockReason(ShellAction::RefreshDataset, state),
        ShellActionBlockReason::DatasetRequired);
    EXPECT_EQ(shellActionBlockReason(ShellAction::EditWorkspaceAppearance, state),
        ShellActionBlockReason::WorkspaceRequired);
    EXPECT_EQ(shellActionBlockReason(ShellAction::OpenRecentDataset, state),
        ShellActionBlockReason::NoRecentDatasets);

    state.datasetLoaded = true;
    state.workspaceAssociated = true;
    state.documentsOpen = true;
    EXPECT_EQ(shellActionBlockReason(ShellAction::AssociateWorkspace, state),
        ShellActionBlockReason::DocumentsMustClose);
    EXPECT_EQ(shellActionBlockReason(ShellAction::PromoteLegacyMetadata, state),
        ShellActionBlockReason::DocumentsMustClose);
}

TEST(ShellPresentationTest, ActiveOperationHasBlockerPrecedence) {
    ShellPresentationState state;
    state.operationActive = true;
    EXPECT_EQ(shellActionBlockReason(ShellAction::OpenRecentDataset, state),
        ShellActionBlockReason::OperationActive);
    EXPECT_EQ(shellActionBlockReason(ShellAction::AssociateWorkspace, state),
        ShellActionBlockReason::OperationActive);
}

TEST(ShellPresentationTest, ReadyShellActionsAreAvailable) {
    ShellPresentationState state;
    state.datasetLoaded = true;
    state.workspaceAssociated = true;
    state.recentDatasetsAvailable = true;
    EXPECT_EQ(shellActionBlockReason(ShellAction::OpenDataset, state),
        ShellActionBlockReason::None);
    EXPECT_EQ(shellActionBlockReason(ShellAction::RebasePatches, state),
        ShellActionBlockReason::None);
    EXPECT_EQ(shellActionBlockReason(ShellAction::OpenRecentDataset, state),
        ShellActionBlockReason::None);
}

TEST(ShellPresentationTest, WindowTitleReflectsDocumentDatasetAndModifiedState) {
    EXPECT_EQ(composeShellWindowTitle("SALSA"), "SALSA");
    EXPECT_EQ(composeShellWindowTitle("SALSA", "dataset"), "dataset — SALSA");
    EXPECT_EQ(composeShellWindowTitle("SALSA", "dataset", "a001.sct", true),
        "*a001.sct — dataset — SALSA");
}

TEST(ShellPresentationTest, CleanRefreshDismissesWithoutSummary) {
    EXPECT_EQ(datasetOperationPresentation(false, true, false, false),
        DatasetOperationPresentation::Dismiss);
}

TEST(ShellPresentationTest, CleanOpenWaitsForRestorationThenDismisses) {
    EXPECT_EQ(datasetOperationPresentation(true, true, false, false),
        DatasetOperationPresentation::AwaitRestoration);
    EXPECT_EQ(restoredDatasetPresentation(false, false),
        DatasetOperationPresentation::Dismiss);
}

TEST(ShellPresentationTest, AttentionConditionsRetainSummary) {
    EXPECT_EQ(datasetOperationPresentation(false, false, false, false),
        DatasetOperationPresentation::ShowSummary);
    EXPECT_EQ(datasetOperationPresentation(false, false, true, false),
        DatasetOperationPresentation::ShowSummary);
    EXPECT_EQ(datasetOperationPresentation(false, true, false, true),
        DatasetOperationPresentation::ShowSummary);
    EXPECT_EQ(restoredDatasetPresentation(false, true),
        DatasetOperationPresentation::ShowSummary);
    EXPECT_EQ(restoredDatasetPresentation(true, false),
        DatasetOperationPresentation::ShowSummary);
}

}  // namespace salsa::core
