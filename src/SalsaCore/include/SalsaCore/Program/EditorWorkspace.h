#pragma once
#include <map>
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"

namespace salsa::core::program {
// Installed only after a worker has successfully prepared this complete session.
// One service owns execution. Views keep IDs and scoped read results only.
struct EditorWorkspace {
    std::shared_ptr<ProgramService> service;
    std::shared_ptr<const LocalGameProject> dataset;
    std::shared_ptr<const LocalSalsaWorkspace> workspace;
    ServiceStatus status;
    Presentation presentation;
    Catalog catalog;
    std::optional<std::string> checkpointDigest;
    bool needsInitialSave = false;
    std::uint64_t openedMicroseconds = 0;
};
struct EditorOpenRequest {
    LocalGameProjectOptions dataset;
    std::filesystem::path workspace;
    WorkspaceDatasetAcceptance acceptance = WorkspaceDatasetAcceptance::ExactOnly;
    // Empty means SPICE source assessment, independently for each script.
    std::optional<spice::sct::SctKnownTextConvention> textConvention;
    std::map<std::string, spice::sct::SctKnownTextConvention> textOverrides;
    std::size_t nativeWorkers = 0;
};
struct EditorSaveResult {
    ServiceStatus status;
    std::string digest;
    std::uint64_t elapsedMicroseconds = 0;
};
class EditorWorkspaceIO final {
  public:
    // Blocking worker functions; never run these on the UI thread. Cancellation
    // leaves the previous installed session untouched. A worker owns its stage.
    static Result<std::shared_ptr<EditorWorkspace>> open(const EditorOpenRequest &, std::stop_token = {},
                                                         const SctLoadingObserver & = {});
    static Result<smores::script::Resource> inspectResource(const EditorWorkspace &, const AssetLocator &,
                                                            smores::script::ResourceRole, std::stop_token = {});
    static Result<EditorSaveResult> save(const EditorWorkspace &, Revision, const Presentation &, std::stop_token = {},
                                         const SctLoadingObserver & = {});
    static Result<ExportedScript> publish(const EditorWorkspace &, Revision, SctScriptId,
                                          const spice::sct::SctDocumentExportOptions &, const std::filesystem::path &,
                                          bool replaceSource, std::stop_token = {}, const SctLoadingObserver & = {});
};
} // namespace salsa::core::program
