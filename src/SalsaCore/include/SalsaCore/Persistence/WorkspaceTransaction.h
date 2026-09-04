#pragma once

#include "SalsaCore/Foundation/Result.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {

struct WorkspaceArtifactMutation final {
    std::filesystem::path relativePath{};
    bool expectedPresent = false;
    std::optional<std::string> expectedSha256{};
    std::optional<std::vector<std::byte>> replacement{};
};

struct WorkspaceTransactionRequest final {
    std::string transactionId{};
    std::string workspaceId{};
    std::string operationType{};
    std::string planId{};
    std::vector<WorkspaceArtifactMutation> mutations{};
};

enum class WorkspaceTransactionStatus {
    Prepared,
    Committing,
    Verified,
    RolledBack,
    RecoveryBlocked,
};

struct WorkspaceTransactionResult final {
    std::string transactionId{};
    WorkspaceTransactionStatus status = WorkspaceTransactionStatus::RecoveryBlocked;
    std::vector<Diagnostic> diagnostics{};

    [[nodiscard]] bool succeeded() const noexcept {
        return status == WorkspaceTransactionStatus::Prepared
            || status == WorkspaceTransactionStatus::Verified
            || status == WorkspaceTransactionStatus::RolledBack;
    }
};

struct WorkspaceTransactionHooks final {
    // Tests use this to simulate process loss. Production callers leave it empty.
    std::function<bool(std::size_t completedMutations)> continueAfterMutation{};
};

class WorkspaceTransactionService final {
public:
    [[nodiscard]] static WorkspaceTransactionResult prepare(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& transactionsRelativeRoot,
        const WorkspaceTransactionRequest& request);
    [[nodiscard]] static WorkspaceTransactionResult commitPrepared(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& transactionsRelativeRoot,
        std::string_view transactionId,
        const WorkspaceTransactionHooks& hooks = {});
    [[nodiscard]] static std::vector<WorkspaceTransactionResult> recoverAll(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& transactionsRelativeRoot);
};

struct WorkspaceCleanupCandidate final {
    std::filesystem::path relativePath{};
    std::uintmax_t byteSize = 0;
    std::string reason{};
};

class WorkspaceArtifactCleanupService final {
public:
    [[nodiscard]] static Result<std::vector<WorkspaceCleanupCandidate>> assess(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& transactionsRelativeRoot);
    [[nodiscard]] static Result<void> remove(
        const std::filesystem::path& workspaceRoot,
        std::span<const WorkspaceCleanupCandidate> candidates);
};

}  // namespace salsa::core
