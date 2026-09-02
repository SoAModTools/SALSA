#pragma once

#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctDocumentAnalysis.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace salsa::spice_sct_prototype {

struct SctBasicBlockId final {
    spice::sct::SctSectionId section;
    spice::sct::SctInstructionId entryInstruction;
    auto operator<=>(const SctBasicBlockId&) const = default;
};

struct SctBasicBlockSuccessor final {
    spice::sct::SctInstructionId sourceInstruction;
    spice::sct::SctControlFlowKind kind = spice::sct::SctControlFlowKind::Fallthrough;
    spice::sct::SctSemanticConfidence confidence = spice::sct::SctSemanticConfidence::Unknown;
    std::optional<spice::sct::SctParameterSite> origin{};
    std::optional<SctBasicBlockId> target{};
    std::vector<spice::sct::SctOpaqueAttachmentId> crossedOpaqueAttachments{};
    bool importedOpaqueHint = false;
    auto operator<=>(const SctBasicBlockSuccessor&) const = default;
};

struct SctBasicBlock final {
    SctBasicBlockId id;
    std::vector<spice::sct::SctInstructionId> instructions{};
    std::vector<SctBasicBlockSuccessor> successors{};
    bool reachable = false;
    auto operator<=>(const SctBasicBlock&) const = default;
};

enum class SctStructureClaimStrength {
    Verified,
    EvidenceLimited,
};

enum class SctStructureEvidenceKind {
    CanonicalControlFlow,
    ConditionalFalseTarget,
    PreTargetJump,
    BackwardTerminatorJump,
    CommonForwardExit,
    PhysicalCaseBoundary,
    SharedCaseTarget,
    CaseFallthrough,
    ImportedOpaqueControlFlowGap,
};

struct SctStructureEvidence final {
    SctStructureEvidenceKind kind = SctStructureEvidenceKind::CanonicalControlFlow;
    spice::sct::SctSemanticConfidence confidence = spice::sct::SctSemanticConfidence::Unknown;
    std::optional<spice::sct::SctInstructionId> source{};
    std::optional<spice::sct::SctInstructionId> target{};
    std::optional<spice::sct::SctOpaqueAttachmentId> opaqueAttachment{};
    auto operator<=>(const SctStructureEvidence&) const = default;
};

enum class SctStructuredRegionKind {
    If,
    IfElse,
    While,
    NaturalLoop,
    Switch,
};

struct SctStructuredRegionId final {
    spice::sct::SctSectionId section;
    spice::sct::SctInstructionId headerInstruction;
    SctStructuredRegionKind kind = SctStructuredRegionKind::If;
    auto operator<=>(const SctStructuredRegionId&) const = default;
};

enum class SctStructuredArmKind {
    Then,
    Else,
    LoopBody,
    SwitchCase,
};

struct SctSwitchCaseLabel final {
    std::uint32_t repeatedGroupOrdinal = 0;
    std::optional<std::int32_t> value{};
    std::optional<spice::sct::SctParameterSite> valueSite{};
    auto operator<=>(const SctSwitchCaseLabel&) const = default;
};

struct SctStructuredArm final {
    SctStructuredArmKind kind = SctStructuredArmKind::Then;
    std::optional<SctBasicBlockId> entry{};
    std::vector<SctBasicBlockId> blocks{};
    std::vector<SctSwitchCaseLabel> caseLabels{};
    auto operator<=>(const SctStructuredArm&) const = default;
};

struct SctStructuredRegion final {
    SctStructuredRegionId id;
    SctBasicBlockId header;
    std::optional<SctBasicBlockId> join{};
    std::vector<SctBasicBlockId> members{};
    std::vector<SctStructuredArm> arms{};
    std::optional<SctStructuredRegionId> parent{};
    SctStructureClaimStrength strength = SctStructureClaimStrength::Verified;
    spice::sct::SctSemanticConfidence confidence = spice::sct::SctSemanticConfidence::Unknown;
    std::vector<SctStructureEvidence> evidence{};
    auto operator<=>(const SctStructuredRegion&) const = default;
};

enum class SctStructureIssueKind {
    UnresolvedControlFlow,
    CrossSectionControlFlow,
    MissingControlFlow,
    InsufficientConfidence,
    IrreducibleCycle,
    MultipleEntryRegion,
    AmbiguousJoin,
    OverlappingRegions,
    AmbiguousSwitchCases,
    HistoricalEdgeConflict,
    RejectedLegacyCandidate,
};

struct SctStructureIssue final {
    SctStructureIssueKind kind = SctStructureIssueKind::RejectedLegacyCandidate;
    spice::sct::SctSectionId section;
    std::optional<spice::sct::SctInstructionId> instruction{};
    std::vector<spice::sct::SctInstructionId> relatedInstructions{};
    std::vector<SctStructureEvidence> evidence{};
    auto operator<=>(const SctStructureIssue&) const = default;
};

enum class SctStructuredOutlineItemKind {
    Instruction,
    Region,
    Arm,
    BasicBlock,
    Issue,
};

struct SctStructuredOutlineItem final {
    SctStructuredOutlineItemKind kind = SctStructuredOutlineItemKind::Instruction;
    std::optional<spice::sct::SctInstructionId> instruction{};
    std::optional<SctBasicBlockId> block{};
    std::optional<SctStructuredRegionId> region{};
    std::optional<SctStructuredArmKind> arm{};
    std::vector<SctSwitchCaseLabel> caseLabels{};
    SctStructureClaimStrength strength = SctStructureClaimStrength::Verified;
    spice::sct::SctSemanticConfidence confidence = spice::sct::SctSemanticConfidence::Unknown;
    std::optional<SctStructureIssueKind> issue{};
    std::vector<SctStructureEvidence> evidence{};
    std::vector<SctStructuredOutlineItem> children{};
    auto operator<=>(const SctStructuredOutlineItem&) const = default;
};

struct SctSectionStructure final {
    spice::sct::SctSectionId section;
    std::vector<SctBasicBlock> blocks{};
    std::vector<SctStructuredRegion> regions{};
    std::vector<SctStructureIssue> issues{};
    std::vector<SctStructuredOutlineItem> outline{};
    auto operator<=>(const SctSectionStructure&) const = default;
};

class SctStructuredControlFlowAnalysis final {
public:
    [[nodiscard]] static SctStructuredControlFlowAnalysis build(
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentAnalysis& analysis);

    [[nodiscard]] std::span<const SctSectionStructure> sections() const noexcept {
        return sections_;
    }
    [[nodiscard]] const SctSectionStructure* findSection(
        spice::sct::SctSectionId section) const noexcept;
    [[nodiscard]] const SctBasicBlock* blockContaining(
        spice::sct::SctInstructionId instruction) const noexcept;
    [[nodiscard]] const SctStructuredRegion* findRegion(
        const SctStructuredRegionId& region) const noexcept;
    [[nodiscard]] std::vector<SctStructuredRegion> regionsContaining(
        spice::sct::SctInstructionId instruction) const;

private:
    std::vector<SctSectionStructure> sections_{};
};

}  // namespace salsa::spice_sct_prototype
