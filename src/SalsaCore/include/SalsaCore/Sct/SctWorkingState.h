#pragma once

#include "SalsaCore/Sct/SctSemanticOperation.h"

#include "SpiceSCT/SctDocument.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace salsa::core {

struct SctWorkingApplication final {
    SctSemanticOperationBatch inverse{};
    SctEditChangeSet forwardChanges{};
    SctEditChangeSet reverseChanges{};
    std::vector<SctOperationIssue> issues{};

    [[nodiscard]] bool succeeded() const noexcept { return issues.empty(); }
};

// A document-local, Qt-free projection optimized for bounded semantic edits.
// It is authoritative for the live editor while a complete SctDocument is
// materialized and verified asynchronously.
class SctWorkingState final {
public:
    explicit SctWorkingState(
        std::shared_ptr<const spice::sct::SctDocument> checkpoint,
        std::span<const SctTextRepairRecord> repairs = {});

    [[nodiscard]] const spice::sct::SctDocumentInstruction* instruction(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] const spice::sct::SctDocumentParameter* parameter(
        const spice::sct::SctParameterSite& site) const noexcept;
    [[nodiscard]] const spice::sct::SctDocumentSection* section(
        spice::sct::SctSectionId id) const noexcept;
    [[nodiscard]] std::span<const spice::sct::SctSectionId> sectionOrder() const noexcept;
    [[nodiscard]] std::optional<SctSectionPlacement> sectionPlacement(
        spice::sct::SctSectionId id) const noexcept;
    [[nodiscard]] std::optional<SctInstructionPlacement> placement(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] std::span<const spice::sct::SctInstructionId> instructionOrder(
        spice::sct::SctSectionId section) const noexcept;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId> instructionBefore(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId> instructionAfter(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] const spice::sct::SctInstructionSemanticContribution* contribution(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] std::size_t incomingReferenceCount(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] std::span<const spice::sct::SctOpaqueAttachmentId>
        opaqueAttachments(spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] const spice::sct::SctTextValue* textValue(
        const SctTextTarget& target) const noexcept;
    [[nodiscard]] std::optional<spice::sct::SctTextKind> stringKind(
        spice::sct::SctStringId id) const noexcept;
    [[nodiscard]] std::optional<std::string_view> stringSectionName(
        spice::sct::SctStringId id) const noexcept;
    [[nodiscard]] std::optional<SctTextRepairProvenance> textRepairProvenance(
        const SctTextTarget& target) const;
    [[nodiscard]] std::vector<SctTextRepairRecord> textRepairProvenances() const;
    [[nodiscard]] const spice::sct::SctMessage* message(
        const SctMessageTarget& target) const noexcept;
    [[nodiscard]] const spice::sct::SctDocumentSupplementaryText* supplementaryText(
        spice::sct::SctSupplementaryTextId id) const noexcept;
    [[nodiscard]] std::span<const spice::sct::SctSupplementaryTextId> supplementaryTextOrder() const noexcept;
    [[nodiscard]] std::vector<spice::sct::SctInstructionId> inboundReferenceSources(
        const spice::sct::SctDocumentReferenceTarget& target) const;
    [[nodiscard]] std::size_t referenceOccurrenceCount(
        const spice::sct::SctDocumentReferenceTarget& target) const noexcept;
    [[nodiscard]] std::vector<spice::sct::SctOpaqueAttachmentId> opaqueAttachments(
        const spice::sct::SctOpaqueAnchor& anchor) const;
    [[nodiscard]] std::uint64_t nextSectionIdValue() const noexcept;
    [[nodiscard]] std::uint64_t nextInstructionIdValue() const noexcept;
    [[nodiscard]] std::uint64_t nextStringIdValue() const noexcept;
    [[nodiscard]] std::uint64_t nextSupplementaryTextIdValue() const noexcept;

    [[nodiscard]] SctWorkingApplication apply(
        const SctSemanticOperationBatch& batch);

private:
    struct InstructionEntry final {
        spice::sct::SctDocumentInstruction value{};
        spice::sct::SctSectionId section{};
        spice::sct::SctInstructionSemanticContribution semantics{};
    };

    [[nodiscard]] std::optional<SctOperationIssue> applyPrimitive(
        const SctPrimitiveOperation& operation,
        SctPrimitiveOperation& inverse,
        SctEditChangeSet& forward,
        SctEditChangeSet& reverse);
    void addContribution(const spice::sct::SctInstructionSemanticContribution& contribution);
    void removeContribution(const spice::sct::SctInstructionSemanticContribution& contribution);

    std::shared_ptr<const spice::sct::SctDocument> checkpoint_{};
    std::unordered_map<spice::sct::SctSectionId, spice::sct::SctDocumentSection> sections_{};
    std::vector<spice::sct::SctSectionId> physicalSectionOrder_{};
    std::unordered_map<spice::sct::SctInstructionId, InstructionEntry> instructions_{};
    std::unordered_map<spice::sct::SctSectionId,
        std::vector<spice::sct::SctInstructionId>> sectionOrder_{};
    std::unordered_map<spice::sct::SctInstructionId, std::size_t> incomingReferences_{};
    std::unordered_map<spice::sct::SctInstructionId,
        std::vector<spice::sct::SctOpaqueAttachmentId>> opaqueAttachments_{};
    std::unordered_map<spice::sct::SctStringId, spice::sct::SctTextValue> stringValues_{};
    std::unordered_map<spice::sct::SctStringId, spice::sct::SctTextKind> stringKinds_{};
    std::unordered_map<spice::sct::SctSupplementaryTextId, spice::sct::SctDocumentSupplementaryText> supplementaryText_{};
    std::vector<spice::sct::SctSupplementaryTextId> supplementaryTextOrder_{};
    std::unordered_map<std::string, SctTextRepairProvenance> textRepairProvenance_{};
    std::uint64_t nextSectionId_ = 1;
    std::uint64_t nextInstructionId_ = 1;
    std::uint64_t nextStringId_ = 1;
    std::uint64_t nextSupplementaryTextId_ = 1;
};

}  // namespace salsa::core
