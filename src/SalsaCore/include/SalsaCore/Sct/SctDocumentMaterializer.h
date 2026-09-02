#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SalsaCore/Sct/SctSemanticUsageIndex.h"

#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctDocumentIndex.h"

#include <cstdint>
#include <memory>
#include <stop_token>
#include <vector>

namespace salsa::core {

struct SctMaterializationTimings final {
    std::uint64_t replayMicroseconds = 0;
    std::uint64_t validationMicroseconds = 0;
    std::uint64_t semanticAuditMicroseconds = 0;
    std::uint64_t documentIndexMicroseconds = 0;
};

struct SctMaterializationRequest final {
    std::uint64_t generation = 0;
    RevisionId baseRevision{};
    RevisionId targetRevision{};
    std::shared_ptr<const spice::sct::SctDocument> baseDocument{};
    std::vector<SctSemanticOperationBatch> journalTail{};
};

struct SctMaterializationResult final {
    std::uint64_t generation = 0;
    RevisionId baseRevision{};
    RevisionId targetRevision{};
    std::shared_ptr<const spice::sct::SctDocument> document{};
    spice::sct::SctDocumentValidationResult validation{};
    SctSemanticUsageIndex semanticIndex{};
    std::shared_ptr<const spice::sct::SctDocumentIndex> documentIndex{};
    std::vector<SctPipelineDiagnostic> diagnostics{};
    std::vector<SctOperationIssue> operationIssues{};
    bool cancelled = false;
    SctMaterializationTimings timings{};

    [[nodiscard]] bool succeeded() const noexcept {
        return document != nullptr && operationIssues.empty()
            && validation.validDocument && !cancelled;
    }
};

class SctDocumentMaterializer final {
public:
    [[nodiscard]] static SctMaterializationResult materialize(
        const SctMaterializationRequest& request,
        std::stop_token stopToken = {});
};

}  // namespace salsa::core
