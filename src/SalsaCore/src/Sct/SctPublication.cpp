#include "SalsaCore/Sct/SctPublication.h"
#include "SalsaCore/Authoring/SctAuthoringMaterializer.h"

#include "SalsaCore/Persistence/AtomicFile.h"

#include <algorithm>
#include <cwctype>
#include <ranges>
#include <span>
#include <system_error>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic publicationDiagnostic(
    const DiagnosticSeverity severity, const DiagnosticCode code,
    std::string message, std::optional<std::filesystem::path> path = std::nullopt) {
    return {severity, code, std::move(message), std::move(path)};
}

[[nodiscard]] std::filesystem::path sourcePath(
    const GameProjectContext& project, const AssetLocator& locator) {
    return project.dataset().root / locator.path();
}

[[nodiscard]] std::wstring pathIdentity(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    auto normalized = absolute.lexically_normal().wstring();
    std::ranges::transform(normalized, normalized.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    std::ranges::replace(normalized, L'/', L'\\');
    while (normalized.size() > 3u && normalized.back() == L'\\') normalized.pop_back();
    return normalized;
}

[[nodiscard]] SctPipelineDiagnostic cancelledDiagnostic(
    const AssetLocator& locator) {
    SctPipelineDiagnostic result;
    result.severity = DiagnosticSeverity::Info;
    result.stage = SctPipelineStage::Publication;
    result.code = "PublicationCancelled";
    result.message = "SCT export was cancelled.";
    result.locator = locator;
    return result;
}

void appendExportDiagnostics(
    SctPublicationResult& result,
    const std::vector<spice::sct::SctDocumentDiagnostic>& diagnostics,
    const AssetLocator& locator) {
    for (const auto& diagnostic : diagnostics) {
        result.diagnostics.push_back(convertSctDiagnostic(
            diagnostic, SctPipelineStage::Publication, locator));
    }
}

}  // namespace

SctPublicationDefaults SctPublicationService::defaultsFor(
    const DatasetContext& dataset,
    const SctDocumentSnapshot& snapshot) noexcept {
    SctPublicationDefaults result;
    if (dataset.identity.platform) {
        result.platform = *dataset.identity.platform == GamePlatform::Dreamcast
            ? spice::sct::SctPlatform::Dreamcast
            : spice::sct::SctPlatform::GameCube;
    } else if (snapshot.provenance && snapshot.provenance->importReceipt()
        && snapshot.provenance->importReceipt()->declaredSourcePlatform) {
        result.platform = snapshot.provenance->importReceipt()->declaredSourcePlatform;
    }
    if (snapshot.provenance && snapshot.provenance->textConvention) {
        result.textEncoding = spice::sct::sctTextEncodingFor(
            *snapshot.provenance->textConvention);
    }
    if (snapshot.provenance && snapshot.provenance->importReceipt()) {
        const auto& source = snapshot.provenance->importReceipt()->source;
        if (source.byteOrder == spice::sct::SctSourceByteOrder::BigEndian) {
            result.byteOrder = spice::sct::SctDocumentOutputByteOrder::BigEndian;
        } else if (source.byteOrder == spice::sct::SctSourceByteOrder::LittleEndian) {
            result.byteOrder = spice::sct::SctDocumentOutputByteOrder::LittleEndian;
        }
        result.wrapper = source.wrapper == spice::sct::SctSourceWrapper::Aklz
            ? spice::sct::SctDocumentOutputWrapper::Aklz
            : spice::sct::SctDocumentOutputWrapper::Raw;
    }
    return result;
}

