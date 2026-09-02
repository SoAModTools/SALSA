#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctPresentation.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <stop_token>

namespace {
using namespace salsa::core;
using namespace spice::sct;

Sha256Digest zeroDigest() { return Sha256Digest(std::array<std::byte, Sha256Digest::Size>{}); }

AssetLocator locator() {
    auto result = AssetLocator::fromRelativePath("scripts/test.sct");
    EXPECT_TRUE(result);
    return std::move(result).takeValue();
}

std::vector<std::byte> inspectableSctBytes(const bool includeText) {
    SctDocumentBuilder builder;
    const auto sectionId = builder.allocateSectionId();
    SctInstructionFactoryRequest request;
    request.opcode = 12;
    const auto draft = SctInstructionFactory::createDraft(request);
    EXPECT_TRUE(draft.draft.has_value());
    const auto instruction = SctInstructionFactory::materialize(builder.document(), *draft.draft);
    EXPECT_TRUE(instruction.instruction.has_value());
    builder.document().sections.push_back({ sectionId, "SCRIPT",
        SctScriptSectionContent{{ *instruction.instruction }} });
    if (includeText) {
        builder.document().sections.push_back({ builder.allocateSectionId(), "MS0000001",
            SctStringSectionContent{ SctDocumentString{ builder.allocateStringId(),
                SctMessage{ std::nullopt, SctFormattedText{{ SctTextChunk{ "Hello" } }} },
                SctTextKind::SctString } } });
    }
    const auto document = std::move(builder).finish();
    const SctDocumentExportOptions options{ SctPlatform::GameCube,
        kSctShiftJisByte7FEncoding, SctDocumentOutputByteOrder::BigEndian,
        SctDocumentOutputWrapper::Raw, SctOpaquePreservationPolicy::RequirePreservation };
    const auto exported = SctDocumentExporter::exportDocument(document, options);
    EXPECT_TRUE(exported.success);
    std::vector<std::byte> bytes(exported.bytes.size());
    std::memcpy(bytes.data(), exported.bytes.data(), exported.bytes.size());
    return bytes;
}

SctParseResult importedGapParse() {
    SctParseResult parsed;
    parsed.parseOk = true;
    parsed.file.detectedEndian = "big";
    parsed.file.originalPayloadBytes.resize(56u, 0u);
    parsed.file.originalPayloadBytes[11] = 1u;
    parsed.file.originalPayloadBytes[16] = 'M';
    parsed.file.originalPayloadBytes[17] = 'A';
    parsed.file.originalPayloadBytes[18] = 'I';
    parsed.file.originalPayloadBytes[19] = 'N';
    std::fill(parsed.file.originalPayloadBytes.begin() + 40,
        parsed.file.originalPayloadBytes.begin() + 52, 0xacu);

    SctInstruction jump;
    jump.offset = 0u;
    jump.payloadOffset = 0u;
    jump.opcode = 10u;
    jump.rawWords = {10u, 12u};
    jump.parameters = {{0u, "offset", SctParameterValueKind::Link,
        SctSemanticConfidence::Known, {12u}}};
    jump.sizeBytes = 8u;
    jump.decodeOk = true;

    SctInstruction target;
    target.offset = 20u;
    target.payloadOffset = 20u;
    target.opcode = 12u;
    target.rawWords = {12u};
    target.sizeBytes = 4u;
    target.decodeOk = true;

    SctSection section;
    section.id = {0u, "MAIN"};
    section.startOffset = 32u;
    section.endOffset = 56u;
    section.kind = SctSectionKind::Script;
    section.instructions = {jump, target};
    section.edges.push_back({SctEdgeType::Jump, SctSemanticConfidence::Known,
        0u, 20u, 0u, 20u, 10u, "jump"});
    parsed.file.sections.push_back(std::move(section));
    return parsed;
}

class FakeCatalog final : public AssetCatalog {
public:
    FakeCatalog(AssetLocator assetLocator, std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)), descriptor_(AssetDescriptor{
            std::move(assetLocator), static_cast<std::uint64_t>(bytes_.size()),
            SourceRevision{ zeroDigest() } }),
          snapshot_(AssetCatalogSnapshot{ {}, DatasetFingerprint{ zeroDigest() } }) {
        snapshot_.assets.push_back(descriptor_);
    }
    const AssetCatalogSnapshot& snapshot() const noexcept override { return snapshot_; }
    Result<SourceAssetSnapshot> loadAsset(const AssetLocator& requested) const override {
        if (requested != descriptor_.locator) {
            return Result<SourceAssetSnapshot>::failure({ DiagnosticSeverity::Error,
                DiagnosticCode::AssetNotFound, "not found", requested.path() });
        }
        return Result<SourceAssetSnapshot>::success({ descriptor_, bytes_ });
    }
private:
    std::vector<std::byte> bytes_{};
    AssetDescriptor descriptor_;
    AssetCatalogSnapshot snapshot_;
};

