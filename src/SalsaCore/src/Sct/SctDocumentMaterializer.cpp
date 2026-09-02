#include "SalsaCore/Sct/SctDocumentMaterializer.h"

#include <chrono>
#include <ranges>

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
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    const auto structureStart = std::chrono::steady_clock::now();
    result.structuredControlFlow = std::make_shared<
        const spice_sct_prototype::SctStructuredControlFlowAnalysis>(
        spice_sct_prototype::SctStructuredControlFlowAnalysis::build(
            *document, *result.analysis));
    result.timings.structureAnalysisMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - structureStart).count());
    for (const auto& expected : request.expectedStructuredArms) {
        if (expected.realization != SctAuthoredArmRealization::Physical) continue;
        const auto* section = result.structuredControlFlow->findSection(
            expected.controller.section);
        const auto region = section == nullptr ? nullptr : [&]()
            -> const spice_sct_prototype::SctStructuredRegion* {
            const auto found = std::ranges::find_if(section->regions, [&](const auto& item) {
                return item.id.headerInstruction == expected.controller.instruction
                    && item.strength
                        == spice_sct_prototype::SctStructureClaimStrength::Verified;
            });
            return found == section->regions.end() ? nullptr : &*found;
        }();
        const auto arm = region == nullptr ? nullptr : [&]()
            -> const spice_sct_prototype::SctStructuredArm* {
            const auto found = std::ranges::find_if(region->arms, [&](const auto& item) {
                if (item.kind != expected.kind) return false;
                if (expected.kind
                    != spice_sct_prototype::SctStructuredArmKind::SwitchCase) return true;
                return expected.caseValue && std::ranges::any_of(item.caseLabels,
                    [&](const auto& label) { return label.value == expected.caseValue; });
            });
            return found == region->arms.end() ? nullptr : &*found;
        }();
        bool membersPresent = arm != nullptr;
        if (membersPresent) {
            for (const auto member : expected.members) {
                const bool inArm = std::ranges::any_of(arm->blocks, [&](const auto blockId) {
                    const auto block = std::ranges::find(
                        section->blocks, blockId, &spice_sct_prototype::SctBasicBlock::id);
                    return block != section->blocks.end()
                        && std::ranges::find(block->instructions, member)
                            != block->instructions.end();
                });
                if (!inArm) {
                    membersPresent = false;
                    break;
                }
            }
        }
        const bool joinMatches = region != nullptr && expected.expectedJoin
            && region->join
            && region->join->entryInstruction == *expected.expectedJoin;
        if (region == nullptr || arm == nullptr || !membersPresent || !joinMatches) {
            result.operationIssues.push_back({"StructuredPostconditionFailed",
                "The materialized document did not preserve the authored semantic arm.",
                SctNavigationTarget{SctNavigationKind::Instruction,
                    expected.controller.instruction.value()}});
        }
    }
    result.document = std::move(document);
    return result;
}

}  // namespace salsa::core
