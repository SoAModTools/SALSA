#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {

struct SctOpcodeColor final {
    std::uint16_t opcode = 0;
    std::uint32_t colorRgb = 0;
    auto operator<=>(const SctOpcodeColor&) const = default;
};

struct SctWorkspaceAuthoringState final {
    std::vector<SctVariableAlias> projectAliases{};
    std::vector<SctOpcodeColor> opcodeColors{};
    auto operator<=>(const SctWorkspaceAuthoringState&) const = default;
};

struct SctCatalogParameterOverride final {
    std::uint32_t schemaIndex = 0;
    std::optional<std::string> label{};
    std::optional<std::uint32_t> creationDefaultWord{};
    auto operator<=>(const SctCatalogParameterOverride&) const = default;
};

struct SctCatalogOpcodeOverride final {
    std::uint16_t opcode = 0;
    std::optional<std::string> mnemonic{};
    std::optional<std::string> description{};
    std::optional<std::string> note{};
    std::vector<SctCatalogParameterOverride> parameters{};
    std::optional<std::uint32_t> colorRgb{};
    std::optional<std::string> category{};
    auto operator<=>(const SctCatalogOpcodeOverride&) const = default;
};

struct SctPersonalCatalog final {
    std::vector<SctCatalogOpcodeOverride> opcodes{};
    auto operator<=>(const SctPersonalCatalog&) const = default;
};

struct SctResolvedCatalogEntry final {
    std::uint16_t opcode = 0;
    std::string mnemonic{};
    std::string description{};
    std::string note{};
    std::vector<std::string> parameterLabels{};
    std::vector<SctCatalogParameterOverride> parameterOverrides{};
    std::optional<std::uint32_t> colorRgb{};
    std::string category{};
};

class SctAuthoringCatalogCodec final {
public:
    static constexpr std::uint32_t WorkspaceSchemaVersion = 1;
    static constexpr std::uint32_t PersonalCatalogSchemaVersion = 1;

    [[nodiscard]] static Result<std::vector<std::byte>> serializeWorkspace(
        const SctWorkspaceAuthoringState& state);
    [[nodiscard]] static Result<SctWorkspaceAuthoringState> deserializeWorkspace(
        std::span<const std::byte> bytes);
    [[nodiscard]] static Result<std::vector<std::byte>> serializeCatalog(
        const SctPersonalCatalog& catalog);
    [[nodiscard]] static Result<SctPersonalCatalog> deserializeCatalog(
        std::span<const std::byte> bytes);
};

class SctWorkspaceAuthoringStore final {
public:
    explicit SctWorkspaceAuthoringStore(std::filesystem::path path);
    [[nodiscard]] Result<SctWorkspaceAuthoringState> load() const;
    [[nodiscard]] Result<void> save(const SctWorkspaceAuthoringState& state) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
private:
    std::filesystem::path path_{};
};

class SctPersonalCatalogStore final {
public:
    explicit SctPersonalCatalogStore(std::filesystem::path path);
    [[nodiscard]] Result<SctPersonalCatalog> load() const;
    [[nodiscard]] Result<void> save(const SctPersonalCatalog& catalog) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
private:
    std::filesystem::path path_{};
};

// Process-wide because the selected overlay is personal application state.
// Snapshots are immutable and replacement is synchronized.
class SctCatalogResolver final {
public:
    static void install(std::shared_ptr<const SctPersonalCatalog> catalog);
    [[nodiscard]] static std::shared_ptr<const SctPersonalCatalog> catalog();
    [[nodiscard]] static SctResolvedCatalogEntry resolve(std::uint16_t opcode);
    static void applyCreationDefaults(spice::sct::SctInstructionFactoryRequest& request);
};

struct SctLegacyCatalogFieldChange final {
    std::uint16_t opcode = 0;
    std::string field{};
    std::string incomingSummary{};
    bool conflict = false;
};

struct SctLegacyCatalogImportPreview final {
    SctPersonalCatalog incoming{};
    std::vector<SctLegacyCatalogFieldChange> changes{};
    std::vector<std::string> ignoredLockedFields{};
};

class SctLegacyCatalogImporter final {
public:
    [[nodiscard]] static Result<SctLegacyCatalogImportPreview> preview(
        std::span<const std::byte> bytes, const SctPersonalCatalog& current);
    [[nodiscard]] static Result<SctPersonalCatalog> apply(
        const SctLegacyCatalogImportPreview& preview,
        const SctPersonalCatalog& current,
        std::span<const SctLegacyCatalogFieldChange> selected);
};

} // namespace salsa::core
