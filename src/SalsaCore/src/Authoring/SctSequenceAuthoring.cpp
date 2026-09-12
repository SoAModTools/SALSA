#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctScptEncoding.h"
#include <charconv>
#include <map>

namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)};
}
const sct::SctScriptSectionContent* section(const sct::SctDocument& document, sct::SctSectionId id) {
    const auto found = std::ranges::find(document.sections, id, &sct::SctDocumentSection::id);
    return found == document.sections.end() ? nullptr : std::get_if<sct::SctScriptSectionContent>(&found->content);
}
const sct::SctCanonicalExpression* condition(const sct::SctDocument& document, const sct::SctParameterSite& site) {
    const auto index = sct::SctDocumentIndex::build(document);
    const auto* instruction = index.find(document, site.instruction);
    if (!instruction || site.parameter.schemaIndex != 0 || site.parameter.repeatedGroupOrdinal) return nullptr;
    const auto* schema = sct::findSctOpcodeSchema(instruction->opcode);
    if (!schema || schema->semantic.controlRole != sct::SctOpcodeControlRole::Branch) return nullptr;
    const auto found = std::ranges::find(instruction->fixedParameters, 0u, &sct::SctDocumentParameter::schemaIndex);
    return found == instruction->fixedParameters.end() ? nullptr : std::get_if<sct::SctCanonicalExpression>(&found->value);
}
bool editable(const sct::SctCanonicalExpression& expression) {
    return SctExpressionLanguage::project(expression).availability == SctExpressionTextAvailability::Editable;
}
Result<SctAuthoringState> changed(const SctAuthoringState& state, SctScriptId script,
    SctSemanticState working, const SctSemanticOperationBatch& batch) {
    auto applied = SctSemanticOperationService::apply(*working.document, batch);
    if (!applied.succeeded()) return Result<SctAuthoringState>::failure(error(applied.issues.empty()
        ? "Semantic action failed physical preflight." : applied.issues.front().message));
    working.document = applied.document;
    auto project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working);
    if (!project) return Result<SctAuthoringState>::failure(project.diagnostics());
    // Valid drafts may still need text/reference repair before export. Output
    // readiness is assessed by materialization, not by accepting a command.
    return Result<SctAuthoringState>::success({std::move(project).takeValue(), state.programs});
}
}
Result<void> SctSequenceAuthoring::validateProgram(const SctAuthoringProject& project, SctScriptId script,
    const sct::SctDocument& document) {
    for (const auto& sequence : project.sequences) if (sequence.script == script) {
        const auto* body = section(document, sequence.section);
        if (!body || body->instructions.size() != sequence.actions.size())
            return Result<void>::failure(error("Sequence binding no longer covers its exact section. Remove the definition before deleting its section."));
        for (std::size_t i = 0; i < sequence.actions.size(); ++i)
            if (sequence.actions[i].instruction != body->instructions[i].id)
                return Result<void>::failure(error("Sequence action order differs from its program."));
    }
    for (const auto& definition : project.predicates) if (definition.script == script) {
        std::optional<std::vector<std::uint32_t>> expected;
        for (const auto& site : definition.uses) {
            const auto* expression = condition(document, site);
            if (!expression || !editable(*expression))
                return Result<void>::failure(error("Named condition use is missing or no longer a conventional branch condition. Remove its binding before changing its structure."));
            auto words = sct::encodeSctCanonicalExpressionWords(*expression);
            if (expected && *expected != words)
                return Result<void>::failure(error("Named condition uses disagree. Edit them together through the named condition."));
            expected = std::move(words);
        }
    }
    return Result<void>::success();
}
Result<void> SctSequenceAuthoring::reconcileActions(SctAuthoringProject& project, SctScriptId script,
    const sct::SctDocument& document) {
    std::map<sct::SctInstructionId, SctSequenceAction> existing;
    for (const auto& sequence : project.sequences) if (sequence.script == script)
        for (const auto& action : sequence.actions) existing.emplace(action.instruction, action);
    for (auto& sequence : project.sequences) if (sequence.script == script) {
        const auto* body = section(document, sequence.section);
        if (!body) return Result<void>::failure(error("Remove the sequence definition before deleting its section."));
        std::vector<SctSequenceAction> actions;
        for (const auto& instruction : body->instructions) {
            const auto found = existing.find(instruction.id);
            if (found != existing.end()) actions.push_back(found->second);
            else {
                auto id = project.allocate<SctActionId>();
                if (!id) return Result<void>::failure(id.diagnostics());
                actions.push_back({id.value(), instruction.id});
            }
        }
        sequence.actions = std::move(actions);
    }
    return validateProgram(project, script, document);
}
Result<SctAuthoringState> SctSequenceAuthoring::promoteSequence(const SctAuthoringState& state,
    SctScriptId script, sct::SctSectionId id, std::string name) {
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    if (!section(*working.value().document, id)) return Result<SctAuthoringState>::failure(error("Only an imported script section can be promoted."));
    auto next = state;
    auto allocated = next.project.allocate<SctSequenceId>();
    if (!allocated) return Result<SctAuthoringState>::failure(allocated.diagnostics());
    const auto& baseline = *state.project.find(state.project.find(script)->baseline);
    next.project.sequences.push_back({allocated.value(), script, std::move(name), {baseline.id, baseline.importedDocument}, id, {}});
    auto reconciled = reconcileActions(next.project, script, *working.value().document);
    if (!reconciled) return Result<SctAuthoringState>::failure(reconciled.diagnostics());
    auto valid = SctAuthoringSession::validate(next);
    if (!valid) return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<std::vector<SctSequenceActionView>> SctSequenceAuthoring::actions(const SctAuthoringState& state, SctSequenceId id) {
    const auto* sequence = state.project.find(id);
    if (!sequence) return Result<std::vector<SctSequenceActionView>>::failure(error("Sequence is missing."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, sequence->script);
    if (!working) return Result<std::vector<SctSequenceActionView>>::failure(working.diagnostics());
    const auto index = sct::SctDocumentIndex::build(*working.value().document);
    std::vector<SctSequenceActionView> result;
    for (const auto& action : sequence->actions) {
        const auto& instruction = *index.find(*working.value().document, action.instruction);
        SctSequenceActionView view{action.id, instruction};
        const auto* schema = sct::findSctOpcodeSchema(instruction.opcode);
        if (schema) {
            view.label = schema->semantic.mnemonic;
            switch (schema->semantic.controlRole) {
            case sct::SctOpcodeControlRole::Branch: view.kind = SctSequenceActionKind::Branch; break;
            case sct::SctOpcodeControlRole::Switch: view.kind = SctSequenceActionKind::Switch; break;
            case sct::SctOpcodeControlRole::Jump: view.kind = SctSequenceActionKind::Jump; break;
            case sct::SctOpcodeControlRole::CallSubscript: view.kind = SctSequenceActionKind::Call; break;
            case sct::SctOpcodeControlRole::Return: view.kind = SctSequenceActionKind::Return; break;
            default: view.kind = instruction.opcode == 16 ? SctSequenceActionKind::Wait : SctSequenceActionKind::Action;
            }
        }
        if (view.label.empty()) view.label = "Preserved opcode " + std::to_string(instruction.opcode);
        const auto targets = [&](const auto& parameters) {
            for (const auto& parameter : parameters)
                if (const auto* target = std::get_if<sct::SctInstructionReference>(&parameter.value)) view.targets.push_back(target->target);
        };
        targets(instruction.fixedParameters);
        for (const auto& group : instruction.repeatedParameterGroups) targets(group.parameters);
        result.push_back(std::move(view));
    }
    return Result<std::vector<SctSequenceActionView>>::success(std::move(result));
}
Result<SctAuthoringState> SctSequenceAuthoring::setTiming(const SctAuthoringState& state, SctSequenceId sequenceId,
    SctActionId actionId, std::optional<sct::SctCanonicalExpression> schedule, bool skipRefresh) {
    const auto* sequence = state.project.find(sequenceId);
    if (!sequence) return Result<SctAuthoringState>::failure(error("Sequence is missing."));
    const auto found = std::ranges::find(sequence->actions, actionId, &SctSequenceAction::id);
    if (found == sequence->actions.end()) return Result<SctAuthoringState>::failure(error("Action belongs to another sequence."));
    if (schedule && !editable(*schedule)) return Result<SctAuthoringState>::failure(error("Use the preserved editor for an unconventional schedule."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, sequence->script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    const auto index = sct::SctDocumentIndex::build(*working.value().document);
    auto replacement = *index.find(*working.value().document, found->instruction);
    replacement.scheduledExpression = std::move(schedule); replacement.skipRefresh = skipRefresh;
    return changed(state, sequence->script, working.value(), {{{SctReplaceInstructionOperation{found->instruction, replacement}}}});
}
Result<SctAuthoringState> SctSequenceAuthoring::namePredicate(const SctAuthoringState& state, SctScriptId script,
    std::vector<sct::SctParameterSite> uses, std::string name) {
    const auto* context = state.project.find(script);
    if (!context || !state.project.find(context->baseline)) return Result<SctAuthoringState>::failure(error("Script or baseline is missing."));
    auto next = state;
    auto id = next.project.allocate<SctPredicateId>();
    if (!id) return Result<SctAuthoringState>::failure(id.diagnostics());
    const auto& baseline = *state.project.find(context->baseline);
    next.project.predicates.push_back({id.value(), script, std::move(name), {baseline.id, baseline.importedDocument}, std::move(uses)});
    auto valid = SctAuthoringSession::validate(next);
    if (!valid) return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<sct::SctCanonicalExpression> SctSequenceAuthoring::predicate(const SctAuthoringState& state, SctPredicateId id) {
    const auto* definition = state.project.find(id);
    if (!definition) return Result<sct::SctCanonicalExpression>::failure(error("Named condition is missing."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, definition->script);
    if (!working) return Result<sct::SctCanonicalExpression>::failure(working.diagnostics());
    return Result<sct::SctCanonicalExpression>::success(*condition(*working.value().document, definition->uses.front()));
}
Result<SctAuthoringState> SctSequenceAuthoring::editPredicate(const SctAuthoringState& state,
    SctPredicateId id, const sct::SctCanonicalExpression& expression) {
    const auto* definition = state.project.find(id);
    if (!definition || !editable(expression)) return Result<SctAuthoringState>::failure(error("A named condition requires a conventional expression."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, definition->script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    SctSemanticOperationBatch batch;
    for (const auto& site : definition->uses) batch.operations.push_back(SctReplaceParameterValueOperation{site, expression});
    return changed(state, definition->script, working.value(), batch);
}
std::vector<SctStateRead> SctSequenceAuthoring::stateReads(const sct::SctCanonicalExpression& expression) {
    const auto* program = std::get_if<sct::SctTypedScptProgram>(&expression.body);
    std::vector<SctStateRead> result;
    if (!program) return result;
    for (std::size_t i = 0; i < program->operations.size(); ++i) {
        const auto* value = std::get_if<sct::SctScptValueOperation>(&program->operations[i]);
        if (!value) continue;
        std::optional<SctVariableKind> storage;
        switch (value->kind) {
        case sct::SctScptValueKind::BitVariable: storage = SctVariableKind::Bit; break;
        case sct::SctScptValueKind::ByteVariable: storage = SctVariableKind::Byte; break;
        case sct::SctScptValueKind::FloatVariable: storage = SctVariableKind::Float; break;
        case sct::SctScptValueKind::IntegerVariable:
        case sct::SctScptValueKind::IntegerVariableLow16Comparison:
        case sct::SctScptValueKind::FloatBackedIntegerVariable: storage = SctVariableKind::Integer; break;
        default: break;
        }
        if (storage) result.push_back({{*storage, value->encodingWord & 0xffffffu}, value->kind, static_cast<std::uint32_t>(i)});
    }
    return result;
}
Result<SctArrivalLocation> SctSequenceAuthoring::arrivalLocation(std::string stem) {
    if (stem.size() != 6 || stem.substr(0, 2) != "me" || stem[5] < 'a' || stem[5] > 'j')
        return Result<SctArrivalLocation>::failure(error("Use an unambiguous me000a–me999j location. Later letters and special return values are unqualified."));
    std::uint32_t number = 0;
    const auto parsed = std::from_chars(stem.data() + 2, stem.data() + 5, number);
    if (parsed.ec != std::errc{} || parsed.ptr != stem.data() + 5)
        return Result<SctArrivalLocation>::failure(error("Location requires three decimal digits."));
    return Result<SctArrivalLocation>::success({stem, number * 10 + static_cast<unsigned>(stem[5] - 'a')});
}
Result<sct::SctCanonicalExpression> SctSequenceAuthoring::arrivalCondition(std::string stem) {
    auto location = arrivalLocation(std::move(stem));
    if (!location) return Result<sct::SctCanonicalExpression>::failure(location.diagnostics());
    auto expression = SctExpressionLanguage::parse("Low16IntVar[15] == " + std::to_string(location.value().encoded));
    if (!expression.succeeded()) return Result<sct::SctCanonicalExpression>::failure(error("Arrival expression cannot be represented."));
    return Result<sct::SctCanonicalExpression>::success(*expression.expression);
}
Result<SctAuthoringState> SctSequenceAuthoring::selectArrival(const SctAuthoringState& state, SctPredicateId id, std::string stem) {
    auto current = predicate(state, id);
    if (!current) return Result<SctAuthoringState>::failure(current.diagnostics());
    const auto* program = std::get_if<sct::SctTypedScptProgram>(&current.value().body);
    if (!program || program->operations.size() != 3)
        return Result<SctAuthoringState>::failure(error("Arrival picker requires a simple existing Low16IntVar[15] comparison; edit compound conditions explicitly."));
    const auto* read = std::get_if<sct::SctScptValueOperation>(&program->operations[0]);
    const auto* literal = std::get_if<sct::SctScptValueOperation>(&program->operations[1]);
    const auto* op = std::get_if<sct::SctScptBinaryOperation>(&program->operations[2]);
    if (!read || read->kind != sct::SctScptValueKind::IntegerVariableLow16Comparison || (read->encodingWord & 0xffffffu) != 15
        || !literal || literal->kind != sct::SctScptValueKind::DecimalLiteral || !op || sct::sctScptOperatorSymbol(op->encodingWord) != "==")
        return Result<SctAuthoringState>::failure(error("This condition is not a qualified previous-location equality."));
    auto replacement = arrivalCondition(std::move(stem));
    if (!replacement) return Result<SctAuthoringState>::failure(replacement.diagnostics());
    // Keep imported read/operator encodings and termination. Only the literal changes.
    auto expression = current.value();
    std::get<sct::SctTypedScptProgram>(expression.body).operations[1] = std::get<sct::SctTypedScptProgram>(replacement.value().body).operations[1];
    return editPredicate(state, id, expression);
}
std::string SctSequenceAuthoring::specialArrivalMeaning(std::uint32_t encoded) {
    if (encoded == 10000) return "Battle return (tentative user interpretation)";
    if (encoded == 40000) return "Ship battle return (tentative user interpretation)";
    return "Unresolved arrival value";
}
Result<SctAuthoringState> SctSequenceAuthoring::selectArrivalCase(const SctAuthoringState& state,
    SctScriptId script, sct::SctInstructionId controller, std::uint32_t ordinal, std::string stem) {
    auto location = arrivalLocation(std::move(stem));
    if (!location) return Result<SctAuthoringState>::failure(location.diagnostics());
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    const auto index = sct::SctDocumentIndex::build(*working.value().document);
    const auto* instruction = index.find(*working.value().document, controller);
    if (!instruction || instruction->opcode != 3 || ordinal >= instruction->repeatedParameterGroups.size())
        return Result<SctAuthoringState>::failure(error("Arrival case must identify an existing switch case."));
    const auto selector = std::ranges::find(instruction->fixedParameters, 0u, &sct::SctDocumentParameter::schemaIndex);
    const auto* expression = selector == instruction->fixedParameters.end() ? nullptr : std::get_if<sct::SctCanonicalExpression>(&selector->value);
    const auto expected = SctExpressionLanguage::parse("Low16IntVar[15]");
    if (!expression || sct::encodeSctCanonicalExpressionWords(*expression) != sct::encodeSctCanonicalExpressionWords(*expected.expression))
        return Result<SctAuthoringState>::failure(error("Arrival case selection requires the exact Low16IntVar[15] selector."));
    for (std::uint32_t i = 0; i < instruction->repeatedParameterGroups.size(); ++i) {
        const auto& parameters = instruction->repeatedParameterGroups[i].parameters;
        const auto parameter = std::ranges::find(parameters, 2u, &sct::SctDocumentParameter::schemaIndex);
        const auto* value = parameter == parameters.end() ? nullptr : std::get_if<sct::SctEncodedWordValue>(&parameter->value);
        if (!value || (i == ordinal && value->value == UINT32_MAX))
            return Result<SctAuthoringState>::failure(error("Default or opaque switch cases cannot become a location through this picker."));
        if (i != ordinal && value->value == location.value().encoded)
            return Result<SctAuthoringState>::failure(error("Another switch case already handles this location."));
    }
    return changed(state, script, working.value(), {{{SctReplaceParameterValueOperation{
        {controller, {2, ordinal}}, sct::SctEncodedWordValue{location.value().encoded}}}}});
}
} // namespace salsa::core
