#include "SalsaCore/Persistence/WorkspaceTransaction.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::string_view FormatId = "jahorta.salsa.workspace-transaction";
constexpr std::uint32_t SchemaVersion = 1;
constexpr std::wstring_view ManifestName = L"transaction.json";

struct StoredMutation final {
    std::filesystem::path relativePath{};
    bool beforePresent = false;
    std::optional<std::string> beforeSha256{};
    bool afterPresent = false;
    std::optional<std::string> afterSha256{};
    std::filesystem::path stagedPath{};
    std::filesystem::path backupPath{};
};

struct StoredTransaction final {
    WorkspaceTransactionRequest request{};
    WorkspaceTransactionStatus status = WorkspaceTransactionStatus::Prepared;
    std::size_t completedMutations = 0;
    std::vector<StoredMutation> mutations{};
};

[[nodiscard]] Diagnostic transactionError(std::string message,
    const std::filesystem::path& path,
    const DiagnosticCode code = DiagnosticCode::InvalidWorkspaceTransaction) {
    return {DiagnosticSeverity::Error, code, std::move(message), path};
}

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

[[nodiscard]] bool validId(const std::string_view value) {
    return !value.empty() && value.size() <= 128u
        && std::ranges::all_of(value, [](const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9') || character == '-';
        });
}

[[nodiscard]] bool validSha256(const std::string_view value) {
    return value.size() == 64u
        && std::ranges::all_of(value, [](const char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
}

[[nodiscard]] bool safeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()
        || path.lexically_normal() != path) return false;
    return std::ranges::all_of(path, [](const auto& component) {
        return component != L"." && component != L".." && !component.empty();
    });
}

[[nodiscard]] Result<std::vector<std::byte>> readBytes(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return Result<std::vector<std::byte>>::failure(transactionError(
        "A workspace transaction artifact could not be opened.", path,
        DiagnosticCode::PersistenceReadFailed));
    const auto end = stream.tellg();
    if (end < 0) return Result<std::vector<std::byte>>::failure(transactionError(
        "A workspace transaction artifact has an invalid size.", path,
        DiagnosticCode::PersistenceReadFailed));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), end))
        return Result<std::vector<std::byte>>::failure(transactionError(
            "A workspace transaction artifact could not be read completely.", path,
            DiagnosticCode::PersistenceReadFailed));
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::string> digest(
    const std::span<const std::byte> bytes) {
    const auto value = sha256(bytes);
    if (!value) return Result<std::string>::failure(value.diagnostics());
    return Result<std::string>::success(value.value().toHex());
}

[[nodiscard]] Result<std::optional<std::string>> fileDigest(
    const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return Result<std::optional<std::string>>::failure(transactionError(
        "A workspace transaction destination could not be inspected: " + error.message(),
        path, DiagnosticCode::PersistenceReadFailed));
    if (!exists) return Result<std::optional<std::string>>::success(std::nullopt);
    const auto bytes = readBytes(path);
    if (!bytes) return Result<std::optional<std::string>>::failure(bytes.diagnostics());
    const auto value = digest(bytes.value());
    if (!value) return Result<std::optional<std::string>>::failure(value.diagnostics());
    return Result<std::optional<std::string>>::success(value.value());
}

[[nodiscard]] std::string statusName(const WorkspaceTransactionStatus status) {
    switch (status) {
    case WorkspaceTransactionStatus::Prepared: return "prepared";
    case WorkspaceTransactionStatus::Committing: return "committing";
    case WorkspaceTransactionStatus::Verified: return "verified";
    case WorkspaceTransactionStatus::RolledBack: return "rolled-back";
    case WorkspaceTransactionStatus::RecoveryBlocked: return "recovery-blocked";
    }
    return "recovery-blocked";
}

