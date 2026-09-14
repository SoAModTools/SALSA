#pragma once
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Legacy/LegacyFreshImport.h"
namespace salsa::core::program {
struct LegacyProgramSelection {
    std::uint32_t ordinal = 0;
    AssetLocator locator;
    SctPublicationOptions publication;
    std::optional<GameRegion> region;
    bool retainAliases = true, retainFolders = true;
};
struct LegacyProgramResult {
    ProjectFile project;
    // Retain through the existing baseline store/transaction. Never execution state.
    std::vector<SourceAssetSnapshot> baselines;
};
struct LegacyProgramMetadataPolicy { bool projectAliases = true, opcodeColors = true; };
class LegacyProgramImport final {
  public:
    // The existing trusted Python-v7 converter produces this capsule first.
    // Selected accepted scripts are translated once; no native reparse is used.
    static Result<LegacyProgramResult> importCapsule(SctAuthoringProjectId, const std::filesystem::path &capsuleRoot,
                                                     const DatasetFingerprint &,
                                                     std::span<const LegacyProgramSelection>, std::stop_token = {},
                                                     const SctLoadingObserver & = {}, LegacyProgramMetadataPolicy = {});
};
} // namespace salsa::core::program
