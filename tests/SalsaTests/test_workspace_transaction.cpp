#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <fstream>
#include <span>
#include <string>

namespace salsa::core {
namespace {

class TransactionTemporaryDirectory final {
public:
    TransactionTemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-transaction-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence_++));
        std::filesystem::create_directories(path_ / L"transactions");
        std::filesystem::create_directories(path_ / L"patches");
    }
    ~TransactionTemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    inline static std::uint64_t sequence_ = 0;
    std::filesystem::path path_{};
};

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text) {
    const auto raw = std::as_bytes(std::span{text.data(), text.size()});
    return {raw.begin(), raw.end()};
}

[[nodiscard]] std::string digest(const std::vector<std::byte>& value) {
    return sha256(value).value().toHex();
}

[[nodiscard]] std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

[[nodiscard]] WorkspaceArtifactMutation replaceMutation(
    const std::filesystem::path& path, const std::string_view before,
    const std::string_view after) {
    auto beforeBytes = bytes(before);
    return {path, true, digest(beforeBytes), bytes(after)};
}

TEST(WorkspaceTransactionTest, PreparesAndCommitsReplacementCreationAndDeletion) {
    TransactionTemporaryDirectory temporary;
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("old-a")));
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/c.json", bytes("old-c")));
    WorkspaceTransactionRequest request{"tx-one", "workspace", "patch-rebase", "plan", {
        replaceMutation(L"patches/a.json", "old-a", "new-a"),
        {L"patches/b.json", false, std::nullopt, bytes("new-b")},
        {L"patches/c.json", true, digest(bytes("old-c")), std::nullopt},
    }};
    const auto prepared = WorkspaceTransactionService::prepare(
        temporary.path(), L"transactions", request);
    ASSERT_EQ(prepared.status, WorkspaceTransactionStatus::Prepared);
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "old-a");

    const auto committed = WorkspaceTransactionService::commitPrepared(
        temporary.path(), L"transactions", request.transactionId);
    ASSERT_EQ(committed.status, WorkspaceTransactionStatus::Verified);
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "new-a");
    EXPECT_EQ(read(temporary.path() / L"patches/b.json"), "new-b");
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / L"patches/c.json"));
}

TEST(WorkspaceTransactionTest, RecoversAnInterruptedCommitByRollingForward) {
    TransactionTemporaryDirectory temporary;
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("old-a")));
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/b.json", bytes("old-b")));
    WorkspaceTransactionRequest request{"tx-interrupted", "workspace", "patch-rebase", "plan", {
        replaceMutation(L"patches/a.json", "old-a", "new-a"),
        replaceMutation(L"patches/b.json", "old-b", "new-b"),
    }};
    ASSERT_EQ(WorkspaceTransactionService::prepare(
        temporary.path(), L"transactions", request).status,
        WorkspaceTransactionStatus::Prepared);
    WorkspaceTransactionHooks hooks;
    hooks.continueAfterMutation = [](const std::size_t completed) {
        return completed != 1u;
    };
    EXPECT_EQ(WorkspaceTransactionService::commitPrepared(
        temporary.path(), L"transactions", request.transactionId, hooks).status,
        WorkspaceTransactionStatus::RecoveryBlocked);
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "new-a");
    EXPECT_EQ(read(temporary.path() / L"patches/b.json"), "old-b");

    const auto recovered = WorkspaceTransactionService::recoverAll(
        temporary.path(), L"transactions");
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered.front().status, WorkspaceTransactionStatus::Verified);
    EXPECT_EQ(read(temporary.path() / L"patches/b.json"), "new-b");
}

