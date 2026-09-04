#pragma once

#include "SalsaCore/Persistence/SctScriptPatch.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {

enum class SctChangeCategory {
    Structure,
    Instruction,
    Text,
    Footer,
    AuthoringMetadata,
    Preservation,
};

enum class SctChangeDisposition {
    Informational,
    Warning,
    Conflict,
    InvalidResult,
};

enum class SctChangeEntityKind {
    Section,
    Instruction,
    IndexedString,
    FooterEntry,
    AuthoredArm,
    TextRepair,
    AllocatorState,
    OpaquePreservation,
};

enum class SctIdentityMatchStatus {
    Exact,
    Strong,
    Ambiguous,
    Unmatched,
    Contradictory,
};

enum class SctIdentityMatchProvenance {
    Automatic,
    CurrentUserDecision,
    PersistedUserDecision,
};

struct SctIdentityEvidence final {
    std::string code{};
    std::string detail{};
    auto operator<=>(const SctIdentityEvidence&) const = default;
};

struct SctChangeIdentity final {
    SctIdentityMatchStatus status = SctIdentityMatchStatus::Unmatched;
    SctIdentityMatchProvenance provenance =
        SctIdentityMatchProvenance::Automatic;
    std::optional<SctNavigationTarget> baselineTarget{};
    std::optional<SctNavigationTarget> incomingTarget{};
    std::vector<SctIdentityEvidence> evidence{};
};

struct SctChangeDiagnostic final {
    SctChangeDisposition disposition = SctChangeDisposition::Informational;
    std::string code{};
    std::string message{};
    std::optional<std::string> unitId{};
    std::optional<SctNavigationTarget> target{};
};

struct SctChangeUnit final {
    std::string id{};
    std::string entityKey{};
    SctChangeCategory category = SctChangeCategory::Structure;
    SctChangeDisposition disposition = SctChangeDisposition::Informational;
    SctChangeEntityKind entityKind = SctChangeEntityKind::Section;
    std::string summary{};
    std::vector<std::string> details{};
    std::vector<std::string> coupledUnitIds{};
    std::string dependencyGroupId{};
    std::optional<SctNavigationTarget> target{};
    std::optional<SctChangeIdentity> identity{};
    bool selectable = true;
    bool acknowledgementRequired = false;
    bool affectsOrder = false;
};

struct SctScriptComparisonInput final {
    AssetLocator locator;
    SourceRevision baselineRevision;
    SctSemanticState before{};
    SctSemanticState after{};
    std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention{};
};

struct SctScriptChangePlan final {
    std::string id{};
    AssetLocator locator;
    SourceRevision baselineRevision;
    SctSemanticState before{};
    SctSemanticState after{};
    SalsaScriptPatch completePatch{};
    std::vector<SctChangeUnit> units{};
    std::vector<SctChangeDiagnostic> diagnostics{};
};

struct SctChangePlan final {
    std::string id{};
    std::vector<SctScriptChangePlan> scripts{};
};

struct SctChangeSelection final {
    std::string planId{};
    std::vector<std::string> selectedUnitIds{};
    std::vector<std::string> acknowledgedWarningIds{};
};

struct SctCurrentScriptState final {
    AssetLocator locator;
    SctSemanticState state{};
};

enum class SctChangeApplicationStatus {
    Unchanged,
    Applied,
    RepairRequired,
    Blocked,
};

struct SctScriptChangeApplication final {
    AssetLocator locator;
    SctChangeApplicationStatus status = SctChangeApplicationStatus::Unchanged;
    std::optional<SctSemanticState> state{};
    std::vector<std::string> appliedUnitIds{};
    std::vector<SctChangeDiagnostic> diagnostics{};
};

struct SctChangeApplication final {
    std::string planId{};
    std::vector<SctScriptChangeApplication> scripts{};
};

class SctChangePlanService final {
public:
    [[nodiscard]] static Result<SctChangePlan> build(
        std::span<const SctScriptComparisonInput> scripts);
    [[nodiscard]] static SctChangeSelection selectAll(const SctChangePlan& plan);
    [[nodiscard]] static SctChangeApplication preview(
        const SctChangePlan& plan,
        std::span<const SctCurrentScriptState> current,
        const SctChangeSelection& selection);
    [[nodiscard]] static SctChangeApplication apply(
        const SctChangePlan& plan,
        std::span<const SctCurrentScriptState> current,
        const SctChangeSelection& selection);
};

class SctChangeDiagnosticFormatter final {
public:
    [[nodiscard]] static std::string format(const SctChangeDiagnostic& diagnostic);
};

}  // namespace salsa::core
