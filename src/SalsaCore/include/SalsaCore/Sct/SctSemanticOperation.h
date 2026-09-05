#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctMessageAuthoring.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctDocumentAnalysis.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace salsa::core {

struct SctInstructionPlacement final {
    spice::sct::SctSectionId section;
    std::optional<spice::sct::SctInstructionId> after{};
    auto operator<=>(const SctInstructionPlacement&) const = default;
};

struct SctSectionPlacement final {
    std::optional<spice::sct::SctSectionId> after{};
    auto operator<=>(const SctSectionPlacement&) const = default;
};

struct SctSupplementaryTextPlacement final {
    std::optional<spice::sct::SctSupplementaryTextId> after{};
    auto operator<=>(const SctSupplementaryTextPlacement&) const = default;
};

struct SctSectionStructuralChange final {
    spice::sct::SctSectionId section;
    std::optional<SctSectionPlacement> before{};
    std::optional<SctSectionPlacement> after{};
    std::optional<std::string> beforeName{};
    std::optional<std::string> afterName{};
    std::optional<spice::sct::SctDocumentSection> beforeValue{};
    std::optional<spice::sct::SctDocumentSection> afterValue{};
};

struct SctSupplementaryTextStructuralChange final {
    spice::sct::SctSupplementaryTextId entry;
    std::optional<SctSupplementaryTextPlacement> before{};
    std::optional<SctSupplementaryTextPlacement> after{};
    std::optional<spice::sct::SctDocumentSupplementaryText> beforeValue{};
    std::optional<spice::sct::SctDocumentSupplementaryText> afterValue{};
};

struct SctTextValueChange final {
    SctTextTarget target;
    spice::sct::SctTextValue beforeValue;
    spice::sct::SctTextValue afterValue;
};

struct SctTextRepairProvenance final {
    spice::sct::SctTextEncoding encoding{};
    std::optional<spice::sct::SctKnownTextConvention> knownConvention{};
    std::string sourceSha256{};
    auto operator<=>(const SctTextRepairProvenance&) const = default;
};

struct SctTextRepairRecord final {
    SctTextTarget target;
    SctTextRepairProvenance provenance;
    auto operator<=>(const SctTextRepairRecord&) const = default;
};

struct SctInstructionStructuralChange final {
    spice::sct::SctInstructionId instruction;
    std::optional<SctInstructionPlacement> before{};
    std::optional<SctInstructionPlacement> after{};
    std::optional<spice::sct::SctDocumentInstruction> beforeValue{};
    std::optional<spice::sct::SctDocumentInstruction> afterValue{};
    spice::sct::SctInstructionSemanticContribution beforeSemantics{};
    spice::sct::SctInstructionSemanticContribution afterSemantics{};
};

struct SctParameterValueChange final {
    spice::sct::SctParameterSite site;
    spice::sct::SctDocumentParameterValue beforeValue;
    spice::sct::SctDocumentParameterValue afterValue;
};

struct SctRepeatedGroupStructuralChange final {
    spice::sct::SctInstructionId instruction;
    std::optional<std::uint32_t> beforeOrdinal{};
    std::optional<std::uint32_t> afterOrdinal{};
    std::optional<spice::sct::SctDocumentRepeatedParameterGroup> beforeValue{};
    std::optional<spice::sct::SctDocumentRepeatedParameterGroup> afterValue{};
};

enum class SctDerivedAnalysisInvalidation : std::uint32_t {
    None = 0,
    StructuredControlFlow = 1u << 0u,
};

