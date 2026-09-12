#pragma once
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"

namespace salsa::core {
struct SctModuleBoundaryItem final {
    std::string description;
    spice::sct::SctInstructionId instruction;
    std::optional<spice::sct::SctInstructionId> destination;
};
struct SctModuleView final {
    RevisionId revision;
    SctModuleId module;
    SctScriptId script;
    std::vector<SctSequenceId> sequences;
    std::vector<SctModuleBoundaryItem> boundary;
};
class SctModuleAuthoring final {
public:
    [[nodiscard]] static Result<SctAuthoringState> extract(const SctAuthoringState& state,
        SctScriptId script, std::vector<spice::sct::SctSectionId> sections, std::string name);
    [[nodiscard]] static Result<SctAuthoringState> createEmpty(const SctAuthoringState& state,
        SctScriptId script, std::string name);
    [[nodiscard]] static Result<SctAuthoringState> appendAction(const SctAuthoringState& state,
        SctModuleId module, SctSequenceId sequence, std::uint16_t opcode);
    [[nodiscard]] static Result<SctModuleView> inspect(const SctAuthoringState& state, SctModuleId module);
};
} // namespace salsa::core
