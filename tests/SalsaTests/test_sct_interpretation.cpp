#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
namespace {
using namespace salsa::core;
namespace sct = spice::sct;
sct::SctCanonicalExpression expr(std::string s) {
    auto r = SctExpressionLanguage::parse(std::move(s));
    if (!r.succeeded())
        throw std::runtime_error("Bad fixture expression");
    return *r.expression;
}
SctAuthoringState fixture() {
    SctAuthoringState state{SctAuthoringProject::create().value()};
    for (unsigned n = 0; n < 2; ++n) {
        sct::SctDocument d;
        for (const auto name : {"init", "loop"})
            d.sections.push_back(
                {d.allocateSectionId(), name, sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
        const auto set = d.allocateInstructionId();
        d.sections.push_back({d.allocateSectionId(), n ? "me002ba26" : "me002aa25",
                              sct::SctScriptSectionContent{{{set, 17, false, {}, {{0, expr(n ? "22" : "21")}}},
                                                            {d.allocateInstructionId(), 12}}}});
        auto condition = d.allocateInstructionId(), ret = d.allocateInstructionId();
        d.sections.push_back(
            {d.allocateSectionId(), "M00001",
             sct::SctScriptSectionContent{{{condition,
                                            0,
                                            false,
                                            {},
                                            {{0, expr("BitVar[21] == 1 && BitVar[22] == 0 && ByteVar[42] == 3")},
                                             {1, sct::SctInstructionReference{ret}}}},
                                           {ret, 12}}}});
        auto output = sct::SctDocumentExporter::exportDocument(
            d, sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast,
                                             sct::kSctShiftJisByte7FEncoding,
                                             sct::SctDocumentOutputByteOrder::LittleEndian,
                                             sct::SctDocumentOutputWrapper::Raw,
                                             sct::SctOpaquePreservationPolicy::RequirePreservation,
                                             {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
        if (!output.success)
            throw std::runtime_error("Synthetic export failed");
        auto span = std::as_bytes(std::span(output.bytes));
        std::vector<std::byte> bytes(span.begin(), span.end());
        auto hash = sha256(bytes).value();
        SctAuthoringImportRequest request{
            {{AssetLocator::fromRelativePath(n ? "me002b.sct" : "me002a.sct").value(), bytes.size(), {hash}}, bytes},
            {GamePlatform::Dreamcast, GameRegion::Europe, {hash}},
            {GamePlatform::Dreamcast, false},
            {},
            n ? "me002b" : "me002a"};
        auto imported = SctAuthoringImporter::import(state.project, state.project.revision, request);
        if (!imported)
            throw std::runtime_error(imported.diagnostics().front().message);
        state.project = imported.value().project;
        state.programs.push_back(imported.value().program);
    }
    return state;
}
std::vector<std::uint8_t> output(const SctAuthoringState &s) {
    auto r = SctAuthoringMaterializer::materialize(
        {std::make_shared<const SctAuthoringProject>(s.project), s.programs, {s.project.scripts.front().id}});
    if (!r.succeeded())
        throw std::runtime_error("Materialization failed");
    return r.scripts.front().prepared->bytes;
}
TEST(SctInterpretationTest, CrossScriptDependenciesAndEntryEvidenceAreQualified) {
    auto s = fixture();
    auto a = SctInterpretation::analyze(s);
    EXPECT_EQ(a.suggestions.size(), 2u);
    EXPECT_EQ(a.dependencies.size(), 4u);
    ASSERT_FALSE(a.suggestions.empty());
    EXPECT_NE(a.suggestions.front().qualification.find("not runtime"), std::string::npos);
    EXPECT_TRUE(SctInterpretation::fresh(s, a.suggestions.front()));
    EXPECT_TRUE(std::ranges::any_of(a.dependencies, [](const auto &d) { return d.writer.script != d.reader.script; }));
}
TEST(SctInterpretationTest, DecisionsDeduplicateAndExplicitNamesSurviveRescan) {
    auto s = fixture();
    auto before = output(s);
    auto a = SctInterpretation::analyze(s);
    s = SctInterpretation::record(s, a).value();
    auto id = s.project.explanations.front().id;
    s = SctInterpretation::decide(s, id, SctInterpretationDecision::Accepted).value();
    EXPECT_EQ(s.project.meanings.size(), 1u);
    auto count = s.project.explanations.size();
    s = SctInterpretation::record(s, SctInterpretation::analyze(s)).value();
    EXPECT_EQ(s.project.explanations.size(), count);
    auto t = s.project.meanings.front().target;
    s = SctInterpretation::setName(s, t, "Captain introduced").value();
    s = SctInterpretation::record(s, SctInterpretation::analyze(s)).value();
    EXPECT_EQ(SctInterpretation::label(s, t, "raw"), "Captain introduced");
    EXPECT_EQ(output(s), before);
    auto text =
        SctInterpretation::condition(s, s.project.scripts.back().id, expr("BitVar[21] == 0 || ByteVar[42] == 3"));
    EXPECT_NE(text.find("NOT {Captain introduced}"), std::string::npos);
    EXPECT_NE(text.find("ByteVar[42]"), std::string::npos);
}
TEST(SctInterpretationTest, ScriptAliasesDoNotRenameExecutionAndUndoPersists) {
    auto s = fixture();
    auto bytes = output(s);
    auto script = s.project.scripts.front().id;
    auto d = SctAuthoringMaterializer::workingState(s.project, s.programs, script).value().document;
    auto target = SctInterpretation::sectionTarget(s.project, script, d->sections.back().id);
    auto opened = SctAuthoringSession::open(s);
    ASSERT_TRUE(opened);
    auto &session = *opened.value();
    ASSERT_TRUE(session.execute(session.state().project.revision, "Name chest", [&](const auto &v) {
        return SctInterpretation::setName(v, target, "Treasure Chest 1");
    }));
    auto id = session.state().project.meanings.front().id;
    EXPECT_EQ(output(session.state()), bytes);
    ASSERT_TRUE(session.undo());
    EXPECT_TRUE(session.state().project.meanings.empty());
    ASSERT_TRUE(session.redo());
    EXPECT_EQ(session.state().project.meanings.front().id, id);
    auto encoded = SctAuthoringCodec::encode(session.state().project);
    ASSERT_TRUE(encoded);
    auto decoded = SctAuthoringCodec::decode(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(SctAuthoringCodec::encode(decoded.value()).value(), encoded.value());
    EXPECT_EQ(decoded.value().meanings.front().target, target);
    EXPECT_EQ(decoded.value().explanations.front().origin, SctEvidenceKind::UserAssertion);
}
TEST(SctInterpretationTest, ChangedEvidenceAndForeignAnalysisCannotBeAccepted) {
    auto s = fixture();
    auto a = SctInterpretation::analyze(s);
    s = SctInterpretation::record(s, a).value();
    auto id = s.project.explanations.front().id;
    auto site = *a.suggestions.front().subjects[1].site;
    auto script = a.suggestions.front().subjects[1].script;
    auto w = SctAuthoringMaterializer::workingState(s.project, s.programs, script).value();
    auto edited =
        SctSemanticOperationService::apply(*w.document, {{{SctReplaceParameterValueOperation{site, expr("25")}}}});
    ASSERT_TRUE(edited.succeeded());
    w.document = edited.document;
    s.project = SctAuthoringMaterializer::replaceWorkingState(s.project, s.programs, script, w).value();
    EXPECT_FALSE(SctInterpretation::fresh(s, s.project.explanations.front()));
    EXPECT_FALSE(SctInterpretation::decide(s, id, SctInterpretationDecision::Accepted));
    a.revision = {s.project.revision.value + 1};
    EXPECT_FALSE(SctInterpretation::record(s, a));
    a.revision = s.project.revision;
    a.project = SctAuthoringProject::create().value().id;
    EXPECT_FALSE(SctInterpretation::record(s, a));
}
TEST(SctInterpretationTest, ValueDefinitionsRespectScopeAndComparisons) {
    auto s = fixture();
    SctMeaningTarget t;
    t.variable = SctVariableKey{SctVariableKind::Byte, 42};
    t.value = 3;
    s = SctInterpretation::setName(s, t, "Preparing to depart").value();
    t.script = s.project.scripts.front().id;
    s = SctInterpretation::setName(s, t, "Local departure").value();
    auto e = expr("ByteVar[42] != 3 && BitVar[21] == 10");
    auto first = SctInterpretation::condition(s, t.script, e);
    EXPECT_NE(first.find("NOT {Local departure}"), std::string::npos);
    EXPECT_NE(first.find("BitVar[21] == 10"), std::string::npos);
    auto second = SctInterpretation::condition(s, s.project.scripts.back().id, e);
    EXPECT_NE(second.find("Preparing to depart"), std::string::npos);
    EXPECT_EQ(SctInterpretation::condition(s, t.script, e, false), SctExpressionLanguage::project(e).text);
}
TEST(SctInterpretationTest, InvalidMetadataRejectsDecodeWithoutPartialProject) {
    auto s = fixture();
    SctMeaningTarget t;
    t.variable = SctVariableKey{SctVariableKind::Bit, 21};
    t.value = 1;
    s = SctInterpretation::setName(s, t, "Scene entered").value();
    auto j = nlohmann::json::parse(SctAuthoringCodec::encode(s.project).value());
    j["meanings"][0]["target"]["value"] = "2";
    EXPECT_FALSE(SctAuthoringCodec::decode(j.dump()));
    j = nlohmann::json::parse(SctAuthoringCodec::encode(s.project).value());
    j["explanations"][0]["observations"] = nlohmann::json::array();
    EXPECT_FALSE(SctAuthoringCodec::decode(j.dump()));
    j = nlohmann::json::parse(SctAuthoringCodec::encode(s.project).value());
    j["meanings"].push_back(j["meanings"][0]);
    EXPECT_FALSE(SctAuthoringCodec::decode(j.dump()));
}
TEST(SctInterpretationTest, MultipleWritersAreDisclosedAndNewWritersInvalidateEvidence) {
    auto s = fixture();
    auto a = SctInterpretation::analyze(s);
    auto target = a.suggestions.front().subjects[2];
    auto script = s.project.scripts.back().id;
    auto w = SctAuthoringMaterializer::workingState(s.project, s.programs, script).value();
    auto d = std::make_shared<sct::SctDocument>(*w.document);
    auto &b = std::get<sct::SctScriptSectionContent>(d->sections.back().content);
    b.instructions.insert(b.instructions.begin(), {d->allocateInstructionId(), 18, false, {}, {{0, expr("21")}}});
    w.document = d;
    s.project = SctAuthoringMaterializer::replaceWorkingState(s.project, s.programs, script, w).value();
    EXPECT_FALSE(SctInterpretation::fresh(s, a.suggestions.front()));
    auto updated = SctInterpretation::analyze(s);
    EXPECT_NE(updated.suggestions.front().qualification.find("Multiple writers"), std::string::npos);
}

TEST(SctInterpretationTest, IntegralFloatBitOperandsAndComparisonsRetainTheirEncoding) {
    auto state=fixture();auto script=state.project.scripts.front().id;
    auto w=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value();
    auto doc=std::make_shared<sct::SctDocument>(*w.document);auto& body=std::get<sct::SctScriptSectionContent>(doc->sections[2].content);
    body.instructions[0].fixedParameters[0].value=expr("21.0f");
    // An explicit label prefix is a physical instruction, not a scene action.
    body.instructions.insert(body.instructions.begin(),{doc->allocateInstructionId(),9,false,{},{{0,sct::SctTerminatedWordSequenceValue{{0x1d}}}}});
    w.document=doc;state.project.contents[0].authoredState=w;
    auto a=SctInterpretation::analyze(state);ASSERT_EQ(a.suggestions.size(),2u);
    SctMeaningTarget t;t.variable=SctVariableKey{SctVariableKind::Bit,21};t.value=1;
    auto named=SctInterpretation::setName(state,t,"Scene entered");ASSERT_TRUE(named);state=named.value();
    const auto e=expr("BitVar[21] == 1.0f && BitVar[21] != 0.5f");
    auto text=SctInterpretation::condition(state,script,e);
    EXPECT_NE(text.find("{Scene entered}"),std::string::npos);
    EXPECT_NE(text.find("0.5f"),std::string::npos);
    EXPECT_EQ(SctInterpretation::condition(state,script,e,false),SctExpressionLanguage::project(e).text);
}

TEST(SctInterpretationTest, FriendlyRenderingDoesNotInterpretAliasTextOrLow16Access) {
    auto state=fixture();SctMeaningTarget value;value.variable=SctVariableKey{SctVariableKind::Bit,21};value.value=1;
    state=SctInterpretation::setName(state,value,"Check ByteVar[42]").value();
    SctMeaningTarget variable;variable.variable=SctVariableKey{SctVariableKind::Byte,42};state=SctInterpretation::setName(state,variable,"Harbor").value();
    variable.variable=SctVariableKey{SctVariableKind::Integer,15};state=SctInterpretation::setName(state,variable,"Arrival").value();
    auto text=SctInterpretation::condition(state,state.project.scripts.front().id,expr("BitVar[21] == 1 && Low16IntVar[15] == 20"));
    EXPECT_NE(text.find("{Check ByteVar[42]}"),std::string::npos);EXPECT_NE(text.find("Low16IntVar[15]"),std::string::npos);
}
} // namespace