[[nodiscard]] std::optional<WorkspaceTransactionStatus> parseStatus(
    const std::string_view value) {
    if (value == "prepared") return WorkspaceTransactionStatus::Prepared;
    if (value == "committing") return WorkspaceTransactionStatus::Committing;
    if (value == "verified") return WorkspaceTransactionStatus::Verified;
    if (value == "rolled-back") return WorkspaceTransactionStatus::RolledBack;
    if (value == "recovery-blocked") return WorkspaceTransactionStatus::RecoveryBlocked;
    return std::nullopt;
}

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] Result<std::vector<std::byte>> encode(const StoredTransaction& value) {
    try {
        Json root{{"formatId", FormatId}, {"schemaVersion", SchemaVersion},
            {"transactionId", value.request.transactionId},
            {"workspaceId", value.request.workspaceId},
            {"operationType", value.request.operationType},
            {"planId", value.request.planId},
            {"status", statusName(value.status)},
            {"completedMutations", value.completedMutations},
            {"mutations", Json::array()}};
        for (const auto& mutation : value.mutations)
            root["mutations"].push_back(Json{
                {"relativePath", pathUtf8(mutation.relativePath)},
                {"beforePresent", mutation.beforePresent},
                {"beforeSha256", mutation.beforeSha256
                    ? Json(*mutation.beforeSha256) : Json(nullptr)},
                {"afterPresent", mutation.afterPresent},
                {"afterSha256", mutation.afterSha256
                    ? Json(*mutation.afterSha256) : Json(nullptr)},
                {"stagedPath", pathUtf8(mutation.stagedPath)},
                {"backupPath", pathUtf8(mutation.backupPath)}});
        auto text = root.dump(2);
        text.push_back('\n');
        return Result<std::vector<std::byte>>::success(bytesOf(text));
    } catch (const std::exception& error) {
        return Result<std::vector<std::byte>>::failure(transactionError(
            std::string("A workspace transaction could not be encoded: ") + error.what(), {}));
    }
}

[[nodiscard]] bool exactKeys(const Json& value,
    const std::initializer_list<std::string_view> keys) {
    return value.is_object() && value.size() == keys.size()
        && std::ranges::all_of(keys, [&](const auto key) {
            return value.contains(std::string(key));
        });
}

[[nodiscard]] Result<StoredTransaction> decode(const std::filesystem::path& path) {
    const auto bytes = readBytes(path);
    if (!bytes) return Result<StoredTransaction>::failure(bytes.diagnostics());
    try {
        const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
            bytes.value().size());
        const auto root = Json::parse(text);
        if (!exactKeys(root, {"formatId", "schemaVersion", "transactionId",
                "workspaceId", "operationType", "planId", "status",
                "completedMutations", "mutations"})
            || root.at("formatId").get<std::string>() != FormatId
            || root.at("schemaVersion").get<std::uint32_t>() != SchemaVersion)
            throw std::runtime_error("the transaction header is invalid");
        StoredTransaction value;
        value.request.transactionId = root.at("transactionId").get<std::string>();
        value.request.workspaceId = root.at("workspaceId").get<std::string>();
        value.request.operationType = root.at("operationType").get<std::string>();
        value.request.planId = root.at("planId").get<std::string>();
        const auto status = parseStatus(root.at("status").get<std::string>());
        if (!status || !validId(value.request.transactionId))
            throw std::runtime_error("the transaction status or ID is invalid");
        value.status = *status;
        value.completedMutations = root.at("completedMutations").get<std::size_t>();
        for (const auto& item : root.at("mutations")) {
            if (!exactKeys(item, {"relativePath", "beforePresent", "beforeSha256",
                    "afterPresent", "afterSha256", "stagedPath", "backupPath"}))
                throw std::runtime_error("a transaction mutation is malformed");
            StoredMutation mutation;
            mutation.relativePath = pathFromUtf8(item.at("relativePath").get<std::string>());
            mutation.beforePresent = item.at("beforePresent").get<bool>();
            if (!item.at("beforeSha256").is_null())
                mutation.beforeSha256 = item.at("beforeSha256").get<std::string>();
            mutation.afterPresent = item.at("afterPresent").get<bool>();
            if (!item.at("afterSha256").is_null())
                mutation.afterSha256 = item.at("afterSha256").get<std::string>();
            mutation.stagedPath = pathFromUtf8(item.at("stagedPath").get<std::string>());
            mutation.backupPath = pathFromUtf8(item.at("backupPath").get<std::string>());
            if (!safeRelative(mutation.relativePath)
                || !safeRelative(mutation.stagedPath)
                || !safeRelative(mutation.backupPath)
                || mutation.beforePresent != mutation.beforeSha256.has_value()
                || mutation.afterPresent != mutation.afterSha256.has_value()
                || (mutation.beforeSha256 && !validSha256(*mutation.beforeSha256))
                || (mutation.afterSha256 && !validSha256(*mutation.afterSha256)))
                throw std::runtime_error("a transaction mutation has invalid paths or hashes");
            value.mutations.push_back(std::move(mutation));
        }
        if (value.mutations.empty() || value.completedMutations > value.mutations.size())
            throw std::runtime_error("the transaction mutation count is invalid");
        return Result<StoredTransaction>::success(std::move(value));
    } catch (const std::exception& error) {
        return Result<StoredTransaction>::failure(transactionError(
            std::string("The workspace transaction is malformed: ") + error.what(), path));
    }
}

