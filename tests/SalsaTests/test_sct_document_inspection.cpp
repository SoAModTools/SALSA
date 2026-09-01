#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctPresentation.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

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

void collectProperties(
    const SctPropertyItem& property,
    const std::string_view name,
    std::vector<const SctPropertyItem*>& matches) {
    if (property.name == name) matches.push_back(&property);
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

SctEntityPresentation expressionPresentation(
    const std::vector<SctCanonicalExpressionNode>& nodes) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(false));
    const auto loaded = SctDocumentLoader::load(project, asset);
    EXPECT_TRUE(loaded.succeeded());
    auto document = std::make_shared<SctDocument>(*loaded.document->document);
    auto& script = std::get<SctScriptSectionContent>(document->sections.front().content);
    SctDocumentInstruction instruction{document->allocateInstructionId(), 125};
    for (std::uint32_t index = 0; index < nodes.size(); ++index) {
        instruction.fixedParameters.push_back({index,
            SctCanonicalExpression{nodes[index], SctExpressionTermination::StopCode}});
    }
    const auto instructionId = instruction.id;
    script.instructions.insert(script.instructions.begin(), std::move(instruction));
    auto snapshot = *loaded.document;
    snapshot.document = std::move(document);
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
    EXPECT_FALSE(loaded.document->textConvention.has_value());
    EXPECT_EQ(loaded.document->textSelectionOrigin, SctTextSelectionOrigin::None);
    ASSERT_EQ(loaded.document->document->sections.size(), 1u);
}

TEST(SctDocumentLoader, AmbiguousTextStaysOpaqueUntilExplicitReimport) {
    const auto asset = locator();
    FakeProject project(asset, inspectableSctBytes(true));
    const auto loaded = SctDocumentLoader::load(project, asset);
    ASSERT_TRUE(loaded.succeeded());
    ASSERT_FALSE(loaded.inspection->textAssessment.records.empty());
    EXPECT_FALSE(loaded.document->textConvention.has_value());
    const auto* parsedAddress = loaded.inspection->parsed.get();
    const auto reimported = SctDocumentLoader::materialize(loaded.inspection,
        SctKnownTextConvention::ShiftJisByte7F, SctTextSelectionOrigin::UserSelected);
    ASSERT_TRUE(reimported.succeeded());
    EXPECT_EQ(reimported.inspection->parsed.get(), parsedAddress);
    EXPECT_EQ(reimported.document->textConvention, SctKnownTextConvention::ShiftJisByte7F);
    EXPECT_EQ(reimported.document->textSelectionOrigin, SctTextSelectionOrigin::UserSelected);
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
    SctCanonicalExpressionNode positiveFloat{SctCanonicalExpressionNodeKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(1.5f)}, {}};
    SctCanonicalExpressionNode negativeFloat{SctCanonicalExpressionNodeKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(-12.25f)}, {}};
    SctCanonicalExpressionNode decimal{SctCanonicalExpressionNodeKind::DecimalLiteral,
        0x08000380u, {}, {}};
    SctCanonicalExpressionNode variable{SctCanonicalExpressionNodeKind::BitVariable,
        0x2000002au, {}, {}};
    SctCanonicalExpressionNode add{SctCanonicalExpressionNodeKind::ArithmeticOperator,
        0x0eu, {}, {decimal, variable}};

    const auto presentation = expressionPresentation(
        {positiveFloat, negativeFloat, decimal, variable, add});
    const auto floats = propertiesNamed(presentation, "Float literal");
    ASSERT_EQ(floats.size(), 2u);
    EXPECT_EQ(floats[0]->value, "1.5");
    EXPECT_EQ(floats[1]->value, "-12.25");
    EXPECT_NE(floats[0]->notes.find("Encoding 0x04000000"), std::string::npos);
    EXPECT_NE(floats[0]->notes.find("payload 0x3FC00000"), std::string::npos);

    const auto decimals = propertiesNamed(presentation, "Decimal literal");
    ASSERT_GE(decimals.size(), 2u);
    EXPECT_EQ(decimals.front()->value, "3.5");
    EXPECT_NE(decimals.front()->notes.find("0x08000380"), std::string::npos);
    const auto variables = propertiesNamed(presentation, "Bit variable");
    ASSERT_GE(variables.size(), 2u);
    EXPECT_EQ(variables.front()->value, "42");
    EXPECT_NE(variables.front()->notes.find("0x2000002A"), std::string::npos);
    const auto operators = propertiesNamed(presentation, "Arithmetic");
    ASSERT_EQ(operators.size(), 1u);
    EXPECT_EQ(operators.front()->value, "+");
    EXPECT_NE(operators.front()->notes.find("0x0000000E"), std::string::npos);
}

TEST(SctPresentation, KeepsNonFiniteAndMalformedFloatEvidenceInspectable) {
    SctCanonicalExpressionNode infinity{SctCanonicalExpressionNodeKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::infinity())}, {}};
    SctCanonicalExpressionNode negativeInfinity{SctCanonicalExpressionNodeKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(
            -std::numeric_limits<float>::infinity())}, {}};
    SctCanonicalExpressionNode notANumber{SctCanonicalExpressionNodeKind::FloatLiteral,
        0x04000000u, {std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN())}, {}};
    SctCanonicalExpressionNode malformed{SctCanonicalExpressionNodeKind::FloatLiteral,
        0x04000000u, {}, {}};

    const auto presentation = expressionPresentation(
        {infinity, negativeInfinity, notANumber, malformed});
    const auto floats = propertiesNamed(presentation, "Float literal");
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