class FakeProject final : public GameProjectContext {
public:
    FakeProject(AssetLocator assetLocator, std::vector<std::byte> bytes)
        : dataset_(DatasetContext{ {}, DatasetIdentity{ std::nullopt, std::nullopt,
              DatasetFingerprint{ zeroDigest() } } }),
          catalog_(std::move(assetLocator), std::move(bytes)) {}
    const DatasetContext& dataset() const noexcept override { return dataset_; }
    const AssetCatalog& assets() const noexcept override { return catalog_; }
private:
    DatasetContext dataset_;
    FakeCatalog catalog_;
};

SctDocumentSnapshot snapshotWithEvidence(
    SctDocument document, const SctBoundImportEvidence& evidence) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    EXPECT_TRUE(loaded.succeeded());
    auto provenance = std::make_shared<SctDocumentProvenance>(
        *loaded.document->provenance);
    provenance->importEvidence = evidence;
    auto documentPtr = std::make_shared<const SctDocument>(std::move(document));
    auto analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*documentPtr, &evidence));
    return {std::move(provenance), std::move(documentPtr), std::move(analysis),
        SctDocumentReadiness::StructurallyValid, {}};
}

void collectProperties(
    const SctPropertyItem& property,
    const std::string_view name,
    std::vector<const SctPropertyItem*>& matches) {
    if (property.name == name
        || (property.name.size() > name.size()
            && property.name.ends_with(name)
            && property.name[property.name.size() - name.size() - 1u] == ' ')) {
        matches.push_back(&property);
    }
    for (const auto& child : property.children) collectProperties(child, name, matches);
}

std::vector<const SctPropertyItem*> propertiesNamed(
    const SctEntityPresentation& presentation,
    const std::string_view name) {
    std::vector<const SctPropertyItem*> matches;
    for (const auto& property : presentation.properties)
        collectProperties(property, name, matches);
    return matches;
}

std::vector<const SctPropertyItem*> orderedOperationPropertiesNamed(
    const SctEntityPresentation& presentation,
    const std::string_view name) {
    std::vector<const SctPropertyItem*> matches;
    const auto orderedGroups = propertiesNamed(presentation, "Ordered operations");
    for (const auto* group : orderedGroups) {
        for (const auto& child : group->children) {
            if (child.name == name || child.name.ends_with(name))
                matches.push_back(&child);
        }
    }
    return matches;
}

SctEntityPresentation expressionPresentation(
    const std::vector<SctScptOperation>& operations) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    EXPECT_TRUE(loaded.succeeded());
    auto document = std::make_shared<SctDocument>(*loaded.document->document);
    auto& script = std::get<SctScriptSectionContent>(document->sections.front().content);
    SctDocumentInstruction instruction{document->allocateInstructionId(), 125};
    for (std::uint32_t index = 0; index < operations.size(); ++index) {
        instruction.fixedParameters.push_back({index,
            SctCanonicalExpression{SctTypedScptProgram{{operations[index]}},
                SctExpressionTermination::StopCode}});
    }
    const auto instructionId = instruction.id;
    script.instructions.insert(script.instructions.begin(), std::move(instruction));
    auto snapshot = *loaded.document;
    snapshot.document = std::move(document);
    snapshot.analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*snapshot.document,
            snapshot.provenance->importEvidence
                ? &*snapshot.provenance->importEvidence : nullptr));
    return SctPresentationService::describe(
        snapshot, {SctNavigationKind::Instruction, instructionId.value()});
}

SctEntityPresentation expressionProgramPresentation(
    std::vector<SctScptOperation> operations) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    EXPECT_TRUE(loaded.succeeded());
    auto document = std::make_shared<SctDocument>(*loaded.document->document);
    auto& script = std::get<SctScriptSectionContent>(document->sections.front().content);
    SctDocumentInstruction instruction{document->allocateInstructionId(), 125};
    instruction.fixedParameters.push_back({0u, SctCanonicalExpression{
        SctTypedScptProgram{std::move(operations)}, SctExpressionTermination::StopCode}});
    const auto instructionId = instruction.id;
    script.instructions.insert(script.instructions.begin(), std::move(instruction));
    auto snapshot = *loaded.document;
    snapshot.document = std::move(document);
    snapshot.analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*snapshot.document,
            snapshot.provenance->importEvidence
                ? &*snapshot.provenance->importEvidence : nullptr));
    return SctPresentationService::describe(
        snapshot, {SctNavigationKind::Instruction, instructionId.value()});
}
}  // namespace

TEST(SctDocumentLoader, LoadsNoTextDocumentWithoutInventingAConvention) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    ASSERT_NE(loaded.inspection, nullptr);
    EXPECT_TRUE(loaded.inspection->textAssessment.records.empty());
    EXPECT_FALSE(loaded.document->provenance->textConvention.has_value());
    EXPECT_EQ(loaded.document->provenance->textSelectionOrigin, SctTextSelectionOrigin::None);
    ASSERT_TRUE(loaded.document->provenance->importEvidence.has_value());
    ASSERT_NE(loaded.document->analysis, nullptr);
    EXPECT_FALSE(loaded.document->analysis->structuredControlFlow.sections().empty());
    EXPECT_TRUE(loaded.document->analysis->importedSites.has_value());
    ASSERT_EQ(loaded.document->document->sections.size(), 1u);
}

