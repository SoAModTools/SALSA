#pragma once
#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SalsaCore/Project/GameProjectContext.h"
namespace salsa::core {
class SctEntrypointAuthoring final {
public:
    // Selected resource inputs only. No filename pairing or tile discovery.
    [[nodiscard]] static Result<smores::script::Resource> inspectResource(const GameProjectContext&,
        const AssetLocator&, smores::script::ResourceRole);
    [[nodiscard]] static Result<SctAuthoringState> bind(const SctAuthoringState&, SctScriptId,
        std::string name, SctEntrypointBinding, const smores::script::Catalog&, std::optional<SctEntrypointId> replace = {});
    [[nodiscard]] static Result<SctAuthoringState> createDirectHandler(const SctAuthoringState&, SctScriptId,
        const smores::script::Entry&, smores::script::ResourceRole, SctPortId response, const smores::script::Catalog&);
    [[nodiscard]] static Result<void> validateProgram(const SctAuthoringProject&, SctScriptId,
        const spice::sct::SctDocument&, bool forOutput = false);
    [[nodiscard]] static Result<void> verifyResources(const SctAuthoringProject&, SctScriptId, const GameProjectContext&);
    [[nodiscard]] static std::vector<Diagnostic> inspectBindings(const SctAuthoringProject&, SctScriptId,
        const smores::script::Catalog&);
};
}
