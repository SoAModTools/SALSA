#pragma once
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"

namespace salsa::core {
struct SctTransitionSupport final {
    SctTransitionKind kind;
    std::optional<std::uint16_t> opcode;
    bool authorable;
    std::string explanation;
};
struct SctRouteRequest final {
    SctPortId source, response;
    SctEntrypointId arrival;
    std::optional<std::uint32_t> arrivalCase;
    std::optional<SctConnectionId> replace;
    // These actions are selected explicitly; the command does not invent effects.
    std::vector<SctActionId> prerequisites, requirements, effects;
    std::optional<SctActionId> continuation;
    bool insertLoadAfterSource = false;
    std::optional<std::uint32_t> discriminatorBit;
    bool discriminatorValue = false;
};
class SctRouteAuthoring final {
  public:
    [[nodiscard]] static std::vector<SctTransitionSupport> support();
    [[nodiscard]] static Result<SctAuthoringState> connect(const SctAuthoringState &, const SctRouteRequest &);
    [[nodiscard]] static Result<SctAuthoringState> draft(const SctAuthoringState &, const SctRouteRequest &,
                                                         SctTransitionKind, std::string originalEvidence,
                                                         std::string explanation);
    // Detach route intent; existing executable instructions remain ordinary authoring content.
    [[nodiscard]] static Result<SctAuthoringState> remove(const SctAuthoringState &, SctConnectionId);
    [[nodiscard]] static std::vector<Diagnostic> validate(const SctAuthoringProject &, std::optional<SctScriptId> = {},
                                                          bool forOutput = false);
    static void reconcile(SctAuthoringProject &, SctScriptId);
    [[nodiscard]] static std::vector<SctScriptId> closure(const SctAuthoringProject &, std::span<const SctScriptId>);
};
} // namespace salsa::core