TEST(SctDocumentLoader, AutomaticConventionSelectionFollowsV3RecommendationStatus) {
    SctSourceTextAssessment assessment;
    assessment.records.push_back({});
    for (const auto status : {SctSourceTextRecommendationStatus::Ambiguous,
            SctSourceTextRecommendationStatus::Conflicting,
            SctSourceTextRecommendationStatus::InsufficientEvidence}) {
        assessment.recommendation.status = status;
        assessment.recommendation.convention = SctKnownTextConvention::ShiftJisByte7F;
        EXPECT_FALSE(recommendedSctTextConvention(assessment).has_value());
    }
    assessment.recommendation.status = SctSourceTextRecommendationStatus::Unique;
    assessment.recommendation.convention = SctKnownTextConvention::Windows1252Byte7F;
    EXPECT_EQ(recommendedSctTextConvention(assessment),
        SctKnownTextConvention::Windows1252Byte7F);
    assessment.records.clear();
    EXPECT_FALSE(recommendedSctTextConvention(assessment).has_value());
}

TEST(SctDocumentLoader, BoundImportEvidenceAndAnalysisOutliveTheLoadResult) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    std::shared_ptr<const SctDocumentSnapshot> snapshot;
    {
        auto loaded = SctDocumentLoader::load(project, asset);
        ASSERT_TRUE(loaded.succeeded());
        snapshot = std::move(loaded.document);
    }
    ASSERT_TRUE(snapshot->provenance->importEvidence.has_value());
    EXPECT_GT(snapshot->provenance->importReceipt()->sourceMap.decodedPayloadSize(), 0u);
    ASSERT_NE(snapshot->analysis, nullptr);
    EXPECT_EQ(snapshot->analysis->entities.find(*snapshot->document,
        std::get<SctScriptSectionContent>(snapshot->document->sections.front().content)
            .instructions.front().id)->opcode, 12u);

    SctDocument unrelated;
    const auto instruction = std::get<SctScriptSectionContent>(
        snapshot->document->sections.front().content).instructions.front().id;
    EXPECT_EQ(snapshot->analysis->entities.find(unrelated, instruction), nullptr);
}

TEST(SctDiagnostics, PreservesExactPrimaryAndRelatedV3Locations) {
    const SctParameterSite primary{SctInstructionId{7}, {3u, 2u}};
    const SctExpressionSite related{SctInstructionId{7},
        SctScheduledExpressionSite{}};
    SctDocumentDiagnostic source;
    source.severity = SctDiagnosticSeverity::Warning;
    source.code = SctDiagnosticCode::ExpressionRuntimeStackDepth;
    source.message = "runtime stack evidence";
    source.primaryLocation = SctDiagnosticLocation{primary};
    source.relatedLocations = {SctDiagnosticLocation{related},
        SctDiagnosticLocation{SctDraftExpressionOperationSite{
            SctDraftExpressionSite{10u,
                SctExpressionOwner{SctScheduledExpressionSite{}}}, 2u}}};

    const auto converted = convertSctDiagnostic(
        source, SctPipelineStage::Validation, locator());
    EXPECT_EQ(converted.code, "ExpressionRuntimeStackDepth");
    EXPECT_EQ(converted.primaryLocation, source.primaryLocation);
    EXPECT_EQ(converted.relatedLocations, source.relatedLocations);
    EXPECT_FALSE(converted.target.has_value());
    const auto location = inspectionLocationForDiagnostic(*converted.primaryLocation);
    ASSERT_TRUE(location.has_value());
    const auto* parameter = std::get_if<SctParameterSite>(&*location);
    ASSERT_NE(parameter, nullptr);
    EXPECT_EQ(*parameter, primary);
    EXPECT_EQ(formatSctDiagnosticLocation(*converted.primaryLocation),
        "instruction 7, parameter 3 group 2");
    EXPECT_EQ(formatSctDiagnosticLocation(converted.relatedLocations.back()),
        "draft opcode 10 scheduled expression, operation 2");
}

