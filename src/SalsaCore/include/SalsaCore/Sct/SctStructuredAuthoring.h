#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SpiceSCT/SctStructuredControlFlow.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {

class SctWorkingState;

struct SctAuthoredArmId final {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    auto operator<=>(const SctAuthoredArmId&) const = default;
};

struct SctStructuredControllerKey final {
    spice::sct::SctSectionId section;
    spice::sct::SctInstructionId instruction;
    auto operator<=>(const SctStructuredControllerKey&) const = default;
};

enum class SctAuthoredArmRealization {
    Virtual,
    Physical,
};

struct SctAuthoredArm final {
    SctAuthoredArmId id;
    SctStructuredControllerKey controller;
    spice::sct::SctStructuredArmKind kind =
        spice::sct::SctStructuredArmKind::Then;
    std::optional<std::int32_t> caseValue{};
    std::optional<spice::sct::SctInstructionId> expectedJoin{};
    std::vector<spice::sct::SctInstructionId> members{};
    std::vector<spice::sct::SctInstructionId> managedScaffolding{};
    SctAuthoredArmRealization realization = SctAuthoredArmRealization::Virtual;
    auto operator<=>(const SctAuthoredArm&) const = default;
};

struct SctSetAuthoredArmOperation final {
    SctAuthoredArmId id;
    std::optional<SctAuthoredArm> before{};
    std::optional<SctAuthoredArm> after{};
    auto operator<=>(const SctSetAuthoredArmOperation&) const = default;
};

struct SctStructuredAuthoringOperationBatch final {
    std::vector<SctSetAuthoredArmOperation> operations{};
};

struct SctStructuredAuthoringChange final {
    SctAuthoredArmId id;
    std::optional<SctAuthoredArm> before{};
    std::optional<SctAuthoredArm> after{};
    auto operator<=>(const SctStructuredAuthoringChange&) const = default;
};

struct SctStructuredAuthoringApplication final {
    SctStructuredAuthoringOperationBatch inverse{};
    std::vector<SctStructuredAuthoringChange> changes{};
    std::optional<std::string> issue{};
    [[nodiscard]] bool succeeded() const noexcept { return !issue.has_value(); }
};

class SctStructuredAuthoringState final {
public:
    SctStructuredAuthoringState() = default;
    explicit SctStructuredAuthoringState(std::span<const SctAuthoredArm> arms);
    [[nodiscard]] SctAuthoredArmId nextId() const noexcept;
    [[nodiscard]] std::span<const SctAuthoredArm> arms() const noexcept;
    [[nodiscard]] const SctAuthoredArm* find(SctAuthoredArmId id) const noexcept;
    [[nodiscard]] SctStructuredAuthoringApplication apply(
        const SctStructuredAuthoringOperationBatch& batch);

private:
    std::vector<SctAuthoredArm> arms_{};
    std::uint64_t nextId_ = 1;
};

enum class SctSemanticArmStatus {
    Verified,
    Virtual,
    NeedsValue,
    PendingVerification,
    Conflicted,
};

struct SctSemanticAuthoredArmProjection final {
    SctAuthoredArm arm{};
    SctSemanticArmStatus status = SctSemanticArmStatus::Virtual;
    std::vector<spice::sct::SctDocumentInstruction> visibleMembers{};
};

class SctSemanticEditorProjection final {
public:
    [[nodiscard]] static SctSemanticEditorProjection build(
        const spice::sct::SctStructuredControlFlowAnalysis& analysis,
        const SctWorkingState& workingState,
        const SctStructuredAuthoringState& authoring,
        RevisionId workingRevision,
        RevisionId verifiedRevision);

    [[nodiscard]] std::span<const SctSemanticAuthoredArmProjection>
        authoredArms() const noexcept { return authoredArms_; }
    [[nodiscard]] const SctSemanticAuthoredArmProjection* find(
        SctAuthoredArmId id) const noexcept;

private:
    std::vector<SctSemanticAuthoredArmProjection> authoredArms_{};
};

}  // namespace salsa::core
