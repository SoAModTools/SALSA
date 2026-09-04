#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Persistence/SctScriptPatch.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctDocumentMaterializer.h"
#include "SalsaCore/Sct/SctFragment.h"
#include "SalsaCore/Sct/SctMessageAuthoring.h"
#include "SalsaCore/Sct/SctPublication.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SalsaCore/Sct/SctWorkingState.h"

#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

enum class SctInstructionMoveDirection { Up, Down };
enum class SctSectionMoveDirection { Up, Down };
enum class SctCreatedFooterTextKind { Message, PlainText };
enum class SctRepeatedGroupMoveDirection { Up, Down };

enum class SctRevisionTransitionKind {
    Commit,
    Undo,
    Redo,
    VerificationRollback,
};

enum class SctRevisionVerification {
    Pending,
    Verified,
    Rejected,
};

struct SctWorkingTransition final {
    SctRevisionTransitionKind kind = SctRevisionTransitionKind::Commit;
    RevisionId from{};
    RevisionId to{};
    SctEditChangeSet changes{};
    std::optional<SctNavigationTarget> suggestedSelection{};
    SctRevisionVerification verification = SctRevisionVerification::Pending;
    std::vector<SctNavigationTarget> suggestedSelectionRange{};
};

using SctRevisionTransition = SctWorkingTransition;

struct SctEditResult final {
    bool committed = false;
    RevisionId revision{};
    std::shared_ptr<const SctDocumentSnapshot> snapshot{};
    SctEditChangeSet changes{};
    std::optional<SctRevisionTransition> transition{};
    std::optional<SctNavigationTarget> suggestedSelection{};
    std::vector<SctNavigationTarget> suggestedSelectionRange{};
    std::vector<SctPipelineDiagnostic> diagnostics{};
    std::uint64_t preflightMicroseconds = 0;
    std::uint64_t journalMicroseconds = 0;
};

struct SctInsertableOpcode final {
    std::uint16_t opcode = 0;
    std::string mnemonic{};
    auto operator<=>(const SctInsertableOpcode&) const = default;
};

struct SctOwnedFooterTextDraft final {
    spice::sct::SctParameterAddress parameter;
    spice::sct::SctTextKind kind = spice::sct::SctTextKind::PlainString;
    spice::sct::SctTextValue value = spice::sct::SctPlainText{};
};

struct SctInstructionAuthoringDraft final {
    RevisionId baseRevision{};
    spice::sct::SctInstructionDraft instruction;
    std::vector<SctOwnedFooterTextDraft> ownedFooterText;
};

struct SctInstructionAuthoringDraftResult final {
    std::optional<SctInstructionAuthoringDraft> draft;
    std::vector<SctPipelineDiagnostic> diagnostics;
};

class SctEditSession final {
public:
    explicit SctEditSession(std::shared_ptr<const SctDocumentSnapshot> initialSnapshot);
    SctEditSession(
        std::shared_ptr<const SctDocumentSnapshot> baselineSnapshot,
        std::shared_ptr<const SctDocumentSnapshot> restoredSnapshot,
        std::span<const SctAuthoredArm> authoredArms,
        std::span<const SctPatchedTextRepair> textRepairs,
        std::span<const SctUnboundReferenceOrigin> unboundReferences = {});

    SctEditSession(const SctEditSession&) = delete;
    SctEditSession& operator=(const SctEditSession&) = delete;
    SctEditSession(SctEditSession&&) noexcept = default;
    SctEditSession& operator=(SctEditSession&&) noexcept = default;

    [[nodiscard]] static std::unique_ptr<SctEditSession> createRebased(
        std::shared_ptr<const SctDocumentSnapshot> newBaselineSnapshot,
        std::shared_ptr<const SctDocumentSnapshot> rebasedSnapshot,
        std::span<const SctAuthoredArm> authoredArms,
        std::span<const SctPatchedTextRepair> textRepairs,
        std::span<const SctUnboundReferenceOrigin> unboundReferences = {});

