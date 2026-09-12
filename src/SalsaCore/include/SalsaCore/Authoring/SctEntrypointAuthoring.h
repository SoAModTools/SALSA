#pragma once
#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SalsaCore/Project/GameProjectContext.h"
namespace salsa::core {
class SctEntrypointAuthoring final {
public:
    struct Lowered final {
        spice::sct::SctDocument document;
        std::vector<SctRealizationRecord> generated;
    };
    [[nodiscard]] static Result<Lowered> lower(const SctAuthoringProject&, SctScriptId, const spice::sct::SctDocument&);
    [[nodiscard]] static Result<SctAuthoringState> setEnabled(const SctAuthoringState&, SctEntrypointId, bool);
    [[nodiscard]] static Result<SctAuthoringState> setActivation(const SctAuthoringState&, SctEntrypointId, SctEntrypointActivation);
    [[nodiscard]] static Result<SctAuthoringState> declareNamed(const SctAuthoringState&, SctScriptId, spice::sct::SctSectionId);
    [[nodiscard]] static Result<SctAuthoringState> associateResource(const SctAuthoringState&, SctScriptId, const smores::script::Resource&);
    [[nodiscard]] static Result<void> validateActivations(const SctAuthoringProject&, SctScriptId, const spice::sct::SctDocument&);
    // Selected optional context, including streamed tiles.
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
