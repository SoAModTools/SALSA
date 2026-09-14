#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
namespace {
using namespace salsa::core;
namespace ir = salsa::core::program;
namespace sct = spice::sct;
std::string messages(const std::vector<Diagnostic> &ds) {
    std::string s;
    for (auto &d : ds)
        s += d.message + "\n";
    return s;
}
ir::ValueBuffer value(ir::ValueKind kind, std::uint64_t data) {
    ir::ValueBuffer b;
    b.add(kind, data);
    return b;
}
ir::ValueBuffer expr(const char *text) {
    return ir::ValueBuffer::expression(*SctExpressionLanguage::parse(text).expression).value();
}
ir::Primitive semantic(ir::SemanticOperation op) { return op; }
struct Fixture {
    ir::ProgramService service;
    ir::Revision revision;
    SctScriptId script, second;
    SctSequenceId scene, other, handler, helper, destination;
    SctActionId start, finish, otherStart, guard, response, handlerEnd, helperStart, helperReturn, load,
        arrivalResponse;
    SctTextResourceId filename;
    const SctModuleId module{1000}, otherModule{1001}, destinationModule{1002}, handlerModule{1003};
    const SctPortId entry{2000}, exit{2001}, otherEntry{2002}, loadExit{2003}, destinationEntry{2004},
        handlerExit{2005}, afterStart{2006};
    const SctEntrypointId arrival{3000};
    Fixture() {
        ir::ProgramBuilder b({"e44499b5-dcf0-449a-9815-5bdf558eface"});
        auto imported = [&](const char *name, const char *uuid) {
            return b
                .importedScript({{},
                                 {Sha256Digest{{}}},
                                 {AssetLocator::fromRelativePath(name).value(), 0, {Sha256Digest{{}}}},
                                 {uuid},
                                 sct::SctKnownTextConvention::ShiftJisByte7F},
                                {})
                .value();
        };
        script = imported("field/me002a.sct", "a44499b5-dcf0-449a-9815-5bdf558eface");
        second = imported("field/me003b.sct", "b44499b5-dcf0-449a-9815-5bdf558eface");
        auto section = [&](SctScriptId script, const char *name) {
            return b.sectionBody(b.section({{}, script, name}).value());
        };
        auto emit = [&](SctSequenceId seq, std::uint32_t op,
                        std::vector<std::pair<unsigned, ir::ValueId>> params = {}) {
            std::vector<ir::ValueId> ps;
            for (auto [index, v] : params)
                ps.push_back(b.value(ir::ValueKind::Parameter, index, 0, std::span(&v, 1)).value());
            return b.instruction(seq, b.value(ir::ValueKind::Instruction, op, 0, ps).value()).value();
        };
        for (auto id : {script, second})
            for (auto name : {"init", "loop"})
                emit(section(id, name), 12);
        helper = section(script, "helper");
        helperStart =
            emit(helper, 17, {{0, b.expression(*SctExpressionLanguage::parse("150").expression).value()}});
        helperReturn = emit(helper, 12);
        scene = section(script, "scene");
        start = emit(scene, 11, {{0, b.value(ir::ValueKind::ActionReference, helperStart.value).value()}});
        filename = b.textEntity({{},
                                 script,
                                 b.text(ir::ValueKind::PlainText, "me099a.sct").value(),
                                 sct::SctTextKind::PlainString,
                                 true})
                       .value();
        load = emit(scene, 43, {{0, b.value(ir::ValueKind::SupplementaryTextReference, filename.value).value()}});
        finish = emit(scene, 12);
        other = section(script, "other");
        otherStart = emit(other, 18, {{0, b.expression(*SctExpressionLanguage::parse("190").expression).value()}});
        emit(other, 12);
        handler = section(script, "M00001");
        // Forward ID is an ordinary builder reference, qualified after construction.
        handlerEnd = SctActionId{800};
        guard = emit(handler, 0,
                     {{0, b.expression(*SctExpressionLanguage::parse("BitVar[121] == 0").expression).value()},
                      {1, b.value(ir::ValueKind::ActionReference, handlerEnd.value).value()}});
        response = emit(handler, 11, {{0, b.value(ir::ValueKind::ActionReference, start.value).value()}});
        b.instruction(handler, b.value(ir::ValueKind::Instruction, 12).value(), handlerEnd);
        destination = section(second, "destination");
        auto destStart = emit(destination, 12);
        auto arrivals = section(second, "arrivals");
        arrivalResponse =
            emit(arrivals, 11, {{0, b.value(ir::ValueKind::ActionReference, destStart.value).value()}});
        emit(arrivals, 12);
        b.metadata().modules = {{module, script, "Scene", {scene}},
                                {otherModule, script, "Other", {other}},
                                {destinationModule, second, "Destination", {destination}},
                                {handlerModule, script, "Handler", {handler}}};
        b.metadata().ports = {
            {entry, module, "Begin", start, scene, ir::SlotRole::Entry},
            {exit, module, "End", finish, scene, ir::SlotRole::Exit, ir::SlotBoundary::Terminal},
            {otherEntry, otherModule, "Begin", otherStart, other, ir::SlotRole::Entry},
            {loadExit, module, "Field transition", load, scene, ir::SlotRole::Exit, ir::SlotBoundary::Operand,
             sct::SctParameterAddress{0, {}}},
            {destinationEntry, destinationModule, "Begin", destStart, destination, ir::SlotRole::Entry},
            {handlerExit, handlerModule, "Response", response, handler, ir::SlotRole::Exit,
             ir::SlotBoundary::Operand, sct::SctParameterAddress{0, {}}},
            {afterStart, module, "After helper", start, scene, ir::SlotRole::Exit, ir::SlotBoundary::Fallthrough}};
        ir::Entrypoint e;
        e.id = arrival;
        e.script = second;
        e.name = "Arriving";
        e.kind = SctEntrypointKind::Arrival;
        e.body = arrivals;
        e.response = destinationEntry;
        e.responseSite = ir::Site{arrivalResponse, sct::SctParameterAddress{0, {}}};
        b.metadata().entrypoints.push_back(e);
        auto built = b.finish();
        if (!built)
            throw std::runtime_error(messages(built.diagnostics()));
        auto opened = service.adopt({}, std::move(built).takeValue()).get();
        if (!opened)
            throw std::runtime_error(messages(opened.diagnostics()));
        revision = opened.value().current;
    }
    ~Fixture() { service.shutdown().get(); }
    ir::Checkpoint checkpoint() { return service.checkpoint(revision).get().value(); }
    ir::Connection connection(SctPortId source, SctPortId target) {
        ir::Connection c;
        c.source = source;
        c.destination = target;
        c.cutscene = ir::RouteDefinition{};
        return c;
    }
    ir::ChangeReceipt edit(ir::Primitive op) {
        auto result = service.edit({revision, {std::move(op)}}).get();
        if (!result)
            throw std::runtime_error(messages(result.diagnostics()));
        revision = result.value().status.current;
        return result.value();
    }
};
TEST(ProgramCutsceneTest, IncomingGuardsWholeBodyEffectsAndScopedInvalidation) {
    Fixture f;
    auto q = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(q) << messages(q.diagnostics());
    const auto &summary = *q.value().summary;
    auto route = std::ranges::find(summary.incoming, f.response, &ir::CutsceneRouteView::source);
    ASSERT_NE(route, summary.incoming.end());
    ASSERT_FALSE(route->paths.empty());
    ASSERT_FALSE(route->paths.front().requirements.empty());
    EXPECT_EQ(route->paths.front().requirements.front().site.action, f.guard);
    EXPECT_TRUE(std::ranges::any_of(summary.effects, [&](auto &use) {
        return use.site.action == f.helperStart && use.access == ir::Access::Set;
    }));
    EXPECT_FALSE(std::ranges::any_of(summary.exits, [&](auto &exit) { return exit.source == f.helperReturn; }));
    EXPECT_FALSE(std::ranges::any_of(summary.effects, [&](auto &use) { return use.site.action == f.otherStart; }));
    EXPECT_TRUE(f.service.cutscene(f.revision, f.module).get().value().reused);
    f.edit(ir::ReplaceParameter{f.otherStart, {0, {}}, expr("191")});
    EXPECT_TRUE(f.service.cutscene(f.revision, f.module).get().value().reused);
    f.edit(ir::ReplaceParameter{f.guard, {0, {}}, expr("BitVar[122] == 0")});
    auto changed = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(changed);
    EXPECT_FALSE(changed.value().reused);
    f.edit(ir::ReplaceParameter{f.response, {0, {}}, value(ir::ValueKind::ActionReference, f.otherStart.value)});
    auto removed = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(removed);
    EXPECT_TRUE(removed.value().summary->incoming.empty());
    f.edit(ir::ReplaceParameter{f.response, {0, {}}, value(ir::ValueKind::ActionReference, f.start.value)});
    EXPECT_FALSE(f.service.cutscene(f.revision, f.module).get().value().reused);
}
TEST(ProgramCutsceneTest, ConnectRetargetRoundTripUndoAndExecutableOutput) {
    Fixture f;
    auto c = f.connection(f.exit, f.otherEntry);
    c.cutscene->paths = {
        {{{{f.guard, sct::SctParameterAddress{0, {}}}, ir::RequirementMatch::True, {}}}, {f.start}, {f.load}}};
    auto receipt = f.edit(semantic(ir::ConnectSlots{c}));
    ASSERT_EQ(receipt.metadata.size(), 1u);
    c.id = SctConnectionId{receipt.metadata.front()};
    auto checkpoint = f.checkpoint();
    auto encoded = ir::ProgramPersistence::encode(*checkpoint.program);
    ASSERT_TRUE(encoded);
    auto decoded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
    const auto &stored = decoded.value().program->metadata().connections.front();
    EXPECT_EQ(stored.id, c.id);
    EXPECT_EQ(stored.cutscene->implementation, f.finish);
    EXPECT_EQ(stored.cutscene->paths.front().requirements.front().site.action, f.guard);
    EXPECT_EQ(ir::ProgramPersistence::encode(*decoded.value().program).value(), encoded.value());
    auto q = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(q);
    EXPECT_FALSE(q.value().summary->exits.back().changed);
    ir::ProgramMaterializer materializer;
    auto exported = materializer.exportScript(
        checkpoint, f.revision, f.script, {"f44499b5-dcf0-449a-9815-5bdf558eface"},
        sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding});
    ASSERT_TRUE(exported) << messages(exported.diagnostics());
    EXPECT_TRUE(std::ranges::any_of(exported.value().materialized.realization.locations,
                                    [&](auto &location) { return location.subject == ir::Subject{c.id}; }));
    auto undo = f.service.undo(f.revision).get();
    ASSERT_TRUE(undo);
    f.revision = undo.value().status.current;
    EXPECT_TRUE(f.checkpoint().program->metadata().connections.empty());
    auto redo = f.service.redo(f.revision).get();
    ASSERT_TRUE(redo);
    f.revision = redo.value().status.current;
    f.edit(ir::ReplaceParameter{f.finish, {0, {}}, value(ir::ValueKind::ActionReference, f.start.value)});
    auto modified = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(modified);
    EXPECT_TRUE(modified.value().summary->exits.back().changed);
    // A second connection cannot own the same terminal, and a failed command is atomic.
    auto before = f.revision;
    auto duplicate =
        f.service.edit({f.revision, {semantic(ir::ConnectSlots{f.connection(f.exit, f.entry)})}}).get();
    EXPECT_FALSE(duplicate);
    EXPECT_EQ(f.service.catalog(f.revision).get().value().status.current, before);
}
TEST(ProgramCutsceneTest, FallthroughScheduledCallAndUnresolvedDraft) {
    Fixture f;
    auto c = f.connection(f.afterStart, f.otherEntry);
    auto receipt = f.edit(semantic(ir::ConnectSlots{c, ir::RouteOutcome::ScheduledCall, expr("1")}));
    c.id = SctConnectionId{receipt.metadata.front()};
    auto cp = f.checkpoint();
    const auto &stored = cp.program->metadata().connections.front();
    ASSERT_TRUE(stored.cutscene->implementation);
    auto generated = *stored.cutscene->implementation;
    auto *s = cp.program->find(f.scene);
    auto i = std::ranges::find(*s->order, generated);
    ASSERT_NE(i, s->order->end());
    EXPECT_EQ(*(i + 1), f.load);
    auto firstCount = s->order->size();
    c = stored;
    c.destination = f.entry;
    f.edit(semantic(ir::ConnectSlots{c, ir::RouteOutcome::ScheduledCall, expr("2")}));
    cp = f.checkpoint();
    EXPECT_EQ(cp.program->find(f.scene)->order->size(), firstCount);
    auto view = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(view);
    EXPECT_TRUE(view.value().summary->exits.back().schedule.valid());
    f.edit(ir::InsertInstruction{f.scene, 1, value(ir::ValueKind::Instruction, 13)});
    auto detached = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(detached);
    EXPECT_TRUE(detached.value().summary->exits.back().changed);
    EXPECT_FALSE(
        f.service.edit({f.revision, {semantic(ir::ConnectSlots{c, ir::RouteOutcome::ScheduledCall, expr("2")})}})
            .get());
    auto draft = f.connection(f.exit, f.entry);
    draft.destination = SctUnresolvedBinding{"unknown scene",
                                             "User has not selected a destination",
                                             {{SctEvidenceKind::UserAssertion, "Draft connection"}}};
    f.edit(semantic(ir::PutMetadata{draft}));
    auto encoded = ir::ProgramPersistence::encode(*f.checkpoint().program);
    ASSERT_TRUE(encoded);
    auto decoded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<SctUnresolvedBinding>(
        decoded.value().program->metadata().connections.back().destination));
}
TEST(ProgramCutsceneTest, LiteralCrossScriptConnectionKeepsSelectedArrivalLogicAndSharedText) {
    Fixture f;
    auto c = f.connection(f.loadExit, f.destinationEntry);
    c.cutscene->arrival = f.arrival;
    auto before = f.checkpoint();
    auto receipt = f.edit(semantic(ir::ConnectSlots{c, ir::RouteOutcome::ScriptTransition}));
    EXPECT_EQ(receipt.actions.size(), 2u);
    auto cp = f.checkpoint();
    EXPECT_NE(cp.program->find(f.filename), nullptr);
    EXPECT_EQ(cp.program->metadata().connections.back().cutscene->boundFilename, "me003b.sct");
    EXPECT_EQ(cp.program->find(f.script)->supplementaryText.size(),
              before.program->find(f.script)->supplementaryText.size() + 1);
    auto result = f.service.cutscene(f.revision, f.module).get();
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().summary->exits.back().changed);
    EXPECT_EQ(result.value().summary->exits.back().destinationScript, f.second);
    ir::ProgramMaterializer materializer;
    for (auto script : {f.script, f.second}) {
        auto exported = materializer.exportScript(
            cp, f.revision, script, {"f44499b5-dcf0-449a-9815-5bdf558eface"},
            sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding});
        ASSERT_TRUE(exported) << messages(exported.diagnostics());
    }
    c = cp.program->metadata().connections.back();
    auto count = cp.program->find(f.script)->supplementaryText.size();
    f.edit(semantic(ir::ConnectSlots{c, ir::RouteOutcome::ScriptTransition}));
    EXPECT_EQ(f.checkpoint().program->find(f.script)->supplementaryText.size(), count);
    auto unsupported = f.connection(f.exit, f.destinationEntry);
    EXPECT_FALSE(f.service.edit({f.revision, {semantic(ir::ConnectSlots{unsupported})}}).get());
}
TEST(ProgramCutsceneTest, ExactSwitchOperandAndSequenceEndCommands) {
    Fixture f;
    ir::ValueBuffer switchValue;
    auto selector = switchValue.add(ir::ValueKind::Word, 0);
    // The fixture exercises exact parameter identity; expression schema validation
    // remains at export, independently of this edit/query contract.
    auto fixed = switchValue.add(ir::ValueKind::Parameter, 0, 0, std::span(&selector, 1));
    auto target = switchValue.add(ir::ValueKind::ActionReference, f.start.value);
    auto label = switchValue.add(ir::ValueKind::Word, 7);
    auto labelParameter = switchValue.add(ir::ValueKind::Parameter, 2, 0, std::span(&label, 1));
    auto referenceParameter = switchValue.add(ir::ValueKind::Parameter, 3, 0, std::span(&target, 1));
    std::array parameters{labelParameter, referenceParameter};
    auto group = switchValue.add(ir::ValueKind::RepeatedGroup, 0, 0, parameters);
    std::array children{fixed, group, group};
    switchValue.add(ir::ValueKind::Instruction, 3, 0, children);
    auto port = f.checkpoint().program->metadata().ports.at(5);
    port.operand = sct::SctParameterAddress{3, 1};
    auto coordinated =
        f.service
            .edit({f.revision, {ir::ReplaceInstruction{f.response, switchValue}, semantic(ir::PutMetadata{port})}})
            .get();
    ASSERT_TRUE(coordinated) << messages(coordinated.diagnostics());
    f.revision = coordinated.value().status.current;
    auto c = f.connection(f.handlerExit, f.otherEntry);
    f.edit(semantic(ir::ConnectSlots{c}));
    auto incoming = f.service.incoming(f.revision, f.start).get();
    ASSERT_TRUE(incoming);
    auto count =
        std::ranges::count_if(incoming.value().uses, [&](auto &use) { return use.site.action == f.response; });
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(std::ranges::any_of(incoming.value().uses, [&](auto &use) {
        return use.site.action == f.response && use.site.parameter->repeatedGroupOrdinal == 0;
    }));
    ir::Port end{{},      f.module,           "Sequence continuation",      {},
                 f.scene, ir::SlotRole::Exit, ir::SlotBoundary::SequenceEnd};
    auto added = f.edit(semantic(ir::PutMetadata{end}));
    end.id = SctPortId{added.metadata.front()};
    auto returnConnection = f.connection(end.id, f.entry);
    auto before = f.checkpoint().program->find(f.scene)->order->size();
    f.edit(semantic(ir::ConnectSlots{returnConnection, ir::RouteOutcome::ReturnToCaller}));
    EXPECT_EQ(f.checkpoint().program->find(f.scene)->order->size(), before + 1);
}
} // namespace
