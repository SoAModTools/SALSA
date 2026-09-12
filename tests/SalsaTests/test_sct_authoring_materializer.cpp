#include "SalsaCore/Authoring/SctAuthoringMaterializer.h"
#include "SalsaCore/Authoring/SctAuthoringCodec.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstring>

namespace {
using namespace salsa::core;
namespace sct = spice::sct;
SctLiteralConstant literal(std::int16_t value) {
    return *SctLiteralConstant::fromExpression(sct::SctExpressionFactory::encodedDecimalLiteral(value));
}
std::string messages(const SctAuthoringMaterializationResult& r) {
    std::string text;
    for (const auto& d : r.diagnostics) text += d.message + "\n";
    for (const auto& s : r.scripts) {
        for (const auto& d : s.infrastructureDiagnostics) text += d.message + "\n";
        for (const auto& d : s.diagnostics) if (d.severity == DiagnosticSeverity::Error) text += d.message + "\n";
    }
    return text;
}
SctAuthoringImportRequest source(GamePlatform platform = GamePlatform::GameCube,
    sct::SctDocumentOutputWrapper wrapper = sct::SctDocumentOutputWrapper::Raw, std::string name = "synthetic.sct",
    sct::SctCanonicalExpression initial = sct::SctExpressionFactory::encodedDecimalLiteral(77)) {
    sct::SctDocument d;
    const auto mainId = d.allocateSectionId(), helperA = d.allocateSectionId(), helperB = d.allocateSectionId();
    const auto wordId = d.allocateInstructionId(), callA = d.allocateInstructionId(), callB = d.allocateInstructionId();
    const auto tailId = d.allocateInstructionId(), returnA = d.allocateInstructionId(), returnB = d.allocateInstructionId();
    sct::SctDocumentInstruction word; word.id = wordId; word.opcode = 16; word.skipRefresh = true;
    word.scheduledExpression = sct::SctExpressionFactory::encodedDecimalLiteral(2);
    word.fixedParameters = {{0, std::move(initial)}};
    sct::SctDocumentInstruction a; a.id = callA; a.opcode = 11; a.fixedParameters = {{0, sct::SctInstructionReference{returnA}}};
    auto b = a; b.id = callB; b.fixedParameters = {{0, sct::SctInstructionReference{returnB}}};
    sct::SctDocumentInstruction tail; tail.id = tailId; tail.opcode = 16; tail.fixedParameters = {{0, sct::SctExpressionFactory::encodedDecimalLiteral(8)}};
    sct::SctDocumentInstruction ret; ret.id = returnA; ret.opcode = 12;
    d.sections.push_back({mainId, "MAIN", sct::SctScriptSectionContent{{word, a, b, tail}}});
    d.sections.push_back({helperA, "HELPER_A", sct::SctScriptSectionContent{{ret}}});
    ret.id = returnB; d.sections.push_back({helperB, "HELPER_B", sct::SctScriptSectionContent{{ret}}});
    d.sections.push_back({d.allocateSectionId(), "MS0001", sct::SctStringSectionContent{
        {d.allocateStringId(), sct::SctOpaqueText{{0x82, 0xa0, 0x7f, 0x81, 0x40, 0}}, sct::SctTextKind::SctString}}});
    const auto exported = sct::SctDocumentExporter::exportDocument(d, sct::SctDocumentExportOptions{platform == GamePlatform::GameCube ? sct::SctPlatform::GameCube : sct::SctPlatform::Dreamcast,
        sct::kSctShiftJisByte7FEncoding, platform == GamePlatform::GameCube ? sct::SctDocumentOutputByteOrder::BigEndian : sct::SctDocumentOutputByteOrder::LittleEndian,
        wrapper, sct::SctOpaquePreservationPolicy::RequirePreservation,
        {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
    if (!exported.success) {
        std::string detail;
        for (const auto& diagnostic : exported.diagnostics) detail += diagnostic.message + "\n";
        throw std::runtime_error(detail);
    }
    std::vector<std::byte> bytes(exported.bytes.size()); std::memcpy(bytes.data(), exported.bytes.data(), bytes.size());
    auto hash = sha256(bytes).value();
    return {{ {AssetLocator::fromRelativePath(name).value(), bytes.size(), {hash}}, std::move(bytes)},
        {platform, GameRegion::Japan, {hash}}, {platform, false}, {}, name};
}
SctAuthoringAdoption adopted(GamePlatform platform = GamePlatform::GameCube, sct::SctDocumentOutputWrapper wrapper = sct::SctDocumentOutputWrapper::Raw) {
    const auto p = SctAuthoringProject::create().value();
    auto result = SctAuthoringImporter::import(p, p.revision, source(platform, wrapper));
    if (!result) throw std::runtime_error(result.diagnostics()[0].message);
    return std::move(result).takeValue();
}
SctAuthoringMaterializationResult prepare(const SctAuthoringAdoption& a, const SctAuthoringProject& p,
    SctAuthoringOutputMode mode = SctAuthoringOutputMode::Rebuild) {
    return SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(p), {a.program}, {a.script}, mode});
}
SctPreservedEditRequest editRequest(const SctAuthoringAdoption& a) {
    const auto& instruction = std::get<sct::SctScriptSectionContent>(a.program->document().sections[0].content).instructions[0];
    return {a.project.revision, a.content, {instruction.id, {0, {}}}, 16, literal(77), literal(78)};
}
TEST(SctAuthoringMaterializerTest, PreservesBytesAndRebuildsBothPlatformsAndWrappers) {
    for (auto platform : {GamePlatform::GameCube, GamePlatform::Dreamcast}) for (auto wrapper : {sct::SctDocumentOutputWrapper::Raw, sct::SctDocumentOutputWrapper::Aklz}) {
        SCOPED_TRACE(static_cast<int>(platform));
        SCOPED_TRACE(static_cast<int>(wrapper));
        auto a = adopted(platform, wrapper);
        auto reused = prepare(a, a.project, SctAuthoringOutputMode::ReuseUnchangedSource);
        ASSERT_TRUE(reused.succeeded()) << messages(reused);
        const auto& output = *reused.scripts[0].prepared;
        EXPECT_TRUE(output.reusedSource); EXPECT_TRUE(output.sourceLocations); EXPECT_FALSE(output.layout);
        EXPECT_EQ(output.bytes.size(), a.program->bytes().size());
        EXPECT_EQ(output.digest, a.program->baseline().source.revision.digest);
        auto rebuilt = prepare(a, a.project); ASSERT_TRUE(rebuilt.succeeded()) << messages(rebuilt);
        EXPECT_FALSE(rebuilt.scripts[0].prepared->reusedSource); EXPECT_TRUE(rebuilt.scripts[0].prepared->layout);
        EXPECT_EQ(rebuilt.scripts[0].prepared->preservation->header->values, (sct::SctHeaderValues{2001,1,2,7}));
        auto repeated = prepare(a, a.project); ASSERT_TRUE(repeated.succeeded());
        EXPECT_EQ(rebuilt.scripts[0].prepared->bytes, repeated.scripts[0].prepared->bytes);
        EXPECT_EQ(rebuilt.scripts[0].prepared->realization.key, repeated.scripts[0].prepared->realization.key);
    }
}
TEST(SctAuthoringMaterializerTest, EditsRoundTripAndRetainCrossSectionTargetsFallthroughAndModifiers) {
    auto a = adopted(); auto request = editRequest(a);
    auto edited = SctPreservedEditService::replaceLiteral(a.project, {a.program}, request); ASSERT_TRUE(edited);
    EXPECT_EQ(edited.value().affectedScripts, std::vector<SctScriptId>{a.script});
    EXPECT_EQ(a.project.find(a.content)->literalOverrides.size(), 0);
    auto json = SctAuthoringCodec::encode(edited.value().project); ASSERT_TRUE(json);
    auto decoded = SctAuthoringCodec::decode(json.value()); ASSERT_TRUE(decoded);
    auto result = prepare(a, decoded.value()); ASSERT_TRUE(result.succeeded()) << messages(result);
    const auto& output = *result.scripts[0].prepared;
    const auto& main = std::get<sct::SctScriptSectionContent>(output.document->sections[0].content).instructions;
    EXPECT_EQ(SctLiteralConstant::fromExpression(std::get<sct::SctCanonicalExpression>(main[0].fixedParameters[0].value)), literal(78));
    EXPECT_TRUE(main[0].skipRefresh); EXPECT_TRUE(main[0].scheduledExpression);
    EXPECT_EQ(main.back().opcode, 16); // No inserted terminator at the physical section boundary.
    const auto aTarget = std::get<sct::SctInstructionReference>(main[1].fixedParameters[0].value).target;
    const auto bTarget = std::get<sct::SctInstructionReference>(main[2].fixedParameters[0].value).target;
    EXPECT_NE(aTarget, bTarget);
    auto reparsed = sct::SctParser{}.parse(output.bytes); ASSERT_TRUE(reparsed.parseOk);
    auto imported = sct::SctDocumentImporter::import(reparsed, {{sct::SctPlatform::GameCube}, {}}); ASSERT_TRUE(imported.document);
    const auto& reMain = std::get<sct::SctScriptSectionContent>(imported.document->sections[0].content).instructions;
    const auto reIndex = sct::SctDocumentIndex::build(*imported.document);
    EXPECT_EQ(reIndex.instructionLocation(std::get<sct::SctInstructionReference>(reMain[1].fixedParameters[0].value).target)->sectionOrdinal, 1);
    EXPECT_EQ(reIndex.instructionLocation(std::get<sct::SctInstructionReference>(reMain[2].fixedParameters[0].value).target)->sectionOrdinal, 2);
    // Cross-section fallthrough is a physical adjacency requirement; SPICE's
    // imported edge observations do not guarantee an edge across index rows.
    const auto& locations = output.layout->instructions;
    const auto tail = std::ranges::find(locations, main.back().id, &sct::SctInstructionLayoutRecord::id);
    const auto next = std::ranges::find(locations, aTarget, &sct::SctInstructionLayoutRecord::id);
    ASSERT_NE(tail, locations.end()); ASSERT_NE(next, locations.end());
    EXPECT_EQ(tail->span.offset + tail->span.size, next->span.offset);
    request.expectedRevision = edited.value().project.revision; request.expectedValue = literal(78); request.value = literal(77);
    auto restored = SctPreservedEditService::replaceLiteral(edited.value().project, {a.program}, request); ASSERT_TRUE(restored);
    EXPECT_TRUE(restored.value().project.find(a.content)->literalOverrides.empty());
    auto reused = prepare(a, restored.value().project, SctAuthoringOutputMode::ReuseUnchangedSource); ASSERT_TRUE(reused.succeeded());
    EXPECT_TRUE(reused.scripts[0].prepared->reusedSource);
}
TEST(SctAuthoringMaterializerTest, RejectsStaleEditsAndUnsupportedSitesAtomically) {
    auto a = adopted(); const auto before = SctAuthoringCodec::encode(a.project).value();
    auto request = editRequest(a); request.expectedRevision.value++; EXPECT_FALSE(SctPreservedEditService::replaceLiteral(a.project, {a.program}, request));
    request = editRequest(a); request.expectedValue = literal(76); EXPECT_FALSE(SctPreservedEditService::replaceLiteral(a.project, {a.program}, request));
    request = editRequest(a); request.expectedOpcode++; EXPECT_FALSE(SctPreservedEditService::replaceLiteral(a.project, {a.program}, request));
    request = editRequest(a); request.site.parameter.repeatedGroupOrdinal = 0; EXPECT_FALSE(SctPreservedEditService::replaceLiteral(a.project, {a.program}, request));
    const auto& code = std::get<sct::SctScriptSectionContent>(a.program->document().sections[0].content).instructions;
    request = editRequest(a); request.site.instruction = code[1].id; request.expectedOpcode = 11; EXPECT_FALSE(SctPreservedEditService::replaceLiteral(a.project, {a.program}, request));
    request = editRequest(a); request.value = request.expectedValue;
    auto noop = SctPreservedEditService::replaceLiteral(a.project, {a.program}, request); ASSERT_TRUE(noop);
    EXPECT_TRUE(noop.value().affectedScripts.empty()); EXPECT_EQ(noop.value().project.revision, a.project.revision);
    EXPECT_EQ(SctAuthoringCodec::encode(a.project).value(), before);
}
TEST(SctAuthoringMaterializerTest, RejectsMissingBackingSourceDriftPartitionsAndUnsupportedIntent) {
    auto a = adopted();
    auto absent = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(a.project), {}, {a.script}});
    EXPECT_FALSE(absent.succeeded()); EXPECT_FALSE(absent.scripts[0].prepared);
    auto changed = a.project; changed.baselines[0].source.byteSize++; EXPECT_FALSE(prepare(a, changed).succeeded());
    changed = a.project; changed.baselines[0].importedDocument.value = generateSctAuthoringUuid().value();
    changed.contents[0].region.importedDocument = changed.baselines[0].importedDocument; EXPECT_FALSE(prepare(a, changed).succeeded());
    changed = a.project; changed.baselines[0].recipe.platform = GamePlatform::Dreamcast; EXPECT_FALSE(prepare(a, changed).succeeded());
    changed = a.project; changed.contents[0].region.coverage = SctOrderedSourceSelection{{sct::SctInstructionId{999}}}; EXPECT_FALSE(prepare(a, changed).succeeded());
    changed = a.project; changed.modules.push_back({changed.allocate<SctModuleId>().value(), a.script, "unsupported", {}, {}}); EXPECT_FALSE(prepare(a, changed).succeeded());
    auto request = source(); request.source.bytes[0] ^= std::byte{1};
    EXPECT_FALSE(SctAuthoringImporter::import(a.project, a.project.revision, std::move(request)));
}
TEST(SctAuthoringMaterializerTest, CancellationAndUnknownPlatformCannotProduceOutput) {
    auto a = adopted(); std::stop_source stop; stop.request_stop();
    EXPECT_FALSE(SctAuthoringImporter::import(a.project, a.project.revision, source(), stop.get_token()));
    const auto result = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(a.project), {a.program}, {a.script}}, stop.get_token());
    EXPECT_TRUE(result.cancelled); EXPECT_FALSE(result.succeeded()); EXPECT_TRUE(result.scripts.empty());
    auto unknown = source(); unknown.recipe.platform.reset(); unknown.dataset.platform.reset();
    auto imported = SctAuthoringImporter::import(a.project, a.project.revision, std::move(unknown)); ASSERT_TRUE(imported);
    auto output = prepare(imported.value(), imported.value().project); EXPECT_FALSE(output.succeeded()); EXPECT_FALSE(output.scripts[0].prepared);
}
TEST(SctAuthoringMaterializerTest, RebuildOmitsOpaquePrefixAndGeneratesNewIndexedStart) {
    // One synthetic index row points past a 32-byte opaque prefix. The prefix
    // happens to encode opcode 79; it must never become part of the section.
    std::vector<std::byte> bytes(68, std::byte{0});
    bytes[11] = std::byte{1}; // index count
    bytes[15] = std::byte{32}; // section offset relative to the data start (32)
    std::memcpy(bytes.data() + 16, "MAIN", 4);
    bytes[35] = std::byte{79};
    for (std::size_t i = 0; i < 7; ++i) bytes[37 + 4 * i] = std::byte{0x80}; // seven inline SCPT zero values
    bytes[67] = std::byte{12};
    const auto hash = sha256(bytes).value();
    auto project = SctAuthoringProject::create().value();
    auto a = SctAuthoringImporter::import(project, project.revision,
        {{{AssetLocator::fromRelativePath("prefix.sct").value(), bytes.size(), {hash}}, std::move(bytes)},
            {GamePlatform::GameCube, GameRegion::Japan, {hash}}, {GamePlatform::GameCube, false}, {}, "prefix"});
    ASSERT_TRUE(a);
    ASSERT_EQ(std::get<sct::SctScriptSectionContent>(a.value().program->document().sections[0].content).instructions.size(), 1);
    auto reused = prepare(a.value(), a.value().project, SctAuthoringOutputMode::ReuseUnchangedSource);
    ASSERT_TRUE(reused.succeeded()) << messages(reused);
    auto rebuilt = prepare(a.value(), a.value().project);
    ASSERT_TRUE(rebuilt.succeeded()) << messages(rebuilt);
    const auto& output = *rebuilt.scripts[0].prepared;
    EXPECT_NE(output.digest, hash);
    ASSERT_TRUE(output.layout);
    ASSERT_EQ(output.layout->sections.size(), 1);
    EXPECT_EQ(output.layout->sections[0].dataRelativeOffset, 0);
    EXPECT_TRUE(output.document->opaqueAttachments.empty());
    EXPECT_FALSE(a.value().program->document().opaqueAttachments.empty());
    auto reimported = sct::SctDocumentImporter::import(sct::SctParser{}.parse(output.bytes), {{sct::SctPlatform::GameCube}, {}});
    ASSERT_TRUE(reimported.document);
    const auto& instructions = std::get<sct::SctScriptSectionContent>(reimported.document->sections[0].content).instructions;
    ASSERT_EQ(instructions.size(), 1);
    EXPECT_EQ(instructions[0].opcode, 12);
}
TEST(SctAuthoringMaterializerTest, SemanticLoweringOmitsArtifactsButRejectsReferencedOpaqueText) {
    sct::SctDocument source;
    const auto code = source.allocateSectionId(), unused = source.allocateSectionId();
    const auto opaqueString = source.allocateStringId();
    const auto opaqueFooter = source.allocateSupplementaryTextId();
    const auto ret = source.allocateInstructionId();
    source.sections.push_back({code, "loop", sct::SctScriptSectionContent{{{ret, 12}}}});
    source.sections.push_back({unused, "unknown", sct::SctOpaqueSectionContent{}});
    source.sections.push_back({source.allocateSectionId(), "text", sct::SctStringSectionContent{
        {opaqueString, sct::SctOpaqueText{{0xff, 0}}, sct::SctTextKind::SctString}}});
    source.supplementaryText.push_back({opaqueFooter, sct::SctTextKind::PlainString, sct::SctOpaqueText{{0x82, 0xa0, 0}}});
    source.opaqueAttachments.push_back({source.allocateOpaqueAttachmentId(), {0xff}, unused,
        sct::SctOpaquePlacement::FixedOffset, 4096});
    auto lowered = SctAuthoringMaterializer::buildSemanticDocument(source); ASSERT_TRUE(lowered);
    EXPECT_EQ(lowered.value().sections.size(), 1u); EXPECT_TRUE(lowered.value().opaqueAttachments.empty());
    EXPECT_TRUE(lowered.value().supplementaryText.empty()); EXPECT_FALSE(lowered.diagnostics().empty());
    EXPECT_EQ(source.sections.size(), 3u); EXPECT_EQ(source.opaqueAttachments.size(), 1u);
    auto& instructions = std::get<sct::SctScriptSectionContent>(source.sections[0].content).instructions;
    instructions.push_back({source.allocateInstructionId(), 24, false, {}, {{0, sct::SctSupplementaryTextReference{opaqueFooter}}}});
    auto rejected = SctAuthoringMaterializer::buildSemanticDocument(source); EXPECT_FALSE(rejected);
    ASSERT_FALSE(rejected.diagnostics().empty());
    EXPECT_TRUE(std::ranges::any_of(rejected.diagnostics(), [](const auto& d) { return d.message.find("referenced supplementary text") != std::string::npos; }));
    // Explicit text repair resolves the logical message without importing its old layout.
    source.supplementaryText[0].value = sct::SctPlainText{"Repaired debug message"};
    auto repaired = SctAuthoringMaterializer::buildSemanticDocument(source); ASSERT_TRUE(repaired);
    ASSERT_EQ(repaired.value().supplementaryText.size(), 1u);
    EXPECT_EQ(repaired.value().supplementaryText[0].id, opaqueFooter);
    EXPECT_EQ(std::get<sct::SctSupplementaryTextReference>(std::get<sct::SctScriptSectionContent>(repaired.value().sections[0].content).instructions[1].fixedParameters[0].value).target, opaqueFooter);
    instructions[1].fixedParameters[0].value = sct::SctStringReference{opaqueString};
    EXPECT_FALSE(SctAuthoringMaterializer::buildSemanticDocument(source));
}
TEST(SctAuthoringMaterializerTest, SemanticLoweringDoesNotEmitOpaqueExecutableOperands) {
    auto a = adopted(); auto source = a.program->document();
    auto& instruction = std::get<sct::SctScriptSectionContent>(source.sections[0].content).instructions[0];
    instruction.fixedParameters[0].value = sct::SctOpaqueParameterValue{{1}};
    EXPECT_FALSE(SctAuthoringMaterializer::buildSemanticDocument(source));
}
TEST(SctAuthoringMaterializerTest, SourceReuseCannotBypassUnresolvedInstructionReferences) {
    std::vector<std::byte> bytes(44, std::byte{0});
    bytes[11] = std::byte{1};
    std::memcpy(bytes.data() + 16, "MAIN", 4);
    bytes[35] = std::byte{11}; // call with an out-of-range relative target
    bytes[36] = std::byte{0x7f}; bytes[37] = std::byte{0xff}; bytes[38] = std::byte{0xff}; bytes[39] = std::byte{0xf0};
    bytes[43] = std::byte{12};
    const auto hash = sha256(bytes).value();
    auto project = SctAuthoringProject::create().value();
    auto imported = SctAuthoringImporter::import(project, project.revision,
        {{{AssetLocator::fromRelativePath("unresolved.sct").value(), bytes.size(), {hash}}, std::move(bytes)},
            {GamePlatform::GameCube, GameRegion::Japan, {hash}}, {GamePlatform::GameCube, false}, {}, "unresolved"});
    ASSERT_TRUE(imported);
    auto output = prepare(imported.value(), imported.value().project, SctAuthoringOutputMode::ReuseUnchangedSource);
    EXPECT_FALSE(output.succeeded()); EXPECT_FALSE(output.scripts[0].prepared);
    EXPECT_FALSE(output.scripts[0].diagnostics.empty());
}
TEST(SctAuthoringMaterializerTest, EditingOneScriptLeavesOtherOutputUntouched) {
    auto first = adopted(); auto second = SctAuthoringImporter::import(first.project, first.project.revision,
        source(GamePlatform::GameCube, sct::SctDocumentOutputWrapper::Raw, "second.sct")); ASSERT_TRUE(second);
    auto request = editRequest(first); request.expectedRevision = second.value().project.revision;
    const SctImportedPrograms programs{first.program, second.value().program};
    auto edited = SctPreservedEditService::replaceLiteral(second.value().project, programs, request); ASSERT_TRUE(edited);
    const auto result = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(edited.value().project), programs, {first.script, second.value().script}});
    ASSERT_TRUE(result.succeeded()) << messages(result);
    EXPECT_FALSE(result.scripts[0].prepared->reusedSource); EXPECT_FALSE(result.scripts[1].prepared->reusedSource);
    const auto before = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(second.value().project), programs, {second.value().script}});
    ASSERT_TRUE(before.succeeded());
    EXPECT_EQ(result.scripts[1].prepared->digest, before.scripts[0].prepared->digest);
}
TEST(SctAuthoringMaterializerTest, SchemaRejectsMalformedAndDuplicateOverrides) {
    auto a = adopted(); auto edited = SctPreservedEditService::replaceLiteral(a.project, {a.program}, editRequest(a)); ASSERT_TRUE(edited);
    auto json = nlohmann::ordered_json::parse(SctAuthoringCodec::encode(edited.value().project).value());
    auto stale = edited.value().project; stale.contents[0].literalOverrides[0].baselineValue = literal(76);
    EXPECT_FALSE(prepare(a, stale).succeeded());
    auto duplicate = json; duplicate["contents"][0]["literalOverrides"].push_back(duplicate["contents"][0]["literalOverrides"][0]);
    EXPECT_FALSE(SctAuthoringCodec::decode(duplicate.dump()));
    auto invalid = json; invalid["contents"][0]["literalOverrides"][0]["value"]["kind"] = "variable";
    EXPECT_FALSE(SctAuthoringCodec::decode(invalid.dump()));
    invalid = json; invalid["contents"][0]["literalOverrides"][0]["value"]["encodingWord"] = 0x50000000u;
    EXPECT_FALSE(SctAuthoringCodec::decode(invalid.dump()));
    invalid = json; invalid["contents"][0]["literalOverrides"][0]["value"]["termination"] = "inline";
    EXPECT_FALSE(SctAuthoringCodec::decode(invalid.dump()));
    json["contents"][0]["literalOverrides"][0]["value"]["encodingWord"] = 4294967296ULL; EXPECT_FALSE(SctAuthoringCodec::decode(json.dump()));
}
TEST(SctAuthoringMaterializerTest, EditsFloatAndInlineConstantsWithoutChangingWidth) {
    const auto inlineA = sct::SctExpressionFactory::oneWordValue(sct::SctExpressionOneWordValue::Value00800000);
    const auto inlineB = sct::SctExpressionFactory::oneWordValue(sct::SctExpressionOneWordValue::Value7F7FFFFF);
    for (const auto& pair : {std::pair{sct::SctExpressionFactory::floatLiteral(1.25f), sct::SctExpressionFactory::floatLiteral(-2.5f)},
        std::pair{inlineA, inlineB}}) {
        auto project = SctAuthoringProject::create().value();
        auto imported = SctAuthoringImporter::import(project, project.revision,
            source(GamePlatform::Dreamcast, sct::SctDocumentOutputWrapper::Raw, "literal.sct", pair.first));
        ASSERT_TRUE(imported);
        auto& a = imported.value(); auto request = editRequest(a);
        request.expectedValue = *SctLiteralConstant::fromExpression(pair.first);
        request.value = *SctLiteralConstant::fromExpression(pair.second);
        auto edited = SctPreservedEditService::replaceLiteral(a.project, {a.program}, request); ASSERT_TRUE(edited);
        auto reloaded = SctAuthoringCodec::decode(SctAuthoringCodec::encode(edited.value().project).value()); ASSERT_TRUE(reloaded);
        auto output = prepare(a, reloaded.value()); ASSERT_TRUE(output.succeeded()) << messages(output);
        auto inspected = SctPreservedEditService::inspectLiteral(*output.scripts[0].prepared->document, request.site, 16, request.value);
        ASSERT_TRUE(inspected); EXPECT_EQ(inspected.value(), request.value);
        EXPECT_EQ(output.scripts[0].prepared->bytes.size(), prepare(a, a.project).scripts[0].prepared->bytes.size());
    }
}
TEST(SctAuthoringMaterializerTest, ExcludesVariablesCompoundProgramsAndEncodingFamilyChanges) {
    auto a = adopted(); const auto request = editRequest(a);
    auto document = a.program->document();
    auto& parameter = std::get<sct::SctScriptSectionContent>(document.sections[0].content).instructions[0].fixedParameters[0].value;
    parameter = *sct::SctExpressionFactory::byteVariable(87).expression;
    EXPECT_FALSE(SctPreservedEditService::inspectLiteral(document, request.site, 16, request.value));
    parameter = *sct::SctExpressionFactory::binaryOperator(sct::SctExpressionBinaryOperator::Add, literal(1).expression(), literal(2).expression()).expression;
    EXPECT_FALSE(SctPreservedEditService::inspectLiteral(document, request.site, 16, request.value));
    const auto floatValue = *SctLiteralConstant::fromExpression(sct::SctExpressionFactory::floatLiteral(78));
    EXPECT_FALSE(SctPreservedEditService::inspectLiteral(a.program->document(), request.site, 16, floatValue));
    auto invalid = request.value; invalid.operation.encodingWord = 0x50000000u;
    EXPECT_FALSE(SctPreservedEditService::inspectLiteral(a.program->document(), request.site, 16, invalid));
}
} // namespace