TEST(SctDocumentLoader, AmbiguousTextStaysOpaqueUntilExplicitReimport) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(true));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    ASSERT_FALSE(loaded.inspection->textAssessment.records.empty());
    EXPECT_FALSE(loaded.document->provenance->textConvention.has_value());
    const auto* parsedAddress = loaded.inspection->parsed.get();
    const auto reimported = SctDocumentLoader::materialize(loaded.inspection,
        SctKnownTextConvention::ShiftJisByte7F, SctTextSelectionOrigin::UserSelected);
    ASSERT_TRUE(reimported.succeeded());
    EXPECT_EQ(reimported.inspection->parsed.get(), parsedAddress);
    EXPECT_EQ(reimported.document->provenance->textConvention,
        SctKnownTextConvention::ShiftJisByte7F);
    EXPECT_EQ(reimported.document->provenance->textSelectionOrigin,
        SctTextSelectionOrigin::UserSelected);
    EXPECT_EQ(loaded.document->provenance->importReceipt()->footerTextPromotion,
        SctFooterTextPromotionPolicy::PreserveAmbiguous);
    EXPECT_EQ(reimported.document->provenance->importReceipt()->footerTextPromotion,
        SctFooterTextPromotionPolicy::TrustSelectedEncoding);
    ASSERT_EQ(reimported.document->document->sections.size(), 2u);
    const auto& string = std::get<SctStringSectionContent>(
        reimported.document->document->sections.back().content).string;
    EXPECT_TRUE(std::holds_alternative<SctMessage>(string.value));
}

TEST(SctDocumentLoader, HonoursCancellationBeforeReadingTheAsset) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    std::stop_source stop;
    stop.request_stop();
    const auto loaded = SctDocumentLoader::load(project, asset, stop.get_token());
    EXPECT_TRUE(loaded.cancelled);
    EXPECT_FALSE(loaded.succeeded());
    ASSERT_EQ(loaded.infrastructureDiagnostics.size(), 1u);
    EXPECT_EQ(loaded.infrastructureDiagnostics.front().code, DiagnosticCode::Cancelled);
}

TEST(SctPresentation, ProjectsPhysicalOutlineAndInstructionDetails) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    const auto outline = SctPresentationService::outline(*loaded.document);
    ASSERT_GE(outline.size(), 4u);
    const auto section = std::ranges::find_if(outline, [](const auto& item) {
        return item.target.kind == SctNavigationKind::Section;
    });
    ASSERT_NE(section, outline.end());
    ASSERT_EQ(section->children.size(), 1u);
    EXPECT_EQ(section->children.front().target.kind, SctNavigationKind::Instruction);
    const auto details = SctPresentationService::describe(
        *loaded.document, section->children.front().target);
    EXPECT_NE(details.title.find("Return"), std::string::npos);
    EXPECT_FALSE(details.properties.empty());
}

TEST(SctPresentation, DisplaysSemanticExpressionValuesWithRawEncodingEvidence) {
    SctScptValueOperation positiveFloat{SctScptValueKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(1.5f)}};
    SctScptValueOperation negativeFloat{SctScptValueKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(-12.25f)}};
    SctScptValueOperation decimal{SctScptValueKind::DecimalLiteral,
        0x08000380u, {}};
    SctScptValueOperation variable{SctScptValueKind::BitVariable,
        0x2000002au, {}};
    SctScptValueOperation negated{SctScptValueKind::NegatedIntVariable,
        0x5000002bu, {}};
    SctScptValueOperation low16{SctScptValueKind::NegatedIntVariableLow16Comparison,
        0x5000002cu, {}};
    SctScptBinaryOperation add{SctScptBinaryOperationKind::Arithmetic, 0x0eu};

    const auto presentation = expressionPresentation(
        {positiveFloat, negativeFloat, decimal, variable, negated, low16, add});
    const auto floats = orderedOperationPropertiesNamed(presentation, "Float literal");
    ASSERT_EQ(floats.size(), 2u);
    EXPECT_EQ(floats[0]->value, "1.5");
    EXPECT_EQ(floats[1]->value, "-12.25");
    EXPECT_NE(floats[0]->notes.find("Encoding 0x04000000"), std::string::npos);
    EXPECT_NE(floats[0]->notes.find("payload 0x3FC00000"), std::string::npos);

    const auto decimals = orderedOperationPropertiesNamed(presentation, "Decimal literal");
    ASSERT_EQ(decimals.size(), 1u);
    EXPECT_EQ(decimals.front()->value, "3.5");
    EXPECT_NE(decimals.front()->notes.find("0x08000380"), std::string::npos);
    const auto variables = orderedOperationPropertiesNamed(presentation, "Bit variable");
    ASSERT_EQ(variables.size(), 1u);
    EXPECT_EQ(variables.front()->value, "42");
    EXPECT_NE(variables.front()->notes.find("0x2000002A"), std::string::npos);
    const auto negatedVariables = orderedOperationPropertiesNamed(
        presentation, "Negated integer variable");
    ASSERT_EQ(negatedVariables.size(), 1u);
    EXPECT_EQ(negatedVariables.front()->value, "43");
    const auto low16Variables = orderedOperationPropertiesNamed(
        presentation, "Negated integer variable (low-16 comparison)");
    ASSERT_EQ(low16Variables.size(), 1u);
    EXPECT_EQ(low16Variables.front()->value, "44");
    const auto operators = orderedOperationPropertiesNamed(presentation, "Arithmetic");
    ASSERT_EQ(operators.size(), 1u);
    EXPECT_EQ(operators.front()->value, "+");
    EXPECT_NE(operators.front()->notes.find("0x0000000E"), std::string::npos);
}

