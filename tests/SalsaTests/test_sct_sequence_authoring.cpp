#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SalsaCore/Authoring/SctOverworldAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
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
    document.sections.push_back({document.allocateSectionId(), "init", sct::SctScriptSectionContent{{{document.allocateInstructionId(),12}}}});
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
    const auto& before = std::get<sct::SctScriptSectionContent>(state.programs.front()->document().sections[2].content).instructions.front();
    auto changed = SctSequenceAuthoring::selectArrivalCase(state, script, before.id, 0, "me099a"); ASSERT_TRUE(changed);
    auto working = SctAuthoringMaterializer::workingState(changed.value().project, state.programs, script).value();
    const auto& after = std::get<sct::SctScriptSectionContent>(working.document->sections[2].content).instructions.front();
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
TEST(SctModuleAuthoringTest, MultiSectionExtractionPreservesOutputAndExposesExternalCallers) {
    auto state = initial(); const auto before = output(state); const auto script = state.project.scripts.front().id;
    const auto& doc = state.programs.front()->document();
    auto extracted = SctModuleAuthoring::extract(state, script, {doc.sections[1].id, doc.sections[0].id}, "Cutscene");
    ASSERT_TRUE(extracted) << extracted.diagnostics().front().message;
    const auto& module = extracted.value().project.modules.back();
    EXPECT_EQ(module.script, script); EXPECT_EQ(module.sections[0], doc.sections[0].id);
    EXPECT_EQ(output(extracted.value()), before);
    auto view = SctModuleAuthoring::inspect(extracted.value(), module.id); ASSERT_TRUE(view);
    EXPECT_EQ(view.value().sequences.size(), 2u); EXPECT_FALSE(view.value().boundary.empty());
    EXPECT_TRUE(std::ranges::any_of(view.value().boundary, [](const auto& item) { return item.description.find("Incoming") != std::string::npos; }));
    EXPECT_FALSE(extracted.value().project.ports.empty());
    auto encoded = SctAuthoringCodec::encode(extracted.value().project); ASSERT_TRUE(encoded);
    auto restored = SctAuthoringCodec::decode(encoded.value()); ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().modules[0].id, module.id);
    EXPECT_EQ(restored.value().modules[0].sections, module.sections);
    EXPECT_EQ(SctAuthoringCodec::encode(restored.value()).value(), encoded.value());
}
TEST(SctModuleAuthoringTest, EditUndoReopenRetainsIdentityAndRejectsConflictingOwnership) {
    auto state = initial(); const auto script = state.project.scripts.front().id;
    const auto section = state.programs.front()->document().sections.front().id;
    auto session = SctAuthoringSession::open(state); ASSERT_TRUE(session);
    ASSERT_TRUE(session.value()->execute(state.project.revision, "Extract cutscene", [&](const auto& current) { return SctModuleAuthoring::extract(current, script, {section}, "Cutscene"); }));
    const auto extracted = session.value()->state(); const auto module = extracted.project.modules.front().id;
    const auto seq = extracted.project.sequences.front(); const auto action = seq.actions.front();
    ASSERT_TRUE(session.value()->execute(extracted.project.revision, "Timing", [&](const auto& current) {
        return SctSequenceAuthoring::setTiming(current, seq.id, action.id, expression("3"), false);
    }));
    EXPECT_NE(output(session.value()->state()), output(extracted));
    ASSERT_TRUE(session.value()->undo()); EXPECT_EQ(output(session.value()->state()), output(extracted));
    ASSERT_TRUE(session.value()->redo()); EXPECT_EQ(session.value()->state().project.modules.front().id, module);
    EXPECT_FALSE(SctModuleAuthoring::extract(extracted, script, {section}, "Conflict"));
    auto invalid = extracted.project; invalid.modules[0].sections.push_back(section); EXPECT_TRUE(hasErrors(invalid.validate()));
    invalid = extracted.project; invalid.modules[0].script = {999999}; EXPECT_TRUE(hasErrors(invalid.validate()));
    invalid = extracted.project; invalid.ports[0].action = SctActionId{999999}; EXPECT_TRUE(hasErrors(invalid.validate()));
    auto opened = SctAuthoringCodec::decode(SctAuthoringCodec::encode(session.value()->state().project).value()); ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value().sequences.front().actions.front().id, action.id);
}
TEST(SctModuleAuthoringTest, EmptyModuleIsDurableAndCanReceiveItsFirstAction) {
    auto state = initial(); const auto script = state.project.scripts.front().id;
    auto created = SctModuleAuthoring::createEmpty(state, script, "New interaction"); ASSERT_TRUE(created) << created.diagnostics().front().message;
    const auto module = created.value().project.modules.back();
    const auto sequence = created.value().project.sequences.back().id;
    const auto port = created.value().project.ports.back().id;
    auto appended = SctModuleAuthoring::appendAction(created.value(), module.id, sequence, 12); ASSERT_TRUE(appended);
    EXPECT_EQ(appended.value().project.ports.back().id, port);
    EXPECT_TRUE(appended.value().project.ports.back().action);
    auto edited = Result<SctAuthoringProject>::success(appended.value().project);
    auto decoded = SctAuthoringCodec::decode(SctAuthoringCodec::encode(edited.value()).value()); ASSERT_TRUE(decoded);
    auto result = SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(decoded.value()), state.programs, {script}});
    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(decoded.value().modules.back().id, module.id);
}

}

