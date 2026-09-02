#include "SalsaCore/Sct/SctDocumentMaterializer.h"

#include <chrono>

namespace salsa::core {

SctMaterializationResult SctDocumentMaterializer::materialize(
    const SctMaterializationRequest& request,
    const std::stop_token stopToken) {
    SctMaterializationResult result;
    result.generation = request.generation;
    result.baseRevision = request.baseRevision;
    result.targetRevision = request.targetRevision;
    if (request.baseDocument == nullptr || stopToken.stop_requested()) {
        result.cancelled = stopToken.stop_requested();
        return result;
    }

    const auto replayStart = std::chrono::steady_clock::now();
    auto document = std::make_shared<spice::sct::SctDocument>(*request.baseDocument);
    for (const auto& batch : request.journalTail) {
        if (stopToken.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        auto applied = SctSemanticOperationService::applyInPlace(*document, batch);
        if (!applied.succeeded()) {
            result.operationIssues = std::move(applied.issues);
            return result;
        }
    }
    result.timings.replayMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - replayStart).count());
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    const auto validationStart = std::chrono::steady_clock::now();
    result.validation = spice::sct::SctDocumentValidator::validateDocument(*document);
    for (const auto& diagnostic : result.validation.diagnostics) {
        result.diagnostics.push_back(convertSctDiagnostic(
            diagnostic, SctPipelineStage::Validation, request.locator));
    }
    result.timings.validationMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - validationStart).count());
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    const auto analysisStart = std::chrono::steady_clock::now();
    result.analysis = std::make_shared<const spice::sct::SctDocumentAnalysis>(
        spice::sct::SctDocumentAnalysis::build(*document,
            request.importEvidence ? &*request.importEvidence : nullptr));
    result.timings.analysisMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - analysisStart).count());
    result.document = std::move(document);
    return result;
}

}  // namespace salsa::core
