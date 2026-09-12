#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SpiceSCT/SctScptEncoding.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
using namespace salsa::core;
namespace sct = spice::sct;
sct::SctCanonicalExpression expression(std::string value) {
    auto result = SctExpressionLanguage::parse(std::move(value));
    if (!result.succeeded()) throw std::runtime_error(result.issues.front().message);
    return *result.expression;
}
SctAuthoringState initial(bool fixedGap = false) {
    sct::SctDocument document;
    const auto a = document.allocateInstructionId(), b = document.allocateInstructionId(), wait = document.allocateInstructionId();
    const auto call = document.allocateInstructionId(), ret = document.allocateInstructionId(), callee = document.allocateInstructionId();
    const auto condition = expression("Low16IntVar[15] == 20");
    document.sections.push_back({document.allocateSectionId(), "loop", sct::SctScriptSectionContent{{
        {a, 0, true, {}, {{0, condition}, {1, sct::SctInstructionReference{wait}}}},
        {b, 0, false, {}, {{0, condition}, {1, sct::SctInstructionReference{ret}}}},
        {wait, 16, true, expression("20"), {{0, expression("3")}}},
        {call, 11, false, {}, {{0, sct::SctInstructionReference{callee}}}},
        {ret, 12}}}});
    document.sections.push_back({document.allocateSectionId(), "called", sct::SctScriptSectionContent{{{callee, 12}}}});
    const auto switchId = document.allocateInstructionId();
    sct::SctDocumentInstruction dispatch{switchId, 3, true};
    dispatch.fixedParameters = {{0, expression("Low16IntVar[15]")}};
    dispatch.repeatedParameterGroups = {
        {{{2, sct::SctEncodedWordValue{20}}, {3, sct::SctInstructionReference{a}}}},
        {{{2, sct::SctEncodedWordValue{331}}, {3, sct::SctInstructionReference{b}}}},
        {{{2, sct::SctEncodedWordValue{UINT32_MAX}}, {3, sct::SctInstructionReference{callee}}}}};
    document.sections.push_back({document.allocateSectionId(), "arrivalDispatch", sct::SctScriptSectionContent{{dispatch}}});
    auto output = sct::SctDocumentExporter::exportDocument(document, sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,
        sct::kSctShiftJisByte7FEncoding, sct::SctDocumentOutputByteOrder::BigEndian, sct::SctDocumentOutputWrapper::Raw,
        sct::SctOpaquePreservationPolicy::RequirePreservation, {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
    if (!output.success) {
        std::string message = "Synthetic sequence export failed";
        for (const auto& diagnostic : output.diagnostics) message += " " + diagnostic.message;
        throw std::runtime_error(message);
    }
    if (fixedGap) {
        // Insert a synthetic unclaimed span between indexed sections, updating
        // the index and cross-boundary relative references from SPICE's layout.
        const auto gap = output.layout->sections[1].payloadSpan.offset;
        output.bytes.insert(output.bytes.begin() + gap, 8, 0xff);
        const auto writeWord = [&](std::uint32_t offset, std::uint32_t value) {
            for (unsigned byte = 0; byte < 4; ++byte) output.bytes[offset + byte] = static_cast<std::uint8_t>(value >> (24 - byte * 8));
        };
        for (const auto& entry : output.layout->sections) if (entry.payloadSpan.offset >= gap)
            writeWord(entry.indexRowSpan.offset, entry.dataRelativeOffset + 8);
        for (const auto& relocation : output.layout->relocations) {
            const auto* target = std::get_if<sct::SctInstructionId>(&relocation.target);
            if (!target) throw std::runtime_error("Synthetic fixture expects instruction references only");
            const auto found = std::ranges::find(output.layout->instructions, *target, &sct::SctInstructionLayoutRecord::id);
            const auto sourceShift = relocation.operandSpan.offset >= gap ? 8u : 0u;
            const auto targetShift = found->span.offset >= gap ? 8u : 0u;
            writeWord(relocation.operandSpan.offset + sourceShift, relocation.encodedValue + targetShift - sourceShift);
        }
    }
    const auto span = std::as_bytes(std::span(output.bytes)); const std::vector<std::byte> bytes(span.begin(), span.end());
    const auto hash = sha256(bytes).value(); auto project = SctAuthoringProject::create().value();
    SctAuthoringImportRequest request{{{AssetLocator::fromRelativePath("me002a.sct").value(), bytes.size(), {hash}}, bytes},
        {GamePlatform::GameCube, GameRegion::Japan, {hash}}, {GamePlatform::GameCube, false}, {}, "me002a"};
    auto imported = SctAuthoringImporter::import(project, project.revision, request);
    if (!imported) throw std::runtime_error(imported.diagnostics().front().message);
    return {imported.value().project, {imported.value().program}};
}
std::vector<sct::SctParameterSite> conditions(const SctAuthoringState& state) {
    std::vector<sct::SctParameterSite> result;
    for (const auto& section : state.programs.front()->document().sections)
        if (const auto* body = std::get_if<sct::SctScriptSectionContent>(&section.content))
            for (const auto& instruction : body->instructions) if (instruction.opcode == 0) result.push_back({instruction.id, {0, {}}});
    return result;
}
SctAuthoringState promoted() {
    auto state = initial(); const auto script = state.project.scripts.front().id;
    auto sequence = SctSequenceAuthoring::promoteSequence(state, script, state.programs.front()->document().sections.front().id, "Arrival sequence");
    if (!sequence) throw std::runtime_error(sequence.diagnostics().front().message);
    auto predicate = SctSequenceAuthoring::namePredicate(sequence.value(), script, conditions(state), "Arrived from town");
    if (!predicate) throw std::runtime_error(predicate.diagnostics().front().message);
    return predicate.value();
}
std::vector<std::uint8_t> output(const SctAuthoringState& state) {
    auto result = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(state.project), state.programs,
        {state.project.scripts.front().id}, SctAuthoringOutputMode::Rebuild});
    if (!result.succeeded()) {
        std::string error = "materialization failed";
        for (const auto& script : result.scripts) {
            for (const auto& d : script.infrastructureDiagnostics) error += " " + d.message;
            for (const auto& d : script.diagnostics) error += " " + d.message;
        }
        throw std::runtime_error(error);
    }
    return result.scripts.front().prepared->bytes;
}
TEST(SctSequenceAuthoringTest, PromotionRenameAndReloadDoNotChangeOutputOrActionIdentity) {
    auto state = initial(); const auto bytes = output(state); state = promoted(); EXPECT_EQ(output(state), bytes);
    const auto sequence = state.project.sequences.front(); const auto predicate = state.project.predicates.front();
    state.project.sequences.front().name = "A new title"; state.project.predicates.front().name = "A new condition name";
    auto encoded = SctAuthoringCodec::encode(state.project); ASSERT_TRUE(encoded);
    auto decoded = SctAuthoringCodec::decode(encoded.value()); ASSERT_TRUE(decoded);
    state.project = decoded.value(); EXPECT_EQ(SctAuthoringCodec::encode(state.project).value(), encoded.value());
    EXPECT_EQ(state.project.sequences.front().id, sequence.id); EXPECT_EQ(state.project.sequences.front().actions.front().id, sequence.actions.front().id);
    EXPECT_EQ(state.project.predicates.front().id, predicate.id); EXPECT_EQ(output(state), bytes);
    auto view = SctSequenceAuthoring::actions(state, sequence.id); ASSERT_TRUE(view);
    EXPECT_EQ(view.value()[0].kind, SctSequenceActionKind::Branch); EXPECT_TRUE(view.value()[0].instruction.skipRefresh);
    EXPECT_EQ(view.value()[2].kind, SctSequenceActionKind::Wait); EXPECT_EQ(view.value()[3].kind, SctSequenceActionKind::Call);
    EXPECT_EQ(view.value()[4].kind, SctSequenceActionKind::Return); EXPECT_FALSE(view.value()[3].targets.empty());
    SctAuthoringPresentation presentation{state.project.id, {{sequence.id, 3, 4, false}, {sequence.actions[0].id, 8, 9, false}}, {predicate.id}};
    ASSERT_TRUE(SctAuthoringPresentationCodec::encode(presentation, state.project)); EXPECT_EQ(SctAuthoringCodec::encode(state.project).value(), encoded.value());
}
TEST(SctSequenceAuthoringTest, CompoundPredicateEditsAllUsesWithSingleUndoAndKeepsStateViews) {
    auto opened = SctAuthoringSession::open(promoted()); ASSERT_TRUE(opened); auto& session = *opened.value();
    const auto before = session.capture(); const auto predicate = before->project.predicates.front().id;
    auto edited = session.execute(before->project.revision, "Compound condition", [&](const auto& state) {
        return SctSequenceAuthoring::editPredicate(state, predicate, expression("Low16IntVar[15] == 331 && BitVar[12] != 0"));
    }); ASSERT_TRUE(edited);
    const auto result = SctSequenceAuthoring::predicate(session.state(), predicate); ASSERT_TRUE(result);
    const auto reads = SctSequenceAuthoring::stateReads(result.value()); ASSERT_EQ(reads.size(), 2u);
    EXPECT_EQ(reads[0].storage.kind, SctVariableKind::Integer); EXPECT_EQ(reads[0].storage.index, 15u);
    EXPECT_EQ(reads[0].access, sct::SctScptValueKind::IntegerVariableLow16Comparison);
    EXPECT_NE(output(session.state()), output(*before));
    EXPECT_FALSE(SctSequenceAuthoring::selectArrival(session.state(), predicate, "me033b"));
    ASSERT_TRUE(session.undo()); EXPECT_EQ(output(session.state()), output(*before)); ASSERT_TRUE(session.redo());
    EXPECT_EQ(SctSequenceAuthoring::predicate(session.state(), predicate).value().termination, result.value().termination);
    auto divergent = SctAuthoringMaterializer::workingState(session.state().project, session.state().programs, session.state().project.scripts.front().id).value();
    auto applied = SctSemanticOperationService::apply(*divergent.document, {{{SctReplaceParameterValueOperation{before->project.predicates.front().uses[0], expression("0")}}}});
    ASSERT_TRUE(applied.succeeded()); divergent.document = applied.document;
    EXPECT_FALSE(SctAuthoringMaterializer::replaceWorkingState(session.state().project, session.state().programs, session.state().project.scripts.front().id, divergent));
}
TEST(SctSequenceAuthoringTest, ArrivalPickerPreservesReadModeAndRejectsAmbiguousOrSpecialValues) {
    auto state = promoted(); const auto id = state.project.predicates.front().id;
    EXPECT_EQ(SctSequenceAuthoring::arrivalLocation("me033b").value().encoded, 331u);
    EXPECT_EQ(SctSequenceAuthoring::arrivalLocation("me099a").value().encoded, 990u);
    EXPECT_FALSE(SctSequenceAuthoring::arrivalLocation("me002k")); EXPECT_FALSE(SctSequenceAuthoring::arrivalLocation("me1000a"));
    EXPECT_FALSE(SctSequenceAuthoring::arrivalLocation("40000")); EXPECT_FALSE(SctSequenceAuthoring::arrivalLocation("me-01a"));
    EXPECT_NE(SctSequenceAuthoring::specialArrivalMeaning(10000).find("tentative"), std::string::npos);
    EXPECT_EQ(SctSequenceAuthoring::specialArrivalMeaning(20000), "Unresolved arrival value");
    auto arrival = SctSequenceAuthoring::selectArrival(state, id, "me033b"); ASSERT_TRUE(arrival);
    EXPECT_EQ(SctExpressionLanguage::project(SctSequenceAuthoring::predicate(arrival.value(), id).value()).text, "Low16IntVar[15] == 331");
    auto ordinary = SctSequenceAuthoring::editPredicate(state, id, expression("IntVar[87] == 20")); ASSERT_TRUE(ordinary);
    EXPECT_FALSE(SctSequenceAuthoring::selectArrival(ordinary.value(), id, "me033b"));
}
TEST(SctSequenceAuthoringTest, ArrivalSwitchRetainsCaseOrderTargetsAndDefaultBehavior) {
    const auto state = promoted(); const auto script = state.project.scripts.front().id;
    const auto& before = std::get<sct::SctScriptSectionContent>(state.programs.front()->document().sections.back().content).instructions.front();
    auto changed = SctSequenceAuthoring::selectArrivalCase(state, script, before.id, 0, "me099a"); ASSERT_TRUE(changed);
    auto working = SctAuthoringMaterializer::workingState(changed.value().project, state.programs, script).value();
    const auto& after = std::get<sct::SctScriptSectionContent>(working.document->sections.back().content).instructions.front();
    EXPECT_EQ(std::get<sct::SctEncodedWordValue>(after.repeatedParameterGroups[0].parameters[0].value).value, 990u);
    for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(std::get<sct::SctInstructionReference>(before.repeatedParameterGroups[i].parameters[1].value).target,
        std::get<sct::SctInstructionReference>(after.repeatedParameterGroups[i].parameters[1].value).target);
    EXPECT_EQ(std::get<sct::SctEncodedWordValue>(after.repeatedParameterGroups[2].parameters[0].value).value, UINT32_MAX);
    EXPECT_TRUE(after.skipRefresh); EXPECT_FALSE(SctSequenceAuthoring::selectArrivalCase(state, script, before.id, 0, "me033b"));
    EXPECT_FALSE(SctSequenceAuthoring::selectArrivalCase(state, script, before.id, 2, "me033a")); EXPECT_NE(output(state), output(changed.value()));
}
TEST(SctSequenceAuthoringTest, TimingChangesPreserveTargetsAndSurviveDurableCheckpoint) {
    auto state = promoted(); const auto sequence = state.project.sequences.front();
    auto before = SctSequenceAuthoring::actions(state, sequence.id).value(); const auto action = sequence.actions[2].id;
    auto result = SctSequenceAuthoring::setTiming(state, sequence.id, action, expression("40"), true); ASSERT_TRUE(result);
    auto after = SctSequenceAuthoring::actions(result.value(), sequence.id).value();
    EXPECT_EQ(after[0].targets, before[0].targets); EXPECT_EQ(after[3].targets, before[3].targets);
    EXPECT_EQ(after[2].id, action); EXPECT_TRUE(after[2].instruction.skipRefresh); EXPECT_EQ(SctExpressionLanguage::project(*after[2].instruction.scheduledExpression).text, "40");
    EXPECT_NE(output(result.value()), output(state));
    struct Temporary { std::filesystem::path root = std::filesystem::temp_directory_path() / ("salsa-s4-" + generateSctAuthoringUuid().value());
        ~Temporary() { std::error_code ec; std::filesystem::remove_all(root, ec); } } temp;
    auto workspace = LocalSalsaWorkspace::openOrCreate(temp.root, {temp.root / "source", {GamePlatform::GameCube, GameRegion::Japan, state.project.baselines.front().datasetFingerprint}}).value();
    auto saved = SctAuthoringStore::save(workspace, result.value(), {state.project.id}, {}); ASSERT_TRUE(saved);
    auto loaded = SctAuthoringStore::load(workspace); ASSERT_TRUE(loaded); ASSERT_TRUE(loaded.value());
    EXPECT_EQ(output(loaded.value()->state), output(result.value()));
    EXPECT_EQ(loaded.value()->state.project.sequences.front().actions[2].id, action);
}
TEST(SctSequenceAuthoringTest, RejectsDuplicateIdsForeignBindingsDanglingUsesAndBadActionOrder) {
    auto state = promoted(); auto bad = state;
    bad.project.sequences[0].actions[1].id = bad.project.sequences[0].actions[0].id; EXPECT_FALSE(SctAuthoringSession::open(bad));
    bad = state; bad.project.predicates[0].binding.importedDocument = {generateSctAuthoringUuid().value()}; EXPECT_FALSE(SctAuthoringCodec::encode(bad.project));
    bad = state; bad.project.predicates[0].uses[0].instruction = sct::SctInstructionId{999999}; EXPECT_FALSE(SctAuthoringSession::open(bad));
    bad = state; std::swap(bad.project.sequences[0].actions[0], bad.project.sequences[0].actions[1]); EXPECT_FALSE(SctAuthoringSession::open(bad));
    EXPECT_FALSE(SctSequenceAuthoring::namePredicate(state, state.project.scripts.front().id, conditions(state), "Duplicate ownership"));
    auto json = nlohmann::json::parse(SctAuthoringCodec::encode(state.project).value());
    json["sequences"][0]["actions"][0]["id"] = 123; EXPECT_FALSE(SctAuthoringCodec::decode(json.dump()));
    auto materialized = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(bad.project), bad.programs,
        {bad.project.scripts.front().id}, SctAuthoringOutputMode::Rebuild}); EXPECT_FALSE(materialized.succeeded());
}
TEST(SctSequenceAuthoringTest, PhysicalInsertionRetainsExistingActionIdsAndUndoDoesNotRecycleNewIds) {
    auto opened = SctAuthoringSession::open(promoted()); ASSERT_TRUE(opened); auto& session = *opened.value();
    const auto prior = session.state().project.sequences.front();
    auto result = session.execute(session.state().project.revision, "Insert wait", [&](const auto& state) {
        auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, prior.script).value();
        auto document = std::make_shared<sct::SctDocument>(*working.document);
        sct::SctDocumentInstruction inserted{document->allocateInstructionId(), 16, false, {}, {{0, expression("5")}}};
        auto applied = SctSemanticOperationService::applyInPlace(*document, {{{SctInsertInstructionAfterOperation{prior.actions[2].instruction, inserted}}}});
        if (!applied.succeeded()) return Result<SctAuthoringState>::failure({DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, "Insertion failed"});
        working.document = document;
        auto project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, prior.script, working);
        if (!project) return Result<SctAuthoringState>::failure(project.diagnostics());
        return Result<SctAuthoringState>::success({project.value(), state.programs});
    }); ASSERT_TRUE(result);
    const auto& updated = session.state().project.sequences.front(); ASSERT_EQ(updated.actions.size(), prior.actions.size() + 1);
    EXPECT_EQ(updated.actions[2].id, prior.actions[2].id); EXPECT_EQ(updated.actions[4].id, prior.actions[3].id);
    const auto high = session.state().project.nextEntityId; ASSERT_TRUE(session.undo()); EXPECT_EQ(session.state().project.nextEntityId, high);
    ASSERT_TRUE(session.redo()); EXPECT_EQ(session.state().project.sequences.front().actions[2].id, prior.actions[2].id);
}
TEST(SctSequenceAuthoringTest, FixedGapGrowthRebuildsFreshLayoutAndSupportsUndo) {
    auto state = initial(true);
    auto named = SctSequenceAuthoring::namePredicate(state, state.project.scripts.front().id, conditions(state), "Gap-constrained condition");
    ASSERT_TRUE(named); state = named.value();
    auto opened = SctAuthoringSession::open(state); ASSERT_TRUE(opened); auto& session = *opened.value();
    const auto captured = session.capture();
    auto changed = session.execute(state.project.revision, "Expand near fixed gap", [&](const auto& current) {
        return SctSequenceAuthoring::editPredicate(current, current.project.predicates.front().id,
            expression("Low16IntVar[15] == 331 && BitVar[12] != 0"));
    });
    ASSERT_TRUE(changed);
    const auto grown = output(session.state());
    EXPECT_NE(grown, output(*captured));
    auto rebuilt = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(session.state().project),
        state.programs, {state.project.scripts.front().id}, SctAuthoringOutputMode::Rebuild});
    ASSERT_TRUE(rebuilt.succeeded());
    EXPECT_TRUE(rebuilt.scripts[0].prepared->document->opaqueAttachments.empty());
    EXPECT_TRUE(rebuilt.scripts[0].prepared->preservation->attachments.empty());
    EXPECT_FALSE(state.programs.front()->document().opaqueAttachments.empty());
    ASSERT_TRUE(session.undo()); EXPECT_EQ(output(session.state()), output(*captured));
    ASSERT_TRUE(session.redo()); EXPECT_EQ(output(session.state()), grown);
}

