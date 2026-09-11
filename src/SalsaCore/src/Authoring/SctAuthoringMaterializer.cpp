#include "SalsaCore/Authoring/SctAuthoringMaterializer.h"
#include "SalsaCore/Authoring/SctAuthoringCodec.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctParser.h"

#include <cstring>
#include <set>
#include <type_traits>

namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string message, DiagnosticCode code = DiagnosticCode::InvalidSctAuthoringProject) {
    return {DiagnosticSeverity::Error, code, std::move(message)};
}
Diagnostic cancelled() { return error("Authoring operation cancelled.", DiagnosticCode::Cancelled); }
sct::SctPlatform platform(GamePlatform value) {
    return value == GamePlatform::GameCube ? sct::SctPlatform::GameCube : sct::SctPlatform::Dreamcast;
}
bool validPlatform(std::optional<GamePlatform> value) {
    return !value || *value == GamePlatform::GameCube || *value == GamePlatform::Dreamcast;
}
std::span<const std::uint8_t> bytesOf(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}
Result<const SctImportedProgram*> resolve(const SctAuthoringProject& project,
    const SctImportedPrograms& programs, const SctAuthoringBaseline& expected) {
    const SctImportedProgram* result = nullptr;
    for (const auto& program : programs) {
        if (!program || program->baseline().id != expected.id) continue;
        if (result) return Result<const SctImportedProgram*>::failure(error("Duplicate imported backing binding."));
        result = program.get();
    }
    if (!result) return Result<const SctImportedProgram*>::failure(error("Imported baseline backing is unavailable."));
    const auto& actual = result->baseline();
    if (result->projectId() != project.id || actual.importedDocument != expected.importedDocument
        || actual.datasetFingerprint != expected.datasetFingerprint || actual.source.locator != expected.source.locator
        || actual.source.revision != expected.source.revision || actual.source.byteSize != expected.source.byteSize
        || actual.textConvention != expected.textConvention || actual.recipe != expected.recipe)
        return Result<const SctImportedProgram*>::failure(error("Imported baseline identity or recipe does not match its backing."));
    return Result<const SctImportedProgram*>::success(result);
}
Result<const SctAuthoringContent*> wholeRegion(const SctAuthoringProject& project, SctScriptId script,
    const SctImportedProgram& program) {
    const auto* context = project.find(script);
    if (!context) return Result<const SctAuthoringContent*>::failure(error("Selected script does not exist."));
    if (context->platform != program.baseline().recipe.platform)
        return Result<const SctAuthoringContent*>::failure(error("Script platform differs from its immutable import recipe."));
    if (std::ranges::any_of(project.modules, [&](const auto& m) { return m.script == script; })
        || std::ranges::any_of(project.entrypoints, [&](const auto& e) { return e.script == script; }))
        return Result<const SctAuthoringContent*>::failure(error("Module and entrypoint lowering is not implemented in S2."));
    for (const auto& connection : project.connections) {
        for (const auto* endpoint : {&connection.source, &connection.destination}) {
            const auto* portId = std::get_if<SctPortId>(endpoint);
            if (!portId) return Result<const SctAuthoringContent*>::failure(error("Unresolved execution connection prevents output."));
            const auto* port = project.find(*portId);
            if (port && std::visit([&](auto owner) { return project.effectiveScript(SctContentOwner{owner}) == script; }, port->owner))
                return Result<const SctAuthoringContent*>::failure(error("Connection lowering is not implemented in S2."));
        }
    }
    if (context->contentUses.size() != 1 || !std::holds_alternative<SctContentId>(context->contentUses.front()))
        return Result<const SctAuthoringContent*>::failure(error("S2 requires one resolved whole-document content use."));
    const auto* content = project.find(std::get<SctContentId>(context->contentUses.front()));
    if (!content || content->owner != SctContentOwner{script})
        return Result<const SctAuthoringContent*>::failure(error("Preserved content is not owned by the selected script."));
    const auto ownedCount = std::ranges::count_if(project.contents,
        [&](const auto& c) { return project.effectiveScript(c.owner) == script; });
    if (ownedCount != 1) return Result<const SctAuthoringContent*>::failure(error("Partitioned or unused content cannot be silently omitted."));
    if (const auto* selection = std::get_if<SctOrderedSourceSelection>(&content->region.coverage)) {
        const auto index = sct::SctDocumentIndex::build(program.document());
        for (const auto& entity : selection->entities) {
            const bool found = std::visit([&](auto id) {
                if constexpr (std::is_same_v<decltype(id), std::monostate>) return false;
                else return index.find(program.document(), id) != nullptr;
            }, entity);
            if (!found) return Result<const SctAuthoringContent*>::failure(error("Preserved selection contains an entity absent from its imported document."));
        }
        return Result<const SctAuthoringContent*>::failure(error("Ordered-selection lowering is not implemented in S2."));
    }
    return Result<const SctAuthoringContent*>::success(content);
}
Result<sct::SctDocument> replay(const SctImportedProgram& program, const SctAuthoringContent& content) {
    sct::SctDocument document = program.document();
    SctSemanticOperationBatch batch;
    for (const auto& edit : content.literalOverrides) {
        auto baseline = SctPreservedEditService::inspectLiteral(program.document(), edit.site, edit.opcode, edit.value);
        if (!baseline) return Result<sct::SctDocument>::failure(baseline.diagnostics());
        if (baseline.value() != edit.baselineValue)
            return Result<sct::SctDocument>::failure(error("Preserved override baseline precondition failed."));
        batch.operations.push_back(SctReplaceParameterValueOperation{edit.site, edit.value.expression()});
    }
    if (batch.operations.empty()) return Result<sct::SctDocument>::success(std::move(document));
    auto applied = SctSemanticOperationService::applyInPlace(document, batch);
    if (!applied.succeeded()) return Result<sct::SctDocument>::failure(error(applied.issues.front().message));
    return Result<sct::SctDocument>::success(std::move(document));
}
void appendDiagnostics(SctAuthoringScriptOutput& result, const std::vector<sct::SctDocumentDiagnostic>& diagnostics,
    SctPipelineStage stage, const AssetLocator& locator) {
    for (const auto& d : diagnostics) result.diagnostics.push_back(convertSctDiagnostic(d, stage, locator));
}
Result<SctRealizationId> realizationId(const SctAuthoringProject& project, SctScriptId script,
    SctAuthoringOutputMode mode, const Sha256Digest& output) {
    auto encoded = SctAuthoringCodec::encode(project);
    if (!encoded) return Result<SctRealizationId>::failure(encoded.diagnostics());
    const auto input = encoded.value() + "\n" + std::to_string(script.value) + "\n"
        + std::to_string(static_cast<int>(mode)) + "\n" + output.toHex();
    auto digest = sha256(std::as_bytes(std::span(input.data(), input.size())));
    if (!digest) return Result<SctRealizationId>::failure(digest.diagnostics());
    auto hex = digest.value().toHex().substr(0, 32);
    hex[12] = '8'; // UUIDv8 derived from the captured input, not a per-run random ID.
    constexpr char variants[] = "89ab";
    hex[16] = variants[std::to_integer<unsigned>(digest.value().bytes()[8]) & 3];
    return Result<SctRealizationId>::success({hex.substr(0,8) + "-" + hex.substr(8,4) + "-"
        + hex.substr(12,4) + "-" + hex.substr(16,4) + "-" + hex.substr(20)});
}
SctRealizationMap mapping(const SctAuthoringProject& project, SctScriptId script, SctContentId content,
    SctRealizationId realization, const sct::SctDocument& document) {
    SctRealizationRecord record{content, {}};
    for (const auto& section : document.sections) {
        record.locations.emplace_back(sct::SctDocumentEntityId{section.id});
        if (const auto* code = std::get_if<sct::SctScriptSectionContent>(&section.content)) {
            for (const auto& instruction : code->instructions) {
                record.locations.emplace_back(sct::SctDocumentEntityId{instruction.id});
                for (const auto& parameter : instruction.fixedParameters)
                    record.locations.emplace_back(sct::SctParameterSite{instruction.id, {parameter.schemaIndex, {}}});
                for (std::uint32_t i = 0; i < instruction.repeatedParameterGroups.size(); ++i)
                    for (const auto& parameter : instruction.repeatedParameterGroups[i].parameters)
                        record.locations.emplace_back(sct::SctParameterSite{instruction.id, {parameter.schemaIndex, i}});
            }
        } else if (const auto* text = std::get_if<sct::SctStringSectionContent>(&section.content))
            record.locations.emplace_back(sct::SctDocumentEntityId{text->string.id});
    }
    for (const auto& text : document.supplementaryText) record.locations.emplace_back(sct::SctDocumentEntityId{text.id});
    for (const auto& attachment : document.opaqueAttachments) record.locations.emplace_back(sct::SctDocumentEntityId{attachment.id});
    return {{project.id, project.revision, script, std::move(realization)}, {std::move(record)}};
}
SctAuthoringScriptOutput prepare(const SctAuthoringProject& project, const SctImportedPrograms& programs,
    SctScriptId scriptId, SctAuthoringOutputMode mode, std::stop_token stop) {
    SctAuthoringScriptOutput result; result.script = scriptId;
    const auto* script = project.find(scriptId);
    if (!script) { result.infrastructureDiagnostics.push_back(error("Requested script is absent.")); return result; }
    const auto* baseline = project.find(script->baseline);
    auto resolved = resolve(project, programs, *baseline);
    if (!resolved) { result.infrastructureDiagnostics = resolved.diagnostics(); return result; }
    const auto& program = *resolved.value();
    result.readiness = program.readiness();
    result.diagnostics = program.diagnostics();
    auto region = wholeRegion(project, scriptId, program);
    if (!region) { result.infrastructureDiagnostics = region.diagnostics(); return result; }
    const auto& receipt = program.evidence().receipt();
    if (!receipt.sourceMap.hasCompleteLeafCoverage()) {
        result.infrastructureDiagnostics.push_back(error("Imported source coverage is incomplete.")); return result;
    }
    if (!baseline->recipe.platform || receipt.source.byteOrder == sct::SctSourceByteOrder::Unknown) {
        result.infrastructureDiagnostics.push_back(error("Output requires a declared source platform and observed byte order.")); return result;
    }
    if (stop.stop_requested()) return result;
    auto replayed = replay(program, *region.value());
    if (!replayed) { result.infrastructureDiagnostics = replayed.diagnostics(); return result; }
    auto document = std::make_shared<const sct::SctDocument>(std::move(replayed).takeValue());
    const sct::SctDocumentExportOptions options{platform(*baseline->recipe.platform),
        baseline->textConvention ? *sct::sctTextEncodingFor(*baseline->textConvention) : sct::kSctShiftJisByte7FEncoding,
        receipt.source.byteOrder == sct::SctSourceByteOrder::BigEndian ? sct::SctDocumentOutputByteOrder::BigEndian : sct::SctDocumentOutputByteOrder::LittleEndian,
        receipt.source.wrapper == sct::SctSourceWrapper::Aklz ? sct::SctDocumentOutputWrapper::Aklz : sct::SctDocumentOutputWrapper::Raw};
    const auto neutral = sct::SctDocumentValidator::validateDocument(*document);
    appendDiagnostics(result, neutral.diagnostics, SctPipelineStage::Validation, baseline->source.locator);
    const auto target = sct::SctDocumentValidator::validateForTarget(*document, options.targetPlatform,
        options.textEncoding, &program.evidence(), neutral.receipt ? &*neutral.receipt : nullptr);
    appendDiagnostics(result, target.diagnostics, SctPipelineStage::Validation, baseline->source.locator);
    if (!neutral.validDocument || !target.validForTarget) { result.readiness = sct::SctDocumentReadiness::Inspectable; return result; }
    result.readiness = sct::SctDocumentReadiness::StructurallyValid;
    if (std::ranges::any_of(result.diagnostics, [](const auto& d) { return d.severity == DiagnosticSeverity::Error; })) return result;
    if (stop.stop_requested()) return result;
    const bool reuse = mode == SctAuthoringOutputMode::ReuseUnchangedSource && region.value()->literalOverrides.empty();
    std::vector<std::uint8_t> bytes;
    std::optional<sct::SctDocumentLayout> layout;
    std::optional<sct::SctPreservationReport> preservation;
    if (reuse) {
        const auto assessed = sct::SctDocumentLayoutEngine::layout(*document, options, &program.evidence(), target.receipt ? &*target.receipt : nullptr);
        appendDiagnostics(result, assessed.diagnostics, SctPipelineStage::Validation, baseline->source.locator);
        if (!assessed.success) return result;
        const auto original = bytesOf(program.bytes()); bytes.assign(original.begin(), original.end());
    } else {
        auto exported = sct::SctDocumentExporter::exportDocument(*document, options, &program.evidence(), target.receipt ? &*target.receipt : nullptr);
        appendDiagnostics(result, exported.diagnostics, SctPipelineStage::Publication, baseline->source.locator);
        if (!exported.success) return result;
        bytes = std::move(exported.bytes); layout = std::move(exported.layout); preservation = std::move(exported.preservation);
    }
    if (stop.stop_requested()) return result;
    auto parsed = sct::SctParser{}.parse(bytes);
    if (!parsed.parseOk || std::ranges::any_of(parsed.diagnostics,
        [](const auto& d) { return d.severity == sct::SctDiagnosticSeverity::Error; })) {
        result.infrastructureDiagnostics.push_back(error("Prepared output failed reparsing.")); return result;
    }
    sct::SctDocumentImportOptions importOptions;
    importOptions.declaredSourcePlatform = options.targetPlatform;
    if (baseline->textConvention) importOptions.sourceTextEncoding = options.textEncoding;
    importOptions.footerTextPromotion = baseline->recipe.trustSelectedTextEncoding
        ? sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding : sct::SctFooterTextPromotionPolicy::PreserveAmbiguous;
    auto verified = sct::SctDocumentWorkflow::importForEditing(parsed, importOptions);
    appendDiagnostics(result, verified.import.diagnostics, SctPipelineStage::Validation, baseline->source.locator);
    appendDiagnostics(result, verified.documentValidation.diagnostics, SctPipelineStage::Validation, baseline->source.locator);
    if (!verified.import.document || !verified.documentValidation.validDocument) return result;
    // Successful parsing/validation is insufficient: layout must not make opaque
    // bytes executable by moving an indexed section start across a preserved gap.
    const auto& reparsedDocument = *verified.import.document;
    bool sameInstructionShape = document->sections.size() == reparsedDocument.sections.size();
    for (std::size_t i = 0; sameInstructionShape && i < document->sections.size(); ++i) {
        const auto& expected = document->sections[i];
        const auto& actual = reparsedDocument.sections[i];
        sameInstructionShape = expected.nameBytes == actual.nameBytes && expected.content.index() == actual.content.index();
        const auto* code = std::get_if<sct::SctScriptSectionContent>(&expected.content);
        if (!sameInstructionShape || !code) continue;
        const auto& other = std::get<sct::SctScriptSectionContent>(actual.content).instructions;
        sameInstructionShape = code->instructions.size() == other.size();
        for (std::size_t j = 0; sameInstructionShape && j < other.size(); ++j)
            sameInstructionShape = code->instructions[j].opcode == other[j].opcode;
    }
    if (!sameInstructionShape) {
        result.infrastructureDiagnostics.push_back(error("SPICE output changed the preserved section/instruction shape on reimport."));
        return result;
    }
    auto bound = verified.import.context.bind(verified.import.context.revisionProvenance());
    auto verifiedTarget = sct::SctDocumentValidator::validateForTarget(*verified.import.document,
        options.targetPlatform, options.textEncoding, bound ? &*bound : nullptr);
    appendDiagnostics(result, verifiedTarget.diagnostics, SctPipelineStage::Validation, baseline->source.locator);
    if (!verifiedTarget.validForTarget || !bound || !bound->receipt().sourceMap.hasCompleteLeafCoverage()
        || std::ranges::any_of(result.diagnostics, [](const auto& d) { return d.severity == DiagnosticSeverity::Error; })) return result;
    auto digest = sha256(std::as_bytes(std::span(bytes)));
    if (!digest) { result.infrastructureDiagnostics = digest.diagnostics(); return result; }
    auto identity = realizationId(project, scriptId, mode, digest.value());
    if (!identity) { result.infrastructureDiagnostics = identity.diagnostics(); return result; }
    result.prepared.emplace(SctPreparedAuthoringOutput{std::move(bytes), document,
        mapping(project, scriptId, region.value()->id, identity.value(), *document), digest.value(), reuse,
        reuse ? std::optional{receipt.sourceMap} : std::nullopt, std::move(layout), std::move(preservation)});
    result.readiness = sct::SctDocumentReadiness::ExportReady;
    return result;
}
} // namespace

