#include "SalsaCore/Legacy/LegacyFreshImport.h"
#include "SalsaCore/Legacy/LegacyMetadataPromotion.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "SalsaCore/Persistence/AtomicFile.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceSession.h"
#include "SalsaCore/Project/LocalGameProject.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <span>
#include <vector>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::string_view JournalFormat = "jahorta.salsa.legacy-import-transaction";
constexpr std::uint32_t JournalSchema = 1;
constexpr std::wstring_view MarkerName = L".salsa-legacy-import.json";

void report(const FreshLegacyImportObserver& observer,
    const FreshLegacyImportPhase phase, const std::uint64_t completed,
    const std::uint64_t total, std::string current = {}) {
    if (observer) observer({phase, completed, total, std::move(current)});
}

[[nodiscard]] Diagnostic failure(std::string message,
    const std::filesystem::path& path = {},
    const DiagnosticCode code = DiagnosticCode::LegacyImportInvalidRequest) {
    return {DiagnosticSeverity::Error, code, std::move(message),
        path.empty() ? std::nullopt : std::optional{path}};
}

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

[[nodiscard]] Result<std::vector<std::byte>> readBytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Result<std::vector<std::byte>>::failure(failure(
        "An import artifact could not be opened.", path,
        DiagnosticCode::PersistenceReadFailed));
    const auto end = input.tellg();
    if (end < 0) return Result<std::vector<std::byte>>::failure(failure(
        "An import artifact has an invalid size.", path,
        DiagnosticCode::PersistenceReadFailed));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), end))
        return Result<std::vector<std::byte>>::failure(failure(
            "An import artifact could not be read completely.", path,
            DiagnosticCode::PersistenceReadFailed));
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<Sha256Digest> hashFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<Sha256Digest>::failure(failure(
        "An import input could not be opened.", path,
        DiagnosticCode::PersistenceReadFailed));
    auto hasher = Sha256Hasher::create();
    if (!hasher) return Result<Sha256Digest>::failure(hasher.diagnostics());
    std::vector<char> buffer(1024u * 1024u);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            auto updated = hasher.value().update(std::as_bytes(std::span{
                buffer.data(), static_cast<std::size_t>(count)}));
            if (!updated) return Result<Sha256Digest>::failure(updated.diagnostics());
        }
    }
    if (!input.eof()) return Result<Sha256Digest>::failure(failure(
        "An import input could not be read completely.", path,
        DiagnosticCode::PersistenceReadFailed));
    return hasher.value().finish();
}

[[nodiscard]] Result<Json> readJson(const std::filesystem::path& path) {
    auto bytes = readBytes(path);
    if (!bytes) return Result<Json>::failure(bytes.diagnostics());
    try {
        return Result<Json>::success(Json::parse(
            reinterpret_cast<const char*>(bytes.value().data()),
            reinterpret_cast<const char*>(bytes.value().data() + bytes.value().size())));
    } catch (const std::exception&) {
        return Result<Json>::failure(failure("An import transaction journal is malformed.",
            path, DiagnosticCode::WorkspaceRecoveryRequired));
    }
}

[[nodiscard]] Result<void> writeJson(
    const std::filesystem::path& path, const Json& value) {
    auto text = value.dump(2);
    text.push_back('\n');
    return replaceFileAtomically(path, bytesOf(text));
}

[[nodiscard]] LegacyImportDestinationInspection inspectDestination(
    const std::filesystem::path& path) {
    LegacyImportDestinationInspection result{path};
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.state = GetLastError() == ERROR_FILE_NOT_FOUND
                || GetLastError() == ERROR_PATH_NOT_FOUND
            ? LegacyImportDestinationState::Missing
            : LegacyImportDestinationState::Unreadable;
    } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.state = LegacyImportDestinationState::ReparsePoint;
    } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.state = LegacyImportDestinationState::NotDirectory;
    } else {
        std::error_code error;
        const bool empty = std::filesystem::is_empty(path, error);
        result.state = error ? LegacyImportDestinationState::Unreadable
            : empty ? LegacyImportDestinationState::EmptyDirectory
                    : LegacyImportDestinationState::NonEmptyDirectory;
    }
    return result;
}