    [[nodiscard]] SctEditResult insertInstructionAfter(
        spice::sct::SctInstructionId anchorInstruction,
        std::uint16_t opcode);
    [[nodiscard]] SctInstructionAuthoringDraftResult createInstructionDraft(
        std::uint16_t opcode) const;
    [[nodiscard]] SctEditResult createInstructionAfter(
        spice::sct::SctInstructionId anchorInstruction,
        SctInstructionAuthoringDraft draft);
    [[nodiscard]] SctEditResult deleteInstruction(
        spice::sct::SctInstructionId instruction);
    [[nodiscard]] SctEditResult moveInstruction(
        spice::sct::SctInstructionId instruction,
        SctInstructionMoveDirection direction);
    [[nodiscard]] Result<SctSemanticFragment> captureInstructions(
        std::span<const spice::sct::SctInstructionId> instructions) const;
    [[nodiscard]] Result<SctSemanticFragment> captureSections(
        std::span<const spice::sct::SctSectionId> sections) const;
    [[nodiscard]] SctEditResult pasteFragment(
        const SctSemanticFragment& fragment,
        SctFragmentPasteDestination destination);
    [[nodiscard]] SctEditResult deleteInstructions(
        std::span<const spice::sct::SctInstructionId> instructions);
    [[nodiscard]] SctEditResult deleteSections(
        std::span<const spice::sct::SctSectionId> sections);
    [[nodiscard]] SctEditResult moveInstructionsAfter(
        std::span<const spice::sct::SctInstructionId> instructions,
        spice::sct::SctInstructionId anchor);
    [[nodiscard]] SctEditResult replaceMessage(
        const SctMessageTarget& target,
        const SctMessageDraft& draft,
        SctMessageEditKind editKind);
    [[nodiscard]] SctEditResult replacePlainText(
        const SctTextTarget& target, std::string utf8);
    [[nodiscard]] SctEditResult replaceTextValue(
        const SctTextTarget& target, spice::sct::SctTextValue value,
        std::string description = "Repair text interpretation",
        std::optional<SctTextRepairProvenance> repairProvenance = std::nullopt);
    [[nodiscard]] SctEditResult replaceParameterValue(
        const spice::sct::SctParameterSite& site,
        spice::sct::SctDocumentParameterValue value);
    [[nodiscard]] SctEditResult editParameterText(
        const spice::sct::SctParameterSite& site, std::string text);
    [[nodiscard]] SctEditResult insertRepeatedGroup(
        spice::sct::SctInstructionId instruction, std::uint32_t ordinal,
        spice::sct::SctDocumentRepeatedParameterGroup group);
    [[nodiscard]] SctEditResult deleteRepeatedGroup(
        spice::sct::SctInstructionId instruction, std::uint32_t ordinal);
    [[nodiscard]] SctEditResult moveRepeatedGroup(
        spice::sct::SctInstructionId instruction, std::uint32_t ordinal,
        SctRepeatedGroupMoveDirection direction);
    [[nodiscard]] SctEditResult editReferencedFooterText(
        const spice::sct::SctParameterSite& site, std::string utf8);
    [[nodiscard]] SctEditResult createScriptSection(
        std::string name, std::optional<spice::sct::SctSectionId> after,
        bool includeReturn = true);
    [[nodiscard]] SctEditResult createIndexedString(
        std::string name, std::optional<spice::sct::SctSectionId> after);
    [[nodiscard]] SctEditResult renameSection(
        spice::sct::SctSectionId section, std::string name);
    [[nodiscard]] SctEditResult deleteSection(spice::sct::SctSectionId section);
    [[nodiscard]] SctEditResult moveSection(
        spice::sct::SctSectionId section, SctSectionMoveDirection direction);
    [[nodiscard]] SctEditResult createFooterText(
        SctCreatedFooterTextKind kind,
        std::optional<spice::sct::SctFooterEntryId> after);
    [[nodiscard]] SctEditResult deleteTextEntity(const SctTextTarget& target);
    [[nodiscard]] SctEditResult addVirtualElse(
        spice::sct::SctInstructionId controller);
    [[nodiscard]] SctEditResult addVirtualCase(
        spice::sct::SctInstructionId controller);
    [[nodiscard]] SctEditResult setVirtualCaseValue(
        SctAuthoredArmId arm, std::optional<std::int32_t> value);
    [[nodiscard]] SctEditResult removeVirtualArm(SctAuthoredArmId arm);
    [[nodiscard]] SctEditResult insertInstructionIntoAuthoredArm(
        SctAuthoredArmId arm, std::uint16_t opcode);
    [[nodiscard]] SctEditResult deleteOnlyInstructionFromAuthoredArm(
        SctAuthoredArmId arm, spice::sct::SctInstructionId instruction);
    [[nodiscard]] SctEditResult insertInstructionIntoStructuredArm(
        spice::sct::SctInstructionId controller,
        spice::sct::SctStructuredArmKind arm,
        std::uint16_t opcode);

    [[nodiscard]] std::optional<SctEditResult> undo();
    [[nodiscard]] std::optional<SctEditResult> redo();