SctImportedProgram::SctImportedProgram(SctAuthoringProjectId project, SctAuthoringBaseline baseline,
    std::vector<std::byte> bytes, spice::sct::SctDocument document, spice::sct::SctBoundImportEvidence evidence,
    std::vector<SctPipelineDiagnostic> diagnostics, spice::sct::SctDocumentReadiness readiness)
    : project_(std::move(project)), baseline_(std::move(baseline)), bytes_(std::move(bytes)), document_(std::move(document)),
      evidence_(std::move(evidence)), diagnostics_(std::move(diagnostics)), readiness_(readiness) {}

Result<SctAuthoringAdoption> SctAuthoringImporter::import(const SctAuthoringProject& project,
    RevisionId expectedRevision, SctAuthoringImportRequest request, std::stop_token stop) {
    auto diagnostics = project.validate();
    if (hasErrors(diagnostics)) return Result<SctAuthoringAdoption>::failure(std::move(diagnostics));
    if (expectedRevision != project.revision || project.revision.value == UINT64_MAX)
        return Result<SctAuthoringAdoption>::failure(error("Import revision is stale or exhausted."));
    if (stop.stop_requested()) return Result<SctAuthoringAdoption>::failure(cancelled());
    if (!validPlatform(request.recipe.platform) || (request.dataset.platform && request.dataset.platform != request.recipe.platform)
        || (request.recipe.trustSelectedTextEncoding && !request.textConvention)
        || (request.textConvention && !sct::findSctKnownTextConvention(*request.textConvention)))
        return Result<SctAuthoringAdoption>::failure(error("Invalid or conflicting import profile/encoding selection."));
    auto hash = sha256(request.source.bytes);
    if (!hash) return Result<SctAuthoringAdoption>::failure(hash.diagnostics());
    if (request.source.descriptor.byteSize != request.source.bytes.size() || hash.value() != request.source.descriptor.revision.digest)
        return Result<SctAuthoringAdoption>::failure(error("Source snapshot does not match its size/SHA-256 identity.", DiagnosticCode::SourceChanged));
    auto parsed = sct::SctParser{}.parse(bytesOf(request.source.bytes), request.source.descriptor.locator.identityKey());
    if (!parsed.parseOk) return Result<SctAuthoringAdoption>::failure(error("Source SCT could not be parsed.", DiagnosticCode::SctParseFailed));
    if (stop.stop_requested()) return Result<SctAuthoringAdoption>::failure(cancelled());
    sct::SctDocumentImportOptions options;
    if (request.recipe.platform) options.declaredSourcePlatform = platform(*request.recipe.platform);
    if (request.textConvention) options.sourceTextEncoding = sct::sctTextEncodingFor(*request.textConvention);
    options.footerTextPromotion = request.recipe.trustSelectedTextEncoding
        ? sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding : sct::SctFooterTextPromotionPolicy::PreserveAmbiguous;
    auto imported = sct::SctDocumentWorkflow::importForEditing(parsed, options);
    if (!imported.import.document) return Result<SctAuthoringAdoption>::failure(error("Source SCT has no usable imported document.", DiagnosticCode::SctImportFailed));
    auto bound = imported.import.context.bind(imported.import.context.revisionProvenance());
    if (!bound) return Result<SctAuthoringAdoption>::failure(error("Import evidence could not be bound."));
    std::vector<SctPipelineDiagnostic> pipeline;
    for (const auto& d : parsed.diagnostics) {
        SctPipelineDiagnostic item;
        item.severity = d.severity == sct::SctDiagnosticSeverity::Error ? DiagnosticSeverity::Error
            : d.severity == sct::SctDiagnosticSeverity::Warning ? DiagnosticSeverity::Warning : DiagnosticSeverity::Info;
        item.stage = SctPipelineStage::Parse; item.code = "ParserDiagnostic"; item.message = d.message;
        item.locator = request.source.descriptor.locator; item.payloadOffset = d.offset; pipeline.push_back(std::move(item));
    }
    for (const auto& d : imported.import.diagnostics) pipeline.push_back(convertSctDiagnostic(d, SctPipelineStage::Import, request.source.descriptor.locator));
    for (const auto& d : imported.documentValidation.diagnostics) pipeline.push_back(convertSctDiagnostic(d, SctPipelineStage::Validation, request.source.descriptor.locator));
    auto next = project;
    auto baselineId = next.allocate<SctBaselineId>(); auto scriptId = next.allocate<SctScriptId>(); auto contentId = next.allocate<SctContentId>();
    if (!baselineId || !scriptId || !contentId) return Result<SctAuthoringAdoption>::failure(error("Authoring IDs exhausted."));
    auto uuid = generateSctAuthoringUuid();
    if (!uuid) return Result<SctAuthoringAdoption>::failure(uuid.diagnostics());
    SctAuthoringBaseline baseline{baselineId.value(), request.dataset.fingerprint, request.source.descriptor,
        {uuid.value()}, request.textConvention, {{SctEvidenceKind::SourceObservation, "Imported through SPICE with bound source evidence."}}, request.recipe};
    next.baselines.push_back(baseline);
    next.scripts.push_back({scriptId.value(), std::move(request.name), baseline.id, request.recipe.platform,
        request.dataset.region, {}, {contentId.value()}});
    next.contents.push_back({contentId.value(), scriptId.value(), {baseline.id, baseline.importedDocument, SctWholeDocument{}}, {}, {}});
    ++next.revision.value;
    diagnostics = next.validate();
    if (hasErrors(diagnostics)) return Result<SctAuthoringAdoption>::failure(std::move(diagnostics));
    if (stop.stop_requested()) return Result<SctAuthoringAdoption>::failure(cancelled());
    auto program = std::shared_ptr<const SctImportedProgram>(new SctImportedProgram(project.id, std::move(baseline),
        std::move(request.source.bytes), std::move(*imported.import.document), *bound, std::move(pipeline), imported.readiness));
    return Result<SctAuthoringAdoption>::success({std::move(next), std::move(program), scriptId.value(), contentId.value()}, std::move(diagnostics));
}

