#include "SalsaCore/Sct/SctDocumentLoader.h"

#include "SpiceSCT/SctDocumentIndex.h"
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
    }
    return "UnknownSctDiagnostic";
}

[[nodiscard]] std::optional<SctNavigationTarget> navigationFor(
    const std::optional<spice::sct::SctDocumentEntityId>& entity) {
    if (!entity.has_value()) return std::nullopt;
    return std::visit([](const auto& id) -> std::optional<SctNavigationTarget> {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, std::monostate>) return std::nullopt;
        else if constexpr (std::is_same_v<T, spice::sct::SctSectionId>)
            return SctNavigationTarget{ SctNavigationKind::Section, id.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return SctNavigationTarget{ SctNavigationKind::Instruction, id.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return SctNavigationTarget{ SctNavigationKind::String, id.value() };
        else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryId>)
            return SctNavigationTarget{ SctNavigationKind::FooterEntry, id.value() };
        else
            return SctNavigationTarget{ SctNavigationKind::OpaqueAttachment, id.value() };
    }, *entity);
}

[[nodiscard]] SctPipelineDiagnostic convertDocumentDiagnostic(
    const AssetLocator& locator,
    const SctPipelineStage stage,
    const spice::sct::SctDocumentDiagnostic& source) {
    SctPipelineDiagnostic converted;
    converted.severity = severityOf(source.severity);
    converted.stage = stage;
    converted.code = diagnosticCodeName(source.code);
    converted.message = source.message;
    converted.locator = locator;
    converted.target = navigationFor(source.entity);
    if (source.parameter.has_value()) {
        converted.schemaIndex = source.parameter->schemaIndex;
        converted.repeatedGroupOrdinal = source.parameter->repeatedGroupOrdinal;
    }
    converted.expressionChildPath = source.expressionChildPath;
    if (source.textRange.has_value()) {
        converted.textOffset = source.textRange->offset;
        converted.textSize = source.textRange->size;
    } else if (source.textLocation.has_value()) {
        converted.textOffset = source.textLocation->utf8Range.offset;
        converted.textSize = source.textLocation->utf8Range.size;
    }
    return converted;
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

std::string_view sctTextConventionName(
    const spice::sct::SctKnownTextConvention convention) noexcept {
    const auto* descriptor = spice::sct::findSctKnownTextConvention(convention);
    return descriptor == nullptr ? std::string_view{} : descriptor->stableName;
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
    if (!inspection->textAssessment.records.empty()
        && inspection->textAssessment.viableConventions.size() == 1) {
        convention = inspection->textAssessment.viableConventions.front();
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
    auto assessment = spice::sct::SctDocumentWorkflow::importForEditing(*inspection->parsed, options);
    if (!assessment.import.document.has_value()) {
        result.infrastructureDiagnostics.push_back({ DiagnosticSeverity::Error,
            DiagnosticCode::SctImportFailed, "The parsed SCT could not be imported as an editable document.",
            locator.path() });
        return result;
    }

    auto snapshot = std::make_shared<SctDocumentSnapshot>(SctDocumentSnapshot{
        inspection->source,
        inspection,
        convention,
        origin,
        std::make_shared<const spice::sct::SctDocument>(
            std::move(*assessment.import.document)),
        std::move(assessment.import.receipt),
        assessment.readiness,
        inspection->diagnostics,
    });
    for (const auto& diagnostic : assessment.import.diagnostics) {
        snapshot->diagnostics.push_back(convertDocumentDiagnostic(
            locator, SctPipelineStage::Import, diagnostic));
    }
    for (const auto& diagnostic : assessment.documentValidation.diagnostics) {
        snapshot->diagnostics.push_back(convertDocumentDiagnostic(
            locator, SctPipelineStage::Validation, diagnostic));
    }
    result.document = std::move(snapshot);
    return result;
}

}  // namespace salsa::core