[[nodiscard]] Result<void> writeManifest(
    const std::filesystem::path& path, const StoredTransaction& value) {
    const auto encoded = encode(value);
    if (!encoded) return Result<void>::failure(encoded.diagnostics());
    return replaceFileAtomically(path, encoded.value());
}

[[nodiscard]] WorkspaceTransactionResult blocked(const std::string& id,
    std::string message, const std::filesystem::path& path) {
    return {id, WorkspaceTransactionStatus::RecoveryBlocked,
        {transactionError(std::move(message), path,
            DiagnosticCode::WorkspaceRecoveryRequired)}};
}

[[nodiscard]] WorkspaceTransactionResult finishCommit(
    const std::filesystem::path& root, const std::filesystem::path& transactionRoot,
    StoredTransaction transaction, const WorkspaceTransactionHooks& hooks) {
    const auto manifest = transactionRoot / ManifestName;
    transaction.status = WorkspaceTransactionStatus::Committing;
    if (const auto written = writeManifest(manifest, transaction); !written)
        return {transaction.request.transactionId,
            WorkspaceTransactionStatus::RecoveryBlocked, written.diagnostics()};
    for (std::size_t index = transaction.completedMutations;
        index < transaction.mutations.size(); ++index) {
        const auto& mutation = transaction.mutations[index];
        const auto destination = root / mutation.relativePath;
        if (mutation.afterPresent) {
            const auto staged = readBytes(transactionRoot / mutation.stagedPath);
            if (!staged) return {transaction.request.transactionId,
                WorkspaceTransactionStatus::RecoveryBlocked, staged.diagnostics()};
            const auto stagedDigest = digest(staged.value());
            if (!stagedDigest || stagedDigest.value() != mutation.afterSha256)
                return blocked(transaction.request.transactionId,
                    "A staged workspace transaction artifact failed integrity verification.",
                    transactionRoot / mutation.stagedPath);
            if (const auto replaced = replaceFileAtomically(destination, staged.value()); !replaced)
                return {transaction.request.transactionId,
                    WorkspaceTransactionStatus::RecoveryBlocked, replaced.diagnostics()};
        } else {
            std::error_code error;
            std::filesystem::remove(destination, error);
            if (error) return blocked(transaction.request.transactionId,
                "A workspace transaction deletion failed: " + error.message(), destination);
        }
        transaction.completedMutations = index + 1u;
        if (const auto written = writeManifest(manifest, transaction); !written)
            return {transaction.request.transactionId,
                WorkspaceTransactionStatus::RecoveryBlocked, written.diagnostics()};
        if (hooks.continueAfterMutation
            && !hooks.continueAfterMutation(transaction.completedMutations))
            return blocked(transaction.request.transactionId,
                "The transaction was intentionally interrupted for fault injection.", manifest);
    }
    for (const auto& mutation : transaction.mutations) {
        const auto current = fileDigest(root / mutation.relativePath);
        if (!current || current.value() != mutation.afterSha256)
            return blocked(transaction.request.transactionId,
                "A committed workspace artifact failed reopen verification.",
                root / mutation.relativePath);
    }
    transaction.status = WorkspaceTransactionStatus::Verified;
    const auto written = writeManifest(manifest, transaction);
    return written
        ? WorkspaceTransactionResult{transaction.request.transactionId,
            WorkspaceTransactionStatus::Verified, {}}
        : WorkspaceTransactionResult{transaction.request.transactionId,
            WorkspaceTransactionStatus::RecoveryBlocked, written.diagnostics()};
}

