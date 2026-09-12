#pragma once
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
namespace salsa::core {
struct SctSelectorEdit final {
    std::optional<SctActionId> replace;
    std::uint32_t destination;
    spice::sct::SctCanonicalExpression expression;
};
class SctOverworldAuthoring final {
public:
    [[nodiscard]] static Result<SctAuthoringState> edit(const SctAuthoringState&, SctSequenceId,
        std::optional<SctActionId> before, const std::vector<SctSelectorEdit>&);
    [[nodiscard]] static Result<smores::script::KnownBytes> preview(const SctAuthoringState&, SctSequenceId,
        SctActionId first, SctActionId last, smores::script::KnownBytes initial);
};
}
