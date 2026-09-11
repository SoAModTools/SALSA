#pragma once
#include "SalsaCore/Authoring/SctAuthoringSession.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"

namespace salsa::core {
struct SctAuthoringCheckpoint final {
    SctAuthoringState state;
    SctAuthoringPresentation presentation;
    Sha256Digest digest;
};
class SctAuthoringStore final {
public:
    [[nodiscard]] static std::filesystem::path relativePath(const LocalSalsaWorkspace& workspace);
    // Recovery runs before any checkpoint is accepted. No partial project escapes.
    [[nodiscard]] static Result<std::optional<SctAuthoringCheckpoint>> load(
        const LocalSalsaWorkspace& workspace, std::stop_token stop = {});
    // Compose the project checkpoint into an existing workspace transaction.
    // Caller must already have retained all immutable backing bytes.
    [[nodiscard]] static Result<WorkspaceArtifactMutation> mutation(const LocalSalsaWorkspace& workspace,
        const SctAuthoringState& state, const SctAuthoringPresentation& presentation,
        std::optional<Sha256Digest> expected);
    // Expected digest is a compare-and-swap token; null requires no existing project.
    // Immutable baseline retention precedes the atomic semantic/presentation save.
    [[nodiscard]] static Result<Sha256Digest> save(const LocalSalsaWorkspace& workspace,
        const SctAuthoringState& state, const SctAuthoringPresentation& presentation,
        std::optional<Sha256Digest> expected, std::stop_token stop = {},
        const WorkspaceTransactionHooks& hooks = {});
};
} // namespace salsa::core
