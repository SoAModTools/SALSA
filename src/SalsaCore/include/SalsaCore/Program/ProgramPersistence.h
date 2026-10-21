#pragma once
#include "SalsaCore/Program/ProgramService.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"

namespace salsa::core::program {
using Subject = decltype(Annotation::subject);
struct Placement {
    Subject subject;
    double x = 0, y = 0;
    bool collapsed = false;
};
struct DocumentTab {
    SctScriptId script;
    bool semanticView = true;
    std::optional<Subject> selection;
    editor::OutlineViewState instructions, semantic;
};
// Transient editor session/presentation is stored separately from execution.
struct Presentation {
    std::vector<Placement> placements;
    std::vector<Subject> selection;
    std::vector<DocumentTab> tabs;
    std::optional<SctScriptId> activeTab;
};
struct ProjectFile {
    std::shared_ptr<const Program> program;
    Presentation presentation;
    std::optional<Sha256Digest> encodedDigest;
    std::uint32_t sourceSchema = 11;
    std::vector<std::byte> upgradeSource; // Original encoded bytes, retained only by load for a pending upgrade.
};
struct PersistenceLimits {
    std::uint64_t encodedBytes = 1ull << 30;
    std::uint64_t decodedBytes = 4ull << 30;
    std::uint32_t collectionItems = 64u << 20;
};
struct WorkspaceRecoveryContext {
    std::filesystem::path workspaceRoot;
    std::filesystem::path transactionsRelativeRoot;
};
struct ProgramOpenRequest {
    Revision expected;
    std::filesystem::path path;
    PersistenceLimits limits;
    SctLoadingObserver progress;
    std::optional<WorkspaceRecoveryContext> recovery;
    std::optional<bool> keepUpgradeBackup = false; // Unset requests an interactive choice; false upgrades without a retained backup.
};
struct ProgramOpenResult {
    ServiceStatus status;
    Presentation presentation;
    std::optional<Sha256Digest> encodedDigest;
};
class ProgramPersistence final {
  public:
    static Result<std::vector<std::byte>> encodeFragment(const ProgramFragment &);
    static Result<std::shared_ptr<const ProgramFragment>> decodeFragment(std::span<const std::byte>);

  public:
    // SALSAPRJ framing v2, codec 1 (zstd), binary table schema 11.
    // Never accepts the old v1/zlib authoring container as a new program.
    static Result<std::vector<std::byte>> encode(const Program &, const Presentation & = {}, std::stop_token = {},
                                                 const SctLoadingObserver & = {});
    static Result<ProjectFile> decode(std::span<const std::byte>, const PersistenceLimits & = {}, std::stop_token = {},
                                      const SctLoadingObserver & = {});
    static Result<ProjectFile> load(const std::filesystem::path &, const PersistenceLimits & = {}, std::stop_token = {},
                                    const SctLoadingObserver & = {}, bool allowUpgrade = true);
    // Explicit disk upgrade after read-only decoding. A sibling backup is opt-in;
    // current checkpoints are a no-op. Called by saved opening.
    static Result<void> upgrade(const std::filesystem::path &, ProjectFile &, const WorkspaceRecoveryContext &,
                                std::stop_token = {}, const SctLoadingObserver & = {},
                                const std::function<bool()> &mayCommit = {}, bool keepBackup = false);
    // Prepares one artifact for the existing workspace transaction service.
    // Caller commits without cancellation, then marks this checkpoint's token saved.
    static Result<WorkspaceArtifactMutation> mutation(const Checkpoint &, const Presentation &,
                                                      std::filesystem::path relativePath,
                                                      std::optional<std::string> expectedSha256, std::stop_token = {},
                                                      const SctLoadingObserver & = {});
};
} // namespace salsa::core::program
