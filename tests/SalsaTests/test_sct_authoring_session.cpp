#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fstream>

namespace {
using namespace salsa::core;
namespace sct = spice::sct;
SctAuthoringImportRequest source(std::string name) {
    sct::SctDocument d;
    sct::SctDocumentInstruction word; word.id = d.allocateInstructionId(); word.opcode = 16;
    word.fixedParameters = {{0, sct::SctExpressionFactory::encodedDecimalLiteral(12)}};
    d.sections.push_back({d.allocateSectionId(), "MAIN", sct::SctScriptSectionContent{{word}}});
    auto output = sct::SctDocumentExporter::exportDocument(d, sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,
        sct::kSctShiftJisByte7FEncoding, sct::SctDocumentOutputByteOrder::BigEndian,
        sct::SctDocumentOutputWrapper::Raw, sct::SctOpaquePreservationPolicy::RequirePreservation,
        {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
    if (!output.success) throw std::runtime_error("Synthetic export failed");
    auto span = std::as_bytes(std::span(output.bytes));
    std::vector<std::byte> bytes(span.begin(), span.end());
    auto hash = sha256(bytes).value();
    return {{{AssetLocator::fromRelativePath(name).value(), bytes.size(), {hash}}, std::move(bytes)},
        {GamePlatform::GameCube, GameRegion::Japan, {hash}}, {GamePlatform::GameCube, false}, {}, name};
}
SctAuthoringState initial() {
    auto p = SctAuthoringProject::create().value();
    auto a = SctAuthoringImporter::import(p, p.revision, source("a.sct"));
    if (!a) throw std::runtime_error(a.diagnostics()[0].message);
    auto b = SctAuthoringImporter::import(a.value().project, a.value().project.revision, source("b.sct"));
    if (!b) throw std::runtime_error(b.diagnostics()[0].message);
    return {b.value().project, {a.value().program, b.value().program}};
}
Result<SctAuthoringState> rename(const SctAuthoringState& state) {
    auto next = state;
    for (auto& script : next.project.scripts) script.name += " renamed";
    return Result<SctAuthoringState>::success(std::move(next));
}
struct Workspace {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("salsa-s3-" + generateSctAuthoringUuid().value());
    LocalSalsaWorkspace workspace;
    explicit Workspace(const SctAuthoringState& state) : workspace(LocalSalsaWorkspace::openOrCreate(root,
        {root / "source", {GamePlatform::GameCube, GameRegion::Japan, state.project.baselines.front().datasetFingerprint}}).value()) {}
    ~Workspace() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
TEST(SctAuthoringSessionTest, TwoScriptTransactionHasOneHistoryAndMonotonicRevisions) {
    auto opened = SctAuthoringSession::open(initial()); ASSERT_TRUE(opened);
    auto& session = *opened.value(); const auto before = session.capture();
    auto change = session.execute(before->project.revision, "Rename both scripts", rename); ASSERT_TRUE(change);
    EXPECT_EQ(change.value().affectedScripts.size(), 2u); EXPECT_TRUE(session.isDirty());
    EXPECT_EQ(session.undoDescription(), "Rename both scripts");
    auto undone = session.undo(); ASSERT_TRUE(undone);
    EXPECT_GT(undone.value().after.value, change.value().after.value);
    EXPECT_FALSE(session.canUndo()); EXPECT_FALSE(session.isDirty());
    EXPECT_EQ(session.state().project.scripts[0].name, "a.sct");
    EXPECT_EQ(session.state().project.scripts[1].name, "b.sct");
    ASSERT_TRUE(session.redo()); EXPECT_TRUE(session.isDirty());
    EXPECT_EQ(session.state().project.scripts[0].id, before->project.scripts[0].id);
    auto captured = session.capture(); session.markSaved(*captured); EXPECT_FALSE(session.isDirty());
    ASSERT_TRUE(session.execute(session.state().project.revision, "Another rename", rename));
    session.markSaved(*captured); EXPECT_TRUE(session.isDirty());
}
TEST(SctAuthoringSessionTest, RejectsPartialAndStaleCommandsWithoutAdoptingAnything) {
    auto session = std::move(SctAuthoringSession::open(initial())).takeValue();
    const auto before = session->capture();
    EXPECT_FALSE(session->execute({1}, "Stale", rename));
    EXPECT_FALSE(session->execute(before->project.revision, "Bad second script", [](const auto& state) {
        auto next = state; next.project.scripts[0].name = "Changed"; next.project.scripts[1].baseline = {9999};
        return Result<SctAuthoringState>::success(std::move(next));
    }));
    EXPECT_FALSE(session->execute(before->project.revision, "Throw", [](const auto&) -> Result<SctAuthoringState> { throw std::runtime_error("cancelled preparation"); }));
    EXPECT_EQ(session->capture(), before); EXPECT_FALSE(session->canUndo()); EXPECT_FALSE(session->isDirty());
}
TEST(SctAuthoringSessionTest, UndoNeverReusesAllocatedEntityIdsAndBranchingDiscardsRedo) {
    auto session = std::move(SctAuthoringSession::open(initial())).takeValue();
    auto imported = session->execute(session->state().project.revision, "Import c", [](const auto& state) {
        auto next = SctAuthoringImporter::import(state.project, state.project.revision, source("c.sct"));
        if (!next) return Result<SctAuthoringState>::failure(next.diagnostics());
        auto programs = state.programs; programs.push_back(next.value().program);
        return Result<SctAuthoringState>::success({next.value().project, std::move(programs)});
    }); ASSERT_TRUE(imported);
    const auto high = session->state().project.nextEntityId;
    ASSERT_TRUE(session->undo()); EXPECT_EQ(session->state().project.nextEntityId, high);
    EXPECT_FALSE(session->execute(session->state().project.revision, "Reuse retired content ID", [&](const auto& state) {
        auto next = state;
        auto baseline = next.project.baselines.front(); baseline.id = {high - 1};
        next.project.baselines.push_back(baseline);
        return Result<SctAuthoringState>::success(std::move(next));
    }));
    ASSERT_TRUE(session->execute(session->state().project.revision, "Branch", rename)); EXPECT_FALSE(session->canRedo());
}
TEST(SctAuthoringSessionTest, StaleOutputIsRejectedAfterEditUndoOrForeignProject) {
    auto session = std::move(SctAuthoringSession::open(initial())).takeValue();
    auto captured = session->capture();
    auto output = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(captured->project),
        captured->programs, {captured->project.scripts[0].id}});
    ASSERT_TRUE(output.succeeded()); EXPECT_TRUE(session->accepts(output));
    auto forged = output; forged.scripts[0].prepared->realization.key.script = {999}; EXPECT_FALSE(session->accepts(forged));
    ASSERT_TRUE(session->execute(captured->project.revision, "Rename", rename)); EXPECT_FALSE(session->accepts(output));
    ASSERT_TRUE(session->undo()); EXPECT_FALSE(session->accepts(output));
}
TEST(SctAuthoringStoreTest, PhysicalEditsMetadataAndLegacyIdentitySurviveSaveReopenAndExport) {
    auto state = initial(); auto script = state.project.scripts.front().id;
    state.project.scripts.front().legacyOrigin = SctLegacyOrigin{"capsule", "old key", 5};
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script); ASSERT_TRUE(working);
    auto doc = std::make_shared<sct::SctDocument>(*working.value().document);
    doc->sections[0].nameBytes = "RENAMED";
    std::get<sct::SctScriptSectionContent>(doc->sections[0].content).instructions[0].fixedParameters[0].value = sct::SctExpressionFactory::encodedDecimalLiteral(98);
    working.value().document = doc;
    auto edited = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working.value()); ASSERT_TRUE(edited);
    state.project = edited.value();
    ASSERT_TRUE(state.project.contents[0].physicalPatch);
    Workspace w(state); SctAuthoringPresentation presentation{state.project.id, {{script, 100, 200, true}}, {script}};
    auto saved = SctAuthoringStore::save(w.workspace, state, presentation, {}); ASSERT_TRUE(saved) << saved.diagnostics()[0].message;
    auto loaded = SctAuthoringStore::load(w.workspace); ASSERT_TRUE(loaded); ASSERT_TRUE(loaded.value());
    EXPECT_EQ(SctAuthoringCodec::encode(loaded.value()->state.project).value(), SctAuthoringCodec::encode(state.project).value());
    EXPECT_EQ(loaded.value()->presentation.placements[0].x, 100);
    EXPECT_EQ(loaded.value()->state.project.scripts.front().legacyOrigin->scriptKey, "old key");
    auto output = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(loaded.value()->state.project), loaded.value()->state.programs, {script}});
    ASSERT_TRUE(output.succeeded()); EXPECT_FALSE(output.scripts[0].prepared->reusedSource);
    EXPECT_EQ(output.scripts[0].prepared->document->sections[0].nameBytes, "RENAMED");
    EXPECT_FALSE(SctAuthoringStore::save(w.workspace, state, presentation, {}));
    EXPECT_EQ(SctAuthoringStore::save(w.workspace, state, presentation, saved.value()).value(), saved.value());
}
TEST(SctAuthoringStoreTest, InterruptedSaveRecoversAndCancelledLoadReturnsNoProject) {
    auto state = initial(); Workspace w(state); SctAuthoringPresentation view{state.project.id};
    auto first = SctAuthoringStore::save(w.workspace, state, view, {}); ASSERT_TRUE(first);
    state.project.scripts[0].name = "new";
    auto interrupted = SctAuthoringStore::save(w.workspace, state, view, first.value(), {}, {[](std::size_t) { return false; }});
    EXPECT_FALSE(interrupted);
    auto reopened = SctAuthoringStore::load(w.workspace); ASSERT_TRUE(reopened); ASSERT_TRUE(reopened.value());
    EXPECT_EQ(reopened.value()->state.project.scripts[0].name, "new");
    std::stop_source stop; stop.request_stop(); EXPECT_FALSE(SctAuthoringStore::load(w.workspace, stop.get_token()));
}
TEST(SctAuthoringStoreTest, DamagedPhysicalIdentityBindingFailsClosed) {
    auto state = initial(); Workspace w(state);
    ASSERT_TRUE(SctAuthoringStore::save(w.workspace, state, {state.project.id}, {}));
    const auto path = w.workspace.componentPath(SctAuthoringStore::relativePath(w.workspace));
    nlohmann::json root; { std::ifstream input(path); input >> root; }
    root["bindings"][std::to_string(state.project.baselines[0].id.value)] = "wrong";
    { std::ofstream output(path); output << root.dump(); }
    EXPECT_FALSE(SctAuthoringStore::load(w.workspace));
}
TEST(SctAuthoringStoreTest, CancelledSaveAndMissingOrCorruptBaselinesCannotAdoptAProject) {
    auto state = initial(); Workspace w(state); std::stop_source stop; stop.request_stop();
    EXPECT_FALSE(SctAuthoringStore::save(w.workspace, state, {state.project.id}, {}, stop.get_token()));
    EXPECT_FALSE(std::filesystem::exists(w.workspace.componentPath(SctAuthoringStore::relativePath(w.workspace))));
    ASSERT_TRUE(SctAuthoringStore::save(w.workspace, state, {state.project.id}, {}));
    DirectorySctBaselineStore baselines(w.workspace.componentPath(w.workspace.descriptor().components.baselines));
    const auto path = baselines.path(state.project.baselines[0].source.revision);
    { std::ofstream corrupt(path, std::ios::binary | std::ios::trunc); corrupt << "corrupt"; }
    EXPECT_FALSE(SctAuthoringStore::load(w.workspace));
    std::filesystem::remove(path);
    EXPECT_FALSE(SctAuthoringStore::load(w.workspace));
}
TEST(SctAuthoringStoreTest, RetiredScriptsRemoveOnlyTheirGeneratedPatchCheckpoint) {
    auto state = initial(); Workspace w(state);
    auto saved = SctAuthoringStore::save(w.workspace, state, {state.project.id}, {}); ASSERT_TRUE(saved);
    const auto retired = state.project.scripts.back();
    const auto path = w.workspace.patchPath(state.project.find(retired.baseline)->source.locator);
    ASSERT_TRUE(std::filesystem::exists(path));
    std::erase_if(state.project.contents, [&](const auto& c) { return c.owner == SctContentOwner{retired.id}; });
    state.project.scripts.pop_back();
    std::erase_if(state.project.baselines, [&](const auto& b) { return b.id == retired.baseline; });
    std::erase_if(state.programs, [&](const auto& p) { return p->baseline().id == retired.baseline; });
    ++state.project.revision.value;
    ASSERT_TRUE(SctAuthoringStore::save(w.workspace, state, {state.project.id}, saved.value()));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(w.workspace.patchPath(state.project.baselines.front().source.locator)));
    auto reopened = SctAuthoringStore::load(w.workspace); ASSERT_TRUE(reopened); ASSERT_TRUE(reopened.value());
    EXPECT_EQ(reopened.value()->state.project.scripts.size(), 1u);
}
} // namespace
