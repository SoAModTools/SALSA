#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Authoring/SctPublicationSet.h"
#include "SalsaCore/Authoring/SctRouteAuthoring.h"
#include "SctRouteTestFixture.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
namespace {
using namespace salsa::core;
namespace sct = spice::sct;
namespace fs = std::filesystem;
using namespace salsa::tests::route;
SctAuthoringMaterializationResult outputs(const SctAuthoringState &s) {
    std::vector<SctScriptId> ids;
    for (const auto &x : s.project.scripts)
        ids.push_back(x.id);
    auto r = SctAuthoringMaterializer::materialize(
        {std::make_shared<const SctAuthoringProject>(s.project), s.programs, ids});
    if (!r.succeeded()) {
        for (const auto &d : r.diagnostics)
            std::cerr << d.message << "\n";
        for (const auto &script : r.scripts) {
            for (const auto &d : script.infrastructureDiagnostics)
                std::cerr << d.message << "\n";
            for (const auto &d : script.diagnostics)
                if (d.severity == DiagnosticSeverity::Error)
                    std::cerr << d.message << "\n";
        }
    }
    return r;
}
struct Temp {
    fs::path path = fs::temp_directory_path() / ("salsa-routes-" + take(generateSctAuthoringUuid()));
    Temp() {
        fs::create_directories(path);
    }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
SctPublicationSetRequest publication(const SctAuthoringState &s, const fs::path &root) {
    SctPublicationSetRequest r{
        std::make_shared<const SctAuthoringState>(s), s.project.revision, root, {s.project.scripts[0].id}};
    for (const auto &script : s.project.scripts)
        r.targets.push_back({script.id,
                             s.project.find(script.baseline)->source.locator.path(),
                             {sct::SctPlatform::Dreamcast, sct::kSctShiftJisByte7FEncoding,
                              sct::SctDocumentOutputByteOrder::LittleEndian, sct::SctDocumentOutputWrapper::Raw}});
    return r;
}
TEST(SctRouteAuthoringTest, BothEndpointsUndoReconnectCodecAndMappings) {
    auto f = fixture();
    auto before = outputs(f.state);
    ASSERT_TRUE(before.succeeded());
    auto session = take(SctAuthoringSession::open(f.state));
    auto command = session->execute(session->state().project.revision, "Connect",
                                    [&](const auto &s) { return SctRouteAuthoring::connect(s, f.request); });
    ASSERT_TRUE(command) << command.diagnostics().front().message;
    auto after = outputs(session->state());
    ASSERT_TRUE(after.succeeded());
    EXPECT_NE(before.scripts[0].prepared->bytes, after.scripts[0].prepared->bytes);
    EXPECT_NE(before.scripts[1].prepared->bytes, after.scripts[1].prepared->bytes);
    auto c = session->state().project.connections.back();
    for (const auto &out : after.scripts)
        EXPECT_TRUE(std::ranges::any_of(out.prepared->realization.records,
                                        [&](const auto &r) { return r.entity == SctAuthoringEntityId{c.id}; }));
    auto encoded = take(SctAuthoringCodec::encode(session->state().project));
    auto decoded = take(SctAuthoringCodec::decode(encoded));
    EXPECT_EQ(take(SctAuthoringCodec::encode(decoded)), encoded);
    ASSERT_TRUE(session->undo());
    EXPECT_EQ(outputs(session->state()).scripts[0].prepared->bytes, before.scripts[0].prepared->bytes);
    ASSERT_TRUE(session->redo());
    f.request.replace = c.id;
    f.request.response = f.alternative;
    auto reconnected = session->execute(session->state().project.revision, "Reconnect",
                                        [&](const auto &s) { return SctRouteAuthoring::connect(s, f.request); });
    ASSERT_TRUE(reconnected) << reconnected.diagnostics().front().message;
    EXPECT_EQ(session->state().project.connections.back().id, c.id);
    EXPECT_EQ(session->state().project.connections.back().route->sourceAction, c.route->sourceAction);
    auto malformed = nlohmann::json::parse(encoded);
    malformed["connections"][0]["route"]["sourceAction"] = "999999";
    EXPECT_FALSE(SctAuthoringCodec::decode(malformed.dump()));
    EXPECT_FALSE(session->execute(f.state.project.revision, "Stale",
                                  [](const auto &s) { return Result<SctAuthoringState>::success(s); }));
}
TEST(SctRouteAuthoringTest, SwitchCasesAndUnsupportedDrafts) {
    auto f = fixture(true);
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    ASSERT_TRUE(outputs(s).succeeded());
    s.project.connections[0].route->kind = SctTransitionKind::Battle;
    s.project.connections[0].route->unresolved = SctUnresolvedBinding{"10000", "Producer unqualified", {}};
    EXPECT_FALSE(hasErrors(s.project.validate()));
    EXPECT_TRUE(SctAuthoringCodec::decode(take(SctAuthoringCodec::encode(s.project))));
    EXPECT_FALSE(outputs(s).succeeded());
    auto invalid = f.request;
    invalid.arrivalCase = 1;
    EXPECT_FALSE(SctRouteAuthoring::connect(f.state, invalid));
}
TEST(SctRouteAuthoringTest, LowLevelEditsRetainDraftAndNoHalfChangesOnFailure) {
    auto f = fixture();
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    auto source = s.project.scripts[0].id;
    auto working = take(SctAuthoringMaterializer::workingState(s.project, s.programs, source));
    auto d = std::make_shared<sct::SctDocument>(*working.document);
    for (auto &t : d->supplementaryText)
        if (auto *text = std::get_if<sct::SctPlainText>(&t.value); text && text->utf8 == "me002b.sct")
            text->utf8 = "me033b.sct";
    working.document = d;
    s.project = take(SctAuthoringMaterializer::replaceWorkingState(s.project, s.programs, source, working));
    ASSERT_TRUE(s.project.connections[0].route->unresolved);
    EXPECT_FALSE(outputs(s).succeeded());
    EXPECT_TRUE(SctAuthoringCodec::decode(take(SctAuthoringCodec::encode(s.project))));
    auto invalid = f.request;
    invalid.response = invalid.source;
    auto encoded = take(SctAuthoringCodec::encode(f.state.project));
    EXPECT_FALSE(SctRouteAuthoring::connect(f.state, invalid));
    EXPECT_EQ(encoded, take(SctAuthoringCodec::encode(f.state.project)));
}
TEST(SctPublicationSetTest, ClosureCapturedRevisionHashesAndDrift) {
    Temp temp;
    auto f = fixture();
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    auto request = publication(s, temp.path);
    auto incomplete = request;
    incomplete.targets.pop_back();
    EXPECT_FALSE(SctPublicationSet::prepare(incomplete));
    auto stale = request;
    ++stale.expectedRevision.value;
    EXPECT_FALSE(SctPublicationSet::prepare(stale));
    auto prepared = take(SctPublicationSet::prepare(request));
    EXPECT_FALSE(fs::exists(temp.path / "me002a.sct"));
    auto result = SctPublicationSet::install(prepared);
    ASSERT_TRUE(result.receipt);
    EXPECT_TRUE(SctPublicationSet::verifyReceipt(*result.receipt));
    auto partial = *result.receipt;
    partial.outputs.pop_back();
    EXPECT_FALSE(SctPublicationSet::verifyReceipt(partial));
    auto altered = *result.receipt;
    altered.outputs[0].target.options.wrapper = sct::SctDocumentOutputWrapper::Aklz;
    EXPECT_FALSE(SctPublicationSet::verifyReceipt(altered));
    auto replacement = take(SctPublicationSet::prepare(request));
    std::ofstream(temp.path / "me002b.sct", std::ios::binary) << "external change";
    EXPECT_FALSE(SctPublicationSet::verifyReceipt(*result.receipt));
    EXPECT_FALSE(SctPublicationSet::install(replacement).receipt);
    EXPECT_EQ(result.receipt->revision, s.project.revision);
}
TEST(SctPublicationSetTest, InterruptedReplacementRecoveryAndBlockedRecovery) {
    for (size_t interrupted = 1; interrupted <= 3; ++interrupted) {
        Temp temp;
        auto f = fixture();
        auto s = take(SctRouteAuthoring::connect(f.state, f.request));
        auto request = publication(s, temp.path);
        auto prepared = take(SctPublicationSet::prepare(request));
        auto result = SctPublicationSet::install(prepared, {}, {{[&](size_t n) { return n != interrupted; }}});
        EXPECT_FALSE(result.receipt);
        auto recovered = SctPublicationSet::recover(temp.path);
        ASSERT_FALSE(recovered.empty());
        for (const auto &r : recovered)
            EXPECT_TRUE(r.succeeded());
        EXPECT_TRUE(SctPublicationSet::verifyReceipt(prepared.review()));
    }
    Temp temp;
    auto f = fixture();
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    auto prepared = take(SctPublicationSet::prepare(publication(s, temp.path)));
    EXPECT_FALSE(SctPublicationSet::install(prepared, {}, {{[](size_t) { return false; }}}).receipt);
    std::ofstream(temp.path / "me002a.sct", std::ios::binary) << "external";
    auto blocked = SctPublicationSet::recover(temp.path);
    ASSERT_FALSE(blocked.empty());
    EXPECT_EQ(blocked[0].status, WorkspaceTransactionStatus::RecoveryBlocked);
}
TEST(SctPublicationSetTest, CollisionConfinementCancellationAndProfileFailuresBeforeInstall) {
    Temp temp;
    auto f = fixture();
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    auto request = publication(s, temp.path);
    auto bad = request;
    bad.targets[1].relativePath = bad.targets[0].relativePath;
    EXPECT_FALSE(SctPublicationSet::prepare(bad));
    bad = request;
    bad.targets[0].relativePath = "../me002a.sct";
    EXPECT_FALSE(SctPublicationSet::prepare(bad));
    bad = request;
    bad.targets[1].options.platform = sct::SctPlatform::GameCube;
    EXPECT_FALSE(SctPublicationSet::prepare(bad));
    auto prepared = take(SctPublicationSet::prepare(request));
    std::stop_source stop;
    stop.request_stop();
    EXPECT_TRUE(SctPublicationSet::install(prepared, stop.get_token()).cancelled);
    EXPECT_FALSE(fs::exists(temp.path / "me002a.sct"));
}

TEST(SctRouteAuthoringTest, InsertIsOneCommandAndDisabledTargetsBecomeDrafts) {
    auto f = fixture();
    auto before = outputs(f.state);
    auto session = take(SctAuthoringSession::open(f.state));
    f.request.insertLoadAfterSource = true;
    auto result = session->execute(session->state().project.revision, "Insert load and bind arrival",
                                   [&](const auto &s) { return SctRouteAuthoring::connect(s, f.request); });
    ASSERT_TRUE(result);
    EXPECT_EQ(session->state().project.connections.size(), 1u);
    EXPECT_GT(session->state().project.nextEntityId, f.state.project.nextEntityId);
    EXPECT_TRUE(outputs(session->state()).succeeded());
    ASSERT_TRUE(session->undo());
    EXPECT_EQ(outputs(session->state()).scripts[0].prepared->bytes, before.scripts[0].prepared->bytes);
    ASSERT_TRUE(session->redo());
    auto disabled = take(SctEntrypointAuthoring::setEnabled(session->state(), f.request.arrival, false));
    EXPECT_TRUE(disabled.project.connections[0].route->unresolved);
    EXPECT_FALSE(outputs(disabled).succeeded());
    EXPECT_TRUE(SctAuthoringCodec::decode(take(SctAuthoringCodec::encode(disabled.project))));
}
TEST(SctRouteAuthoringTest, SharedFilenameAndDeliberateConvergencePreserveUnrelatedLoad) {
    auto f = fixture();
    auto source = f.state.project.scripts[0].id;
    auto port = *f.state.project.find(f.request.source);
    auto physical = f.state.project.find(*port.action)->instruction;
    auto working = take(SctAuthoringMaterializer::workingState(f.state.project, f.state.programs, source));
    auto doc = std::make_shared<sct::SctDocument>(*working.document);
    auto copy = *sct::SctDocumentIndex::build(*doc).find(*doc, physical);
    copy.id = doc->allocateInstructionId();
    for (auto &section : doc->sections)
        if (auto *body = std::get_if<sct::SctScriptSectionContent>(&section.content))
            if (std::ranges::any_of(body->instructions, [&](const auto &i) { return i.id == physical; }))
                body->instructions.insert(body->instructions.end() - 1, copy);
    working.document = doc;
    f.state.project =
        take(SctAuthoringMaterializer::replaceWorkingState(f.state.project, f.state.programs, source, working));
    SctActionId action;
    for (const auto &seq : f.state.project.sequences)
        for (const auto &a : seq.actions)
            if (a.instruction == copy.id && seq.script == source)
                action = a.id;
    auto newPort = take(f.state.project.allocate<SctPortId>());
    f.state.project.ports.push_back({newPort, port.owner, "Second load", action, port.sequence});
    auto connected = take(SctRouteAuthoring::connect(f.state, f.request));
    auto changed = take(SctAuthoringMaterializer::workingState(connected.project, connected.programs, source));
    EXPECT_EQ(take(SctProgramStateCodec::instructionFingerprint(*doc, copy.id)),
              take(SctProgramStateCodec::instructionFingerprint(*changed.document, copy.id)));
    EXPECT_TRUE(outputs(connected).succeeded());
    auto second = f.request;
    second.source = newPort;
    auto converged = take(SctRouteAuthoring::connect(connected, second));
    EXPECT_EQ(converged.project.connections.size(), 2u);
    EXPECT_TRUE(outputs(converged).succeeded());
    EXPECT_FALSE(SctRouteAuthoring::connect(converged, second)); // duplicate physical source owner
    auto unsupported = f.request;
    unsupported.discriminatorBit = 7;
    unsupported.discriminatorValue = true;
    EXPECT_FALSE(SctRouteAuthoring::connect(f.state, unsupported));
}
TEST(SctPublicationSetTest, CancellationRollsBackAndChecksEveryReplacementForDrift) {
    Temp temp;
    auto f = fixture();
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    auto prepared = take(SctPublicationSet::prepare(publication(s, temp.path)));
    std::stop_source stop;
    auto cancelled = SctPublicationSet::install(prepared, stop.get_token(), {{[&](size_t) {
                                                    stop.request_stop();
                                                    return false;
                                                }}});
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_EQ(cancelled.status, WorkspaceTransactionStatus::RolledBack);
    EXPECT_FALSE(cancelled.receipt);
    EXPECT_FALSE(fs::exists(temp.path / "me002a.sct"));
    EXPECT_FALSE(fs::exists(temp.path / "me002b.sct"));
    auto fresh = take(SctPublicationSet::prepare(publication(s, temp.path)));
    auto failed = SctPublicationSet::install(fresh, {}, {{[&](size_t n) {
                                                 if (n == 1)
                                                     std::ofstream(temp.path / "me002b.sct", std::ios::binary)
                                                         << "external";
                                                 return true;
                                             }}});
    EXPECT_FALSE(failed.receipt);
    EXPECT_EQ(failed.status, WorkspaceTransactionStatus::RecoveryBlocked);
    std::ifstream file(temp.path / "me002b.sct");
    std::string text;
    file >> text;
    EXPECT_EQ(text, "external");
}
TEST(SctPublicationSetTest, EncodingFailureProducesNoFilesAndSourceReplacementChecksHash) {
    Temp temp;
    auto f = fixture();
    auto s = take(SctRouteAuthoring::connect(f.state, f.request));
    auto request = publication(s, temp.path);
    request.targets[0].originalSource = temp.path / request.targets[0].relativePath;
    EXPECT_FALSE(SctPublicationSet::prepare(request));
    request.targets[0].allowSourceReplacement = true;
    EXPECT_FALSE(SctPublicationSet::prepare(request));
    auto script = s.project.scripts[1].id;
    auto working = take(SctAuthoringMaterializer::workingState(s.project, s.programs, script));
    auto doc = std::make_shared<sct::SctDocument>(*working.document);
    auto text = doc->allocateSupplementaryTextId();
    doc->supplementaryText.push_back(
        {text, sct::SctTextKind::SctString, sct::SctMessage{{}, {{{sct::SctTextChunk{"\xF0\x9F\x98\x80"}}}}}});
    doc->sections.push_back(
        {doc->allocateSectionId(), "debug",
         sct::SctScriptSectionContent{
             {{doc->allocateInstructionId(), 24, false, {}, {{0, sct::SctSupplementaryTextReference{text}}}},
              {doc->allocateInstructionId(), 12}}}});
    working.document = doc;
    s.project = take(SctAuthoringMaterializer::replaceWorkingState(s.project, s.programs, script, working));
    EXPECT_FALSE(SctPublicationSet::prepare(publication(s, temp.path)));
    EXPECT_FALSE(fs::exists(temp.path / "me002a.sct"));
    EXPECT_FALSE(fs::exists(temp.path / "me002b.sct"));
}

TEST(SctRouteAuthoringTest, DistinctResponsesRequireExplicitDisjointProducersAndRequirements) {
    auto f = fixture();
    auto src = f.state.project.scripts[0].id, dst = f.state.project.scripts[1].id;
    auto state = f.state;
    std::array<SctRouteRequest, 2> requests;
    for (unsigned value = 0; value < 2; ++value) {
        auto sw = take(SctAuthoringMaterializer::workingState(state.project, state.programs, src));
        auto sd = std::make_shared<sct::SctDocument>(*sw.document);
        auto text = sd->supplementaryText.front().id;
        auto section = sd->allocateSectionId();
        sd->sections.push_back(
            {section, "source" + std::to_string(value),
             sct::SctScriptSectionContent{
                 {{sd->allocateInstructionId(),
                   static_cast<std::uint16_t>(value ? 17 : 18),
                   false,
                   {},
                   {{0, expr("7")}}},
                  {sd->allocateInstructionId(), 43, false, {}, {{0, sct::SctSupplementaryTextReference{text}}}},
                  {sd->allocateInstructionId(), 12}}}});
        sw.document = sd;
        state.project = take(SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, src, sw));
        state = take(SctModuleAuthoring::extract(state, src, {section}, "Explicit producer"));
        auto ports = state.project.ports.size();
        requests[value].source = state.project.ports[ports - 2].id;
        requests[value].effects = {*state.project.ports[ports - 3].action};
        requests[value].discriminatorBit = 7;
        requests[value].discriminatorValue = value != 0;
        requests[value].response = value ? f.alternative : f.request.response;
        auto dw = take(SctAuthoringMaterializer::workingState(state.project, state.programs, dst));
        auto dd = std::make_shared<sct::SctDocument>(*dw.document);
        auto guard = dd->allocateInstructionId(), arrival = dd->allocateInstructionId(),
             call = dd->allocateInstructionId(), ret = dd->allocateInstructionId();
        auto ds = dd->allocateSectionId();
        auto response = state.project.find(*state.project.find(requests[value].response)->action)->instruction;
        dd->sections.push_back(
            {ds, "arrival" + std::to_string(value),
             sct::SctScriptSectionContent{
                 {{guard,
                   0,
                   false,
                   {},
                   {{0, expr("BitVar[7] == " + std::to_string(value))}, {1, sct::SctInstructionReference{ret}}}},
                  {arrival,
                   0,
                   false,
                   {},
                   {{0, expr("Low16IntVar[15] == 990")}, {1, sct::SctInstructionReference{ret}}}},
                  {call, 11, false, {}, {{0, sct::SctInstructionReference{response}}}},
                  {ret, 12}}}});
        dw.document = dd;
        state.project = take(SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, dst, dw));
        state = take(SctSequenceAuthoring::promoteSequence(state, dst, ds, "Arrival " + std::to_string(value)));
        auto seq = state.project.sequences.back();
        SctEntrypointBinding binding;
        binding.kind = SctEntrypointKind::Arrival;
        binding.sequence = seq.id;
        binding.predicate = sct::SctParameterSite{arrival, {0, {}}};
        binding.responseSite = sct::SctParameterSite{call, {0, {}}};
        binding.response = requests[value].response;
        state = take(SctEntrypointAuthoring::bind(state, dst, "Discriminated arrival", binding, {}));
        requests[value].arrival = state.project.entrypoints.back().id;
        requests[value].requirements = {seq.actions[0].id};
    }
    state = take(SctRouteAuthoring::connect(state, requests[0]));
    auto ambiguous = requests[1];
    ambiguous.discriminatorBit.reset();
    EXPECT_FALSE(SctRouteAuthoring::connect(state, ambiguous));
    state = take(SctRouteAuthoring::connect(state, requests[1]));
    ASSERT_EQ(state.project.connections.size(), 2u);
    EXPECT_TRUE(outputs(state).succeeded());
    EXPECT_FALSE(hasErrors(take(SctAuthoringCodec::decode(take(SctAuthoringCodec::encode(state.project)))).validate()));
}
TEST(SctRouteAuthoringTest, UnqualifiedIntentIsAHistoryCommandWithoutExecutionEdits) {
    auto f = fixture();
    auto original =
        take(SctAuthoringMaterializer::workingState(f.state.project, f.state.programs, f.state.project.scripts[0].id));
    auto draft = take(SctRouteAuthoring::draft(f.state, f.request, SctTransitionKind::ShipBattleExit,
                                               "40000 return observation", "Producer not qualified"));
    EXPECT_TRUE(draft.project.connections[0].route->unresolved);
    EXPECT_TRUE(SctAuthoringCodec::decode(take(SctAuthoringCodec::encode(draft.project))));
    auto current =
        take(SctAuthoringMaterializer::workingState(draft.project, draft.programs, draft.project.scripts[0].id));
    EXPECT_EQ(take(SctProgramStateCodec::encode(original)), take(SctProgramStateCodec::encode(current)));
    EXPECT_FALSE(outputs(draft).succeeded());
}
} // namespace