namespace {
smores::script::Catalog entryCatalog(const SctAuthoringState& state, bool eventhook = false) {
    namespace rel = smores::script;
    rel::ResourceIdentity identity{state.project.baselines.front().datasetFingerprint.digest.toHex(), "field/a002a.mld", "revision", "document"};
    rel::Entry entry{identity, 3, {44}, 1234, 4000, eventhook ? "eventhook" : "goscript", {}};
    if (eventhook) { entry.parameters.resize(9); entry.parameters[8] = 42; }
    rel::Resource r{identity, rel::ResourceRole::Base, {}};
    for (unsigned i = 0; i < 4; ++i) { auto e = entry; e.tableIndex = i; e.localId.value += i; r.entries.push_back(e); }
    return {{r}};
}
SctAuthoringState withResponseModule() {
    auto state = initial(); const auto script = state.project.scripts.front().id;
    const auto section = state.programs.front()->document().sections[1].id;
    return SctModuleAuthoring::extract(state, script, {section}, "Response").value();
}
}
TEST(SctEntrypointAuthoring, NewDirectHandlerRoundTripsAndPreservesOriginalProgram) {
    auto state = withResponseModule(); const auto script = state.project.scripts.front().id;
    const auto catalog = entryCatalog(state); const auto entry = catalog.resources[0].entries[3]; const auto port = state.project.ports.front().id;
    auto result = SctEntrypointAuthoring::createDirectHandler(state, script, entry, smores::script::ResourceRole::Base, port, catalog);
    ASSERT_TRUE(result) << result.diagnostics().front().message;
    auto next = result.value(); ASSERT_EQ(next.project.entrypoints.size(), 1); const auto id = next.project.entrypoints.front().id;
    EXPECT_EQ(next.project.modules.front().id, state.project.modules.front().id);
    EXPECT_FALSE(SctEntrypointAuthoring::createDirectHandler(next, script, entry, smores::script::ResourceRole::Base, port, catalog));
    const auto encoded = SctAuthoringCodec::encode(next.project); ASSERT_TRUE(encoded);
    const auto decoded = SctAuthoringCodec::decode(encoded.value()); ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().entrypoints.front().id, id);
    EXPECT_EQ(SctAuthoringCodec::encode(decoded.value()).value(), encoded.value());
    EXPECT_FALSE(output(next).empty());
    auto session = SctAuthoringSession::open(state); ASSERT_TRUE(session);
    ASSERT_TRUE(session.value()->execute(state.project.revision, "Add trigger", [&](const auto&) { return result; }));
    ASSERT_TRUE(session.value()->undo()); EXPECT_TRUE(session.value()->state().project.entrypoints.empty());
    ASSERT_TRUE(session.value()->redo()); EXPECT_EQ(session.value()->state().project.entrypoints.front().id, id);
}
TEST(SctEntrypointAuthoring, StaleAndMissingResourcesRemainExplicit) {
    auto state = withResponseModule(); const auto script = state.project.scripts.front().id; auto catalog = entryCatalog(state);
    const auto entry = catalog.resources[0].entries[3];
    auto added = SctEntrypointAuthoring::createDirectHandler(state, script, entry, smores::script::ResourceRole::Auxiliary, state.project.ports.front().id, catalog); ASSERT_TRUE(added);
    const auto missing = SctEntrypointAuthoring::inspectBindings(added.value().project, script, {});
    ASSERT_EQ(missing.size(), 1); EXPECT_EQ(missing[0].severity, DiagnosticSeverity::Warning);
    catalog.resources[0].entries[3].entryId++;
    const auto stale = SctEntrypointAuthoring::inspectBindings(added.value().project, script, catalog);
    ASSERT_EQ(stale.size(), 1); EXPECT_EQ(stale[0].severity, DiagnosticSeverity::Warning);
    EXPECT_TRUE(SctAuthoringCodec::encode(added.value().project));
    const auto& bound = added.value().project.entrypoints.front();
    EXPECT_TRUE(SctEntrypointAuthoring::bind(added.value(), script, bound.name, *bound.binding, {}, bound.id));
    auto reselect = *bound.binding; reselect.provider = catalog.resources[0].entries[3];
    EXPECT_FALSE(SctEntrypointAuthoring::bind(added.value(), script, bound.name, reselect, {}, bound.id));
}
TEST(SctEntrypointAuthoring, SignalKeepsLevelPredicateAndOrderedConsumption) {
    auto state = withResponseModule(); const auto script = state.project.scripts.front().id;
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script).value();
    auto doc = std::make_shared<sct::SctDocument>(*working.document); auto& body = std::get<sct::SctScriptSectionContent>(doc->sections.front().content);
    body.instructions.front().fixedParameters.front().value = expression("BitVar[42] == 1");
    const auto call = body.instructions[3].id; const auto predicate = body.instructions.front().id;
    const auto clear = doc->allocateInstructionId(); body.instructions.insert(body.instructions.begin() + 4, {clear, 18, false, {}, {{0, expression("42")}}});
    working.document = doc; state.project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working).value();
    state = SctSequenceAuthoring::promoteSequence(state, script, doc->sections.front().id, "Loop").value();
    const auto seq = state.project.sequences.back().id; const auto catalog = entryCatalog(state, true);
    SctEntrypointBinding b{SctEntrypointKind::SignalConsumer, seq, sct::SctParameterSite{call,{0,{}}}, sct::SctParameterSite{predicate,{0,{}}},
        state.project.ports.front().id, catalog.resources[0].entries[3], smores::script::ResourceRole::Base, 42};
    const auto before = output(state);
    auto bound = SctEntrypointAuthoring::bind(state, script, "Signal response", b, catalog); ASSERT_TRUE(bound) << bound.diagnostics().front().message;
    EXPECT_EQ(output(bound.value()), before);
    auto next = bound.value(); auto& binding = *next.project.entrypoints.front().binding;
    binding.signalBit = 43; EXPECT_FALSE(SctAuthoringSession::validate(next));
    next = bound.value(); next.project.entrypoints.front().binding->provider = SctUnresolvedBinding{"Unknown runtime provider", "Provider not inspected.", {{SctEvidenceKind::Inference, "Observed bit consumer"}}};
    auto encoded = SctAuthoringCodec::encode(next.project); ASSERT_TRUE(encoded); EXPECT_FALSE(encoded.diagnostics().empty());
    auto restored = SctAuthoringCodec::decode(encoded.value()); ASSERT_TRUE(restored);
    EXPECT_TRUE(std::holds_alternative<SctUnresolvedBinding>(*restored.value().entrypoints.front().binding->provider));
    EXPECT_EQ(output(next), before);
    const auto& actions = next.project.find(seq)->actions;
    const auto entry = next.project.entrypoints.front().id;
    auto disabled = SctEntrypointAuthoring::setActivation(next, entry,
        SctLocalActivation{actions[1].id, {}, actions[2].id, actions.back().id});
    ASSERT_TRUE(disabled);
    disabled = SctEntrypointAuthoring::setEnabled(disabled.value(), entry, false); ASSERT_TRUE(disabled);
    auto current = SctAuthoringMaterializer::workingState(disabled.value().project, disabled.value().programs, script).value();
    auto lowered = SctEntrypointAuthoring::lower(disabled.value().project, script, *current.document); ASSERT_TRUE(lowered);
    const auto& emitted = std::get<sct::SctScriptSectionContent>(lowered.value().document.sections.front().content).instructions;
    ASSERT_EQ(emitted[2].opcode, 10);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(emitted[2].fixedParameters[0].value).target, actions.back().instruction);
    EXPECT_EQ(emitted[3].id, actions[2].instruction); // preparation retained behind bypass
    EXPECT_EQ(emitted[5].id, clear); EXPECT_EQ(emitted[5].opcode, 18); // consumption retained, also bypassed
    EXPECT_FALSE(output(disabled.value()).empty());
}
TEST(SctEntrypointAuthoring, ArrivalBindsExistingCallWithoutRewritingConditions) {
    auto state = withResponseModule(); const auto script = state.project.scripts.front().id;
    const auto& doc = state.programs.front()->document(); const auto& body = std::get<sct::SctScriptSectionContent>(doc.sections.front().content);
    state = SctSequenceAuthoring::promoteSequence(state, script, doc.sections.front().id, "Arrival").value();
    SctEntrypointBinding b{SctEntrypointKind::Arrival, state.project.sequences.back().id,
        sct::SctParameterSite{body.instructions[3].id, {0,{}}}, sct::SctParameterSite{body.instructions[0].id, {0,{}}}, state.project.ports.front().id};
    auto result = SctEntrypointAuthoring::bind(state, script, "Arrival response", b, {}); ASSERT_TRUE(result);
    EXPECT_EQ(output(result.value()), output(state));
    auto invalid = result.value(); invalid.project.entrypoints.front().binding->sequence = {999999}; EXPECT_FALSE(SctAuthoringCodec::encode(invalid.project));
    invalid = result.value(); invalid.project.entrypoints.front().binding->responseSite->instruction = sct::SctInstructionId{999999}; EXPECT_FALSE(SctAuthoringSession::validate(invalid));
}

