#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Persistence/AtomicFile.h"
#include "SalsaCore/Program/EditorWorkspace.h"
#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Program/ProgramService.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <gtest/gtest.h>

namespace {
using namespace salsa::core;
namespace ir = salsa::core::program;
namespace sct = spice::sct;
constexpr auto Project = "e44499b5-dcf0-449a-9815-5bdf558eface";
std::string messages(const std::vector<Diagnostic> &ds) {
    std::string out;
    for (auto &d : ds)
        out += d.message + "\n";
    return out;
}
SctAuthoringBaseline baseline(bool second = false) {
    return {{},
            {Sha256Digest{{}}},
            {AssetLocator::fromRelativePath(second ? "field/b.sct" : "field/a.sct").value(), 0, {Sha256Digest{{}}}},
            {second ? "b44499b5-dcf0-449a-9815-5bdf558eface" : "a44499b5-dcf0-449a-9815-5bdf558eface"},
            sct::SctKnownTextConvention::ShiftJisByte7F};
}
ir::ValueBuffer word(std::uint32_t value) {
    ir::ValueBuffer b;
    b.add(ir::ValueKind::Word, value);
    return b;
}
ir::ValueBuffer target(SctActionId value) {
    ir::ValueBuffer b;
    b.add(ir::ValueKind::ActionReference, value.value);
    return b;
}
ir::ValueBuffer instruction(std::uint32_t opcode) {
    ir::ValueBuffer b;
    b.add(ir::ValueKind::Instruction, opcode);
    return b;
}
struct Fixture {
    ir::ProgramService service;
    ir::Revision revision;
    SctScriptId script;
    ir::SectionId section;
    SctSequenceId sequence;
    SctActionId writer, other, branch, loop;
    ir::ConditionId condition;
    SctTextResourceId text;
    Fixture() {
        ir::ProgramBuilder b({Project});
        script = b.importedScript(baseline(), {}).value();
        section = b.section({{}, script, "scene"}).value();
        sequence = b.sectionBody(section);
        auto w = b.value(ir::ValueKind::Word, 121).value();
        auto p = b.value(ir::ValueKind::Parameter, 0, 0, std::span(&w, 1)).value();
        auto set = b.value(ir::ValueKind::Instruction, 17, 0, std::span(&p, 1)).value();
        writer = b.instruction(sequence, set).value();
        other = b.instruction(sequence, b.value(ir::ValueKind::Instruction, 12).value()).value();
        auto ref = b.value(ir::ValueKind::ActionReference, writer.value).value();
        auto rp = b.value(ir::ValueKind::Parameter, 2, 0, std::span(&ref, 1)).value();
        auto group = b.value(ir::ValueKind::RepeatedGroup, 0, 0, std::span(&rp, 1)).value();
        std::array groups{group, group};
        branch = b.instruction(sequence, b.value(ir::ValueKind::Instruction, 3, 0, groups).value()).value();
        auto expression = b.expression(*sct::SctExpressionFactory::bitVariable(121).expression).value();
        loop = b.loop(sequence, ir::LoopKind::While, expression).value();
        condition = b.loop(loop)->condition;
        b.instruction(b.loop(loop)->body, set);
        b.metadata().annotations.push_back({condition, "Story gate"});
        text = b.textEntity({{},
                             script,
                             b.text(ir::ValueKind::PlainText, "Original").value(),
                             sct::SctTextKind::PlainString,
                             true})
                   .value();
        auto built = b.finish();
        if (!built)
            throw std::runtime_error(messages(built.diagnostics()));
        auto opened = service.adopt({}, std::move(built).takeValue()).get();
        if (!opened)
            throw std::runtime_error(messages(opened.diagnostics()));
        revision = opened.value().current;
    }
    ~Fixture() { service.shutdown().get(); }
};
TEST(EditorBackendTest, SemanticStringsUseExistingGroupMarkersAndStableNavigation) {
    ir::ProgramBuilder builder({Project});
    auto script = builder.importedScript(baseline(), {}).value();
    auto indexed = [&](const char *name) {
        auto text = builder.textEntity({{}, script, builder.text(ir::ValueKind::PlainText, name).value(),
                                       sct::SctTextKind::PlainString, false}).value();
        auto preamble = builder.value(ir::ValueKind::Words).value();
        auto content = builder.value(ir::ValueKind::IndexedSection, text.value, 0, std::span(&preamble, 1)).value();
        auto section = builder.section({{}, script, name, {}, content}).value();
        return std::pair(section, text);
    };
    auto marker = [&](const char *name) {
        return builder.section({{}, script, name, {}, builder.value(ir::ValueKind::StringGroup).value()}).value();
    };
    auto unmarked = indexed("unmarked");
    auto groupA = marker("strings_A");
    auto a = indexed("a"), b = indexed("b");
    auto empty = marker("empty");
    auto groupB = marker("strings_B");
    auto c = indexed("c");
    auto code = builder.section({{}, script, "code"}).value();
    auto trailing = indexed("after_code");
    auto footer = builder.textEntity({{},script,builder.text(ir::ValueKind::PlainText,"debug").value(),sct::SctTextKind::PlainString,true}).value();
    auto order = builder.scriptSections(script);
    auto program = builder.finish();ASSERT_TRUE(program)<<messages(program.diagnostics());
    auto service = std::make_shared<ir::ProgramService>();
    auto opened = service->adopt({},std::move(program).takeValue()).get();ASSERT_TRUE(opened);
    auto revision = opened.value().current;
    editor::ProgramBackend backend(service);
    auto root = backend.outline(revision,{script,0,128,editor::ViewMode::Semantic},{});ASSERT_TRUE(root);
    ASSERT_EQ(root.value().rows.size(),6u);
    EXPECT_EQ(root.value().rows[0].subject,editor::Subject(unmarked.first));
    EXPECT_EQ(root.value().rows[1].subject,editor::Subject(groupA));
    EXPECT_TRUE(root.value().rows[1].expandable);EXPECT_TRUE(root.value().rows[2].expandable);
    EXPECT_EQ(root.value().rows.back().subject,editor::Subject(trailing.first));
    auto members = backend.outline(revision,{groupA,1,1,editor::ViewMode::Semantic},{});ASSERT_TRUE(members);
    EXPECT_EQ(members.value().total,2u);ASSERT_EQ(members.value().rows.size(),1u);
    EXPECT_EQ(members.value().rows.front().subject,editor::Subject(b.first));
    EXPECT_EQ(backend.outline(revision,{empty,0,128,editor::ViewMode::Semantic},{}).value().total,0u);
    auto flat = backend.outline(revision,{script,0,128,editor::ViewMode::Instructions},{});ASSERT_TRUE(flat);
    EXPECT_EQ(flat.value().total,order.size());
    auto path = backend.locate(revision,b.second,editor::ViewMode::Semantic);ASSERT_TRUE(path);
    EXPECT_EQ(path.value().subjects,(std::vector<editor::Subject>{script,groupA,b.first}));
    EXPECT_EQ(backend.locate(revision,b.second,editor::ViewMode::Instructions).value().subjects,
              (std::vector<editor::Subject>{script,b.first}));
    EXPECT_EQ(backend.inspect(revision,b.first,{}).value().text,b.second);
    EXPECT_EQ(backend.locate(revision,trailing.second,editor::ViewMode::Semantic).value().subjects,
              (std::vector<editor::Subject>{script,trailing.first}));
    std::erase(order,b.first);order.insert(std::ranges::find(order,c.first),b.first);
    auto moved = service->edit({revision,{ir::ReorderSections{script,order}}}).get();ASSERT_TRUE(moved);
    EXPECT_EQ(backend.locate(moved.value().status.current,b.second,editor::ViewMode::Semantic).value().subjects,
              (std::vector<editor::Subject>{script,groupB,b.first}));
    auto undone=service->undo(moved.value().status.current).get();ASSERT_TRUE(undone);
    EXPECT_EQ(backend.locate(undone.value().status.current,b.second,editor::ViewMode::Semantic).value().subjects,
              (std::vector<editor::Subject>{script,groupA,b.first}));
}
TEST(EditorBackendTest, ReducedMessageGroupsKeepVariantsAndInspectTheSelectedLanguage) {
    ir::ProgramBuilder b({Project});
    auto script = b.importedScript(baseline(), {}).value();
    auto english = b.allocate<SctLanguageGroupId>().value();
    auto french = b.allocate<SctLanguageGroupId>().value();
    auto missing = b.allocate<SctLanguageGroupId>().value();
    b.metadata().languages = {{english, "English", ""}, {french, "French", ""}, {missing, "Custom", ""}};
    auto marker = [&](const char *name) {
        return b.section({{}, script, name, {}, b.value(ir::ValueKind::StringGroup).value()}).value();
    };
    auto indexed = [&](const char *name, const char *body, SctLanguageGroupId language) {
        ir::Text text{{}, script, b.text(ir::ValueKind::PlainText, body).value(), sct::SctTextKind::PlainString};
        text.sourceName = name;
        text.languages = {language};
        auto id = b.textEntity(text).value();
        auto preamble = b.value(ir::ValueKind::Words).value();
        auto content = b.value(ir::ValueKind::IndexedSection, id.value, 0, std::span(&preamble, 1)).value();
        auto section = b.section({{}, script, name, {}, content}).value();
        return std::pair(section, id);
    };
    auto baseGroup = marker("default");
    auto base = indexed("greeting", "Hello", english);
    auto translatedGroup = marker("French");
    auto translated = indexed("greetingF", "Bonjour", french);
    auto partialGroup = marker("partial");
    auto otherTranslation = indexed("greetingF2", "Salut", french);
    auto unique = indexed("unique", "Unique", french);
    auto empty = marker("empty");
    auto logical = b.allocate<SctLogicalTextId>().value();
    b.metadata().logicalTexts.push_back({logical, script, "greeting", {base.second, translated.second}});
    // A partial group keeps its unique members even when another member is reduced.
    auto secondBase = indexed("second", "Second", english);
    b.metadata().logicalTexts.push_back({b.allocate<SctLogicalTextId>().value(), script, "second",
                                        {secondBase.second, otherTranslation.second}});
    auto program = b.finish(); ASSERT_TRUE(program) << messages(program.diagnostics());
    auto service = std::make_shared<ir::ProgramService>();
    auto adopted = service->adopt({}, std::move(program).takeValue()).get(); ASSERT_TRUE(adopted);
    auto r = adopted.value().current;
    editor::ProgramBackend backend(service);
    auto root = backend.outline(r, {script, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(root);
    std::vector<editor::Subject> subjects;
    for (const auto &row : root.value().rows) subjects.push_back(row.subject);
    EXPECT_EQ(subjects, (std::vector<editor::Subject>{baseGroup, partialGroup, empty}));
    auto page = backend.outline(r, {baseGroup, 0, 1, editor::ViewMode::Semantic, french}); ASSERT_TRUE(page);
    ASSERT_EQ(page.value().rows.size(), 1u);
    EXPECT_EQ(page.value().rows[0].subject, editor::Subject(base.first));
    EXPECT_EQ(page.value().rows[0].previewText, translated.second);
    auto partial = backend.outline(r, {partialGroup, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(partial);
    ASSERT_EQ(partial.value().rows.size(), 1u);
    EXPECT_EQ(partial.value().rows[0].subject, editor::Subject(unique.first));
    auto inspection = backend.inspect(r, base.first, {}, french); ASSERT_TRUE(inspection);
    ASSERT_TRUE(inspection.value().message);
    EXPECT_EQ(inspection.value().text, translated.second);
    EXPECT_EQ(inspection.value().message->content, "Bonjour");
    EXPECT_EQ(inspection.value().message->languages, (std::vector<std::string>{"French"}));
    auto explicitVariant = backend.inspect(r, translated.second, {}, english); ASSERT_TRUE(explicitVariant);
    ASSERT_TRUE(explicitVariant.value().message);
    EXPECT_EQ(explicitVariant.value().text, translated.second);
    EXPECT_EQ(explicitVariant.value().message->content, "Bonjour");
    auto absent = backend.inspect(r, base.first, {}, missing); ASSERT_TRUE(absent);
    EXPECT_FALSE(absent.value().text);
    ASSERT_TRUE(absent.value().message);
    auto path = backend.locate(r, translated.second, editor::ViewMode::Semantic); ASSERT_TRUE(path);
    EXPECT_EQ(path.value().subjects, (std::vector<editor::Subject>{script, baseGroup, base.first, translated.second}));
    auto flat = backend.outline(r, {script, 0, 128, editor::ViewMode::Instructions}); ASSERT_TRUE(flat);
    EXPECT_NE(std::ranges::find(flat.value().rows, editor::Subject(translatedGroup), &editor::OutlineRow::subject), flat.value().rows.end());
    auto draft = backend.text(r, translated.second); ASSERT_TRUE(draft);
    draft.value().value = sct::SctPlainText{"Changed"};
    auto changed = backend.execute(r, editor::WriteText{draft.value()}); ASSERT_TRUE(changed);
    auto reread = backend.inspect(changed.value().status.current, base.first, {}, french); ASSERT_TRUE(reread);
    EXPECT_EQ(reread.value().message->content, "Changed");
    auto undo = backend.history(changed.value().status.current, false); ASSERT_TRUE(undo);
    EXPECT_EQ(backend.inspect(undo.value().status.current, base.first, {}, french).value().message->content, "Bonjour");
}
TEST(ProgramServiceTest, EditorPagesAndSelectedValuesRemainScoped) {
    Fixture f;
    auto page = f.service.outline(f.revision, {f.section, 1, 2}).get();
    ASSERT_TRUE(page) << messages(page.diagnostics());
    EXPECT_EQ(page.value().total, 4u);
    ASSERT_EQ(page.value().rows.size(), 2u);
    EXPECT_EQ(std::get<SctActionId>(page.value().rows[0].subject), f.other);
    EXPECT_EQ(std::get<SctActionId>(page.value().rows[1].subject), f.branch);
    auto empty = f.service.outline(f.revision, {f.section, 500, 128}).get();
    ASSERT_TRUE(empty); EXPECT_TRUE(empty.value().rows.empty()); EXPECT_EQ(empty.value().total, 4u);
    ir::ReadSelection selection; selection.actions = {f.loop}; selection.descend = false; selection.referencedText = false;
    auto scope = f.service.read(f.revision, selection).get(); ASSERT_TRUE(scope);
    EXPECT_EQ(scope.value()->actions.size(), 1u); EXPECT_TRUE(scope.value()->texts.empty());
    ASSERT_EQ(scope.value()->conditions.size(), 1u);
    auto expression = ir::ProgramValues::expression(scope.value()->payload, scope.value()->conditions[0].result);
    ASSERT_TRUE(expression);
    EXPECT_EQ(SctExpressionLanguage::project(expression.value()).text, SctExpressionLanguage::project(*sct::SctExpressionFactory::bitVariable(121).expression).text);
    auto path = f.service.locate(f.revision, f.condition).get(); ASSERT_TRUE(path);
    EXPECT_EQ(path.value().script, f.script);
    EXPECT_EQ(path.value().subjects, (std::vector<ir::EditorSubject>{f.script,f.section,f.loop,f.condition}));
    selection = {}; selection.texts = {f.text}; selection.descend = false;
    auto text = f.service.read(f.revision, selection).get(); ASSERT_TRUE(text);
    EXPECT_TRUE(text.value()->actions.empty()); ASSERT_EQ(text.value()->texts.size(), 1u);
    auto value = ir::ProgramValues::text(text.value()->payload,text.value()->texts.front().value); ASSERT_TRUE(value);
    EXPECT_EQ(std::get<sct::SctPlainText>(value.value()).utf8,"Original");
    auto encoded=ir::ProgramValues::text(value.value()); ASSERT_TRUE(encoded);
    auto changed=f.service.edit({f.revision,{ir::ReplaceText{f.text,encoded.value()}}}).get(); ASSERT_TRUE(changed);
    EXPECT_FALSE(f.service.outline(f.revision,{f.section,0,128}).get());
    f.revision=changed.value().status.current;
    auto added=f.service.edit({f.revision,{ir::CreateText{f.script,"new_text",true,sct::SctTextKind::PlainString,encoded.value()}}}).get();ASSERT_TRUE(added);f.revision=added.value().status.current;
    ASSERT_EQ(added.value().texts.size(),1u);const auto createdText=added.value().texts.front();
    auto removed=f.service.edit({f.revision,{ir::RemoveText{createdText}}}).get();ASSERT_TRUE(removed);f.revision=removed.value().status.current;
    auto restored=f.service.undo(f.revision).get();ASSERT_TRUE(restored);f.revision=restored.value().status.current;
    ir::ReadSelection createdSelection;createdSelection.texts={createdText};createdSelection.descend=false;
    ASSERT_TRUE(f.service.read(f.revision,createdSelection).get());
    auto cp=f.service.checkpoint(f.revision).get();ASSERT_TRUE(cp);
    auto binary=ir::ProgramPersistence::encode(*cp.value().program);ASSERT_TRUE(binary);
    auto reopened=ir::ProgramPersistence::decode(binary.value());ASSERT_TRUE(reopened);EXPECT_NE(reopened.value().program->find(createdText),nullptr);

}
TEST(ProgramServiceTest, LocalBatchHistoryAndReaderLifetime) {
    Fixture f;
    auto old = f.service.read(f.revision, {{}, {}, {f.writer, f.loop}}).get();
    ASSERT_TRUE(old);
    ASSERT_EQ(old.value()->actions.size(), 3);
    ir::ValueBuffer text;
    text.text(ir::ValueKind::PlainText, "Edited");
    auto expression = ir::ValueBuffer::expression(*sct::SctExpressionFactory::bitVariable(122).expression).value();
    auto prepared = f.service
                        .prepare({f.revision,
                                  {ir::ReplaceParameter{f.writer, {0, {}}, word(122)},
                                   ir::ReplaceCondition{f.condition, expression}, ir::ReplaceText{f.text, text},
                                   ir::EditAnnotation{0, "New gate", {}}}})
                        .get();
    ASSERT_TRUE(prepared) << messages(prepared.diagnostics());
    EXPECT_EQ(prepared.value().preview().examinedActions, 2);
    EXPECT_EQ(prepared.value().preview().copiedOrderItems, 0);
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 121}).get().value().uses.size(), 3);
    auto edited = f.service.install(prepared.value()).get();
    ASSERT_TRUE(edited);
    f.revision = edited.value().status.current;
    EXPECT_TRUE(edited.value().status.dirty);
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 121}).get().value().uses.size(), 1);
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 122}).get().value().uses.size(), 2);
    auto checkpoint = f.service.checkpoint(f.revision).get();
    ASSERT_TRUE(checkpoint);
    auto snapshot = checkpoint.value().program;
    ASSERT_TRUE(f.service.markSaved(f.revision, checkpoint.value().status.historyState).get());
    ASSERT_TRUE(f.service.maintain(f.revision).get()); // Retained inverse payloads must survive collection.
    auto undo = f.service.undo(f.revision).get();
    ASSERT_TRUE(undo);
    f.revision = undo.value().status.current;
    EXPECT_TRUE(undo.value().status.dirty);
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 121}).get().value().uses.size(), 3);
    auto redo = f.service.redo(f.revision).get();
    ASSERT_TRUE(redo);
    f.revision = redo.value().status.current;
    EXPECT_FALSE(redo.value().status.dirty);
    EXPECT_EQ(snapshot->uses({ir::Bank::Bit, 122}).size(), 2);
    EXPECT_EQ(old.value()->payload.values[old.value()->conditions[0].result.value].kind, ir::ValueKind::Expression);
    EXPECT_FALSE(f.service.install(prepared.value()).get());
    for (unsigned i = 0; i < 12; ++i) {
        auto r = f.service.edit({f.revision, {ir::ReplaceParameter{f.writer, {0, {}}, word(200 + i)}}}).get();
        ASSERT_TRUE(r);
        f.revision = r.value().status.current;
    }
    auto gc = f.service.maintain(f.revision, true).get();
    ASSERT_TRUE(gc);
    EXPECT_LT(gc.value().valuesAfter, gc.value().valuesBefore);
    EXPECT_FALSE(f.service.undo(f.revision).get());
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 211}).get().value().uses.size(), 1);
}
TEST(ProgramServiceTest, TargetOrdinalsOrderAndFailuresAreAtomic) {
    {
        Fixture fresh;
        auto removed = fresh.service
                           .edit({fresh.revision,
                                  {ir::EraseInstruction{fresh.writer},
                                   ir::ReplaceParameter{fresh.branch, {2, 0}, target(fresh.other)},
                                   ir::ReplaceParameter{fresh.branch, {2, 1}, target(fresh.other)}}})
                           .get();
        ASSERT_TRUE(removed);
        fresh.revision = removed.value().status.current;
        ASSERT_TRUE(fresh.service.maintain(fresh.revision).get());
        auto restored = fresh.service.undo(fresh.revision).get();
        ASSERT_TRUE(restored);
        EXPECT_EQ(fresh.service.incoming(restored.value().status.current, fresh.writer).get().value().uses.size(), 2);
    }

    Fixture f;
    auto pending = f.service.prepare({f.revision, {ir::ReplaceParameter{f.writer, {0, {}}, word(124)}}}).get();
    ASSERT_TRUE(pending);
    auto moved = f.service
                     .edit({f.revision,
                            {ir::ReplaceParameter{f.branch, {2, 1}, target(f.other)},
                             ir::MoveInstruction{f.other, f.sequence, 0}, ir::AddSection{f.script, "extra", 1}}})
                     .get();
    ASSERT_TRUE(moved) << messages(moved.diagnostics());
    f.revision = moved.value().status.current;
    EXPECT_FALSE(f.service.install(pending.value()).get());
    auto refs = f.service.incoming(f.revision, f.writer).get();
    ASSERT_TRUE(refs);
    ASSERT_EQ(refs.value().uses.size(), 1);
    EXPECT_EQ(refs.value().uses[0].site.parameter->repeatedGroupOrdinal, 0);
    auto checkpoint = f.service.checkpoint(f.revision).get().value().program;
    EXPECT_EQ(checkpoint->nextInSequence(f.other), f.writer);
    EXPECT_FALSE(f.service.edit({f.revision, {ir::EraseInstruction{f.writer}}}).get());
    auto invalid = f.service
                       .edit({f.revision,
                              {ir::ReplaceParameter{f.writer, {0, {}}, word(999)},
                               ir::ReplaceParameter{f.branch, {2, 0}, target(SctActionId{999999})}}})
                       .get();
    EXPECT_FALSE(invalid);
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 121}).get().value().uses.size(), 3);
    EXPECT_TRUE(f.service.uses(f.revision, {ir::Bank::Bit, 999}).get().value().uses.empty());
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_FALSE(f.service.edit({f.revision, {ir::EraseInstruction{f.other}}}, cancelled.get_token()).get());
    auto erased = f.service
                      .edit({f.revision,
                             {ir::ReplaceParameter{f.branch, {2, 0}, target(f.other)}, ir::EraseInstruction{f.writer}}})
                      .get();
    ASSERT_TRUE(erased) << messages(erased.diagnostics());
    f.revision = erased.value().status.current;
    ASSERT_TRUE(f.service.maintain(f.revision).get());
    auto undo = f.service.undo(f.revision).get();
    ASSERT_TRUE(undo);
    f.revision = undo.value().status.current;
    EXPECT_EQ(f.service.incoming(f.revision, f.writer).get().value().uses.size(), 1);
    auto empty = checkpoint->find(f.script)->sections[1];
    auto renamed =
        f.service.edit({f.revision, {ir::RenameSection{empty, "renamed"}, ir::EraseEmptySection{empty}}}).get();
    ASSERT_TRUE(renamed);
    f.revision = renamed.value().status.current;
    auto before = f.service.status().get().value();
    EXPECT_EQ(before.current, f.revision);
    EXPECT_FALSE(f.service.markSaved(f.revision, 99999).get());
}
std::vector<std::uint8_t> nativeBytes() {
    sct::SctDocument d;
    for (auto name : {"init", "loop"})
        d.sections.push_back(
            {d.allocateSectionId(), name, sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
    auto header=d.allocateInstructionId(), body=d.allocateInstructionId(), exit=d.allocateInstructionId(), done=d.allocateInstructionId();
    sct::SctDocumentInstruction check{header,0};
    check.fixedParameters={{0,sct::SctExpressionFactory::encodedDecimalLiteral(0)},{1,sct::SctInstructionReference{done}}};
    sct::SctDocumentInstruction jump{exit,10};jump.fixedParameters={{0,sct::SctInstructionReference{done}}};
    d.sections.push_back({d.allocateSectionId(),"group",sct::SctScriptSectionContent{{check,{body,125},jump,{done,12}}}});
    sct::SctDocumentExportOptions options{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding};
    options.byteOrder = sct::SctDocumentOutputByteOrder::BigEndian;
    options.header = {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}};
    auto result = sct::SctDocumentExporter::exportDocument(d, options);
    if (!result.success)
        throw std::runtime_error("Synthetic native export failed");
    return result.bytes;
}
ir::NativeLoadRequest nativeRequest(ir::Revision expected, std::size_t workers) {
    ir::NativeLoadRequest r;
    r.expected = expected;
    r.project = {Project};
    r.workers = workers;
    r.workingByteBudget = 2048;
    auto bytes = nativeBytes();
    for (bool second : {false, true}) {
        auto b = baseline(second);
        b.source.byteSize = bytes.size();
        b.source.revision.digest = sha256(std::as_bytes(std::span(bytes))).value();
        ir::NativeLoadItem item{b};
        item.context.name = second ? "b" : "a";
        item.workingBytes = 1024;
        item.options.declaredSourcePlatform = sct::SctPlatform::GameCube;
        item.read = [bytes](std::stop_token) { return Result<std::vector<std::uint8_t>>::success(bytes); };
        r.items.push_back(std::move(item));
    }
    return r;
}
TEST(ProgramServiceTest, BoundedLoadingIsResponsiveAndManifestDeterministic) {
    ir::ProgramService parallel, sequential;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto r = nativeRequest({}, 2);
    auto reader = r.items[0].read;
    r.items[0].read = [&entered, gate, reader](std::stop_token stop) {
        entered.set_value();
        gate.wait();
        return reader(stop);
    };
    std::atomic<unsigned> progress = 0;
    r.progress = [&](auto &p) {
        EXPECT_EQ(p.total, 2);
        ++progress;
    };
    auto load = parallel.load(std::move(r));
    entered.get_future().wait();
    auto status = parallel.status();
    EXPECT_EQ(status.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    release.set_value();
    ASSERT_TRUE(status.get());
    auto loaded = load.get();
    ASSERT_TRUE(loaded) << messages(loaded.diagnostics());
    EXPECT_GT(progress.load(), 0u);
    auto one = sequential.load(nativeRequest({}, 1)).get();
    ASSERT_TRUE(one) << messages(one.diagnostics());
    auto a = parallel.checkpoint(loaded.value().current).get().value().program;
    auto b = sequential.checkpoint(one.value().current).get().value().program;
    EXPECT_EQ(a->highWater(), b->highWater());
    ASSERT_EQ(a->scripts().size(), 2);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(a->scripts()[i].context.id, b->scripts()[i].context.id);
        EXPECT_EQ(a->scripts()[i].sections, b->scripts()[i].sections);
        const auto* root=a->find(a->scripts()[i].sections.back());
        auto header=a->find(root->body)->order->front();
        ASSERT_EQ(a->find(header)->kind,ir::ActionKind::Conditional);
        const auto& arm=a->arms(header).front();
        EXPECT_EQ(a->find(arm.body)->script,a->scripts()[i].context.id);
        EXPECT_FALSE(a->find(arm.condition)->skipRefresh);

    }
    auto encoded=ir::ProgramPersistence::encode(*a);ASSERT_TRUE(encoded);
    auto decoded=ir::ProgramPersistence::decode(encoded.value());ASSERT_TRUE(decoded)<<messages(decoded.diagnostics());
    auto init = a->scripts()[0].sections[0];
    EXPECT_FALSE(parallel.edit({loaded.value().current, {ir::RenameSection{init, "changed"}}}).get());
    parallel.shutdown().get();
    sequential.shutdown().get();
}
TEST(ProgramServiceTest, WorkersReplenishBeforeAnEarlierScriptFinishes) {
    auto request = nativeRequest({}, 2);
    request.concurrency = std::make_shared<BackgroundConcurrency>(2);
    auto third = request.items.back();
    third.baseline.importedDocument = {"c44499b5-dcf0-449a-9815-5bdf558eface"};
    third.baseline.source.locator = AssetLocator::fromRelativePath("field/c.sct").value();
    third.context.name = "c";
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto reader = request.items.front().read;
    request.items.front().read = [gate, reader](auto token) { gate.wait(); return reader(token); };
    third.read = [&](auto token) { entered.set_value(); return reader(token); };
    request.items.push_back(std::move(third));
    ir::ProgramService service;
    auto loaded = service.load(std::move(request));
    auto ready = entered.get_future().wait_for(std::chrono::seconds(5));
    release.set_value(); // Always release, including a regression to wave scheduling.
    EXPECT_EQ(ready, std::future_status::ready);
    auto result = loaded.get(); ASSERT_TRUE(result) << messages(result.diagnostics());
    auto catalog = service.catalog(result.value().current).get(); ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog.value().scripts.size(), 3u);
    EXPECT_EQ(catalog.value().scripts[0].name, "a");
    EXPECT_EQ(catalog.value().scripts[2].name, "c");
}
TEST(ProgramServiceTest, DatasetPipelineConsumesCapturedBytesAndReopensCheckpointWithoutParsing) {
    const auto root = std::filesystem::temp_directory_path() / generateSctAuthoringUuid().value();
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); } } cleanup{root};
    auto bytes = nativeBytes();
    auto writeSources = [&] {
        std::filesystem::create_directories(root / "source");
        for (const auto* name : {"a.sct", "b.sct", "c.sct"})
            ASSERT_TRUE(replaceFileAtomically(root / "source" / name, std::as_bytes(std::span(bytes))));
    };
    writeSources();
    const auto original = LocalGameProject::inspect({root / "source"}); ASSERT_TRUE(original);
    ir::EditorOpenRequest request{{root / "source"}, root / "workspace"};
    auto absent = ir::EditorWorkspaceIO::hasSavedCheckpoint(request.workspace);
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent.value());
    std::atomic<unsigned> parsed = 0;
    unsigned localizationPasses = 0;
    auto opened = ir::EditorWorkspaceIO::open(request, {}, [&](const auto& p) {
        if (p.phase == "Linking localized messages") ++localizationPasses;
        if (p.phase == "Parsing SCT") {
            ++parsed;
            // The scan has already captured this file. Nothing downstream may reopen it.
            EXPECT_TRUE(std::filesystem::remove(root / "source" / (p.item + ".sct")));
        }
    });
    ASSERT_TRUE(opened) << messages(opened.diagnostics());
    EXPECT_EQ(parsed, 3u);
    auto session = opened.value();
    EXPECT_TRUE(session->newlyImported);
    EXPECT_FALSE(session->needsInitialSave);
    EXPECT_FALSE(session->status.dirty);
    EXPECT_TRUE(session->checkpointDigest.has_value());
    auto savedInitially = ir::EditorWorkspaceIO::hasSavedCheckpoint(request.workspace);
    ASSERT_TRUE(savedInitially);
    EXPECT_TRUE(savedInitially.value());
    EXPECT_EQ(localizationPasses, 1u);
    ASSERT_EQ(session->catalog.scripts.size(), 3u);
    EXPECT_EQ(session->dataset->snapshot().fingerprint, original.value().snapshot().fingerprint);
    auto cp = session->service->checkpoint(session->status.current).get(); ASSERT_TRUE(cp);
    for (const auto& base : cp.value().program->baselines()) {
        EXPECT_EQ(base.datasetFingerprint, original.value().snapshot().fingerprint);
        auto retained = session->workspace->loadBaseline(base.source.revision); ASSERT_TRUE(retained);
        ASSERT_TRUE(retained.value());
        EXPECT_EQ(*retained.value(), (std::vector<std::byte>(std::as_bytes(std::span(bytes)).begin(), std::as_bytes(std::span(bytes)).end())));
    }
    writeSources();
    // Saved authoring content must reopen without touching the extraction.
    std::filesystem::rename(root / "source", root / "offline-source");
    request.requireSavedProject = true;
    request.dataset.datasetRoot.clear();
    parsed = 0;
    unsigned scans = 0;
    auto reopened = ir::EditorWorkspaceIO::open(request, {}, [&](const auto& p) {
        if (p.phase == "Parsing SCT") ++parsed;
        if (p.phase == "Linking localized messages") ++localizationPasses;
        if (p.phase == "Discovering dataset assets" || p.phase == "Reading dataset fingerprints") ++scans;
    });
    ASSERT_TRUE(reopened) << messages(reopened.diagnostics());
    EXPECT_EQ(parsed, 0u);
    EXPECT_EQ(scans, 0u);
    EXPECT_EQ(localizationPasses, 1u);
    EXPECT_FALSE(reopened.value()->newlyImported);
    for (std::size_t i = 0; i < session->catalog.scripts.size(); ++i)
        EXPECT_EQ(session->catalog.scripts[i].id, reopened.value()->catalog.scripts[i].id);
    EXPECT_TRUE(ir::EditorWorkspaceIO::save(*reopened.value(), reopened.value()->status.current, {}));
    const auto savedPath = session->workspace->componentPath(
        session->workspace->descriptor().components.authoring / L"project.salsaproject");
    std::filesystem::rename(savedPath, savedPath.wstring() + L".held");
    auto removed = ir::EditorWorkspaceIO::hasSavedCheckpoint(request.workspace);
    ASSERT_TRUE(removed);
    EXPECT_FALSE(removed.value());
    EXPECT_FALSE(ir::EditorWorkspaceIO::open(request)); // Never silently import a missing saved project.
    EXPECT_FALSE(std::filesystem::exists(savedPath));
    std::filesystem::rename(savedPath.wstring() + L".held", savedPath);
    std::filesystem::rename(root / "offline-source", root / "source");
    auto reviewed = ir::EditorWorkspaceIO::reviewSources(*reopened.value(), reopened.value()->status.current);
    ASSERT_TRUE(reviewed);
    request.requireSavedProject = false;
    request.dataset.datasetRoot = root / "source";
    std::stop_source cancelled;
    request.workspace = root / "cancelled";
    auto stopped = ir::EditorWorkspaceIO::open(request, cancelled.get_token(), [&](const auto& p) {
        if (p.phase == "Parsing SCT") cancelled.request_stop();
    });
    EXPECT_FALSE(stopped);
    EXPECT_FALSE(std::filesystem::exists(request.workspace));
    for (const auto& entry : std::filesystem::directory_iterator(root))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".salsa-import-"));
}
TEST(ProgramServiceTest, InitialSaveFailureDoesNotOverwriteAnAppearingCheckpoint) {
    const auto root = std::filesystem::temp_directory_path() / generateSctAuthoringUuid().value();
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); } } cleanup{root};
    std::filesystem::create_directories(root / "source");
    const auto bytes = nativeBytes();
    ASSERT_TRUE(replaceFileAtomically(root / "source" / "a.sct", std::as_bytes(std::span(bytes))));
    const auto checkpoint = root / "workspace" / "authoring" / "project.salsaproject";
    const std::string other = "Concurrent saved content";
    bool reachedSave = false;
    auto opened = ir::EditorWorkspaceIO::open({{root / "source"}, root / "workspace"}, {}, [&](const auto& p) {
        if (p.phase == "Saving authoring project") {
            reachedSave = true;
            EXPECT_TRUE(replaceFileAtomically(checkpoint, std::as_bytes(std::span(other))));
        }
    });
    EXPECT_TRUE(reachedSave);
    EXPECT_FALSE(opened);
    std::ifstream file(checkpoint, std::ios::binary);
    const std::string retained{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(retained, other);
}
TEST(ProgramServiceTest, SourceReviewComparesImmutableBaselinesWithoutParsingOrEditing) {
    const auto root=std::filesystem::temp_directory_path()/generateSctAuthoringUuid().value();
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code ec;std::filesystem::remove_all(root,ec);}} cleanup{root};
    std::filesystem::create_directories(root/"source");
    auto bytes=nativeBytes();
    for(auto name:{"a.sct","b.sct","c.sct"}) ASSERT_TRUE(replaceFileAtomically(root/"source"/name,std::as_bytes(std::span(bytes))));
    auto opened=ir::EditorWorkspaceIO::open({{root/"source"},root/"workspace"});
    ASSERT_TRUE(opened)<<messages(opened.diagnostics());auto session=opened.value();
    const auto revision=session->status.current;
    auto before=session->service->checkpoint(revision).get();ASSERT_TRUE(before);
    auto immutable=ir::ProgramPersistence::encode(*before.value().program);ASSERT_TRUE(immutable);
    // Even invalid SCT bytes can be reviewed: this operation does not invoke a parser.
    const std::array changed{std::byte{0xaa}};
    ASSERT_TRUE(replaceFileAtomically(root/"source/a.sct",changed));
    ASSERT_TRUE(std::filesystem::remove(root/"source/b.sct"));
    ASSERT_TRUE(replaceFileAtomically(root/"source/new.sct",changed));
    unsigned parsing=0;std::size_t events=0;
    auto review=ir::EditorWorkspaceIO::reviewSources(*session,revision,{},[&](const auto &p){++events;if(p.phase=="Parsing SCT")++parsing;});
    ASSERT_TRUE(review)<<messages(review.diagnostics());EXPECT_TRUE(review.value().inspected);EXPECT_GT(events,0u);EXPECT_EQ(parsing,0u);
    std::map<std::string,editor::SourceState> states;
    for(const auto &row:review.value().sources){states[row.asset]=row.state;
        if(row.asset=="a.sct"){ASSERT_TRUE(row.script);EXPECT_NE(row.baselineDigest,row.currentDigest);EXPECT_EQ(row.currentBytes,1u);}}
    EXPECT_EQ(states["a.sct"],editor::SourceState::Changed);
    EXPECT_EQ(states["b.sct"],editor::SourceState::Missing);
    EXPECT_EQ(states["c.sct"],editor::SourceState::Unchanged);
    EXPECT_EQ(states["new.sct"],editor::SourceState::Added);
    auto after=session->service->checkpoint(revision).get();ASSERT_TRUE(after);
    EXPECT_EQ(ir::ProgramPersistence::encode(*after.value().program).value(),immutable.value());
    std::stop_source stop;stop.request_stop();
    EXPECT_FALSE(ir::EditorWorkspaceIO::reviewSources(*session,revision,stop.get_token()));
    auto stale=revision;++stale.revision;EXPECT_FALSE(ir::EditorWorkspaceIO::reviewSources(*session,stale));
    // A later scan remains compared to the imported source, not the previous scan.
    auto repeated=ir::EditorWorkspaceIO::reviewSources(*session,revision);ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated.value().sources.front().state,editor::SourceState::Changed);
}
TEST(ProgramServiceTest, DatasetMainConventionResolvesEarlierAmbiguousTextWithoutRereading) {
    auto request=nativeRequest({},2);std::array<unsigned,2> reads{};
    for(std::size_t n=0;n<2;++n) {
        sct::SctDocument document;
        for(auto name:{"init","loop"})document.sections.push_back({document.allocateSectionId(),name,sct::SctScriptSectionContent{{{document.allocateInstructionId(),12}}}});
        sct::SctMessage message;message.body.elements={sct::SctTextChunk{n==0?"\xe3\x81\x82":"\xe3\x80\x80"}};
        document.sections.push_back({document.allocateSectionId(),"line",sct::SctStringSectionContent{{document.allocateStringId(),message,sct::SctTextKind::SctString}}});
        auto encoded=sct::SctDocumentExporter::exportDocument(document,sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,sct::kSctShiftJisByte7FEncoding});ASSERT_TRUE(encoded.success);
        auto &item=request.items[n];item.baseline.textConvention.reset();item.options={};item.baseline.source.byteSize=encoded.bytes.size();
        item.baseline.source.revision.digest=sha256(std::as_bytes(std::span(encoded.bytes))).value();
        item.read=[&,n,bytes=std::move(encoded.bytes)](auto){++reads[n];return Result<std::vector<std::uint8_t>>::success(bytes);};
    }
    ir::ProgramService service;auto loaded=service.load(std::move(request)).get();ASSERT_TRUE(loaded)<<messages(loaded.diagnostics());
    auto checkpoint=service.checkpoint(loaded.value().current).get();ASSERT_TRUE(checkpoint);
    const auto &program=*checkpoint.value().program;
    EXPECT_EQ(reads,(std::array<unsigned,2>{1,1}));
    for(const auto &base:program.baselines())EXPECT_EQ(base.textConvention,sct::SctKnownTextConvention::ShiftJisByte7F);
    ir::ReadSelection select;select.texts={program.texts().front().id};
    auto read=service.read(loaded.value().current,select).get();ASSERT_TRUE(read);
    auto text=ir::ProgramValues::text(read.value()->payload,read.value()->texts.front().value);ASSERT_TRUE(text);
    EXPECT_EQ(std::get<sct::SctTextChunk>(std::get<sct::SctMessage>(text.value()).body.elements.front()).utf8,"\xe3\x81\x82");
}
TEST(ProgramServiceTest, CancelledStaleAndShutdownLoadsCannotReplaceAuthority) {
    Fixture f;
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto r = nativeRequest(f.revision, 1);
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        auto reader = r.items[0].read;
        r.items[0].read = [&entered, gate, reader](std::stop_token stop) {
            entered.set_value();
            gate.wait();
            return reader(stop);
        };
        std::stop_source cancel;
        auto load = f.service.load(std::move(r), cancel.get_token());
        entered.get_future().wait();
        if (mode == 0)
            cancel.request_stop();
        if (mode == 1) {
            auto edited = f.service.edit({f.revision, {ir::ReplaceParameter{f.writer, {0, {}}, word(130)}}}).get();
            ASSERT_TRUE(edited);
            f.revision = edited.value().status.current;
        }
        if (mode == 2) {
            auto shutdown = f.service.shutdown();
            EXPECT_EQ(shutdown.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        }
        release.set_value();
        EXPECT_FALSE(load.get());
        if (mode < 2)
            EXPECT_EQ(f.service.status().get().value().current, f.revision);
    }
}

