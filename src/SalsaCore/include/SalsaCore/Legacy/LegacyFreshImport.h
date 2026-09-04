#pragma once

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Legacy/LegacyCapsule.h"
#include "SalsaCore/Project/ProjectTypes.h"
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

enum class LegacyImportRegion {
    NorthAmerica,
    Europe,
    Japan,
    Unknown,
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
    Unsupported,
    Invalid,
};

enum class LegacyMetadataKind {
    ProjectVariableAliases,
    OpcodeColors,
    FoldedSections,
    SectionGroups,
    StringGroups,
    ScriptVariableAliases,
    InstructionLabels,
    InstructionGroups,
    SuppressedInstructions,
    PreservedEvidence,
    AdvisoryDiagnostics,
    RecomputedState,
};

struct LegacyMetadataPlanRecord final {
    std::string recordId{};
    LegacyMetadataKind kind = LegacyMetadataKind::PreservedEvidence;
    std::optional<std::uint32_t> scriptOrdinal{};
    std::string owner{};
    std::string field{};
    LegacyMetadataDisposition disposition = LegacyMetadataDisposition::Pending;
    std::string reason{};
};

enum class LegacyMetadataDecisionAction {
    Retain,
    Drop,
};

struct LegacyMetadataDecision final {
    std::string recordId{};
    LegacyMetadataDecisionAction action = LegacyMetadataDecisionAction::Retain;
};

struct FreshLegacyScriptDecision final {
    std::uint32_t ordinal = 0;
    bool include = true;
    std::optional<std::string> remappedStem{};
    std::optional<SctPublicationOptions> publicationOverride{};
};

enum class LegacyEntityKind {
    Section,
    Instruction,
    String,
};

struct LegacyEntityMapping final {
    LegacyEntityKind kind = LegacyEntityKind::Section;
    std::string legacyIdentity{};
    std::uint64_t currentId = 0;
};

struct FreshLegacyImportRequest final {
    std::filesystem::path capsuleRoot{};
    std::filesystem::path sourceDirectory{};
    std::filesystem::path workspaceDirectory{};
    LegacyImportTargetScope targetScope = LegacyImportTargetScope::UnknownCustom;
    LegacyImportRegion region = LegacyImportRegion::Unknown;
    std::string customTargetName{};
    SctPublicationOptions publication{};
    std::vector<FreshLegacyScriptDecision> scriptDecisions{};
    std::vector<LegacyMetadataDecision> metadataDecisions{};
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
    std::vector<LegacyEntityMapping> entityMappings{};
    std::vector<std::string> reasons{};
};

enum class FreshLegacyImportPhase {
    ValidatingCapsule,
    InspectingDestinations,
    ConvertingScripts,
    FinalizingPlan,
    StagingArtifacts,
    CreatingWorkspace,
    PublishingDestinations,
    VerifyingCommit,
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
    LegacyImportRegion region = LegacyImportRegion::Unknown;
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

struct FreshLegacyImportPreparation final {
    FreshLegacyImportRequest request{};
    FreshLegacyImportPlan plan{};
    std::filesystem::path stagedSourceDirectory{};
};

class LegacyFreshImportPreparer final {
public:
    [[nodiscard]] static Result<FreshLegacyImportPreparation> prepare(
        const FreshLegacyImportRequest& request,
        const std::filesystem::path& stagedSourceDirectory,
        const LegacyCapsuleValidationLimits& limits = {},
        std::stop_token stopToken = {},
        const FreshLegacyImportObserver& observer = {});
};

struct FreshLegacyImportCommitHooks final {
    // Tests use this to simulate process loss after a destination is published.
    std::function<bool(std::size_t publishedDestinations)> continueAfterPublication{};
};

enum class FreshLegacyImportCommitStatus {
    Committed,
    RolledBack,
    Interrupted,
    RecoveryBlocked,
};

struct FreshLegacyImportCommitRequest final {
    FreshLegacyImportPreparation preparation{};
    std::filesystem::path originalProject{};
    std::filesystem::path stagedWorkspaceDirectory{};
    std::filesystem::path recoveryRegistryDirectory{};
};

struct FreshLegacyImportCommitResult final {
    FreshLegacyImportCommitStatus status =
        FreshLegacyImportCommitStatus::RecoveryBlocked;
    std::optional<DatasetContext> dataset{};
    std::filesystem::path workspaceDirectory{};
    std::vector<Diagnostic> diagnostics{};

    [[nodiscard]] bool succeeded() const noexcept {
        return status == FreshLegacyImportCommitStatus::Committed;
    }
};

class LegacyFreshImportCommitService final {
public:
    [[nodiscard]] static FreshLegacyImportCommitResult commit(
        const FreshLegacyImportCommitRequest& request,
        const FreshLegacyImportCommitHooks& hooks = {},
        const FreshLegacyImportObserver& observer = {});
};

class LegacyFreshImportRecoveryService final {
public:
    [[nodiscard]] static std::vector<FreshLegacyImportCommitResult> recoverAll(
        const std::filesystem::path& recoveryRegistryDirectory);
};

}  // namespace salsa::core
