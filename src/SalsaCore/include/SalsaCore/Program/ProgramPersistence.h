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
    // SALSAPRJ framing v2, codec 1 (zstd), binary table schema 10.
    // Never accepts the old v1/zlib authoring container as a new program.
    static Result<std::vector<std::byte>> encode(const Program &, const Presentation & = {}, std::stop_token = {},
                                                 const SctLoadingObserver & = {});
    static Result<ProjectFile> decode(std::span<const std::byte>, const PersistenceLimits & = {}, std::stop_token = {},
                                      const SctLoadingObserver & = {});
    static Result<ProjectFile> load(const std::filesystem::path &, const PersistenceLimits & = {}, std::stop_token = {},
                                    const SctLoadingObserver & = {});
    // Prepares one artifact for the existing workspace transaction service.
    // Caller commits without cancellation, then marks this checkpoint's token saved.
    static Result<WorkspaceArtifactMutation> mutation(const Checkpoint &, const Presentation &,
                                                      std::filesystem::path relativePath,
                                                      std::optional<std::string> expectedSha256, std::stop_token = {},
                                                      const SctLoadingObserver & = {});
};
} // namespace salsa::core::program