TEST(SctPresentation, SeparatesAuthoritativeScptProgramsFromDerivedResults) {
    const auto conventional = expressionProgramPresentation({
        SctScptValueOperation{SctScptValueKind::DecimalLiteral, 0x08000100u, {}},
        SctScptValueOperation{SctScptValueKind::BitVariable, 0x20000007u, {}},
        SctScptBinaryOperation{SctScptBinaryOperationKind::Arithmetic, 0x0eu},
    });
    EXPECT_EQ(propertiesNamed(conventional, "Ordered operations").size(), 1u);
    EXPECT_EQ(propertiesNamed(conventional, "Conventional result").size(), 1u);

    const auto nonstandard = expressionProgramPresentation({
        SctScptValueOperation{SctScptValueKind::DecimalLiteral, 0x08000100u, {}},
        SctScptValueOperation{SctScptValueKind::BitVariable, 0x20000007u, {}},
        SctScptStackOverwritePreviousWithTopOperation{},
        SctScptInertOperation{},
    });
    const auto ordered = propertiesNamed(nonstandard, "Ordered operations");
    ASSERT_EQ(ordered.size(), 1u);
    EXPECT_EQ(ordered.front()->value, "4");
    EXPECT_EQ(orderedOperationPropertiesNamed(
        nonstandard, "Stack overwrite").size(), 1u);
    EXPECT_EQ(orderedOperationPropertiesNamed(
        nonstandard, "Inert operation").size(), 1u);
}

TEST(SctPresentation, ShowsV3IndexedStringGroupMarkersAndCurrentGrouping) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    auto document = std::make_shared<SctDocument>();
    const auto marker = document->allocateSectionId();
    const auto member = document->allocateSectionId();
    const auto string = document->allocateStringId();
    document->sections.push_back({marker, "GROUP",
        SctStringGroupMarkerSectionContent{{9u, 0x1du}}});
    document->sections.push_back({member, "MS0000001",
        SctStringSectionContent{{string, SctPlainText{"hello"},
            SctTextKind::PlainString}}});
    auto snapshot = *loaded.document;
    snapshot.document = std::move(document);
    snapshot.analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*snapshot.document));

    const auto outline = SctPresentationService::outline(snapshot);
    const auto markerRow = std::ranges::find_if(outline, [marker](const auto& row) {
        return row.target == SctNavigationTarget{
            SctNavigationKind::Section, marker.value()};
    });
    ASSERT_NE(markerRow, outline.end());
    EXPECT_EQ(markerRow->secondary, "String group marker");
    const auto details = SctPresentationService::describe(snapshot, markerRow->target);
    const auto preamble = propertiesNamed(details, "Preamble words");
    ASSERT_EQ(preamble.size(), 1u);
    EXPECT_NE(preamble.front()->value.find("0x0000001D"), std::string::npos);
    const auto groups = propertiesNamed(details, "Current indexed-string group");
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups.front()->notes, "explicit marker");
    ASSERT_EQ(groups.front()->children.size(), 3u);
}