[[nodiscard]] bool canRollForward(const std::filesystem::path& transactionRoot,
    const StoredTransaction& transaction) {
    for (const auto& mutation : transaction.mutations) {
        if (!mutation.afterPresent) continue;
        const auto staged = fileDigest(transactionRoot / mutation.stagedPath);
        if (!staged || staged.value() != mutation.afterSha256) return false;
    }
    return true;
}

[[nodiscard]] WorkspaceTransactionResult rollBack(
    const std::filesystem::path& root, const std::filesystem::path& transactionRoot,
    StoredTransaction transaction) {
    const auto manifest = transactionRoot / ManifestName;
    for (const auto& mutation : transaction.mutations) {
        const auto destination = root / mutation.relativePath;
        if (mutation.beforePresent) {
            const auto backup = readBytes(transactionRoot / mutation.backupPath);
            if (!backup) return {transaction.request.transactionId,
                WorkspaceTransactionStatus::RecoveryBlocked, backup.diagnostics()};
            const auto backupDigest = digest(backup.value());
            if (!backupDigest || backupDigest.value() != mutation.beforeSha256)
                return blocked(transaction.request.transactionId,
                    "A workspace transaction backup failed integrity verification.",
                    transactionRoot / mutation.backupPath);
            if (const auto restored = replaceFileAtomically(destination, backup.value());
                !restored)
                return {transaction.request.transactionId,
                    WorkspaceTransactionStatus::RecoveryBlocked,
                    restored.diagnostics()};
        } else {
            std::error_code error;
            std::filesystem::remove(destination, error);
            if (error) return blocked(transaction.request.transactionId,
                "A workspace transaction rollback deletion failed: " + error.message(),
                destination);
        }
    }
    for (const auto& mutation : transaction.mutations) {
        const auto current = fileDigest(root / mutation.relativePath);
        if (!current || current.value() != mutation.beforeSha256)
            return blocked(transaction.request.transactionId,
                "A rolled-back workspace artifact failed reopen verification.",
                root / mutation.relativePath);
    }
    transaction.completedMutations = 0;
    transaction.status = WorkspaceTransactionStatus::RolledBack;
    const auto written = writeManifest(manifest, transaction);
    return written
        ? WorkspaceTransactionResult{transaction.request.transactionId,
            WorkspaceTransactionStatus::RolledBack, {}}
        : WorkspaceTransactionResult{transaction.request.transactionId,
            WorkspaceTransactionStatus::RecoveryBlocked, written.diagnostics()};
}

}  // namespace

