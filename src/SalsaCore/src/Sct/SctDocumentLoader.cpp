#include "SalsaCore/Sct/SctDocumentLoader.h"

#include "SpiceSCT/SctDocumentAnalysis.h"
#include "SpiceSCT/SctParser.h"

#include <span>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic cancelledDiagnostic(const AssetLocator& locator) {
    return { DiagnosticSeverity::Info, DiagnosticCode::Cancelled,
        "SCT document loading was cancelled.", locator.path() };
}

[[nodiscard]] DiagnosticSeverity severityOf(const spice::sct::SctDiagnosticSeverity severity) {
    switch (severity) {
    case spice::sct::SctDiagnosticSeverity::Info: return DiagnosticSeverity::Info;
    case spice::sct::SctDiagnosticSeverity::Warning: return DiagnosticSeverity::Warning;
    case spice::sct::SctDiagnosticSeverity::Error: return DiagnosticSeverity::Error;
    }
    return DiagnosticSeverity::Error;
}

[[nodiscard]] std::string diagnosticCodeName(const spice::sct::SctDiagnosticCode code) {
    using enum spice::sct::SctDiagnosticCode;
    switch (code) {
    case ParseFailed: return "ParseFailed";
    case UnsafePhysicalStructure: return "UnsafePhysicalStructure";
    case OverlappingSourceClaims: return "OverlappingSourceClaims";
    case UnresolvedReference: return "UnresolvedReference";
    case RepeatedCountMismatch: return "RepeatedCountMismatch";
    case AmbiguousExpression: return "AmbiguousExpression";
    case AmbiguousString: return "AmbiguousString";
    case InvalidId: return "InvalidId";
    case DuplicateId: return "DuplicateId";
    case AllocatorDiscontinuity: return "AllocatorDiscontinuity";
    case InvalidName: return "InvalidName";
    case InvalidContent: return "InvalidContent";
    case OpcodeUnavailable: return "OpcodeUnavailable";
    case ParameterMismatch: return "ParameterMismatch";
    case ExpressionInvalid: return "ExpressionInvalid";
    case AttachmentInvalid: return "AttachmentInvalid";
    case OpaquePlatformUnverified: return "OpaquePlatformUnverified";
    case LayoutOverflow: return "LayoutOverflow";
    case EncodingUnsupported: return "EncodingUnsupported";
    case RelocationOutOfRange: return "RelocationOutOfRange";
    case OpaquePlacementUnsatisfied: return "OpaquePlacementUnsatisfied";
    case CompressionFailed: return "CompressionFailed";
    case ProvisionalAuthoringDefault: return "ProvisionalAuthoringDefault";
    case ProvisionalOpcodeConstraint: return "ProvisionalOpcodeConstraint";
    case TextInvalid: return "TextInvalid";
    case HeaderUnavailable: return "HeaderUnavailable";
    case ExpressionRuntimeStackDepth: return "ExpressionRuntimeStackDepth";
    }
    return "UnknownSctDiagnostic";
}

void addAssessmentDiagnostics(SctSourceInspection& inspection) {
    for (const auto& issue : inspection.textAssessment.issues) {
        SctPipelineDiagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Warning;
        diagnostic.stage = SctPipelineStage::TextAssessment;
        diagnostic.code = "TextEvidence";
        diagnostic.message = issue.message;
        diagnostic.locator = inspection.source.descriptor.locator;
        if (issue.record.has_value()) {
            diagnostic.payloadOffset = issue.record->decodedPayloadOffset;
        }
        inspection.diagnostics.push_back(std::move(diagnostic));
    }
}

}  // namespace

SctPipelineDiagnostic convertSctDiagnostic(
    const spice::sct::SctDocumentDiagnostic& source,
    const SctPipelineStage stage,
    std::optional<AssetLocator> locator) {
    SctPipelineDiagnostic converted;
    converted.severity = severityOf(source.severity);
    converted.stage = stage;
    converted.code = diagnosticCodeName(source.code);
    converted.message = source.message;
    converted.locator = std::move(locator);
    converted.primaryLocation = source.primaryLocation;
    converted.relatedLocations = source.relatedLocations;
    return converted;
}

std::string_view sctTextConventionName(
    const spice::sct::SctKnownTextConvention convention) noexcept {
    const auto* descriptor = spice::sct::findSctKnownTextConvention(convention);
    return descriptor == nullptr ? std::string_view{} : descriptor->stableName;
}

std::optional<spice::sct::SctKnownTextConvention> recommendedSctTextConvention(
    const spice::sct::SctSourceTextAssessment& assessment) noexcept {
    if (assessment.records.empty()
        || assessment.recommendation.status
            != spice::sct::SctSourceTextRecommendationStatus::Unique) {
        return std::nullopt;
    }
    return assessment.recommendation.convention;
}