    [[nodiscard]] std::shared_ptr<const SctDocumentSnapshot> currentSnapshot() const noexcept;
    [[nodiscard]] std::shared_ptr<const SctDocumentSnapshot> verifiedSnapshot() const noexcept;
    [[nodiscard]] RevisionId workingRevision() const;
    [[nodiscard]] RevisionId currentRevision() const;
    [[nodiscard]] bool structurallyValid() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool isDirty() const noexcept;
    [[nodiscard]] std::vector<SctPipelineDiagnostic> currentDiagnostics() const;
    [[nodiscard]] std::optional<std::string_view> undoDescription() const noexcept;
    [[nodiscard]] std::optional<std::string_view> redoDescription() const noexcept;
    [[nodiscard]] std::optional<std::shared_ptr<const spice::sct::SctDocument>>
        materializeRevision(RevisionId revision) const;
    [[nodiscard]] std::optional<SctMaterializationRequest> materializationRequest(
        std::uint64_t generation) const;
    [[nodiscard]] bool installVerifiedMaterialization(
        const SctMaterializationResult& result);
    [[nodiscard]] std::optional<SctEditResult> rejectToVerifiedRevision(
        RevisionId revision,
        std::vector<SctPipelineDiagnostic> diagnostics);
    [[nodiscard]] bool isActiveRevision(RevisionId revision) const;
    [[nodiscard]] std::optional<SctCheckpointRequest> checkpointRequest(
        std::uint64_t generation) const;
    [[nodiscard]] std::optional<SctPublicationRevision> capturePublicationRevision(
        std::uint64_t generation) const;
    [[nodiscard]] bool markPatchCheckpoint(
        RevisionId revision, std::shared_ptr<const void> historyStateToken) noexcept;

    [[nodiscard]] const SctWorkingState& workingState() const noexcept;
    [[nodiscard]] const SctStructuredAuthoringState& structuredAuthoring() const noexcept;
    [[nodiscard]] std::span<const SctUnboundReferenceOrigin>
        unboundReferences() const noexcept;
    [[nodiscard]] std::shared_ptr<const SctSemanticEditorProjection>
        semanticProjection() const noexcept;

    [[nodiscard]] static const std::vector<SctInsertableOpcode>& insertableOpcodes();
    [[nodiscard]] static const std::vector<SctInsertableOpcode>& authorableOpcodes();

private:
    struct SelectionHints final {
        std::optional<SctNavigationTarget> undoSelection{};
        std::optional<SctNavigationTarget> redoSelection{};
        std::vector<SctNavigationTarget> undoSelectionRange{};
        std::vector<SctNavigationTarget> redoSelectionRange{};
    };

    struct RevisionDelta final {
        struct ExternalState final {
            std::shared_ptr<const SctDocumentSnapshot> snapshot{};
            std::vector<SctAuthoredArm> authoredArms{};
            std::vector<SctPatchedTextRepair> textRepairs{};
            std::vector<SctUnboundReferenceOrigin> unboundReferences{};
        };
        RevisionId parent{};
        SctSemanticOperationBatch forward{};
        SctSemanticOperationBatch inverse{};
        SctStructuredAuthoringOperationBatch authoringForward{};
        SctStructuredAuthoringOperationBatch authoringInverse{};
        SctEditChangeSet forwardChanges{};
        SctEditChangeSet reverseChanges{};
        SelectionHints selections{};
        std::optional<ExternalState> externalBefore{};
        std::optional<ExternalState> externalAfter{};
    };

    struct MaterializationCheckpoint final {
        RevisionId revision{};
        std::shared_ptr<const SctDocumentSnapshot> snapshot{};
    };

    [[nodiscard]] SctEditResult failure(std::vector<SctPipelineDiagnostic> diagnostics) const;
    void appendOrphanedFooterPlainTextCleanup(
        SctSemanticOperationBatch& operation) const;
    [[nodiscard]] SctEditResult commit(
        SctSemanticOperationBatch operation,
        SctStructuredAuthoringOperationBatch authoringOperation,
        std::string description,
        SelectionHints selections,
        std::uint64_t preflightMicroseconds = 0);
    void pruneMaterializationCheckpoints();
    void rebuildSemanticProjection();
    void installExternalState(const RevisionDelta::ExternalState& state);

    std::shared_ptr<const SctDocumentSnapshot> baselineSnapshot_;
    RevisionHistory<RevisionDelta> history_;
    SctWorkingState workingState_;
    SctStructuredAuthoringState structuredAuthoring_{};
    std::shared_ptr<const SctSemanticEditorProjection> semanticProjection_{};
    std::shared_ptr<const spice::sct::SctDocument> materializedDocument_;
    std::shared_ptr<const SctDocumentSnapshot> currentSnapshot_;
    bool structurallyValid_ = false;
    std::vector<MaterializationCheckpoint> materializationCheckpoints_{};
    RevisionId verifiedRevision_{1};
    std::vector<RevisionDelta> rejectedTail_{};
};

}  // namespace salsa::core