SctPublicationResult SctPublicationService::publish(
    const GameProjectContext& project,
    const SctPublicationRequest& request,
    const std::stop_token stopToken,
    const SctPublicationObserver& observer) {
    SctPublicationResult result;
    const auto progress = [&observer](const SctPublicationPhase phase,
        const std::uint64_t completed) {
        if (observer) observer({phase, completed, 1});
    };
    const auto cancel = [&]() -> SctPublicationResult {
        result.cancelled = true;
        result.diagnostics.push_back(cancelledDiagnostic(request.locator));
        return std::move(result);
    };
    progress(SctPublicationPhase::Preflight, 0);
    if (stopToken.stop_requested()) return cancel();

    if (!request.capturedRevision.revision.valid()
        || request.capturedRevision.historyStateToken == nullptr
        || request.capturedRevision.provenance == nullptr
        || request.destination.empty() || request.destination.filename().empty()) {
        result.infrastructureDiagnostics.push_back(publicationDiagnostic(
            DiagnosticSeverity::Error, DiagnosticCode::SctExportFailed,
            "The SCT publication request is incomplete.", request.destination));
        return result;
    }

    const auto loadedSource = project.assets().loadAsset(request.locator);
    const bool sourceCurrent = loadedSource
        && loadedSource.value().descriptor.revision == request.expectedSourceRevision;
    const auto loadedSourcePath = sourcePath(project, request.locator);
    const bool replacingSource = pathIdentity(loadedSourcePath)
        == pathIdentity(request.destination);
    if (replacingSource && !request.allowSourceReplacement) {
        result.infrastructureDiagnostics.push_back(publicationDiagnostic(
            DiagnosticSeverity::Error,
            DiagnosticCode::PublicationSourceReplacementNotConfirmed,
            "Replacing the loaded source SCT requires explicit confirmation.",
            request.destination));
        return result;
    }
    if (replacingSource && !sourceCurrent) {
        result.infrastructureDiagnostics.push_back(publicationDiagnostic(
            DiagnosticSeverity::Error, DiagnosticCode::PublicationSourceChanged,
            "The loaded source SCT changed after this document revision was created; it was not replaced.",
            loadedSourcePath));
        return result;
    }
    if (!replacingSource && !sourceCurrent) {
        result.infrastructureDiagnostics.push_back(publicationDiagnostic(
            DiagnosticSeverity::Warning, DiagnosticCode::PublicationSourceChanged,
            "The source SCT is no longer current. The exported file still represents the captured in-memory revision.",
            loadedSourcePath));
    }
    progress(SctPublicationPhase::Preflight, 1);
    if (stopToken.stop_requested()) return cancel();

    std::shared_ptr<const SctDocumentSnapshot> snapshot =
        request.capturedRevision.verifiedSnapshot;
    if (!snapshot) {
        progress(SctPublicationPhase::Materializing, 0);
        if (!request.capturedRevision.materialization) {
            result.infrastructureDiagnostics.push_back(publicationDiagnostic(
                DiagnosticSeverity::Error, DiagnosticCode::SctExportFailed,
                "The captured SCT revision cannot be materialized."));
            return result;
        }
        result.materialization = SctDocumentMaterializer::materialize(
            *request.capturedRevision.materialization, stopToken);
        if (result.materialization->cancelled || stopToken.stop_requested()) return cancel();
        if (!result.materialization->succeeded()) {
            result.diagnostics = result.materialization->diagnostics;
            if (result.diagnostics.empty()) {
                result.infrastructureDiagnostics.push_back(publicationDiagnostic(
                    DiagnosticSeverity::Error, DiagnosticCode::SctExportFailed,
                    "The captured SCT revision failed structural verification."));
            }
            return result;
        }
        snapshot = std::make_shared<SctDocumentSnapshot>(SctDocumentSnapshot{
            request.capturedRevision.provenance,
            result.materialization->document,
            result.materialization->analysis,
            spice::sct::SctDocumentReadiness::StructurallyValid,
            result.materialization->diagnostics});
        progress(SctPublicationPhase::Materializing, 1);
    }
    if (!snapshot || !snapshot->document) {
        result.infrastructureDiagnostics.push_back(publicationDiagnostic(
            DiagnosticSeverity::Error, DiagnosticCode::SctExportFailed,
            "The captured SCT revision has no complete document."));
        return result;
    }

    spice::sct::SctDocumentExportOptions options{
        request.options.platform,
        request.options.textEncoding,
        request.options.byteOrder,
        request.options.wrapper,
        spice::sct::SctOpaquePreservationPolicy::RequirePreservation,
        {}};
    const auto* evidence = snapshot->provenance && snapshot->provenance->importEvidence
        ? &*snapshot->provenance->importEvidence : nullptr;
    auto outputDocument = snapshot->document;
    if (request.capturedRevision.semanticOutput) {
        auto lowered = SctAuthoringMaterializer::buildSemanticDocument(*outputDocument);
        result.infrastructureDiagnostics = lowered.diagnostics();
        if (!lowered) return result;
        outputDocument = std::make_shared<const spice::sct::SctDocument>(std::move(lowered).takeValue());
        if (evidence && evidence->receipt().source.header.available)
            options.header = {spice::sct::SctHeaderExportMode::ExplicitValues, evidence->receipt().source.header.values};
        evidence = nullptr;
    }
    progress(SctPublicationPhase::Encoding, 0);
    auto exported = spice::sct::SctDocumentExporter::exportDocument(
        *outputDocument, options, evidence);
    appendExportDiagnostics(result, exported.diagnostics, request.locator);
    if (!exported.success || !exported.layout) return result;
    if (request.capturedRevision.semanticOutput) {
        auto verified = SctAuthoringMaterializer::verifySemanticOutput(*outputDocument, exported.bytes, options);
        if (!verified) { result.infrastructureDiagnostics = verified.diagnostics(); return result; }
    }
    progress(SctPublicationPhase::Encoding, 1);
    if (stopToken.stop_requested()) return cancel();

    const auto output = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(exported.bytes.data()),
        exported.bytes.size()};
    progress(SctPublicationPhase::Hashing, 0);
    const auto digest = sha256(output);
    if (!digest) {
        result.infrastructureDiagnostics.insert(result.infrastructureDiagnostics.end(),
            digest.diagnostics().begin(), digest.diagnostics().end());
        return result;
    }
    progress(SctPublicationPhase::Hashing, 1);
    if (stopToken.stop_requested()) return cancel();
    progress(SctPublicationPhase::Installing, 0);
    const auto written = replaceFileAtomically(request.destination, output);
    if (!written) {
        result.infrastructureDiagnostics = written.diagnostics();
        for (auto& diagnostic : result.infrastructureDiagnostics) {
            diagnostic.code = DiagnosticCode::PublicationWriteFailed;
            diagnostic.message = "The exported SCT could not be installed: "
                + diagnostic.message;
        }
        return result;
    }
    progress(SctPublicationPhase::Installing, 1);

    result.receipt.emplace(SctPublicationReceipt{
        request.locator,
        request.capturedRevision.revision,
        request.sourceDataset,
        request.expectedSourceRevision,
        request.destination,
        request.options,
        digest.value(),
        exported.outputSize,
        exported.decodedPayloadSize,
        std::move(*exported.layout),
        std::move(exported.preservation),
        replacingSource,
    });
    return result;
}

}  // namespace salsa::core