WorkspaceTransactionResult WorkspaceTransactionService::prepare(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& transactionsRelativeRoot,
    const WorkspaceTransactionRequest& request) {
    if (!validId(request.transactionId) || request.workspaceId.empty()
        || request.operationType.empty() || request.planId.empty()
        || request.mutations.empty() || !safeRelative(transactionsRelativeRoot))
        return blocked(request.transactionId,
            "A workspace transaction request is incomplete or malformed.", workspaceRoot);
    std::set<std::filesystem::path> paths;
    for (const auto& mutation : request.mutations)
        if (!safeRelative(mutation.relativePath)
            || !paths.insert(mutation.relativePath).second
            || mutation.expectedPresent != mutation.expectedSha256.has_value())
            return blocked(request.transactionId,
                "A workspace transaction mutation is malformed or duplicated.",
                workspaceRoot / mutation.relativePath);

    const auto transactionRoot = workspaceRoot / transactionsRelativeRoot
        / request.transactionId;
    std::error_code error;
    if (std::filesystem::exists(transactionRoot, error) || error)
        return blocked(request.transactionId,
            "A workspace transaction with this ID already exists.", transactionRoot);
    if (!std::filesystem::create_directories(transactionRoot / L"staged", error)
        || error || !std::filesystem::create_directories(transactionRoot / L"backup", error)
        || error) {
        std::error_code cleanupError;
        std::filesystem::remove_all(transactionRoot, cleanupError);
        return blocked(request.transactionId,
            "The workspace transaction staging directories could not be created.",
            transactionRoot);
    }
    const auto preparationFailure = [&](std::vector<Diagnostic> diagnostics) {
        std::error_code cleanupError;
        std::filesystem::remove_all(transactionRoot, cleanupError);
        if (cleanupError) diagnostics.push_back(transactionError(
            "Incomplete transaction staging could not be removed: "
                + cleanupError.message(), transactionRoot,
            DiagnosticCode::WorkspaceCleanupFailed));
        return WorkspaceTransactionResult{request.transactionId,
            WorkspaceTransactionStatus::RecoveryBlocked, std::move(diagnostics)};
    };

    StoredTransaction stored;
    stored.request = request;
    for (std::size_t index = 0; index < request.mutations.size(); ++index) {
        const auto& mutation = request.mutations[index];
        const auto destination = workspaceRoot / mutation.relativePath;
        const auto current = fileDigest(destination);
        if (!current || current.value() != mutation.expectedSha256) {
            auto failure = blocked(request.transactionId,
                "A workspace artifact changed before the transaction was prepared.",
                destination);
            return preparationFailure(std::move(failure.diagnostics));
        }
        StoredMutation item;
        item.relativePath = mutation.relativePath;
        item.beforePresent = mutation.expectedPresent;
        item.beforeSha256 = mutation.expectedSha256;
        item.afterPresent = mutation.replacement.has_value();
        item.stagedPath = std::filesystem::path(L"staged")
            / (std::to_wstring(index) + L".bin");
        item.backupPath = std::filesystem::path(L"backup")
            / (std::to_wstring(index) + L".bin");
        if (mutation.replacement) {
            const auto after = digest(*mutation.replacement);
            if (!after) return preparationFailure(after.diagnostics());
            item.afterSha256 = after.value();
            if (const auto staged = replaceFileAtomically(
                    transactionRoot / item.stagedPath, *mutation.replacement); !staged)
                return preparationFailure(staged.diagnostics());
        }
        if (mutation.expectedPresent) {
            const auto before = readBytes(destination);
            if (!before) return preparationFailure(before.diagnostics());
            if (const auto backup = replaceFileAtomically(
                    transactionRoot / item.backupPath, before.value()); !backup)
                return preparationFailure(backup.diagnostics());
        }
        stored.mutations.push_back(std::move(item));
    }
    const auto written = writeManifest(transactionRoot / ManifestName, stored);
    return written
        ? WorkspaceTransactionResult{request.transactionId,
            WorkspaceTransactionStatus::Prepared, {}}
        : preparationFailure(written.diagnostics());
}

WorkspaceTransactionResult WorkspaceTransactionService::commitPrepared(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& transactionsRelativeRoot,
    const std::string_view transactionId,
    const WorkspaceTransactionHooks& hooks) {
    if (!validId(transactionId) || !safeRelative(transactionsRelativeRoot))
        return blocked(std::string(transactionId),
            "The workspace transaction ID or root is invalid.", workspaceRoot);
    const auto transactionRoot = workspaceRoot / transactionsRelativeRoot
        / std::filesystem::path(transactionId);
    auto decoded = decode(transactionRoot / ManifestName);
    if (!decoded) return {std::string(transactionId),
        WorkspaceTransactionStatus::RecoveryBlocked, decoded.diagnostics()};
    if (decoded.value().status == WorkspaceTransactionStatus::Verified
        || decoded.value().status == WorkspaceTransactionStatus::RolledBack)
        return {std::string(transactionId), decoded.value().status, {}};
    return finishCommit(workspaceRoot, transactionRoot,
        std::move(decoded).takeValue(), hooks);
}

