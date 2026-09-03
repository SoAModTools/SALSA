#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <system_error>

#pragma comment(lib, "bcrypt.lib")

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view FormatId = "jahorta.salsa.workspace";
constexpr std::uint32_t SchemaOne = 1;
constexpr std::wstring_view ManifestName = L"project.json";
constexpr std::wstring_view RollbackName = L"project.json.schema1.rollback";

struct ParsedManifest final {
    std::string workspaceId;
    DatasetContext dataset;
    std::optional<std::filesystem::path> relativeDatasetRoot{};
    std::filesystem::path lastKnownDatasetRoot;
    SalsaWorkspaceComponents components;
};

[[nodiscard]] Diagnostic workspaceError(
    std::string message, const std::filesystem::path& path,
    const DiagnosticCode code = DiagnosticCode::InvalidSalsaWorkspace) {
    return {DiagnosticSeverity::Error, code, std::move(message), path};
}

[[nodiscard]] bool hasExactKeys(
    const Json& object, const std::initializer_list<std::string_view> keys) {
    return object.is_object() && object.size() == keys.size()
        && std::ranges::all_of(keys, [&object](const auto key) {
            return object.contains(std::string(key));
        });
}

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

[[nodiscard]] std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        result = std::filesystem::absolute(path, error);
    }
    return error ? path.lexically_normal() : result.lexically_normal();
}

[[nodiscard]] bool samePath(
    const std::filesystem::path& left, const std::filesystem::path& right) {
    return _wcsicmp(normalized(left).c_str(), normalized(right).c_str()) == 0;
}

[[nodiscard]] bool containedBy(
    const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code error;
    const auto relative = std::filesystem::relative(
        normalized(candidate), normalized(root), error);
    if (error || relative.empty() || relative.is_absolute() || relative.has_root_path())
        return false;
    for (const auto& component : relative)
        if (component == L"..") return false;
    return true;
}

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] Result<std::string> readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (!stream.good() && !stream.eof()) return Result<std::string>::failure(
        workspaceError("The SALSA workspace manifest could not be read.", path,
            DiagnosticCode::PersistenceReadFailed));
    return Result<std::string>::success(std::move(text));
}

[[nodiscard]] std::optional<unsigned int> hexNibble(const char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return std::nullopt;
}

[[nodiscard]] Result<Sha256Digest> parseDigest(
    const std::string_view value, const std::filesystem::path& path) {
    if (value.size() != Sha256Digest::Size * 2u) return Result<Sha256Digest>::failure(
        workspaceError("The workspace dataset fingerprint is not a SHA-256 value.", path));
    std::array<std::byte, Sha256Digest::Size> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = hexNibble(value[index * 2u]);
        const auto low = hexNibble(value[index * 2u + 1u]);
        if (!high || !low) return Result<Sha256Digest>::failure(workspaceError(
            "The workspace dataset fingerprint contains non-hexadecimal text.", path));
        bytes[index] = static_cast<std::byte>((*high << 4u) | *low);
    }
    return Result<Sha256Digest>::success(Sha256Digest(bytes));
}

