#pragma once

#include "SalsaCore/Foundation/Diagnostic.h"
#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Project/GameProjectContext.h"

#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctDocumentAnalysis.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include "SpiceSCT/SctDocumentWorkflow.h"
#include "SpiceSCT/SctModel.h"
#include "SpiceSCT/SctTextContract.h"
#include "SpiceSCT/SctTextEvidence.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace salsa::core {

enum class SctPipelineStage { Parse, TextAssessment, Import, Validation, Edit, Publication };
enum class SctTextSelectionOrigin { None, UniqueAssessment, UserSelected };

enum class SctNavigationKind {
    Document,
    Section,
    Instruction,
    String,
    FooterEntry,
    OpaqueAttachment,
    FooterGroup,
    OpaqueGroup,
};

struct SctNavigationTarget final {
    SctNavigationKind kind = SctNavigationKind::Document;
    std::uint64_t id = 0;
    auto operator<=>(const SctNavigationTarget&) const = default;
};

struct SctPipelineDiagnostic final {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    SctPipelineStage stage = SctPipelineStage::Parse;
    std::string code{};
    std::string message{};
    std::optional<AssetLocator> locator{};
    std::optional<std::uint32_t> payloadOffset{};
    std::optional<SctNavigationTarget> target{};
    std::optional<spice::sct::SctDiagnosticLocation> primaryLocation{};
    std::vector<spice::sct::SctDiagnosticLocation> relatedLocations{};
};

struct SctSourceInspection final {
    SourceAssetSnapshot source;
    DatasetFingerprint sourceDatasetFingerprint;
    std::shared_ptr<const spice::sct::SctParseResult> parsed;
    spice::sct::SctSourceTextAssessment textAssessment;
    std::vector<SctPipelineDiagnostic> diagnostics;
};

struct SctDocumentProvenance final {
    std::shared_ptr<const SctSourceInspection> inspection;
    std::optional<spice::sct::SctKnownTextConvention> textConvention;
    SctTextSelectionOrigin textSelectionOrigin = SctTextSelectionOrigin::None;
    std::optional<spice::sct::SctBoundImportEvidence> importEvidence;
    std::vector<SctPipelineDiagnostic> baselineDiagnostics;

    [[nodiscard]] const SourceAssetSnapshot& source() const noexcept {
        return inspection->source;
    }

    [[nodiscard]] const spice::sct::SctDocumentImportReceipt* importReceipt() const noexcept {
        return importEvidence ? &importEvidence->receipt() : nullptr;
    }
};

struct SctDocumentSnapshot final {
    std::shared_ptr<const SctDocumentProvenance> provenance;
    std::shared_ptr<const spice::sct::SctDocument> document;
    std::shared_ptr<const spice::sct::SctDocumentAnalysis> analysis;
    spice::sct::SctDocumentReadiness readiness = spice::sct::SctDocumentReadiness::Unavailable;
    std::vector<SctPipelineDiagnostic> diagnostics;
};

struct SctLoadResult final {
    std::shared_ptr<const SctSourceInspection> inspection;
    std::shared_ptr<const SctDocumentSnapshot> document;
    std::vector<Diagnostic> infrastructureDiagnostics;
    bool cancelled = false;

    [[nodiscard]] bool succeeded() const noexcept { return document != nullptr; }
};

class SctDocumentLoader final {
public:
    [[nodiscard]] static SctLoadResult load(
        const GameProjectContext& project,
        const AssetLocator& locator,
        std::stop_token stopToken = {});

    [[nodiscard]] static SctLoadResult materialize(
        std::shared_ptr<const SctSourceInspection> inspection,
        std::optional<spice::sct::SctKnownTextConvention> convention,
        SctTextSelectionOrigin origin,
        std::stop_token stopToken = {});
};

[[nodiscard]] std::string_view sctTextConventionName(
    spice::sct::SctKnownTextConvention convention) noexcept;

[[nodiscard]] std::optional<spice::sct::SctKnownTextConvention>
recommendedSctTextConvention(
    const spice::sct::SctSourceTextAssessment& assessment) noexcept;

[[nodiscard]] SctPipelineDiagnostic convertSctDiagnostic(
    const spice::sct::SctDocumentDiagnostic& source,
    SctPipelineStage stage,
    std::optional<AssetLocator> locator = std::nullopt);

}  // namespace salsa::core