[[nodiscard]] std::optional<std::filesystem::path> volumeRoot(
    std::filesystem::path path) {
    std::error_code error;
    path = std::filesystem::absolute(path, error);
    if (error) return std::nullopt;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        error.clear();
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    if (path.empty() || error) return std::nullopt;
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    if (!GetVolumePathNameW(path.c_str(), buffer.data(),
            static_cast<DWORD>(buffer.size()))) return std::nullopt;
    auto root = std::filesystem::path(buffer.data()).lexically_normal();
    auto folded = root.wstring();
    std::ranges::transform(folded, folded.begin(), towlower);
    return std::filesystem::path(folded);
}

[[nodiscard]] bool sameVolume(const std::filesystem::path& left,
    const std::filesystem::path& right) {
    const auto lhs = volumeRoot(left);
    const auto rhs = volumeRoot(right);
    return lhs && rhs && *lhs == *rhs;
}

[[nodiscard]] std::optional<GamePlatform> platformFor(
    const FreshLegacyImportRequest& request) {
    if (request.targetScope == LegacyImportTargetScope::GameCube)
        return GamePlatform::GameCube;
    if (request.targetScope == LegacyImportTargetScope::DreamcastDisc1
        || request.targetScope == LegacyImportTargetScope::DreamcastDisc2)
        return GamePlatform::Dreamcast;
    return request.publication.platform == spice::sct::SctPlatform::GameCube
        ? GamePlatform::GameCube : GamePlatform::Dreamcast;
}