TEST(SctEntrypointAuthoring, ReselectKeepsIdentityAndEditsOnlyTheSelectedTarget) {
    auto state=withResponseModule(); const auto script=state.project.scripts.front().id; const auto catalog=entryCatalog(state);
    state=SctEntrypointAuthoring::createDirectHandler(state,script,catalog.resources[0].entries[3],smores::script::ResourceRole::Base,state.project.ports.front().id,catalog).value();
    const auto entry=state.project.entrypoints.front();
    state=SctModuleAuthoring::createEmpty(state,script,"Second response").value();
    const auto module=state.project.modules.back().id; const auto sequence=state.project.sequences.back().id;
    state=SctModuleAuthoring::appendAction(state,module,sequence,12).value();
    const auto port=state.project.ports.back().id;
    const auto before=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value();
    auto b=*entry.binding; b.response=port;
    state=SctEntrypointAuthoring::setEnabled(state,entry.id,false).value();
    auto result=SctEntrypointAuthoring::bind(state,script,"New response",b,{},entry.id); ASSERT_TRUE(result);
    EXPECT_EQ(result.value().project.entrypoints.front().id,entry.id);
    EXPECT_FALSE(result.value().project.entrypoints.front().enabled);
    EXPECT_EQ(std::get<SctNamedActivation>(result.value().project.entrypoints.front().activation).body,
        std::get<SctNamedActivation>(entry.activation).body);
    auto after=SctAuthoringMaterializer::workingState(result.value().project,result.value().programs,script).value();
    auto expected=std::make_shared<sct::SctDocument>(*before.document);
    for(auto& section:expected->sections) if(auto* body=std::get_if<sct::SctScriptSectionContent>(&section.content)) for(auto& i:body->instructions)
        if(i.id==b.responseSite->instruction) i.fixedParameters.front().value=sct::SctInstructionReference{result.value().project.find(*result.value().project.find(port)->action)->instruction};
    auto intended=before; intended.document=expected;
    EXPECT_EQ(SctProgramStateCodec::encode(after).value(),SctProgramStateCodec::encode(intended).value());
    EXPECT_FALSE(output(result.value()).empty());
    auto malformed=nlohmann::ordered_json::parse(SctAuthoringCodec::encode(result.value().project).value());
    auto overflow = malformed; overflow["entrypoints"][0]["binding"]["provider"]["tblId"] = UINT64_MAX;
    EXPECT_FALSE(SctAuthoringCodec::decode(overflow.dump()));
    malformed["entrypoints"][0]["binding"]["predicate"]={{"instruction","99999"},{"parameter",0},{"group",nullptr}};
    EXPECT_FALSE(SctAuthoringCodec::decode(malformed.dump()));
    auto unresolved = result.value(); auto& binding = *unresolved.project.entrypoints.front().binding;
    binding.response = SctUnresolvedBinding{"Unselected module", "Choose a response.", {}};
    EXPECT_TRUE(SctAuthoringCodec::encode(unresolved.project));
    binding.responseSite->instruction = sct::SctInstructionId{999999};
    EXPECT_FALSE(SctAuthoringCodec::encode(unresolved.project));
}
TEST(SctEntrypointAuthoring, LifecycleRequiresKnownSectionAndWrapperCannotExtendFallthrough) {
    auto state=withResponseModule(); const auto script=state.project.scripts.front().id;
    auto working=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value();
    const auto section=working.document->sections.front().id;
    state=SctModuleAuthoring::extract(state,script,{section},"Loop lifecycle").value();
    const auto seq=std::ranges::find(state.project.sequences,section,&SctAuthoredSequence::section);
    const auto port=std::ranges::find_if(state.project.ports,[&](const auto& p){return p.action==seq->actions.front().id;});
    SctEntrypointBinding binding{SctEntrypointKind::Lifecycle,seq->id,{}, {},port->id};
    auto result=SctEntrypointAuthoring::bind(state,script,"Loop",binding,{}); ASSERT_TRUE(result); EXPECT_EQ(output(result.value()),output(state));
    auto document=std::make_shared<sct::SctDocument>(*working.document);
    document->sections.push_back({document->allocateSectionId(),"trailing",sct::SctScriptSectionContent{}}); working.document=document;
    state.project=SctAuthoringMaterializer::replaceWorkingState(state.project,state.programs,script,working).value();
    auto catalog=entryCatalog(state);
    EXPECT_FALSE(SctEntrypointAuthoring::createDirectHandler(state,script,catalog.resources[0].entries[3],smores::script::ResourceRole::Base,state.project.ports.front().id,catalog));
}