std::vector<WorkspaceTransactionResult> WorkspaceTransactionService::recoverAll(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& transactionsRelativeRoot) {
    std::vector<WorkspaceTransactionResult> results;
    const auto root = workspaceRoot / transactionsRelativeRoot;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) return results;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) break;
        if (!entry.is_directory(error) || error) continue;
        auto decoded = decode(entry.path() / ManifestName);
        if (!decoded) {
            results.push_back({pathUtf8(entry.path().filename()),
                WorkspaceTransactionStatus::RecoveryBlocked, decoded.diagnostics()});
            continue;
        }
        auto transaction = std::move(decoded).takeValue();
        if (transaction.status == WorkspaceTransactionStatus::Verified
            || transaction.status == WorkspaceTransactionStatus::RolledBack) {
            results.push_back({transaction.request.transactionId,
                transaction.status, {}});
            continue;
        }
        bool unknown = false;
        for (const auto& mutation : transaction.mutations) {
            const auto current = fileDigest(workspaceRoot / mutation.relativePath);
            const bool recognized = transaction.status == WorkspaceTransactionStatus::Prepared
                ? current && current.value() == mutation.beforeSha256
                : current && (current.value() == mutation.beforeSha256
                    || current.value() == mutation.afterSha256);
            if (!recognized) {
                unknown = true;
                break;
            }
        }
        if (unknown) {
            results.push_back(blocked(transaction.request.transactionId,
                "Recovery found an artifact that matches neither recorded state.",
                entry.path() / ManifestName));
            continue;
        }
        if (transaction.status == WorkspaceTransactionStatus::Prepared) {
            transaction.status = WorkspaceTransactionStatus::RolledBack;
            const auto written = writeManifest(entry.path() / ManifestName, transaction);
            results.push_back(written
                ? WorkspaceTransactionResult{transaction.request.transactionId,
                    WorkspaceTransactionStatus::RolledBack, {}}
                : WorkspaceTransactionResult{transaction.request.transactionId,
                    WorkspaceTransactionStatus::RecoveryBlocked, written.diagnostics()});
            continue;
        }
        if (canRollForward(entry.path(), transaction)) {
            transaction.completedMutations = 0;
            results.push_back(finishCommit(workspaceRoot, entry.path(),
                std::move(transaction), {}));
        } else {
            results.push_back(rollBack(workspaceRoot, entry.path(),
                std::move(transaction)));
        }
    }
    if (error) results.push_back(blocked("transaction-scan",
        "Workspace transaction recovery could not enumerate its journal: "
            + error.message(), root));
    return results;
}

Result<std::vector<WorkspaceCleanupCandidate>> WorkspaceArtifactCleanupService::assess(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& transactionsRelativeRoot) {
    std::vector<WorkspaceCleanupCandidate> result;
    const auto root = workspaceRoot / transactionsRelativeRoot;
    std::error_code error;
    if (!std::filesystem::exists(root, error))
        return Result<std::vector<WorkspaceCleanupCandidate>>::success({});
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) break;
        if (!entry.is_directory(error) || error) continue;
        const auto decoded = decode(entry.path() / ManifestName);
        if (!decoded) continue;
        if (decoded.value().status != WorkspaceTransactionStatus::Verified
            && decoded.value().status != WorkspaceTransactionStatus::RolledBack) continue;
        std::uintmax_t bytes = 0;
        for (const auto& file : std::filesystem::recursive_directory_iterator(
                entry.path(), error)) {
            if (error) break;
            if (file.is_regular_file(error) && !error) bytes += file.file_size(error);
        }
        if (error) break;
        result.push_back({std::filesystem::relative(entry.path(), workspaceRoot, error),
            bytes, "Verified transaction staging and rollback evidence"});
        if (error) break;
    }
    if (error) return Result<std::vector<WorkspaceCleanupCandidate>>::failure(
        transactionError("Workspace cleanup assessment failed: " + error.message(),
            root, DiagnosticCode::WorkspaceCleanupFailed));
    std::ranges::sort(result, {}, [](const auto& item) {
        return item.relativePath.generic_wstring();
    });
    return Result<std::vector<WorkspaceCleanupCandidate>>::success(std::move(result));
}

Result<void> WorkspaceArtifactCleanupService::remove(
    const std::filesystem::path& workspaceRoot,
    const std::span<const WorkspaceCleanupCandidate> candidates) {
    for (const auto& candidate : candidates) {
        if (!safeRelative(candidate.relativePath)) return Result<void>::failure(
            transactionError("A workspace cleanup target is not safely contained.",
                candidate.relativePath, DiagnosticCode::WorkspaceCleanupFailed));
        const auto path = workspaceRoot / candidate.relativePath;
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) return Result<void>::failure(transactionError(
            "A workspace cleanup target could not be removed: " + error.message(),
            path, DiagnosticCode::WorkspaceCleanupFailed));
    }
    return Result<void>::success();
}

}  // namespace salsa::core
