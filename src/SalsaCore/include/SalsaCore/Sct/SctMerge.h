#pragma once

#include "SalsaCore/Sct/SctChangePlan.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {

enum class SctMergeMode {
    TrueThreeWay,
    ComparisonIntegration,
};

enum class SctMergeConflictKind {
    DivergentValue,
    ModifyDelete,
    DeleteModify,
    IncompatibleOrder,
    ReferenceTarget,
    TextOwnership,
    OpaquePreservation,
    AuthoringMetadata,
    Identity,
};

enum class SctMergeResolutionKind {
    KeepLocal,
    AcceptIncoming,
    DropLocal,
    UseEditedCandidate,
};

struct SctMergeRequest final {
    SctMergeMode mode = SctMergeMode::TrueThreeWay;
    AssetLocator locator;
    SourceRevision baseRevision;
    SctSemanticState base{};
    SctSemanticState local{};
    SctSemanticState incoming{};
    std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention{};
    std::string contextToken{};
};

struct SctMergeConflict final {
    std::string id{};
    std::string dependencyGroupId{};
    SctMergeConflictKind kind = SctMergeConflictKind::DivergentValue;
    SctChangeEntityKind entityKind = SctChangeEntityKind::Section;
    std::vector<std::string> unitIds{};
    std::vector<std::string> entityKeys{};
    std::optional<SctNavigationTarget> target{};
    std::string summary{};
    std::vector<std::string> details{};
};

struct SctMergeResolution final {
    std::string conflictId{};
    SctMergeResolutionKind kind = SctMergeResolutionKind::AcceptIncoming;
    std::optional<SctSemanticState> editedCandidate{};
};

struct SctMergePlan final {
    std::string id{};
    SctMergeRequest request;
    SctChangePlan localChanges{};
    SctChangePlan incomingChanges{};
    SctSemanticState automaticCandidate{};
    std::vector<SctMergeConflict> conflicts{};
    std::optional<SctChangePlan> automaticChangePlan{};
};

enum class SctMergePreviewStatus {
    Ready,
    Conflicted,
    Invalid,
    Stale,
};

struct SctMergePreview final {
    std::string planId{};
    SctMergePreviewStatus status = SctMergePreviewStatus::Invalid;
    std::optional<SctSemanticState> candidate{};
    std::optional<SctChangePlan> changePlan{};
    std::vector<SctMergeConflict> unresolvedConflicts{};
    std::vector<SctChangeDiagnostic> diagnostics{};
};

class SctMergePlanService final {
public:
    [[nodiscard]] static Result<SctMergePlan> build(const SctMergeRequest& request);
    [[nodiscard]] static SctMergePreview preview(
        const SctMergePlan& plan,
        std::span<const SctMergeResolution> resolutions,
        std::string_view currentContextToken);
};

}  // namespace salsa::core
