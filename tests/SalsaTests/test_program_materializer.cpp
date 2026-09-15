#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctParser.h"
#include <gtest/gtest.h>
#include <array>
#include <map>

namespace {
using namespace salsa::core;
namespace ir = salsa::core::program;
namespace sct = spice::sct;
constexpr auto projectId = "e44499b5-dcf0-449a-9815-5bdf558eface";
const SctRealizationId realizationId{"f44499b5-dcf0-449a-9815-5bdf558eface"};
std::string messages(const std::vector<Diagnostic> &ds) {
    std::string out;
    for (const auto &d : ds)
        out += d.message + "\n";
    return out;
}
SctAuthoringBaseline baseline() {
    return {{},
            {Sha256Digest{{}}},
            {AssetLocator::fromRelativePath("field/a.sct").value(), 0, {Sha256Digest{{}}}},
            {"a44499b5-dcf0-449a-9815-5bdf558eface"},
            sct::SctKnownTextConvention::ShiftJisByte7F};
}
sct::SctCanonicalExpression expression(const char *text) {
    return *SctExpressionLanguage::parse(text).expression;
}
sct::SctDocumentExportOptions options() {
    return sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding};
}
ir::Checkpoint capture(std::shared_ptr<const ir::Program> p) {
    return {{{p->id(), p->revision(), 1}}, std::move(p)};
}
SctActionId emit(ir::ProgramBuilder &b, SctSequenceId seq, std::uint16_t opcode,
                 std::optional<std::uint32_t> operand = {}) {
    std::vector<ir::ValueId> parameters;
    if (operand) {
        auto word = b.expression(expression(std::to_string(*operand).c_str())).value();
        parameters.push_back(b.value(ir::ValueKind::Parameter, 0, 0, std::span(&word, 1)).value());
    }
    return b.instruction(seq, b.value(ir::ValueKind::Instruction, opcode, 0, parameters).value()).value();
}
SctSequenceId section(ir::ProgramBuilder &b, SctScriptId script, const char *name) {
    return b.sectionBody(b.section({{}, script, name}).value());
}
void roots(ir::ProgramBuilder &b, SctScriptId script) {
    for (auto name : {"init", "loop"})
        emit(b, section(b, script, name), 12);
}
sct::SctInstructionId location(const ir::MaterializedScript &m, ir::Subject subject,
                               std::optional<std::uint16_t> opcode = {}) {
    const auto index = sct::SctDocumentIndex::build(*m.document);
    for (const auto &r : m.realization.locations)
        if (r.subject == subject) {
            if (const auto *entity = std::get_if<sct::SctDocumentEntityId>(&r.location))
                if (const auto *id = std::get_if<sct::SctInstructionId>(entity)) {
                    const auto *i = index.find(*m.document, *id);
                    if (i && (!opcode || i->opcode == *opcode))
                        return *id;
                }
        }
    return {};
}
const sct::SctDocumentInstruction &at(const ir::MaterializedScript &m, sct::SctInstructionId id) {
    return *sct::SctDocumentIndex::build(*m.document).find(*m.document, id);
}
sct::SctInstructionId target(const sct::SctDocumentInstruction &i, std::uint32_t param,
                             std::optional<std::uint32_t> group = {}) {
    const auto &ps = group ? i.repeatedParameterGroups.at(*group).parameters : i.fixedParameters;
    return std::get<sct::SctInstructionReference>(
               std::ranges::find(ps, param, &sct::SctDocumentParameter::schemaIndex)->value)
        .target;
}
TEST(ProgramMaterializerTest, StructuredControlFlowAndLocalTemplateReuse) {
    ir::ProgramBuilder b({projectId});
    const auto script = b.importedScript(baseline(), {}).value();
    roots(b, script);
    const auto root = section(b, script, "story");
    const auto condition = b.expression(expression("BitVar[121]")).value();
    const std::array conditions{condition, condition, ir::ValueId{}};
    const auto chain = b.conditional(root, conditions).value();
    const auto bodies = b.branchBodies(chain);
    const auto checks = b.branchConditions(chain);
    const auto first = emit(b, bodies[0], 17, 121);
    emit(b, bodies[1], 17, 122);
    emit(b, bodies[2], 18, 121);
    const auto prep = emit(b, b.conditionPreparation(checks[1]), 17, 123);
    const auto forId = b.loop(root, ir::LoopKind::For, condition).value();
    const auto loop = *b.loop(forId);
    const auto initial = emit(b, loop.initialize, 17, 124);
    const auto checkPrep = emit(b, b.conditionPreparation(loop.condition), 17, 125);
    const auto update = emit(b, loop.update, 18, 125);
    const std::array cases{ir::Case{{1, 2}, true, ir::CaseExit::Fallthrough}, ir::Case{{3}, false}};
    const auto sw = b.switchRegion(loop.body, b.expression(expression("ByteVar[6]")).value(), cases).value();
    const auto caseBodies = b.branchBodies(sw);
    emit(b, caseBodies[0], 17, 126);
    const auto cont = b.transfer(caseBodies[1], ir::TransferKind::Continue, forId).value();
    const auto doId = b.loop(root, ir::LoopKind::DoWhile, condition).value();
    const auto doLoop = *b.loop(doId);
    const auto doBody = emit(b, doLoop.body, 17, 127);
    const auto doPrep = emit(b, b.conditionPreparation(doLoop.condition), 18, 127);
    const auto doContinue = b.transfer(doLoop.body, ir::TransferKind::Continue, doId).value();
    const auto whileId = b.loop(root, ir::LoopKind::While, condition).value();
    const auto whileLoop = *b.loop(whileId);
    const auto leave = b.transfer(whileLoop.body, ir::TransferKind::Break, whileId).value();
    const auto end = emit(b, root, 12);
    const auto callers = section(b, script, "internal");
    const auto reference = b.value(ir::ValueKind::ActionReference, first.value).value();
    const auto callParameter = b.value(ir::ValueKind::Parameter, 0, 0, std::span(&reference, 1)).value();
    const auto internalCall =
        b.instruction(callers, b.value(ir::ValueKind::Instruction, 11, 0, std::span(&callParameter, 1)).value())
            .value();
    emit(b, callers, 12);
    // All arms return, so this final chain needs no invented continuation.
    const auto terminal = section(b, script, "terminal");
    const std::array terminalConditions{condition, ir::ValueId{}};
    const auto terminalChain = b.conditional(terminal, terminalConditions).value();
    for (auto body : b.branchBodies(terminalChain))
        emit(b, body, 12);
    auto built = b.finish();
    ASSERT_TRUE(built) << messages(built.diagnostics());
    ir::ProgramService service;
    auto adopted = service.adopt({}, std::move(built).takeValue()).get();
    ASSERT_TRUE(adopted);
    auto revision = adopted.value().current;
    auto captured = service.checkpoint(revision).get().value();
    ir::ProgramMaterializer materializer;
    auto output = materializer.exportScript(captured, revision, script, realizationId, options());
    ASSERT_TRUE(output) << messages(output.diagnostics());
    const auto &m = output.value().materialized;
    EXPECT_EQ(target(at(m, location(m, checks[0], 0)), 1), location(m, prep)); // false -> next preparation
    EXPECT_EQ(target(at(m, location(m, checks[1], 0)), 1), location(m, captured.program->arms(chain)[2].body));
    EXPECT_EQ(target(at(m, location(m, chain, 10)), 0), location(m, initial));        // normal arm exit
    EXPECT_EQ(target(at(m, location(m, loop.condition, 0)), 1), location(m, doBody)); // failed final check exits
    EXPECT_EQ(target(at(m, location(m, cont)), 0), location(m, update));
    const auto &switchInstruction = at(m, location(m, sw, 3));
    ASSERT_EQ(switchInstruction.repeatedParameterGroups.size(), 4u);
    EXPECT_EQ(target(switchInstruction, 3, 0), target(switchInstruction, 3, 1));
    EXPECT_EQ(target(switchInstruction, 3, 0), target(switchInstruction, 3, 3));
    EXPECT_NE(target(switchInstruction, 3, 2), target(switchInstruction, 3, 3));
    EXPECT_EQ(target(at(m, location(m, doContinue)), 0), location(m, doPrep));
    EXPECT_EQ(target(at(m, location(m, leave)), 0), location(m, end));
    EXPECT_EQ(target(at(m, location(m, internalCall)), 0), location(m, first));
    EXPECT_FALSE(m.document->opaqueAttachments.size());
    auto repeated = materializer.exportScript(captured, revision, script, realizationId, options());
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated.value().bytes, output.value().bytes);
    EXPECT_EQ(repeated.value().materialized.loweredActions, 0u);
    auto w = ir::ValueBuffer::expression(expression("144")).value();
    auto edited = service.edit({revision, {ir::ReplaceParameter{first, {0, {}}, w}}}).get();
    ASSERT_TRUE(edited);
    revision = edited.value().status.current;
    auto editedCapture = service.checkpoint(revision).get().value();
    auto changed = materializer.exportScript(editedCapture, revision, script, realizationId, options());
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed.value().materialized.loweredActions, 1u); // no ancestor recompilation
    EXPECT_FALSE(m.realization.fresh(revision, script, realizationId));
    auto file = ir::ProgramPersistence::encode(*editedCapture.program);
    ASSERT_TRUE(file);
    auto reopened = ir::ProgramPersistence::decode(file.value());
    ASSERT_TRUE(reopened);
    auto reopenedCapture = capture(reopened.value().program);
    auto reexport =
        materializer.exportScript(reopenedCapture, reopenedCapture.status.current, script, realizationId, options());
    ASSERT_TRUE(reexport);
    EXPECT_EQ(reexport.value().bytes, changed.value().bytes);
    revision = service.undo(revision).get().value().status.current;
    auto undone = materializer.exportScript(service.checkpoint(revision).get().value(), revision, script, realizationId,
                                            options());
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone.value().bytes, output.value().bytes);
    EXPECT_TRUE(sct::SctParser{}.parse(output.value().bytes).parseOk); // one boundary smoke check, no edit-time reparse
    service.shutdown().get();
}
TEST(ProgramMaterializerTest, ActivationBypassesKeepBodiesSchedulesAndTypedText) {
    sct::SctDocument d;
    auto guard = d.allocateInstructionId(), prepare = d.allocateInstructionId(), call = d.allocateInstructionId(),
         consume = d.allocateInstructionId(), end = d.allocateInstructionId();
    auto scene = d.allocateInstructionId(), sceneEnd = d.allocateInstructionId();
    d.sections.push_back(
        {d.allocateSectionId(), "init", sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
    d.sections.push_back(
        {d.allocateSectionId(), "loop",
         sct::SctScriptSectionContent{
             {{guard, 0, false, {}, {{0, expression("BitVar[121]")}, {1, sct::SctInstructionReference{end}}}},
              {prepare, 16, true, expression("20"), {{0, expression("3")}}},
              {call, 11, false, {}, {{0, sct::SctInstructionReference{scene}}}},
              {consume, 18, false, {}, {{0, expression("121")}}},
              {end, 12}}}});
    // Physical predecessor must continue to the original body around a wrapper.
    auto preceding = d.allocateInstructionId();
    d.sections.push_back({d.allocateSectionId(), "prefix",
                          sct::SctScriptSectionContent{{{preceding, 17, false, {}, {{0, expression("122")}}}}}});
    d.sections.push_back(
        {d.allocateSectionId(), "M00001",
         sct::SctScriptSectionContent{
             {{scene, 0, true, {}, {{0, expression("1")}, {1, sct::SctInstructionReference{sceneEnd}}}},
              {sceneEnd, 12}}}});
    sct::SctMessage msg;
    msg.headerUtf8 = "Name";
    msg.body.elements = {
        sct::SctTextChunk{"Hello"},
        sct::SctInlineCommand{sct::SctMessageCommandCode::A, sct::SctDecimalCommandArgument{2}},
        sct::SctInlineCommand{sct::SctMessageCommandCode::P, sct::SctByteListCommandArgument{{1, 2, 3}}}};
    d.sections.push_back({d.allocateSectionId(), "group", sct::SctStringGroupMarkerSectionContent{}});
    d.sections.push_back({d.allocateSectionId(), "message",
                          sct::SctStringSectionContent{{d.allocateStringId(), msg, sct::SctTextKind::SctString}}});
    d.supplementaryText.push_back(
        {d.allocateSupplementaryTextId(), sct::SctTextKind::PlainString, sct::SctPlainText{"debug"}});
    d.supplementaryText.push_back(
        {d.allocateSupplementaryTextId(), sct::SctTextKind::PlainString, sct::SctOpaqueText{{0x82}}});
    const auto messageId = std::get<sct::SctStringSectionContent>(d.sections.back().content).string.id;
    auto &initCode = std::get<sct::SctScriptSectionContent>(d.sections.front().content).instructions;
    initCode.insert(
        initCode.begin(),
        {{d.allocateInstructionId(), 144, false, {}, {{0, sct::SctStringReference{messageId}}, {1, expression("0")}}},
         {d.allocateInstructionId(),
          23,
          false,
          {},
          {{0, sct::SctSupplementaryTextReference{d.supplementaryText.front().id}}}}});
    ir::ProgramBuilder b({projectId});
    auto imported = b.appendImported(d, baseline(), {});
    ASSERT_TRUE(imported);
    const auto script = imported.value();
    auto built = b.finish();
    ASSERT_TRUE(built);
    auto original = capture(std::move(built).takeValue());
    const auto &p = *original.program;
    auto sections = p.find(script)->sections;
    auto loop = p.find(sections[1])->body, body = p.find(sections[3])->body;
    // Import now nests recognized groups; locate the original instructions by
    // baseline-qualified source identity instead of indexing the root sequence.
    std::map<std::uint64_t, SctActionId> sourceActions;
    std::function<void(SctSequenceId)> collect = [&](auto sequence) {
        for (auto id : *p.find(sequence)->order) {
            sourceActions.emplace(p.find(id)->sourceInstruction, id);
            for (const auto &arm : p.arms(id)) collect(arm.body);
        }
    };
    collect(loop);
    collect(body);
    std::array actions{sourceActions.at(guard.value()), sourceActions.at(prepare.value()),
                       sourceActions.at(call.value()), sourceActions.at(consume.value()),
                       sourceActions.at(end.value())};
    auto sceneAction = sourceActions.at(scene.value());
    ir::ProgramService service;
    auto opened = service.adopt({}, std::move(original.program)).get();
    ASSERT_TRUE(opened);
    auto rev = opened.value().current;
    ir::Entrypoint named;
    named.script = script;
    named.name = "M00001";
    named.body = body;
    named.enabled = false;
    named.activation = SctNamedActivation{"M00001", sceneAction};
    ir::Entrypoint route;
    route.script = script;
    route.name = "Arrival";
    route.body = loop;
    route.enabled = false;
    route.activation = SctLocalActivation{actions[0], {}, actions[1], actions[4]};
    auto edit =
        service
            .edit({rev, {ir::SemanticOperation{ir::PutMetadata{named}}, ir::SemanticOperation{ir::PutMetadata{route}}}})
            .get();
    ASSERT_TRUE(edit) << messages(edit.diagnostics());
    rev = edit.value().status.current;
    ir::ProgramMaterializer materializer;
    auto cp = service.checkpoint(rev).get().value();
    auto output = materializer.exportScript(cp, rev, script, realizationId, options());
    ASSERT_TRUE(output) << messages(output.diagnostics());
    const auto &m = output.value().materialized;
    const auto &code = std::get<sct::SctScriptSectionContent>(m.document->sections[1].content).instructions;
    // Structured lowering may emit an additional explicit continuation jump.
    ASSERT_GE(code.size(), 6u);
    EXPECT_EQ(code[1].opcode, 10);
    EXPECT_EQ(target(code[1], 0), location(m, actions[4]));
    EXPECT_TRUE(at(m, location(m, actions[1])).scheduledExpression);
    EXPECT_TRUE(at(m, location(m, actions[1])).skipRefresh);
    EXPECT_EQ(target(at(m, location(m, actions[2])), 0), location(m, sceneAction));
    const auto wrapper =
        std::ranges::find(m.document->sections, std::string("M00001"), &sct::SctDocumentSection::nameBytes);
    ASSERT_NE(wrapper, m.document->sections.end());
    EXPECT_EQ(std::get<sct::SctScriptSectionContent>(wrapper->content).instructions.front().opcode, 0);
    EXPECT_NE(std::get<sct::SctScriptSectionContent>(wrapper->content).instructions.front().id,
              location(m, sceneAction));
    const auto &prefix = std::get<sct::SctScriptSectionContent>(m.document->sections[2].content).instructions;
    EXPECT_EQ(prefix.size(), 2u);
    EXPECT_EQ(target(prefix.back(), 0), location(m, sceneAction));
    EXPECT_EQ(m.document->supplementaryText.size(), 1u);
    const auto &initOutput = std::get<sct::SctScriptSectionContent>(m.document->sections.front().content).instructions;
    EXPECT_EQ(std::get<sct::SctSupplementaryTextReference>(initOutput[1].fixedParameters[0].value).target,
              m.document->supplementaryText.front().id);
    const auto &messageOutput = std::get<sct::SctStringSectionContent>(m.document->sections.back().content).string;
    EXPECT_EQ(std::get<sct::SctStringReference>(initOutput[0].fixedParameters[0].value).target, messageOutput.id);
    EXPECT_EQ(std::get<sct::SctMessage>(messageOutput.value).headerUtf8, "Name");
    auto dc = materializer.exportScript(cp, rev, script, realizationId,
                                        sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast,
                                                                      sct::kSctShiftJisByte7FEncoding,
                                                                      sct::SctDocumentOutputByteOrder::LittleEndian,
                                                                      sct::SctDocumentOutputWrapper::Aklz});
    ASSERT_TRUE(dc) << messages(dc.diagnostics());
    EXPECT_TRUE(sct::SctParser{}.parse(dc.value().bytes).parseOk);
    EXPECT_TRUE(std::ranges::any_of(m.realization.locations, [](const auto &r) {
        return r.generated && std::holds_alternative<SctEntrypointId>(r.subject);
    }));
    rev = service.undo(rev).get().value().status.current;
    auto active =
        materializer.exportScript(service.checkpoint(rev).get().value(), rev, script, realizationId, options());
    ASSERT_TRUE(active);
    EXPECT_EQ(std::get<sct::SctScriptSectionContent>(active.value().materialized.document->sections[1].content)
                  .instructions.size(),
              code.size() - 1); // Reenabling removes only the generated bypass.
    auto current = service.checkpoint(rev).get().value();
    const auto initSequence = current.program->find(current.program->find(script)->sections.front())->body;
    ir::ValueBuffer ret;
    ret.add(ir::ValueKind::Instruction, 12);
    auto bodyEdit = service.edit({rev, {ir::InsertInstruction{initSequence, 0, ret}}}).get();
    ASSERT_TRUE(bodyEdit);
    rev = bodyEdit.value().status.current;
    // Engine roots enter the edited body; their old imported first-action
    // observation must not make this valid edit fail activation lowering.
    auto editedRoot =
        materializer.exportScript(service.checkpoint(rev).get().value(), rev, script, realizationId, options());
    ASSERT_TRUE(editedRoot) << messages(editedRoot.diagnostics());
    EXPECT_EQ(std::get<sct::SctScriptSectionContent>(editedRoot.value().materialized.document->sections.front().content)
                  .instructions.front()
                  .opcode,
              12);
    service.shutdown().get();
}
TEST(ProgramMaterializerTest, DraftAndStaleFailuresAreAtomic) {
    ir::ProgramBuilder b({projectId});
    auto script = b.importedScript(baseline(), {}).value();
    emit(b, section(b, script, "draft"), 12);
    auto built = b.finish();
    ASSERT_TRUE(built);
    auto cp = capture(std::move(built).takeValue());
    EXPECT_TRUE(ir::ProgramPersistence::encode(*cp.program));
    ir::ProgramMaterializer materializer;
    EXPECT_FALSE(materializer.materialize(cp, cp.status.current, script, realizationId));
    auto stale = cp.status.current;
    ++stale.revision;
    EXPECT_FALSE(materializer.materialize(cp, stale, script, realizationId));
    stale = cp.status.current;
    ++stale.generation;
    EXPECT_FALSE(materializer.materialize(cp, stale, script, realizationId));
    ir::Realization map{cp.status.current, script, realizationId};
    EXPECT_TRUE(map.fresh(cp.status.current, script, realizationId));
    EXPECT_FALSE(map.fresh(cp.status.current, SctScriptId{script.value + 1}, realizationId));
    EXPECT_FALSE(map.fresh(cp.status.current, script, {projectId}));
    stale = cp.status.current;
    stale.project.value = "b44499b5-dcf0-449a-9815-5bdf558eface";
    EXPECT_FALSE(map.fresh(stale, script, realizationId));
    ir::ProgramBuilder unsupported({projectId});
    script = unsupported.importedScript(baseline(), {}).value();
    roots(unsupported, script);
    auto seq = section(unsupported, script, "broken");
    auto missing = unsupported.value(ir::ValueKind::Unavailable, 1, 2).value();
    auto parameter = unsupported.value(ir::ValueKind::Parameter, 0, 0, std::span(&missing, 1)).value();
    unsupported.instruction(seq,
                            unsupported.value(ir::ValueKind::Instruction, 11, 0, std::span(&parameter, 1)).value());
    emit(unsupported, seq, 12);
    auto incomplete = unsupported.finish();
    ASSERT_TRUE(incomplete);
    auto bad = capture(std::move(incomplete).takeValue());
    EXPECT_FALSE(materializer.materialize(bad, bad.status.current, script, realizationId));
}
} // namespace
