#include "SalsaCore/Legacy/LegacyMetadataPromotion.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "SalsaCore/Foundation/Hashing.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <ranges>
#include <set>
#include <span>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] Diagnostic error(std::string message,
    const std::filesystem::path& path = {}) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidWorkspaceTransaction,
        std::move(message), path.empty() ? std::nullopt : std::optional{path}};
}

[[nodiscard]] Result<std::vector<std::byte>> readBytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Result<std::vector<std::byte>>::failure(error(
        "The legacy import-state ledger could not be opened.", path));
    const auto end = input.tellg();
    if (end < 0) return Result<std::vector<std::byte>>::failure(error(
        "The legacy import-state ledger has an invalid size.", path));
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!result.empty() && !input.read(reinterpret_cast<char*>(result.data()), end))
        return Result<std::vector<std::byte>>::failure(error(
            "The legacy import-state ledger could not be read completely.", path));
    return Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] Result<Json> readState(const std::filesystem::path& path,
    std::vector<std::byte>* encoded = nullptr) {
    auto bytes = readBytes(path);
    if (!bytes) return Result<Json>::failure(bytes.diagnostics());
    try {
        auto json = Json::parse(reinterpret_cast<const char*>(bytes.value().data()),
            reinterpret_cast<const char*>(bytes.value().data() + bytes.value().size()));
        if (!json.is_object()
            || json.value("formatId", "") != "jahorta.salsa.legacy-import-state"
            || json.value("schemaVersion", 0u) != 1u || !json.contains("metadata")
            || !json.at("metadata").is_array())
            return Result<Json>::failure(error(
                "The legacy import-state ledger is malformed or unsupported.", path));
        if (encoded != nullptr) *encoded = std::move(bytes).takeValue();
        return Result<Json>::success(std::move(json));
    } catch (const std::exception&) {
        return Result<Json>::failure(error(
            "The legacy import-state ledger is malformed.", path));
    }
}

[[nodiscard]] std::optional<LegacyMetadataDisposition> parseDisposition(
    const std::string_view value) {
    if (value == "applied") return LegacyMetadataDisposition::Applied;
    if (value == "pending") return LegacyMetadataDisposition::Pending;
    if (value == "recomputed") return LegacyMetadataDisposition::Recomputed;
    if (value == "contract") return LegacyMetadataDisposition::Contract;
    if (value == "dropped-by-user") return LegacyMetadataDisposition::DroppedByUser;
    if (value == "blocked") return LegacyMetadataDisposition::Blocked;
    if (value == "unsupported") return LegacyMetadataDisposition::Unsupported;
    if (value == "invalid") return LegacyMetadataDisposition::Invalid;
    return std::nullopt;
}

[[nodiscard]] Result<std::vector<LegacyMetadataPlanRecord>> recordsFrom(
    const Json& state, const std::filesystem::path& path) {
    std::vector<LegacyMetadataPlanRecord> records;
    std::set<std::string, std::less<>> ids;
    try {
        for (const auto& item : state.at("metadata")) {
            LegacyMetadataPlanRecord record;
            record.recordId = item.at("recordId").get<std::string>();
            const auto kind = item.at("kind").get<int>();
            if (kind < static_cast<int>(LegacyMetadataKind::ProjectVariableAliases)
                || kind > static_cast<int>(LegacyMetadataKind::RecomputedState))
                return Result<std::vector<LegacyMetadataPlanRecord>>::failure(error(
                    "A legacy metadata record kind is unsupported.", path));
            record.kind = static_cast<LegacyMetadataKind>(kind);
            if (!item.at("scriptOrdinal").is_null())
                record.scriptOrdinal = item.at("scriptOrdinal").get<std::uint32_t>();
            record.owner = item.at("owner").get<std::string>();
            record.field = item.at("field").get<std::string>();
            const auto disposition = parseDisposition(
                item.at("disposition").get<std::string>());
            if (record.recordId.empty() || !ids.insert(record.recordId).second || !disposition)
                return Result<std::vector<LegacyMetadataPlanRecord>>::failure(error(
                    "Legacy metadata record identities or dispositions are invalid.", path));
            record.disposition = *disposition;
            record.reason = item.at("reason").get<std::string>();
            records.push_back(std::move(record));
        }
    } catch (const std::exception&) {
        return Result<std::vector<LegacyMetadataPlanRecord>>::failure(error(
            "A legacy metadata record is malformed.", path));
    }
    return Result<std::vector<LegacyMetadataPlanRecord>>::success(std::move(records));
}

