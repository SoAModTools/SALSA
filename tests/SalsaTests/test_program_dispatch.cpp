#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <array>
#include <bit>
#include <gtest/gtest.h>
#include <set>
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
auto expression(const char *s) { return *SctExpressionLanguage::parse(s).expression; }
auto buffer(const char *s) { return ir::ValueBuffer::expression(expression(s)).value(); }
struct Fixture {
    ir::ProgramService service;
    ir::Revision revision;
    SctScriptId script;
    SctActionId root, firstWrite;
    SctSequenceId firstBody, secondBody;
    ir::ArmId a, b;
    Fixture(bool firstMatch = false, bool preparation = false) {
        ir::ProgramBuilder builder({"e44499b5-dcf0-449a-9815-5bdf558eface"});
        script = builder
                     .importedScript(
                         {{},
                          {Sha256Digest{{}}},
                          {AssetLocator::fromRelativePath("field/synthetic.sct").value(), 0, {Sha256Digest{{}}}},
                          {"a44499b5-dcf0-449a-9815-5bdf558eface"},
                          sct::SctKnownTextConvention::ShiftJisByte7F},
                         {})
                     .value();
        auto section = [&](const char *name) {
            return builder.sectionBody(builder.section({{}, script, name}).value());
        };
        auto emit = [&](SctSequenceId seq, unsigned op, const char *value = nullptr) {
            std::vector<ir::ValueId> ps;
            if (value) {
                auto v = builder.expression(expression(value)).value();
                ps.push_back(builder.value(ir::ValueKind::Parameter, 0, 0, std::span(&v, 1)).value());
            }
            return builder.instruction(seq, builder.value(ir::ValueKind::Instruction, op, 0, ps).value()).value();
        };
        emit(section("init"), 12);
        auto loop = section("loop");
        auto p = builder.expression(expression("BitVar[0]")).value();
        auto context = builder.expression(expression("BitVar[3]")).value();
        auto outer = builder.conditional(loop, std::span(&context, 1)).value();
        auto outerCondition = builder.branchConditions(outer).front();
        root = builder.conditional(builder.branchBodies(outer).front(), std::span(&p, 1)).value();
        if (preparation)
            emit(builder.conditionPreparation(builder.branchConditions(root).front()), 17, "30");
        auto body = builder.branchBodies(root).front();
        auto q = builder.expression(expression("BitVar[1]")).value(),
             r = builder.expression(expression("BitVar[2]")).value();
        if (firstMatch) {
            const std::array checks{q, r, ir::ValueId{}};
            auto chain = builder.conditional(body, checks).value();
            auto bodies = builder.branchBodies(chain);
            firstBody = bodies[0];
            secondBody = bodies[1];
            emit(bodies[2], 17, "22");
        } else {
            auto qa = builder.conditional(body, std::span(&q, 1)).value();
            firstBody = builder.branchBodies(qa).front();
            auto ra = builder.conditional(body, std::span(&r, 1)).value();
            secondBody = builder.branchBodies(ra).front();
        }
        firstWrite = emit(firstBody, 18, "0");
        emit(firstBody, 17, "20");
        auto secondWrite = emit(secondBody, 17, "21");
        emit(loop, 12);
        builder.metadata().modules = {{SctModuleId{1001}, script, "A", {firstBody}},
                                      {SctModuleId{1002}, script, "B", {secondBody}}};
        builder.metadata().ports = {
            {SctPortId{2001}, SctModuleId{1001}, "Response", firstWrite, firstBody, ir::SlotRole::Exit,
             ir::SlotBoundary::Fallthrough},
            {SctPortId{2002}, SctModuleId{1002}, "Entry", secondWrite, secondBody, ir::SlotRole::Entry}};
        ir::Connection connection;
        connection.id = SctConnectionId{3001};
        connection.source = SctPortId{2001};
        connection.destination = SctPortId{2002};
        connection.cutscene = ir::RouteDefinition{};
        ir::RoutePath outside;
        outside.requirements.push_back({{outer, {}, {}, 0, outerCondition}, ir::RequirementMatch::True, {}});
        builder.metadata().connections.push_back(connection);
        auto built = builder.finish();
        if (!built)
            throw std::runtime_error(messages(built.diagnostics()));
        outside.requirements.front().site.arm=built.value()->find(outerCondition)->arm;
        connection.cutscene->paths.push_back(outside);
        a = std::get<ir::ArmId>(*built.value()->find(firstBody)->owner);
        b = std::get<ir::ArmId>(*built.value()->find(secondBody)->owner);
        auto adopted = service.adopt({}, std::move(built).takeValue()).get();
        if (!adopted)
            throw std::runtime_error(messages(adopted.diagnostics()));
        revision = adopted.value().current;
        auto described=service.edit({revision,{ir::SemanticOperation{ir::PutMetadata{connection}}}}).get();
        if(!described)throw std::runtime_error(messages(described.diagnostics()));
        revision=described.value().status.current;
    }
    ~Fixture() { service.shutdown().get(); }
    auto checkpoint() { return service.checkpoint(revision).get().value(); }
    auto edit(ir::Primitive op) {
        auto changed = service.edit({revision, {std::move(op)}}).get();
        if (!changed)
            throw std::runtime_error(messages(changed.diagnostics()));
        revision = changed.value().status.current;
        return changed.value();
    }
    auto materialize() {
        auto cp = checkpoint();
        ir::ProgramMaterializer m;
        auto result = m.materialize(cp, revision, script, {"f44499b5-dcf0-449a-9815-5bdf558eface"});
        if (!result)
            throw std::runtime_error(messages(result.diagnostics()));
        return std::move(result).takeValue();
    }
};
// A deliberately tiny test tracer for this fixture's pure bit expressions and
// four opcodes, not a game VM or a runtime-compatibility assertion.
std::set<unsigned> trace(const sct::SctDocument &document, unsigned initial) {
    std::set<unsigned> bits{3};
    for (unsigned i = 0; i < 3; ++i)
        if (initial & (1u << i))
            bits.insert(i);
    std::vector<const sct::SctDocumentInstruction *> code;
    std::map<sct::SctInstructionId, std::size_t> offsets;
    std::size_t pc = 0;
    for (auto &section : document.sections)
        if (auto *body = std::get_if<sct::SctScriptSectionContent>(&section.content)) {
            if (section.nameBytes == "loop")
                pc = code.size();
            for (auto &i : body->instructions) {
                offsets[i.id] = code.size();
                code.push_back(&i);
            }
        }
    auto eval = [&](const sct::SctCanonicalExpression &expression) {
        std::vector<float> stack;
        for (auto &op : std::get<sct::SctTypedScptProgram>(expression.body).operations) {
            if (auto *v = std::get_if<sct::SctScptValueOperation>(&op)) {
                if (v->kind == sct::SctScptValueKind::BitVariable)
                    stack.push_back(bits.contains(v->encodingWord & 0x1fffffffu));
                else if (v->kind == sct::SctScptValueKind::DecimalLiteral)
                    stack.push_back(float(v->encodingWord & 0xffffffu) / 256);
                else if (v->kind == sct::SctScptValueKind::InlineValue)
                    stack.push_back(std::bit_cast<float>(v->encodingWord));
                else
                    throw std::runtime_error("Unexpected test expression value");
            } else {
                auto binary = std::get<sct::SctScptBinaryOperation>(op);
                auto right = stack.back();
                stack.pop_back();
                auto left = stack.back();
                stack.pop_back();
                if (binary.encodingWord == 8)
                    stack.push_back(left && right);
                else if (binary.encodingWord == 9)
                    stack.push_back(left || right);
                else
                    throw std::runtime_error("Unexpected test comparison");
            }
        }
        return stack.at(0);
    };
    for (unsigned steps = 0; steps < 4096; ++steps) {
        auto &i = *code.at(pc++);
        auto param = [&](unsigned n) -> const sct::SctDocumentParameterValue & {
            return std::ranges::find(i.fixedParameters, n, &sct::SctDocumentParameter::schemaIndex)->value;
        };
        if (i.opcode == 12)
            return bits;
        if (i.opcode == 10)
            pc = offsets.at(std::get<sct::SctInstructionReference>(param(0)).target);
        else if (i.opcode == 0) {
            if (!eval(std::get<sct::SctCanonicalExpression>(param(0))))
                pc = offsets.at(std::get<sct::SctInstructionReference>(param(1)).target);
        } else if (i.opcode == 17 || i.opcode == 18) {
            auto bit = static_cast<unsigned>(eval(std::get<sct::SctCanonicalExpression>(param(0))));
            if (i.opcode == 17)
                bits.insert(bit);
            else
                bits.erase(bit);
        } else
            throw std::runtime_error("Unexpected test opcode");
    }
    throw std::runtime_error("Synthetic dispatch did not terminate");
}
TEST(ProgramDispatchTest, IndependentRequirementsPreserveEarlierSharedReads) {
    Fixture f;
    auto before = f.materialize();
    f.edit(ir::SemanticOperation{ir::PromoteDispatch{f.root, {{f.firstBody, SctConnectionId{3001}}}}});
    auto promoted = f.materialize();
    for (unsigned state = 0; state < 8; ++state)
        EXPECT_EQ(trace(*before.document, state), trace(*promoted.document, state));
    auto checkpoint = f.checkpoint();
    auto bCondition = checkpoint.program->find(f.b)->condition;
    auto bValue = checkpoint.program->find(bCondition)->result;
    auto receipt = f.edit(ir::SemanticOperation{ir::SetRouteRequirement{f.a, buffer("BitVar[1]")}});
    EXPECT_EQ(f.checkpoint().program->find(bCondition)->result, bValue);
    EXPECT_EQ(receipt.actions.size(), 1u);
    auto query = f.service.cutscene(f.revision, SctModuleId{1002}).get();
    ASSERT_TRUE(query) << messages(query.diagnostics());
    auto route = std::ranges::find(query.value().summary->incoming, SctConnectionId{3001},
                                   &ir::CutsceneRouteView::connection);
    ASSERT_NE(route, query.value().summary->incoming.end());
    ASSERT_FALSE(route->paths.empty());
    EXPECT_EQ(route->paths.front().requirements.size(), 2u);
    EXPECT_EQ(route->paths.front().requirements.back().site.condition,
              f.checkpoint().program->find(f.a)->condition);
    EXPECT_TRUE(f.service.cutscene(f.revision, SctModuleId{1002}).get().value().reused);
    auto changed = f.materialize();
    for (unsigned state = 0; state < 8; ++state) {
        auto bits = trace(*changed.document, state);
        EXPECT_EQ(bits.contains(20), (state & 2) != 0);
        EXPECT_EQ(bits.contains(21), (state & 1) != 0 && (state & 4) != 0);
    }
    std::set<sct::SctInstructionId> copies;
    for (auto &mapping : changed.realization.locations)
        if (mapping.subject == ir::Subject{f.firstWrite})
            if (auto *entity = std::get_if<sct::SctDocumentEntityId>(&mapping.location))
                if (auto *id = std::get_if<sct::SctInstructionId>(entity))
                    copies.insert(*id);
    EXPECT_GT(copies.size(), 1u);
    auto condition = f.checkpoint().program->find(f.a)->condition;
    f.edit(ir::ReplaceCondition{condition, buffer("BitVar[0] && BitVar[1]")});
    auto semantic = f.materialize();
    for (unsigned state = 0; state < 8; ++state)
        EXPECT_EQ(trace(*before.document, state), trace(*semantic.document, state));
}
TEST(ProgramDispatchTest, FirstMatchPersistenceAndAtomicHistory) {
    Fixture f(true);
    auto before = f.materialize();
    f.edit(ir::SemanticOperation{ir::PromoteDispatch{f.root, {}}});
    auto promoted = f.materialize();
    for (unsigned state = 0; state < 8; ++state)
        EXPECT_EQ(trace(*before.document, state), trace(*promoted.document, state));
    f.edit(ir::SemanticOperation{ir::SetRouteRequirement{f.a, buffer("BitVar[1]")}});
    auto cp = f.checkpoint();
    auto encoded = ir::ProgramPersistence::encode(*cp.program);
    ASSERT_TRUE(encoded) << messages(encoded.diagnostics());
    EXPECT_EQ(encoded.value(), ir::ProgramPersistence::encode(*cp.program).value());
    auto decoded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
    EXPECT_EQ(decoded.value().program->find(f.a)->body, f.firstBody);
    EXPECT_EQ(decoded.value().program->dispatch(f.root)->mode, ir::DispatchMode::FirstMatch);
    ir::Checkpoint reopened{cp.status, decoded.value().program};
    ir::ProgramMaterializer materializer;
    auto output = materializer.exportScript(
        reopened, f.revision, f.script, {"f44499b5-dcf0-449a-9815-5bdf558eface"},
        sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding});
    ASSERT_TRUE(output) << messages(output.diagnostics());
    for (unsigned state = 0; state < 8; ++state) {
        auto bits = trace(*output.value().materialized.document, state);
        EXPECT_EQ(bits.contains(20), (state & 2) != 0);
        EXPECT_EQ(bits.contains(21), (state & 1) != 0 && (state & 4) != 0 && !(state & 2));
    }
    f.revision = f.service.undo(f.revision).get().value().status.current;
    f.revision = f.service.undo(f.revision).get().value().status.current;
    EXPECT_EQ(f.checkpoint().program->find(f.root)->kind, ir::ActionKind::Conditional);
    EXPECT_EQ(f.checkpoint().program->find(f.a)->body, f.firstBody);
    f.revision = f.service.redo(f.revision).get().value().status.current;
    EXPECT_NE(f.checkpoint().program->dispatch(f.root), nullptr);
}
TEST(ProgramDispatchTest, UnsupportedPreparationAndTimingEditsFailWithoutPartialChanges) {
    Fixture prepared(false, true);
    auto before = prepared.checkpoint();
    auto failed =
        prepared.service.edit({prepared.revision, {ir::SemanticOperation{ir::PromoteDispatch{prepared.root, {}}}}})
            .get();
    EXPECT_FALSE(failed);
    EXPECT_EQ(prepared.checkpoint().program->find(prepared.root)->kind, ir::ActionKind::Conditional);
    Fixture f;
    f.edit(ir::SemanticOperation{ir::PromoteDispatch{f.root, {}}});
    auto cp = f.checkpoint();
    auto sample = cp.program->dispatch(f.root)->samples.front().condition;
    EXPECT_FALSE(f.service.edit({f.revision, {ir::ReplaceCondition{sample, buffer("BitVar[3]")}}}).get());
    EXPECT_EQ(ir::ProgramPersistence::encode(*cp.program).value(),
              ir::ProgramPersistence::encode(*f.checkpoint().program).value());
}
} // namespace
