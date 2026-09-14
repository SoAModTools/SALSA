#include "SalsaCore/Program/ProgramService.h"
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
    EXPECT_GT(progress, 0);
    auto one = sequential.load(nativeRequest({}, 1)).get();
    ASSERT_TRUE(one) << messages(one.diagnostics());
    auto a = parallel.checkpoint(loaded.value().current).get().value().program;
    auto b = sequential.checkpoint(one.value().current).get().value().program;
    EXPECT_EQ(a->highWater(), b->highWater());
    ASSERT_EQ(a->scripts().size(), 2);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(a->scripts()[i].context.id, b->scripts()[i].context.id);
        EXPECT_EQ(a->scripts()[i].sections, b->scripts()[i].sections);
    }
    auto init = a->scripts()[0].sections[0];
    EXPECT_FALSE(parallel.edit({loaded.value().current, {ir::RenameSection{init, "changed"}}}).get());
    parallel.shutdown().get();
    sequential.shutdown().get();
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
} // namespace
