#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

enum class SctDocumentView {
    Physical,
    Semantic,
};

struct WorkspaceDocumentSession final {
    AssetLocator locator;
    SctDocumentView view = SctDocumentView::Physical;
    std::optional<SctNavigationTarget> selection{};
    bool operator==(const WorkspaceDocumentSession&) const = default;
};

struct WorkspaceNavigationEntry final {
    std::optional<AssetLocator> locator{};
    std::optional<SctNavigationTarget> target{};
    bool operator==(const WorkspaceNavigationEntry&) const = default;
};

struct WorkspaceSessionState final {
    std::string workspaceId{};
    std::vector<WorkspaceDocumentSession> documents{};
    std::optional<AssetLocator> activeDocument{};
    std::optional<AssetLocator> selectedProjectAsset{};
    std::vector<std::filesystem::path> expandedProjectDirectories{};
    std::vector<WorkspaceNavigationEntry> navigation{};
    std::size_t navigationIndex = 0;
    bool operator==(const WorkspaceSessionState&) const = default;
};

class WorkspaceSessionCodec final {
public:
    static constexpr std::string_view FormatId = "jahorta.salsa.session";
    static constexpr std::uint32_t SchemaVersion = 2;
    static constexpr std::uint32_t LegacySchemaVersion = 1;

    [[nodiscard]] static Result<std::string> serialize(
        const WorkspaceSessionState& state);
    [[nodiscard]] static Result<WorkspaceSessionState> deserialize(
        std::string_view json);
};

class WorkspaceSessionStore final {
public:
    [[nodiscard]] Result<std::optional<WorkspaceSessionState>> load(
        const std::filesystem::path& path,
        std::string_view expectedWorkspaceId) const;
    [[nodiscard]] Result<void> checkpoint(
        const std::filesystem::path& path,
        const WorkspaceSessionState& state) const;
};

}  // namespace salsa::core