[[nodiscard]] std::string digestOf(const std::span<const std::byte> bytes) {
    const auto digest = sha256(bytes);
    return digest ? digest.value().toHex() : std::string{};
}

[[nodiscard]] std::vector<std::byte> encode(Json value) {
    auto text = value.dump(2);
    text.push_back('\n');
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

}  // namespace

Result<void> LegacyMetadataPromotionRegistry::add(
    std::shared_ptr<const LegacyMetadataPromotionAdapter> adapter) {
    if (!adapter || find(adapter->kind()) != nullptr)
        return Result<void>::failure(error(
            "A metadata promotion adapter is missing or duplicates an existing kind."));
    adapters_.push_back(std::move(adapter));
    return Result<void>::success();
}

const LegacyMetadataPromotionAdapter* LegacyMetadataPromotionRegistry::find(
    const LegacyMetadataKind kind) const noexcept {
    const auto found = std::ranges::find_if(adapters_, [kind](const auto& adapter) {
        return adapter->kind() == kind;
    });
    return found == adapters_.end() ? nullptr : found->get();
}

bool LegacyMetadataPromotionPreview::hasEligible() const noexcept {
    return std::ranges::any_of(items, [](const auto& item) {
        return item.assessment.eligible && !item.assessment.conflict;
    });
}

Result<LegacyMetadataPromotionPreview> LegacyMetadataPromotionService::preview(
    const LocalSalsaWorkspace& workspace, const std::string_view capsuleId,
    const LegacyMetadataPromotionRegistry& registry) {
    const auto path = workspace.componentPath(workspace.descriptor().components.importState
        / (std::string(capsuleId) + ".json"));
    std::vector<std::byte> encoded;
    auto state = readState(path, &encoded);
    if (!state) return Result<LegacyMetadataPromotionPreview>::failure(state.diagnostics());
    if (state.value().value("capsuleId", "") != capsuleId)
        return Result<LegacyMetadataPromotionPreview>::failure(error(
            "The requested capsule does not match the import-state ledger.", path));
    auto records = recordsFrom(state.value(), path);
    if (!records) return Result<LegacyMetadataPromotionPreview>::failure(records.diagnostics());
    LegacyMetadataPromotionPreview result;
    result.capsuleId = std::string(capsuleId);
    result.stateDigest = digestOf(encoded);
    const auto capsuleRoot = workspace.componentPath(
        workspace.descriptor().components.imports / std::string(capsuleId) / L"capsule");
    if (!std::filesystem::is_directory(capsuleRoot))
        return Result<LegacyMetadataPromotionPreview>::failure(error(
            "The retained legacy capsule is missing.", capsuleRoot));
    for (auto& record : records.value()) {
        LegacyMetadataPromotionAssessment assessment;
        if (record.disposition != LegacyMetadataDisposition::Pending
            && record.disposition != LegacyMetadataDisposition::DroppedByUser) {
            assessment.reason = "This record is not pending promotion.";
        } else if (const auto* adapter = registry.find(record.kind)) {
            assessment = adapter->assess(record, workspace, capsuleRoot);
        } else {
            assessment.reason = "The running SALSA version has no owning feature adapter.";
        }
        result.items.push_back({std::move(record), std::move(assessment)});
    }
    return Result<LegacyMetadataPromotionPreview>::success(std::move(result));
}