TEST(SctEntrypointAuthoring, NewResourceSnapshotDoesNotRewriteImmutableScriptBaseline) {
    auto state=withResponseModule(); const auto script=state.project.scripts.front().id; auto catalog=entryCatalog(state);
    const auto originalBaseline=state.project.baselines.front().datasetFingerprint;
    state=SctEntrypointAuthoring::createDirectHandler(state,script,catalog.resources[0].entries[3],smores::script::ResourceRole::Base,state.project.ports.front().id,catalog).value();
    const auto entry=state.project.entrypoints.front();
    auto& resource=catalog.resources[0]; resource.identity.dataset=sha256(std::span<const std::byte>{}).value().toHex(); resource.identity.revision="updated";
    for(auto& e:resource.entries) e.resource=resource.identity;
    EXPECT_FALSE(hasErrors(SctEntrypointAuthoring::inspectBindings(state.project,script,catalog)));
    auto binding=*entry.binding; binding.provider=resource.entries[3];
    auto selected=SctEntrypointAuthoring::bind(state,script,entry.name,binding,catalog,entry.id); ASSERT_TRUE(selected);
    EXPECT_EQ(selected.value().project.baselines.front().datasetFingerprint,originalBaseline);
    EXPECT_EQ(selected.value().project.entrypoints.front().id,entry.id);
    EXPECT_TRUE(SctEntrypointAuthoring::inspectBindings(selected.value().project,script,catalog).empty());
    EXPECT_TRUE(SctAuthoringCodec::decode(SctAuthoringCodec::encode(selected.value().project).value()));
}