Result<SctLiteralConstant> SctPreservedEditService::inspectLiteral(const spice::sct::SctDocument& document,
    const spice::sct::SctParameterSite& site, std::uint16_t expectedOpcode, const SctLiteralConstant& replacement) {
    const auto index = sct::SctDocumentIndex::build(document);
    const auto* instruction = index.find(document, site.instruction);
    if (!instruction || instruction->opcode != expectedOpcode || site.parameter.repeatedGroupOrdinal)
        return Result<SctLiteralConstant>::failure(error("Literal edit instruction, opcode, or fixed-parameter precondition failed."));
    const auto* schema = sct::findSctOpcodeSchema(instruction->opcode);
    if (!schema || schema->binaryShapeConfidence != sct::SctBinaryShapeConfidence::Confirmed
        || schema->documentRole != sct::SctOpcodeDocumentRole::Instruction)
        return Result<SctLiteralConstant>::failure(error("Literal edit requires a confirmed instruction binary shape."));
    const auto catalog = std::span(schema->parameterCatalog).first(schema->parameterCatalogCount);
    const auto found = std::ranges::find(catalog, site.parameter.schemaIndex, &sct::SctOpcodeParameterSchema::schemaIndex);
    if (found == catalog.end() || found->encoding != sct::SctOpcodeParameterEncoding::ScptExpression
        || found->referenceKind != sct::SctOpcodeReferenceKind::None
        || found->belongsToRepeatedGroup || found->terminator
        || found->defaultKind == sct::SctOpcodeDefaultKind::DerivedRepeatedGroupCount
        || found->defaultKind == sct::SctOpcodeDefaultKind::DerivedInstructionByteLength)
        return Result<SctLiteralConstant>::failure(error("Parameter is outside the fixed SCPT literal-edit capability."));
    const auto parameter = std::ranges::find(instruction->fixedParameters, site.parameter.schemaIndex, &sct::SctDocumentParameter::schemaIndex);
    if (parameter == instruction->fixedParameters.end() || !std::holds_alternative<sct::SctCanonicalExpression>(parameter->value))
        return Result<SctLiteralConstant>::failure(error("Parameter is not an existing SCPT literal."));
    const auto current = SctLiteralConstant::fromExpression(std::get<sct::SctCanonicalExpression>(parameter->value));
    if (!current || !replacement.valid() || current->operation.kind != replacement.operation.kind
        || current->termination != replacement.termination)
        return Result<SctLiteralConstant>::failure(error("Literal edits must retain a single literal's encoding family, width, and termination."));
    return Result<SctLiteralConstant>::success(*current);
}
Result<SctPreservedEditResult> SctPreservedEditService::replaceLiteral(const SctAuthoringProject& project,
    const SctImportedPrograms& programs, const SctPreservedEditRequest& request) {
    auto diagnostics = project.validate();
    if (hasErrors(diagnostics)) return Result<SctPreservedEditResult>::failure(std::move(diagnostics));
    if (project.revision != request.expectedRevision) return Result<SctPreservedEditResult>::failure(error("Literal edit revision is stale."));
    const auto* content = project.find(request.content);
    if (!content) return Result<SctPreservedEditResult>::failure(error("Literal edit content is absent."));
    const auto script = *project.effectiveScript(content->owner);
    auto resolved = resolve(project, programs, *project.find(content->region.baseline));
    if (!resolved) return Result<SctPreservedEditResult>::failure(resolved.diagnostics());
    auto region = wholeRegion(project, script, *resolved.value());
    if (!region) return Result<SctPreservedEditResult>::failure(region.diagnostics());
    auto current = replay(*resolved.value(), *content);
    if (!current) return Result<SctPreservedEditResult>::failure(current.diagnostics());
    auto currentLiteral = inspectLiteral(current.value(), request.site, request.expectedOpcode, request.value);
    if (!currentLiteral) return Result<SctPreservedEditResult>::failure(currentLiteral.diagnostics());
    if (currentLiteral.value() != request.expectedValue) return Result<SctPreservedEditResult>::failure(error("Literal edit current value is stale."));
    if (request.value == request.expectedValue) return Result<SctPreservedEditResult>::success({project, {}});
    if (project.revision.value == UINT64_MAX) return Result<SctPreservedEditResult>::failure(error("Project revision is exhausted."));
    auto baselineLiteral = inspectLiteral(resolved.value()->document(), request.site, request.expectedOpcode, request.value);
    if (!baselineLiteral) return Result<SctPreservedEditResult>::failure(baselineLiteral.diagnostics());
    auto next = project;
    auto& edits = std::ranges::find(next.contents, request.content, &SctAuthoringContent::id)->literalOverrides;
    std::erase_if(edits, [&](const auto& edit) { return edit.site == request.site; });
    if (request.value != baselineLiteral.value()) edits.push_back({request.site, request.expectedOpcode, baselineLiteral.value(), request.value});
    std::ranges::sort(edits, {}, &SctPreservedLiteralOverride::site);
    auto changed = replay(*resolved.value(), *next.find(request.content));
    if (!changed) return Result<SctPreservedEditResult>::failure(changed.diagnostics());
    const auto validated = sct::SctDocumentValidator::validateDocument(changed.value());
    if (!validated.validDocument) return Result<SctPreservedEditResult>::failure(error("Edited document failed structural validation."));
    ++next.revision.value;
    return Result<SctPreservedEditResult>::success({std::move(next), {script}});
}
bool SctAuthoringMaterializationResult::succeeded() const noexcept {
    return !cancelled && !hasErrors(diagnostics) && !scripts.empty()
        && std::ranges::all_of(scripts, [](const auto& script) { return script.prepared.has_value(); });
}
SctAuthoringMaterializationResult SctAuthoringMaterializer::materialize(
    const SctAuthoringMaterializationRequest& request, std::stop_token stop) {
    SctAuthoringMaterializationResult result;
    if (!request.project) { result.diagnostics.push_back(error("Captured project is absent.")); return result; }
    result.project = request.project->id; result.revision = request.project->revision;
    result.diagnostics = request.project->validate();
    if (hasErrors(result.diagnostics)) return result;
    if (request.mode != SctAuthoringOutputMode::ReuseUnchangedSource && request.mode != SctAuthoringOutputMode::Rebuild) {
        result.diagnostics.push_back(error("Invalid output preparation mode.")); return result;
    }
    auto scripts = request.scripts; std::ranges::sort(scripts);
    if (scripts.empty() || std::adjacent_find(scripts.begin(), scripts.end()) != scripts.end()) {
        result.diagnostics.push_back(error("Select a nonempty set of unique scripts.")); return result;
    }
    for (const auto script : scripts) {
        if (stop.stop_requested()) break;
        result.scripts.push_back(prepare(*request.project, request.programs, script, request.mode, stop));
    }
    if (stop.stop_requested()) {
        result.cancelled = true;
        for (auto& script : result.scripts) { script.prepared.reset(); script.readiness = sct::SctDocumentReadiness::Inspectable; }
    }
    return result;
}
} // namespace salsa::core
