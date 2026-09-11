#pragma once

#include "SalsaCore/Authoring/SctAuthoringProject.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SpiceSCT/SctDocumentExporter.h"

namespace salsa::core {

// Only the importer can construct backing objects. Own all input values so callers
// cannot mutate evidence or bytes through another shared pointer after adoption.
class SctImportedProgram final {
public:
    [[nodiscard]] const SctAuthoringProjectId& projectId() const noexcept { return project_; }
    [[nodiscard]] const SctAuthoringBaseline& baseline() const noexcept { return baseline_; }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] const spice::sct::SctDocument& document() const noexcept { return document_; }
    [[nodiscard]] const spice::sct::SctBoundImportEvidence& evidence() const noexcept { return evidence_; }
    [[nodiscard]] const std::vector<SctPipelineDiagnostic>& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] spice::sct::SctDocumentReadiness readiness() const noexcept { return readiness_; }
private:
    friend class SctAuthoringImporter;
    SctImportedProgram(SctAuthoringProjectId project, SctAuthoringBaseline baseline,
        std::vector<std::byte> bytes, spice::sct::SctDocument document,
        spice::sct::SctBoundImportEvidence evidence, std::vector<SctPipelineDiagnostic> diagnostics,
        spice::sct::SctDocumentReadiness readiness);
    SctAuthoringProjectId project_;
    SctAuthoringBaseline baseline_;
    std::vector<std::byte> bytes_;
    spice::sct::SctDocument document_;
    spice::sct::SctBoundImportEvidence evidence_;
    std::vector<SctPipelineDiagnostic> diagnostics_;
    spice::sct::SctDocumentReadiness readiness_;
};
using SctImportedPrograms = std::vector<std::shared_ptr<const SctImportedProgram>>;

struct SctAuthoringImportRequest final {
    SourceAssetSnapshot source;
    DatasetIdentity dataset;
    SctAuthoringImportRecipe recipe;
    std::optional<spice::sct::SctKnownTextConvention> textConvention;
    std::string name;
};
struct SctAuthoringAdoption final {
    SctAuthoringProject project;
    std::shared_ptr<const SctImportedProgram> program;
    SctScriptId script;
    SctContentId content;
};
class SctAuthoringImporter final {
public:
    [[nodiscard]] static Result<SctAuthoringAdoption> import(
        const SctAuthoringProject& project, RevisionId expectedRevision,
        SctAuthoringImportRequest request, std::stop_token stop = {});
    [[nodiscard]] static Result<SctAuthoringAdoption> replaceImportedScript(
        const SctAuthoringProject& project, RevisionId expectedRevision, SctScriptId script,
        SctAuthoringImportRequest request, std::stop_token stop = {});
    // Reconstitute an existing binding from verified immutable bytes. The store
    // also checks the persisted physical identity signature before adoption.
    [[nodiscard]] static Result<std::shared_ptr<const SctImportedProgram>> restore(
        const SctAuthoringProject& project, SctBaselineId baseline,
        std::vector<std::byte> bytes, std::stop_token stop = {});
};

struct SctPreservedEditRequest final {
    RevisionId expectedRevision;
    SctContentId content;
    spice::sct::SctParameterSite site;
    std::uint16_t expectedOpcode = 0;
    SctLiteralConstant expectedValue;
    SctLiteralConstant value;
};
struct SctPreservedEditResult final {
    SctAuthoringProject project;
    std::vector<SctScriptId> affectedScripts;
};
class SctPreservedEditService final {
public:
    // Fixed parameters only; replacement retains the literal encoding family,
    // serialized width, and termination. Uses the same check as override replay.
    [[nodiscard]] static Result<SctLiteralConstant> inspectLiteral(
        const spice::sct::SctDocument& document, const spice::sct::SctParameterSite& site,
        std::uint16_t expectedOpcode, const SctLiteralConstant& replacement);
    [[nodiscard]] static Result<SctPreservedEditResult> replaceLiteral(
        const SctAuthoringProject& project, const SctImportedPrograms& programs,
        const SctPreservedEditRequest& request);
};

enum class SctAuthoringOutputMode { ReuseUnchangedSource, Rebuild };
struct SctAuthoringMaterializationRequest final {
    std::shared_ptr<const SctAuthoringProject> project;
    SctImportedPrograms programs;
    std::vector<SctScriptId> scripts;
    SctAuthoringOutputMode mode = SctAuthoringOutputMode::ReuseUnchangedSource;
};
struct SctPreparedAuthoringOutput final {
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<const spice::sct::SctDocument> document;
    SctRealizationMap realization;
    Sha256Digest digest;
    bool reusedSource = false;
    // Source reuse is evidenced by the original bytes and imported map, never a
    // rebuilt layout that could describe different offsets.
    std::optional<spice::sct::SctImportedSourceMap> sourceLocations;
    std::optional<spice::sct::SctDocumentLayout> layout;
    std::optional<spice::sct::SctPreservationReport> preservation;
};
struct SctAuthoringScriptOutput final {
    SctScriptId script;
    spice::sct::SctDocumentReadiness readiness = spice::sct::SctDocumentReadiness::Unavailable;
    std::optional<SctPreparedAuthoringOutput> prepared;
    std::vector<SctPipelineDiagnostic> diagnostics;
    std::vector<Diagnostic> infrastructureDiagnostics;
};
struct SctAuthoringMaterializationResult final {
    SctAuthoringProjectId project;
    RevisionId revision;
    std::vector<SctAuthoringScriptOutput> scripts;
    std::vector<Diagnostic> diagnostics;
    bool cancelled = false;
    [[nodiscard]] bool succeeded() const noexcept;
};
class SctAuthoringMaterializer final {
public:
    [[nodiscard]] static Result<SctSemanticState> workingState(
        const SctAuthoringProject& project, const SctImportedPrograms& programs, SctScriptId script);
    [[nodiscard]] static Result<SctAuthoringProject> replaceWorkingState(
        const SctAuthoringProject& project, const SctImportedPrograms& programs,
        SctScriptId script, const SctSemanticState& working);
    [[nodiscard]] static std::shared_ptr<const SctDocumentSnapshot> snapshot(
        const SctImportedProgram& program, std::shared_ptr<const spice::sct::SctDocument> document);
    [[nodiscard]] static SctAuthoringMaterializationResult materialize(
        const SctAuthoringMaterializationRequest& request, std::stop_token stop = {});
};

} // namespace salsa::core