[[nodiscard]] constexpr SctDerivedAnalysisInvalidation operator|(
    const SctDerivedAnalysisInvalidation left,
    const SctDerivedAnalysisInvalidation right) noexcept {
    return static_cast<SctDerivedAnalysisInvalidation>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool hasInvalidation(
    const SctDerivedAnalysisInvalidation value,
    const SctDerivedAnalysisInvalidation flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0u;
}

struct SctEditChangeSet final {
    std::vector<SctSectionStructuralChange> sections{};
    std::vector<SctInstructionStructuralChange> instructions{};
    std::vector<SctParameterValueChange> parameters{};
    std::vector<SctRepeatedGroupStructuralChange> repeatedGroups{};
    std::vector<SctSupplementaryTextStructuralChange> supplementaryText{};
    std::vector<SctTextValueChange> textValues{};
    std::vector<SctNavigationTarget> modified{};
    std::vector<SctStructuredAuthoringChange> structuredAuthoring{};
    SctDerivedAnalysisInvalidation invalidations = SctDerivedAnalysisInvalidation::None;
    bool documentChanged = false;
};

struct SctInsertSectionAfterOperation final {
    std::optional<spice::sct::SctSectionId> anchor{};
    spice::sct::SctDocumentSection section;
};

struct SctDeleteSectionOperation final {
    spice::sct::SctSectionId section;
};

struct SctRelocateSectionAfterOperation final {
    spice::sct::SctSectionId section;
    std::optional<spice::sct::SctSectionId> anchor{};
};

struct SctRenameSectionOperation final {
    spice::sct::SctSectionId section;
    std::string nameBytes;
};

struct SctInsertInstructionAfterOperation final {
    spice::sct::SctInstructionId anchor;
    spice::sct::SctDocumentInstruction instruction;
};

struct SctDeleteInstructionOperation final {
    spice::sct::SctInstructionId instruction;
};

struct SctRelocateInstructionAfterOperation final {
    spice::sct::SctInstructionId instruction;
    spice::sct::SctInstructionId anchor;
};

struct SctReplaceTextValueOperation final {
    SctTextTarget target;
    spice::sct::SctTextValue value;
    bool updatesRepairProvenance = false;
    std::optional<SctTextRepairProvenance> repairProvenance{};
};

// Source-compatible name for existing message-only callers. New code should
// use SctReplaceTextValueOperation because the same primitive now owns every
// semantic SCT text variant.
using SctReplaceMessageOperation = SctReplaceTextValueOperation;

struct SctInsertSupplementaryTextAfterOperation final {
    std::optional<spice::sct::SctSupplementaryTextId> anchor{};
    spice::sct::SctDocumentSupplementaryText entry;
};

struct SctDeleteSupplementaryTextOperation final {
    spice::sct::SctSupplementaryTextId entry;
};

struct SctReplaceInstructionOperation final {
    spice::sct::SctInstructionId instruction;
    spice::sct::SctDocumentInstruction replacement;
};

struct SctReplaceParameterValueOperation final {
    spice::sct::SctParameterSite site;
    spice::sct::SctDocumentParameterValue value;
};

struct SctInsertRepeatedGroupOperation final {
    spice::sct::SctInstructionId instruction;
    std::uint32_t ordinal = 0;
    spice::sct::SctDocumentRepeatedParameterGroup group;
};

struct SctDeleteRepeatedGroupOperation final {
    spice::sct::SctInstructionId instruction;
    std::uint32_t ordinal = 0;
};

struct SctRelocateRepeatedGroupOperation final {
    spice::sct::SctInstructionId instruction;
    std::uint32_t fromOrdinal = 0;
    std::uint32_t toOrdinal = 0;
};

using SctPrimitiveOperation = std::variant<
    SctInsertSectionAfterOperation,
    SctDeleteSectionOperation,
    SctRelocateSectionAfterOperation,
    SctRenameSectionOperation,
    SctInsertInstructionAfterOperation,
    SctDeleteInstructionOperation,
    SctRelocateInstructionAfterOperation,
    SctReplaceInstructionOperation,
    SctReplaceParameterValueOperation,
    SctInsertRepeatedGroupOperation,
    SctDeleteRepeatedGroupOperation,
    SctRelocateRepeatedGroupOperation,
    SctReplaceTextValueOperation,
    SctInsertSupplementaryTextAfterOperation,
    SctDeleteSupplementaryTextOperation>;

struct SctSemanticOperationBatch final {
    std::vector<SctPrimitiveOperation> operations{};
};

struct SctOperationIssue final {
    std::string code{};
    std::string message{};
    std::optional<SctNavigationTarget> target{};
};

struct SctOperationApplication final {
    std::shared_ptr<const spice::sct::SctDocument> document{};
    SctSemanticOperationBatch inverse{};
    SctEditChangeSet forwardChanges{};
    SctEditChangeSet reverseChanges{};
    std::vector<SctOperationIssue> issues{};

    [[nodiscard]] bool succeeded() const noexcept {
        return document != nullptr && issues.empty();
    }
};

struct SctOperationReplay final {
    SctSemanticOperationBatch inverse{};
    SctEditChangeSet forwardChanges{};
    SctEditChangeSet reverseChanges{};
    std::vector<SctOperationIssue> issues{};

    [[nodiscard]] bool succeeded() const noexcept { return issues.empty(); }
};

class SctSemanticOperationService final {
public:
    [[nodiscard]] static SctOperationApplication apply(
        const spice::sct::SctDocument& document,
        const SctSemanticOperationBatch& batch);
    [[nodiscard]] static SctOperationReplay applyInPlace(
        spice::sct::SctDocument& document,
        const SctSemanticOperationBatch& batch);
};

}  // namespace salsa::core
