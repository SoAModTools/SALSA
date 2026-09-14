#pragma once
#include "SalsaCore/Program/ProgramService.h"
#include "SpiceSCT/SctDocumentExporter.h"

namespace salsa::core::program {
using Subject = decltype(Annotation::subject);
struct RealizationLocation {
    Subject subject;
    spice::sct::SctDiagnosticLocation location;
    bool generated = false;
};
struct Realization {
    Revision captured;
    SctScriptId script;
    SctRealizationId id;
    std::vector<RealizationLocation> locations;
    bool fresh(const Revision &current, SctScriptId selected, const SctRealizationId &realization) const noexcept {
        return captured == current && script == selected && id == realization;
    }
};
struct MaterializedScript {
    std::shared_ptr<const spice::sct::SctDocument> document;
    Realization realization;
    std::size_t loweredActions = 0, reusedActions = 0;
};
struct ExportedScript {
    MaterializedScript materialized;
    std::vector<std::uint8_t> bytes;
    spice::sct::SctDocumentLayout layout;
    Sha256Digest digest;
};
// One worker-owned cache. Templates contain symbolic labels and child sequence
// references, never a mutable authored document. Unchanged local constructs are
// reused; selected-script assembly and SPICE physical layout remain export work.
class ProgramMaterializer final {
  public:
    ProgramMaterializer();
    ~ProgramMaterializer();
    ProgramMaterializer(const ProgramMaterializer &) = delete;
    Result<MaterializedScript> materialize(const Checkpoint &, const Revision &expected, SctScriptId, SctRealizationId,
                                           std::stop_token = {});
    Result<ExportedScript> exportScript(const Checkpoint &, const Revision &expected, SctScriptId, SctRealizationId,
                                        const spice::sct::SctDocumentExportOptions &, std::stop_token = {});

  private:
    struct Cache;
    std::unique_ptr<Cache> cache_;
};
} // namespace salsa::core::program
