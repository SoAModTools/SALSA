#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
using namespace salsa::core;
namespace sct = spice::sct;
constexpr auto encoding = sct::kSctWindows1252Byte7FEncoding;
sct::SctTextValue message(std::string value) {
    return sct::SctMessage{{}, {{sct::SctTextChunk{std::move(value)}}}};
}
sct::SctCanonicalExpression expression(std::string value) {
    return *SctExpressionLanguage::parse(value).expression;
}
template <class T> T take(Result<T> result) {
    if (!result) {
        std::string error;
        for (const auto &d : result.diagnostics())
            error += d.message + '\n';
        throw std::runtime_error(error);
    }
    return std::move(result).takeValue();
}
SctAuthoringState fixture() {
    sct::SctDocument d;
    std::vector<sct::SctStringId> strings;
    for (int i = 0; i < 4; ++i)
        strings.push_back(d.allocateStringId());
    const auto init = d.allocateInstructionId(), ret = d.allocateInstructionId();
    std::vector<sct::SctInstructionId> guards, dialogs;
    for (int i = 0; i < 4; ++i) {
        guards.push_back(d.allocateInstructionId());
        dialogs.push_back(d.allocateInstructionId());
    }
    sct::SctScriptSectionContent code;
    for (int i = 0; i < 4; ++i) {
        code.instructions.push_back({guards[i],
                                     0,
                                     true,
                                     {},
                                     {{0, expression("ByteVar[87] == " + std::to_string(i))},
                                      {1, sct::SctInstructionReference{i == 3 ? ret : guards[i + 1]}}}});
        code.instructions.push_back({dialogs[i],
                                     144,
                                     i % 2 == 0,
                                     i == 2 ? std::optional{expression("3")} : std::nullopt,
                                     {{0, sct::SctStringReference{strings[i]}}, {1, expression("0")}}});
    }
    code.instructions.push_back(
        {d.allocateInstructionId(),
         155,
         false,
         {},
         {{0, expression("1")}, {1, sct::SctStringReference{strings[0]}}, {2, expression("0")}}});
    const auto footer = d.allocateSupplementaryTextId();
    d.supplementaryText.push_back({footer, sct::SctTextKind::SctString, message("[A][B]")});
    code.instructions.push_back({d.allocateInstructionId(),
                                 25,
                                 true,
                                 {},
                                 {{0, expression("1")}, {1, sct::SctSupplementaryTextReference{footer}}}});
    code.instructions.push_back(
        {d.allocateInstructionId(), 265, false, {}, {{0, expression("7")}, {1, sct::SctStringReference{strings[0]}}}});
    code.instructions.push_back({ret, 12});
    d.sections.push_back({d.allocateSectionId(), "init", sct::SctScriptSectionContent{{{init, 12}}}});
    d.sections.push_back({d.allocateSectionId(), "loop", code});
    const std::vector<std::string> markers{"", "F", "S", "G"};
    for (int i = 0; i < 4; ++i) {
        d.sections.push_back(
            {d.allocateSectionId(),
             "talk" +
                 (markers[i].empty() ? std::string{} : std::string(1, static_cast<char>(markers[i][0] - 'A' + 'a'))),
             sct::SctStringGroupMarkerSectionContent{}});
        d.sections.push_back({d.allocateSectionId(), "t" + markers[i] + "line0000",
                              sct::SctStringSectionContent{
                                  {strings[i], message("Text " + std::to_string(i)), sct::SctTextKind::SctString}}});
    }
    const auto output = sct::SctDocumentExporter::exportDocument(
        d, sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,
                                         encoding,
                                         sct::SctDocumentOutputByteOrder::BigEndian,
                                         sct::SctDocumentOutputWrapper::Raw,
                                         sct::SctOpaquePreservationPolicy::RequirePreservation,
                                         {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 3}}});
    if (!output.success) {
        std::string error;
        for (const auto &d : output.diagnostics)
            error += d.message + '\n';
        throw std::runtime_error(error);
    }
    const auto span = std::as_bytes(std::span(output.bytes));
    std::vector<std::byte> bytes(span.begin(), span.end());
    const auto hash = sha256(bytes).value();
    auto p = SctAuthoringProject::create().value();
    auto imported = take(SctAuthoringImporter::import(
        p, p.revision,
        {{{AssetLocator::fromRelativePath("me002a.sct").value(), bytes.size(), {hash}}, bytes},
         {GamePlatform::GameCube, GameRegion::Europe, {hash}},
         {GamePlatform::GameCube, true},
         sct::SctKnownTextConvention::Windows1252Byte7F,
         "Synthetic"}));
    SctAuthoringState state{imported.project, {imported.program}};
    return take(SctTextAuthoring::discover(state, imported.script));
}
SctScriptId script(const SctAuthoringState &s) {
    return s.project.scripts.front().id;
}
SctTextResourceId text(const SctAuthoringState &s, std::string name) {
    return std::ranges::find(s.project.texts, name, &SctAuthoredText::sourceName)->id;
}
std::vector<std::uint8_t> output(const SctAuthoringState &state) {
    auto out = SctAuthoringMaterializer::materialize(
        {std::make_shared<const SctAuthoringProject>(state.project), state.programs, {script(state)}});
    if (!out.succeeded()) {
        std::string error;
        for (const auto &s : out.scripts) {
            for (const auto &d : s.infrastructureDiagnostics)
                error += d.message + '\n';
            for (const auto &d : s.diagnostics)
                error += d.message + '\n';
        }
        throw std::runtime_error(error);
    }
    return out.scripts.front().prepared->bytes;
}
std::string encoded(const SctAuthoringState &s) {
    return take(SctAuthoringCodec::encode(s.project));
}
TEST(SctTextAuthoringTest, LegacyPatternsAreBoundedAndCorroborated) {
    auto state = fixture();
    EXPECT_EQ(state.project.languageGroups.size(), 4);
    EXPECT_EQ(state.project.texts.size(), 5);
    for (const auto &t : state.project.texts)
        if (std::holds_alternative<sct::SctStringId>(t.target)) {
            EXPECT_EQ(t.languages.size(), 1);
            EXPECT_EQ(t.correspondenceKey, "talk/tline0000");
        }
    sct::SctDocument doc;
    doc.sections.push_back({doc.allocateSectionId(), "palyoko", sct::SctStringGroupMarkerSectionContent{}});
    doc.sections.push_back(
        {doc.allocateSectionId(), "abcdefgs1", sct::SctStringSectionContent{{doc.allocateStringId(), message("A")}}});
    doc.sections.push_back(
        {doc.allocateSectionId(), "abcdefg01", sct::SctStringSectionContent{{doc.allocateStringId(), message("Base")}}});
    doc.sections.push_back(
        {doc.allocateSectionId(), "a", sct::SctStringSectionContent{{doc.allocateStringId(), message("B")}}});
    doc.sections.push_back({doc.allocateSectionId(), "talkG", sct::SctStringGroupMarkerSectionContent{}});
    doc.sections.push_back(
        {doc.allocateSectionId(), "tFline0000", sct::SctStringSectionContent{{doc.allocateStringId(), message("C")}}});
    const auto found = SctTextAuthoring::observe(doc);
    ASSERT_EQ(found.size(), 4);
    EXPECT_EQ(found[0].languageKey, "S");
    EXPECT_EQ(found[0].correspondenceKey, "palyoko/abcdefg1");
    EXPECT_EQ(found[1].languageKey, "base");
    EXPECT_EQ(found[1].correspondenceKey, found[0].correspondenceKey);
    EXPECT_FALSE(found[2].languageKey);
    EXPECT_FALSE(found[3].languageKey);
}
TEST(SctTextAuthoringTest, CustomGroupsBatchReassignmentRenameAndRescanPreserveIdentityAndOutput) {
    auto state = fixture();
    const auto bytes = output(state);
    auto session = take(SctAuthoringSession::open(state));
    ASSERT_TRUE(session->execute(state.project.revision, "Custom",
                                 [](const auto &s) { return SctTextAuthoring::createLanguage(s, "Pirate dialect"); }));
    const auto group = session->state().project.languageGroups.back().id;
    const auto first = text(state, "tline0000"), second = text(state, "tFline0000");
    ASSERT_TRUE(session->execute(session->state().project.revision, "Assign",
                                 [&](const auto &s) { return SctTextAuthoring::assign(s, {first, second}, {group}); }));
    ASSERT_TRUE(session->execute(session->state().project.revision, "Rename",
                                 [&](const auto &s) { return SctTextAuthoring::renameLanguage(s, group, "Custom"); }));
    auto rediscovered = take(SctTextAuthoring::discover(session->state(), script(state)));
    EXPECT_EQ(rediscovered.project.find(first)->languages, std::vector{group});
    EXPECT_EQ(rediscovered.project.find(group)->name, "Custom");
    EXPECT_EQ(output(rediscovered), bytes);
    EXPECT_EQ(encoded(rediscovered),
              SctAuthoringCodec::encode(SctAuthoringCodec::decode(encoded(rediscovered)).value()).value());
    ASSERT_TRUE(session->undo());
    ASSERT_TRUE(session->undo());
    EXPECT_NE(session->state().project.find(first)->languages, std::vector{group});
    ASSERT_TRUE(session->redo());
    EXPECT_EQ(session->state().project.find(second)->languages, std::vector{group});
    auto unassigned = take(SctTextAuthoring::assign(rediscovered, {first}, {}));
    unassigned = take(SctTextAuthoring::discover(unassigned, script(state)));
    EXPECT_TRUE(unassigned.project.find(first)->languages.empty());
}
TEST(SctTextAuthoringTest, FourResourceEditIsAtomicAndEncodingFailureDoesNotAcceptAPartialGroup) {
    auto state = fixture();
    std::vector<SctTextEdit> edits;
    for (const auto &t : state.project.texts)
        if (std::holds_alternative<sct::SctStringId>(t.target))
            edits.push_back({t.id, message("Updated " + t.sourceName)});
    auto session = take(SctAuthoringSession::open(state));
    const auto before = encoded(state);
    auto bad = edits;
    bad.back().value = message("\xF0\x9F\x98\x80");
    EXPECT_FALSE(session->execute(state.project.revision, "Bad batch",
                                  [&](const auto &s) { return SctTextAuthoring::edit(s, bad, encoding); }));
    EXPECT_EQ(encoded(session->state()), before);
    ASSERT_TRUE(session->execute(state.project.revision, "Edit all",
                                 [&](const auto &s) { return SctTextAuthoring::edit(s, edits, encoding); }));
    EXPECT_NE(output(session->state()), output(state));
    ASSERT_TRUE(session->undo());
    EXPECT_EQ(output(session->state()), output(state));
    ASSERT_TRUE(session->redo());
    EXPECT_FALSE(SctTextAuthoring::edit(session->state(), {edits.front(), edits.front()}, encoding));
}
TEST(SctTextAuthoringTest, ExplicitFourRouteInsertionRetainsGuardsParametersAndScheduling) {
    auto state = fixture();
    auto w = take(SctAuthoringMaterializer::workingState(state.project, state.programs, script(state)));
    state = take(SctModuleAuthoring::extract(state, script(state), {w.document->sections[1].id}, "Conversation"));
    const auto seq = state.project.sequences.front();
    std::vector<SctTextInsertion> insertions;
    std::vector<SctTextResourceId> variants;
    for (int i = 0; i < 4; ++i) {
        state = take(SctTextAuthoring::createText(state, script(state), "new" + std::to_string(i),
                                                  sct::SctTextKind::SctString, sct::SctTextStorage::IndexedSection,
                                                  message("New"), {state.project.languageGroups[i].id}, encoding));
        variants.push_back(state.project.texts.back().id);
        insertions.push_back({seq.id, seq.actions[i * 2 + 1].id, seq.actions[i * 2 + 1].id, {0, {}}, variants.back()});
    }
    state =
        take(SctTextAuthoring::link(state, script(state), "New dialogue", variants, state.project.modules.front().id));
    const auto before = take(SctSequenceAuthoring::actions(state, seq.id));
    auto inserted = take(SctTextAuthoring::insert(state, insertions, encoding));
    const auto after = take(SctSequenceAuthoring::actions(inserted, seq.id));
    ASSERT_EQ(after.size(), before.size() + 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(after[i * 3].id, before[i * 2].id);
        EXPECT_EQ(after[i * 3 + 2].id, before[i * 2 + 1].id);
        EXPECT_EQ(after[i * 3 + 1].instruction.opcode, 144);
        EXPECT_EQ(after[i * 3 + 1].instruction.skipRefresh, before[i * 2 + 1].instruction.skipRefresh);
        EXPECT_EQ(after[i * 3 + 1].instruction.scheduledExpression.has_value(),
                  before[i * 2 + 1].instruction.scheduledExpression.has_value());
    }
    EXPECT_FALSE(output(inserted).empty());
}
TEST(SctTextAuthoringTest, SharedChoiceSplitRetargetsOnlySelectedUse) {
    auto state = fixture();
    const auto id = text(state, "tline0000");
    const auto groups = std::vector{state.project.languageGroups[0].id, state.project.languageGroups[1].id};
    state = take(SctTextAuthoring::assign(state, {id}, groups));
    state = take(SctTextAuthoring::link(state, script(state), "Shared choice", {id}));
    auto view = take(SctTextAuthoring::inspect(state, id));
    ASSERT_EQ(view.uses.size(), 3);
    const auto choice = view.uses[1];
    auto split = take(SctTextAuthoring::split(state, id, groups[1], "splitChoice", {choice}));
    EXPECT_EQ(split.project.find(id)->languages, std::vector{groups[0]});
    const auto newId = split.project.texts.back().id;
    EXPECT_EQ(take(SctTextAuthoring::inspect(split, newId)).uses, std::vector{choice});
    EXPECT_EQ(take(SctTextAuthoring::inspect(split, id)).uses.size(), 2);
    EXPECT_EQ(split.project.logicalTexts.back().variants.size(), 2);
    EXPECT_FALSE(output(split).empty());
    EXPECT_FALSE(SctTextAuthoring::split(state, id, groups[1], "bad", {{sct::SctInstructionId{999999}, {0, {}}}}));
}
TEST(SctTextAuthoringTest, FooterChoicesAndGeneratedListsKeepTheirOwnOperations) {
    auto state = fixture();
    const auto w = take(SctAuthoringMaterializer::workingState(state.project, state.programs, script(state)));
    state = take(SctSequenceAuthoring::promoteSequence(state, script(state), w.document->sections[1].id, "Loop"));
    const auto seq = state.project.sequences.back();
    const auto indexed = text(state, "tline0000"), footer = state.project.texts.back().id;
    const std::vector<SctTextInsertion> edits{{seq.id, seq.actions.back().id, seq.actions[9].id, {1, {}}, footer},
                                              {seq.id, seq.actions.back().id, seq.actions[10].id, {1, {}}, indexed}};
    auto inserted = take(SctTextAuthoring::insert(state, edits, encoding));
    const auto actions = take(SctSequenceAuthoring::actions(inserted, seq.id));
    EXPECT_EQ(actions[actions.size() - 3].instruction.opcode, 25);
    EXPECT_EQ(actions[actions.size() - 2].instruction.opcode, 265);
    EXPECT_FALSE(output(inserted).empty());
    auto invalid = edits;
    invalid[0].text = indexed;
    EXPECT_FALSE(SctTextAuthoring::insert(state, invalid, encoding));
}
TEST(SctTextAuthoringTest, CodecRejectsDanglingAssignmentsAndCrossScriptResources) {
    auto state = fixture();
    auto json = nlohmann::ordered_json::parse(encoded(state));
    json["texts"][0]["languages"][0] = "999999";
    EXPECT_FALSE(SctAuthoringCodec::decode(json.dump()));
    json = nlohmann::ordered_json::parse(encoded(state));
    json["texts"][0]["target"]["id"] = "999999";
    EXPECT_FALSE(SctAuthoringCodec::decode(json.dump()));
    json = nlohmann::ordered_json::parse(encoded(state));
    json["languageGroups"][1]["id"] = json["languageGroups"][0]["id"];
    EXPECT_FALSE(SctAuthoringCodec::decode(json.dump()));
    EXPECT_FALSE(SctTextAuthoring::link(state, SctScriptId{999999}, "Bad", {state.project.texts.front().id}));
}
TEST(SctTextAuthoringTest, ConflictingAndDuplicateNameCandidatesAreOrderIndependent) {
    sct::SctDocument doc;
    // Each name is an unmarked counterpart of the next one, while also carrying
    // a language marker itself. Conflicting proposals must not win by order.
    for (const auto name : {"tline0000", "tFline0000", "tSFline0000", "tGFline0000"})
        doc.sections.push_back(
            {doc.allocateSectionId(), name, sct::SctStringSectionContent{{doc.allocateStringId(), message("A")}}});
    const auto first = SctTextAuthoring::observe(doc);
    EXPECT_FALSE(first[1].languageKey);
    std::ranges::reverse(doc.sections);
    const auto reversed = SctTextAuthoring::observe(doc);
    for (const auto &a : first) {
        const auto b = std::ranges::find(reversed, a.target, &SctTextObservation::target);
        ASSERT_NE(b, reversed.end());
        EXPECT_EQ(a.languageKey, b->languageKey);
        EXPECT_EQ(a.correspondenceKey, b->correspondenceKey);
    }
    doc.sections.push_back(doc.sections.front());
    doc.sections.back().id = doc.allocateSectionId();
    std::get<sct::SctStringSectionContent>(doc.sections.back().content).string.id = doc.allocateStringId();
    const auto duplicated = SctTextAuthoring::observe(doc);
    EXPECT_FALSE(duplicated.front().languageKey);
    EXPECT_FALSE(duplicated.back().languageKey);
}
TEST(SctTextAuthoringTest, InsertionAndSplitHaveStableUndoReloadAndAtomicFailure) {
    auto state = fixture();
    const auto w = take(SctAuthoringMaterializer::workingState(state.project, state.programs, script(state)));
    state = take(SctSequenceAuthoring::promoteSequence(state, script(state), w.document->sections[1].id, "Loop"));
    const auto seq = state.project.sequences.back();
    const auto id = text(state, "tline0000");
    state = take(SctTextAuthoring::assign(state, {id},
                                          {state.project.languageGroups[0].id, state.project.languageGroups[1].id}));
    auto session = take(SctAuthoringSession::open(state));
    const auto original = encoded(state);
    const SctTextInsertion placement{seq.id, seq.actions[1].id, seq.actions[1].id, {0, {}}, id};
    auto bad = placement;
    bad.operand.schemaIndex = 99;
    EXPECT_FALSE(session->execute(state.project.revision, "Invalid insertion", [&](const auto &s) {
        return SctTextAuthoring::insert(s, {placement, bad}, encoding);
    }));
    EXPECT_EQ(encoded(session->state()), original);
    ASSERT_TRUE(session->execute(state.project.revision, "Insert",
                                 [&](const auto &s) { return SctTextAuthoring::insert(s, {placement}, encoding); }));
    const auto inserted = session->state().project.find(seq.id)->actions[1].id;
    ASSERT_TRUE(session->undo());
    EXPECT_EQ(session->state().project.find(seq.id)->actions.size(), seq.actions.size());
    ASSERT_TRUE(session->redo());
    EXPECT_EQ(session->state().project.find(seq.id)->actions[1].id, inserted);
    const auto uses = take(SctTextAuthoring::inspect(session->state(), id)).uses;
    const auto group = state.project.languageGroups[1].id;
    ASSERT_TRUE(session->execute(session->state().project.revision, "Split", [&](const auto &s) {
        return SctTextAuthoring::split(s, id, group, "independent", {uses.front()});
    }));
    const auto split = session->state().project.texts.back().id;
    ASSERT_TRUE(session->undo());
    EXPECT_EQ(session->state().project.find(split), nullptr);
    ASSERT_TRUE(session->redo());
    ASSERT_NE(session->state().project.find(split), nullptr);
    auto reopened = session->state();
    reopened.project = take(SctAuthoringCodec::decode(encoded(reopened)));
    EXPECT_EQ(reopened.project.find(seq.id)->actions[1].id, inserted);
    EXPECT_EQ(reopened.project.find(split)->languages, std::vector{group});
    EXPECT_EQ(output(reopened), output(session->state()));
    EXPECT_FALSE(session->execute(session->state().project.revision, "Silent rebind", [&](const auto &s) {
        auto next = s;
        std::ranges::find(next.project.texts, id, &SctAuthoredText::id)->target = next.project.find(split)->target;
        return Result<SctAuthoringState>::success(next);
    }));
}
TEST(SctTextAuthoringTest, ExplicitSwitchCaseInsertionPreservesOtherArrivalsToSharedBody) {
    auto state = fixture();
    auto w = take(SctAuthoringMaterializer::workingState(state.project, state.programs, script(state)));
    auto doc = std::make_shared<sct::SctDocument>(*w.document);
    auto &body = std::get<sct::SctScriptSectionContent>(doc->sections[1].content).instructions;
    const auto guard = body[0].id, dialog = body[1].id;
    sct::SctDocumentInstruction dispatch{doc->allocateInstructionId(), 3, true};
    dispatch.fixedParameters = {{0, expression("ByteVar[87]")}};
    dispatch.repeatedParameterGroups = {
        {{{2, sct::SctEncodedWordValue{0}}, {3, sct::SctInstructionReference{dialog}}}},
        {{{2, sct::SctEncodedWordValue{1}}, {3, sct::SctInstructionReference{dialog}}}},
        {{{2, sct::SctEncodedWordValue{UINT32_MAX}}, {3, sct::SctInstructionReference{guard}}}}};
    body.insert(body.begin(), dispatch);
    w.document = doc;
    state.project =
        take(SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script(state), w));
    state = take(SctSequenceAuthoring::promoteSequence(state, script(state), doc->sections[1].id, "Dispatch"));
    const auto seq = state.project.sequences.back();
    const SctTextInsertion insertion{seq.id,  seq.actions[2].id,        seq.actions[2].id,
                                     {0, {}}, text(state, "tline0000"), SctTextIncomingEdge{seq.actions[0].id, {3, 0}}};
    auto changed = take(SctTextAuthoring::insert(state, {insertion}, encoding));
    const auto actions = take(SctSequenceAuthoring::actions(changed, seq.id));
    const auto &cases = actions.front().instruction.repeatedParameterGroups;
    EXPECT_EQ(std::get<sct::SctInstructionReference>(cases[0].parameters[1].value).target, actions[2].instruction.id);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(cases[1].parameters[1].value).target, dialog);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(cases[2].parameters[1].value).target, guard);
    EXPECT_FALSE(output(changed).empty());
    EXPECT_FALSE(SctTextAuthoring::insert(state, {insertion, insertion}, encoding));
    auto invalid = insertion;
    invalid.incoming->operand.repeatedGroupOrdinal = 2;
    EXPECT_FALSE(SctTextAuthoring::insert(state, {invalid}, encoding));
}
TEST(SctTextAuthoringTest, RealizationContainsOnlyEmittedTextLocations) {
    auto state = fixture();
    auto w = take(SctAuthoringMaterializer::workingState(state.project, state.programs, script(state)));
    auto doc = std::make_shared<sct::SctDocument>(*w.document);
    const auto unused = doc->allocateSupplementaryTextId();
    doc->supplementaryText.push_back({unused, sct::SctTextKind::PlainString, sct::SctOpaqueText{{0xff, 0}}});
    w.document = doc;
    state.project =
        take(SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script(state), w));
    state = take(SctTextAuthoring::discover(state, script(state)));
    const auto id = state.project.texts.back().id;
    state = take(SctTextAuthoring::link(state, script(state), "Unassigned draft", {id}));
    const auto result = SctAuthoringMaterializer::materialize(
        {std::make_shared<const SctAuthoringProject>(state.project), state.programs, {script(state)}});
    ASSERT_TRUE(result.succeeded());
    const auto &records = result.scripts.front().prepared->realization.records;
    EXPECT_EQ(std::ranges::find(records, SctAuthoringEntityId{id}, &SctRealizationRecord::entity), records.end());
    const auto logical = std::ranges::find(records, SctAuthoringEntityId{state.project.logicalTexts.back().id},
                                           &SctRealizationRecord::entity);
    ASSERT_NE(logical, records.end());
    EXPECT_TRUE(logical->locations.empty());
}
} // namespace
