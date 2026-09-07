#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctStructuredControlFlow.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

struct SctUnboundReferenceOrigin final {
    spice::sct::SctParameterSite site;
    std::string sourceAssetIdentity{};
    spice::sct::SctDocumentReferenceTarget sourceTarget;
    std::optional<std::string> sourceTargetNameBytes{};
    auto operator<=>(const SctUnboundReferenceOrigin&) const = default;
};

struct SctSetUnboundReferenceOriginOperation final {
    spice::sct::SctParameterSite site;
    std::optional<SctUnboundReferenceOrigin> before{};
    std::optional<SctUnboundReferenceOrigin> after{};
    auto operator<=>(const SctSetUnboundReferenceOriginOperation&) const = default;
};

enum class SctVariableKind : std::uint8_t { Bit, Byte, Integer, Float };

struct SctVariableKey final {
    SctVariableKind kind = SctVariableKind::Integer;
    std::uint32_t index = 0;
    auto operator<=>(const SctVariableKey&) const = default;
};

struct SctVariableAlias final {
    SctVariableKey variable{};
    std::string alias{};
    auto operator<=>(const SctVariableAlias&) const = default;
};

enum class SctAuthoringTargetKind : std::uint8_t {
    Document, Section, Instruction, String, SupplementaryText, Variable,
};

struct SctAuthoringTarget final {
    SctAuthoringTargetKind kind = SctAuthoringTargetKind::Document;
    std::uint64_t id = 0;
    std::optional<SctVariableKind> variableKind{};
    auto operator<=>(const SctAuthoringTarget&) const = default;
};

struct SctEntityAnnotation final {
    SctAuthoringTarget target{};
    std::optional<std::string> note{};
    // An empty string denotes a bookmark whose label is derived from the entity.
    std::optional<std::string> bookmarkLabel{};
    std::optional<std::uint32_t> colorRgb{};
    auto operator<=>(const SctEntityAnnotation&) const = default;
};

struct SctSectionFolderId final {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    auto operator<=>(const SctSectionFolderId&) const = default;
};

struct SctSectionFolder final {
    SctSectionFolderId id{};
    std::optional<SctSectionFolderId> parent{};
    std::string name{};
    std::vector<spice::sct::SctSectionId> sections{};
    std::optional<std::string> note{};
    std::optional<std::string> bookmarkLabel{};
    std::optional<std::uint32_t> colorRgb{};
    auto operator<=>(const SctSectionFolder&) const = default;
};

struct SctSetVariableAliasOperation final {
    SctVariableKey variable{};
    std::optional<SctVariableAlias> before{};
    std::optional<SctVariableAlias> after{};
    auto operator<=>(const SctSetVariableAliasOperation&) const = default;
};

struct SctSetEntityAnnotationOperation final {
    SctAuthoringTarget target{};
    std::optional<SctEntityAnnotation> before{};
    std::optional<SctEntityAnnotation> after{};
    auto operator<=>(const SctSetEntityAnnotationOperation&) const = default;
};

struct SctSetSectionFolderOperation final {
    SctSectionFolderId id{};
    std::optional<SctSectionFolder> before{};
    std::optional<SctSectionFolder> after{};
    auto operator<=>(const SctSetSectionFolderOperation&) const = default;
};

struct SctStructuredAuthoringOperationBatch final {
    std::vector<SctSetAuthoredArmOperation> operations{};
    std::vector<SctSetUnboundReferenceOriginOperation> unboundReferences{};
    std::vector<SctSetVariableAliasOperation> aliases{};
    std::vector<SctSetEntityAnnotationOperation> annotations{};
    std::vector<SctSetSectionFolderOperation> folders{};

