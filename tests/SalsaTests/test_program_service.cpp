#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Program/EditorWorkspace.h"
#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Persistence/AtomicFile.h"
#include "SalsaCore/Program/ProgramService.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>

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
    std::atomic<unsigned> parsed = 0;
    auto opened = ir::EditorWorkspaceIO::open(request, {}, [&](const auto& p) {
        if (p.phase == "Parsing SCT") {
            ++parsed;
            // The scan has already captured this file. Nothing downstream may reopen it.
            EXPECT_TRUE(std::filesystem::remove(root / "source" / (p.item + ".sct")));
        }
    });
    ASSERT_TRUE(opened) << messages(opened.diagnostics());
    EXPECT_EQ(parsed, 3u);
    auto session = opened.value();
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
    auto saved = ir::EditorWorkspaceIO::save(*session, session->status.current, {}); ASSERT_TRUE(saved);
    parsed = 0;
    auto reopened = ir::EditorWorkspaceIO::open(request, {}, [&](const auto& p) { if (p.phase == "Parsing SCT") ++parsed; });
    ASSERT_TRUE(reopened) << messages(reopened.diagnostics());
    EXPECT_EQ(parsed, 0u);
    for (std::size_t i = 0; i < session->catalog.scripts.size(); ++i)
        EXPECT_EQ(session->catalog.scripts[i].id, reopened.value()->catalog.scripts[i].id);
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