TEST(SctPresentation, AttachesExactParameterAndExpressionInspectionLocations) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    auto document = std::make_shared<SctDocument>(*loaded.document->document);
    auto& instruction = std::get<SctScriptSectionContent>(
        document->sections.front().content).instructions.front();

    instruction.scheduledExpression = SctCanonicalExpression{SctTypedScptProgram{{
        SctScptValueOperation{SctScptValueKind::DirectIntVariable,
            0x10000011u, {}}}}, SctExpressionTermination::InlineValue};
    instruction.fixedParameters.push_back({ 20u, SctCanonicalExpression{
        SctTypedScptProgram{{SctScptValueOperation{SctScptValueKind::FloatVariable,
            0x14000022u, {}}}}, SctExpressionTermination::StopCode } });
    instruction.repeatedParameterGroups.push_back({ {
        { 30u, SctCanonicalExpression{
            SctTypedScptProgram{{
                SctScptValueOperation{SctScptValueKind::ByteVariable,
                    0x24000033u, {}},
                SctScptValueOperation{SctScptValueKind::BitVariable,
                    0x20000044u, {}}}},
            SctExpressionTermination::InlineValue } },
        { 31u, SctCanonicalExpression{
            SctOpaqueExpression{ { 0xdeadbeefu } },
            SctExpressionTermination::StopCode } },
    } });

    auto snapshot = *loaded.document;
    snapshot.document = std::move(document);
    snapshot.analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*snapshot.document,
            snapshot.provenance->importEvidence
                ? &*snapshot.provenance->importEvidence : nullptr));
    const auto presentation = SctPresentationService::describe(snapshot,
        { SctNavigationKind::Instruction, instruction.id.value() });

    const auto scheduled = orderedOperationPropertiesNamed(
        presentation, "Integer variable");
    ASSERT_EQ(scheduled.size(), 1u);
    ASSERT_TRUE(scheduled.front()->location.has_value());
    const auto* scheduledSite = std::get_if<SctExpressionOperationSite>(
        &*scheduled.front()->location);
    ASSERT_NE(scheduledSite, nullptr);
    EXPECT_EQ(scheduledSite->expression.instruction, instruction.id);
    EXPECT_TRUE(std::holds_alternative<SctScheduledExpressionSite>(
        scheduledSite->expression.owner));
    EXPECT_EQ(scheduledSite->operationOrdinal, 0u);

    const auto fixedParameters = propertiesNamed(presentation, "Parameter 20");
    ASSERT_EQ(fixedParameters.size(), 1u);
    ASSERT_TRUE(fixedParameters.front()->location.has_value());
    const auto* fixedParameter = std::get_if<SctParameterSite>(&*fixedParameters.front()->location);
    ASSERT_NE(fixedParameter, nullptr);
    EXPECT_EQ(fixedParameter->parameter.schemaIndex, 20u);
    EXPECT_FALSE(fixedParameter->parameter.repeatedGroupOrdinal.has_value());

    const auto repeatedParameters = propertiesNamed(presentation, "Parameter 30");
    ASSERT_EQ(repeatedParameters.size(), 1u);
    ASSERT_TRUE(repeatedParameters.front()->location.has_value());
    const auto* repeatedParameter = std::get_if<SctParameterSite>(
        &*repeatedParameters.front()->location);
    ASSERT_NE(repeatedParameter, nullptr);
    EXPECT_EQ(repeatedParameter->parameter.repeatedGroupOrdinal, 0u);

    const auto repeatedRoot = orderedOperationPropertiesNamed(presentation, "Byte variable");
    ASSERT_EQ(repeatedRoot.size(), 1u);
    const auto* repeatedRootSite = std::get_if<SctExpressionOperationSite>(
        &*repeatedRoot.front()->location);
    ASSERT_NE(repeatedRootSite, nullptr);
    ASSERT_TRUE(std::holds_alternative<SctParameterAddress>(
        repeatedRootSite->expression.owner));
    EXPECT_EQ(std::get<SctParameterAddress>(
        repeatedRootSite->expression.owner).schemaIndex, 30u);
    EXPECT_EQ(std::get<SctParameterAddress>(
        repeatedRootSite->expression.owner).repeatedGroupOrdinal, 0u);
    EXPECT_EQ(repeatedRootSite->operationOrdinal, 0u);

    const auto nested = orderedOperationPropertiesNamed(presentation, "Bit variable");
    ASSERT_EQ(nested.size(), 1u);
    const auto* nestedSite = std::get_if<SctExpressionOperationSite>(
        &*nested.front()->location);
    ASSERT_NE(nestedSite, nullptr);
    EXPECT_EQ(nestedSite->operationOrdinal, 1u);

    const auto opaque = propertiesNamed(presentation, "Opaque words");
    ASSERT_EQ(opaque.size(), 1u);
    const auto* opaqueSite = std::get_if<SctExpressionSite>(&*opaque.front()->location);
    ASSERT_NE(opaqueSite, nullptr);
    ASSERT_TRUE(std::holds_alternative<SctParameterAddress>(opaqueSite->owner));
    EXPECT_EQ(std::get<SctParameterAddress>(opaqueSite->owner).schemaIndex, 31u);
}

TEST(SctPresentation, ExposesOpaqueAttachmentInstructionAnchorsForNavigation) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    auto document = std::make_shared<SctDocument>(*loaded.document->document);
    const auto instruction = std::get<SctScriptSectionContent>(
        document->sections.front().content).instructions.front().id;
    const auto attachment = document->allocateOpaqueAttachmentId();
    document->opaqueAttachments.push_back({ attachment, { 0x11u, 0x22u },
        SctOpaqueAnchor{ instruction }, SctOpaquePlacement::After, std::nullopt,
        1u, SctOpaqueRelocationSupport::Relocatable, SctOpaqueReason::Gap });

    auto snapshot = *loaded.document;
    snapshot.document = std::move(document);
    snapshot.analysis = std::make_shared<const SctDocumentAnalysis>(
        SctDocumentAnalysis::build(*snapshot.document,
            snapshot.provenance->importEvidence
                ? &*snapshot.provenance->importEvidence : nullptr));
    const auto presentation = SctPresentationService::describe(snapshot,
        { SctNavigationKind::OpaqueAttachment, attachment.value() });
    const auto anchors = propertiesNamed(presentation, "Anchor");
    ASSERT_EQ(anchors.size(), 1u);
    EXPECT_EQ(anchors.front()->value,
        "Instruction " + std::to_string(instruction.value()));
    ASSERT_TRUE(anchors.front()->location.has_value());
    const auto* target = std::get_if<SctNavigationTarget>(&*anchors.front()->location);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(*target, (SctNavigationTarget{
        SctNavigationKind::Instruction, instruction.value() }));
    EXPECT_EQ(navigationTargetForOpaqueAnchor(SctOpaqueAnchor{ instruction }), *target);
}

