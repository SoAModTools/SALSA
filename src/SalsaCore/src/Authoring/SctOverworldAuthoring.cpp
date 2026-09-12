#include "SalsaCore/Authoring/SctOverworldAuthoring.h"
#include <set>
namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string text) { return {DiagnosticSeverity::Error,DiagnosticCode::InvalidSctAuthoringProject,std::move(text)}; }
}
Result<SctAuthoringState> SctOverworldAuthoring::edit(const SctAuthoringState& state, SctSequenceId id,
    std::optional<SctActionId> before, const std::vector<SctSelectorEdit>& edits) {
    const auto* seq = state.project.find(id); if (!seq) return Result<SctAuthoringState>::failure(error("Select a sequence."));
    auto working = SctAuthoringMaterializer::workingState(state.project,state.programs,seq->script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    auto document = std::make_shared<sct::SctDocument>(*working.value().document);
    auto section = std::ranges::find(document->sections,seq->section,&sct::SctDocumentSection::id);
    auto& code = std::get<sct::SctScriptSectionContent>(section->content).instructions;
    const auto resolve = [&](SctActionId action) -> std::optional<sct::SctInstructionId> {
        const auto found = std::ranges::find(seq->actions,action,&SctSequenceAction::id);
        return found == seq->actions.end() ? std::nullopt : std::optional{found->instruction};
    };
    const auto anchor = before ? resolve(*before) : std::nullopt;
    if (before && !anchor) return Result<SctAuthoringState>::failure(error("Insertion anchor belongs to another sequence."));
    std::set<SctActionId> replaced;
    for (const auto& edit : edits) {
        if (edit.destination > 0x7fff) return Result<SctAuthoringState>::failure(error("Byte-variable destination is outside the supported index range."));
        if (edit.replace) {
            const auto instruction = resolve(*edit.replace);
            if (!instruction || !replaced.insert(*edit.replace).second) return Result<SctAuthoringState>::failure(error("Missing or duplicate replacement action."));
            auto i = std::ranges::find(code,*instruction,&sct::SctDocumentInstruction::id);
            if (i == code.end() || !smores::script::byteAssignment(*i)) return Result<SctAuthoringState>::failure(error("Only a byte assignment can be replaced."));
            for (auto& p : i->fixedParameters) {
                if (p.schemaIndex == 0) p.value = sct::SctEncodedWordValue{0x10000000|edit.destination};
                if (p.schemaIndex == 1) p.value = edit.expression;
            }
        } else {
            if (!anchor) return Result<SctAuthoringState>::failure(error("Choose an insertion action; appending past a terminator is not inferred."));
            const auto at = std::ranges::find(code,*anchor,&sct::SctDocumentInstruction::id);
            code.insert(at,{document->allocateInstructionId(),5,true,{},{{0,sct::SctEncodedWordValue{0x10000000|edit.destination}},{1,edit.expression}}});
        }
    }
    working.value().document = document;
    auto updated = SctAuthoringMaterializer::replaceWorkingState(state.project,state.programs,seq->script,working.value());
    if (!updated) return Result<SctAuthoringState>::failure(updated.diagnostics());
    auto next = state; next.project = std::move(updated).takeValue();
    auto valid = SctAuthoringSession::validate(next);
    return valid ? Result<SctAuthoringState>::success(std::move(next)) : Result<SctAuthoringState>::failure(valid.diagnostics());
}
Result<smores::script::KnownBytes> SctOverworldAuthoring::preview(const SctAuthoringState& state, SctSequenceId id,
    SctActionId first, SctActionId last, smores::script::KnownBytes values) {
    auto actions = SctSequenceAuthoring::actions(state,id);
    if (!actions) return Result<smores::script::KnownBytes>::failure(actions.diagnostics());
    const auto begin = std::ranges::find(actions.value(),first,&SctSequenceActionView::id);
    const auto end = std::ranges::find(actions.value(),last,&SctSequenceActionView::id);
    if (begin == actions.value().end() || end == actions.value().end() || end < begin)
        return Result<smores::script::KnownBytes>::failure(error("Select an ordered range in one sequence."));
    for (auto at = begin; at != end+1; ++at) {
        const auto& i = at->instruction;
        const auto assignment = smores::script::byteAssignment(i);
        if (assignment) values[assignment->destination] = i.scheduledExpression ? std::nullopt : smores::script::evaluateByte(assignment->expression,values);
        else if (i.opcode != 15 && i.opcode != 16)
            return Result<smores::script::KnownBytes>::failure(error("Preview accepts straight-line byte assignments and waits only; narrow the range around branches, calls, or other effects."));
    }
    return Result<smores::script::KnownBytes>::success(std::move(values));
}
}