[[nodiscard]] bool validUuid(const std::string_view value) {
    if (value.size() != 36u) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8u || index == 13u || index == 18u || index == 23u) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9')
            || (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

[[nodiscard]] Result<std::string> makeUuid() {
    std::array<unsigned char, 16> bytes{};
    const auto status = BCryptGenRandom(nullptr, bytes.data(),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return Result<std::string>::failure(workspaceError(
        "A stable workspace ID could not be generated.", {}));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4u || index == 6u || index == 8u || index == 10u) output << '-';
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return Result<std::string>::success(output.str());
}

[[nodiscard]] SalsaWorkspaceComponents defaultComponents() {
    return {L"patches", L"baselines", L"authoring", L"imports",
        L"import-state", L"history", L"receipts", L"transactions", L"session.json"};
}

[[nodiscard]] bool validComponentPath(
    const std::filesystem::path& value, const bool file) {
    if (value.empty() || value.is_absolute() || value.has_root_path()) return false;
    for (const auto& component : value)
        if (component.empty() || component == L"." || component == L"..") return false;
    const auto normalizedValue = value.lexically_normal();
    return normalizedValue == value && (!file || value.has_filename());
}

[[nodiscard]] Result<void> validateComponents(
    const SalsaWorkspaceComponents& components, const std::filesystem::path& manifest) {
    const std::array directories{&components.patches, &components.baselines,
        &components.authoring, &components.imports, &components.importState,
        &components.history, &components.receipts, &components.transactions};
    std::vector<std::wstring> identities;
    for (const auto* path : directories) {
        if (!validComponentPath(*path, false)) return Result<void>::failure(workspaceError(
            "A workspace component directory is not a contained relative path.", manifest));
        auto identity = path->generic_wstring();
        std::ranges::transform(identity, identity.begin(), towlower);
        identities.push_back(std::move(identity));
    }
    if (!validComponentPath(components.session, true)) return Result<void>::failure(
        workspaceError("The session component is not a contained relative file path.", manifest));
    auto sessionIdentity = components.session.generic_wstring();
    std::ranges::transform(sessionIdentity, sessionIdentity.begin(), towlower);
    identities.push_back(std::move(sessionIdentity));
    for (std::size_t left = 0; left < identities.size(); ++left) {
        if (identities[left] == ManifestName || identities[left] == RollbackName
            || identities[left].starts_with(std::wstring(ManifestName) + L"/")
            || identities[left].starts_with(std::wstring(RollbackName) + L"/"))
            return Result<void>::failure(workspaceError(
                "A workspace component collides with a workspace manifest.", manifest));
        for (std::size_t right = left + 1u; right < identities.size(); ++right) {
            if (identities[left] == identities[right]
                || identities[left].starts_with(identities[right] + L"/")
                || identities[right].starts_with(identities[left] + L"/"))
                return Result<void>::failure(workspaceError(
                    "Workspace component locations must be unique and non-overlapping.",
                    manifest));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] bool isEmptyStagedLayout(const std::filesystem::path& root) {
    const auto components = defaultComponents();
    const std::array allowed{components.patches, components.baselines,
        components.authoring, components.imports, components.importState,
        components.history, components.receipts, components.transactions};
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error || !entry.is_directory(error) || error) return false;
        const bool known = std::ranges::any_of(allowed, [&entry](const auto& candidate) {
            return _wcsicmp(entry.path().filename().c_str(), candidate.c_str()) == 0;
        });
        if (!known || std::filesystem::directory_iterator(entry.path(), error)
            != std::filesystem::directory_iterator{} || error) return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::string_view> platformName(const GamePlatform value) {
    switch (value) {
    case GamePlatform::GameCube: return "gamecube";
    case GamePlatform::Dreamcast: return "dreamcast";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> regionName(const GameRegion value) {
    switch (value) {
    case GameRegion::NorthAmerica: return "north-america";
    case GameRegion::Europe: return "europe";
    case GameRegion::Japan: return "japan";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<GamePlatform> parsePlatform(const std::string_view value) {
    if (value == "gamecube") return GamePlatform::GameCube;
    if (value == "dreamcast") return GamePlatform::Dreamcast;
    return std::nullopt;
}

[[nodiscard]] std::optional<GameRegion> parseRegion(const std::string_view value) {
    if (value == "north-america") return GameRegion::NorthAmerica;
    if (value == "europe") return GameRegion::Europe;
    if (value == "japan") return GameRegion::Japan;
    return std::nullopt;
}

[[nodiscard]] Json encodeOptionalPlatform(const std::optional<GamePlatform> value) {
    return value ? Json(std::string(*platformName(*value))) : Json{};
}

[[nodiscard]] Json encodeOptionalRegion(const std::optional<GameRegion> value) {
    return value ? Json(std::string(*regionName(*value))) : Json{};
}

[[nodiscard]] Result<std::optional<GamePlatform>> decodeOptionalPlatform(
    const Json& value, const std::filesystem::path& path) {
    if (value.is_null()) return Result<std::optional<GamePlatform>>::success(std::nullopt);
    if (!value.is_string()) return Result<std::optional<GamePlatform>>::failure(
        workspaceError("The workspace dataset platform is malformed.", path));
    const auto parsed = parsePlatform(value.get<std::string>());
    if (!parsed) return Result<std::optional<GamePlatform>>::failure(workspaceError(
        "The workspace dataset platform is unsupported.", path));
    return Result<std::optional<GamePlatform>>::success(parsed);
}

[[nodiscard]] Result<std::optional<GameRegion>> decodeOptionalRegion(
    const Json& value, const std::filesystem::path& path) {
    if (value.is_null()) return Result<std::optional<GameRegion>>::success(std::nullopt);
    if (!value.is_string()) return Result<std::optional<GameRegion>>::failure(
        workspaceError("The workspace dataset region is malformed.", path));
    const auto parsed = parseRegion(value.get<std::string>());
    if (!parsed) return Result<std::optional<GameRegion>>::failure(workspaceError(
        "The workspace dataset region is unsupported.", path));
    return Result<std::optional<GameRegion>>::success(parsed);
}

[[nodiscard]] Result<std::uint32_t> manifestSchema(
    const std::string_view text, const std::filesystem::path& path) {
    try {
        const auto json = Json::parse(text);
        if (!json.is_object() || !json.contains("formatId")
            || !json.contains("schemaVersion") || !json.at("formatId").is_string()
            || json.at("formatId").get<std::string>() != FormatId
            || !json.at("schemaVersion").is_number_unsigned())
            return Result<std::uint32_t>::failure(workspaceError(
                "The SALSA workspace manifest header is invalid.", path));
        const auto version = json.at("schemaVersion").get<std::uint64_t>();
        if (version == 0u || version > std::numeric_limits<std::uint32_t>::max())
            return Result<std::uint32_t>::failure(workspaceError(
                "The SALSA workspace schema version is invalid.", path));
        return Result<std::uint32_t>::success(static_cast<std::uint32_t>(version));
    } catch (const std::exception&) {
        return Result<std::uint32_t>::failure(workspaceError(
            "The SALSA workspace manifest is malformed.", path));
    }
}

[[nodiscard]] Result<std::filesystem::path> parseSchemaOne(
    const std::string_view text, const std::filesystem::path& path) {
    try {
        const auto json = Json::parse(text);
        if (!hasExactKeys(json, {"formatId", "schemaVersion", "datasetRoot"})
            || json.at("formatId").get<std::string>() != FormatId
            || json.at("schemaVersion").get<std::uint32_t>() != SchemaOne
            || !json.at("datasetRoot").is_string())
            return Result<std::filesystem::path>::failure(workspaceError(
                "The schema-one SALSA workspace manifest is invalid.", path));
        const auto dataset = pathFromUtf8(json.at("datasetRoot").get<std::string>());
        if (dataset.empty() || !dataset.is_absolute())
            return Result<std::filesystem::path>::failure(workspaceError(
                "The schema-one dataset root is invalid.", path));
        return Result<std::filesystem::path>::success(normalized(dataset));
    } catch (const std::exception&) {
        return Result<std::filesystem::path>::failure(workspaceError(
            "The schema-one SALSA workspace manifest is malformed.", path));
    }
}

[[nodiscard]] Result<ParsedManifest> parseSchemaTwo(
    const std::string_view text, const std::filesystem::path& path) {
    try {
        const auto root = Json::parse(text);
        if (!hasExactKeys(root, {"formatId", "schemaVersion", "workspaceId",
                "dataset", "components"})
            || root.at("formatId").get<std::string>() != FormatId
            || root.at("schemaVersion").get<std::uint32_t>()
                != LocalSalsaWorkspace::SchemaVersion
            || !root.at("workspaceId").is_string()
            || !validUuid(root.at("workspaceId").get<std::string>()))
            return Result<ParsedManifest>::failure(workspaceError(
                "The schema-two SALSA workspace header is invalid.", path));
        const auto& dataset = root.at("dataset");
        const auto& components = root.at("components");
        if (!hasExactKeys(dataset, {"platform", "region", "fingerprint",
                "relativeRoot", "lastKnownAbsoluteRoot"})
            || !dataset.at("fingerprint").is_string()
            || !(dataset.at("relativeRoot").is_null()
                || dataset.at("relativeRoot").is_string())
            || !dataset.at("lastKnownAbsoluteRoot").is_string()
            || !hasExactKeys(components, {"patches", "baselines", "authoring",
                "imports", "importState", "history", "receipts", "transactions",
                "session"}))
            return Result<ParsedManifest>::failure(workspaceError(
                "The schema-two SALSA workspace body is invalid.", path));
        for (const auto key : {"patches", "baselines", "authoring", "imports",
                "importState", "history", "receipts", "transactions", "session"})
            if (!components.at(key).is_string()) return Result<ParsedManifest>::failure(
                workspaceError("A workspace component path is malformed.", path));
        auto platform = decodeOptionalPlatform(dataset.at("platform"), path);
        auto region = decodeOptionalRegion(dataset.at("region"), path);
        auto fingerprint = parseDigest(dataset.at("fingerprint").get<std::string>(), path);
        if (!platform) return Result<ParsedManifest>::failure(platform.diagnostics());
        if (!region) return Result<ParsedManifest>::failure(region.diagnostics());
        if (!fingerprint) return Result<ParsedManifest>::failure(fingerprint.diagnostics());
        const auto absolute = pathFromUtf8(
            dataset.at("lastKnownAbsoluteRoot").get<std::string>());
        if (absolute.empty() || !absolute.is_absolute())
            return Result<ParsedManifest>::failure(workspaceError(
                "The last-known dataset root is invalid.", path));
        std::optional<std::filesystem::path> relative;
        if (!dataset.at("relativeRoot").is_null()) {
            relative = pathFromUtf8(dataset.at("relativeRoot").get<std::string>());
            if (relative->empty() || relative->is_absolute() || relative->has_root_path())
                return Result<ParsedManifest>::failure(workspaceError(
                    "The portable dataset root is invalid.", path));
            *relative = relative->lexically_normal();
        }
        SalsaWorkspaceComponents parsedComponents{
            pathFromUtf8(components.at("patches").get<std::string>()),
            pathFromUtf8(components.at("baselines").get<std::string>()),
            pathFromUtf8(components.at("authoring").get<std::string>()),
            pathFromUtf8(components.at("imports").get<std::string>()),
            pathFromUtf8(components.at("importState").get<std::string>()),
            pathFromUtf8(components.at("history").get<std::string>()),
            pathFromUtf8(components.at("receipts").get<std::string>()),
            pathFromUtf8(components.at("transactions").get<std::string>()),
            pathFromUtf8(components.at("session").get<std::string>())};
        auto valid = validateComponents(parsedComponents, path);
        if (!valid) return Result<ParsedManifest>::failure(valid.diagnostics());
        return Result<ParsedManifest>::success({
            root.at("workspaceId").get<std::string>(),
            DatasetContext{absolute, DatasetIdentity{platform.value(), region.value(),
                DatasetFingerprint{fingerprint.value()}}},
            relative, normalized(absolute), std::move(parsedComponents)});
    } catch (const std::exception&) {
        return Result<ParsedManifest>::failure(workspaceError(
            "The schema-two SALSA workspace manifest is malformed.", path));
    }
}

[[nodiscard]] std::optional<std::filesystem::path> portableDatasetPath(
    const std::filesystem::path& workspace, const std::filesystem::path& dataset) {
    std::error_code error;
    auto relative = std::filesystem::relative(dataset, workspace, error);
    if (error || relative.empty() || relative.is_absolute()) return std::nullopt;
    return relative.lexically_normal();
}

[[nodiscard]] ParsedManifest manifestFor(
    std::string workspaceId, const std::filesystem::path& workspace,
    const DatasetContext& dataset, SalsaWorkspaceComponents components) {
    const auto root = normalized(dataset.root);
    return {std::move(workspaceId), DatasetContext{root, dataset.identity},
        portableDatasetPath(workspace, root), root, std::move(components)};
}

[[nodiscard]] Result<std::string> serializeManifest(const ParsedManifest& manifest) {
    try {
        Json root = Json::object();
        root["formatId"] = FormatId;
        root["schemaVersion"] = LocalSalsaWorkspace::SchemaVersion;
        root["workspaceId"] = manifest.workspaceId;
        Json dataset = Json::object();
        dataset["platform"] = encodeOptionalPlatform(manifest.dataset.identity.platform);
        dataset["region"] = encodeOptionalRegion(manifest.dataset.identity.region);
        dataset["fingerprint"] = manifest.dataset.identity.fingerprint.digest.toHex();
        dataset["relativeRoot"] = manifest.relativeDatasetRoot
            ? Json(pathUtf8(*manifest.relativeDatasetRoot)) : Json{};
        dataset["lastKnownAbsoluteRoot"] = pathUtf8(manifest.lastKnownDatasetRoot);
        root["dataset"] = std::move(dataset);
        Json components = Json::object();
        components["patches"] = pathUtf8(manifest.components.patches);
        components["baselines"] = pathUtf8(manifest.components.baselines);
        components["authoring"] = pathUtf8(manifest.components.authoring);
        components["imports"] = pathUtf8(manifest.components.imports);
        components["importState"] = pathUtf8(manifest.components.importState);
        components["history"] = pathUtf8(manifest.components.history);
        components["receipts"] = pathUtf8(manifest.components.receipts);
        components["transactions"] = pathUtf8(manifest.components.transactions);
        components["session"] = pathUtf8(manifest.components.session);
        root["components"] = std::move(components);
        auto text = root.dump(2);
        text.push_back('\n');
        return Result<std::string>::success(std::move(text));
    } catch (const std::exception&) {
        return Result<std::string>::failure(workspaceError(
            "The SALSA workspace manifest could not be serialized.", {}));
    }
}

[[nodiscard]] bool identityMatches(
    const DatasetIdentity& left, const DatasetIdentity& right) {
    return left.platform == right.platform && left.region == right.region
        && left.fingerprint == right.fingerprint;
}

[[nodiscard]] Result<void> ensureComponentDirectories(
    const std::filesystem::path& root, const SalsaWorkspaceComponents& components) {
    const std::array directories{&components.patches, &components.baselines,
        &components.authoring, &components.imports, &components.importState,
        &components.history, &components.receipts, &components.transactions};
    for (const auto* relative : directories) {
        const auto path = root / *relative;
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            if (!std::filesystem::create_directories(path, error) || error)
                return Result<void>::failure(workspaceError(
                    "A workspace component directory could not be created.", path));
        }
        if (error || !std::filesystem::is_directory(path, error) || error)
            return Result<void>::failure(workspaceError(
                "A workspace component path is not an accessible directory.", path));
        if (!containedBy(root, path)) return Result<void>::failure(workspaceError(
            "A workspace component directory resolves outside the workspace.", path));
    }
    const auto session = root / components.session;
    std::error_code error;
    if (std::filesystem::exists(session, error)
        && (!std::filesystem::is_regular_file(session, error) || error))
        return Result<void>::failure(workspaceError(
            "The workspace session path is not a regular file.", session));
    if (!containedBy(root, session.parent_path()))
        return Result<void>::failure(workspaceError(
            "The workspace session path resolves outside the workspace.", session));
    return Result<void>::success();
}

[[nodiscard]] Result<void> preflightSchemaOne(
    const std::filesystem::path& root, const SalsaWorkspaceComponents& components) {
    const auto patches = root / components.patches;
    std::error_code error;
    if (!std::filesystem::exists(patches, error)) return error
        ? Result<void>::failure(workspaceError(
            "The schema-one patch directory could not be inspected.", patches))
        : Result<void>::success();
    if (!std::filesystem::is_directory(patches, error) || error)
        return Result<void>::failure(workspaceError(
            "The schema-one patch path is not a directory.", patches));
    PatchEnvelopeFileStore store;
    for (const auto& entry : std::filesystem::directory_iterator(patches, error)) {
        if (error || !entry.is_regular_file(error) || error)
            return Result<void>::failure(workspaceError(
                "The schema-one patch directory contains an unsupported entry.", patches));
        auto loaded = store.load(entry.path());
        if (!loaded || !loaded.value()) return Result<void>::failure(workspaceError(
            "A schema-one patch envelope failed upgrade preflight.", entry.path()));
    }
    return Result<void>::success();
}

[[nodiscard]] SalsaWorkspaceDescriptor descriptorFrom(
    const std::filesystem::path& root, ParsedManifest manifest) {
    return {std::move(manifest.workspaceId), root, std::move(manifest.dataset),
        std::move(manifest.relativeDatasetRoot), std::move(manifest.lastKnownDatasetRoot),
        std::move(manifest.components)};
}

}  // namespace

LocalSalsaWorkspace::LocalSalsaWorkspace(SalsaWorkspaceDescriptor descriptor)
    : descriptor_(std::move(descriptor)) {}

Result<WorkspaceOpenAssessment> LocalSalsaWorkspace::assess(
    const std::filesystem::path& workspaceRoot, const DatasetContext& dataset) {
    if (workspaceRoot.empty() || dataset.root.empty())
        return Result<WorkspaceOpenAssessment>::failure(workspaceError(
            "A SALSA workspace and scanned dataset are required.", workspaceRoot));
    const auto root = normalized(workspaceRoot);
    std::error_code error;
    if (!std::filesystem::exists(root, error)) return error
        ? Result<WorkspaceOpenAssessment>::failure(workspaceError(
            "The SALSA workspace path could not be inspected.", root))
        : Result<WorkspaceOpenAssessment>::success({WorkspaceOpenDisposition::Create,
            root, std::nullopt, "A new schema-two workspace will be created."});
    if (!std::filesystem::is_directory(root, error) || error)
        return Result<WorkspaceOpenAssessment>::failure(workspaceError(
            "The selected SALSA workspace is not an accessible directory.", root));
    const auto manifestPath = root / ManifestName;
    const auto rollbackPath = root / RollbackName;
    if (!std::filesystem::exists(manifestPath, error)) {
        if (error) return Result<WorkspaceOpenAssessment>::failure(workspaceError(
            "The workspace manifest path could not be inspected.", manifestPath));
        if (std::filesystem::exists(rollbackPath, error) && !error) {
            auto rollback = readText(rollbackPath);
            if (!rollback) return Result<WorkspaceOpenAssessment>::failure(rollback.diagnostics());
            auto oldRoot = parseSchemaOne(rollback.value(), rollbackPath);
            if (!oldRoot) return Result<WorkspaceOpenAssessment>::failure(oldRoot.diagnostics());
            return Result<WorkspaceOpenAssessment>::success({
                samePath(oldRoot.value(), dataset.root)
                    ? WorkspaceOpenDisposition::RecoverSchemaOne
                    : WorkspaceOpenDisposition::RequiresReassociation,
                root, oldRoot.value(), "An interrupted schema-one upgrade can be recovered."});
        }
        if (!isEmptyStagedLayout(root))
            return Result<WorkspaceOpenAssessment>::failure(workspaceError(
                "A new SALSA workspace must use an empty directory.", root));
        return Result<WorkspaceOpenAssessment>::success({WorkspaceOpenDisposition::Create,
            root, std::nullopt, "A new schema-two workspace will be created."});
    }
    auto text = readText(manifestPath);
    if (!text) return Result<WorkspaceOpenAssessment>::failure(text.diagnostics());
    auto version = manifestSchema(text.value(), manifestPath);
    if (!version) {
        if (std::filesystem::exists(rollbackPath, error) && !error) {
            auto rollback = readText(rollbackPath);
            auto oldRoot = rollback
                ? parseSchemaOne(rollback.value(), rollbackPath)
                : Result<std::filesystem::path>::failure(rollback.diagnostics());
            if (oldRoot)
                return Result<WorkspaceOpenAssessment>::success({
                    samePath(oldRoot.value(), dataset.root)
                        ? WorkspaceOpenDisposition::RecoverSchemaOne
                        : WorkspaceOpenDisposition::RequiresReassociation,
                    root, oldRoot.value(),
                    "The invalid upgrade manifest can be rolled back and retried."});
        }
        return Result<WorkspaceOpenAssessment>::failure(version.diagnostics());
    }
    if (version.value() > SchemaVersion)
        return Result<WorkspaceOpenAssessment>::failure(workspaceError(
            "This workspace was created by a newer unsupported SALSA schema.", manifestPath,
            DiagnosticCode::UnsupportedPersistenceSchemaVersion));
    if (version.value() == SchemaOne) {
        auto oldRoot = parseSchemaOne(text.value(), manifestPath);
        if (!oldRoot) return Result<WorkspaceOpenAssessment>::failure(oldRoot.diagnostics());
        return Result<WorkspaceOpenAssessment>::success({
            samePath(oldRoot.value(), dataset.root)
                ? WorkspaceOpenDisposition::UpgradeSchemaOne
                : WorkspaceOpenDisposition::RequiresReassociation,
            root, oldRoot.value(), samePath(oldRoot.value(), dataset.root)
                ? "The schema-one workspace will be upgraded."
                : "The schema-one workspace is associated with another dataset location."});
    }
    auto parsed = parseSchemaTwo(text.value(), manifestPath);
    if (!parsed) return Result<WorkspaceOpenAssessment>::failure(parsed.diagnostics());
    const bool identity = identityMatches(parsed.value().dataset.identity, dataset.identity);
    if (identity && samePath(parsed.value().lastKnownDatasetRoot, dataset.root))
        return Result<WorkspaceOpenAssessment>::success({
            WorkspaceOpenDisposition::OpenCompatible, root,
            parsed.value().lastKnownDatasetRoot, "The workspace matches this dataset."});
    if (identity && parsed.value().relativeDatasetRoot
        && samePath(root / *parsed.value().relativeDatasetRoot, dataset.root))
        return Result<WorkspaceOpenAssessment>::success({
            WorkspaceOpenDisposition::RebindRelative, root,
            parsed.value().lastKnownDatasetRoot,
            "The workspace and dataset moved together with an exact fingerprint match."});
    return Result<WorkspaceOpenAssessment>::success({
        WorkspaceOpenDisposition::RequiresReassociation, root,
        parsed.value().lastKnownDatasetRoot,
        identity ? "The matching dataset is outside the workspace's expected locations."
                 : "The selected dataset fingerprint differs from the workspace binding."});
}

Result<LocalSalsaWorkspace> LocalSalsaWorkspace::openOrCreate(
    const std::filesystem::path& workspaceRoot, const DatasetContext& dataset,
    const WorkspaceDatasetAcceptance acceptance) {
    auto assessment = assess(workspaceRoot, dataset);
    if (!assessment) return Result<LocalSalsaWorkspace>::failure(assessment.diagnostics());
    if (assessment.value().requiresReassociation()
        && acceptance != WorkspaceDatasetAcceptance::UserConfirmedReassociation)
        return Result<LocalSalsaWorkspace>::failure(workspaceError(
            assessment.value().message, assessment.value().workspaceRoot,
            DiagnosticCode::WorkspaceDatasetReassociationRequired));
    const auto root = assessment.value().workspaceRoot;
    std::error_code error;
    if (!std::filesystem::exists(root, error)
        && (!std::filesystem::create_directories(root, error) || error))
        return Result<LocalSalsaWorkspace>::failure(workspaceError(
            "The SALSA workspace directory could not be created.", root));
    const auto manifestPath = root / ManifestName;
    const auto rollbackPath = root / RollbackName;
    auto disposition = assessment.value().disposition;
    if (disposition == WorkspaceOpenDisposition::RequiresReassociation
        && std::filesystem::exists(rollbackPath, error) && !error) {
        bool currentIsSchemaTwo = false;
        if (std::filesystem::exists(manifestPath, error) && !error) {
            auto current = readText(manifestPath);
            auto version = current ? manifestSchema(current.value(), manifestPath)
                                   : Result<std::uint32_t>::failure(current.diagnostics());
            currentIsSchemaTwo = version && version.value() == SchemaVersion;
        }
        if (!currentIsSchemaTwo) disposition = WorkspaceOpenDisposition::RecoverSchemaOne;
    }
    if (disposition == WorkspaceOpenDisposition::RecoverSchemaOne) {
        auto rollback = readText(rollbackPath);
        if (!rollback) return Result<LocalSalsaWorkspace>::failure(rollback.diagnostics());
        auto restored = replaceFileAtomically(manifestPath, bytesOf(rollback.value()));
        if (!restored) return Result<LocalSalsaWorkspace>::failure(restored.diagnostics());
        disposition = WorkspaceOpenDisposition::UpgradeSchemaOne;
    }

    ParsedManifest finalManifest = manifestFor("00000000-0000-4000-8000-000000000000",
        root, dataset, defaultComponents());
    bool writeManifest = false;
    bool upgrading = disposition == WorkspaceOpenDisposition::UpgradeSchemaOne;
    if (disposition == WorkspaceOpenDisposition::Create || upgrading) {
        auto id = makeUuid();
        if (!id) return Result<LocalSalsaWorkspace>::failure(id.diagnostics());
        finalManifest = manifestFor(std::move(id).takeValue(), root, dataset,
            defaultComponents());
        writeManifest = true;
        if (upgrading) {
            auto old = readText(manifestPath);
            if (!old) return Result<LocalSalsaWorkspace>::failure(old.diagnostics());
            auto parsedOld = parseSchemaOne(old.value(), manifestPath);
            if (!parsedOld) return Result<LocalSalsaWorkspace>::failure(parsedOld.diagnostics());
            auto preflight = preflightSchemaOne(root, finalManifest.components);
            if (!preflight) return Result<LocalSalsaWorkspace>::failure(preflight.diagnostics());
            auto backup = replaceFileAtomically(rollbackPath, bytesOf(old.value()));
            if (!backup) return Result<LocalSalsaWorkspace>::failure(backup.diagnostics());
        }
    } else {
        auto current = readText(manifestPath);
        if (!current) return Result<LocalSalsaWorkspace>::failure(current.diagnostics());
        auto parsed = parseSchemaTwo(current.value(), manifestPath);
        if (!parsed) return Result<LocalSalsaWorkspace>::failure(parsed.diagnostics());
        finalManifest = std::move(parsed).takeValue();
        if (disposition == WorkspaceOpenDisposition::RebindRelative
            || disposition == WorkspaceOpenDisposition::RequiresReassociation) {
            finalManifest = manifestFor(finalManifest.workspaceId, root, dataset,
                finalManifest.components);
            writeManifest = true;
        }
    }

    auto componentValidation = validateComponents(finalManifest.components, manifestPath);
    if (!componentValidation)
        return Result<LocalSalsaWorkspace>::failure(componentValidation.diagnostics());
    auto directories = ensureComponentDirectories(root, finalManifest.components);
    if (!directories) return Result<LocalSalsaWorkspace>::failure(directories.diagnostics());
    if (writeManifest) {
        auto serialized = serializeManifest(finalManifest);
        if (!serialized) return Result<LocalSalsaWorkspace>::failure(serialized.diagnostics());
        auto written = replaceFileAtomically(manifestPath, bytesOf(serialized.value()));
        if (!written) return Result<LocalSalsaWorkspace>::failure(written.diagnostics());
    }
    auto verifiedText = readText(manifestPath);
    if (!verifiedText) return Result<LocalSalsaWorkspace>::failure(verifiedText.diagnostics());
    auto verified = parseSchemaTwo(verifiedText.value(), manifestPath);
    if (!verified) {
        if (upgrading) {
            auto rollback = readText(rollbackPath);
            if (rollback) (void)replaceFileAtomically(manifestPath, bytesOf(rollback.value()));
        }
        return Result<LocalSalsaWorkspace>::failure(verified.diagnostics());
    }
    if (std::filesystem::exists(rollbackPath, error) && !error)
        std::filesystem::remove(rollbackPath, error);
    if (error) return Result<LocalSalsaWorkspace>::failure(workspaceError(
        "The verified schema-one rollback manifest could not be removed.", rollbackPath));
    return Result<LocalSalsaWorkspace>::success(LocalSalsaWorkspace(
        descriptorFrom(root, std::move(verified).takeValue())));
}

const SalsaWorkspaceDescriptor& LocalSalsaWorkspace::descriptor() const noexcept {
    return descriptor_;
}

std::filesystem::path LocalSalsaWorkspace::componentPath(
    const std::filesystem::path& relativePath) const {
    return descriptor_.root / relativePath;
}

std::filesystem::path LocalSalsaWorkspace::sessionPath() const {
    return componentPath(descriptor_.components.session);
}

std::filesystem::path LocalSalsaWorkspace::patchPath(
    const AssetLocator& locator) const {
    const auto identityKey = locator.identityKey();
    const auto keyBytes = std::as_bytes(std::span{identityKey.data(), identityKey.size()});
    const auto digest = sha256(keyBytes);
    const auto name = digest ? digest.value().toHex() : identityKey;
    return componentPath(descriptor_.components.patches)
        / pathFromUtf8(name + ".salsa-patch.json");
}

Result<std::optional<PatchEnvelope>> LocalSalsaWorkspace::load(
    const AssetLocator& locator) const {
    return files_.load(patchPath(locator));
}

Result<void> LocalSalsaWorkspace::checkpoint(
    const AssetLocator& locator, const PatchEnvelope& envelope) const {
    if (envelope.affectedAssets.size() != 1u
        || envelope.affectedAssets.front().locator != locator)
        return Result<void>::failure(workspaceError(
            "A document checkpoint must contain exactly its requested asset.",
            patchPath(locator)));
    return files_.checkpoint(patchPath(locator), envelope);
}

Result<std::optional<std::vector<std::byte>>> LocalSalsaWorkspace::loadBaseline(
    const SourceRevision& revision) const {
    return DirectorySctBaselineStore(componentPath(descriptor_.components.baselines))
        .loadBaseline(revision);
}

Result<void> LocalSalsaWorkspace::retainBaseline(
    const SourceRevision& revision, const std::span<const std::byte> bytes) const {
    return DirectorySctBaselineStore(componentPath(descriptor_.components.baselines))
        .retainBaseline(revision, bytes);
}

}  // namespace salsa::core