TEST(SctActivationTest, RequiredRootsAreEditableWithoutModulePortsButCannotBeDisabledOrRemoved) {
    auto state=initial(); const auto script=state.project.scripts.front().id;
    const auto original=output(state);
    for (const auto& section:state.programs.front()->document().sections) if(section.nameBytes=="init" || section.nameBytes=="loop") {
        auto declared=SctEntrypointAuthoring::declareNamed(state,script,section.id); ASSERT_TRUE(declared); state=declared.value();
        EXPECT_FALSE(SctEntrypointAuthoring::setEnabled(state,state.project.entrypoints.back().id,false));
    }
    EXPECT_EQ(output(state),original);
    auto session=SctAuthoringSession::open(state); ASSERT_TRUE(session);
    EXPECT_FALSE(session.value()->execute(state.project.revision,"Remove root",[](const auto& s) {
        auto next=s; next.project.entrypoints.erase(next.project.entrypoints.begin()); return Result<SctAuthoringState>::success(next);
    }));
    EXPECT_FALSE(session.value()->execute(state.project.revision,"Rename loop",[&](const auto& s) {
        auto next=s; auto w=SctAuthoringMaterializer::workingState(s.project,s.programs,script).value();
        auto d=std::make_shared<sct::SctDocument>(*w.document); d->sections.front().nameBytes="different"; w.document=d;
        auto changed=SctAuthoringMaterializer::replaceWorkingState(s.project,s.programs,script,w);
        if(!changed) return Result<SctAuthoringState>::failure(changed.diagnostics()); next.project=changed.value(); return Result<SctAuthoringState>::success(next);
    }));
}
TEST(SctActivationTest, MissingRootsRemainSaveableDraftsAndBlockExport) {
    auto state=initial(); const auto script=state.project.scripts.front().id;
    auto w=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value();
    auto d=std::make_shared<sct::SctDocument>(*w.document); std::erase_if(d->sections,[](const auto& s){return s.nameBytes=="init";}); w.document=d;
    state.project=SctAuthoringMaterializer::replaceWorkingState(state.project,state.programs,script,w).value();
    EXPECT_TRUE(SctAuthoringCodec::encode(state.project)); EXPECT_TRUE(SctAuthoringSession::open(state));
    auto result=SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(state.project),state.programs,{script}});
    ASSERT_FALSE(result.succeeded()); EXPECT_FALSE(result.scripts.front().prepared);
}
TEST(SctActivationTest, NamedDisableRetainsBodyAndInternalTargetsAndReenablesExactly) {
    auto state=initial(); const auto script=state.project.scripts.front().id; const auto section=state.programs.front()->document().sections[1].id;
    state=SctEntrypointAuthoring::declareNamed(state,script,section).value(); const auto id=state.project.entrypoints.back().id;
    const auto original=output(state); const auto before=state;
    auto session=SctAuthoringSession::open(state).takeValue();
    ASSERT_TRUE(session->execute(state.project.revision,"Disable",[&](const auto& s){return SctEntrypointAuthoring::setEnabled(s,id,false);})); state=session->state();
    auto working=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value();
    auto lowered=SctEntrypointAuthoring::lower(state.project,script,*working.document); ASSERT_TRUE(lowered);
    const auto& d=lowered.value().document;
    const auto wrapper=std::ranges::find(d.sections,std::string("called"),&sct::SctDocumentSection::nameBytes); ASSERT_NE(wrapper,d.sections.end());
    EXPECT_NE(wrapper->id,section); EXPECT_EQ(std::get<sct::SctScriptSectionContent>(wrapper->content).instructions.front().opcode,0);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(std::get<sct::SctScriptSectionContent>(d.sections.front().content).instructions[3].fixedParameters[0].value).target,
        std::get<sct::SctInstructionReference>(std::get<sct::SctScriptSectionContent>(working.document->sections.front().content).instructions[3].fixedParameters[0].value).target);
    const auto disabled=output(state); EXPECT_NE(disabled,original); EXPECT_EQ(output(state),disabled);
    auto encoded=SctAuthoringCodec::encode(state.project); ASSERT_TRUE(encoded); auto decoded=SctAuthoringCodec::decode(encoded.value()); ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded.value().find(id)->enabled); EXPECT_EQ(SctAuthoringCodec::encode(decoded.value()).value(),encoded.value());
    ASSERT_TRUE(session->undo()); EXPECT_EQ(output(session->state()),original); ASSERT_TRUE(session->redo()); EXPECT_EQ(output(session->state()),disabled);
    auto enabled=SctEntrypointAuthoring::setEnabled(state,id,true); ASSERT_TRUE(enabled); EXPECT_EQ(output(enabled.value()),original);
    auto materialized=SctAuthoringMaterializer::materialize({std::make_shared<const SctAuthoringProject>(state.project),state.programs,{script},SctAuthoringOutputMode::ReuseUnchangedSource});
    ASSERT_TRUE(materialized.succeeded()); EXPECT_FALSE(materialized.scripts.front().prepared->reusedSource);
    const auto& records=materialized.scripts.front().prepared->realization.records;
    const auto record=std::ranges::find(records,SctAuthoringEntityId{id},&SctRealizationRecord::entity); ASSERT_NE(record,records.end()); EXPECT_GT(record->locations.size(),2);
}
TEST(SctActivationTest, WholeActivationBypassesPreparationAndResponseWithoutChangingOtherEdges) {
    auto state=withResponseModule(); const auto script=state.project.scripts.front().id;
    state=SctSequenceAuthoring::promoteSequence(state,script,state.programs.front()->document().sections.front().id,"Arrival").value();
    const auto seq=state.project.sequences.back();
    SctEntrypointBinding b{SctEntrypointKind::Arrival,seq.id,sct::SctParameterSite{seq.actions[3].instruction,{0,{}}},sct::SctParameterSite{seq.actions[0].instruction,{0,{}}},state.project.ports.front().id};
    state=SctEntrypointAuthoring::bind(state,script,"Arrival",b,{}).value(); const auto id=state.project.entrypoints.back().id;
    SctLocalActivation edge{seq.actions[0].id,sct::SctParameterAddress{1,{}},seq.actions[2].id,seq.actions[4].id};
    state=SctEntrypointAuthoring::setActivation(state,id,edge).value(); state=SctEntrypointAuthoring::setEnabled(state,id,false).value();
    const auto w=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value();
    auto lowered=SctEntrypointAuthoring::lower(state.project,script,*w.document); ASSERT_TRUE(lowered);
    const auto& code=std::get<sct::SctScriptSectionContent>(lowered.value().document.sections.front().content).instructions;
    EXPECT_EQ(std::get<sct::SctInstructionReference>(code[0].fixedParameters[1].value).target,seq.actions[4].instruction);
    const auto& preparation=std::get<sct::SctScriptSectionContent>(w.document->sections.front().content).instructions[2];
    EXPECT_EQ(code[2].id,preparation.id); EXPECT_EQ(code[2].opcode,preparation.opcode); EXPECT_EQ(code[2].skipRefresh,preparation.skipRefresh);
    ASSERT_TRUE(code[2].scheduledExpression); EXPECT_EQ(sct::encodeSctCanonicalExpressionWords(*code[2].scheduledExpression),sct::encodeSctCanonicalExpressionWords(*preparation.scheduledExpression));
    EXPECT_FALSE(output(state).empty());
    auto duplicate=state; auto e=duplicate.project.entrypoints.back(); e.id=duplicate.project.allocate<SctEntrypointId>().value(); duplicate.project.entrypoints.push_back(e);
    EXPECT_FALSE(SctAuthoringSession::validate(duplicate));
    edge.operand.reset(); edge.source=seq.actions[1].id; // true fallthrough into preparation
    state=SctEntrypointAuthoring::setActivation(state,id,edge).value();
    lowered=SctEntrypointAuthoring::lower(state.project,script,*w.document); ASSERT_TRUE(lowered);
    const auto& fall=std::get<sct::SctScriptSectionContent>(lowered.value().document.sections.front().content).instructions;
    EXPECT_EQ(fall[2].opcode,10); EXPECT_TRUE(fall[2].skipRefresh); EXPECT_EQ(fall[3].id,seq.actions[2].instruction);
    edge={seq.actions[0].id,{},seq.actions[0].id,seq.actions[4].id,true};
    state=SctEntrypointAuthoring::setActivation(state,id,edge).value(); EXPECT_FALSE(output(state).empty());
}
TEST(SctActivationTest, UnresolvedDisabledBoundaryIsSaveableButCannotExport) {
    auto state=withResponseModule(); const auto script=state.project.scripts.front().id; const auto catalog=entryCatalog(state);
    state=SctEntrypointAuthoring::createDirectHandler(state,script,catalog.resources[0].entries[3],smores::script::ResourceRole::Base,state.project.ports.front().id,catalog).value();
    const auto id=state.project.entrypoints.back().id;
    state=SctEntrypointAuthoring::setActivation(state,id,SctUnresolvedBinding{"Manual draft","Choose the activation boundary.",{}}).value();
    state=SctEntrypointAuthoring::setEnabled(state,id,false).value();
    EXPECT_TRUE(SctAuthoringCodec::encode(state.project));
    auto w=SctAuthoringMaterializer::workingState(state.project,state.programs,script).value(); EXPECT_FALSE(SctEntrypointAuthoring::lower(state.project,script,*w.document));
}
TEST(SctActivationTest, SwitchOrdinalsOwnSeparateEdgesAndMayShareBodies) {
    auto state = initial(); const auto script = state.project.scripts.front().id;
    const auto& sections = state.programs.front()->document().sections;
    state = SctEntrypointAuthoring::declareNamed(state, script, sections.front().id).value();
    state = SctEntrypointAuthoring::declareNamed(state, script, sections[2].id).value();
    const auto loop = state.project.sequences.front(), dispatch = state.project.sequences.back();
    // Two independent case operands activate the same body.
    auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, script).value();
    auto d = std::make_shared<sct::SctDocument>(*w.document);
    auto& sw = std::get<sct::SctScriptSectionContent>(d->sections[2].content).instructions.front();
    sw.repeatedParameterGroups[1].parameters[1].value = sct::SctInstructionReference{loop.actions.front().instruction};
    w.document = d;
    state.project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, w).value();
    const auto first = state.project.entrypoints.back().id;
    state = SctEntrypointAuthoring::setActivation(state, first,
        SctLocalActivation{dispatch.actions.front().id, sct::SctParameterAddress{3,0}, loop.actions.front().id, loop.actions.back().id}).value();
    auto second = state.project.entrypoints.back(); second.id = state.project.allocate<SctEntrypointId>().value();
    std::get<SctLocalActivation>(second.activation).operand->repeatedGroupOrdinal = 1;
    state.project.entrypoints.push_back(second);
    ASSERT_TRUE(SctAuthoringSession::validate(state));
    state = SctEntrypointAuthoring::setEnabled(state, first, false).value();
    auto lowered = SctEntrypointAuthoring::lower(state.project, script, *d); ASSERT_TRUE(lowered);
    const auto& result = std::get<sct::SctScriptSectionContent>(lowered.value().document.sections[2].content).instructions.front();
    EXPECT_EQ(std::get<sct::SctInstructionReference>(result.repeatedParameterGroups[0].parameters[1].value).target, loop.actions.back().instruction);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(result.repeatedParameterGroups[1].parameters[1].value).target, loop.actions.front().instruction);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(result.repeatedParameterGroups[2].parameters[1].value).target,
        std::get<sct::SctInstructionReference>(sw.repeatedParameterGroups[2].parameters[1].value).target);
    EXPECT_FALSE(output(state).empty());
}
TEST(SctActivationTest, NamedWrapperPreservesPhysicalFallthroughIntoOriginalBody) {
    auto state = initial(); const auto script = state.project.scripts.front().id;
    auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, script).value();
    auto d = std::make_shared<sct::SctDocument>(*w.document);
    const auto predecessor = d->allocateInstructionId();
    d->sections.insert(d->sections.begin()+1, {d->allocateSectionId(), "prior", sct::SctScriptSectionContent{{
        {predecessor,16,false,{},{{0,expression("1")}}}}}});
    w.document = d; state.project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, w).value();
    state = SctEntrypointAuthoring::declareNamed(state, script, d->sections[2].id).value();
    const auto originalBody = state.project.sequences.back().actions.front().instruction;
    state = SctEntrypointAuthoring::setEnabled(state, state.project.entrypoints.back().id, false).value();
    auto lowered = SctEntrypointAuthoring::lower(state.project, script, *d); ASSERT_TRUE(lowered);
    const auto& prior = std::get<sct::SctScriptSectionContent>(lowered.value().document.sections[1].content).instructions;
    ASSERT_EQ(prior.size(),2); EXPECT_EQ(prior.front().id,predecessor); EXPECT_FALSE(prior.front().skipRefresh);
    EXPECT_EQ(prior.back().opcode,10); EXPECT_TRUE(prior.back().skipRefresh);
    EXPECT_EQ(std::get<sct::SctInstructionReference>(prior.back().fixedParameters[0].value).target,originalBody);
    EXPECT_FALSE(output(state).empty());
}
TEST(SctOverworldAuthoringTest, BatchAssignmentsCopiesAndUnknownsRemainOrderedAndUndoable) {
    auto state=promoted(); const auto seq=state.project.sequences.front(); const auto anchor=seq.actions.back().id;
    const auto original=output(state); auto session=SctAuthoringSession::open(state).takeValue();
    std::vector<SctSelectorEdit> edits{{{},7,expression("6")},{ {},45,expression("4")},{ {},6,expression("ByteVar[63]")}};
    ASSERT_TRUE(session->execute(state.project.revision,"Configure",[&](const auto& s){return SctOverworldAuthoring::edit(s,seq.id,anchor,edits);}));
    state=session->state(); const auto* after=state.project.find(seq.id); ASSERT_EQ(after->actions.size(),seq.actions.size()+3);
    auto preview=SctOverworldAuthoring::preview(state,seq.id,after->actions[4].id,after->actions[6].id,{{63,16}}); ASSERT_TRUE(preview);
    EXPECT_EQ(smores::script::selectTile("32",preview.value()).filename,"fiel32g.mld"); EXPECT_EQ(smores::script::selectOverworld(true,preview.value()).filename,"me099q.sct");
    auto unknown=SctOverworldAuthoring::preview(state,seq.id,after->actions[4].id,after->actions[6].id,{}); ASSERT_TRUE(unknown); EXPECT_FALSE(unknown.value()[6]);
    EXPECT_FALSE(SctOverworldAuthoring::preview(state,seq.id,after->actions.front().id,after->actions.back().id,{}));
    EXPECT_FALSE(output(state).empty()); ASSERT_TRUE(session->undo()); EXPECT_EQ(output(session->state()),original); ASSERT_TRUE(session->redo());
    EXPECT_EQ(output(session->state()),output(state));
    auto replacement=SctOverworldAuthoring::edit(state,seq.id,{},{{after->actions[4].id,7,expression("ByteVar[12]")}}); ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement.value().project.find(seq.id)->actions[4].id,after->actions[4].id);
}
TEST(SctOverworldAuthoringTest, ResourceAssociationsAreOptionalAndRoundTripIndependentlyOfBindings) {
    auto state=initial(); const auto script=state.project.scripts.front().id; auto resource=entryCatalog(state).resources.front();
    resource.role=smores::script::ResourceRole::StreamedTile; resource.identity.asset="mods/fiel32z.mld";
    auto next=SctEntrypointAuthoring::associateResource(state,script,resource); ASSERT_TRUE(next);
    EXPECT_EQ(output(next.value()),output(state)); auto encoded=SctAuthoringCodec::encode(next.value().project); ASSERT_TRUE(encoded);
    auto decoded=SctAuthoringCodec::decode(encoded.value()); ASSERT_TRUE(decoded); EXPECT_EQ(decoded.value().find(script)->resources.front().resource,resource.identity);
    EXPECT_FALSE(hasErrors(SctEntrypointAuthoring::inspectBindings(next.value().project,script,{})));
}