[[nodiscard]] std::optional<GameRegion> regionFor(const LegacyImportRegion region) {
    switch (region) {
    case LegacyImportRegion::NorthAmerica: return GameRegion::NorthAmerica;
    case LegacyImportRegion::Europe: return GameRegion::Europe;
    case LegacyImportRegion::Japan: return GameRegion::Japan;
    case LegacyImportRegion::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::string scopeName(const LegacyImportTargetScope scope) {
    switch (scope) {
    case LegacyImportTargetScope::GameCube: return "gamecube";
    case LegacyImportTargetScope::DreamcastDisc1: return "dreamcast-disc-1";
    case LegacyImportTargetScope::DreamcastDisc2: return "dreamcast-disc-2";
    case LegacyImportTargetScope::UnknownCustom: return "unknown-custom";
    }
    return "unknown-custom";
}

[[nodiscard]] std::string regionName(const LegacyImportRegion region) {
    switch (region) {
    case LegacyImportRegion::NorthAmerica: return "north-america";
    case LegacyImportRegion::Europe: return "europe";
    case LegacyImportRegion::Japan: return "japan";
    case LegacyImportRegion::Unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string dispositionName(const LegacyMetadataDisposition value) {
    switch (value) {
    case LegacyMetadataDisposition::Applied: return "applied";
    case LegacyMetadataDisposition::Pending: return "pending";
    case LegacyMetadataDisposition::Recomputed: return "recomputed";
    case LegacyMetadataDisposition::Contract: return "contract";
    case LegacyMetadataDisposition::DroppedByUser: return "dropped-by-user";
    case LegacyMetadataDisposition::Blocked: return "blocked";
    case LegacyMetadataDisposition::Unsupported: return "unsupported";
    case LegacyMetadataDisposition::Invalid: return "invalid";
    }
    return "invalid";
}

[[nodiscard]] std::string profileName(const SctPublicationOptions& options) {
    return std::to_string(static_cast<int>(options.platform)) + ":"
        + std::to_string(static_cast<int>(options.textEncoding.characters)) + ":"
        + std::to_string(static_cast<int>(options.textEncoding.messageSpace)) + ":"
        + std::to_string(static_cast<int>(options.byteOrder)) + ":"
        + std::to_string(static_cast<int>(options.wrapper));
}

[[nodiscard]] Result<void> copyCapsule(const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::error_code error;
    if (!std::filesystem::create_directories(destination, error) || error)
        return Result<void>::failure(failure("The retained capsule directory could not be created.",
            destination, DiagnosticCode::PersistenceWriteFailed));
    for (std::filesystem::recursive_directory_iterator iterator(source,
            std::filesystem::directory_options::none, error), end;
         iterator != end; iterator.increment(error)) {
        if (error) return Result<void>::failure(failure(
            "The immutable capsule could not be enumerated.", source,
            DiagnosticCode::PersistenceReadFailed));
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return Result<void>::failure(failure(
                "The immutable capsule contains an unreadable or redirected entry.",
                iterator->path(), DiagnosticCode::LegacyCapsuleInvalid));
        const auto relative = iterator->path().lexically_relative(source);
        const auto target = destination / relative;
        if (iterator->is_directory(error)) {
            std::filesystem::create_directories(target, error);
        } else if (iterator->is_regular_file(error)) {
            std::filesystem::create_directories(target.parent_path(), error);
            if (!error) std::filesystem::copy_file(iterator->path(), target,
                std::filesystem::copy_options::none, error);
        } else {
            error = std::make_error_code(std::errc::invalid_argument);
        }
        if (error) return Result<void>::failure(failure(
            "An immutable capsule artifact could not be retained.", target,
            DiagnosticCode::PersistenceWriteFailed));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> writePortableArtifacts(
    const FreshLegacyImportPreparation& preparation,
    const LocalSalsaWorkspace& workspace) {
    const auto& plan = preparation.plan;
    Json target{{"formatId", "jahorta.salsa.legacy-target-scope"},
        {"schemaVersion", 1}, {"scope", scopeName(plan.targetScope)},
        {"region", regionName(plan.region)}, {"customName", plan.customTargetName}};
    auto written = writeJson(workspace.componentPath(
        workspace.descriptor().components.authoring / L"dataset-target.json"), target);
    if (!written) return written;

    Json scripts = Json::array();
    Json mappings = Json::array();
    Json entityMappings = Json::array();
    for (const auto& script : plan.scripts) {
        Json reasons = Json::array();
        for (const auto& reason : script.reasons) reasons.push_back(reason);
        const auto status = script.status == FreshLegacyScriptPlanStatus::Ready ? "imported"
            : script.status == FreshLegacyScriptPlanStatus::Excluded ? "excluded" : "blocked";
        scripts.push_back({{"ordinal", script.ordinal}, {"legacyKey", script.legacyKey},
            {"storedName", script.storedName}, {"outputStem", script.outputStem},
            {"status", status}, {"publicationProfile", profileName(script.publication)},
            {"sha256", script.outputDigest ? script.outputDigest->toHex() : ""},
            {"size", script.outputSize}, {"reasons", std::move(reasons)}});
        if (script.status == FreshLegacyScriptPlanStatus::Ready)
            mappings.push_back({{"legacyOrdinal", script.ordinal},
                {"legacyKey", script.legacyKey},
                {"asset", pathUtf8(script.outputRelativePath)}});
        for (const auto& mapping : script.entityMappings)
            entityMappings.push_back({{"legacyOrdinal", script.ordinal},
                {"kind", static_cast<int>(mapping.kind)},
                {"legacyIdentity", mapping.legacyIdentity},
                {"currentId", mapping.currentId}});
    }
    Json metadata = Json::array();
    for (const auto& record : plan.metadata)
        metadata.push_back({{"recordId", record.recordId},
            {"kind", static_cast<int>(record.kind)},
            {"scriptOrdinal", record.scriptOrdinal ? Json(*record.scriptOrdinal) : Json{}},
            {"owner", record.owner}, {"field", record.field},
            {"disposition", dispositionName(record.disposition)},
            {"reason", record.reason},
            {"history", Json::array({{{"decision", dispositionName(record.disposition)},
                {"applicationVersion", applicationVersion()}}})}});

    Json state{{"formatId", "jahorta.salsa.legacy-import-state"},
        {"schemaVersion", 1}, {"capsuleId", plan.capsuleId},
        {"planId", plan.planId}, {"applicationVersion", applicationVersion()},
        {"assetMappings", mappings},
        {"entityMappings", entityMappings}, {"metadata", metadata}};
    written = writeJson(workspace.componentPath(workspace.descriptor().components.importState
        / (plan.capsuleId + ".json")), state);
    if (!written) return written;

    Json report{{"formatId", "jahorta.salsa.legacy-import-report"},
        {"schemaVersion", 1}, {"capsuleId", plan.capsuleId},
        {"converterContractId", plan.converterContractId}, {"planId", plan.planId},
        {"applicationVersion", applicationVersion()},
        {"target", target}, {"scripts", scripts}, {"metadata", metadata},
        {"reachability", "Creating an SCT file does not prove that the game can reach it."}};
    return writeJson(workspace.componentPath(workspace.descriptor().components.imports
        / plan.capsuleId / L"report.json"), report);
}

[[nodiscard]] Result<void> verifyPreparedScripts(
    const FreshLegacyImportPreparation& preparation) {
    for (const auto& script : preparation.plan.scripts) {
        if (script.status != FreshLegacyScriptPlanStatus::Ready) continue;
        const auto path = preparation.stagedSourceDirectory / script.outputRelativePath;
        auto bytes = readBytes(path);
        if (!bytes) return Result<void>::failure(bytes.diagnostics());
        const auto digest = sha256(bytes.value());
        if (!digest || !script.outputDigest || digest.value() != *script.outputDigest)
            return Result<void>::failure(failure(
                "A prepared SCT no longer matches its reviewed import plan.", path,
                DiagnosticCode::SourceChanged));
    }
    return Result<void>::success();
}

[[nodiscard]] std::filesystem::path backupPath(const std::filesystem::path& destination,
    const std::string_view planId) {
    return destination.parent_path()
        / (destination.filename().wstring() + L".salsa-backup-"
            + std::filesystem::path(std::string(planId.substr(0, 12))).wstring());
}

[[nodiscard]] bool moveDirectory(const std::filesystem::path& from,
    const std::filesystem::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

[[nodiscard]] bool markerMatches(const std::filesystem::path& root,
    const std::string_view planId) {
    const auto json = readJson(root / MarkerName);
    return json && json.value().is_object() && json.value().value("planId", "") == planId;
}

struct RecoveryPaths final {
    std::string planId{};
    std::filesystem::path registryRoot{};
    std::filesystem::path source{};
    std::filesystem::path workspace{};
    std::filesystem::path sourceStage{};
    std::filesystem::path workspaceStage{};
    std::filesystem::path sourceBackup{};
    std::filesystem::path workspaceBackup{};
    bool sourceWasMissing = true;
    bool workspaceWasMissing = true;
    std::optional<GamePlatform> platform{};
    std::optional<GameRegion> region{};
};

[[nodiscard]] Json encodeJournal(const RecoveryPaths& paths) {
    return {{"formatId", JournalFormat}, {"schemaVersion", JournalSchema},
        {"planId", paths.planId}, {"source", pathUtf8(paths.source)},
        {"workspace", pathUtf8(paths.workspace)},
        {"sourceStage", pathUtf8(paths.sourceStage)},
        {"workspaceStage", pathUtf8(paths.workspaceStage)},
        {"sourceBackup", pathUtf8(paths.sourceBackup)},
        {"workspaceBackup", pathUtf8(paths.workspaceBackup)},
        {"sourceWasMissing", paths.sourceWasMissing},
        {"workspaceWasMissing", paths.workspaceWasMissing},
        {"platform", paths.platform ? Json(static_cast<int>(*paths.platform)) : Json{}},
        {"region", paths.region ? Json(static_cast<int>(*paths.region)) : Json{}}};
}

[[nodiscard]] Result<RecoveryPaths> decodeJournal(
    const std::filesystem::path& registryRoot) {
    auto value = readJson(registryRoot / L"transaction.json");
    if (!value || !value.value().is_object()
        || value.value().value("formatId", "") != JournalFormat
        || value.value().value("schemaVersion", 0u) != JournalSchema)
        return Result<RecoveryPaths>::failure(value ? std::vector{failure(
            "A legacy import recovery journal is unsupported or malformed.", registryRoot,
            DiagnosticCode::WorkspaceRecoveryRequired)} : value.diagnostics());
    const auto& json = value.value();
    try {
        RecoveryPaths result{json.at("planId").get<std::string>(),
            registryRoot, pathFromUtf8(json.at("source").get<std::string>()),
            pathFromUtf8(json.at("workspace").get<std::string>()),
            pathFromUtf8(json.at("sourceStage").get<std::string>()),
            pathFromUtf8(json.at("workspaceStage").get<std::string>()),
            pathFromUtf8(json.at("sourceBackup").get<std::string>()),
            pathFromUtf8(json.at("workspaceBackup").get<std::string>()),
            json.at("sourceWasMissing").get<bool>(),
            json.at("workspaceWasMissing").get<bool>()};
        if (!json.at("platform").is_null())
            result.platform = static_cast<GamePlatform>(json.at("platform").get<int>());
        if (!json.at("region").is_null())
            result.region = static_cast<GameRegion>(json.at("region").get<int>());
        return Result<RecoveryPaths>::success(std::move(result));
    } catch (const std::exception&) {
        return Result<RecoveryPaths>::failure(failure(
            "A legacy import recovery journal is incomplete.", registryRoot,
            DiagnosticCode::WorkspaceRecoveryRequired));
    }
}

void removeKnownDirectory(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

[[nodiscard]] FreshLegacyImportCommitResult blocked(const RecoveryPaths& paths,
    std::string message, const std::filesystem::path& path) {
    return {FreshLegacyImportCommitStatus::RecoveryBlocked, std::nullopt,
        paths.workspace, {failure(std::move(message), path,
            DiagnosticCode::WorkspaceRecoveryRequired)}};
}

[[nodiscard]] FreshLegacyImportCommitResult finishRecovery(RecoveryPaths paths) {
    std::error_code error;
    const bool sourcePublished = std::filesystem::is_directory(paths.source, error)
        && !error && markerMatches(paths.source, paths.planId);
    error.clear();
    bool workspacePublished = std::filesystem::is_directory(paths.workspace, error)
        && !error && markerMatches(paths.workspace, paths.planId);

    if (sourcePublished && !workspacePublished
        && std::filesystem::is_directory(paths.workspaceStage, error) && !error
        && markerMatches(paths.workspaceStage, paths.planId)) {
        if (std::filesystem::exists(paths.workspace, error)) {
            if (!std::filesystem::is_empty(paths.workspace, error) || error
                || !moveDirectory(paths.workspace, paths.workspaceBackup))
                return blocked(paths, "Recovery could not preserve the empty workspace destination.",
                    paths.workspace);
        }
        if (!moveDirectory(paths.workspaceStage, paths.workspace))
            return blocked(paths, "Recovery could not finish publishing the workspace.",
                paths.workspace);
        workspacePublished = true;
    }

    if (sourcePublished && workspacePublished) {
        auto project = LocalGameProject::inspect({paths.source, paths.platform, paths.region});
        auto workspace = project ? LocalSalsaWorkspace::openOrCreate(
            paths.workspace, project.value().dataset())
            : Result<LocalSalsaWorkspace>::failure(project.diagnostics());
        if (project && workspace) {
            std::filesystem::remove(paths.source / MarkerName, error);
            error.clear();
            std::filesystem::remove(paths.workspace / MarkerName, error);
            removeKnownDirectory(paths.sourceBackup);
            removeKnownDirectory(paths.workspaceBackup);
            removeKnownDirectory(paths.sourceStage);
            removeKnownDirectory(paths.workspaceStage);
            removeKnownDirectory(paths.registryRoot);
            return {FreshLegacyImportCommitStatus::Committed, project.value().dataset(),
                paths.workspace, {}};
        }
    }

    const auto rollbackPublished = [&](const std::filesystem::path& destination) {
        std::error_code local;
        if (!std::filesystem::exists(destination, local)) return true;
        if (local || !markerMatches(destination, paths.planId)) return false;
        std::filesystem::remove_all(destination, local);
        return !local;
    };
    if (!rollbackPublished(paths.source) || !rollbackPublished(paths.workspace))
        return blocked(paths,
            "Recovery found a destination that no longer matches the interrupted import.",
            !markerMatches(paths.source, paths.planId) ? paths.source : paths.workspace);
    if (std::filesystem::exists(paths.sourceBackup, error)
        && !moveDirectory(paths.sourceBackup, paths.source))
        return blocked(paths, "Recovery could not restore the original empty source directory.",
            paths.source);
    error.clear();
    if (std::filesystem::exists(paths.workspaceBackup, error)
        && !moveDirectory(paths.workspaceBackup, paths.workspace))
        return blocked(paths, "Recovery could not restore the original empty workspace directory.",
            paths.workspace);
    removeKnownDirectory(paths.sourceStage);
    removeKnownDirectory(paths.workspaceStage);
    removeKnownDirectory(paths.registryRoot);
    return {FreshLegacyImportCommitStatus::RolledBack, std::nullopt,
        paths.workspace, {}};
}

}  // namespace

FreshLegacyImportCommitResult LegacyFreshImportCommitService::commit(
    const FreshLegacyImportCommitRequest& request,
    const FreshLegacyImportCommitHooks& hooks,
    const FreshLegacyImportObserver& observer) {
    const auto& preparation = request.preparation;
    const auto& plan = preparation.plan;
    const auto fail = [&](std::string message, const std::filesystem::path& path = {}) {
        return FreshLegacyImportCommitResult{FreshLegacyImportCommitStatus::RecoveryBlocked,
            std::nullopt, plan.workspaceDestination.path,
            {failure(std::move(message), path)}};
    };
    if (!plan.ready() || plan.planId.empty() || request.originalProject.empty()
        || request.stagedWorkspaceDirectory.empty()
        || request.recoveryRegistryDirectory.empty())
        return fail("A complete reviewed fresh-import preparation is required.");
    auto capsule = LegacyCapsuleReader::validate(preparation.request.capsuleRoot);
    if (!capsule || capsule.value().capsuleId != plan.capsuleId)
        return fail("The legacy capsule changed after import review.",
            preparation.request.capsuleRoot);
    const auto originalDigest = hashFile(request.originalProject);
    if (!originalDigest || originalDigest.value().toHex() != capsule.value().source.sha256)
        return fail("The original legacy project changed after conversion.",
            request.originalProject);
    const auto currentSource = inspectDestination(plan.sourceDestination.path);
    const auto currentWorkspace = inspectDestination(plan.workspaceDestination.path);
    if (!currentSource.fresh() || !currentWorkspace.fresh()
        || currentSource.state != plan.sourceDestination.state
        || currentWorkspace.state != plan.workspaceDestination.state)
        return fail("A selected fresh destination changed before publication.");
    if (!sameVolume(plan.sourceDestination.path, plan.workspaceDestination.path)
        || !sameVolume(plan.sourceDestination.path, preparation.stagedSourceDirectory)
        || !sameVolume(plan.sourceDestination.path, request.stagedWorkspaceDirectory))
        return fail("Fresh source, workspace, and staging directories must share one filesystem volume.");
    auto prepared = verifyPreparedScripts(preparation);
    if (!prepared) return {FreshLegacyImportCommitStatus::RecoveryBlocked, std::nullopt,
        plan.workspaceDestination.path, prepared.diagnostics()};

    report(observer, FreshLegacyImportPhase::CreatingWorkspace, 0, 1);
    std::error_code error;
    if (std::filesystem::exists(request.stagedWorkspaceDirectory, error) || error)
        return fail("The private workspace staging directory already exists.",
            request.stagedWorkspaceDirectory);
    auto stagedProject = LocalGameProject::inspect({preparation.stagedSourceDirectory,
        platformFor(preparation.request), regionFor(preparation.request.region)});
    if (!stagedProject) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
        std::nullopt, plan.workspaceDestination.path, stagedProject.diagnostics()};
    auto finalDataset = stagedProject.value().dataset();
    finalDataset.root = plan.sourceDestination.path;
    auto workspace = LocalSalsaWorkspace::openOrCreate(
        request.stagedWorkspaceDirectory, stagedProject.value().dataset());
    if (!workspace) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
        std::nullopt, plan.workspaceDestination.path, workspace.diagnostics()};
    for (const auto& script : plan.scripts) {
        if (script.status != FreshLegacyScriptPlanStatus::Ready || !script.outputDigest) continue;
        auto bytes = readBytes(preparation.stagedSourceDirectory / script.outputRelativePath);
        if (!bytes) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
            std::nullopt, plan.workspaceDestination.path, bytes.diagnostics()};
        auto retained = workspace.value().retainBaseline(
            SourceRevision{*script.outputDigest}, bytes.value());
        if (!retained) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
            std::nullopt, plan.workspaceDestination.path, retained.diagnostics()};
    }
    const auto importRoot = workspace.value().componentPath(
        workspace.value().descriptor().components.imports / plan.capsuleId);
    auto copied = copyCapsule(preparation.request.capsuleRoot, importRoot / L"capsule");
    if (!copied) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
        std::nullopt, plan.workspaceDestination.path, copied.diagnostics()};
    auto portable = writePortableArtifacts(preparation, workspace.value());
    if (!portable) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
        std::nullopt, plan.workspaceDestination.path, portable.diagnostics()};
    LegacyMetadataPromotionRegistry registry;
    auto registered = registerBuiltInLegacyMetadataPromotionAdapters(registry);
    if (!registered) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, registered.diagnostics()};
    auto preview = LegacyMetadataPromotionService::preview(workspace.value(), plan.capsuleId, registry);
    if (!preview) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, preview.diagnostics()};
    std::vector<std::string> eligible;
    for (const auto& item : preview.value().items)
        if (item.record.disposition == LegacyMetadataDisposition::Pending && item.assessment.eligible && !item.assessment.conflict)
            eligible.push_back(item.record.recordId);
    if (!eligible.empty()) {
        auto promoted = LegacyMetadataPromotionService::promote(workspace.value(), preview.value(), eligible, registry);
        if (!promoted.applied) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, promoted.diagnostics};
    }
    auto created = SctAuthoringProject::create();
    if (!created) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, created.diagnostics()};
    SctAuthoringState authored{std::move(created).takeValue(), {}};
    auto metadata = SctWorkspaceAuthoringStore(workspace.value().componentPath(
        workspace.value().descriptor().components.authoring / L"workspace.json")).load();
    if (!metadata) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, metadata.diagnostics()};
    authored.project.workspaceAuthoring = metadata.value();
    for (const auto& script : plan.scripts) {
        if (script.status != FreshLegacyScriptPlanStatus::Ready) continue;
        auto locator = AssetLocator::fromRelativePath(script.outputRelativePath);
        if (!locator) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, locator.diagnostics()};
        auto loaded = SctPatchCheckpointService::load(stagedProject.value(), &workspace.value(), &workspace.value(), locator.value());
        if (!loaded.load.succeeded() || loaded.patchConflict)
            return fail("A normalized legacy script could not be adopted into the authoring project.");
        const auto& provenance = *loaded.baseline->provenance;
        auto adopted = SctAuthoringImporter::import(authored.project, authored.project.revision,
            {provenance.source(), finalDataset.identity, {finalDataset.identity.platform,
                provenance.textSelectionOrigin == SctTextSelectionOrigin::UserSelected}, provenance.textConvention, script.outputStem});
        if (!adopted) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, adopted.diagnostics()};
        authored.programs.push_back(adopted.value().program);
        auto working = SctAuthoringMaterializer::replaceWorkingState(adopted.value().project, authored.programs, adopted.value().script,
            {loaded.load.document->document, loaded.authoredArms, loaded.textRepairs, loaded.unboundReferences,
                loaded.aliases, loaded.annotations, loaded.folders});
        if (!working) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, working.diagnostics()};
        authored.project = std::move(working).takeValue();
        std::ranges::find(authored.project.scripts, adopted.value().script, &SctScriptContext::id)->legacyOrigin =
            SctLegacyOrigin{plan.capsuleId, script.legacyKey, script.ordinal};
    }
    auto checkpoint = SctAuthoringStore::save(workspace.value(), authored, {authored.project.id}, {});
    if (!checkpoint) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, checkpoint.diagnostics()};
    // The staged project is complete before either destination is published.
    // Rebind only the source location; authored identity never contains it.
    workspace = LocalSalsaWorkspace::openOrCreate(request.stagedWorkspaceDirectory, finalDataset,
        WorkspaceDatasetAcceptance::UserConfirmedReassociation);
    if (!workspace) return {FreshLegacyImportCommitStatus::RecoveryBlocked, {}, plan.workspaceDestination.path, workspace.diagnostics()};
    WorkspaceSessionState session{workspace.value().descriptor().workspaceId};
    auto sessionWritten = WorkspaceSessionStore{}.checkpoint(
        workspace.value().sessionPath(), session);
    if (!sessionWritten) return {FreshLegacyImportCommitStatus::RecoveryBlocked,
        std::nullopt, plan.workspaceDestination.path, sessionWritten.diagnostics()};
    report(observer, FreshLegacyImportPhase::CreatingWorkspace, 1, 1);

    const auto registryRoot = request.recoveryRegistryDirectory / plan.planId;
    RecoveryPaths paths{plan.planId, registryRoot, plan.sourceDestination.path,
        plan.workspaceDestination.path, preparation.stagedSourceDirectory,
        request.stagedWorkspaceDirectory,
        backupPath(plan.sourceDestination.path, plan.planId),
        backupPath(plan.workspaceDestination.path, plan.planId),
        plan.sourceDestination.state == LegacyImportDestinationState::Missing,
        plan.workspaceDestination.state == LegacyImportDestinationState::Missing,
        platformFor(preparation.request), regionFor(preparation.request.region)};
    error.clear();
    const bool sourceBackupExists = std::filesystem::exists(paths.sourceBackup, error);
    if (error) return fail("A reserved import backup path could not be inspected.",
        paths.sourceBackup);
    const bool workspaceBackupExists = std::filesystem::exists(paths.workspaceBackup, error);
    if (error) return fail("A reserved import backup path could not be inspected.",
        paths.workspaceBackup);
    if (sourceBackupExists || workspaceBackupExists)
        return fail("A reserved import backup path already exists.",
            sourceBackupExists ? paths.sourceBackup : paths.workspaceBackup);
    if (std::filesystem::exists(registryRoot, error) || error)
        return fail("A recovery journal already exists for this import plan.", registryRoot);
    if (!std::filesystem::create_directories(registryRoot, error) || error)
        return fail("The import recovery registry could not be created.", registryRoot);
    const Json marker{{"formatId", "jahorta.salsa.legacy-import-marker"},
        {"schemaVersion", 1}, {"planId", plan.planId}};
    auto journal = writeJson(registryRoot / L"transaction.json", encodeJournal(paths));
    auto sourceMarker = writeJson(paths.sourceStage / MarkerName, marker);
    auto workspaceMarker = writeJson(paths.workspaceStage / MarkerName, marker);
    if (!journal || !sourceMarker || !workspaceMarker)
        return fail("The durable import publication markers could not be written.", registryRoot);

    report(observer, FreshLegacyImportPhase::PublishingDestinations, 0, 2);
    if (!paths.sourceWasMissing && !moveDirectory(paths.source, paths.sourceBackup))
        return fail("The empty source destination could not be preserved.", paths.source);
    if (!moveDirectory(paths.sourceStage, paths.source))
        return finishRecovery(paths);
    report(observer, FreshLegacyImportPhase::PublishingDestinations, 1, 2);
    if (hooks.continueAfterPublication && !hooks.continueAfterPublication(1u))
        return {FreshLegacyImportCommitStatus::Interrupted, std::nullopt,
            paths.workspace, {}};
    if (!paths.workspaceWasMissing && !moveDirectory(paths.workspace, paths.workspaceBackup))
        return finishRecovery(paths);
    if (!moveDirectory(paths.workspaceStage, paths.workspace))
        return finishRecovery(paths);
    report(observer, FreshLegacyImportPhase::PublishingDestinations, 2, 2);
    if (hooks.continueAfterPublication && !hooks.continueAfterPublication(2u))
        return {FreshLegacyImportCommitStatus::Interrupted, std::nullopt,
            paths.workspace, {}};

    report(observer, FreshLegacyImportPhase::VerifyingCommit, 0, 2);
    auto reopenedProject = LocalGameProject::inspect({paths.source,
        platformFor(preparation.request), regionFor(preparation.request.region)});
    if (!reopenedProject || reopenedProject.value().dataset().identity.fingerprint
            != finalDataset.identity.fingerprint)
        return blocked(paths, "The published source dataset failed verification.", paths.source);
    report(observer, FreshLegacyImportPhase::VerifyingCommit, 1, 2);
    auto reopenedWorkspace = LocalSalsaWorkspace::openOrCreate(paths.workspace,
        reopenedProject.value().dataset());
    if (!reopenedWorkspace)
        return {FreshLegacyImportCommitStatus::RecoveryBlocked, std::nullopt,
            paths.workspace, reopenedWorkspace.diagnostics()};
    report(observer, FreshLegacyImportPhase::VerifyingCommit, 2, 2);
    auto recovered = finishRecovery(paths);
    recovered.dataset = reopenedProject.value().dataset();
    return recovered;
}

std::vector<FreshLegacyImportCommitResult> LegacyFreshImportRecoveryService::recoverAll(
    const std::filesystem::path& recoveryRegistryDirectory) {
    std::vector<FreshLegacyImportCommitResult> results;
    std::error_code error;
    if (!std::filesystem::exists(recoveryRegistryDirectory, error)) return results;
    for (const auto& entry : std::filesystem::directory_iterator(
            recoveryRegistryDirectory, error)) {
        if (error || !entry.is_directory(error) || error) continue;
        auto paths = decodeJournal(entry.path());
        if (!paths) {
            results.push_back({FreshLegacyImportCommitStatus::RecoveryBlocked,
                std::nullopt, {}, paths.diagnostics()});
        } else {
            results.push_back(finishRecovery(std::move(paths).takeValue()));
        }
    }
    return results;
}

}  // namespace salsa::core