ir::SemanticOperation semantic(auto operation) {
    return ir::SemanticOperation{std::move(operation)};
}
TEST(ProgramSemanticTest, StructuredArmsBodiesAndTransfersUseLocalHistory) {
    Fixture f;
    auto body = f.service.read(f.revision, {{}, {}, {f.loop}}).get().value()->loops[0].body;
    auto expr = ir::ValueBuffer::expression(*sct::SctExpressionFactory::bitVariable(121).expression).value();
    ir::InsertConstruct create;
    create.sequence = body;
    create.kind = ir::ActionKind::Switch;
    create.condition = expr;
    create.arms = {{{}, {}, {1, 2}, false, ir::CaseExit::Fallthrough}, {{}, {}, {3}, true}};
    auto added = f.service.edit({f.revision, {semantic(create)}}).get();
    ASSERT_TRUE(added) << messages(added.diagnostics());
    f.revision = added.value().status.current;
    auto switched = added.value().actions[0];
    auto scope = f.service.read(f.revision, {{}, {}, {switched}}).get();
    ASSERT_TRUE(scope);
    ASSERT_EQ(scope.value()->arms.size(), 2);
    auto first = scope.value()->arms[0], last = scope.value()->arms[1];
    auto edited =
        f.service
            .edit(
                {f.revision,
                 {semantic(ir::ReplaceArms{
                      switched, {{last.id, {}, {7}, true, ir::CaseExit::Fallthrough}, {first.id, {}, {5, 6}, false}}}),
                  semantic(ir::InsertTransfer{first.body, 0, ir::TransferKind::Continue, f.loop})}})
            .get();
    ASSERT_TRUE(edited) << messages(edited.diagnostics());
    f.revision = edited.value().status.current;
    auto changed = f.service.read(f.revision, {{}, {}, {switched}}).get().value();
    EXPECT_EQ(changed->arms[0].id, last.id);
    EXPECT_EQ(changed->arms[1].body, first.body);
    EXPECT_EQ(changed->transfers[0].target, f.loop);
    EXPECT_FALSE(f.service.edit({f.revision, {semantic(ir::MoveAction{switched, f.sequence, 0})}}).get());
    ASSERT_TRUE(f.service.maintain(f.revision).get());
    auto undo = f.service.undo(f.revision).get();
    ASSERT_TRUE(undo);
    f.revision = undo.value().status.current;
    EXPECT_EQ(f.service.read(f.revision, {{}, {}, {switched}}).get().value()->arms[0].id, first.id);
    auto redo = f.service.redo(f.revision).get();
    ASSERT_TRUE(redo);
    f.revision = redo.value().status.current;
    auto erased = f.service.edit({f.revision, {semantic(ir::EraseAction{switched})}}).get();
    ASSERT_TRUE(erased) << messages(erased.diagnostics());
    f.revision = erased.value().status.current;
    auto restored = f.service.undo(f.revision).get();
    ASSERT_TRUE(restored);
    f.revision = restored.value().status.current;
    EXPECT_EQ(f.service.read(f.revision, {{}, {}, {switched}}).get().value()->arms[1].body, first.body);
}
TEST(ProgramSemanticTest, ScopedSummariesReuseUnrelatedExecutionAndKeepPresentationSeparate) {
    Fixture f;
    auto added = f.service.edit({f.revision, {ir::AddSection{f.script, "independent", 1}}}).get();
    ASSERT_TRUE(added);
    f.revision = added.value().status.current;
    auto checkpoint = f.service.checkpoint(f.revision).get().value().program;
    auto extra = checkpoint->find(checkpoint->find(f.script)->sections[1])->body;
    auto metadata = f.service
                        .edit({f.revision,
                               {semantic(ir::PutMetadata{ir::Module{{}, f.script, "Scene", {f.sequence}}}),
                                semantic(ir::PutMetadata{ir::Module{{}, f.script, "Other", {extra}}})}})
                        .get();
    ASSERT_TRUE(metadata) << messages(metadata.diagnostics());
    f.revision = metadata.value().status.current;
    auto a = metadata.value().modules[0], b = metadata.value().modules[1];
    auto beforeA = f.service.summarize(f.revision, a).get();
    ASSERT_TRUE(beforeA) << messages(beforeA.diagnostics());
    auto beforeB = f.service.summarize(f.revision, b).get();
    ASSERT_TRUE(beforeB);
    EXPECT_TRUE(f.service.uses(f.revision, {ir::Bank::Bit, 145}).get().value().uses.empty());
    auto edit = f.service.edit({f.revision, {ir::ReplaceParameter{f.writer, {0, {}}, word(145)}}}).get();
    ASSERT_TRUE(edit);
    f.revision = edit.value().status.current;
    auto afterA = f.service.summarize(f.revision, a).get();
    ASSERT_TRUE(afterA);
    EXPECT_FALSE(afterA.value().reused);
    EXPECT_TRUE(f.service.summarize(f.revision, b).get().value().reused);
    EXPECT_NE(beforeA.value().summary->build, afterA.value().summary->build);
    EXPECT_EQ(f.service.uses(f.revision, {ir::Bank::Bit, 145}).get().value().uses.size(), 1);
    auto named =
        f.service
            .edit(
                {f.revision,
                 {semantic(ir::PutAnnotation{{a, "First scene", {{SctEvidenceKind::UserAssertion, "My story alias"}}}}),
                  semantic(ir::PutMetadata{SctLanguageGroup{{}, "Custom language", ""}})}})
            .get();
    ASSERT_TRUE(named) << messages(named.diagnostics());
    f.revision = named.value().status.current;
    EXPECT_TRUE(f.service.summarize(f.revision, a).get().value().reused);
    EXPECT_EQ(f.service.explain(f.revision, a).get().value().label, "First scene");
    auto info = f.service.interactions(f.revision, f.script).get();
    ASSERT_TRUE(info);
    auto language = info.value().languages[0].id;
    auto assigned = f.service.edit({f.revision, {semantic(ir::AssignLanguages{f.text, {language}})}}).get();
    ASSERT_TRUE(assigned);
    f.revision = assigned.value().status.current;
    EXPECT_TRUE(assigned.value().texts.empty());
    EXPECT_EQ(assigned.value().presentationTexts.size(), 1);
    EXPECT_TRUE(f.service.summarize(f.revision, a).get().value().reused);
    auto undone = f.service.undo(f.revision).get();
    ASSERT_TRUE(undone);
    f.revision = undone.value().status.current;
    EXPECT_TRUE(f.service.read(f.revision, {f.script}).get().value()->texts[0].languages.empty());
    // A shared helper is a dependency of its callers, including late writes.
    auto section = f.service.edit({f.revision, {ir::AddSection{f.script, "helper", 2}}}).get();
    ASSERT_TRUE(section);
    f.revision = section.value().status.current;
    auto helperSequence = section.value().sequences[0];
    ir::ValueBuffer write;
    auto value = write.add(ir::ValueKind::Word, 151);
    auto parameter = write.add(ir::ValueKind::Parameter, 0, 0, std::span(&value, 1));
    write.add(ir::ValueKind::Instruction, 17, 0, std::span(&parameter, 1));
    auto inserted = f.service.edit({f.revision, {ir::InsertInstruction{helperSequence, 0, write}}}).get();
    ASSERT_TRUE(inserted);
    f.revision = inserted.value().status.current;
    auto helper = inserted.value().actions[0];
    auto call = [&](SctActionId target) {
        ir::ValueBuffer out;
        auto v = out.add(ir::ValueKind::ActionReference, target.value);
        auto p = out.add(ir::ValueKind::Parameter, 0, 0, std::span(&v, 1));
        out.add(ir::ValueKind::Instruction, 11, 0, std::span(&p, 1));
        return out;
    };
    auto called = f.service.edit({f.revision, {ir::InsertInstruction{f.sequence, 0, call(helper)}}}).get();
    ASSERT_TRUE(called);
    f.revision = called.value().status.current;
    auto shared = f.service.summarize(f.revision, a).get();
    ASSERT_TRUE(shared);
    EXPECT_TRUE(std::ranges::any_of(shared.value().summary->flow,
                                    [](auto &edge) { return edge.key.role == ir::FlowRole::Call; }));
    EXPECT_TRUE(std::ranges::any_of(shared.value().summary->effects,
                                    [](auto &effect) { return effect.variable == ir::Variable{ir::Bank::Bit, 151}; }));
    auto changed = f.service.edit({f.revision, {ir::ReplaceParameter{helper, {0, {}}, word(152)}}}).get();
    ASSERT_TRUE(changed);
    f.revision = changed.value().status.current;
    EXPECT_FALSE(f.service.summarize(f.revision, a).get().value().reused);
    EXPECT_TRUE(f.service.summarize(f.revision, b).get().value().reused);
    auto recursive = f.service.edit({f.revision, {ir::ReplaceInstruction{helper, call(helper)}}}).get();
    ASSERT_TRUE(recursive);
    f.revision = recursive.value().status.current;
    EXPECT_TRUE(f.service.summarize(f.revision, a).get().value().summary->cyclic);
    ir::Entrypoint entry;
    entry.script = f.script;
    entry.name = "M00001";
    entry.kind = SctEntrypointKind::DirectHandler;
    entry.body = helperSequence;
    entry.activation = SctNamedActivation{"M00001", helper};
    entry.enabled = false;
    auto handler = f.service.edit({f.revision, {semantic(ir::PutMetadata{entry})}}).get();
    ASSERT_TRUE(handler) << messages(handler.diagnostics());
    f.revision = handler.value().status.current;
    auto entries = f.service.interactions(f.revision, f.script).get();
    ASSERT_TRUE(entries);
    ASSERT_EQ(entries.value().entrypoints.size(), 1);
    EXPECT_FALSE(entries.value().entrypoints[0].enabled);
    auto repaired = f.service.edit({f.revision, {semantic(ir::BindLifecycle{f.section, "init"})}}).get();
    ASSERT_TRUE(repaired) << messages(repaired.diagnostics());
    f.revision = repaired.value().status.current;
    auto required = f.service.interactions(f.revision, f.script).get().value().entrypoints.back();
    ASSERT_TRUE(required.required());
    required.enabled = false;
    EXPECT_FALSE(f.service.edit({f.revision, {semantic(ir::PutMetadata{required})}}).get());
}
TEST(ProgramSemanticTest, PromotesPlainFlowAndPreservesSourceIdentity) {
    ir::ProgramBuilder builder({Project});
    auto script = builder.importedScript(baseline(), {}).value();
    auto section = builder.section({{}, script, "scene"}).value();
    auto seq = builder.sectionBody(section);
    auto check = builder.expression(*sct::SctExpressionFactory::bitVariable(121).expression).value();
    auto operand = [&](unsigned index, ir::ValueId value) {
        return builder.value(ir::ValueKind::Parameter, index, 0, std::span(&value, 1)).value();
    };
    auto reference = [&](SctActionId id) { return builder.value(ir::ValueKind::ActionReference, id.value).value(); };
    std::array parameters{operand(0, check), operand(1, reference({105}))};
    auto header =
        builder.instruction(seq, builder.value(ir::ValueKind::Instruction, 0, 0, parameters).value(), {101}).value();
    auto empty = builder.value(ir::ValueKind::Instruction, 125).value();
    builder.instruction(seq, empty, {102});
    auto jump = operand(0, reference({106}));
    builder.instruction(seq, builder.value(ir::ValueKind::Instruction, 10, 0, std::span(&jump, 1)).value(), {104});
    builder.instruction(seq, empty, {105});
    builder.instruction(seq, empty, {106});
    auto built = builder.finish();
    ASSERT_TRUE(built) << messages(built.diagnostics());
    ir::ProgramService service;
    auto adopted = service.adopt({}, std::move(built).takeValue()).get();
    ASSERT_TRUE(adopted);
    editor::ProgramBackend adapter(std::shared_ptr<ir::ProgramService>(&service,[](auto*){}));
    auto revision=adopted.value().current;
    auto physical=adapter.outline(revision,{section,0,128});ASSERT_TRUE(physical);EXPECT_EQ(physical.value().total,5u);
    auto semanticPage=adapter.outline(revision,{section,0,128,editor::ViewMode::Semantic});ASSERT_TRUE(semanticPage);
    ASSERT_EQ(semanticPage.value().rows.size(),5u); // Explicit instructions are not display-only groups.
    auto path=adapter.locate(revision,SctActionId{105},editor::ViewMode::Semantic);ASSERT_TRUE(path);
    EXPECT_EQ(path.value().subjects.back(),editor::Subject{SctActionId{105}});
    EXPECT_EQ(service.status().get().value().current,revision);
    auto edit = service.edit({adopted.value().current, {semantic(ir::PromoteFlow{header, {106}})}}).get();
    ASSERT_TRUE(edit) << messages(edit.diagnostics());
    auto view = service.read(edit.value().status.current, {{}, {}, {header}}).get();
    ASSERT_TRUE(view);
    ASSERT_EQ(view.value()->arms.size(), 2);
    EXPECT_EQ(view.value()->actions[0].id, header);
    EXPECT_EQ(view.value()->actions[0].kind, ir::ActionKind::Conditional);
    auto explanation = service.explain(edit.value().status.current, header).get();
    ASSERT_TRUE(explanation);
    ASSERT_EQ(explanation.value().records.size(), 1);
    EXPECT_EQ(explanation.value().records[0].kind, SctEvidenceKind::Inference);
    auto undo = service.undo(edit.value().status.current).get();
    ASSERT_TRUE(undo);
    auto original = service.read(undo.value().status.current, {{}, {}, {header}}).get().value();
    EXPECT_EQ(original->actions[0].kind, ir::ActionKind::Instruction);
    auto scheduled=adapter.execute(undo.value().status.current,editor::SetTiming{SctActionId{102},true,{}});ASSERT_TRUE(scheduled);
    EXPECT_EQ(scheduled.value().projectionSequences,std::vector<SctSequenceId>{seq});
    auto explicitAgain=adapter.outline(scheduled.value().status.current,{section,0,128,editor::ViewMode::Semantic});ASSERT_TRUE(explicitAgain);
    EXPECT_EQ(explicitAgain.value().total,5u);
    auto inspection=adapter.inspect(scheduled.value().status.current,header,{});ASSERT_TRUE(inspection);
    EXPECT_TRUE(inspection.value().evidence.empty());
    service.shutdown().get();
}

