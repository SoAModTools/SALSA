#pragma once
#include "SalsaCore/Authoring/SctConnectivityFacts.h"
namespace salsa::core {
struct SctStoryPathOptions final {
    std::optional<std::string> start;
    std::size_t maximumEdges = 20000, maximumPaths = 64, maximumSteps = 128;
};
struct SctProjectStoryPaths final {
    SctAuthoringProjectId project;
    RevisionId revision;
    SctConnectivityJson graph;
    std::map<std::string,SctMeaningTarget> targets;
};
class SctStoryPaths final {
public:
    // Inputs are finalized qualified facts, not executable or mutable project data.
    static Result<SctConnectivityJson> propose(const std::vector<SctConnectivityJson>& scripts,
        const std::vector<SctConnectivityJson>& resourceLinks = {}, const SctStoryPathOptions& = {});
    static Result<SctProjectStoryPaths> analyze(const SctAuthoringState&, const SctStoryPathOptions& = {});
    static Result<SctAuthoringState> adopt(const SctAuthoringState&, const SctProjectStoryPaths&,
        SctStorylineId, const std::vector<std::string>& nodes);
    static Result<SctAuthoringState> reject(const SctAuthoringState&, const SctProjectStoryPaths&,
        const std::vector<std::string>& nodes);
};
}
