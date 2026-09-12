#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <set>

namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string text) { return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(text)}; }
bool owns(const SctAuthoringModule& module, sct::SctSectionId id) { return std::ranges::find(module.sections, id) != module.sections.end(); }
}
Result<SctAuthoringState> SctModuleAuthoring::extract(const SctAuthoringState& state,
    SctScriptId script, std::vector<sct::SctSectionId> sections, std::string name) {
    if (sections.empty() || name.empty() || std::set(sections.begin(), sections.end()).size() != sections.size())
        return Result<SctAuthoringState>::failure(error("Choose unique sections and a module name."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    const auto index = sct::SctDocumentIndex::build(*working.value().document);
    for (const auto id : sections) {
        const auto* section = index.find(*working.value().document, id);
        if (!section || !std::holds_alternative<sct::SctScriptSectionContent>(section->content))
            return Result<SctAuthoringState>::failure(error("Extraction requires existing authored script sections."));
        for (const auto& module : state.project.modules) if (module.script == script && owns(module, id))
            return Result<SctAuthoringState>::failure(error("A selected section already belongs to another module."));
    }
    std::ranges::sort(sections, [&](auto a, auto b) { return index.sectionOrdinal(a) < index.sectionOrdinal(b); });
    auto next = state;
    for (const auto id : sections) {
        if (std::ranges::any_of(next.project.sequences, [&](const auto& seq) { return seq.script == script && seq.section == id; })) continue;
        auto promoted = SctSequenceAuthoring::promoteSequence(next, script, id, index.find(*working.value().document, id)->nameBytes);
        if (!promoted) return promoted;
        next = std::move(promoted).takeValue();
    }
    auto id = next.project.allocate<SctModuleId>();
    if (!id) return Result<SctAuthoringState>::failure(id.diagnostics());
    next.project.modules.push_back({id.value(), script, std::move(name), {},
        {{SctEvidenceKind::UserAssertion, "Explicit extraction; caller preparation and shared helpers retain their existing ownership."}}, std::move(sections)});
    const auto& module = next.project.modules.back();
    // Every action is addressable by a stable port. This supports calls into the
    // middle of a section without guessing a single original entry point.
    for (const auto& seq : next.project.sequences) if (seq.script == script && owns(module, seq.section)) {
        if (seq.actions.empty()) {
            auto port = next.project.allocate<SctPortId>(); if (!port) return Result<SctAuthoringState>::failure(port.diagnostics());
            next.project.ports.push_back({port.value(), id.value(), seq.name + " (empty entry)", {}, seq.id});
        }
        for (const auto& action : seq.actions) {
            auto port = next.project.allocate<SctPortId>(); if (!port) return Result<SctAuthoringState>::failure(port.diagnostics());
            next.project.ports.push_back({port.value(), id.value(), seq.name + " / " + std::to_string(action.id.value), action.id, seq.id});
        }
    }
    auto valid = SctAuthoringSession::validate(next);
    if (!valid) return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<SctAuthoringState> SctModuleAuthoring::createEmpty(const SctAuthoringState& state, SctScriptId script, std::string name) {
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    auto document = std::make_shared<sct::SctDocument>(*working.value().document);
    auto section = document->allocateSectionId();
    document->sections.push_back({section, "module_" + std::to_string(section.value()), sct::SctScriptSectionContent{}});
    working.value().document = document;
    auto project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working.value());
    if (!project) return Result<SctAuthoringState>::failure(project.diagnostics());
    auto next = state; next.project = std::move(project).takeValue();
    return extract(next, script, {section}, std::move(name));
}
Result<SctAuthoringState> SctModuleAuthoring::appendAction(const SctAuthoringState& state,
    SctModuleId id, SctSequenceId sequenceId, std::uint16_t opcode) {
    const auto* module = state.project.find(id); const auto* seq = state.project.find(sequenceId);
    if (!module || !seq || seq->script != module->script || !owns(*module, seq->section))
        return Result<SctAuthoringState>::failure(error("The sequence is not owned by this module."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, module->script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    auto document = std::make_shared<sct::SctDocument>(*working.value().document);
    auto draft = sct::SctInstructionFactory::createDraft({opcode});
    if (!draft.draft) return Result<SctAuthoringState>::failure(error("This action requires a configured parameter draft."));
    auto action = sct::SctInstructionFactory::materialize(*document, *draft.draft);
    if (!action.instruction) return Result<SctAuthoringState>::failure(error("Configure this action's required parameters before inserting it."));
    auto& section = *std::ranges::find(document->sections, seq->section, &sct::SctDocumentSection::id);
    std::get<sct::SctScriptSectionContent>(section.content).instructions.push_back(*action.instruction);
    working.value().document = document;
    auto project = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, module->script, working.value());
    if (!project) return Result<SctAuthoringState>::failure(project.diagnostics());
    auto next = state; next.project = std::move(project).takeValue();
    const auto& appended = next.project.find(sequenceId)->actions.back();
    auto empty = std::ranges::find_if(next.project.ports, [&](const auto& p) { return p.owner == SctPortOwner{id} && p.sequence == sequenceId && !p.action; });
    if (seq->actions.empty() && empty != next.project.ports.end()) empty->action = appended.id;
    else { auto port = next.project.allocate<SctPortId>(); if (!port) return Result<SctAuthoringState>::failure(port.diagnostics());
        next.project.ports.push_back({port.value(), id, seq->name + " / " + std::to_string(appended.id.value), appended.id, sequenceId}); }
    auto valid = SctAuthoringSession::validate(next); if (!valid) return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<SctModuleView> SctModuleAuthoring::inspect(const SctAuthoringState& state, SctModuleId id) {
    const auto* module = state.project.find(id);
    if (!module) return Result<SctModuleView>::failure(error("Module does not exist."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, module->script);
    if (!working) return Result<SctModuleView>::failure(working.diagnostics());
    const auto& document = *working.value().document;
    const auto index = sct::SctDocumentIndex::build(document);
    SctModuleView result{state.project.revision, id, module->script};
    for (const auto& seq : state.project.sequences) if (seq.script == module->script && owns(*module, seq.section)) result.sequences.push_back(seq.id);
    for (std::size_t ordinal = 0; ordinal < document.sections.size(); ++ordinal) {
        const auto& section = document.sections[ordinal];
        const auto* body = std::get_if<sct::SctScriptSectionContent>(&section.content); if (!body) continue;
        const bool inside = owns(*module, section.id);
        for (const auto& instruction : body->instructions) {
            if (inside && instruction.scheduledExpression)
                result.boundary.push_back({"Scheduled work: lifetime and caller state remain contextual", instruction.id, {}});
            const auto* schema = sct::findSctOpcodeSchema(instruction.opcode);
            if (inside && schema && schema->semantic.controlRole == sct::SctOpcodeControlRole::Return)
                result.boundary.push_back({"Return to caller", instruction.id, {}});
            const auto scan = [&](const auto& parameters) {
                for (const auto& parameter : parameters) {
                    if (const auto* ref = std::get_if<sct::SctInstructionReference>(&parameter.value)) {
                        const auto target = index.instructionLocation(ref->target);
                        if (target && inside != owns(*module, target->sectionId))
                            result.boundary.push_back({inside ? "External target / shared helper" : "Incoming caller / branch", instruction.id, ref->target});
                    } else if (inside && (std::holds_alternative<sct::SctStringReference>(parameter.value)
                        || std::holds_alternative<sct::SctSupplementaryTextReference>(parameter.value)))
                        result.boundary.push_back({"Uses script-owned text (sharing retained)", instruction.id, {}});
                    else if (inside && std::holds_alternative<sct::SctUnresolvedReferenceValue>(parameter.value))
                        result.boundary.push_back({"Unresolved dependency; explicit binding required for export", instruction.id, {}});
                    if (inside) if (const auto* expression = std::get_if<sct::SctCanonicalExpression>(&parameter.value))
                        for (const auto& read : SctSequenceAuthoring::stateReads(*expression))
                            result.boundary.push_back({"State access: bank " + std::to_string(static_cast<unsigned>(read.storage.kind))
                                + " index " + std::to_string(read.storage.index) + " view " + std::to_string(static_cast<unsigned>(read.access)), instruction.id, {}});
                }
            };
            scan(instruction.fixedParameters);
            for (const auto& group : instruction.repeatedParameterGroups) scan(group.parameters);
        }
        // Report adjacency as evidence, not proof of fallthrough. No relocation
        // or reordering is performed by extraction.
        if (!body->instructions.empty() && ordinal + 1 < document.sections.size()) {
            const auto& following = document.sections[ordinal + 1];
            const auto* code = std::get_if<sct::SctScriptSectionContent>(&following.content);
            if (code && !code->instructions.empty() && inside != owns(*module, following.id))
                result.boundary.push_back({"Adjacent section boundary (possible continuation; assembly order retained)", body->instructions.back().id, code->instructions.front().id});
        }
    }
    return Result<SctModuleView>::success(std::move(result));
}
} // namespace salsa::core
