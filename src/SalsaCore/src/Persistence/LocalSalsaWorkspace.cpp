#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace salsa::core {
namespace {

constexpr std::string_view FormatId = "jahorta.salsa.workspace";
constexpr std::uint32_t SchemaVersion = 1;

[[nodiscard]] Diagnostic workspaceError(
    std::string message, const std::filesystem::path& path) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSalsaWorkspace,
        std::move(message), path};
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

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

}  // namespace

LocalSalsaWorkspace::LocalSalsaWorkspace(SalsaWorkspaceDescriptor descriptor)
    : descriptor_(std::move(descriptor)) {}

Result<LocalSalsaWorkspace> LocalSalsaWorkspace::openOrCreate(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& datasetRoot) {
    if (workspaceRoot.empty() || datasetRoot.empty()) {
        return Result<LocalSalsaWorkspace>::failure(workspaceError(
            "A SALSA workspace and dataset root are required.", workspaceRoot));
    }
    const auto root = normalized(workspaceRoot);
    const auto dataset = normalized(datasetRoot);
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        if (!std::filesystem::create_directories(root, error) || error) {
            return Result<LocalSalsaWorkspace>::failure(workspaceError(
                "The SALSA workspace directory could not be created.", root));
        }
    }
    if (error || !std::filesystem::is_directory(root, error) || error) {
        return Result<LocalSalsaWorkspace>::failure(workspaceError(
            "The selected SALSA workspace is not an accessible directory.", root));
    }

    const auto manifestPath = root / L"project.json";
    if (std::filesystem::exists(manifestPath, error)) {
        std::ifstream stream(manifestPath, std::ios::binary);
        std::string jsonText((std::istreambuf_iterator<char>(stream)), {});
        if (!stream.good() && !stream.eof()) {
            return Result<LocalSalsaWorkspace>::failure(workspaceError(
                "The SALSA workspace manifest could not be read.", manifestPath));
        }
        try {
            const auto json = nlohmann::ordered_json::parse(jsonText);
            if (!json.is_object() || json.size() != 3u
                || !json.contains("formatId") || !json.contains("schemaVersion")
                || !json.contains("datasetRoot")
                || json.at("formatId").get<std::string>() != FormatId
                || !json.at("schemaVersion").is_number_unsigned()
                || json.at("schemaVersion").get<std::uint32_t>() != SchemaVersion
                || !json.at("datasetRoot").is_string()) {
                return Result<LocalSalsaWorkspace>::failure(workspaceError(
                    "The SALSA workspace manifest is invalid or unsupported.", manifestPath));
            }
            const auto storedText = json.at("datasetRoot").get<std::string>();
            const auto stored = pathFromUtf8(storedText);
            if (!samePath(stored, dataset)) {
                return Result<LocalSalsaWorkspace>::failure(workspaceError(
                    "The SALSA workspace is associated with a different dataset root.",
                    manifestPath));
            }
        } catch (const std::exception&) {
            return Result<LocalSalsaWorkspace>::failure(workspaceError(
                "The SALSA workspace manifest is malformed.", manifestPath));
        }
    } else {
        error.clear();
        if (std::filesystem::directory_iterator(root, error)
            != std::filesystem::directory_iterator{} || error) {
            return Result<LocalSalsaWorkspace>::failure(workspaceError(
                "A new SALSA workspace must use an empty directory.", root));
        }
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["formatId"] = FormatId;
        json["schemaVersion"] = SchemaVersion;
        json["datasetRoot"] = pathUtf8(dataset);
        auto text = json.dump(2);
        text.push_back('\n');
        const auto written = replaceFileAtomically(manifestPath, bytesOf(text));
        if (!written) return Result<LocalSalsaWorkspace>::failure(written.diagnostics());
    }

    const auto patches = root / L"patches";
    if (!std::filesystem::exists(patches, error)
        && (!std::filesystem::create_directory(patches, error) || error)) {
        return Result<LocalSalsaWorkspace>::failure(workspaceError(
            "The SALSA patch directory could not be created.", patches));
    }
    if (error || !std::filesystem::is_directory(patches, error) || error) {
        return Result<LocalSalsaWorkspace>::failure(workspaceError(
            "The SALSA patch path is not a directory.", patches));
    }
    return Result<LocalSalsaWorkspace>::success(LocalSalsaWorkspace(
        SalsaWorkspaceDescriptor{root, dataset}));
}

const SalsaWorkspaceDescriptor& LocalSalsaWorkspace::descriptor() const noexcept {
    return descriptor_;
}

std::filesystem::path LocalSalsaWorkspace::patchPath(
    const AssetLocator& locator) const {
    const auto identityKey = locator.identityKey();
    const auto keyBytes = std::as_bytes(std::span{
        identityKey.data(), identityKey.size()});
    const auto digest = sha256(keyBytes);
    const auto name = digest ? digest.value().toHex() : identityKey;
    return descriptor_.root / L"patches"
        / pathFromUtf8(name + ".salsa-patch.json");
}

Result<std::optional<PatchEnvelope>> LocalSalsaWorkspace::load(
    const AssetLocator& locator) const {
    return files_.load(patchPath(locator));
}

Result<void> LocalSalsaWorkspace::checkpoint(
    const AssetLocator& locator, const PatchEnvelope& envelope) const {
    if (envelope.affectedAssets.size() != 1u
        || envelope.affectedAssets.front().locator != locator) {
        return Result<void>::failure(workspaceError(
            "A document checkpoint must contain exactly its requested asset.",
            patchPath(locator)));
    }
    return files_.checkpoint(patchPath(locator), envelope);
}

}  // namespace salsa::core