TEST(WorkspaceTransactionTest, RollsBackWhenAnInterruptedCommitCannotRollForward) {
    TransactionTemporaryDirectory temporary;
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("old-a")));
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/b.json", bytes("old-b")));
    WorkspaceTransactionRequest request{"tx-rollback", "workspace", "patch-rebase", "plan", {
        replaceMutation(L"patches/a.json", "old-a", "new-a"),
        replaceMutation(L"patches/b.json", "old-b", "new-b"),
    }};
    ASSERT_EQ(WorkspaceTransactionService::prepare(
        temporary.path(), L"transactions", request).status,
        WorkspaceTransactionStatus::Prepared);
    WorkspaceTransactionHooks hooks;
    hooks.continueAfterMutation = [](const std::size_t completed) {
        return completed != 1u;
    };
    ASSERT_EQ(WorkspaceTransactionService::commitPrepared(
        temporary.path(), L"transactions", request.transactionId, hooks).status,
        WorkspaceTransactionStatus::RecoveryBlocked);
    ASSERT_TRUE(std::filesystem::remove(
        temporary.path() / L"transactions/tx-rollback/staged/1.bin"));

    const auto recovered = WorkspaceTransactionService::recoverAll(
        temporary.path(), L"transactions");
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered.front().status, WorkspaceTransactionStatus::RolledBack);
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "old-a");
    EXPECT_EQ(read(temporary.path() / L"patches/b.json"), "old-b");
}

TEST(WorkspaceTransactionTest, PreservesUnknownExternalStateAndCleansOnlyVerifiedJournals) {
    TransactionTemporaryDirectory temporary;
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("old-a")));
    WorkspaceTransactionRequest request{"tx-unknown", "workspace", "patch-rebase", "plan", {
        replaceMutation(L"patches/a.json", "old-a", "new-a"),
    }};
    ASSERT_EQ(WorkspaceTransactionService::prepare(
        temporary.path(), L"transactions", request).status,
        WorkspaceTransactionStatus::Prepared);
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("external")));
    const auto blocked = WorkspaceTransactionService::recoverAll(
        temporary.path(), L"transactions");
    ASSERT_EQ(blocked.size(), 1u);
    EXPECT_EQ(blocked.front().status, WorkspaceTransactionStatus::RecoveryBlocked);
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "external");
    EXPECT_TRUE(WorkspaceArtifactCleanupService::assess(
        temporary.path(), L"transactions").value().empty());
}

TEST(WorkspaceTransactionTest, PreparedRecoveryDoesNotMistakeAnExternalAfterValueForCommit) {
    TransactionTemporaryDirectory temporary;
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("old-a")));
    WorkspaceTransactionRequest request{"tx-prepared-external", "workspace",
        "patch-rebase", "plan", {
            replaceMutation(L"patches/a.json", "old-a", "new-a"),
        }};
    ASSERT_EQ(WorkspaceTransactionService::prepare(
        temporary.path(), L"transactions", request).status,
        WorkspaceTransactionStatus::Prepared);
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("new-a")));
    const auto recovered = WorkspaceTransactionService::recoverAll(
        temporary.path(), L"transactions");
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered.front().status,
        WorkspaceTransactionStatus::RecoveryBlocked);
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "new-a");
}

TEST(WorkspaceTransactionTest, ExplicitCleanupRemovesVerifiedRollbackEvidence) {
    TransactionTemporaryDirectory temporary;
    ASSERT_TRUE(replaceFileAtomically(
        temporary.path() / L"patches/a.json", bytes("old-a")));
    WorkspaceTransactionRequest request{"tx-clean", "workspace", "patch-rebase", "plan", {
        replaceMutation(L"patches/a.json", "old-a", "new-a"),
    }};
    ASSERT_EQ(WorkspaceTransactionService::prepare(
        temporary.path(), L"transactions", request).status,
        WorkspaceTransactionStatus::Prepared);
    ASSERT_EQ(WorkspaceTransactionService::commitPrepared(
        temporary.path(), L"transactions", request.transactionId).status,
        WorkspaceTransactionStatus::Verified);
    const auto candidates = WorkspaceArtifactCleanupService::assess(
        temporary.path(), L"transactions");
    ASSERT_TRUE(candidates);
    ASSERT_EQ(candidates.value().size(), 1u);
    ASSERT_TRUE(WorkspaceArtifactCleanupService::remove(
        temporary.path(), candidates.value()));
    EXPECT_FALSE(std::filesystem::exists(
        temporary.path() / L"transactions/tx-clean"));
    EXPECT_EQ(read(temporary.path() / L"patches/a.json"), "new-a");
}

}  // namespace
}  // namespace salsa::core