TEST(ProgramPersistenceTest, BinaryRoundTripPreservesStructuredIdentityAndLiveContent) {
    Fixture f;
    ir::Presentation presentation;
    presentation.tabs.push_back({f.script, true, f.writer});
    presentation.activeTab = f.script;
    presentation.tabs.front().instructions = {{f.sequence}, {f.writer}, f.writer};
    presentation.tabs.front().semantic = {{f.branch}, {f.branch, f.loop}, f.loop};
    presentation.placements.push_back({f.branch, 1.5, -9, true});
    auto expression = ir::ValueBuffer::expression(*sct::SctExpressionFactory::bitVariable(121).expression).value();
    ir::InsertConstruct sw{
        f.sequence,
        0,
        ir::ActionKind::Switch,
        ir::LoopKind::While,
        expression,
        {{{}, {}, {1, 7}, false, ir::CaseExit::Fallthrough}, {{}, {}, {}, true, ir::CaseExit::ExitSwitch}}};
    auto updated =
        f.service
            .edit({f.revision,
                   {ir::SemanticOperation{sw},
                    ir::SemanticOperation{ir::PutMetadata{ir::Module{{}, f.script, "Scene", {f.sequence}}}},
                    ir::SemanticOperation{ir::PutMetadata{SctLanguageGroup{{}, "Custom language", "custom"}}}}})
            .get();
    ASSERT_TRUE(updated) << messages(updated.diagnostics());
    f.revision = updated.value().status.current;
    auto cp = f.service.checkpoint(f.revision).get().value();
    auto structuredSwitch = cp.program->find(f.sequence)->order->front();
    auto lang = cp.program->metadata().languages.front().id;
    auto membership = f.service.edit({f.revision, {ir::SemanticOperation{ir::AssignLanguages{f.text, {lang}}}}}).get();
    ASSERT_TRUE(membership);
    f.revision = membership.value().status.current;
    ir::ValueBuffer content;
    content.text(ir::ValueKind::PlainText, std::string((1u << 20) + 123, 'x'));
    auto edit = f.service.edit({f.revision, {ir::ReplaceText{f.text, content}}}).get();
    ASSERT_TRUE(edit);
    f.revision = edit.value().status.current;
    cp = f.service.checkpoint(f.revision).get().value();
    auto encoded = ir::ProgramPersistence::encode(*cp.program, presentation);
    ASSERT_TRUE(encoded) << messages(encoded.diagnostics());
    auto loaded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(loaded) << messages(loaded.diagnostics());
    EXPECT_EQ(loaded.value().program->highWater(), cp.program->highWater());
    EXPECT_EQ(loaded.value().program->find(f.writer)->sourceInstruction, cp.program->find(f.writer)->sourceInstruction);
    EXPECT_EQ(loaded.value().program->arms(structuredSwitch).front().id, cp.program->arms(structuredSwitch).front().id);
    EXPECT_EQ(loaded.value().program->loop(f.loop)->condition, cp.program->loop(f.loop)->condition);
    EXPECT_EQ(loaded.value().program->find(f.text)->languages, std::vector{lang});
    EXPECT_EQ(loaded.value().presentation.tabs.front().selection, presentation.tabs.front().selection);
    EXPECT_EQ(loaded.value().presentation.tabs.front().instructions.expanded, presentation.tabs.front().instructions.expanded);
    EXPECT_EQ(loaded.value().presentation.tabs.front().semantic.selected, presentation.tabs.front().semantic.selected);
    EXPECT_EQ(loaded.value().presentation.tabs.front().semantic.current, presentation.tabs.front().semantic.current);
    auto oldSchema = encoded.value(); oldSchema[16] = std::byte{6};
    EXPECT_FALSE(ir::ProgramPersistence::decode(oldSchema));
    auto again = ir::ProgramPersistence::encode(*loaded.value().program, loaded.value().presentation);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), encoded.value());
    EXPECT_LT(loaded.value().program->statistics().values,
              cp.program->statistics().values); // obsolete edited text is not saved
    auto damaged = encoded.value();
    damaged[8] = std::byte{1};
    EXPECT_FALSE(ir::ProgramPersistence::decode(damaged));
    damaged = encoded.value();
    damaged[damaged.size() / 2] ^= std::byte{0x80};
    EXPECT_FALSE(ir::ProgramPersistence::decode(damaged));
    EXPECT_FALSE(ir::ProgramPersistence::decode(std::span(encoded.value()).first(encoded.value().size() - 1)));
    ir::PersistenceLimits tiny;
    tiny.decodedBytes = 32;
    EXPECT_FALSE(ir::ProgramPersistence::decode(encoded.value(), tiny));
    std::stop_source stopped;
    stopped.request_stop();
    EXPECT_FALSE(ir::ProgramPersistence::decode(encoded.value(), {}, stopped.get_token()));
    // The new file is an ordinary editable authority, not a cached view.
    ir::ProgramService reopened;
    auto open = reopened.adopt({}, std::move(loaded.value().program)).get();
    ASSERT_TRUE(open);
    auto change = reopened.edit({open.value().current, {ir::ReplaceParameter{f.writer, {0, {}}, word(222)}}}).get();
    ASSERT_TRUE(change);
    EXPECT_FALSE(reopened.uses(change.value().status.current, {ir::Bank::Bit, 222}).get().value().uses.empty());
    ASSERT_TRUE(reopened.undo(change.value().status.current).get());
    reopened.shutdown().get();
}
TEST(ProgramPersistenceTest, TransactionAndAsyncOpenRejectStaleOrCancelledResults) {
    Fixture f;
    auto cp = f.service.checkpoint(f.revision).get().value();
    auto root = std::filesystem::temp_directory_path() / generateSctAuthoringUuid().value();
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};
    auto mutation = ir::ProgramPersistence::mutation(cp, {}, "authoring/program.salsa", {});
    ASSERT_TRUE(mutation);
    std::filesystem::create_directories(root / "authoring");
    WorkspaceTransactionRequest tx{
        "program-save", "workspace", "program-checkpoint", "save", {std::move(mutation).takeValue()}};
    ASSERT_EQ(WorkspaceTransactionService::prepare(root, "transactions", tx).status,
              WorkspaceTransactionStatus::Prepared);
    auto committed = WorkspaceTransactionService::commitPrepared(root, "transactions", tx.transactionId);
    ASSERT_EQ(committed.status, WorkspaceTransactionStatus::Verified) << messages(committed.diagnostics);
    auto edited = f.service.edit({f.revision, {ir::ReplaceParameter{f.writer, {0, {}}, word(999)}}}).get();
    ASSERT_TRUE(edited);
    f.revision = edited.value().status.current;
    ASSERT_TRUE(f.service.markSaved(cp.status.current, cp.status.historyState).get());
    EXPECT_TRUE(f.service.status().get().value().dirty);
    auto original = f.service.undo(f.revision).get();
    ASSERT_TRUE(original);
    f.revision = original.value().status.current;
    EXPECT_FALSE(original.value().status.dirty);
    std::promise<void> entered, release;
    auto barrier = release.get_future().share();
    std::atomic_bool once = false;
    ir::ProgramOpenRequest request{f.revision, root / "authoring/program.salsa"};
    request.progress = [&](const auto &) {
        if (!once.exchange(true)) {
            entered.set_value();
            barrier.wait();
        }
    };
    auto loading = f.service.openProject(request);
    entered.get_future().wait();
    // Owner service still answers while file work is blocked in its worker.
    auto status = f.service.status();
    EXPECT_EQ(status.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto changed = f.service.edit({f.revision, {ir::ReplaceParameter{f.writer, {0, {}}, word(777)}}}).get();
    ASSERT_TRUE(changed);
    f.revision = changed.value().status.current;
    release.set_value();
    EXPECT_FALSE(loading.get());
    EXPECT_EQ(f.service.status().get().value().current, f.revision);
    std::stop_source cancel;
    cancel.request_stop();
    request.expected = f.revision;
    request.progress = {};
    EXPECT_FALSE(f.service.openProject(request, cancel.get_token()).get());
    auto opened = f.service.openProject(request).get();
    ASSERT_TRUE(opened) << messages(opened.diagnostics());
    EXPECT_NE(opened.value().status.current.generation, f.revision.generation);
    EXPECT_FALSE(opened.value().status.dirty);
}
} // namespace

