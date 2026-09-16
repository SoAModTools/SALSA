#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <gtest/gtest.h>
namespace {
using namespace salsa::core;
namespace ed = editor;
namespace ir = program;
namespace sct = spice::sct;
std::string messages(const std::vector<Diagnostic> &ds) {
    std::string out;
    for (auto &d : ds)
        out += d.message + '\n';
    return out;
}
auto expr(const char *s) { return *SctExpressionLanguage::parse(s).expression; }
struct StoryFixture {
    std::shared_ptr<ir::ProgramService> service = std::make_shared<ir::ProgramService>();
    ed::ProgramBackend backend{service};
    ed::Revision revision;
    SctScriptId a, b;
    SctModuleId ma{1001}, mb{1002};
    StoryFixture(bool organize = true) {
        ir::ProgramBuilder builder({"e44499b5-dcf0-449a-9815-5bdf558eface"});
        for (bool second : {false, true}) {
            sct::SctDocument d;
            auto init = d.allocateInstructionId(), loop = d.allocateInstructionId(),
                 first = d.allocateInstructionId(), middle = d.allocateInstructionId(),
                 end = d.allocateInstructionId();
            d.sections.push_back({d.allocateSectionId(), "init", sct::SctScriptSectionContent{{{init, 12}}}});
            d.sections.push_back({d.allocateSectionId(), "loop", sct::SctScriptSectionContent{{{loop, 12}}}});
            sct::SctScriptSectionContent code;
            if (second) {
                code.instructions = {
                    {first, 0, false, {}, {{0, expr("BitVar[121]")}, {1, sct::SctInstructionReference{end}}}},
                    {middle, 125},
                    {end, 12}};
            } else {
                auto text = d.allocateSupplementaryTextId();
                d.supplementaryText.push_back(
                    {text, sct::SctTextKind::PlainString, sct::SctPlainText{"b.sct"}});
                code.instructions = {{first, 17, false, {}, {{0, expr("121")}}},
                                     {middle, 43, false, {}, {{0, sct::SctSupplementaryTextReference{text}}}},
                                     {end, 12}};
            }
            d.sections.push_back({d.allocateSectionId(), "scene", code});
            SctAuthoringBaseline base{
                {},
                {Sha256Digest{{}}},
                {AssetLocator::fromRelativePath(second ? "field/b.sct" : "field/a.sct").value(),
                 0,
                 {Sha256Digest{{}}}},
                {second ? "b44499b5-dcf0-449a-9815-5bdf558eface" : "a44499b5-dcf0-449a-9815-5bdf558eface"},
                sct::SctKnownTextConvention::ShiftJisByte7F};
            auto loaded = builder.appendImported(d, base, {});
            if (!loaded)
                throw std::runtime_error(messages(loaded.diagnostics()));
            if (second)
                b = loaded.value();
            else
                a = loaded.value();
        }
        auto built = builder.finish();
        if (!built)
            throw std::runtime_error(messages(built.diagnostics()));
        auto pa = built.value()->find(a), pb = built.value()->find(b);
        auto sa = built.value()->find(pa->sections.back())->body,
             sb = built.value()->find(pb->sections.back())->body;
        auto adopted = service->adopt({}, std::move(built).takeValue()).get();
        if (!adopted)
            throw std::runtime_error(messages(adopted.diagnostics()));
        revision = adopted.value().current;
        if (!organize)
            return;
        auto modules = service
                           ->edit({revision,
                                   {ir::SemanticOperation{ir::PutMetadata{ir::Module{{}, a, "A", {sa}}}},
                                    ir::SemanticOperation{ir::PutMetadata{ir::Module{{}, b, "B", {sb}}}}}})
                           .get();
        if (!modules)
            throw std::runtime_error(messages(modules.diagnostics()));
        revision = modules.value().status.current;
        auto catalog = backend.storyCatalog(revision).value();
        ma = catalog.modules[0].id;
        mb = catalog.modules[1].id;
    }
    auto edit(ed::StoryEdit edit) {
        auto result = backend.execute(revision, std::move(edit));
        if (!result)
            throw std::runtime_error(messages(result.diagnostics()));
        revision = result.value().status.current;
        return result.value();
    }
    ~StoryFixture() { service->shutdown().get(); }
};
TEST(StoryFeatureTest, ScopedMeaningsNarrativeAndItemDecisionsShareHistoryWithoutRebuildingFlow) {
    StoryFixture f;
    auto empty = f.backend.story(f.revision, {});
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty.value().effects.empty());
    EXPECT_EQ(empty.value().modules.size(), 2u);
    auto observed = f.backend.story(f.revision, {true, false, true});
    ASSERT_TRUE(observed);
    EXPECT_TRUE(std::ranges::any_of(observed.value().links, [&](auto &l) {
        return l.source == f.ma && l.destination == ed::Subject(f.mb);
    }));
    f.edit(ed::OrganizeStory{});
    auto cached = f.service->summarize(f.revision, f.ma).get().value().summary->build;
    ed::Meaning m;
    m.variable = {SctVariableKind::Bit, 121};
    m.name = "Gate opened";
    m.storyState = true;
    f.edit(ed::WriteMeaning{m});
    EXPECT_EQ(f.service->summarize(f.revision, f.ma).get().value().summary->build, cached);
    auto story = f.backend.story(f.revision, {});
    ASSERT_TRUE(story);
    EXPECT_FALSE(story.value().effects.empty());
    auto interactions = f.backend.interaction(f.revision, f.b);
    ASSERT_TRUE(interactions);
    ASSERT_FALSE(interactions.value().conditions.empty());
    EXPECT_NE(interactions.value().conditions.front().friendly.find("Gate opened"), std::string::npos);
    m.script = f.b;
    m.name = "Door unlocked here";
    f.edit(ed::WriteMeaning{m});
    EXPECT_NE(
        f.backend.interaction(f.revision, f.b).value().conditions.front().friendly.find("Door unlocked here"),
        std::string::npos);
    f.edit(ed::WriteNarrative{{f.ma, f.mb}});
    EXPECT_EQ(f.service->summarize(f.revision, f.ma).get().value().summary->build, cached);
    auto subject = interactions.value().conditions.front().subject;
    f.edit(ed::DecideInterpretation{{subject,
                                     0,
                                     ed::InterpretationDecision::Accepted,
                                     "User describes this gate",
                                     {{SctEvidenceKind::Inference, "Comparison of bit 121"}}}});
    auto cp = f.service->checkpoint(f.revision).get();
    ASSERT_TRUE(cp);
    auto encoded = ir::ProgramPersistence::encode(*cp.value().program);
    ASSERT_TRUE(encoded);
    auto decoded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(decoded) << messages(decoded.diagnostics());
    EXPECT_EQ(decoded.value().program->metadata().narrative, (std::vector{f.ma, f.mb}));
    EXPECT_EQ(decoded.value().program->metadata().interpretations.size(), 1u);
    auto undo = f.backend.history(f.revision, false);
    ASSERT_TRUE(undo);
    f.revision = undo.value().status.current;
    EXPECT_TRUE(f.backend.explanation(f.revision, subject).value().decisions.empty());
    auto redo = f.backend.history(f.revision, true);
    ASSERT_TRUE(redo);
    f.revision = redo.value().status.current;
    EXPECT_EQ(f.backend.explanation(f.revision, subject).value().decisions.size(), 1u);
}
TEST(StoryFeatureTest, WholeProjectOrganizationIsExplicitUndoableAndPersistent) {
    StoryFixture f(false);
    auto before = f.service->checkpoint(f.revision).get().value();
    auto bytes = ir::ProgramPersistence::encode(*before.program).value();
    std::vector<SctLoadingProgress> progress;
    ed::StoryRequest request{true};
    request.progress = [&](const auto &p) { progress.push_back(p); };
    auto closed = f.backend.story(f.revision, request);
    ASSERT_TRUE(closed);
    EXPECT_FALSE(closed.value().enabled);
    EXPECT_TRUE(closed.value().candidates.empty());
    EXPECT_TRUE(closed.value().effects.empty());
    EXPECT_TRUE(progress.empty()); // No discovery/summarization before opt-in.
    request.analyze = true;
    auto review = f.backend.story(f.revision, request);
    ASSERT_TRUE(review) << messages(review.diagnostics());
    ASSERT_EQ(review.value().scripts.size(), 2u);
    ASSERT_EQ(review.value().candidates.size(), 2u); // init/loop are not proposed as cutscenes.
    EXPECT_NE(review.value().candidates[0].script, review.value().candidates[1].script);
    EXPECT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().completed, 2u);
    EXPECT_EQ(
        ir::ProgramPersistence::encode(*f.service->checkpoint(f.revision).get().value().program).value(),
        bytes);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(f.backend.story(f.revision, request, stop.get_token()));
    auto revision = f.revision;
    f.edit(ed::OrganizeStory{review.value().candidates});
    EXPECT_FALSE(f.backend.execute(revision, ed::StoryEdit{ed::OrganizeStory{review.value().candidates}}));
    auto enabled = f.backend.story(f.revision, {true});
    ASSERT_TRUE(enabled);
    EXPECT_TRUE(enabled.value().enabled);
    EXPECT_TRUE(enabled.value().candidates.empty());
    ASSERT_EQ(enabled.value().modules.size(), 2u);
    EXPECT_EQ(enabled.value().unorganizedSections, 0u);
    EXPECT_TRUE(std::ranges::any_of(enabled.value().links, [&](const auto &link) {
        return link.source == enabled.value().modules[0].id &&
               link.destination == ed::Subject(enabled.value().modules[1].id);
    }));
    auto checkpoint = f.service->checkpoint(f.revision).get().value();
    auto encoded = ir::ProgramPersistence::encode(*checkpoint.program);
    ASSERT_TRUE(encoded);
    auto decoded = ir::ProgramPersistence::decode(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded.value().program->metadata().storyEnabled);
    EXPECT_EQ(decoded.value().program->metadata().modules.size(), 2u);
    // Organization does not rewrite instructions or create a second execution representation.
    EXPECT_EQ(checkpoint.program->statistics().actions, before.program->statistics().actions);
    auto undo = f.backend.history(f.revision, false);
    ASSERT_TRUE(undo);
    f.revision = undo.value().status.current;
    closed = f.backend.story(f.revision, {});
    ASSERT_TRUE(closed);
    EXPECT_FALSE(closed.value().enabled);
    EXPECT_TRUE(closed.value().modules.empty());
    auto redo = f.backend.history(f.revision, true);
    ASSERT_TRUE(redo);
    f.revision = redo.value().status.current;
    f.edit(ed::OrganizeStory{{}, false});
    closed = f.backend.story(f.revision, {});
    ASSERT_TRUE(closed);
    EXPECT_FALSE(closed.value().enabled);
    EXPECT_EQ(closed.value().modules.size(), 2u);
    EXPECT_TRUE(closed.value().effects.empty());
    f.edit(ed::OrganizeStory{});
    auto again = f.backend.story(f.revision, request);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again.value().candidates.empty()); // Existing organization is not rediscovered or replaced.
}
TEST(StoryFeatureTest, OutputClosureRejectsStaleOrCancelledReviewAndPublishesOneSet) {
    StoryFixture f;
    auto review = f.backend.reviewOutput(f.revision, {f.a});
    ASSERT_TRUE(review) << messages(review.diagnostics());
    ASSERT_EQ(review.value().items.size(), 2u);
    auto parent = std::filesystem::temp_directory_path() / ("salsa-g5-" + generateSctAuthoringUuid().value());
    std::filesystem::create_directory(parent);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{parent};
    ed::OutputRequest request{
        review.value(),
        sct::SctDocumentExportOptions{sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding},
        parent / "output"};
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(f.backend.publishOutput(request, stop.get_token()));
    EXPECT_FALSE(std::filesystem::exists(request.directory));
    f.edit(ed::WriteNarrative{{f.mb, f.ma}});
    EXPECT_FALSE(f.backend.publishOutput(request));
    EXPECT_FALSE(std::filesystem::exists(request.directory));
    request.review = f.backend.reviewOutput(f.revision, {f.a}).value();
    auto published = f.backend.publishOutput(request);
    ASSERT_TRUE(published) << messages(published.diagnostics());
    ASSERT_EQ(published.value().files.size(), 2u);
    for (auto &path : published.value().files)
        EXPECT_GT(std::filesystem::file_size(path), 0u);
    EXPECT_FALSE(f.backend.publishOutput(request));
}
} // namespace
