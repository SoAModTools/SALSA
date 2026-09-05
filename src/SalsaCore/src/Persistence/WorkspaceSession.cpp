#include "SalsaCore/Persistence/WorkspaceSession.h"

#include "SalsaCore/Persistence/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ranges>
#include <set>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] Diagnostic sessionError(
    std::string message, const std::optional<std::filesystem::path>& path = std::nullopt,
    const DiagnosticCode code = DiagnosticCode::InvalidWorkspaceSession) {
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

[[nodiscard]] bool validUuid(const std::string_view value) {
    if (value.size() != 36u) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8u || index == 13u || index == 18u || index == 23u) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9')
            || (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validLogicalDirectory(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    for (const auto& component : path) {
        if (component.empty() || component == L"." || component == L"..") return false;
    }
    return path.lexically_normal() == path;
}

[[nodiscard]] std::optional<std::string_view> viewName(const SctDocumentView view) {
    switch (view) {
    case SctDocumentView::Physical: return "physical";
    case SctDocumentView::Semantic: return "semantic";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> targetName(const SctNavigationKind kind) {
    switch (kind) {
    case SctNavigationKind::Document: return "document";
    case SctNavigationKind::Section: return "section";
    case SctNavigationKind::Instruction: return "instruction";
    case SctNavigationKind::String: return "string";
    case SctNavigationKind::FooterEntry: return "footer-entry";
    case SctNavigationKind::OpaqueAttachment: return "opaque-attachment";
    case SctNavigationKind::FooterGroup: return "footer-group";
    case SctNavigationKind::OpaqueGroup: return "opaque-group";
    case SctNavigationKind::SectionFolder: return "section-folder";
    case SctNavigationKind::Variable: return "variable";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SctNavigationKind> parseTargetName(
    const std::string_view value) {
    if (value == "document") return SctNavigationKind::Document;
    if (value == "section") return SctNavigationKind::Section;
    if (value == "instruction") return SctNavigationKind::Instruction;
    if (value == "string") return SctNavigationKind::String;
    if (value == "footer-entry") return SctNavigationKind::FooterEntry;
    if (value == "opaque-attachment") return SctNavigationKind::OpaqueAttachment;
    if (value == "footer-group") return SctNavigationKind::FooterGroup;
    if (value == "opaque-group") return SctNavigationKind::OpaqueGroup;
    if (value == "section-folder") return SctNavigationKind::SectionFolder;
    if (value == "variable") return SctNavigationKind::Variable;
    return std::nullopt;
}

[[nodiscard]] Json encodeTarget(const SctNavigationTarget& target) {
    Json result = Json::object();
    result["kind"] = std::string(*targetName(target.kind));
    result["id"] = target.id;
    return result;
}

[[nodiscard]] Result<SctNavigationTarget> decodeTarget(const Json& value) {
    if (!hasExactKeys(value, {"kind", "id"}) || !value.at("kind").is_string()
        || !value.at("id").is_number_unsigned()) {
        return Result<SctNavigationTarget>::failure(sessionError(
            "A session navigation target is malformed."));
    }
    const auto kind = parseTargetName(value.at("kind").get<std::string>());
    if (!kind) return Result<SctNavigationTarget>::failure(sessionError(
        "A session navigation target kind is unsupported."));
    return Result<SctNavigationTarget>::success(
        {*kind, value.at("id").get<std::uint64_t>()});
}

[[nodiscard]] Result<AssetLocator> decodeLocator(const Json& value) {
    if (!value.is_string()) return Result<AssetLocator>::failure(sessionError(
        "A session asset locator must be a string."));
    auto locator = AssetLocator::fromRelativePath(
        pathFromUtf8(value.get<std::string>()));
    if (!locator) return Result<AssetLocator>::failure(sessionError(
        "A session asset locator is invalid."));
    return locator;
}

[[nodiscard]] Result<WorkspaceSessionState> validate(WorkspaceSessionState state) {
    if (!validUuid(state.workspaceId)) return Result<WorkspaceSessionState>::failure(
        sessionError("The session workspace ID is not a canonical UUID."));
    std::set<std::string> documents;
    for (const auto& document : state.documents) {
        if (!viewName(document.view)) return Result<WorkspaceSessionState>::failure(
            sessionError("A document session view is invalid."));
        if (!documents.insert(document.locator.identityKey()).second)
            return Result<WorkspaceSessionState>::failure(sessionError(
                "Open document locators must be unique."));
    }
    if (state.activeDocument && !documents.contains(state.activeDocument->identityKey()))
        return Result<WorkspaceSessionState>::failure(sessionError(
            "The active document is not present in the open document list."));
    std::set<std::string> directories;
    for (const auto& directory : state.expandedProjectDirectories) {
        if (!validLogicalDirectory(directory)) return Result<WorkspaceSessionState>::failure(
            sessionError("An expanded project directory path is invalid."));
        if (!directories.insert(pathUtf8(directory)).second)
            return Result<WorkspaceSessionState>::failure(sessionError(
                "Expanded project directory paths must be unique."));
    }
    for (const auto& entry : state.navigation) {
        if (entry.locator.has_value() != entry.target.has_value())
            return Result<WorkspaceSessionState>::failure(sessionError(
                "A navigation entry must be either the overview or a complete document target."));
        if (entry.locator && !documents.contains(entry.locator->identityKey()))
            return Result<WorkspaceSessionState>::failure(sessionError(
                "A navigation entry refers to a document that is not open."));
    }
    if (state.navigation.empty()) {
        if (state.navigationIndex != 0u) return Result<WorkspaceSessionState>::failure(
            sessionError("An empty navigation history must use index zero."));
    } else if (state.navigationIndex >= state.navigation.size()) {
        return Result<WorkspaceSessionState>::failure(sessionError(
            "The navigation history index is out of range."));
    }
    return Result<WorkspaceSessionState>::success(std::move(state));
}

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

}  // namespace

Result<std::string> WorkspaceSessionCodec::serialize(
    const WorkspaceSessionState& state) {
    auto checked = validate(state);
    if (!checked) return Result<std::string>::failure(checked.diagnostics());
    try {
        Json root = Json::object();
        root["formatId"] = FormatId;
        root["schemaVersion"] = SchemaVersion;
        root["workspaceId"] = state.workspaceId;
        root["documents"] = Json::array();
        for (const auto& document : state.documents) {
            Json item = Json::object();
            item["locator"] = pathUtf8(document.locator.path());
            item["view"] = std::string(*viewName(document.view));
            item["selection"] = document.selection ? encodeTarget(*document.selection) : Json{};
            root["documents"].push_back(std::move(item));
        }
        root["activeDocument"] = state.activeDocument
            ? Json(pathUtf8(state.activeDocument->path())) : Json{};
        root["selectedProjectAsset"] = state.selectedProjectAsset
            ? Json(pathUtf8(state.selectedProjectAsset->path())) : Json{};
        root["expandedProjectDirectories"] = Json::array();
        for (const auto& directory : state.expandedProjectDirectories)
            root["expandedProjectDirectories"].push_back(pathUtf8(directory));
        root["navigation"] = Json::array();
        for (const auto& entry : state.navigation) {
            Json item = Json::object();
            item["locator"] = entry.locator ? Json(pathUtf8(entry.locator->path())) : Json{};
            item["target"] = entry.target ? encodeTarget(*entry.target) : Json{};
            root["navigation"].push_back(std::move(item));
        }
        root["navigationIndex"] = state.navigationIndex;
        auto text = root.dump(2);
        text.push_back('\n');
        return Result<std::string>::success(std::move(text));
    } catch (const std::exception&) {
        return Result<std::string>::failure(sessionError(
            "The workspace session could not be serialized."));
    }
}

Result<WorkspaceSessionState> WorkspaceSessionCodec::deserialize(
    const std::string_view text) {
    try {
        const auto root = Json::parse(text);
        if (!hasExactKeys(root, {"formatId", "schemaVersion", "workspaceId",
                "documents", "activeDocument", "selectedProjectAsset",
                "expandedProjectDirectories", "navigation", "navigationIndex"})
            || !root.at("formatId").is_string()
            || root.at("formatId").get<std::string>() != FormatId
            || !root.at("schemaVersion").is_number_unsigned()) {
            return Result<WorkspaceSessionState>::failure(sessionError(
                "The workspace session header is invalid."));
        }
        if (root.at("schemaVersion").get<std::uint64_t>() != SchemaVersion)
            return Result<WorkspaceSessionState>::failure(sessionError(
                "The workspace session schema is unsupported.", std::nullopt,
                DiagnosticCode::UnsupportedPersistenceSchemaVersion));
        if (!root.at("workspaceId").is_string() || !root.at("documents").is_array()
            || !root.at("expandedProjectDirectories").is_array()
            || !root.at("navigation").is_array()
            || !root.at("navigationIndex").is_number_unsigned()) {
            return Result<WorkspaceSessionState>::failure(sessionError(
                "The workspace session body is malformed."));
        }
        WorkspaceSessionState state;
        state.workspaceId = root.at("workspaceId").get<std::string>();
        for (const auto& item : root.at("documents")) {
            if (!hasExactKeys(item, {"locator", "view", "selection"})
                || !item.at("view").is_string())
                return Result<WorkspaceSessionState>::failure(sessionError(
                    "An open document session entry is malformed."));
            auto locator = decodeLocator(item.at("locator"));
            if (!locator) return Result<WorkspaceSessionState>::failure(locator.diagnostics());
            const auto viewText = item.at("view").get<std::string>();
            std::optional<SctDocumentView> view;
            if (viewText == "physical") view = SctDocumentView::Physical;
            else if (viewText == "semantic") view = SctDocumentView::Semantic;
            if (!view) return Result<WorkspaceSessionState>::failure(sessionError(
                "An open document session view is unsupported."));
            std::optional<SctNavigationTarget> selection;
            if (!item.at("selection").is_null()) {
                auto decoded = decodeTarget(item.at("selection"));
                if (!decoded) return Result<WorkspaceSessionState>::failure(decoded.diagnostics());
                selection = decoded.value();
            }
            state.documents.push_back({std::move(locator).takeValue(), *view, selection});
        }
        if (!root.at("activeDocument").is_null()) {
            auto locator = decodeLocator(root.at("activeDocument"));
            if (!locator) return Result<WorkspaceSessionState>::failure(locator.diagnostics());
            state.activeDocument = std::move(locator).takeValue();
        }
        if (!root.at("selectedProjectAsset").is_null()) {
            auto locator = decodeLocator(root.at("selectedProjectAsset"));
            if (!locator) return Result<WorkspaceSessionState>::failure(locator.diagnostics());
            state.selectedProjectAsset = std::move(locator).takeValue();
        }
        for (const auto& item : root.at("expandedProjectDirectories")) {
            if (!item.is_string()) return Result<WorkspaceSessionState>::failure(sessionError(
                "An expanded project directory path must be a string."));
            state.expandedProjectDirectories.push_back(
                pathFromUtf8(item.get<std::string>()));
        }
        for (const auto& item : root.at("navigation")) {
            if (!hasExactKeys(item, {"locator", "target"}))
                return Result<WorkspaceSessionState>::failure(sessionError(
                    "A navigation history entry is malformed."));
            WorkspaceNavigationEntry entry;
            if (!item.at("locator").is_null()) {
                auto locator = decodeLocator(item.at("locator"));
                if (!locator) return Result<WorkspaceSessionState>::failure(locator.diagnostics());
                entry.locator = std::move(locator).takeValue();
            }
            if (!item.at("target").is_null()) {
                auto target = decodeTarget(item.at("target"));
                if (!target) return Result<WorkspaceSessionState>::failure(target.diagnostics());
                entry.target = target.value();
            }
            state.navigation.push_back(std::move(entry));
        }
        const auto navigationIndex = root.at("navigationIndex").get<std::uint64_t>();
        if (navigationIndex > std::numeric_limits<std::size_t>::max())
            return Result<WorkspaceSessionState>::failure(sessionError(
                "The navigation history index is too large."));
        state.navigationIndex = static_cast<std::size_t>(navigationIndex);
        return validate(std::move(state));
    } catch (const std::exception&) {
        return Result<WorkspaceSessionState>::failure(sessionError(
            "The workspace session JSON is malformed."));
    }
}

Result<std::optional<WorkspaceSessionState>> WorkspaceSessionStore::load(
    const std::filesystem::path& path, const std::string_view expectedWorkspaceId) const {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) return Result<std::optional<WorkspaceSessionState>>::failure(sessionError(
            "The workspace session path could not be inspected.", path,
            DiagnosticCode::PersistenceReadFailed));
        return Result<std::optional<WorkspaceSessionState>>::success(std::nullopt);
    }
    std::ifstream stream(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (!stream.good() && !stream.eof())
        return Result<std::optional<WorkspaceSessionState>>::failure(sessionError(
            "The workspace session could not be read.", path,
            DiagnosticCode::PersistenceReadFailed));
    auto decoded = WorkspaceSessionCodec::deserialize(text);
    if (!decoded) return Result<std::optional<WorkspaceSessionState>>::failure(
        decoded.diagnostics());
    if (decoded.value().workspaceId != expectedWorkspaceId)
        return Result<std::optional<WorkspaceSessionState>>::failure(sessionError(
            "The workspace session belongs to a different workspace.", path));
    return Result<std::optional<WorkspaceSessionState>>::success(
        std::optional<WorkspaceSessionState>{std::move(decoded).takeValue()});
}

Result<void> WorkspaceSessionStore::checkpoint(
    const std::filesystem::path& path, const WorkspaceSessionState& state) const {
    auto encoded = WorkspaceSessionCodec::serialize(state);
    if (!encoded) return Result<void>::failure(encoded.diagnostics());
    return replaceFileAtomically(path, bytesOf(encoded.value()));
}

}  // namespace salsa::core