LegacyMetadataPromotionResult LegacyMetadataPromotionService::promote(
    const LocalSalsaWorkspace& workspace,
    const LegacyMetadataPromotionPreview& preview,
    const std::span<const std::string> selectedRecordIds,
    const LegacyMetadataPromotionRegistry& registry) {
    LegacyMetadataPromotionResult result;
    if (selectedRecordIds.empty()) {
        result.diagnostics.push_back(error("At least one eligible metadata record must be selected."));
        return result;
    }
    const auto stateRelative = workspace.descriptor().components.importState
        / (preview.capsuleId + ".json");
    const auto statePath = workspace.componentPath(stateRelative);
    std::vector<std::byte> currentBytes;
    auto state = readState(statePath, &currentBytes);
    if (!state) { result.diagnostics = state.diagnostics(); return result; }
    if (digestOf(currentBytes) != preview.stateDigest) {
        result.diagnostics.push_back(error(
            "The legacy import-state ledger changed after metadata preview.", statePath));
        return result;
    }
    std::set<std::string, std::less<>> selected;
    for (const auto& id : selectedRecordIds)
        if (!selected.insert(id).second) {
            result.diagnostics.push_back(error("Metadata promotion selections must be unique."));
            return result;
        }
    std::vector<WorkspaceArtifactMutation> mutations;
    std::set<std::filesystem::path> paths;
    const auto capsuleRoot = workspace.componentPath(
        workspace.descriptor().components.imports / preview.capsuleId / L"capsule");
    if (!std::filesystem::is_directory(capsuleRoot)) {
        result.diagnostics.push_back(error("The retained legacy capsule is missing.",
            capsuleRoot));
        return result;
    }
    for (const auto& item : preview.items) {
        if (!selected.contains(item.record.recordId)) continue;
        if (!item.assessment.eligible || item.assessment.conflict) {
            result.diagnostics.push_back(error(
                "A selected metadata record is no longer eligible for promotion."));
            return result;
        }
        const auto* adapter = registry.find(item.record.kind);
        if (adapter == nullptr) {
            result.diagnostics.push_back(error(
                "A selected metadata record has no owning feature adapter."));
            return result;
        }
        const auto currentAssessment = adapter->assess(
            item.record, workspace, capsuleRoot);
        if (!currentAssessment.eligible || currentAssessment.conflict) {
            result.diagnostics.push_back(error(
                "A selected metadata record became stale or conflicted after preview."));
            return result;
        }
        auto prepared = adapter->prepare(item.record, workspace, capsuleRoot);
        if (!prepared) { result.diagnostics = prepared.diagnostics(); return result; }
        for (auto& mutation : prepared.value()) {
            if (!paths.insert(mutation.relativePath).second) {
                result.diagnostics.push_back(error(
                    "Selected metadata adapters target the same workspace artifact."));
                return result;
            }
            mutations.push_back(std::move(mutation));
        }
        result.appliedRecordIds.push_back(item.record.recordId);
    }
    if (result.appliedRecordIds.size() != selected.size()) {
        result.diagnostics.push_back(error(
            "At least one selected metadata record is absent from the preview."));
        result.appliedRecordIds.clear();
        return result;
    }
    for (auto& item : state.value().at("metadata")) {
        if (!selected.contains(item.value("recordId", ""))) continue;
        item["disposition"] = "applied";
        item["reason"] = "Applied by the owning typed SALSA feature adapter.";
        if (!item.contains("history") || !item.at("history").is_array())
            item["history"] = Json::array();
        item["history"].push_back({{"decision", "applied"},
            {"applicationVersion", applicationVersion()}});
    }
    auto replacement = encode(std::move(state).takeValue());
    const auto beforeDigest = sha256(currentBytes);
    if (!beforeDigest) { result.diagnostics = beforeDigest.diagnostics(); return result; }
    mutations.push_back({stateRelative, true, beforeDigest.value().toHex(),
        std::move(replacement)});

    std::string identity = preview.capsuleId;
    for (const auto& id : selected) { identity.push_back('\0'); identity += id; }
    const auto transactionDigest = sha256(std::as_bytes(std::span{
        identity.data(), identity.size()}));
    if (!transactionDigest) { result.diagnostics = transactionDigest.diagnostics(); return result; }
    const auto transactionId = "metadata-" + transactionDigest.value().toHex().substr(0, 32);
    WorkspaceTransactionRequest request{transactionId,
        workspace.descriptor().workspaceId, "legacy-metadata-promotion",
        preview.stateDigest, std::move(mutations)};
    auto prepared = WorkspaceTransactionService::prepare(workspace.descriptor().root,
        workspace.descriptor().components.transactions, request);
    if (!prepared.succeeded()) { result.diagnostics = prepared.diagnostics; return result; }
    auto committed = WorkspaceTransactionService::commitPrepared(workspace.descriptor().root,
        workspace.descriptor().components.transactions, transactionId);
    if (committed.status != WorkspaceTransactionStatus::Verified) {
        result.diagnostics = committed.diagnostics;
        return result;
    }
    result.applied = true;
    return result;
}

}  // namespace salsa::core