SctLoadResult SctDocumentLoader::load(
    const GameProjectContext& project,
    const AssetLocator& locator,
    const std::stop_token stopToken) {
    SctLoadResult result;
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        result.infrastructureDiagnostics.push_back(cancelledDiagnostic(locator));
        return result;
    }

    auto sourceResult = project.assets().loadAsset(locator);
    if (!sourceResult) {
        result.infrastructureDiagnostics = sourceResult.diagnostics();
        return result;
    }
    auto source = std::move(sourceResult).takeValue();
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        result.infrastructureDiagnostics.push_back(cancelledDiagnostic(locator));
        return result;
    }

    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(source.bytes.data()), source.bytes.size());
    spice::sct::SctParser parser;
    auto parsed = std::make_shared<spice::sct::SctParseResult>(
        parser.parse(bytes, locator.identityKey()));

    auto inspection = std::make_shared<SctSourceInspection>(SctSourceInspection{
        std::move(source), std::move(parsed), {}, {} });
    for (const auto& parserDiagnostic : inspection->parsed->diagnostics) {
        SctPipelineDiagnostic diagnostic;
        diagnostic.severity = inspection->parsed->parseOk
            ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
        diagnostic.stage = SctPipelineStage::Parse;
        diagnostic.code = "ParserDiagnostic";
        diagnostic.message = parserDiagnostic.message;
        diagnostic.locator = locator;
        diagnostic.payloadOffset = parserDiagnostic.offset;
        inspection->diagnostics.push_back(std::move(diagnostic));
    }
    if (!inspection->parsed->parseOk) {
        result.inspection = std::move(inspection);
        result.infrastructureDiagnostics.push_back({ DiagnosticSeverity::Error,
            DiagnosticCode::SctParseFailed, "The SCT parser could not decode the asset.", locator.path() });
        return result;
    }
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        result.infrastructureDiagnostics.push_back(cancelledDiagnostic(locator));
        return result;
    }

    inspection->textAssessment = spice::sct::SctSourceTextDetector::assess(*inspection->parsed);
    addAssessmentDiagnostics(*inspection);
    std::optional<spice::sct::SctKnownTextConvention> convention;
    auto origin = SctTextSelectionOrigin::None;
    if (const auto recommended = recommendedSctTextConvention(
            inspection->textAssessment)) {
        convention = recommended;
        origin = SctTextSelectionOrigin::UniqueAssessment;
    }
    return materialize(std::move(inspection), convention, origin, stopToken);
}

SctLoadResult SctDocumentLoader::materialize(
    std::shared_ptr<const SctSourceInspection> inspection,
    const std::optional<spice::sct::SctKnownTextConvention> convention,
    const SctTextSelectionOrigin origin,
    const std::stop_token stopToken) {
    SctLoadResult result;
    result.inspection = inspection;
    if (!inspection || !inspection->parsed || !inspection->parsed->parseOk) return result;
    const auto& locator = inspection->source.descriptor.locator;
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        result.infrastructureDiagnostics.push_back(cancelledDiagnostic(locator));
        return result;
    }

    spice::sct::SctDocumentImportOptions options;
    if (convention.has_value()) options.sourceTextEncoding = spice::sct::sctTextEncodingFor(*convention);
    options.footerTextPromotion = origin == SctTextSelectionOrigin::UserSelected
        ? spice::sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding
        : spice::sct::SctFooterTextPromotionPolicy::PreserveAmbiguous;
    auto assessment = spice::sct::SctDocumentWorkflow::importForEditing(*inspection->parsed, options);
    if (!assessment.import.document.has_value()) {
        result.infrastructureDiagnostics.push_back({ DiagnosticSeverity::Error,
            DiagnosticCode::SctImportFailed, "The parsed SCT could not be imported as an editable document.",
            locator.path() });
        return result;
    }

    auto diagnostics = inspection->diagnostics;
    for (const auto& diagnostic : assessment.import.diagnostics) {
        diagnostics.push_back(convertSctDiagnostic(
            diagnostic, SctPipelineStage::Import, locator));
    }
    for (const auto& diagnostic : assessment.documentValidation.diagnostics) {
        diagnostics.push_back(convertSctDiagnostic(
            diagnostic, SctPipelineStage::Validation, locator));
    }
    std::vector<SctPipelineDiagnostic> baselineDiagnostics;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.stage != SctPipelineStage::Validation)
            baselineDiagnostics.push_back(diagnostic);
    }
    auto evidence = assessment.import.context.bind(
        assessment.import.context.revisionProvenance());
    auto provenance = std::make_shared<const SctDocumentProvenance>(SctDocumentProvenance{
        inspection, convention, origin, evidence,
        std::move(baselineDiagnostics),
    });
    auto document = std::make_shared<const spice::sct::SctDocument>(
        std::move(*assessment.import.document));
    auto analysis = std::make_shared<const spice::sct::SctDocumentAnalysis>(
        spice::sct::SctDocumentAnalysis::build(*document,
            evidence ? &*evidence : nullptr));
    auto snapshot = std::make_shared<SctDocumentSnapshot>(SctDocumentSnapshot{
        std::move(provenance), document, std::move(analysis),
        assessment.readiness,
        std::move(diagnostics),
    });
    result.document = std::move(snapshot);
    return result;
}

}  // namespace salsa::core
