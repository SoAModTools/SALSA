#pragma once
#include "SalsaCore/Authoring/SctAuthoringIdentity.h"
#include <variant>
// Existing persisted identity types, independent of program storage records.
namespace salsa::core::program {
using SectionId = SctAuthoringId<struct ProgramSectionTag>;
using ConditionId = SctAuthoringId<struct ProgramConditionTag>;
using ArmId = SctAuthoringId<struct ProgramArmTag>;
}
namespace salsa::core::editor {
using Subject = std::variant<SctScriptId, program::SectionId, SctSequenceId, SctActionId,
    program::ArmId, program::ConditionId, SctModuleId, SctEntrypointId, SctTextResourceId,
    SctPortId, SctConnectionId>;
struct Revision {
    SctAuthoringProjectId project;
    std::uint64_t revision = 0, generation = 0;
    auto operator<=>(const Revision &) const = default;
};
struct SessionStatus {
    Revision current;
    std::uint64_t historyState = 0;
    bool dirty = false;
};
}
