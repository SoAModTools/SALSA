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

struct SctInstructionStructuralChange final {
    spice::sct::SctInstructionId instruction;
    std::optional<SctInstructionPlacement> before{};
    std::optional<SctInstructionPlacement> after{};
    std::optional<spice::sct::SctDocumentInstruction> beforeValue{};
    std::optional<spice::sct::SctDocumentInstruction> afterValue{};
    spice::sct::SctInstructionSemanticContribution beforeSemantics{};
    spice::sct::SctInstructionSemanticContribution afterSemantics{};
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
    std::vector<SctInstructionStructuralChange> instructions{};
    std::vector<SctNavigationTarget> modified{};
    std::vector<SctStructuredAuthoringChange> structuredAuthoring{};
    SctDerivedAnalysisInvalidation invalidations = SctDerivedAnalysisInvalidation::None;
    bool documentChanged = false;
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

struct SctReplaceMessageOperation final {
    SctMessageTarget target;
    spice::sct::SctMessage message;
};

struct SctReplaceInstructionOperation final {
    spice::sct::SctInstructionId instruction;
    spice::sct::SctDocumentInstruction replacement;
};

using SctPrimitiveOperation = std::variant<
    SctInsertInstructionAfterOperation,
    SctDeleteInstructionOperation,
    SctRelocateInstructionAfterOperation,
    SctReplaceInstructionOperation,
    SctReplaceMessageOperation>;

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
