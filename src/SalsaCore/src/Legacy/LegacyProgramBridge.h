#pragma once
#include "SalsaCore/Legacy/LegacyFreshImport.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"
#include <nlohmann/json.hpp>
namespace salsa::core::legacy_detail {
struct Folder {
    std::string name;
    std::vector<spice::sct::SctSectionId> sections;
    std::optional<std::size_t> parent;
};
struct ScriptMetadata {
    std::vector<SctVariableAlias> aliases;
    std::vector<Folder> folders;
    std::vector<std::string> pending;
};
struct Script {
    spice::sct::SctDocument document;
    ScriptMetadata metadata;
};
Result<ScriptMetadata> scriptMetadata(const nlohmann::ordered_json &, std::span<const LegacyEntityMapping>);
Result<SctWorkspaceAuthoringState> projectMetadata(const std::filesystem::path &);
Result<Script> translate(const std::filesystem::path &capsuleRoot, const LegacyScriptSummary &,
                         const SctPublicationOptions &, const LegacyCapsuleValidationLimits & = {});
} // namespace salsa::core::legacy_detail