TEST(EditorBackendTest, SelectedParameterEditAndHistoryKeepIdentityAndAffectedRows) {
    Fixture f;
    editor::ProgramBackend adapter(std::shared_ptr<ir::ProgramService>(&f.service, [](auto *) {}));
    editor::Backend &backend = adapter;
    auto before = backend.inspect(f.revision, f.writer);
    ASSERT_TRUE(before) << messages(before.diagnostics());
    ASSERT_EQ(before.value().parameters.size(), 1u);
    EXPECT_EQ(before.value().parameters[0].value, "121");
    auto edit = backend.replaceParameter(f.revision, {f.writer, {0, {}}, "122"});
    ASSERT_TRUE(edit) << messages(edit.diagnostics());
    EXPECT_EQ(edit.value().actions, std::vector<SctActionId>{f.writer});
    EXPECT_TRUE(edit.value().sequences.empty());
    EXPECT_TRUE(edit.value().scriptLayouts.empty());
    auto current = edit.value().status.current;
    auto inspected = backend.inspect(current, f.writer);
    ASSERT_TRUE(inspected);
    EXPECT_EQ(inspected.value().subject, before.value().subject);
    EXPECT_EQ(inspected.value().parameters[0].value, "122");
    auto row = backend.outline(current, {f.section, 0, 1});
    ASSERT_TRUE(row); ASSERT_EQ(row.value().rows.size(), 1u);
    EXPECT_EQ(row.value().rows.front().subject, editor::Subject{f.writer});
    EXPECT_EQ(row.value().total, 4u);
    auto undone = backend.history(current, false); ASSERT_TRUE(undone);
    auto restored = backend.inspect(undone.value().status.current, f.writer); ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().parameters[0].value, "121");
    auto redone = backend.history(undone.value().status.current, true); ASSERT_TRUE(redone);
    auto again = backend.inspect(redone.value().status.current, f.writer); ASSERT_TRUE(again);
    EXPECT_EQ(again.value().parameters[0].value, "122");
    // Captured reads remain detached; stale commands cannot silently follow edits.
    EXPECT_EQ(before.value().parameters[0].value, "121");
    EXPECT_FALSE(backend.replaceParameter(f.revision, {f.writer, {0, {}}, "123"}));
    EXPECT_FALSE(backend.inspect(f.revision, f.writer));
}
TEST(EditorBackendTest, InvalidCancelledAndReadonlyEditsLeaveStateUntouched) {
    Fixture f;
    editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service, [](auto *) {}));
    EXPECT_FALSE(backend.replaceParameter(f.revision, {f.writer, {0, {}}, "4294967296"}));
    EXPECT_FALSE(backend.replaceParameter(f.revision, {f.writer, {99, {}}, "1"}));
    auto repeated = backend.inspect(f.revision, f.branch); ASSERT_TRUE(repeated);
    ASSERT_EQ(repeated.value().parameters.size(), 2u);
    EXPECT_EQ(repeated.value().parameters[0].key.group, 0u);
    EXPECT_EQ(repeated.value().parameters[1].key.group, 1u);
    EXPECT_EQ(repeated.value().parameters[1].kind, editor::ParameterKind::ActionTarget);
    EXPECT_FALSE(backend.replaceParameter(f.revision, {f.branch, {2, 1}, "0"}));
    std::stop_source cancelled; cancelled.request_stop();
    EXPECT_FALSE(backend.replaceParameter(f.revision, {f.writer, {0, {}}, "123"}, cancelled.get_token()));
    auto wrong = f.revision; ++wrong.generation;
    EXPECT_FALSE(backend.outline(wrong, {f.section}));
    wrong = f.revision; wrong.project.value = "00000000-0000-4000-8000-000000000000";
    EXPECT_FALSE(backend.inspect(wrong, f.writer));
    EXPECT_EQ(f.service.status().get().value().current, f.revision);
}

