#include "SalsaCore/Program/NativeImport.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <gtest/gtest.h>
#include <set>
#include <array>

namespace {
using namespace salsa::core;
namespace ir = salsa::core::program;
namespace sct = spice::sct;
constexpr auto Project = "e44499b5-dcf0-449a-9815-5bdf558eface";
SctAuthoringBaseline baseline(bool second = false) {
    return {{}, {Sha256Digest{{}}}, {AssetLocator::fromRelativePath(second ? "field/synthetic-b.sct" : "field/synthetic-a.sct").value(), 0, {Sha256Digest{{}}}},
        {second ? "b44499b5-dcf0-449a-9815-5bdf558eface" : "a44499b5-dcf0-449a-9815-5bdf558eface"}, sct::SctKnownTextConvention::ShiftJisByte7F,
        {{SctEvidenceKind::SourceObservation, "Synthetic baseline"}}};
}
std::string messages(const std::vector<Diagnostic>& ds) { std::string s; for (const auto& d : ds) s += d.message + "\n"; return s; }
sct::SctCanonicalExpression bit(std::uint32_t index) { return *sct::SctExpressionFactory::bitVariable(index).expression; }
sct::SctDocument fixture() {
    sct::SctDocument d;
    auto target = d.allocateInstructionId(); auto stringId = d.allocateStringId(); auto footer = d.allocateSupplementaryTextId();
    sct::SctDocumentInstruction broad{d.allocateInstructionId(), 65000, true, bit(121)};
    broad.fixedParameters = {{0, sct::SctEncodedWordValue{0xabcdef12}}, {1, sct::SctTerminatedWordSequenceValue{{1,2,3}}},
        {2, sct::SctStringReference{stringId}}, {3, sct::SctSupplementaryTextReference{footer}},
        {4, sct::SctUnresolvedReferenceValue{{sct::SctReferenceTargetStorage::Instruction, {}}, {0xfffffffc}}}};
    auto expression = bit(0x10000011);
    auto& operations = std::get<sct::SctTypedScptProgram>(expression.body).operations;
    operations.push_back(sct::SctScptInertOperation{});
    operations.push_back(sct::SctScptStackOverwritePreviousWithTopOperation{});
    broad.fixedParameters.push_back({5, expression});
    broad.repeatedParameterGroups = {
        {{{2, sct::SctInstructionReference{target}}, {3, sct::SctSupplementaryTextReference{footer}}}},
        {{{2, sct::SctInstructionReference{target}}, {3, sct::SctSupplementaryTextReference{footer}}}}};
    std::vector<sct::SctDocumentInstruction> code;
    code.push_back(broad);
    code.push_back({target, 17, false, {}, {{0, sct::SctExpressionFactory::floatLiteral(121.0f)}}});
    code.push_back({d.allocateInstructionId(), 18, false, {}, {{0, sct::SctExpressionFactory::encodedDecimalLiteral(121)}}});
    code.push_back({d.allocateInstructionId(), 17, false, {}, {{0, bit(122)}}});
    code.push_back({d.allocateInstructionId(), 5, false, {}, {{0, sct::SctEncodedWordValue{87}}, {1, sct::SctExpressionFactory::encodedDecimalLiteral(2)}}});
    for (int i = 0; i < 520; ++i) code.push_back({d.allocateInstructionId(), 12});
    d.sections.push_back({d.allocateSectionId(), "scene", sct::SctScriptSectionContent{std::move(code)}});
    for (auto name : {"init", "loop"}) d.sections.push_back({d.allocateSectionId(), name, sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
    sct::SctMessage message; message.headerUtf8 = "Header";
    message.body.elements = {sct::SctTextChunk{"Body"}, sct::SctInlineCommand{sct::SctMessageCommandCode::C, sct::SctByteListCommandArgument{{2, 7}}}};
    d.sections.push_back({d.allocateSectionId(), "strings_en", sct::SctStringGroupMarkerSectionContent{}});
    d.sections.push_back({d.allocateSectionId(), "line_en", sct::SctStringSectionContent{{stringId, message, sct::SctTextKind::SctString}}});
    d.supplementaryText.push_back({footer, sct::SctTextKind::PlainString, sct::SctPlainText{"Debug"}});
    return d;
}
TEST(WholeProgramTest, ImportsIndependentTypedProgramsAndDirectConnections) {
    ir::ProgramBuilder builder({Project});
    auto d = fixture(); SctScriptContext context; context.name = "Shared name";
    context.legacyOrigin = SctLegacyOrigin{"capsule", "script", 4};
    auto a = builder.appendImported(d, baseline(), context); ASSERT_TRUE(a) << messages(a.diagnostics());
    auto b = builder.appendImported(d, baseline(true), context); ASSERT_TRUE(b) << messages(b.diagnostics());
    d = {}; // The published program must not depend on a resident native graph.
    for (auto id : {a.value(), b.value()}) builder.metadata().modules.push_back({{}, id, "Shared scene", {builder.sectionBody(builder.scriptSections(id)[0])}});
    auto built = builder.finish(); ASSERT_TRUE(built) << messages(built.diagnostics()); const auto& p = *built.value();
    ASSERT_EQ(p.scripts().size(), 2); EXPECT_NE(a.value(), b.value()); EXPECT_EQ(p.statistics().actions, 1054);
    EXPECT_GT(p.statistics().actionPages, 4); EXPECT_GT(p.statistics().valuePages, 4);
    const auto& ma = p.metadata().modules[0]; const auto& mb = p.metadata().modules[1]; EXPECT_NE(ma.id, mb.id); EXPECT_EQ(ma.name, mb.name);
    const auto sequence = builder.sectionBody({}); // Finished builder exposes no stale build data.
    EXPECT_FALSE(sequence.valid());
    const auto* section = p.find(p.scripts()[0].sections[0]); ASSERT_NE(section, nullptr);
    const auto& order = *p.find(section->body)->order;
    const auto* broad = p.find(order[0]); ASSERT_NE(broad, nullptr);
    EXPECT_EQ(p.find(broad->value)->data, 65000); EXPECT_EQ(p.find(broad->value)->detail, 3);
    EXPECT_EQ(p.nextInSequence(order[0]), order[1]); EXPECT_EQ(p.incoming(order[1]).size(), 2);
    EXPECT_EQ(p.incoming(order[1])[0].site.parameter->repeatedGroupOrdinal, 0);
    EXPECT_EQ(p.incoming(order[1])[1].site.parameter->repeatedGroupOrdinal, 1);
    EXPECT_EQ(p.uses({ir::Bank::Bit, 121}).size(), 6); // Schedule read, set, clear in each script.
    EXPECT_EQ(p.uses({ir::Bank::Bit, 0x10000011}).size(), 2);
    EXPECT_EQ(p.computedWrites().size(), 2); EXPECT_EQ(p.uses({ir::Bank::Byte, 87}).size(), 2);
    EXPECT_TRUE(std::ranges::any_of(built.diagnostics(), [](const Diagnostic& d) { return d.code == DiagnosticCode::UnresolvedSctAuthoringBinding; }));
    EXPECT_EQ(p.find(a.value())->context.legacyOrigin->scriptKey, "script");
    auto payload = p.children(broad->value); EXPECT_EQ(p.find(p.children(payload[1])[0])->data, 0xabcdef12);
    const auto text = std::ranges::find_if(p.texts(), [&](const auto &t) { return t.script == a.value() && !t.supplementary; });
    ASSERT_NE(text,p.texts().end()); auto elements = p.children(text->value); ASSERT_EQ(elements.size(), 3);
    EXPECT_EQ(p.text(elements[0]), "Header"); EXPECT_EQ(p.text(elements[1]), "Body");
    EXPECT_EQ(p.find(elements[2])->detail, 2); EXPECT_EQ(p.children(elements[2]).size(), 2);
    std::set<SctTextResourceId> strings;
    for (const auto &t : p.texts()) if(t.script == a.value() && t.supplementary) {
        EXPECT_EQ(p.text(t.value), "Debug"); strings.insert(t.id);
    }
    EXPECT_EQ(strings.size(),3u); // Fixed and repeated operands split the same source string.

}
TEST(WholeProgramTest, StructuredChainOwnsFlatArmsAndBodiesWithoutGeneratedJumps) {
    ir::ProgramBuilder builder({Project}, 7, 1ULL << 40);
    auto script = builder.importedScript(baseline(), {}); ASSERT_TRUE(script);
    auto section = builder.section({{}, script.value(), "scene"}); ASSERT_TRUE(section);
    auto body = builder.sectionBody(section.value());
    auto c1 = builder.expression(bit(121)), c2 = builder.expression(bit(122)); ASSERT_TRUE(c1); ASSERT_TRUE(c2);
    const std::array conditions{c1.value(), c2.value(), ir::ValueId{}};
    auto chain = builder.conditional(body, conditions); ASSERT_TRUE(chain);
    auto bodies = builder.branchBodies(chain.value()); ASSERT_EQ(bodies.size(), 3);
    auto ret = builder.value(ir::ValueKind::Instruction, 12); ASSERT_TRUE(ret);
    for (auto child : bodies) ASSERT_TRUE(builder.instruction(child, ret.value()));
    auto nested = builder.conditional(bodies[1], std::span(conditions).first(1)); ASSERT_TRUE(nested);
    auto result = builder.finish(); ASSERT_TRUE(result) << messages(result.diagnostics()); const auto& p = *result.value();
    EXPECT_EQ(p.revision(), 7); EXPECT_GT(chain.value().value, 1ULL << 40);
    EXPECT_EQ(p.statistics().actions, 5); // Two actual constructs + three instructions.
    EXPECT_EQ(p.find(chain.value())->kind, ir::ActionKind::Conditional);
    EXPECT_FALSE(p.find(chain.value())->value.valid());
    auto arms = p.arms(chain.value()); ASSERT_EQ(arms.size(), 3); EXPECT_FALSE(arms[2].condition.valid());
    for (const auto& arm : arms) EXPECT_EQ(std::get<ir::ArmId>(*p.find(arm.body)->owner), arm.id);
    EXPECT_EQ(p.uses({ir::Bank::Bit, 121}).size(), 2); EXPECT_EQ(p.uses({ir::Bank::Bit, 122}).size(), 1);
    EXPECT_EQ(p.uses({ir::Bank::Bit, 122})[0].site.arm, arms[1].id);
    EXPECT_TRUE(p.incoming(chain.value()).empty());
}
TEST(WholeProgramTest, RejectsBrokenInputWithoutPublishingAndKeepsIncompleteDrafts) {
    ir::ProgramBuilder bad({Project}); auto base = baseline(); base.importedDocument.value.clear();
    EXPECT_FALSE(bad.appendImported(fixture(), base, {})); EXPECT_FALSE(bad.finish());
    ir::ProgramBuilder duplicate({Project}); auto doc = fixture();
    auto& code = std::get<sct::SctScriptSectionContent>(doc.sections[0].content).instructions; code[1].id = code[0].id;
    EXPECT_FALSE(duplicate.appendImported(doc, baseline(), {})); EXPECT_FALSE(duplicate.finish());
    ir::ProgramBuilder dangling({Project});
    auto script = dangling.importedScript(baseline(), {}).value(); auto section = dangling.section({{}, script, "scene"}).value();
    auto ref = dangling.value(ir::ValueKind::ActionReference, 900000).value();
    auto param = dangling.value(ir::ValueKind::Parameter, 0, 0, std::span(&ref, 1)).value();
    auto instruction = dangling.value(ir::ValueKind::Instruction, 11, 0, std::span(&param, 1)).value();
    ASSERT_TRUE(dangling.instruction(dangling.sectionBody(section), instruction)); EXPECT_FALSE(dangling.finish());
    ir::ProgramBuilder lifecycle({Project}); ASSERT_TRUE(lifecycle.appendImported(fixture(), baseline(), {}));
    lifecycle.metadata().entrypoints[0].enabled = false; EXPECT_FALSE(lifecycle.finish());
    ir::ProgramBuilder draft({Project}); ASSERT_TRUE(draft.importedScript(baseline(), {}));
    auto complete = draft.finish(); ASSERT_TRUE(complete); EXPECT_EQ(complete.diagnostics().size(), 2);
}
TEST(WholeProgramTest, NativeBoundaryParsesBothByteOrdersAndReleasesSource) {
    for (auto platform : {sct::SctPlatform::GameCube, sct::SctPlatform::Dreamcast}) {
        sct::SctDocument d;
        for (auto name : {"init", "loop"}) d.sections.push_back({d.allocateSectionId(), name, sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
        sct::SctMessage text; text.body.elements = {sct::SctTextChunk{"Hello"}};
        d.sections.push_back({d.allocateSectionId(), "line_en", sct::SctStringSectionContent{{d.allocateStringId(), text, sct::SctTextKind::SctString}}});
        sct::SctDocumentExportOptions options{platform, sct::kSctShiftJisByte7FEncoding};
        options.byteOrder = platform == sct::SctPlatform::GameCube ? sct::SctDocumentOutputByteOrder::BigEndian : sct::SctDocumentOutputByteOrder::LittleEndian;
        options.header = {sct::SctHeaderExportMode::ExplicitValues, {2001,1,2,7}};
        auto bytes = sct::SctDocumentExporter::exportDocument(d, options); ASSERT_TRUE(bytes.success);
        auto base = baseline(); base.source.byteSize = bytes.bytes.size(); base.source.revision.digest = sha256(std::as_bytes(std::span(bytes.bytes))).value();
        base.textConvention.reset(); // No platform or encoding supplied.
        sct::SctDocumentImportOptions importOptions;
        ir::ProgramBuilder builder({Project}); auto result = ir::appendNative(builder, bytes.bytes, base, {}, importOptions);
        ASSERT_TRUE(result) << messages(result.diagnostics()); bytes.bytes.clear(); d = {};
        auto program = builder.finish(); ASSERT_TRUE(program) << messages(program.diagnostics());
        EXPECT_FALSE(program.value()->scripts()[0].context.platform);
        EXPECT_FALSE(program.value()->scripts()[0].context.region);
        EXPECT_EQ(program.value()->texts().size(), 1);
        EXPECT_EQ(program.value()->metadata().entrypoints.size(), 2); EXPECT_EQ(program.value()->statistics().actions, 2);
        EXPECT_EQ(program.value()->scripts()[0].context.outputProfile->bigEndian, platform == sct::SctPlatform::GameCube);
    }
}
TEST(WholeProgramTest, OwnsPreparedConditionsLoopsSwitchesAndScopedTransfers) {
    ir::ProgramBuilder builder({Project});
    auto script = builder.importedScript(baseline(), {}).value();
    auto section = builder.section({{}, script, "scene"}).value();
    auto root = builder.sectionBody(section);
    auto result = builder.expression(bit(121)).value();
    auto constant = builder.expression(sct::SctExpressionFactory::encodedDecimalLiteral(44)).value();
    auto parameter = builder.value(ir::ValueKind::Parameter, 0, 0, std::span(&constant, 1)).value();
    auto write = builder.value(ir::ValueKind::Instruction, 17, 0, std::span(&parameter, 1)).value();
    const std::array checks{result, result, ir::ValueId{}};
    auto chain = builder.conditional(root, checks).value();
    auto conditions = builder.branchConditions(chain);
    ASSERT_EQ(conditions.size(), 3); EXPECT_NE(conditions[0], conditions[1]); EXPECT_FALSE(conditions[2].valid());
    auto elseIfPrep = builder.conditionPreparation(conditions[1]);
    auto preparedWrite = builder.instruction(elseIfPrep, write).value();
    builder.metadata().annotations.push_back({conditions[1], "Second check", {{SctEvidenceKind::UserAssertion, "Prepare only after the first check fails"}}});
    std::vector<std::pair<SctActionId, SctActionId>> continues;
    for (auto kind : {ir::LoopKind::While, ir::LoopKind::DoWhile, ir::LoopKind::For}) {
        auto created = builder.loop(root, kind, result); ASSERT_TRUE(created);
        auto loop = *builder.loop(created.value());
        auto prep = builder.conditionPreparation(loop.condition);
        ASSERT_TRUE(builder.instruction(prep, write)); ASSERT_TRUE(builder.instruction(prep, write));
        const std::array cases{ir::Case{{5, 1, 3}, false, ir::CaseExit::Fallthrough}, ir::Case{{2}}, ir::Case{{8}, true}};
        auto selected = builder.switchRegion(loop.body, result, cases); ASSERT_TRUE(selected);
        auto caseBodies = builder.branchBodies(selected.value());
        auto continuation = builder.transfer(caseBodies[0], ir::TransferKind::Continue, loop.owner);
        ASSERT_TRUE(continuation); continues.emplace_back(loop.owner, continuation.value());
        ASSERT_TRUE(builder.transfer(caseBodies[1], ir::TransferKind::Break, selected.value()));
        ASSERT_TRUE(builder.transfer(caseBodies[2], ir::TransferKind::Break, loop.owner));
        // Break is also meaningful while preparing a condition.
        ASSERT_TRUE(builder.transfer(prep, ir::TransferKind::Break, loop.owner));
        if (kind == ir::LoopKind::For) {
            ASSERT_TRUE(builder.instruction(loop.initialize, write)); ASSERT_TRUE(builder.instruction(loop.update, write));
        } else { EXPECT_FALSE(loop.initialize.valid()); EXPECT_FALSE(loop.update.valid()); }
    }
    auto built = builder.finish(); ASSERT_TRUE(built) << messages(built.diagnostics());
    const auto& p = *built.value();
    EXPECT_EQ(p.find(preparedWrite)->owner, elseIfPrep);
    EXPECT_EQ(p.find(elseIfPrep)->role, ir::SequenceRole::ConditionPreparation);
    EXPECT_EQ(p.find(conditions[1])->arm, p.arms(chain)[1].id);
    EXPECT_EQ(p.uses({ir::Bank::Bit, 44}).size(), 9); // Else If, six check actions, For initialize/update.
    EXPECT_TRUE(std::ranges::any_of(p.uses({ir::Bank::Bit, 121}), [&](const ir::VariableUse& use) { return use.site.condition == conditions[1]; }));
    for (auto [id, continuation] : continues) {
        const auto* loop = p.loop(id); ASSERT_NE(loop, nullptr);
        auto destination = p.transferDestination(continuation); ASSERT_TRUE(destination);
        EXPECT_EQ(destination->construct, id);
        EXPECT_EQ(destination->sequence, loop->kind == ir::LoopKind::For ? loop->update : p.find(loop->condition)->preparation);
        EXPECT_EQ(p.incoming(id).size(), 3);
        auto switchId = p.find(loop->body)->order->front();
        EXPECT_TRUE(p.switchRegion(switchId)); EXPECT_EQ(p.arms(switchId)[0].caseExit, ir::CaseExit::Fallthrough);
        EXPECT_TRUE(p.arms(switchId)[2].includesDefault);
        auto sharedLabels = p.caseLabels(p.arms(switchId)[0].id);
        EXPECT_EQ((std::vector<std::int32_t>(sharedLabels.begin(), sharedLabels.end())), (std::vector<std::int32_t>{5, 1, 3}));
        EXPECT_EQ(p.arms(switchId).size(), 3); // Five numeric labels and Default, three owned bodies.
        EXPECT_EQ(p.caseLabels(p.arms(switchId)[2].id).front(), 8);
        auto exit = p.find(p.arms(switchId)[1].body)->order->front();
        EXPECT_FALSE(p.transferDestination(exit)->sequence.valid());
        EXPECT_EQ(p.transferDestination(exit)->construct, switchId);
        EXPECT_EQ(p.incoming(switchId)[0].kind, ir::TargetUseKind::Break);
    }
}
TEST(WholeProgramTest, RejectsInvalidStructuredScopesAndSwitchCases) {
    for (int invalid = 0; invalid < 9; ++invalid) {
        SCOPED_TRACE(invalid);
        ir::ProgramBuilder b({Project});
        auto script = b.importedScript(baseline(), {}).value();
        auto section = b.section({{}, script, "scene"}).value(); auto root = b.sectionBody(section);
        auto condition = b.expression(bit(1)).value();
        auto loopId = b.loop(root, ir::LoopKind::For, condition).value(); auto loop = *b.loop(loopId);
        const std::array cases{ir::Case{{1}}, ir::Case{{}, true}};
        auto switchId = b.switchRegion(loop.body, condition, cases).value();
        if (invalid == 0) EXPECT_FALSE(b.transfer(root, ir::TransferKind::Continue, loopId));
        if (invalid == 1) EXPECT_FALSE(b.transfer(b.branchBodies(switchId)[0], ir::TransferKind::Continue, switchId));
        if (invalid == 2) {
            const std::array duplicate{ir::Case{{1, 2}}, ir::Case{{2}}};
            EXPECT_FALSE(b.switchRegion(root, condition, duplicate));
        }
        if (invalid == 3) {
            const std::array tail{ir::Case{{}, true, ir::CaseExit::Fallthrough}};
            EXPECT_FALSE(b.switchRegion(root, condition, tail));
        }
        if (invalid == 4) EXPECT_FALSE(b.transfer(loop.update, ir::TransferKind::Continue, loopId));
        if (invalid == 5) {
            b.metadata().modules.push_back({{}, script, "Outer", {root}});
            b.metadata().modules.push_back({{}, script, "Overlap", {b.conditionPreparation(loop.condition)}});
        }
        if (invalid == 6) {
            const std::array duplicate{ir::Case{{1, 1}}};
            EXPECT_FALSE(b.switchRegion(root, condition, duplicate));
        }
        if (invalid == 7) {
            const std::array duplicate{ir::Case{{1}, true}, ir::Case{{}, true}};
            EXPECT_FALSE(b.switchRegion(root, condition, duplicate));
        }
        if (invalid == 8) {
            const std::array missingLabel{ir::Case{}};
            EXPECT_FALSE(b.switchRegion(root, condition, missingLabel));
        }
        EXPECT_FALSE(b.finish());
    }
}

}

TEST(WholeProgramTest, AmbiguousNativeTextUsesEditableDefaultAndHonorsOverride) {
    sct::SctDocument d;
    for (auto name : {"init", "loop"}) d.sections.push_back({d.allocateSectionId(), name, sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
    sct::SctMessage text; text.body.elements = {sct::SctTextChunk{"\xe3\x81\x82"}};
    d.sections.push_back({d.allocateSectionId(), "line", sct::SctStringSectionContent{{d.allocateStringId(), text, sct::SctTextKind::SctString}}});
    auto encoded = sct::SctDocumentExporter::exportDocument(d, sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding});
    ASSERT_TRUE(encoded.success);
    auto base = baseline(); base.textConvention.reset(); base.source.byteSize = encoded.bytes.size();
    base.source.revision.digest = sha256(std::as_bytes(std::span(encoded.bytes))).value();
    ir::ProgramBuilder builder({Project});
    auto ambiguous = ir::appendNative(builder, encoded.bytes, base, {}, {});
    ASSERT_TRUE(ambiguous) << messages(ambiguous.diagnostics());
    auto automatic=builder.finish();ASSERT_TRUE(automatic);
    EXPECT_EQ(automatic.value()->texts().size(),1);
    ir::ProgramBuilder overridden({Project});
    sct::SctDocumentImportOptions selected; selected.sourceTextEncoding = sct::kSctShiftJisByte7FEncoding;
    auto accepted = ir::appendNative(overridden, encoded.bytes, base, {}, selected);
    ASSERT_TRUE(accepted) << messages(accepted.diagnostics());
    auto built = overridden.finish(); ASSERT_TRUE(built);
    EXPECT_EQ(built.value()->texts().size(), 1);
    EXPECT_FALSE(built.value()->scripts()[0].context.platform);
}

TEST(WholeProgramTest, WesternIndexedTextAndShiftJisFooterImportIndependently) {
    sct::SctDocument d;auto footer=d.allocateSupplementaryTextId();auto messageFooter=d.allocateSupplementaryTextId();
    sct::SctDocumentInstruction print{d.allocateInstructionId(),23};
    print.fixedParameters={{0,sct::SctSupplementaryTextReference{footer}}};
    sct::SctDocumentInstruction messagePrint{d.allocateInstructionId(),24};messagePrint.fixedParameters={{0,sct::SctSupplementaryTextReference{messageFooter}}};
    d.sections.push_back({d.allocateSectionId(),"init",sct::SctScriptSectionContent{{print,messagePrint,{d.allocateInstructionId(),12}}}});
    d.sections.push_back({d.allocateSectionId(),"loop",sct::SctScriptSectionContent{{{d.allocateInstructionId(),12}}}});
    sct::SctMessage text;text.body.elements={sct::SctTextChunk{"caf\xc3\xa9"}};
    d.sections.push_back({d.allocateSectionId(),"line",sct::SctStringSectionContent{{d.allocateStringId(),text,sct::SctTextKind::SctString}}});
    d.supplementaryText.push_back({footer,sct::SctTextKind::PlainString,sct::SctOpaqueText{{0x81,0x40,0}}});
    d.supplementaryText.push_back({messageFooter,sct::SctTextKind::SctString,sct::SctOpaqueText{{0x81,0x40,0}}});
    auto encoded=sct::SctDocumentExporter::exportDocument(d,sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast,sct::kSctWindows1252Byte7FEncoding});ASSERT_TRUE(encoded.success);
    auto base=baseline();base.textConvention.reset();base.source.byteSize=encoded.bytes.size();base.source.revision.digest=sha256(std::as_bytes(std::span(encoded.bytes))).value();
    ir::ProgramBuilder builder({Project});auto imported=ir::appendNative(builder,encoded.bytes,base,{});ASSERT_TRUE(imported)<<messages(imported.diagnostics());
    auto program=builder.finish();ASSERT_TRUE(program)<<messages(program.diagnostics());
    EXPECT_EQ(program.value()->baselines().front().textConvention,sct::SctKnownTextConvention::Windows1252Byte7F);
    ASSERT_EQ(program.value()->texts().size(),3);
    EXPECT_EQ(program.value()->statistics().unavailableItems,0);
    for(const auto &record:program.value()->texts())if(record.supplementary) {
        auto value=record.value;
        if(record.kind==sct::SctTextKind::SctString)value=program.value()->children(value).front();
        EXPECT_EQ(program.value()->text(value),"\xe3\x80\x80");
    }
}
