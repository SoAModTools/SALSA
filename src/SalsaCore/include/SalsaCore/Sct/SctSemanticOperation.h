#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctMessageAuthoring.h"

#include "SpiceSCT/SctDocument.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace salsa::core {

struct SctEditChangeSet final {
    std::vector<SctNavigationTarget> created{};
    std::vector<SctNavigationTarget> removed{};
    std::vector<SctNavigationTarget> moved{};
    std::vector<SctNavigationTarget> modified{};
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

using SctPrimitiveOperation = std::variant<
    SctInsertInstructionAfterOperation,
    SctDeleteInstructionOperation,
    SctRelocateInstructionAfterOperation,
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

class SctSemanticOperationService final {
public:
    [[nodiscard]] static SctOperationApplication apply(
        const spice::sct::SctDocument& document,
        const SctSemanticOperationBatch& batch);
};

}  // namespace salsa::core