TEST(EditorBackendTest, CreatesAndBindsTextWithPagedNavigationAndHistory) {
    Fixture f;editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service,[](auto*){}));
    const auto catalog=backend.opcodes();EXPECT_NE(std::ranges::find(catalog,265,&editor::Opcode::code),catalog.end());
    auto inserted=backend.execute(f.revision,editor::InsertOpcode{f.writer,24});ASSERT_TRUE(inserted)<<messages(inserted.diagnostics());
    auto revision=inserted.value().status.current;
    editor::BrowseRequest query;query.kind=editor::BrowseKind::Opcodes;query.script=f.script;query.number=24;query.limit=1;
    auto found=backend.browse(revision,query);ASSERT_TRUE(found);ASSERT_EQ(found.value().rows.size(),1u);
    auto instructionId=std::get<SctActionId>(found.value().rows.front().subject);
    auto created=backend.execute(revision,editor::CreateText{f.script,"debug note",true,sct::SctTextKind::PlainString,sct::SctPlainText{"Hello"}});
    ASSERT_TRUE(created)<<messages(created.diagnostics());ASSERT_TRUE(created.value().suggestedSelection);
    auto textId=std::get<SctTextResourceId>(*created.value().suggestedSelection);revision=created.value().status.current;
    auto inspected=backend.inspect(revision,instructionId);ASSERT_TRUE(inspected);
    auto param=std::ranges::find(inspected.value().parameters,editor::ParameterKind::FooterText,&editor::Parameter::kind);
    ASSERT_NE(param,inspected.value().parameters.end());
    auto bound=backend.execute(revision,editor::BindReference{instructionId,param->key,textId});ASSERT_TRUE(bound)<<messages(bound.diagnostics());
    revision=bound.value().status.current;
    auto owned = backend.inspect(revision,instructionId); ASSERT_TRUE(owned);
    auto ownedParam = std::ranges::find(owned.value().parameters,editor::ParameterKind::FooterText,&editor::Parameter::kind);
    auto originalText = textId;
    textId = std::get<SctTextResourceId>(*ownedParam->target);
    EXPECT_NE(textId, originalText); // Binding copies a string value; it never links editing.
    editor::BrowseRequest uses;uses.kind=editor::BrowseKind::Incoming;uses.target=textId;
    auto refs=backend.browse(revision,uses);ASSERT_TRUE(refs);ASSERT_EQ(refs.value().rows.size(),1u);EXPECT_EQ(refs.value().rows.front().subject,editor::Subject{instructionId});
    auto draft=backend.text(revision,textId);ASSERT_TRUE(draft);draft.value().value=sct::SctPlainText{"Edited"};
    auto saved=backend.execute(revision,editor::WriteText{draft.value()});ASSERT_TRUE(saved);revision=saved.value().status.current;
    EXPECT_FALSE(backend.execute(revision,editor::WriteText{draft.value()}));
    EXPECT_FALSE(backend.execute(revision,editor::DeleteItem{textId})); // Referenced text remains protected.
    auto undo=backend.history(revision,false);ASSERT_TRUE(undo);
    auto original=backend.text(undo.value().status.current,textId);ASSERT_TRUE(original);
    EXPECT_EQ(std::get<sct::SctPlainText>(original.value().value).utf8,"Hello");
}
TEST(EditorBackendTest, ImportedFooterParametersOwnTheirTextThroughEditsCopiesAndExport) {
    sct::SctDocument document;
    auto sourceText = document.allocateSupplementaryTextId();
    document.supplementaryText.push_back({sourceText, sct::SctTextKind::SctString,
        sct::SctMessage{{}, {{sct::SctTextChunk{"Shared source"}, sct::SctInlineCommand{sct::SctMessageCommandCode::E,{}}}}}});
    sct::SctScriptSectionContent code;
    const auto parameter = sct::findSctOpcodeSchema(24)->textReferences[0].parameterIndex;
    for (unsigned n=0; n<2; ++n) {
        sct::SctDocumentInstruction instruction;
        instruction.id=document.allocateInstructionId(); instruction.opcode=24;
        instruction.fixedParameters.push_back({parameter,sct::SctSupplementaryTextReference{sourceText}});
        code.instructions.push_back(instruction);
    }
    code.instructions.push_back({document.allocateInstructionId(),12});
    document.sections.push_back({document.allocateSectionId(),"init",code});
    document.sections.push_back({document.allocateSectionId(),"loop",sct::SctScriptSectionContent{{{document.allocateInstructionId(),12}}}});
    ir::ProgramBuilder builder({Project});
    auto imported=builder.appendImported(document,baseline(),{});ASSERT_TRUE(imported)<<messages(imported.diagnostics());
    auto program=builder.finish();ASSERT_TRUE(program)<<messages(program.diagnostics());
    ASSERT_EQ(program.value()->texts().size(),2u);
    EXPECT_NE(program.value()->texts()[0].id,program.value()->texts()[1].id);
    EXPECT_EQ(program.value()->texts()[0].sourceText,sourceText.value());
    EXPECT_EQ(program.value()->texts()[1].sourceText,sourceText.value());
    auto script=imported.value();
    auto section=program.value()->scripts()[0].sections[0];
    auto body=program.value()->find(section)->body;
    auto actions=*program.value()->find(body)->order;
    auto service=std::make_shared<ir::ProgramService>();
    auto adopted=service->adopt({},std::move(program).takeValue()).get();ASSERT_TRUE(adopted);
    auto revision=adopted.value().current;
    editor::ProgramBackend backend(service);
    auto inspect=[&](SctActionId action){return backend.inspect(revision,action).value().parameters.front();};
    auto first=inspect(actions[0]), second=inspect(actions[1]);
    ASSERT_TRUE(first.textValue);ASSERT_TRUE(second.textValue);
    EXPECT_NE(first.target,second.target);EXPECT_NE(first.value.find("Shared source"),std::string::npos);
    auto changedValue=*first.textValue;
    std::get<sct::SctTextChunk>(std::get<sct::SctMessage>(changedValue).body.elements.front()).utf8="Independent";
    auto edit=backend.execute(revision,editor::WriteStringParameter{actions[0],{parameter,{}},changedValue});ASSERT_TRUE(edit)<<messages(edit.diagnostics());revision=edit.value().status.current;
    EXPECT_EQ(inspect(actions[0]).target,first.target);
    EXPECT_NE(inspect(actions[0]).value.find("Independent"),std::string::npos);
    EXPECT_EQ(inspect(actions[1]).value,second.value);
    auto undo=backend.history(revision,false);ASSERT_TRUE(undo);revision=undo.value().status.current;
    EXPECT_EQ(inspect(actions[0]).value,first.value);
    auto redo=backend.history(revision,true);ASSERT_TRUE(redo);revision=redo.value().status.current;
    auto clipboard=backend.copy(revision,{{actions[0]}});ASSERT_TRUE(clipboard);
    auto review=backend.reviewPaste(revision,actions[1],clipboard.value());ASSERT_TRUE(review);EXPECT_TRUE(review.value().bindings.empty());
    auto pasted=backend.execute(revision,editor::PasteSelection{actions[1],clipboard.value(),{}});ASSERT_TRUE(pasted)<<messages(pasted.diagnostics());revision=pasted.value().status.current;
    ASSERT_FALSE(pasted.value().selected.empty());
    auto copied=std::get<SctActionId>(pasted.value().selected.front());
    EXPECT_NE(inspect(copied).target,first.target);EXPECT_EQ(inspect(copied).value,inspect(actions[0]).value);
    auto copyText = std::get<SctTextResourceId>(*inspect(copied).target);
    auto path=backend.locate(revision,copyText,editor::ViewMode::Semantic);ASSERT_TRUE(path);
    EXPECT_EQ(path.value().subjects.back(),editor::Subject(copied));
    {
        ir::ProgramBuilder destination({"b44499b5-dcf0-449a-9815-5bdf558eface"});
        auto otherScript=destination.importedScript(baseline(true),{}).value();
        auto otherSection=destination.section({{},otherScript,"init"}).value();
        auto ret=destination.instruction(destination.sectionBody(otherSection),destination.value(ir::ValueKind::Instruction,12).value()).value();
        auto otherProgram=destination.finish();ASSERT_TRUE(otherProgram);
        auto otherService=std::make_shared<ir::ProgramService>();
        auto opened=otherService->adopt({},std::move(otherProgram).takeValue()).get();ASSERT_TRUE(opened);
        editor::ProgramBackend other(otherService);
        auto bindings=other.reviewPaste(opened.value().current,ret,clipboard.value());ASSERT_TRUE(bindings);EXPECT_TRUE(bindings.value().bindings.empty());
        auto result=other.execute(opened.value().current,editor::PasteSelection{ret,clipboard.value(),{}});ASSERT_TRUE(result)<<messages(result.diagnostics());
        auto action=std::get<SctActionId>(result.value().selected.front());
        auto value=other.inspect(result.value().status.current,action);ASSERT_TRUE(value);
        EXPECT_EQ(value.value().parameters.front().value,inspect(actions[0]).value);
    }

    auto removed=backend.execute(revision,editor::DeleteItem{copied});ASSERT_TRUE(removed)<<messages(removed.diagnostics());revision=removed.value().status.current;
    auto cp=service->checkpoint(revision).get();ASSERT_TRUE(cp);
    EXPECT_EQ(cp.value().program->find(copyText),nullptr);
    EXPECT_EQ(std::ranges::count_if(cp.value().program->texts(),[](const auto &t){return t.id.valid();}),2);
    auto saved=ir::ProgramPersistence::encode(*cp.value().program);ASSERT_TRUE(saved);
    auto reopened=ir::ProgramPersistence::decode(saved.value());ASSERT_TRUE(reopened)<<messages(reopened.diagnostics());
    EXPECT_EQ(reopened.value().program->texts().size(),2u);
    for(auto mode:{editor::ViewMode::Instructions,editor::ViewMode::Semantic}) {
        auto outline=backend.outline(revision,{script,0,128,mode});ASSERT_TRUE(outline);EXPECT_EQ(outline.value().total,2u);
    }
    ir::ProgramMaterializer materializer;
    auto exported=materializer.exportScript(cp.value(),revision,script,{"f44499b5-dcf0-449a-9815-5bdf558eface"},
        sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,sct::kSctShiftJisByte7FEncoding});
    ASSERT_TRUE(exported)<<messages(exported.diagnostics());
    EXPECT_EQ(exported.value().materialized.document->supplementaryText.size(),2u);
}
TEST(EditorBackendTest, NewStringParameterCanBeEditedWithoutCreatingASeparateTextItem) {
    Fixture f;editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service,[](auto*){}));
    for (std::uint16_t opcode : {23,24}) {
        auto inserted=backend.execute(f.revision,editor::InsertOpcode{f.writer,opcode});ASSERT_TRUE(inserted);
        auto revision=inserted.value().status.current;
        ASSERT_FALSE(inserted.value().actions.empty());
        auto action=inserted.value().actions.front();
        auto parameter=sct::findSctOpcodeSchema(opcode)->textReferences[0].parameterIndex;
        auto inspection=backend.inspect(revision,action);ASSERT_TRUE(inspection);
        EXPECT_EQ(inspection.value().parameters.front().textKind,sct::findSctOpcodeSchema(opcode)->textReferences[0].kind);
        sct::SctTextValue message=sct::SctMessage{{},{{sct::SctTextChunk{"New message"},sct::SctInlineCommand{sct::SctMessageCommandCode::E,{}}}}};
        if(opcode==23)message=sct::SctPlainText{"New message"};
        auto edited=backend.execute(revision,editor::WriteStringParameter{action,{parameter,{}},message});
        ASSERT_TRUE(edited)<<messages(edited.diagnostics());
        f.revision=edited.value().status.current;
        auto inspected=backend.inspect(f.revision,action);ASSERT_TRUE(inspected);
        EXPECT_NE(inspected.value().parameters.front().value.find("New message"),std::string::npos);
    }
}
TEST(EditorBackendTest, CreatesAndEditsStructuredArmsWithoutWidgetStorageTypes) {
    Fixture f;editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service,[](auto*){}));
    auto condition=*sct::SctExpressionFactory::bitVariable(121).expression;
    auto inserted=backend.execute(f.revision,editor::InsertStructure{f.other,"If",condition});ASSERT_TRUE(inserted)<<messages(inserted.diagnostics());
    auto revision=inserted.value().status.current;
    auto rows=backend.outline(revision,{f.section});ASSERT_TRUE(rows);
    auto at=std::ranges::find_if(rows.value().rows,[&](const auto&r){return r.label=="If / Else If / Else";});ASSERT_NE(at,rows.value().rows.end());
    auto action=std::get<SctActionId>(at->subject);auto draft=backend.structure(revision,action);ASSERT_TRUE(draft);ASSERT_EQ(draft.value().arms.size(),2u);
    auto body=backend.execute(revision,editor::InsertOpcode{draft.value().arms[0].id,12});ASSERT_TRUE(body)<<messages(body.diagnostics());revision=body.value().status.current;
    draft=backend.structure(revision,action);auto originalArm=draft.value().arms[0].id;
    editor::ArmDraft added;added.condition=*sct::SctExpressionFactory::bitVariable(122).expression;
    draft.value().arms.insert(draft.value().arms.begin()+1,added);
    auto edited=backend.execute(revision,editor::WriteArms{draft.value()});ASSERT_TRUE(edited)<<messages(edited.diagnostics());
    auto current=backend.structure(edited.value().status.current,action);ASSERT_TRUE(current);EXPECT_EQ(current.value().arms.size(),3u);EXPECT_EQ(current.value().arms[0].id,originalArm);
    EXPECT_FALSE(backend.execute(edited.value().status.current,editor::WriteArms{draft.value()}));
}

