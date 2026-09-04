#pragma once

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Legacy/LegacyCapsule.h"
#include "SalsaCore/Sct/SctPublication.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace salsa::core {

enum class LegacyImportTargetScope {
    GameCube,
    DreamcastDisc1,
    DreamcastDisc2,
    UnknownCustom,
};

enum class LegacyImportDestinationState {
    Missing,
    EmptyDirectory,
    NonEmptyDirectory,
    NotDirectory,
    ReparsePoint,
    Unreadable,
};

struct LegacyImportDestinationInspection final {
    std::filesystem::path path{};
    LegacyImportDestinationState state = LegacyImportDestinationState::Unreadable;

    [[nodiscard]] bool fresh() const noexcept {
        return state == LegacyImportDestinationState::Missing
            || state == LegacyImportDestinationState::EmptyDirectory;
    }
};

enum class LegacyMetadataDisposition {
    Applied,
    Pending,
    Recomputed,
    Contract,
    DroppedByUser,
    Blocked,
};

struct LegacyMetadataPlanRecord final {
    std::optional<std::uint32_t> scriptOrdinal{};
    std::string owner{};
    std::string field{};
    LegacyMetadataDisposition disposition = LegacyMetadataDisposition::Pending;
    std::string reason{};
};

struct FreshLegacyScriptDecision final {
    std::uint32_t ordinal = 0;
    bool include = true;
    std::optional<std::string> remappedStem{};
    std::optional<SctPublicationOptions> publicationOverride{};
};

struct FreshLegacyImportRequest final {
    std::filesystem::path capsuleRoot{};
    std::filesystem::path sourceDirectory{};
    std::filesystem::path workspaceDirectory{};
    LegacyImportTargetScope targetScope = LegacyImportTargetScope::UnknownCustom;
    std::string customTargetName{};
    SctPublicationOptions publication{};
    std::vector<FreshLegacyScriptDecision> scriptDecisions{};
    bool discardUnsupportedMetadata = false;
};

enum class FreshLegacyScriptPlanStatus {
    Ready,
    Excluded,
    Blocked,
};

struct FreshLegacyScriptPlan final {
    std::uint32_t ordinal = 0;
    std::string legacyKey{};
    std::string storedName{};
    std::string outputStem{};
    std::filesystem::path outputRelativePath{};
    FreshLegacyScriptPlanStatus status = FreshLegacyScriptPlanStatus::Blocked;
    SctPublicationOptions publication{};
    std::optional<Sha256Digest> outputDigest{};
    std::uint64_t outputSize = 0;
    std::uint32_t decodedPayloadSize = 0;
    bool reparseEquivalent = false;
    std::vector<std::string> reasons{};
};

enum class FreshLegacyImportPhase {
    ValidatingCapsule,
    InspectingDestinations,
    ConvertingScripts,
    FinalizingPlan,
};

struct FreshLegacyImportProgress final {
    FreshLegacyImportPhase phase = FreshLegacyImportPhase::ValidatingCapsule;
    std::uint64_t completed = 0;
    std::uint64_t total = 1;
    std::string currentScript{};
};

using FreshLegacyImportObserver = std::function<void(const FreshLegacyImportProgress&)>;

struct FreshLegacyImportPlan final {
    std::string planId{};
    std::string capsuleId{};
    std::string converterContractId{};
    LegacyImportTargetScope targetScope = LegacyImportTargetScope::UnknownCustom;
    std::string customTargetName{};
    LegacyImportDestinationInspection sourceDestination{};
    LegacyImportDestinationInspection workspaceDestination{};
    std::vector<FreshLegacyScriptPlan> scripts{};
    std::vector<LegacyMetadataPlanRecord> metadata{};

    [[nodiscard]] bool ready() const noexcept;
};

class LegacyFreshImportPlanner final {
public:
    [[nodiscard]] static Result<FreshLegacyImportPlan> plan(
        const FreshLegacyImportRequest& request,
        const LegacyCapsuleValidationLimits& limits = {},
        std::stop_token stopToken = {},
        const FreshLegacyImportObserver& observer = {});
};

}  // namespace salsa::core
