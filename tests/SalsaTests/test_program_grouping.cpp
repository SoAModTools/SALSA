#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/NativeImport.h"
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
        {{{2, sct::SctEncodedWordValue{2}}, {3, sct::SctInstructionReference{sct::SctInstructionId{6}}}}}};
    d.sections.push_back({sct::SctSectionId{1}, "main",
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
    return d;
}
struct Fixture {
    std::shared_ptr<ir::ProgramService> service = std::make_shared<ir::ProgramService>();
    ir::Revision revision;
    ir::SectionId main, incomplete;
    std::map<std::uint64_t, SctActionId> ids;
    Fixture() {
        ir::ProgramBuilder builder({Project});
        SctAuthoringBaseline baseline{
            {},
            {Sha256Digest({})},
            {AssetLocator::fromRelativePath("test.sct").value(), 0, {Sha256Digest({})}},
            {"20d9e32e-11c6-4088-a390-3b99624d219f"}};
        auto imported = builder.appendImported(document(), baseline, {});
        if (!imported)
            throw std::runtime_error(messages(imported.diagnostics()));
        auto program = builder.finish();
        if (!program)
            throw std::runtime_error(messages(program.diagnostics()));
        main = program.value()->scripts().front().sections[0];
        incomplete = program.value()->scripts().front().sections[2];
        for (const auto &section : program.value()->sections())
            if (section.body.valid())
                for (auto id : *program.value()->find(section.body)->order)
                    ids.emplace(program.value()->find(id)->sourceInstruction, id);
        auto adopted = service->adopt({}, std::move(program).takeValue()).get();
        if (!adopted)
            throw std::runtime_error(messages(adopted.diagnostics()));
        revision = adopted.value().current;
    }
    std::vector<std::string> tree(ir::EditorSubject parent,
                                  std::optional<editor::FlowOccurrence> occurrence = {}, unsigned depth = 0) {
        if (depth > 20)
            throw std::runtime_error("Unexpected grouping cycle");
        auto page = service->outline(revision, {parent, 0, 128, true, occurrence}).get();
        if (!page)
            throw std::runtime_error(messages(page.diagnostics()));
        std::vector<std::string> labels;
        for (const auto &row : page.value().rows) {
            labels.push_back(row.label);
            if (row.expandable) {
                auto child = tree(row.subject, row.occurrence, depth + 1);
                labels.insert(labels.end(), child.begin(), child.end());
            }
        }
        return labels;
    }
};
TEST(ProgramGroupingTest, SpiceNestedSwitchIfAndLoopSurviveCheckpointWithoutChangingInstructions) {
    Fixture f;
    auto checkpoint = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(checkpoint);
    auto bytes = ir::ProgramPersistence::encode(*checkpoint.value().program);
    ASSERT_TRUE(bytes);
    auto labels = f.tree(f.main);
    for (const auto *label : {"Switch", "Case 1", "Case 2", "If", "While"})
        EXPECT_NE(std::ranges::find(labels, label), labels.end()) << label;
    for (const auto &label : labels)
        EXPECT_EQ(label.find("recognized"), std::string::npos);
    editor::ProgramBackend backend(f.service);
    auto inspected = backend.inspect(f.revision, f.ids.at(2), {});
    ASSERT_TRUE(inspected);
    EXPECT_TRUE(inspected.value().evidence.empty());
    auto path = backend.locate(f.revision, f.ids.at(3), editor::ViewMode::Semantic);
    ASSERT_TRUE(path);
    EXPECT_GE(path.value().occurrences.size(), 5u);
    EXPECT_EQ(path.value().subjects.back(), editor::Subject{f.ids.at(3)});
    auto after = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(after);
    EXPECT_EQ(ir::ProgramPersistence::encode(*after.value().program).value(), bytes.value());
    auto decoded = ir::ProgramPersistence::decode(bytes.value());
    ASSERT_TRUE(decoded);
    f.service = std::make_shared<ir::ProgramService>();
    auto installed = f.service->adopt({}, std::move(decoded.value().program)).get();
    ASSERT_TRUE(installed);
    f.revision = installed.value().current;
    EXPECT_EQ(f.tree(f.main), labels); // Rebuilt from current IR, not imported evidence.
}
TEST(ProgramGroupingTest, ExternalEntryEditsInvalidateTargetGroupingAndIssuesStayLocal) {
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
    EXPECT_NE(f.tree(f.main), original);
    EXPECT_TRUE(
        std::ranges::any_of(tree.value().rows, [](const auto &row) { return !row.groupingIssue.empty(); }));
    auto undone = f.service->undo(f.revision).get();
    ASSERT_TRUE(undone);
    f.revision = undone.value().status.current;
    EXPECT_EQ(f.tree(f.main), original);
    editor::ProgramBackend backend(f.service);
    auto broken = backend.inspect(f.revision, f.ids.at(11), {});
    ASSERT_TRUE(broken);
    EXPECT_NE(broken.value().evidence.find("unresolved"), std::string::npos);
    auto page = backend.outline(f.revision, {f.incomplete, 0, 128, editor::ViewMode::Semantic});
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().rows.size(), 2u);
    EXPECT_FALSE(page.value().rows.front().groupingIssue.empty());
}
} // namespace
