#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include "SalsaCore/Program/Program.h"
#include "SpiceSCT/SctStructuredControlFlow.h"
#include <algorithm>
#include <map>
#include <unordered_map>
namespace salsa::core::program {
struct ProgramData;
struct GroupedRow {
    SctActionId action;
    std::optional<editor::FlowOccurrence> occurrence;
    std::string label;
    ActionKind kind = ActionKind::Instruction;
};
// Derived, value-owned presentation. No SPICE graphs or source pointers survive.
struct FlowGrouping {
    std::vector<GroupedRow> roots;
    std::map<editor::FlowOccurrence, std::vector<GroupedRow>> bodies;
    std::map<editor::FlowOccurrence, editor::FlowOccurrence> parents;
    std::vector<std::pair<SctActionId, editor::FlowOccurrence>> locations;
    const editor::FlowOccurrence *location(SctActionId id) const {
        auto found = std::lower_bound(locations.begin(), locations.end(), id,
                                      [](const auto &item, auto key) { return item.first < key; });
        return found != locations.end() && found->first == id ? &found->second : nullptr;
    }
    std::map<SctActionId, SctActionId> joins;
    std::map<SctActionId, std::string> issues;
};
using FlowGroups = std::map<SctSequenceId, std::shared_ptr<const FlowGrouping>>;
FlowGroups importFlowGroups(const spice::sct::SctDocument &,
                            const std::unordered_map<std::uint64_t, SctActionId> &,
                            const std::map<std::uint64_t, SctSequenceId> &);
std::shared_ptr<const FlowGrouping> analyzeFlowGroup(ProgramData &, SctSequenceId, std::stop_token);
} // namespace salsa::core::program
