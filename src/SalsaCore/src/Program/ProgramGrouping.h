#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include "SalsaCore/Program/Program.h"
#include "SpiceSCT/SctStructuredControlFlow.h"
#include <algorithm>
#include <map>
#include <unordered_map>
namespace salsa::core::program {
struct ProgramData;
// Derived diagnostics only; all groups and bodies live in the authoritative IR.
struct ControlFlowDiagnostics {
    std::map<SctActionId, std::string> issues;
};
using ControlFlowIssues = std::map<SctSequenceId, std::shared_ptr<const ControlFlowDiagnostics>>;
ControlFlowIssues normalizeImportedGroups(ProgramData &, const spice::sct::SctDocument &,
                                          const std::unordered_map<std::uint64_t, SctActionId> &,
                                          const std::map<std::uint64_t, SctSequenceId> &);
std::shared_ptr<const ControlFlowDiagnostics> analyzeFlowIssues(ProgramData &, SctSequenceId,
                                                                std::stop_token);
} // namespace salsa::core::program