    [[nodiscard]] bool empty() const noexcept {
        return operations.empty() && unboundReferences.empty() && aliases.empty()
            && annotations.empty() && folders.empty();
    }
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
    explicit SctStructuredAuthoringState(
        std::span<const SctAuthoredArm> arms,
        std::span<const SctUnboundReferenceOrigin> unboundReferences = {},
        std::span<const SctVariableAlias> aliases = {},
        std::span<const SctEntityAnnotation> annotations = {},
        std::span<const SctSectionFolder> folders = {});
    [[nodiscard]] SctAuthoredArmId nextId() const noexcept;
    [[nodiscard]] std::span<const SctAuthoredArm> arms() const noexcept;
    [[nodiscard]] const SctAuthoredArm* find(SctAuthoredArmId id) const noexcept;
    [[nodiscard]] std::span<const SctUnboundReferenceOrigin>
        unboundReferences() const noexcept;
    [[nodiscard]] std::span<const SctVariableAlias> aliases() const noexcept;
    [[nodiscard]] const SctVariableAlias* findAlias(SctVariableKey variable) const noexcept;
    [[nodiscard]] std::span<const SctEntityAnnotation> annotations() const noexcept;
    [[nodiscard]] const SctEntityAnnotation* findAnnotation(
        SctAuthoringTarget target) const noexcept;
    [[nodiscard]] std::span<const SctSectionFolder> folders() const noexcept;
    [[nodiscard]] const SctSectionFolder* findFolder(SctSectionFolderId id) const noexcept;
    [[nodiscard]] SctSectionFolderId nextFolderId() const noexcept;
    [[nodiscard]] SctStructuredAuthoringApplication apply(
        const SctStructuredAuthoringOperationBatch& batch);

private:
    std::vector<SctAuthoredArm> arms_{};
    std::vector<SctUnboundReferenceOrigin> unboundReferences_{};
    std::vector<SctVariableAlias> aliases_{};
    std::vector<SctEntityAnnotation> annotations_{};
    std::vector<SctSectionFolder> folders_{};
    std::uint64_t nextId_ = 1;
    std::uint64_t nextFolderId_ = 1;
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

enum class SctSemanticNodeKind : std::uint8_t {
    Section,
    BasicBlock,
    Instruction,
    Region,
    Arm,
    Placeholder,
    Issue,
};

// A projection-local identity. It is deterministic for one document revision,
// but is deliberately not persisted as document or authoring state.
struct SctSemanticNodeKey final {
    std::string value{};
    [[nodiscard]] bool valid() const noexcept { return !value.empty(); }
    auto operator<=>(const SctSemanticNodeKey&) const = default;
};

struct SctSemanticProjectionNode final {
    SctSemanticNodeKey key{};
    SctSemanticNodeKind kind = SctSemanticNodeKind::Instruction;
    std::optional<SctNavigationTarget> target{};
    std::optional<spice::sct::SctBasicBlockId> basicBlock{};
    std::optional<spice::sct::SctInstructionId> controller{};
    std::optional<spice::sct::SctInstructionId> joinInstruction{};
    std::optional<SctAuthoredArmId> authoredArm{};
    std::optional<spice::sct::SctStructuredRegionKind> regionKind{};
    std::optional<spice::sct::SctStructuredArmKind> armKind{};
    std::optional<std::int32_t> caseValue{};
    std::vector<std::int32_t> caseValues{};
    std::vector<std::uint32_t> caseGroupOrdinals{};
    std::optional<spice::sct::SctSemanticConfidence> confidence{};
    std::optional<spice::sct::SctStructureIssue> issue{};
    std::vector<spice::sct::SctStructureEvidence> evidence{};
    std::vector<spice::sct::SctInstructionId> physicalInstructions{};
    std::vector<spice::sct::SctInstructionId> managedScaffolding{};
    std::vector<SctSemanticProjectionNode> children{};
    SctSemanticArmStatus armStatus = SctSemanticArmStatus::Verified;
    std::size_t importedEvidenceCount = 0;
    bool verified = false;
    bool virtualArm = false;
    bool needsValue = false;
    bool canReturnToEmpty = false;
    bool hiddenByDefault = false;
    bool authorable = true;
};

enum class SctSemanticDestinationPlacement : std::uint8_t {
    Before,
    After,
    IntoStart,
    IntoEnd,
};

struct SctSemanticSelection final {
    RevisionId revision{};
    SctSemanticNodeKey parent{};
    std::vector<SctSemanticNodeKey> units{};
    auto operator<=>(const SctSemanticSelection&) const = default;
};

struct SctSemanticDestination final {
    RevisionId revision{};
    SctSemanticNodeKey node{};
    SctSemanticDestinationPlacement placement =
        SctSemanticDestinationPlacement::After;
    auto operator<=>(const SctSemanticDestination&) const = default;
};

enum class SctSemanticMoveDirection : std::uint8_t { Up, Down };

struct SctSemanticCommandPlan final {
    SctSemanticSelection selection{};
    std::vector<spice::sct::SctInstructionId> physicalInstructions{};
    std::optional<spice::sct::SctInstructionId> anchorAfter{};
    std::optional<SctAuthoredArmId> destinationAuthoredArm{};
    std::optional<spice::sct::SctInstructionId> destinationController{};
    std::optional<spice::sct::SctStructuredArmKind> destinationArmKind{};
    std::vector<SctAuthoredArmId> authoredArmsToRemove{};
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
    [[nodiscard]] std::span<const SctSemanticProjectionNode> roots() const noexcept {
        return roots_;
    }
    [[nodiscard]] const SctSemanticProjectionNode* find(
        const SctSemanticNodeKey& key) const noexcept;
    [[nodiscard]] const SctSemanticProjectionNode* find(
        SctNavigationTarget target) const noexcept;
    [[nodiscard]] RevisionId workingRevision() const noexcept {
        return workingRevision_;
    }
    [[nodiscard]] RevisionId verifiedRevision() const noexcept {
        return verifiedRevision_;
    }
    [[nodiscard]] bool current() const noexcept {
        return workingRevision_ == verifiedRevision_;
    }

private:
    std::vector<SctSemanticAuthoredArmProjection> authoredArms_{};
    std::vector<SctSemanticProjectionNode> roots_{};
    RevisionId workingRevision_{};
    RevisionId verifiedRevision_{};
};

class SctSemanticCommandPlanner final {
public:
    [[nodiscard]] static Result<SctSemanticSelection> normalizeSelection(
        const SctSemanticEditorProjection& projection,
        std::span<const SctSemanticNodeKey> nodes);
    [[nodiscard]] static Result<SctSemanticCommandPlan> planSelection(
        const SctSemanticEditorProjection& projection,
        const SctSemanticSelection& selection);
    [[nodiscard]] static Result<SctSemanticCommandPlan> planMove(
        const SctSemanticEditorProjection& projection,
        const SctWorkingState& workingState,
        const SctSemanticSelection& selection,
        SctSemanticMoveDirection direction);
    [[nodiscard]] static Result<SctSemanticCommandPlan> planMove(
        const SctSemanticEditorProjection& projection,
        const SctWorkingState& workingState,
        const SctSemanticSelection& selection,
        const SctSemanticDestination& destination);
};

}  // namespace salsa::core