TEST(SctPresentation, ShowsImportedContextForBetweenInstructionOpaqueGap) {
    const auto imported = SctDocumentImporter::import(importedGapParse());
    ASSERT_TRUE(imported.document.has_value());
    const auto evidence = imported.context.bind(imported.context.revisionProvenance());
    ASSERT_TRUE(evidence.has_value());
    const auto attachment = imported.document->opaqueAttachments.front().id;
    const auto snapshot = snapshotWithEvidence(std::move(*imported.document), *evidence);

    const auto presentation = SctPresentationService::describe(snapshot,
        {SctNavigationKind::OpaqueAttachment, attachment.value()});
    const auto spans = propertiesNamed(presentation, "Decoded source span");
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans.front()->value, "0x00000028 - 0x00000034");
    const auto regions = propertiesNamed(presentation, "Source region");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions.front()->value, "section payload");
    EXPECT_EQ(propertiesNamed(presentation, "Containing section").size(), 1u);
    EXPECT_EQ(propertiesNamed(presentation, "Section-relative offset").size(), 1u);

    const auto previous = propertiesNamed(presentation, "Previous semantic entity");
    const auto next = propertiesNamed(presentation, "Next semantic entity");
    ASSERT_EQ(previous.size(), 1u);
    ASSERT_EQ(next.size(), 1u);
    EXPECT_TRUE(previous.front()->location.has_value());
    EXPECT_TRUE(next.front()->location.has_value());
    EXPECT_EQ(propertiesNamed(presentation, "Preceding targeted source leaf").size(), 1u);
    EXPECT_EQ(propertiesNamed(presentation, "Following targeted source leaf").size(), 1u);
    const auto envelopes = propertiesNamed(presentation, "Containing source envelopes");
    ASSERT_EQ(envelopes.size(), 1u);
    EXPECT_FALSE(envelopes.front()->children.empty());
    const auto edges = propertiesNamed(presentation, "Crossing imported control-flow edges");
    ASSERT_EQ(edges.size(), 1u);
    ASSERT_EQ(edges.front()->children.size(), 1u);
    const auto interpretations = propertiesNamed(presentation, "Evidence-qualified interpretations");
    ASSERT_EQ(interpretations.size(), 1u);
    ASSERT_EQ(interpretations.front()->children.size(), 1u);
    EXPECT_EQ(interpretations.front()->children.front().name, "Control-flow gap");
}

TEST(SctPresentation, ShowsWithinInstructionSourceNeighborhoodWithoutInventingOwnership) {
    SctDocument document;
    const auto section = document.allocateSectionId();
    const auto instruction = document.allocateInstructionId();
    const auto attachment = document.allocateOpaqueAttachmentId();
    SctDocumentInstruction value{instruction, 125u};
    value.fixedParameters = {
        {0u, SctEncodedWordValue{1u}},
        {1u, SctEncodedWordValue{2u}},
    };
    document.sections.push_back({section, "MAIN", SctScriptSectionContent{{value}}});
    document.opaqueAttachments.push_back({attachment, {0xdeu, 0xadu},
        SctOpaqueAnchor{instruction}, SctOpaquePlacement::After, std::nullopt, 1u,
        SctOpaqueRelocationSupport::Relocatable, SctOpaqueReason::Gap});

    const SctDocumentEntityId instructionEntity{instruction};
    const SctDocumentEntityId attachmentEntity{attachment};
    const SctParameterSite first{instruction, {0u, std::nullopt}};
    const SctParameterSite second{instruction, {1u, std::nullopt}};
    auto sourceMap = SctImportedSourceMap::build(16u, {
        {{0u, 16u}, SctSourceSpanRole::Instruction, SctSourceSpanLayer::Envelope,
            SctSourceCoverageKind::SemanticEntity, instructionEntity, std::nullopt, std::nullopt,
            SctSourceRegion::SectionPayload, true},
        {{0u, 4u}, SctSourceSpanRole::InstructionParameter, SctSourceSpanLayer::Leaf,
            SctSourceCoverageKind::SemanticEntity, SctImportedSourceTarget{first}},
        {{4u, 4u}, SctSourceSpanRole::OpaqueAttachment, SctSourceSpanLayer::Leaf,
            SctSourceCoverageKind::OpaqueAttachment, attachmentEntity, std::nullopt, std::nullopt,
            SctSourceRegion::SectionPayload, true},
        {{8u, 4u}, SctSourceSpanRole::InstructionParameter, SctSourceSpanLayer::Leaf,
            SctSourceCoverageKind::SemanticEntity, SctImportedSourceTarget{second}},
        {{12u, 4u}, SctSourceSpanRole::DerivedPadding, SctSourceSpanLayer::Leaf,
            SctSourceCoverageKind::DerivedLayout},
    });
    ASSERT_TRUE(sourceMap.map.has_value());
    SctDocumentImportReceipt receipt;
    receipt.sourceMap = std::move(*sourceMap.map);
    SctDocumentImportContext context(std::move(receipt));
    const auto evidence = context.bind(context.revisionProvenance());
    ASSERT_TRUE(evidence.has_value());
    const auto snapshot = snapshotWithEvidence(std::move(document), *evidence);

    const auto presentation = SctPresentationService::describe(snapshot,
        {SctNavigationKind::OpaqueAttachment, attachment.value()});
    const auto preceding = propertiesNamed(presentation, "Preceding targeted source leaf");
    const auto following = propertiesNamed(presentation, "Following targeted source leaf");
    ASSERT_EQ(preceding.size(), 1u);
    ASSERT_EQ(following.size(), 1u);
    ASSERT_TRUE(preceding.front()->location.has_value());
    ASSERT_TRUE(following.front()->location.has_value());
    EXPECT_NE(std::get_if<SctParameterSite>(&*preceding.front()->location), nullptr);
    EXPECT_NE(std::get_if<SctParameterSite>(&*following.front()->location), nullptr);
    const auto envelopes = propertiesNamed(presentation, "Containing source envelopes");
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_EQ(envelopes.front()->children.size(), 1u);
    EXPECT_EQ(envelopes.front()->children.front().name, "instruction");
    EXPECT_EQ(propertiesNamed(presentation, "Containing section").size(), 0u);
}

