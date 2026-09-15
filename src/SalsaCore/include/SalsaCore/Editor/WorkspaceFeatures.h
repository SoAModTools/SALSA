#pragma once
#include "SalsaCore/Foundation/LoadingProgress.h"
#include "SalsaCore/Editor/EditorIdentity.h"
#include "SalsaCore/Foundation/Result.h"
#include <functional>
#include <stop_token>
namespace salsa::core::editor {
enum class SourceState { Unchecked, Unchanged, Changed, Missing, Added, Unqualified };
struct WorkspaceSource {
    std::optional<SctScriptId> script;
    std::string name, asset;
    std::optional<std::uint64_t> baselineBytes, currentBytes;
    std::string baselineDigest, currentDigest;
    SourceState state = SourceState::Unchecked;
};
struct WorkspaceOverview {
    Revision revision;
    std::string datasetRoot, workspaceRoot, checkpointPath;
    bool dirty = false, needsInitialSave = false, inspected = false;
    std::vector<WorkspaceSource> sources;
};
// Blocking adapter contract. A review observes files, never changes authoring or baselines.
using WorkspaceReview =
    std::function<Result<WorkspaceOverview>(Revision, std::stop_token, const SctLoadingObserver &)>;
} // namespace salsa::core::editor
