#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/NativeImport.h"
#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <algorithm>
#include <gtest/gtest.h>
namespace {
using namespace salsa::core;
namespace ir = program;
namespace sct = spice::sct;
constexpr auto Project = "8ed67fd8-e429-4b86-a6f3-74e587c81254";
std::string messages(const std::vector<Diagnostic> &ds) {
    std::string s;
    for (const auto &d : ds)
        s += d.message + '\n';
    return s;
}
sct::SctDocumentInstruction branch(std::uint64_t id, std::uint64_t target) {
    sct::SctDocumentInstruction out{sct::SctInstructionId{id}, 0};
    out.fixedParameters = {{0, sct::SctExpressionFactory::encodedDecimalLiteral(0)},
                           {1, sct::SctInstructionReference{sct::SctInstructionId{target}}}};
    return out;
}
sct::SctDocumentInstruction jump(std::uint64_t id, std::uint64_t target) {
    sct::SctDocumentInstruction out{sct::SctInstructionId{id}, 10};
    out.fixedParameters = {{0, sct::SctInstructionReference{sct::SctInstructionId{target}}}};
    return out;
}
sct::SctDocument document() {
    sct::SctDocument d;
    sct::SctDocumentInstruction select{sct::SctInstructionId{1}, 3};
    select.fixedParameters = {{0, sct::SctExpressionFactory::encodedDecimalLiteral(0)}};
    select.repeatedParameterGroups = {
        {{{2, sct::SctEncodedWordValue{1}}, {3, sct::SctInstructionReference{sct::SctInstructionId{2}}}}},
        {{{2, sct::SctEncodedWordValue{2}}, {3, sct::SctInstructionReference{sct::SctInstructionId{6}}}}},
        {{{2, sct::SctEncodedWordValue{UINT32_MAX}},
          {3, sct::SctInstructionReference{sct::SctInstructionId{6}}}}}};
    d.sections.push_back({sct::SctSectionId{1}, "init",
                          sct::SctScriptSectionContent{{select,
                                                        branch(2, 5),
                                                        {sct::SctInstructionId{3}, 125},
                                                        jump(4, 5),
                                                        jump(5, 9),
                                                        branch(6, 9),
                                                        {sct::SctInstructionId{7}, 125},
                                                        jump(8, 6),
                                                        {sct::SctInstructionId{9}, 12}}}});
    d.sections.push_back(
        {sct::SctSectionId{2}, "caller", sct::SctScriptSectionContent{{{sct::SctInstructionId{10}, 12}}}});
    auto broken = branch(11, 12);
    broken.fixedParameters[1].value =
        sct::SctUnresolvedReferenceValue{{sct::SctReferenceTargetStorage::Instruction, {}}, {0x100}};
    d.sections.push_back({sct::SctSectionId{3}, "incomplete",
                          sct::SctScriptSectionContent{{broken, {sct::SctInstructionId{12}, 12}}}});
    d.sections.push_back(
        {sct::SctSectionId{4}, "loop", sct::SctScriptSectionContent{{{sct::SctInstructionId{13}, 12}}}});
    return d;
}
struct Fixture {
    std::shared_ptr<ir::ProgramService> service = std::make_shared<ir::ProgramService>();
    ir::Revision revision;
    ir::SectionId main, incomplete;
    std::map<std::uint64_t, SctActionId> ids;
    Fixture(sct::SctDocument source = document()) {
        ir::ProgramBuilder builder({Project});
        SctAuthoringBaseline baseline{
            {},
            {Sha256Digest({})},
            {AssetLocator::fromRelativePath("test.sct").value(), 0, {Sha256Digest({})}},
            {"20d9e32e-11c6-4088-a390-3b99624d219f"}};
        auto imported = builder.appendImported(source, baseline, {});
        if (!imported)
            throw std::runtime_error(messages(imported.diagnostics()));
        auto program = builder.finish();
        if (!program)
            throw std::runtime_error(messages(program.diagnostics()));
        main = program.value()->scripts().front().sections[0];
        incomplete = program.value()->scripts().front().sections[2];
        std::function<void(SctSequenceId)> collect = [&](auto sequence) {
            for (auto id : *program.value()->find(sequence)->order) {
                ids.emplace(program.value()->find(id)->sourceInstruction, id);
                for (const auto &arm : program.value()->arms(id))
                    collect(arm.body);
                if (auto *loop = program.value()->loop(id))
                    collect(loop->body);
            }
        };
        for (const auto &section : program.value()->sections())
            if (section.body.valid())
                collect(section.body);
        auto adopted = service->adopt({}, std::move(program).takeValue()).get();
        if (!adopted)
            throw std::runtime_error(messages(adopted.diagnostics()));
        revision = adopted.value().current;
    }
    std::vector<std::string> tree(ir::EditorSubject parent, unsigned depth = 0) {
        if (depth > 20)
            throw std::runtime_error("Unexpected grouping cycle");
        auto page = service->outline(revision, {parent, 0, 128, true}).get();
        if (!page)
            throw std::runtime_error(messages(page.diagnostics()));
        std::vector<std::string> labels;
        for (const auto &row : page.value().rows) {
            labels.push_back(row.label);
            if (row.expandable) {
                auto child = tree(row.subject, depth + 1);
                labels.insert(labels.end(), child.begin(), child.end());
            }
        }
        return labels;
    }
};
TEST(ProgramGroupingTest, CrossSectionJumpRemainsExplicitWithDestinationAndNoWarning) {
    auto source = document();
    std::get<sct::SctScriptSectionContent>(source.sections[1].content).instructions = {jump(10, 13)};
    auto &incomplete = std::get<sct::SctScriptSectionContent>(source.sections[2].content).instructions.front();
    incomplete.opcode = 10;
    incomplete.fixedParameters = {{0, incomplete.fixedParameters[1].value}};
    Fixture f(std::move(source));
    for (bool reopened : {false, true}) {
        SCOPED_TRACE(reopened);
        auto checkpoint = f.service->checkpoint(f.revision).get();
        ASSERT_TRUE(checkpoint);
        auto bytes = ir::ProgramPersistence::encode(*checkpoint.value().program);
        ASSERT_TRUE(bytes);
        if (reopened) {
            auto decoded = ir::ProgramPersistence::decode(bytes.value());
            ASSERT_TRUE(decoded);
            auto adopted = f.service->adopt(f.revision, std::move(decoded.value().program)).get();
            ASSERT_TRUE(adopted);
            f.revision = adopted.value().current;
        }
        auto section = checkpoint.value().program->sections()[1].id;
        for (bool semantic : {false, true}) {
            auto rows = f.service->outline(f.revision, {section, 0, 128, semantic}).get();
            ASSERT_TRUE(rows);
            ASSERT_EQ(rows.value().rows.size(), 1u);
            EXPECT_EQ(rows.value().rows.front().subject, ir::EditorSubject{f.ids.at(10)});
            EXPECT_TRUE(rows.value().rows.front().groupingIssue.empty());
        }
        auto broken = f.service->outline(f.revision, {f.incomplete, 0, 128, true}).get();
        ASSERT_TRUE(broken);
        EXPECT_FALSE(broken.value().rows.front().groupingIssue.empty());
        auto after = f.service->checkpoint(f.revision).get();
        ASSERT_TRUE(after);
        EXPECT_EQ(ir::ProgramPersistence::encode(*after.value().program).value(), bytes.value());
    }
}
TEST(ProgramGroupingTest, ImportedGroupsArePersistentConstructsAndShareBothViews) {
    Fixture f;
    auto checkpoint = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(checkpoint);
    auto bytes = ir::ProgramPersistence::encode(*checkpoint.value().program);
    ASSERT_TRUE(bytes);
    const auto &program = *checkpoint.value().program;
    EXPECT_EQ(program.find(f.ids.at(1))->kind, ir::ActionKind::Switch)
        << f.service->assessFlow(f.revision, f.ids.at(1), {}).get().value().issue;
    EXPECT_EQ(program.find(f.ids.at(2))->kind, ir::ActionKind::Conditional);
    EXPECT_EQ(program.find(f.ids.at(6))->kind, ir::ActionKind::Loop);
    auto physical = f.service->outline(f.revision, {f.main, 0, 128, false}).get();
    auto semantic = f.service->outline(f.revision, {f.main, 0, 128, true}).get();
    ASSERT_TRUE(physical);
    ASSERT_TRUE(semantic);
    ASSERT_EQ(physical.value().rows.size(), semantic.value().rows.size());
    for (std::size_t n = 0; n < physical.value().rows.size(); ++n)
        EXPECT_EQ(physical.value().rows[n].subject, semantic.value().rows[n].subject);
    auto labels = f.tree(f.main);
    for (const auto *label : {"Switch", "Case 1", "Case 2 Default", "If", "While"})
        EXPECT_NE(std::ranges::find_if(labels, [&](const auto &text) {
            return text == label || text.starts_with(std::string(label) + " ");
        }), labels.end()) << label;
    for (const auto &label : labels)
        EXPECT_EQ(label.find("recognized"), std::string::npos);
    editor::ProgramBackend backend(f.service);
    auto inspected = backend.inspect(f.revision, f.ids.at(2), {});
    ASSERT_TRUE(inspected);
    EXPECT_TRUE(inspected.value().evidence.empty());
    auto path = backend.locate(f.revision, f.ids.at(3), editor::ViewMode::Semantic);
    ASSERT_TRUE(path);
    EXPECT_GE(path.value().subjects.size(), 5u);
    EXPECT_EQ(path.value().subjects.back(), editor::Subject{f.ids.at(3)});
    auto after = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(after);
    EXPECT_EQ(ir::ProgramPersistence::encode(*after.value().program).value(), bytes.value());
    auto decoded = ir::ProgramPersistence::decode(bytes.value());
    ASSERT_TRUE(decoded);
    const auto& restored=*decoded.value().program;
    EXPECT_FALSE(restored.find(restored.arms(f.ids.at(2)).front().condition)->skipRefresh);
    EXPECT_FALSE(restored.transfer(f.ids.at(4))->skipRefresh);

    f.service = std::make_shared<ir::ProgramService>();
    auto installed = f.service->adopt({}, std::move(decoded.value().program)).get();
    ASSERT_TRUE(installed);
    f.revision = installed.value().current;
    EXPECT_EQ(f.tree(f.main), labels); // The same group identities and empty bodies persist.
}
TEST(ProgramGroupingTest, SwitchDefaultIsOptionalAcrossImportPersistenceAndOutput) {
    for (bool hasDefault : {false, true}) {
        SCOPED_TRACE(hasDefault);
        auto source = document();
        source.sections.erase(source.sections.begin() + 2); // Omit the unrelated unresolved fixture.
        auto &select = std::get<sct::SctScriptSectionContent>(source.sections.front().content).instructions.front();
        if (!hasDefault)
            select.repeatedParameterGroups.pop_back();
        auto &sourceCode = std::get<sct::SctScriptSectionContent>(source.sections.front().content).instructions;
        sourceCode.insert(sourceCode.begin(), sct::SctDocumentInstruction{sct::SctInstructionId{14}, 125});
        ir::ProgramBuilder builder({Project});
        SctAuthoringBaseline baseline{{}, {Sha256Digest({})},
            {AssetLocator::fromRelativePath("switch.sct").value(), 0, {Sha256Digest({})}},
            {"20d9e32e-11c6-4088-a390-3b99624d219f"}};
        ASSERT_TRUE(builder.appendImported(source, baseline, {}));
        auto built = builder.finish();
        ASSERT_TRUE(built) << messages(built.diagnostics());
        auto &program = *built.value();
        auto root = program.find(program.sections().front().body);
        ASSERT_GE(root->order->size(), 2u);
        EXPECT_EQ(program.find(root->order->front())->sourceInstruction, 14u);
        auto id = (*root->order)[1];
        ASSERT_EQ(program.find(id)->kind, ir::ActionKind::Switch);
        EXPECT_EQ(std::ranges::any_of(program.arms(id), &ir::Arm::includesDefault), hasDefault);
        auto encoded = ir::ProgramPersistence::encode(program);
        ASSERT_TRUE(encoded);
        auto decoded = ir::ProgramPersistence::decode(encoded.value());
        ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
        auto service = std::make_shared<ir::ProgramService>();
        auto loaded = service->adopt({}, std::move(decoded.value().program)).get();
        ASSERT_TRUE(loaded);
        auto cp = service->checkpoint(loaded.value().current).get();
        ASSERT_TRUE(cp);
        ir::ProgramMaterializer materializer;
        auto output = materializer.exportScript(cp.value(), loaded.value().current,
            cp.value().program->scripts().front().context.id,
            {"ea3220aa-4c18-4eb2-bb98-0a72dc9c2e8c"},
            sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast, sct::kSctShiftJisByte7FEncoding});
        ASSERT_TRUE(output) << messages(output.diagnostics());
        const auto &code = std::get<sct::SctScriptSectionContent>(
            output.value().materialized.document->sections.front().content).instructions;
        const auto sw = std::ranges::find(code, 3, &sct::SctDocumentInstruction::opcode);
        ASSERT_NE(sw, code.end());
        EXPECT_EQ(code.front().opcode, 125u); // Prefix was not moved into case one.
        ASSERT_EQ(sw->repeatedParameterGroups.size(), hasDefault ? 3u : 2u);
        EXPECT_EQ(std::get<sct::SctEncodedWordValue>(sw->repeatedParameterGroups[0].parameters[0].value).value, 1u);
        EXPECT_EQ(std::get<sct::SctEncodedWordValue>(sw->repeatedParameterGroups[1].parameters[0].value).value, 2u);
        if (hasDefault)
            EXPECT_EQ(std::get<sct::SctInstructionReference>(sw->repeatedParameterGroups[1].parameters[1].value).target,
                      std::get<sct::SctInstructionReference>(sw->repeatedParameterGroups[2].parameters[1].value).target);
        service->shutdown().get();
    }
}
TEST(ProgramGroupingTest, ReturnGuardKeepsPrefixContinuationAndEditableIdentity) {
    for (int mode = 0; mode != 3; ++mode) {
        SCOPED_TRACE(mode); // Ordinary guard, external body entry, scheduled return.
        sct::SctDocument source;
        auto call = jump(3, 9);
        call.opcode = 11;
        sct::SctDocumentInstruction ret{sct::SctInstructionId{5}, 12, true};
        if (mode == 2) ret.scheduledExpression = sct::SctExpressionFactory::encodedDecimalLiteral(1);
        source.sections.push_back({sct::SctSectionId{1}, "init", sct::SctScriptSectionContent{{
            {sct::SctInstructionId{1}, 125}, branch(2, 6), call,
            {sct::SctInstructionId{4}, 125}, ret, {sct::SctInstructionId{6}, 125}}}});
        source.sections.push_back({sct::SctSectionId{2}, "tail",
            sct::SctScriptSectionContent{{{sct::SctInstructionId{7}, 12}}}});
        source.sections.push_back({sct::SctSectionId{3}, "loop",
            sct::SctScriptSectionContent{{{sct::SctInstructionId{8}, 12}}}});
        source.sections.push_back({sct::SctSectionId{4}, "helper",
            sct::SctScriptSectionContent{{{sct::SctInstructionId{9}, 12}}}});
        if (mode == 1) {
            auto external = jump(10, 4); external.opcode = 11;
            source.sections.push_back({sct::SctSectionId{5}, "external",
                sct::SctScriptSectionContent{{external, {sct::SctInstructionId{11}, 12}}}});
        }
        ir::ProgramBuilder builder({Project});
        SctAuthoringBaseline baseline{{}, {Sha256Digest({})},
            {AssetLocator::fromRelativePath("guard.sct").value(), 0, {Sha256Digest({})}},
            {"20d9e32e-11c6-4088-a390-3b99624d219f"}};
        ASSERT_TRUE(builder.appendImported(source, baseline, {}));
        auto built = builder.finish(); ASSERT_TRUE(built) << messages(built.diagnostics());
        const auto &p = *built.value();
        const auto root = p.sections().front().body;
        const auto guard = (*p.find(root)->order)[1];
        EXPECT_EQ(p.find(p.find(root)->order->front())->sourceInstruction, 1u);
        if (mode != 0) { EXPECT_EQ(p.find(guard)->kind, ir::ActionKind::Instruction); continue; }
        ASSERT_EQ(p.find(guard)->kind, ir::ActionKind::Conditional);
        ASSERT_EQ(p.find(root)->order->size(), 3u);
        EXPECT_EQ(p.find(p.find(root)->order->back())->sourceInstruction, 6u);
        ASSERT_EQ(p.arms(guard).size(), 1u);
        const auto arm = p.arms(guard).front();
        const auto body = *p.find(arm.body)->order;
        ASSERT_EQ(body.size(), 3u);
        EXPECT_EQ(p.find(body.back())->sourceInstruction, 5u);
        auto encoded = ir::ProgramPersistence::encode(p); ASSERT_TRUE(encoded);
        auto decoded = ir::ProgramPersistence::decode(encoded.value()); ASSERT_TRUE(decoded);
        auto service = std::make_shared<ir::ProgramService>();
        auto loaded = service->adopt({}, std::move(decoded.value().program)).get(); ASSERT_TRUE(loaded);
        auto revision = loaded.value().current;
        auto change = service->edit({revision, {ir::ReplaceCondition{arm.condition,
            ir::ValueBuffer::expression(sct::SctExpressionFactory::encodedDecimalLiteral(1)).value()}}}).get();
        ASSERT_TRUE(change); revision = change.value().status.current;
        auto undone = service->undo(revision).get(); ASSERT_TRUE(undone);
        auto redone = service->redo(undone.value().status.current).get(); ASSERT_TRUE(redone);
        revision = redone.value().status.current;
        auto cp = service->checkpoint(revision).get(); ASSERT_TRUE(cp);
        EXPECT_EQ(*cp.value().program->find(arm.body)->order, body);
        EXPECT_TRUE(service->assessFlow(revision, guard, {}).get().value().issue.empty());
        ir::ProgramMaterializer materializer;
        auto output = materializer.exportScript(cp.value(), revision,
            cp.value().program->scripts().front().context.id,
            {"ea3220aa-4c18-4eb2-bb98-0a72dc9c2e8c"},
            sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast, sct::kSctShiftJisByte7FEncoding});
        ASSERT_TRUE(output) << messages(output.diagnostics());
        const auto &code = std::get<sct::SctScriptSectionContent>(output.value().materialized.document->sections.front().content).instructions;
        std::vector<std::uint16_t> opcodes;
        for (const auto &i : code) opcodes.push_back(i.opcode);
        EXPECT_EQ(opcodes, (std::vector<std::uint16_t>{125, 0, 11, 125, 12, 125}));
        EXPECT_EQ(std::get<sct::SctInstructionReference>(code[1].fixedParameters[1].value).target, code.back().id);
        EXPECT_TRUE(code[4].skipRefresh);
        EXPECT_EQ(output.value().materialized.document->sections[1].nameBytes, "tail");
        // Reusing the materializer after changing Return must rebuild the arm exit.
        ir::ValueBuffer ordinary;
        ordinary.add(ir::ValueKind::Instruction, 125);
        auto replaced = service->edit({revision, {ir::ReplaceInstruction{body.back(), ordinary}}}).get();
        ASSERT_TRUE(replaced); revision = replaced.value().status.current;
        cp = service->checkpoint(revision).get(); ASSERT_TRUE(cp);
        auto changedOutput = materializer.materialize(cp.value(), revision,
            cp.value().program->scripts().front().context.id, {"ea3220aa-4c18-4eb2-bb98-0a72dc9c2e8c"});
        ASSERT_TRUE(changedOutput);
        const auto &changedCode = std::get<sct::SctScriptSectionContent>(changedOutput.value().document->sections.front().content).instructions;
        ASSERT_EQ(changedCode.size(), 7u);
        EXPECT_EQ(changedCode[5].opcode, 10u);
        auto restored = service->undo(revision).get(); ASSERT_TRUE(restored);
        revision = restored.value().status.current;
        cp = service->checkpoint(revision).get(); ASSERT_TRUE(cp);
        auto restoredOutput = materializer.materialize(cp.value(), revision,
            cp.value().program->scripts().front().context.id, {"ea3220aa-4c18-4eb2-bb98-0a72dc9c2e8c"});
        ASSERT_TRUE(restoredOutput);
        EXPECT_EQ(std::get<sct::SctScriptSectionContent>(restoredOutput.value().document->sections.front().content).instructions.size(), 6u);
        service->shutdown().get();
    }
}
TEST(ProgramGroupingTest, GroupsRemainAuthoritativeAcrossReferenceEditsAndEmptyBodyHistory) {
    Fixture f;
    auto original = f.tree(f.main);
    ir::ValueBuffer value;
    auto target = value.add(ir::ValueKind::ActionReference, f.ids.at(3).value);
    auto parameter = value.add(ir::ValueKind::Parameter, 0, 0, std::span(&target, 1));
    value.add(ir::ValueKind::Instruction, 11, 0, std::span(&parameter, 1));
    auto changed =
        f.service->edit({f.revision, {ir::ReplaceInstruction{f.ids.at(10), std::move(value)}}}).get();
    ASSERT_TRUE(changed) << messages(changed.diagnostics());
    f.revision = changed.value().status.current;
    auto tree = f.service->outline(f.revision, {f.main, 0, 128, true}).get();
    ASSERT_TRUE(tree);
    EXPECT_EQ(f.tree(f.main), original); // A new reference does not replace or erase an authored group.
    auto undone = f.service->undo(f.revision).get();
    ASSERT_TRUE(undone);
    f.revision = undone.value().status.current;
    EXPECT_EQ(f.tree(f.main), original);
    auto captured = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(captured);
    const auto arm = captured.value().program->arms(f.ids.at(2)).front();
    auto cleared = f.service
                       ->edit({f.revision,
                               {ir::SemanticOperation{ir::EraseAction{f.ids.at(3)}},
                                ir::SemanticOperation{ir::EraseAction{f.ids.at(4)}}}})
                       .get();
    ASSERT_TRUE(cleared) << messages(cleared.diagnostics());
    f.revision = cleared.value().status.current;
    auto empty = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty.value().program->find(arm.body)->order->empty());
    EXPECT_EQ(empty.value().program->find(f.ids.at(2))->kind, ir::ActionKind::Conditional);
    auto encoded = ir::ProgramPersistence::encode(*empty.value().program);
    ASSERT_TRUE(encoded);
    auto decoded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
    EXPECT_TRUE(decoded.value().program->find(arm.body)->order->empty());
    auto bodyUndo = f.service->undo(f.revision).get();
    ASSERT_TRUE(bodyUndo);
    f.revision = bodyUndo.value().status.current;
    EXPECT_EQ(f.tree(f.main), original);
    auto bodyRedo = f.service->redo(f.revision).get();
    ASSERT_TRUE(bodyRedo);
    f.revision = bodyRedo.value().status.current;
    auto outputCapture = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(outputCapture);
    ir::ProgramMaterializer materializer;
    auto output = materializer.materialize(outputCapture.value(), f.revision,
                                           outputCapture.value().program->scripts().front().context.id,
                                           SctRealizationId{"ea3220aa-4c18-4eb2-bb98-0a72dc9c2e8c"});
    // This fixture deliberately also contains an unresolved branch in another section;
    // executable publication continues to diagnose it rather than hiding it in a group.
    EXPECT_FALSE(output);
    editor::ProgramBackend backend(f.service);
    auto broken = backend.inspect(f.revision, f.ids.at(11), {});
    ASSERT_TRUE(broken);
    EXPECT_NE(broken.value().evidence.find("unresolved"), std::string::npos);
    auto page = backend.outline(f.revision, {f.incomplete, 0, 128, editor::ViewMode::Semantic});
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().rows.size(), 2u);
    EXPECT_FALSE(page.value().rows.front().groupingIssue.empty());
    ir::ValueBuffer repairedTarget;
    repairedTarget.add(ir::ValueKind::ActionReference, f.ids.at(12).value);
    auto repaired =
        f.service
            ->edit({f.revision, {ir::ReplaceParameter{f.ids.at(11), {1, {}}, std::move(repairedTarget)}}})
            .get();
    ASSERT_TRUE(repaired) << messages(repaired.diagnostics());
    f.revision = repaired.value().status.current;
    auto complete = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(complete);
    auto realized = materializer.materialize(complete.value(), f.revision,
                                             complete.value().program->scripts().front().context.id,
                                             SctRealizationId{"ea3220aa-4c18-4eb2-bb98-0a72dc9c2e8c"});
    ASSERT_TRUE(realized) << messages(realized.diagnostics());
    std::size_t checks = 0;
    for (const auto &section : realized.value().document->sections)
        if (auto *code = std::get_if<sct::SctScriptSectionContent>(&section.content))
            for (const auto &instruction : code->instructions)
                if (instruction.opcode == 0 || instruction.opcode == 3) {
                    ++checks;
                    EXPECT_FALSE(instruction.skipRefresh);
                }
    EXPECT_GE(checks, 3u);
}
} // namespace