TEST(SctSequenceAuthoringTest, ValidDraftWithUnresolvedOutputCanBeEditedAndUndone) {
    auto state = promoted();
    const auto script = state.project.scripts.front().id;
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script).value();
    auto document = std::make_shared<sct::SctDocument>(*working.document);
    auto& call = std::get<sct::SctScriptSectionContent>(document->sections[0].content).instructions[3];
    call.fixedParameters[0].value = sct::SctUnresolvedReferenceValue{{sct::SctReferenceTargetStorage::Instruction, {}}, {0}};
    working.document = document;
    auto replacement = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working);
    ASSERT_TRUE(replacement); state.project = replacement.value();
    auto session = SctAuthoringSession::open(state); ASSERT_TRUE(session);
    auto edit = session.value()->execute(state.project.revision, "Edit incomplete draft", [&](const auto& current) {
        return SctSequenceAuthoring::editPredicate(current, current.project.predicates.front().id, expression("BitVar[12] != 0"));
    });
    ASSERT_TRUE(edit);
    auto output = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(session.value()->state().project),
        state.programs, {script}, SctAuthoringOutputMode::Rebuild});
    EXPECT_FALSE(output.succeeded()); EXPECT_FALSE(output.scripts[0].prepared);
    ASSERT_TRUE(session.value()->undo()); ASSERT_TRUE(session.value()->redo());
}
}
