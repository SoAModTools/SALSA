#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctDocumentMaterializer.h"
#include "SalsaCore/Sct/SctMessageAuthoring.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SalsaCore/Sct/SctWorkingState.h"

#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctDocumentValidator.h"

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
};

using SctRevisionTransition = SctWorkingTransition;

struct SctEditResult final {
    bool committed = false;
    RevisionId revision{};
    std::shared_ptr<const SctDocumentSnapshot> snapshot{};
    SctEditChangeSet changes{};
    std::optional<SctRevisionTransition> transition{};
    std::optional<SctNavigationTarget> suggestedSelection{};
    std::vector<SctPipelineDiagnostic> diagnostics{};
    std::uint64_t preflightMicroseconds = 0;
    std::uint64_t journalMicroseconds = 0;
};

struct SctInsertableOpcode final {
    std::uint16_t opcode = 0;
    std::string mnemonic{};
    auto operator<=>(const SctInsertableOpcode&) const = default;
};

class SctEditSession final {
public:
    explicit SctEditSession(std::shared_ptr<const SctDocumentSnapshot> initialSnapshot);

    SctEditSession(const SctEditSession&) = delete;
    SctEditSession& operator=(const SctEditSession&) = delete;
    SctEditSession(SctEditSession&&) noexcept = default;
    SctEditSession& operator=(SctEditSession&&) noexcept = default;

    [[nodiscard]] SctEditResult insertInstructionAfter(
        spice::sct::SctInstructionId anchorInstruction,
        std::uint16_t opcode);
    [[nodiscard]] SctEditResult deleteInstruction(
        spice::sct::SctInstructionId instruction);
    [[nodiscard]] SctEditResult moveInstruction(
        spice::sct::SctInstructionId instruction,
        SctInstructionMoveDirection direction);
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
        spice_sct_prototype::SctStructuredArmKind arm,
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

    [[nodiscard]] const SctWorkingState& workingState() const noexcept;
    [[nodiscard]] const SctStructuredAuthoringState& structuredAuthoring() const noexcept;
    [[nodiscard]] std::shared_ptr<const SctSemanticEditorProjection>
        semanticProjection() const noexcept;

    [[nodiscard]] static const std::vector<SctInsertableOpcode>& insertableOpcodes();

private:
    struct SelectionHints final {
        std::optional<SctNavigationTarget> undoSelection{};
        std::optional<SctNavigationTarget> redoSelection{};
    };

    struct RevisionDelta final {
        RevisionId parent{};
        SctSemanticOperationBatch forward{};
        SctSemanticOperationBatch inverse{};
        SctStructuredAuthoringOperationBatch authoringForward{};
        SctStructuredAuthoringOperationBatch authoringInverse{};
        SctEditChangeSet forwardChanges{};
        SctEditChangeSet reverseChanges{};
        SelectionHints selections{};
    };

    struct MaterializationCheckpoint final {
        RevisionId revision{};
        std::shared_ptr<const SctDocumentSnapshot> snapshot{};
    };

    [[nodiscard]] SctEditResult failure(std::vector<SctPipelineDiagnostic> diagnostics) const;
    [[nodiscard]] SctEditResult commit(
        SctSemanticOperationBatch operation,
        SctStructuredAuthoringOperationBatch authoringOperation,
        std::string description,
        SelectionHints selections,
        std::uint64_t preflightMicroseconds = 0);
    void pruneMaterializationCheckpoints();
    void rebuildSemanticProjection();

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