// View flattening must retain one stored chain and exact branch edit targets.
TEST(EditorBackendTest, CompactConditionsKeepBranchIdentityAndPreparationEditable) {
    ir::ProgramBuilder builder({Project});
    auto script = builder.importedScript(baseline(), {}).value();
    auto section = builder.section({{}, script, "story"}).value();
    auto body = builder.sectionBody(section);
    auto bit = [&](unsigned n) { return builder.expression(*sct::SctExpressionFactory::bitVariable(n).expression).value(); };
    const std::array conditions{bit(121), bit(122), ir::ValueId{}};
    auto chain = builder.conditional(body, conditions).value();
    auto checks = builder.branchConditions(chain);
    auto bodies = builder.branchBodies(chain);
    auto firstBody = builder.instruction(bodies[0], builder.value(ir::ValueKind::Instruction, 12).value()).value();
    auto last = builder.instruction(body, builder.value(ir::ValueKind::Instruction, 12).value()).value();
    auto built = builder.finish(); ASSERT_TRUE(built) << messages(built.diagnostics());
    auto service = std::make_shared<ir::ProgramService>();
    auto opened = service->adopt({}, std::move(built).takeValue()).get(); ASSERT_TRUE(opened);
    auto revision = opened.value().current;
    editor::ProgramBackend backend(service);
    auto page = backend.outline(revision, {section, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(page);
    ASSERT_EQ(page.value().rows.size(), 4u);
    EXPECT_EQ(page.value().rows[0].subject, editor::Subject(chain));
    EXPECT_EQ(page.value().rows[0].label, "If BitVar[121]");
    EXPECT_EQ(page.value().rows[1].label, "Else If BitVar[122]");
    EXPECT_EQ(page.value().rows[2].label, "Else");
    EXPECT_EQ(page.value().rows[3].subject, editor::Subject(last));
    auto second = page.value().rows[1].subject;
    auto fallback = page.value().rows[2].subject;
    for (std::size_t i = 0; i < page.value().rows.size(); ++i) {
        auto single = backend.outline(revision, {section, i, 1, editor::ViewMode::Semantic}); ASSERT_TRUE(single);
        EXPECT_EQ(single.value().total, 4u);
        ASSERT_EQ(single.value().rows.size(), 1u);
        EXPECT_EQ(single.value().rows[0].subject, page.value().rows[i].subject);
    }
    auto children = backend.outline(revision, {chain, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(children);
    ASSERT_EQ(children.value().rows.size(), 1u);
    EXPECT_EQ(children.value().rows[0].subject, editor::Subject(firstBody));
    auto inspected = backend.inspect(revision, second); ASSERT_TRUE(inspected);
    EXPECT_EQ(inspected.value().conditionTarget, checks[1]);
    EXPECT_EQ(inspected.value().body, bodies[1]);
    EXPECT_EQ(inspected.value().construct, chain);
    auto elseInspection = backend.inspect(revision, fallback); ASSERT_TRUE(elseInspection);
    EXPECT_FALSE(elseInspection.value().conditionTarget);
    EXPECT_FALSE(elseInspection.value().condition);
    auto path = backend.locate(revision, checks[1], editor::ViewMode::Semantic); ASSERT_TRUE(path);
    EXPECT_EQ(path.value().subjects, (std::vector<editor::Subject>{script, section, second}));
    auto childPath = backend.locate(revision, firstBody, editor::ViewMode::Semantic); ASSERT_TRUE(childPath);
    EXPECT_EQ(childPath.value().subjects, (std::vector<editor::Subject>{script, section, chain, firstBody}));
    auto whole = backend.copy(revision, {{chain}}); ASSERT_TRUE(whole);
    auto visible = backend.copy(revision, {{chain, second, fallback}}); ASSERT_TRUE(visible);
    EXPECT_EQ(whole.value(), visible.value());
    EXPECT_FALSE(backend.copy(revision, {{second}}));

    auto edit = backend.execute(revision, editor::SetExpression{*inspected.value().conditionTarget, {},
                                       *sct::SctExpressionFactory::bitVariable(123).expression});
    ASSERT_TRUE(edit) << messages(edit.diagnostics()); revision = edit.value().status.current;
    EXPECT_EQ(edit.value().conditions, (std::vector<ir::ConditionId>{checks[1]}));
    page = backend.outline(revision, {section, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(page);
    EXPECT_EQ(page.value().rows[0].label, "If BitVar[121]");
    EXPECT_EQ(page.value().rows[1].label, "Else If BitVar[123]");
    auto preparation = backend.execute(revision, editor::InsertOpcode{checks[1], 16});
    ASSERT_TRUE(preparation) << messages(preparation.diagnostics()); revision = preparation.value().status.current;
    auto prepRows = backend.outline(revision, {second, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(prepRows);
    EXPECT_TRUE(prepRows.value().rows.empty());
    inspected = backend.inspect(revision, second); ASSERT_TRUE(inspected);
    ASSERT_EQ(inspected.value().supportingSequences.size(), 1u);
    auto held = inspected.value().supportingSequences.front().sequence;
    auto instructions = backend.outline(revision, {checks[1], 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(instructions);
    ASSERT_EQ(instructions.value().rows.size(), 1u);
    auto firstPrep = instructions.value().rows[0].subject;
    auto more = backend.execute(revision, editor::InsertOpcode{checks[1], 16}); ASSERT_TRUE(more);
    revision = more.value().status.current;
    instructions = backend.outline(revision, {checks[1], 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(instructions);
    ASSERT_EQ(instructions.value().rows.size(), 2u); EXPECT_EQ(instructions.value().rows[0].subject, firstPrep);
    page = backend.outline(revision, {section, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(page);
    EXPECT_EQ(page.value().rows[1].label, "Else If BitVar[123] [2 instructions before check]");
    path = backend.locate(revision, firstPrep, editor::ViewMode::Semantic); ASSERT_TRUE(path);
    EXPECT_EQ(path.value().subjects, (std::vector<editor::Subject>{script, section, second}));
    EXPECT_EQ(path.value().heldSequence, held);
    auto heldInspection = backend.inspect(revision, firstPrep); ASSERT_TRUE(heldInspection);
    EXPECT_EQ(heldInspection.value().supportingOwner, second);
    EXPECT_EQ(heldInspection.value().heldSequence, held);
    EXPECT_TRUE(heldInspection.value().instruction);
    for (int n = 0; n < 2; ++n) {
        auto undo = backend.history(revision, false); ASSERT_TRUE(undo); revision = undo.value().status.current;
    }
    prepRows = backend.outline(revision, {second, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(prepRows);
    EXPECT_TRUE(prepRows.value().rows.empty());
    auto redo = backend.history(revision, true); ASSERT_TRUE(redo);
    revision = redo.value().status.current;
    EXPECT_TRUE(backend.outline(revision, {checks[1], 0, 128, editor::ViewMode::Semantic}));
    auto draft = backend.structure(revision, chain); ASSERT_TRUE(draft);
    editor::ArmDraft added; added.condition = *sct::SctExpressionFactory::bitVariable(124).expression;
    draft.value().arms.insert(draft.value().arms.end() - 1, added);
    auto changed = backend.execute(revision, editor::WriteArms{draft.value()}); ASSERT_TRUE(changed);
    revision = changed.value().status.current;
    page = backend.outline(revision, {section, 3, 2, editor::ViewMode::Semantic}); ASSERT_TRUE(page);
    EXPECT_EQ(page.value().total, 5u); ASSERT_EQ(page.value().rows.size(), 2u);
    EXPECT_EQ(page.value().rows[0].subject, fallback); EXPECT_EQ(page.value().rows[1].subject, editor::Subject(last));
    auto undo = backend.history(revision, false); ASSERT_TRUE(undo);
    page = backend.outline(undo.value().status.current, {section, 3, 1, editor::ViewMode::Semantic}); ASSERT_TRUE(page);
    EXPECT_EQ(page.value().total, 4u); EXPECT_EQ(page.value().rows[0].subject, editor::Subject(last));
}
TEST(EditorBackendTest, LoopAndSwitchHeadersExposeExactConditionsWithoutEmptyPreparationRows) {
    ir::ProgramBuilder builder({Project});
    auto script = builder.importedScript(baseline(), {}).value();
    auto section = builder.section({{}, script, "loops"}).value();
    auto body = builder.sectionBody(section);
    auto value = builder.expression(*sct::SctExpressionFactory::bitVariable(121).expression).value();
    auto loop = builder.loop(body, ir::LoopKind::While, value).value();
    auto condition = builder.loop(loop)->condition;
    auto sw = builder.switchRegion(body, value, std::array{ir::Case{{1}, true}}).value();
    auto built = builder.finish(); ASSERT_TRUE(built);
    auto service = std::make_shared<ir::ProgramService>();
    auto opened = service->adopt({}, std::move(built).takeValue()).get(); ASSERT_TRUE(opened);
    auto revision = opened.value().current;
    editor::ProgramBackend backend(service);
    auto page = backend.outline(revision, {section, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(page);
    ASSERT_EQ(page.value().rows.size(), 2u);
    EXPECT_EQ(page.value().rows[0].label, "While BitVar[121]");
    EXPECT_EQ(page.value().rows[1].label, "Switch BitVar[121]");
    auto inspect = backend.inspect(revision, loop); ASSERT_TRUE(inspect);
    EXPECT_EQ(inspect.value().conditionTarget, condition); EXPECT_TRUE(inspect.value().body);
    auto loopRows = backend.outline(revision, {loop, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(loopRows);
    EXPECT_TRUE(loopRows.value().rows.empty());
    ASSERT_EQ(inspect.value().supportingSequences.size(), 1u);
    auto caseRows = backend.outline(revision, {sw, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(caseRows);
    ASSERT_EQ(caseRows.value().rows.size(), 1u); EXPECT_EQ(caseRows.value().rows[0].label, "Case 1 Default");
    auto prep = backend.execute(revision, editor::InsertOpcode{condition, 16}); ASSERT_TRUE(prep);
    revision = prep.value().status.current;
    loopRows = backend.outline(revision, {loop, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(loopRows);
    EXPECT_TRUE(loopRows.value().rows.empty());
    inspect = backend.inspect(revision, loop); ASSERT_TRUE(inspect);
    auto held = inspect.value().supportingSequences.front().sequence;
    auto heldRows = backend.outline(revision, {held, 0, 128, editor::ViewMode::Instructions}); ASSERT_TRUE(heldRows);
    ASSERT_EQ(heldRows.value().rows.size(), 1u);
    auto path = backend.locate(revision, heldRows.value().rows.front().subject, editor::ViewMode::Semantic);
    ASSERT_TRUE(path); EXPECT_EQ(path.value().heldSequence, held);
    EXPECT_EQ(path.value().subjects.back(), editor::Subject(loop));
}

TEST(EditorBackendTest, SupportingLoopPhasesKeepNestedNavigationAndLocalHistory) {
    for (auto kind : {ir::LoopKind::While, ir::LoopKind::DoWhile, ir::LoopKind::For}) {
        ir::ProgramBuilder builder({Project});
        auto script = builder.importedScript(baseline(), {}).value();
        auto section = builder.section({{}, script, "loops"}).value();
        auto value = builder.expression(*sct::SctExpressionFactory::bitVariable(121).expression).value();
        auto loop = builder.loop(builder.sectionBody(section), kind, value).value();
        auto detail = *builder.loop(loop);
        auto prep = builder.conditionPreparation(detail.condition);
        auto nested = builder.conditional(prep, std::array{value}).value();
        auto leaf = builder.instruction(builder.branchBodies(nested).front(),
                                       builder.value(ir::ValueKind::Instruction, 16).value()).value();
        auto bodyAction = builder.instruction(detail.body,
                                             builder.value(ir::ValueKind::Instruction, 16).value()).value();
        auto built = builder.finish(); ASSERT_TRUE(built);
        auto service = std::make_shared<ir::ProgramService>();
        auto opened = service->adopt({}, std::move(built).takeValue()).get(); ASSERT_TRUE(opened);
        auto revision = opened.value().current;
        editor::ProgramBackend backend(service);
        auto rows = backend.outline(revision, {loop, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(rows);
        ASSERT_EQ(rows.value().rows.size(), 1u);
        EXPECT_EQ(rows.value().rows.front().subject, editor::Subject(bodyAction));
        auto bodyPath = backend.locate(revision, bodyAction, editor::ViewMode::Semantic); ASSERT_TRUE(bodyPath);
        EXPECT_FALSE(bodyPath.value().heldSequence);
        EXPECT_EQ(bodyPath.value().subjects, (std::vector<editor::Subject>{script, section, loop, bodyAction}));
        auto leafPath = backend.locate(revision, leaf, editor::ViewMode::Semantic); ASSERT_TRUE(leafPath);
        EXPECT_EQ(leafPath.value().subjects, (std::vector<editor::Subject>{script, section, loop}));
        EXPECT_EQ(leafPath.value().heldSequence, prep);
        auto raw = backend.locate(revision, leaf, editor::ViewMode::Instructions); ASSERT_TRUE(raw);
        EXPECT_EQ(raw.value().subjects.back(), editor::Subject(leaf));
        EXPECT_FALSE(raw.value().heldSequence);
        auto inspected = backend.inspect(revision, leaf); ASSERT_TRUE(inspected) << messages(inspected.diagnostics());
        EXPECT_TRUE(inspected.value().instruction);
        EXPECT_EQ(inspected.value().supportingOwner, editor::Subject(loop));
        EXPECT_EQ(inspected.value().heldSequence, prep);
        ASSERT_EQ(inspected.value().supportingSequences.size(), kind == ir::LoopKind::For ? 3u : 1u);
        if (kind == ir::LoopKind::For) {
            EXPECT_EQ(inspected.value().supportingSequences[0].sequence, detail.initialize);
            EXPECT_EQ(inspected.value().supportingSequences[2].sequence, detail.update);
            auto edit = backend.execute(revision, editor::InsertOpcode{detail.update, 16}); ASSERT_TRUE(edit);
            revision = edit.value().status.current;
            auto phase = backend.outline(revision, {detail.update, 0, 128}); ASSERT_TRUE(phase);
            ASSERT_EQ(phase.value().rows.size(), 1u);
            auto added = phase.value().rows.front().subject;
            auto path = backend.locate(revision, added, editor::ViewMode::Semantic); ASSERT_TRUE(path);
            EXPECT_EQ(path.value().heldSequence, detail.update);
            auto header = backend.outline(revision, {section, 0, 128, editor::ViewMode::Semantic}); ASSERT_TRUE(header);
            EXPECT_NE(header.value().rows.front().label.find("1 update instructions"), std::string::npos);
            auto undo = backend.history(revision, false); ASSERT_TRUE(undo);
            revision = undo.value().status.current;
            phase = backend.outline(revision, {detail.update, 0, 128}); ASSERT_TRUE(phase);
            EXPECT_TRUE(phase.value().rows.empty());
            auto redo = backend.history(revision, true); ASSERT_TRUE(redo);
            phase = backend.outline(redo.value().status.current, {detail.update, 0, 128}); ASSERT_TRUE(phase);
            EXPECT_EQ(phase.value().rows.front().subject, added);
        }
    }
}
TEST(EditorProductivityTest, FragmentsRemapSelectedReferencesAndRetainEmptyGroups) {
    Fixture f;
    editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service, [](auto *) {}));
    auto original = f.service.checkpoint(f.revision).get();
    ASSERT_TRUE(original);
    auto bytes = backend.copy(f.revision, {{f.writer, f.branch, f.loop}});
    ASSERT_TRUE(bytes) << messages(bytes.diagnostics());
    auto again = backend.copy(f.revision, {{f.loop, f.branch, f.writer}});
    ASSERT_TRUE(again);
    EXPECT_EQ(bytes.value(), again.value());
    auto review = backend.reviewPaste(f.revision, f.other, bytes.value());
    ASSERT_TRUE(review);
    EXPECT_TRUE(review.value().bindings.empty());
    auto pasted = backend.execute(f.revision, editor::PasteSelection{f.other, bytes.value(), {}});
    ASSERT_TRUE(pasted) << messages(pasted.diagnostics());
    auto revision = pasted.value().status.current;
    auto checkpoint = f.service.checkpoint(revision).get();
    ASSERT_TRUE(checkpoint);
    auto &program = *checkpoint.value().program;
    const auto &order = *program.find(f.sequence)->order;
    ASSERT_EQ(order.size(), 7u);
    auto copiedWriter = order[2], copiedBranch = order[3], copiedLoop = order[4];
    EXPECT_NE(copiedWriter, f.writer);
    EXPECT_EQ(program.find(copiedLoop)->kind, ir::ActionKind::Loop);
    EXPECT_NE(program.loop(copiedLoop)->body, program.loop(f.loop)->body);
    ASSERT_FALSE(program.incoming(copiedWriter).empty());
    EXPECT_EQ(program.incoming(copiedWriter).front().site.action, copiedBranch);
    EXPECT_EQ(program.incoming(f.writer).front().site.action, f.branch);
    auto saved = ir::ProgramPersistence::encode(program);
    ASSERT_TRUE(saved);
    auto reopened = ir::ProgramPersistence::decode(saved.value());
    ASSERT_TRUE(reopened) << messages(reopened.diagnostics());
    EXPECT_NE(reopened.value().program->find(copiedLoop), nullptr);
    auto undo = backend.history(revision, false);
    ASSERT_TRUE(undo);
    auto afterUndo = f.service.checkpoint(undo.value().status.current).get();
    ASSERT_TRUE(afterUndo);
    EXPECT_EQ(*afterUndo.value().program->find(f.sequence)->order,
              *original.value().program->find(f.sequence)->order);
    auto redo = backend.history(undo.value().status.current, true);
    ASSERT_TRUE(redo);
    revision = redo.value().status.current;
    auto condition = *sct::SctExpressionFactory::bitVariable(122).expression;
    auto inserted = backend.execute(revision, editor::InsertStructure{f.other, "If", condition});
    ASSERT_TRUE(inserted);
    revision = inserted.value().status.current;
    auto scope = f.service.checkpoint(revision).get();
    ASSERT_TRUE(scope);
    auto id = (*scope.value().program->find(f.sequence)->order)[2];
    auto fragment = backend.copy(revision, {{id}});
    ASSERT_TRUE(fragment);
    auto empty = backend.execute(revision, editor::PasteSelection{f.other, fragment.value(), {}});
    ASSERT_TRUE(empty) << messages(empty.diagnostics());
    scope = f.service.checkpoint(empty.value().status.current).get();
    ASSERT_TRUE(scope);
    auto cloned = (*scope.value().program->find(f.sequence)->order)[2];
    EXPECT_NE(cloned, id);
    ASSERT_EQ(scope.value().program->arms(cloned).size(), 2u);
    for (auto arm : scope.value().program->arms(cloned))
        EXPECT_TRUE(scope.value().program->find(arm.body)->order->empty());
    auto external = backend.copy(empty.value().status.current, {{f.branch}});
    ASSERT_TRUE(external);
    ir::ProgramBuilder second({"c44499b5-dcf0-449a-9815-5bdf558eface"});
    auto script = second.importedScript(baseline(true), {}).value();
    auto section = second.section({{}, script, "receiver"}).value();
    auto ret =
        second.instruction(second.sectionBody(section), second.value(ir::ValueKind::Instruction, 12).value())
            .value();
    auto built = second.finish();
    ASSERT_TRUE(built);
    auto service = std::make_shared<ir::ProgramService>();
    auto adopted = service->adopt({}, std::move(built).takeValue()).get();
    ASSERT_TRUE(adopted) << messages(adopted.diagnostics());
    editor::ProgramBackend other(service);
    auto reviewed = other.reviewPaste(adopted.value().current, ret, external.value());
    ASSERT_TRUE(reviewed);
    ASSERT_EQ(reviewed.value().bindings.size(), 1u);
    EXPECT_FALSE(reviewed.value().bindings.front().target);
    EXPECT_FALSE(other.execute(adopted.value().current, editor::PasteSelection{ret, external.value(), {}}));
    reviewed.value().bindings.front().target = ret;
    auto bound = other.execute(adopted.value().current,
                               editor::PasteSelection{ret, external.value(), reviewed.value().bindings});
    ASSERT_TRUE(bound) << messages(bound.diagnostics());
    auto imported = service->checkpoint(bound.value().status.current).get();
    ASSERT_TRUE(imported);
    ASSERT_FALSE(imported.value().program->incoming(ret).empty());
    service->shutdown().get();
    auto malformed = fragment.value();
    malformed[8] = std::byte{9};
    EXPECT_FALSE(
        backend.execute(empty.value().status.current, editor::PasteSelection{f.other, malformed, {}}));
}
TEST(EditorProductivityTest, MultiItemMovesDeletesAndSectionCopiesAreAtomic) {
    Fixture f;
    editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service, [](auto *) {}));
    auto section = f.service.edit({f.revision, {ir::AddSection{f.script, "destination", 1}}}).get();
    ASSERT_TRUE(section);
    auto revision = section.value().status.current;
    auto checkpoint = f.service.checkpoint(revision).get();
    auto newSection = checkpoint.value().program->scripts().front().sections.back();
    auto newBody = checkpoint.value().program->find(newSection)->body;
    auto moved = backend.execute(revision, editor::MoveSelection{{f.other, f.loop}, newBody});
    ASSERT_TRUE(moved) << messages(moved.diagnostics());
    revision = moved.value().status.current;
    checkpoint = f.service.checkpoint(revision).get();
    ASSERT_TRUE(checkpoint);
    EXPECT_EQ(*checkpoint.value().program->find(newBody)->order, (std::vector<SctActionId>{f.other, f.loop}));
    EXPECT_EQ(checkpoint.value().program->find(f.loop)->owner, newBody);
    auto body = checkpoint.value().program->loop(f.loop)->body;
    EXPECT_FALSE(backend.execute(revision, editor::MoveSelection{{f.loop}, body}));
    EXPECT_FALSE(backend.execute(revision, editor::DeleteSelection{{f.writer}}));
    auto fragment = backend.copy(revision, {{newSection}});
    ASSERT_TRUE(fragment);
    auto pasted = backend.execute(revision, editor::PasteSelection{newSection, fragment.value(), {}});
    ASSERT_TRUE(pasted) << messages(pasted.diagnostics());
    revision = pasted.value().status.current;
    checkpoint = f.service.checkpoint(revision).get();
    ASSERT_TRUE(checkpoint);
    auto copy = checkpoint.value().program->scripts().front().sections.back();
    EXPECT_NE(copy, newSection);
    EXPECT_EQ(checkpoint.value().program->find(copy)->name, "destination_copy1");
    auto erased = backend.execute(revision, editor::DeleteSelection{{copy}});
    ASSERT_TRUE(erased) << messages(erased.diagnostics());
    auto undo = backend.history(erased.value().status.current, false);
    ASSERT_TRUE(undo);
    checkpoint = f.service.checkpoint(undo.value().status.current).get();
    EXPECT_NE(checkpoint.value().program->find(copy), nullptr);
    EXPECT_FALSE(backend.execute(revision, editor::DeleteSelection{{copy}})); // stale, not another deletion
}
TEST(EditorProductivityTest, NotesFoldersAndCatalogPersistWithoutReindexingExecution) {
    Fixture f;
    editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service, [](auto *) {}));
    auto note = backend.execute(
        f.revision,
        editor::WriteNote{{f.writer, "Story flag", "Set by the opening scene", "Opening", 0xaabbcc}});
    ASSERT_TRUE(note) << messages(note.diagnostics());
    auto revision = note.value().status.current;
    EXPECT_TRUE(note.value().actions.empty());
    auto page = backend.outline(revision, {f.section});
    ASSERT_TRUE(page);
    EXPECT_EQ(page.value().rows.front().label, "Story flag");
    EXPECT_EQ(page.value().rows.front().color, 0xaabbcc);
    EXPECT_TRUE(page.value().rows.front().bookmarked);
    auto folders = backend.execute(
        revision,
        editor::WriteOrganization{
            f.script, {f.section}, {{f.script, "Scenes", {}, {}}, {f.script, "Opening", {f.section}, 0}}});
    ASSERT_TRUE(folders) << messages(folders.diagnostics());
    revision = folders.value().status.current;
    auto custom = backend.execute(
        revision,
        editor::WriteOpcode{{17, "Set story bit", "Sets the selected bit", 0x99bbdd, {{0, "Flag"}}}});
    ASSERT_TRUE(custom) << messages(custom.diagnostics());
    revision = custom.value().status.current;
    auto inspection = backend.inspect(revision, f.writer);
    ASSERT_TRUE(inspection);
    EXPECT_EQ(inspection.value().parameters.front().name, "Flag");
    auto checkpoint = f.service.checkpoint(revision).get();
    auto saved = ir::ProgramPersistence::encode(*checkpoint.value().program);
    ASSERT_TRUE(saved);
    auto reopened = ir::ProgramPersistence::decode(saved.value());
    ASSERT_TRUE(reopened) << messages(reopened.diagnostics());
    EXPECT_EQ(reopened.value().program->metadata().folders.size(), 2u);
    EXPECT_EQ(reopened.value().program->metadata().opcodes.front().name, "Set story bit");
    auto undo = backend.history(revision, false);
    ASSERT_TRUE(undo);
    auto org = backend.organization(undo.value().status.current, f.script);
    ASSERT_TRUE(org);
    EXPECT_TRUE(org.value().opcodes.empty());
    EXPECT_EQ(org.value().folders.size(), 2u);
    auto redo = backend.history(undo.value().status.current, true);
    ASSERT_TRUE(redo);
    auto deleted =
        backend.execute(redo.value().status.current, editor::DeleteSelection{{f.writer, f.branch}});
    ASSERT_TRUE(deleted) << messages(deleted.diagnostics());
    auto restored = backend.history(deleted.value().status.current, false);
    ASSERT_TRUE(restored);
    page = backend.outline(restored.value().status.current, {f.section});
    ASSERT_TRUE(page);
    EXPECT_TRUE(page.value().rows.front().bookmarked);
}

TEST(EditorAuthoringToolsTest, ModulesActivationAndResourceSelectionShareHistory) {
    Fixture f; editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service,[](auto*){}));
    auto edit=[&](editor::FeatureEdit op){auto result=backend.execute(f.revision,std::move(op));if(result)f.revision=result.value().status.current;return result;};
    auto module=edit(editor::WriteModule{{{},f.script,"Standalone",{}},true});ASSERT_TRUE(module)<<messages(module.diagnostics());
    auto inventory=backend.features(f.revision,f.script);ASSERT_TRUE(inventory);ASSERT_EQ(inventory.value().modules.size(),1u);auto m=inventory.value().modules.front();ASSERT_EQ(m.bodies.size(),1u);
    auto outline=backend.outline(f.revision,{m.id});ASSERT_TRUE(outline);ASSERT_EQ(outline.value().rows.size(),1u);EXPECT_EQ(outline.value().rows.front().subject,editor::Subject(m.bodies.front()));
    auto body=backend.outline(f.revision,{m.bodies.front()});ASSERT_TRUE(body);EXPECT_TRUE(body.value().rows.empty());
    m.bodies.push_back(f.sequence);ASSERT_TRUE(edit(editor::WriteModule{m}));
    auto detail=backend.module(f.revision,m.id);ASSERT_TRUE(detail)<<messages(detail.diagnostics());
    editor::EntryItem entry;entry.script=f.script;entry.name="M00001";entry.body=f.sequence;entry.activation=SctNamedActivation{"scene",f.writer};
    ASSERT_TRUE(edit(editor::WriteEntry{entry}));inventory=backend.features(f.revision,f.script);ASSERT_TRUE(inventory);entry=inventory.value().entries.front();auto entryId=entry.id;entry.enabled=false;
    ASSERT_TRUE(edit(editor::WriteEntry{entry}));auto oldRevision=f.revision;
    smores::script::Resource resource{{"synthetic","field/context.mld","revision-one","document-one"},smores::script::ResourceRole::Base,{}};
    resource.entries.push_back({resource.identity,7,spice::mld::MldEntryId{2},3,1,"GRND",{}});
    backend.setResourceLoader([resource](auto,auto,auto){return Result<smores::script::Resource>::success(resource);});
    EXPECT_FALSE(edit(editor::SelectResourceEntry{entry.id,resource,7}));
    ASSERT_TRUE(backend.inspectResource(f.revision,"field/context.mld",resource.role));
    ASSERT_TRUE(edit(editor::SelectResourceEntry{entry.id,resource,7}));inventory=backend.features(f.revision,f.script);ASSERT_TRUE(inventory);EXPECT_EQ(inventory.value().entries.front().id,entryId);EXPECT_FALSE(inventory.value().entries.front().enabled);EXPECT_TRUE(inventory.value().entries.front().provider);
    EXPECT_FALSE(backend.execute(oldRevision,editor::FeatureEdit{editor::WriteEntry{entry}}));
    auto undo=backend.history(f.revision,false);ASSERT_TRUE(undo);f.revision=undo.value().status.current;
    inventory=backend.features(f.revision,f.script);ASSERT_TRUE(inventory);EXPECT_FALSE(inventory.value().entries.front().provider);
    entry.enabled=true;
    auto response=edit(editor::WriteEntry{entry,editor::FeatureSite{f.branch,{2,0}},f.sequence});ASSERT_TRUE(response)<<messages(response.diagnostics());
    auto initial=backend.execute(f.revision,editor::SetExpression{f.condition,{},*SctExpressionLanguage::parse("Low16IntVar[15] == 1").expression});ASSERT_TRUE(initial);f.revision=initial.value().status.current;
    ASSERT_TRUE(edit(editor::SetArrival{f.condition,"me033b",{}}));
    auto inspection=backend.inspect(f.revision,f.condition);ASSERT_TRUE(inspection);ASSERT_TRUE(inspection.value().condition);EXPECT_NE(SctExpressionLanguage::project(*inspection.value().condition).text.find("331"),std::string::npos);
    auto saved=f.service.checkpoint(f.revision).get();ASSERT_TRUE(saved);auto encoded=ir::ProgramPersistence::encode(*saved.value().program);ASSERT_TRUE(encoded);auto reopened=ir::ProgramPersistence::decode(encoded.value());ASSERT_TRUE(reopened);EXPECT_EQ(reopened.value().program->metadata().modules.front().id,m.id);
}
TEST(EditorAuthoringToolsTest, LanguageBatchesPreserveExplicitAssignmentsAndUndo) {
    Fixture f;editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service,[](auto*){}));
    auto create=[&](std::string name){auto result=backend.execute(f.revision,editor::CreateText{f.script,name,false,sct::SctTextKind::PlainString,sct::SctPlainText{"Initial"}});if(!result)throw std::runtime_error(messages(result.diagnostics()));f.revision=result.value().status.current;return std::get<SctTextResourceId>(*result.value().suggestedSelection);};
    auto base=create("ahello"),french=create("aFhello");auto suggestions=backend.suggestLanguages(f.revision,f.script);ASSERT_TRUE(suggestions);ASSERT_EQ(suggestions.value().size(),2u);
    auto apply=backend.execute(f.revision,editor::FeatureEdit{editor::ApplyLanguageSuggestions{f.script,{base,french}}});ASSERT_TRUE(apply)<<messages(apply.diagnostics());f.revision=apply.value().status.current;
    auto inventory=backend.features(f.revision,f.script);ASSERT_TRUE(inventory);ASSERT_EQ(inventory.value().languages.size(),2u);auto group=inventory.value().languages.front().id;
    auto assigned=backend.execute(f.revision,editor::FeatureEdit{editor::AssignTextGroups{{base,french},{group}}});ASSERT_TRUE(assigned);f.revision=assigned.value().status.current;
    auto rescan=backend.execute(f.revision,editor::FeatureEdit{editor::ApplyLanguageSuggestions{f.script,{base,french}}});ASSERT_TRUE(rescan);f.revision=rescan.value().status.current;
    inventory=backend.features(f.revision,f.script);ASSERT_TRUE(inventory);for(const auto& t:inventory.value().texts)if(t.id==base || t.id==french)EXPECT_EQ(t.languages,(std::vector<SctLanguageGroupId>{group}));
    auto link=backend.execute(f.revision,editor::FeatureEdit{editor::WriteLogicalText{{{},f.script,"Greeting",{base,french},{}}}});ASSERT_TRUE(link);f.revision=link.value().status.current;
    auto batch=backend.execute(f.revision,editor::FeatureEdit{editor::WriteTextBatch{{{base,sct::SctPlainText{"One"}},{french,sct::SctPlainText{"Deux"}}}}});ASSERT_TRUE(batch)<<messages(batch.diagnostics());f.revision=batch.value().status.current;
    auto value=backend.text(f.revision,base);ASSERT_TRUE(value);EXPECT_EQ(std::get<sct::SctPlainText>(value.value().value).utf8,"One");
    auto undo=backend.history(f.revision,false);ASSERT_TRUE(undo);value=backend.text(undo.value().status.current,french);ASSERT_TRUE(value);EXPECT_EQ(std::get<sct::SctPlainText>(value.value().value).utf8,"Initial");
    f.revision=undo.value().status.current;
    auto inserted=backend.execute(f.revision,editor::InsertOpcode{f.other,144});ASSERT_TRUE(inserted)<<messages(inserted.diagnostics());f.revision=inserted.value().status.current;
    auto checkpoint=f.service.checkpoint(f.revision).get();ASSERT_TRUE(checkpoint);auto dialogAction=(*checkpoint.value().program->find(f.sequence)->order)[2];
    auto parameter=sct::findSctOpcodeSchema(144)->textReferences[0].parameterIndex;
    auto bound=backend.execute(f.revision,editor::BindReference{dialogAction,{parameter,{}},base});ASSERT_TRUE(bound)<<messages(bound.diagnostics());f.revision=bound.value().status.current;
    auto split=backend.execute(f.revision,editor::FeatureEdit{editor::SplitText{base,group,"separate",{{dialogAction,{parameter,{}}}}}});ASSERT_TRUE(split)<<messages(split.diagnostics());f.revision=split.value().status.current;
    auto splitText=std::get<SctTextResourceId>(split.value().selected.front());auto uses=backend.textUses(f.revision,splitText);ASSERT_TRUE(uses);EXPECT_EQ(uses.value().size(),1u);
    auto insertion=backend.execute(f.revision,editor::FeatureEdit{editor::InsertTextUses{{{{dialogAction,{parameter,{}}},f.branch,french,{}}}}});ASSERT_TRUE(insertion)<<messages(insertion.diagnostics());f.revision=insertion.value().status.current;
    uses=backend.textUses(f.revision,french);ASSERT_TRUE(uses);EXPECT_EQ(uses.value().size(),1u);
    auto rewind=backend.history(f.revision,false);ASSERT_TRUE(rewind);uses=backend.textUses(rewind.value().status.current,french);ASSERT_TRUE(uses);EXPECT_TRUE(uses.value().empty());

}
TEST(EditorAuthoringToolsTest, SelectorBatchPropagatesKnownCopiesAndRejectsBranchPreview) {
    Fixture f;editor::ProgramBackend backend(std::shared_ptr<ir::ProgramService>(&f.service,[](auto*){}));
    auto literal=SctExpressionLanguage::parse("2"),copy=SctExpressionLanguage::parse("ByteVar[8]");ASSERT_TRUE(literal.succeeded());ASSERT_TRUE(copy.succeeded());
    auto result=backend.execute(f.revision,editor::FeatureEdit{editor::WriteSelectors{f.other,{{{},8,*literal.expression},{},}}});
    // The invalid second edit leaves the first uninstalled.
    EXPECT_FALSE(result);
    result=backend.execute(f.revision,editor::FeatureEdit{editor::WriteSelectors{f.other,{{{},8,*literal.expression},{{},9,*copy.expression}}}});ASSERT_TRUE(result)<<messages(result.diagnostics());f.revision=result.value().status.current;
    ASSERT_EQ(result.value().selected.size(),2u);auto first=std::get<SctActionId>(result.value().selected.front()),last=std::get<SctActionId>(result.value().selected.back());
    auto preview=backend.previewSelectors(f.revision,first,last,{{6,1}});ASSERT_TRUE(preview)<<messages(preview.diagnostics());EXPECT_EQ(preview.value().values[8],2);EXPECT_EQ(preview.value().values[9],2);EXPECT_EQ(preview.value().values[6],1);EXPECT_EQ(preview.value().tiles.size(),42u);
    EXPECT_FALSE(backend.previewSelectors(f.revision,f.writer,f.branch,{}));auto undo=backend.history(f.revision,false);ASSERT_TRUE(undo);auto checkpoint=f.service.checkpoint(undo.value().status.current).get();ASSERT_TRUE(checkpoint);EXPECT_EQ(checkpoint.value().program->find(f.sequence)->order->size(),4u);
}

namespace {
TEST(LocalizationTest, SharedBodyWideCsvAndRuntimeLanguagesShareOneHistory) {
    for (int mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        ir::ProgramBuilder b({Project});
        SctScriptContext context;
        context.name = "a";
        auto script = b.importedScript(baseline(), context).value();
        for (auto name : {"init", "loop"}) {
            auto seq = b.sectionBody(b.section({{}, script, name}).value());
            b.instruction(seq, b.value(ir::ValueKind::Instruction, 12).value());
        }
        auto main = b.sectionBody(b.section({{}, script, "scene"}).value());
        std::array conditions{
            b.expression(*SctExpressionLanguage::parse("ByteVar[87] == 0").expression).value(),
            b.expression(*SctExpressionLanguage::parse("ByteVar[87] == 1").expression).value()};
        SctActionId header;
        std::vector<SctSequenceId> bodies;
        if (mode >= 2) {
            std::vector<ir::Case> cases{ir::Case{{0}}, ir::Case{{1}}};
            if (mode == 3)
                cases.push_back(ir::Case{{}, true});
            header = b.switchRegion(
                          main, b.expression(*SctExpressionLanguage::parse("ByteVar[87]").expression).value(),
                          cases)
                         .value();
            bodies = b.branchBodies(header);
        } else if (mode == 1) {
            header = b.conditional(main, std::span(conditions).first(1)).value();
            bodies = b.branchBodies(header);
            auto next = b.conditional(main, std::span(conditions).subspan(1)).value();
            bodies.push_back(b.branchBodies(next).front());
        } else {
            header = b.conditional(main, conditions).value();
            bodies = b.branchBodies(header);
        }
        b.instruction(main, b.value(ir::ValueKind::Instruction, 12).value());
        auto built = b.finish();
        ASSERT_TRUE(built) << messages(built.diagnostics());
        auto service = std::make_shared<ir::ProgramService>();
        auto opened = service->adopt({}, std::move(built).takeValue()).get();
        ASSERT_TRUE(opened) << messages(opened.diagnostics());
        auto revision = opened.value().current;
        editor::ProgramBackend backend(service);
        auto edit = [&](editor::EditIntent request) {
            auto result = backend.execute(revision, std::move(request));
            if (!result)
                throw std::runtime_error(messages(result.diagnostics()));
            revision = result.value().status.current;
            return result.value();
        };
        auto create = [&](std::string name, std::string value) {
            sct::SctMessage msg;
            msg.headerUtf8 = "Speaker";
            msg.body.elements = {sct::SctTextChunk{value},
                                 sct::SctInlineCommand{sct::SctMessageCommandCode::E, {}}};
            auto c = edit(editor::CreateText{script, name, false, sct::SctTextKind::SctString, msg});
            return std::get<SctTextResourceId>(*c.suggestedSelection);
        };
        auto base = create("ahello", "Hello, \"there\"\nNext"), french = create("aFhello", "Bonjour");
        std::vector<SctActionId> dialogActions;
        for (std::size_t n = 0; n < 2; ++n) {
            edit(editor::InsertOpcode{bodies[n], 144});
            auto cp = service->checkpoint(revision).get().value();
            auto id = cp.program->find(bodies[n])->order->front();
            dialogActions.push_back(id);
            auto parameter = sct::findSctOpcodeSchema(144)->textReferences[0].parameterIndex;
            edit(editor::BindReference{id, {parameter, {}}, n ? french : base});
            if (mode == 3) {
                // Equivalent branches compare owned strings by value, not text ID.
                auto printed=edit(editor::InsertOpcode{id,23});
                auto print=printed.actions.front();
                edit(editor::WriteStringParameter{print,{0,{}},sct::SctPlainText{"same debug text"}});
            }
            auto owner = cp.program->find(std::get<ir::ArmId>(*cp.program->find(bodies[n])->owner))->owner;
            auto transfer =
                service
                    ->edit({revision,
                            {semantic(ir::InsertTransfer{bodies[n], mode == 3 ? 2u : 1u, ir::TransferKind::Break, owner})}})
                    .get();
            ASSERT_TRUE(transfer) << messages(transfer.diagnostics());
            revision = transfer.value().status.current;
        }
        editor::LocalizationChange discover;
        discover.discover = true;
        // The fresh-import pass performs discovery and reduction together, using
        // the same equivalence rules as these explicit editing commands.
        {
            auto source = service->checkpoint(revision).get(); ASSERT_TRUE(source);
            ir::ProgramService imported;
            auto adopted = imported.adopt({}, std::move(source.value().program)).get();
            ASSERT_TRUE(adopted) << messages(adopted.diagnostics());
            std::vector<std::string> phases;
            auto ready = imported.organizeImportedLocalization(adopted.value().current, {},
                [&](const auto &p) { phases.push_back(p.phase); }).get();
            ASSERT_TRUE(ready) << messages(ready.diagnostics());
            auto result = imported.checkpoint(ready.value().current).get(); ASSERT_TRUE(result);
            EXPECT_EQ(result.value().program->metadata().localization.blocks.size(), 1u);
            EXPECT_EQ(result.value().program->metadata().logicalTexts.size(), 1u);
            EXPECT_NE(std::ranges::find(phases, "Reducing localized sequences"), phases.end());
        }
        edit(editor::FeatureEdit{discover});
        auto catalog = backend.localizations(revision, true);
        ASSERT_TRUE(catalog) << messages(catalog.diagnostics());
        ASSERT_EQ(catalog.value().messages.size(), 1u);
        ASSERT_EQ(catalog.value().candidates.size(), 1u);
        EXPECT_EQ(catalog.value().candidates[0].first, header);
        auto original = service->checkpoint(revision).get().value();
        ir::ProgramMaterializer materializer;
        auto output = [&] {
            auto cp = service->checkpoint(revision).get().value();
            return materializer.exportScript(
                cp, revision, script, {"f44499b5-dcf0-449a-9815-5bdf558eface"},
                sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding});
        };
        editor::LocalizationChange organize;
        organize.promote = {header};
        edit(editor::FeatureEdit{organize});
        auto cp = service->checkpoint(revision).get().value();
        ASSERT_EQ(cp.program->metadata().localization.blocks.size(), 1u);
        EXPECT_NE(cp.program->find(dialogActions[0]), nullptr);
        EXPECT_EQ(cp.program->find(dialogActions[1]), nullptr);
        auto lowered = output();
        ASSERT_TRUE(lowered) << messages(lowered.diagnostics());
        auto checkDefault = [&](const auto &result, std::size_t languageCount) {
            if (mode < 2) return;
            const auto &sections = result.value().materialized.document->sections;
            const auto scene = std::ranges::find(sections, std::string("scene"), &sct::SctDocumentSection::nameBytes);
            ASSERT_NE(scene, sections.end());
            const auto &code = std::get<sct::SctScriptSectionContent>(scene->content).instructions;
            const auto sw = std::ranges::find(code, 3, &sct::SctDocumentInstruction::opcode);
            ASSERT_NE(sw, code.end());
            EXPECT_EQ(sw->repeatedParameterGroups.size(), languageCount + (mode == 3 ? 1u : 0u));
            const auto defaults = std::ranges::count_if(sw->repeatedParameterGroups, [](const auto &g) {
                return std::get<sct::SctEncodedWordValue>(g.parameters[0].value).value == UINT32_MAX;
            });
            EXPECT_EQ(defaults, mode == 3 ? 1 : 0);
        };
        checkDefault(lowered, 2);
        if (mode == 1) {
            auto &doc = *lowered.value().materialized.document;
            auto section =
                std::ranges::find(doc.sections, std::string("scene"), &sct::SctDocumentSection::nameBytes);
            ASSERT_NE(section, doc.sections.end());
            auto &code = std::get<sct::SctScriptSectionContent>(section->content).instructions;
            std::vector<sct::SctInstructionId> guards;
            for (auto &i : code)
                if (i.opcode == 0)
                    guards.push_back(i.id);
            ASSERT_EQ(guards.size(), 2u);
            auto jump = std::ranges::find(code, 10u, &sct::SctDocumentInstruction::opcode);
            ASSERT_NE(jump, code.end());
            EXPECT_EQ(std::get<sct::SctInstructionReference>(jump->fixedParameters.front().value).target,
                      guards[1]);
        }

        auto language = cp.program->find(french)->languages.front();
        auto outline =
            backend.outline(revision, {bodies.front(), 0, 128, editor::ViewMode::Semantic, language});
        ASSERT_TRUE(outline);
        EXPECT_NE(outline.value().rows.front().label.find("Bonjour"), std::string::npos);
        auto mapped = backend.translation(revision, base, language);
        ASSERT_TRUE(mapped);
        EXPECT_EQ(mapped.value(), french);
        catalog = backend.localizations(revision);
        ASSERT_TRUE(catalog);
        std::vector<std::string> languages;
        for (auto &l : catalog.value().languages)
            languages.push_back(l.name);
        auto csv = editor::LocalizationCsv::encode(catalog.value(), languages);
        ASSERT_TRUE(csv);
        EXPECT_NE(csv.value().find("\"Message\",\"Unmarked\",\"French\""), std::string::npos);
        auto decoded = editor::LocalizationCsv::decode(csv.value(), catalog.value());
        ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
        ASSERT_EQ(decoded.value().size(), 2u);
        auto wrong = catalog.value();
        wrong.revision.project.value = "other";
        EXPECT_FALSE(editor::LocalizationCsv::decode(csv.value(), wrong));
        editor::LocalizationChange translate;
        translate.translations = decoded.value();
        auto message = catalog.value().messages.front().id;
        translate.translations.push_back(
            {message, "Mod language",
             sct::SctMessage{
                 {},
                 {{sct::SctTextChunk{"New"}, sct::SctInlineCommand{sct::SctMessageCommandCode::E, {}}}}}});
        edit(editor::FeatureEdit{translate});
        EXPECT_FALSE(output()); // new runtime language has no assigned selector yet
        catalog = backend.localizations(revision);
        ASSERT_TRUE(catalog);
        auto rules = catalog.value().settings.languages;
        ASSERT_EQ(rules.size(), 3u);
        rules.back().selector = 8;
        rules[1].included = false;
        editor::LocalizationChange settings;
        settings.languages = rules;
        edit(editor::FeatureEdit{settings});
        auto newOutput = output();
        ASSERT_TRUE(newOutput) << messages(newOutput.diagnostics());
        checkDefault(newOutput, 2);
        cp = service->checkpoint(revision).get().value();
        auto encoded = ir::ProgramPersistence::encode(*cp.program);
        ASSERT_TRUE(encoded);
        auto reopened = ir::ProgramPersistence::decode(encoded.value());
        ASSERT_TRUE(reopened) << messages(reopened.diagnostics());
        EXPECT_EQ(reopened.value().program->metadata().localization.blocks.size(), 1u);
        EXPECT_EQ(reopened.value().program->metadata().localization.languages.back().selector, 8u);
        if (mode >= 2)
            EXPECT_EQ(std::ranges::any_of(reopened.value().program->arms(header), &ir::Arm::includesDefault), mode == 3);
        auto undo = backend.history(revision, false);
        ASSERT_TRUE(undo);
        revision = undo.value().status.current;
        EXPECT_FALSE(output());
        auto redo = backend.history(revision, true);
        ASSERT_TRUE(redo);
        revision = redo.value().status.current;
        EXPECT_TRUE(output());
        editor::LocalizationChange expand;
        expand.expand = {header};
        edit(editor::FeatureEdit{expand});
        cp = service->checkpoint(revision).get().value();
        EXPECT_TRUE(cp.program->metadata().localization.blocks.empty());
        EXPECT_NE(cp.program->find(dialogActions[0]), nullptr);
        auto expandedOutput = output();
        ASSERT_TRUE(expandedOutput) << messages(expandedOutput.diagnostics());
        checkDefault(expandedOutput, 2);
        service->shutdown().get();
    }
}
TEST(LocalizationTest, CsvPreservesKindsCommandsAndMissingCells) {
    editor::LocalizationCatalog catalog;
    catalog.revision.project = {Project};
    auto language = SctLanguageGroupId{1};
    catalog.languages.push_back({language, "Custom, language", {}});
    sct::SctMessage message;
    message.headerUtf8 = "A)\\B";
    message.body.elements = {
        sct::SctTextChunk{"=literal,\"quoted\"\n\\message"},
        sct::SctInlineCommand{sct::SctMessageCommandCode::C, sct::SctByteListCommandArgument{{1, 2, 3}}},
        sct::SctInlineCommand{sct::SctMessageCommandCode::P, sct::SctDecimalCommandArgument{12}},
        sct::SctInlineCommand{sct::SctMessageCommandCode::E, {}}};
    std::vector<sct::SctTextValue> values{message, sct::SctPlainText{""}, sct::SctEmptyIndexedText{},
                                          sct::SctMessage{}};
    for (std::size_t n = 0; n < values.size(); ++n) {
        auto id = SctLogicalTextId{n + 10};
        editor::LocalizationMessage row;
        row.id = id;
        row.translations.push_back({id, "Custom, language", values[n]});
        catalog.messages.push_back(row);
    }
    auto missing = SctLogicalTextId{20};
    catalog.messages.push_back({missing});
    auto encoded = editor::LocalizationCsv::encode(catalog, {"Custom, language"});
    ASSERT_TRUE(encoded);
    auto decoded = editor::LocalizationCsv::decode(encoded.value(), catalog);
    ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
    ASSERT_EQ(decoded.value().size(), 5u);
    EXPECT_FALSE(decoded.value().back().value);
    for (std::size_t n = 0; n < values.size(); ++n) {
        ASSERT_TRUE(decoded.value()[n].value);
        EXPECT_EQ(editor::LocalizationCsv::text(*decoded.value()[n].value).value(),
                  editor::LocalizationCsv::text(values[n]).value());
    }
    EXPECT_FALSE(editor::LocalizationCsv::decode(encoded.value() + "\"unfinished", catalog));
}
} // namespace