TEST(SctPresentation, ShowsDocumentAnchoredOpaqueSourceContext) {
    SctDocument document;
    const auto attachment = document.allocateOpaqueAttachmentId();
    document.opaqueAttachments.push_back({attachment, {0xbeu, 0xefu},
        SctOpaqueAnchor{SctDocumentAnchor{}}, SctOpaquePlacement::After,
        std::nullopt, 1u, SctOpaqueRelocationSupport::Relocatable,
        SctOpaqueReason::Gap});
    const SctDocumentEntityId attachmentEntity{attachment};
    auto sourceMap = SctImportedSourceMap::build(4u, {
        {{0u, 4u}, SctSourceSpanRole::OpaqueAttachment, SctSourceSpanLayer::Leaf,
            SctSourceCoverageKind::OpaqueAttachment, attachmentEntity, std::nullopt,
            std::nullopt, SctSourceRegion::Footer, true},
    });
    ASSERT_TRUE(sourceMap.map.has_value());
    SctDocumentImportReceipt receipt;
    receipt.sourceMap = std::move(*sourceMap.map);
    SctDocumentImportContext context(std::move(receipt));
    const auto evidence = context.bind(context.revisionProvenance());
    ASSERT_TRUE(evidence.has_value());
    const auto snapshot = snapshotWithEvidence(std::move(document), *evidence);

    const auto presentation = SctPresentationService::describe(snapshot,
        {SctNavigationKind::OpaqueAttachment, attachment.value()});
    const auto anchors = propertiesNamed(presentation, "Anchor");
    ASSERT_EQ(anchors.size(), 1u);
    EXPECT_EQ(anchors.front()->value, "Document");
    EXPECT_EQ(propertiesNamed(presentation, "Containing section").size(), 0u);
    const auto regions = propertiesNamed(presentation, "Source region");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions.front()->value, "footer");
    EXPECT_EQ(navigationTargetForOpaqueAnchor(SctOpaqueAnchor{SctDocumentAnchor{}}),
        (SctNavigationTarget{SctNavigationKind::Document, 0u}));
}

TEST(SctPresentation, KeepsNonFiniteAndMalformedFloatEvidenceInspectable) {
    SctScptValueOperation infinity{SctScptValueKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::infinity())}};
    SctScptValueOperation negativeInfinity{SctScptValueKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(
            -std::numeric_limits<float>::infinity())}};
    SctScptValueOperation notANumber{SctScptValueKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN())}};
    SctScptValueOperation malformed{SctScptValueKind::FloatLiteral,
        0x04000000u, {}};

    const auto presentation = expressionPresentation(
        {infinity, negativeInfinity, notANumber, malformed});
    const auto floats = orderedOperationPropertiesNamed(presentation, "Float literal");
    ASSERT_EQ(floats.size(), 4u);
    EXPECT_EQ(floats[0]->value, "Infinity");
    EXPECT_EQ(floats[1]->value, "-Infinity");
    EXPECT_EQ(floats[2]->value, "NaN");
    EXPECT_EQ(floats[3]->value, "(invalid float payload)");
    EXPECT_EQ(floats[3]->notes, "Encoding 0x04000000");
}

TEST(SctDocumentLoader, ReportsMalformedAssetsWithoutInstallingADocument) {
    const auto asset = locator();
    FakeProject project(asset, std::vector<std::byte>{ std::byte{0x01}, std::byte{0x02} });
    const auto loaded = SctDocumentLoader::load(project, asset);
    EXPECT_FALSE(loaded.succeeded());
    ASSERT_FALSE(loaded.infrastructureDiagnostics.empty());
    EXPECT_EQ(loaded.infrastructureDiagnostics.back().code, DiagnosticCode::SctParseFailed);
}
