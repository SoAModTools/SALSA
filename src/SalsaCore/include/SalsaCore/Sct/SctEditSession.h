#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctMessageAuthoring.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"

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

enum class SctRevisionTransitionKind {
    Commit,
    Undo,
    Redo,
};

struct SctRevisionTransition final {
    SctRevisionTransitionKind kind = SctRevisionTransitionKind::Commit;
    RevisionId from{};
    RevisionId to{};
    SctEditChangeSet changes{};
};

struct SctEditResult final {
    bool committed = false;
    RevisionId revision{};
    std::shared_ptr<const SctDocumentSnapshot> snapshot{};
    SctEditChangeSet changes{};
    std::optional<SctRevisionTransition> transition{};
    std::optional<SctNavigationTarget> suggestedSelection{};
    std::vector<SctPipelineDiagnostic> diagnostics{};
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

    [[nodiscard]] std::optional<SctEditResult> undo();
    [[nodiscard]] std::optional<SctEditResult> redo();

    [[nodiscard]] std::shared_ptr<const SctDocumentSnapshot> currentSnapshot() const noexcept;
    [[nodiscard]] RevisionId currentRevision() const;
    [[nodiscard]] bool structurallyValid() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool isDirty() const noexcept;
    [[nodiscard]] std::optional<std::string_view> undoDescription() const noexcept;
    [[nodiscard]] std::optional<std::string_view> redoDescription() const noexcept;
    [[nodiscard]] std::optional<std::shared_ptr<const spice::sct::SctDocument>>
        materializeRevision(RevisionId revision) const;

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
        SctEditChangeSet forwardChanges{};
        SctEditChangeSet reverseChanges{};
        SelectionHints selections{};
    };

    struct MaterializationCheckpoint final {
        RevisionId revision{};
        std::shared_ptr<const spice::sct::SctDocument> document{};
    };

    [[nodiscard]] SctEditResult failure(std::vector<SctPipelineDiagnostic> diagnostics) const;
    [[nodiscard]] SctEditResult commit(
        SctSemanticOperationBatch operation,
        std::string description,
        SelectionHints selections);
    void rebuildSnapshot(
        std::shared_ptr<const spice::sct::SctDocument> document,
        const spice::sct::SctDocumentValidationResult& validation);
    void pruneMaterializationCheckpoints();
    void maybeAddMaterializationCheckpoint();

    std::shared_ptr<const SctDocumentSnapshot> baselineSnapshot_;
    RevisionHistory<RevisionDelta> history_;
    std::shared_ptr<const spice::sct::SctDocument> materializedDocument_;
    std::shared_ptr<const SctDocumentSnapshot> currentSnapshot_;
    bool structurallyValid_ = false;
    std::vector<MaterializationCheckpoint> materializationCheckpoints_{};
    std::size_t commitsSinceMaterializationCheckpoint_ = 0;
};

}  // namespace salsa::core
