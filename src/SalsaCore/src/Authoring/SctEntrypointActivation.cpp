#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <set>

namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string text) { return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(text)}; }
bool required(std::string_view name) { return name == "init" || name == "loop"; }
std::string privateLabel(std::uint64_t value) {
    std::string digits; do { digits += "0123456789abcdefghijklmnopqrstuvwxyz"[value%36]; value/=36; } while(value);
    std::ranges::reverse(digits); return "_s"+digits; // at most 15 bytes for a uint64
}
const sct::SctDocumentParameter* parameter(const sct::SctDocumentInstruction& i, const sct::SctParameterAddress& a) {
    if (a.repeatedGroupOrdinal && *a.repeatedGroupOrdinal >= i.repeatedParameterGroups.size()) return nullptr;
    const auto& ps = a.repeatedGroupOrdinal ? i.repeatedParameterGroups[*a.repeatedGroupOrdinal].parameters : i.fixedParameters;
    const auto found = std::ranges::find(ps, a.schemaIndex, &sct::SctDocumentParameter::schemaIndex);
    return found == ps.end() ? nullptr : &*found;
}
bool hasFallthrough(const sct::SctDocumentInstruction& i) {
    const auto* schema = sct::findSctOpcodeSchema(i.opcode);
    return schema && schema->semantic.controlRole != sct::SctOpcodeControlRole::Jump
        && schema->semantic.controlRole != sct::SctOpcodeControlRole::Return;
}
std::optional<sct::SctInstructionId> instruction(const SctAuthoringProject& p, SctScriptId script, SctActionId id) {
    for (const auto& seq : p.sequences) if (seq.script == script)
        for (const auto& a : seq.actions) if (a.id == id) return a.instruction;
    return {};
}
Result<SctAuthoringState> checked(SctAuthoringState next) {
    auto valid = SctAuthoringSession::validate(next);
    return valid ? Result<SctAuthoringState>::success(std::move(next), valid.diagnostics()) : Result<SctAuthoringState>::failure(valid.diagnostics());
}
}

Result<void> SctEntrypointAuthoring::validateActivations(const SctAuthoringProject& p, SctScriptId script, const sct::SctDocument& d) {
    const auto index = sct::SctDocumentIndex::build(d);
    std::set<std::string> names;
    std::set<std::tuple<SctActionId, std::optional<sct::SctParameterAddress>,bool>> edges;
    for (const auto& e : p.entrypoints) if (e.script == script) {
        if (const auto* a = std::get_if<SctNamedActivation>(&e.activation)) {
            const auto body = instruction(p, script, a->body);
            const auto section = std::ranges::find(d.sections, a->externalName, &sct::SctDocumentSection::nameBytes);
            const auto* code = section == d.sections.end() ? nullptr : std::get_if<sct::SctScriptSectionContent>(&section->content);
            const auto* sequence = e.binding ? p.find(e.binding->sequence) : nullptr;
            if (!body || !code || code->instructions.empty() || code->instructions.front().id != *body || !names.insert(a->externalName).second)
                return Result<void>::failure(error("Named activation must uniquely own its current section entry."));
            if (sequence && sequence->section != section->id)
                return Result<void>::failure(error("Named activation and response binding must describe the same section."));
            if (required(a->externalName) && !e.enabled) return Result<void>::failure(error("init and loop are always enabled."));
        } else if (const auto* a = std::get_if<SctLocalActivation>(&e.activation)) {
            const auto source = instruction(p, script, a->source), start = instruction(p, script, a->start), continuation = instruction(p, script, a->continuation);
            if (!source || !start || !continuation || *start == *continuation || !index.find(d,*continuation) || !index.find(d,*start))
                return Result<void>::failure(error("Activation actions must resolve in the owning script, with a distinct bypass continuation."));
            const auto* from = index.find(d,*source);
            if (!from || !edges.emplace(a->source,a->operand,a->lifecycleEntry).second) return Result<void>::failure(error("Missing or multiply owned activation edge."));
            if (a->lifecycleEntry) {
                const auto loc = index.instructionLocation(*start);
                const auto* section = loc ? index.find(d,loc->sectionId) : nullptr;
                if (a->operand || a->source != a->start || !section || !required(section->nameBytes) || loc->instructionOrdinal != 0)
                    return Result<void>::failure(error("Lifecycle route must start at the first init/loop action."));
            } else if (a->operand) {
                const auto* param = parameter(*from,*a->operand);
                const auto* target = param ? std::get_if<sct::SctInstructionReference>(&param->value) : nullptr;
                if (!target || target->target != *start) return Result<void>::failure(error("Activation operand no longer targets the selected block start."));
            } else {
                const auto loc = index.instructionLocation(*source);
                const auto si = std::ranges::find(d.sections,loc->sectionId,&sct::SctDocumentSection::id);
                const auto& code = std::get<sct::SctScriptSectionContent>(si->content);
                std::optional<sct::SctInstructionId> following;
                if (loc->instructionOrdinal + 1 < code.instructions.size()) following = code.instructions[loc->instructionOrdinal+1].id;
                else if (si + 1 != d.sections.end()) if (const auto* next = std::get_if<sct::SctScriptSectionContent>(&(si+1)->content); next && !next->instructions.empty()) following = next->instructions.front().id;
                if (!hasFallthrough(*from) || following != start) return Result<void>::failure(error("Activation is not the selected physical fallthrough edge."));
            }
        }
    }
    return Result<void>::success();
}

Result<SctEntrypointAuthoring::Lowered> SctEntrypointAuthoring::lower(const SctAuthoringProject& p, SctScriptId script, const sct::SctDocument& authored) {
    if (!p.find(script)) return Result<Lowered>::failure(error("Captured activation script is missing from the project."));
    auto valid = validateProgram(p,script,authored,true);
    if (!valid) return Result<Lowered>::failure(valid.diagnostics());
    Lowered result{authored,{}};
    auto& d = result.document;
    std::vector<const SctAuthoringEntrypoint*> entries;
    for (const auto& e : p.entrypoints) if (e.script == script && !e.enabled) entries.push_back(&e);
    std::ranges::sort(entries, [](const auto* a, const auto* b) { return a->id < b->id; });
    for (const auto* e : entries) {
        SctRealizationRecord record{e->id,{}};
        if (const auto* a = std::get_if<SctLocalActivation>(&e->activation)) {
            const auto source = *instruction(p,script,a->source), destination = *instruction(p,script,a->continuation);
            for (auto& section : d.sections) if (auto* code = std::get_if<sct::SctScriptSectionContent>(&section.content)) {
                auto from = std::ranges::find(code->instructions,source,&sct::SctDocumentInstruction::id);
                if (from == code->instructions.end()) continue;
                if (a->lifecycleEntry) {
                    const auto position = static_cast<std::size_t>(&section-d.sections.data());
                    if (position) if (auto* prior = std::get_if<sct::SctScriptSectionContent>(&d.sections[position-1].content);
                        prior && !prior->instructions.empty() && hasFallthrough(prior->instructions.back())) {
                        const auto jump = d.allocateInstructionId();
                        prior->instructions.push_back({jump,10,true,{},{{0,sct::SctInstructionReference{source}}}});
                        record.locations.emplace_back(sct::SctDocumentEntityId{jump});
                    }
                    const auto jump = d.allocateInstructionId();
                    code->instructions.insert(from,{jump,10,true,{},{{0,sct::SctInstructionReference{destination}}}});
                    record.locations.emplace_back(sct::SctDocumentEntityId{jump});
                } else if (a->operand) {
                    const_cast<sct::SctDocumentParameter*>(parameter(*from,*a->operand))->value = sct::SctInstructionReference{destination};
                    record.locations.emplace_back(sct::SctParameterSite{source,*a->operand});
                } else {
                    const auto jump = d.allocateInstructionId();
                    code->instructions.insert(from+1,{jump,10,true,{},{{0,sct::SctInstructionReference{destination}}}});
                    record.locations.emplace_back(sct::SctDocumentEntityId{jump});
                }
                break;
            }
        } else if (const auto* a = std::get_if<SctNamedActivation>(&e->activation)) {
            auto section = std::ranges::find(d.sections,a->externalName,&sct::SctDocumentSection::nameBytes);
            const auto position = static_cast<std::size_t>(section-d.sections.begin());
            const auto body = *instruction(p,script,a->body);
            // Physical predecessors continue into the original body, never the
            // newly introduced external-only wrapper.
            if (position) if (auto* prior = std::get_if<sct::SctScriptSectionContent>(&d.sections[position-1].content);
                prior && !prior->instructions.empty() && hasFallthrough(prior->instructions.back())) {
                const auto jump = d.allocateInstructionId();
                prior->instructions.push_back({jump,10,true,{},{{0,sct::SctInstructionReference{body}}}});
                record.locations.emplace_back(sct::SctDocumentEntityId{jump});
            }
            auto nonce=e->id.value; std::string privateName = privateLabel(nonce);
            while (std::ranges::any_of(d.sections,[&](const auto& s) { return s.nameBytes == privateName; })) privateName = privateLabel(++nonce);
            section->nameBytes = privateName;
            const auto wrapper = d.allocateSectionId();
            const auto guard = d.allocateInstructionId(), ret = d.allocateInstructionId();
            auto no = SctExpressionLanguage::parse("0");
            if (!no.succeeded()) return Result<Lowered>::failure(error("Cannot construct inactive entry condition."));
            d.sections.insert(d.sections.begin()+position,{wrapper,a->externalName,sct::SctScriptSectionContent{{
                {guard,0,true,{},{{0,*no.expression},{1,sct::SctInstructionReference{ret}}}}, {ret,12}}}});
            record.locations.emplace_back(sct::SctDocumentEntityId{wrapper});
            record.locations.emplace_back(sct::SctDocumentEntityId{guard});
            record.locations.emplace_back(sct::SctDocumentEntityId{ret});
        } else return Result<Lowered>::failure(error("Disabled entrypoint needs an explicit activation boundary before export: " + e->name));
        result.generated.push_back(std::move(record));
    }
    return Result<Lowered>::success(std::move(result));
}

Result<SctAuthoringState> SctEntrypointAuthoring::setEnabled(const SctAuthoringState& state, SctEntrypointId id, bool enabled) {
    auto next = state; const auto found = std::ranges::find(next.project.entrypoints,id,&SctAuthoringEntrypoint::id);
    if (found == next.project.entrypoints.end()) return Result<SctAuthoringState>::failure(error("Entrypoint is missing."));
    found->enabled = enabled;
    return checked(std::move(next));
}
Result<SctAuthoringState> SctEntrypointAuthoring::setActivation(const SctAuthoringState& state, SctEntrypointId id, SctEntrypointActivation activation) {
    auto next = state; const auto found = std::ranges::find(next.project.entrypoints,id,&SctAuthoringEntrypoint::id);
    if (found == next.project.entrypoints.end()) return Result<SctAuthoringState>::failure(error("Entrypoint is missing."));
    found->activation = std::move(activation);
    return checked(std::move(next));
}
Result<SctAuthoringState> SctEntrypointAuthoring::declareNamed(const SctAuthoringState& state, SctScriptId script, sct::SctSectionId section) {
    auto next = state;
    auto working = SctAuthoringMaterializer::workingState(next.project,next.programs,script);
    if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    const auto index = sct::SctDocumentIndex::build(*working.value().document);
    const auto* s = index.find(*working.value().document,section);
    const auto* body = s ? std::get_if<sct::SctScriptSectionContent>(&s->content) : nullptr;
    if (!body || body->instructions.empty()) return Result<SctAuthoringState>::failure(error("Select a nonempty script section."));
    const auto name = s->nameBytes;
    if (!std::ranges::any_of(next.project.sequences,[&](const auto& q) { return q.script == script && q.section == section; })) {
        auto promoted = SctSequenceAuthoring::promoteSequence(next,script,section,name);
        if (!promoted) return promoted;
        next = std::move(promoted).takeValue();
    }
    const auto seq = std::ranges::find_if(next.project.sequences,[&](const auto& q) { return q.script == script && q.section == section; });
    auto id = next.project.allocate<SctEntrypointId>(); if (!id) return Result<SctAuthoringState>::failure(id.diagnostics());
    SctEntrypointBinding binding; binding.kind = required(name) ? SctEntrypointKind::Lifecycle : SctEntrypointKind::NamedSection; binding.sequence = seq->id;
    next.project.entrypoints.push_back({id.value(),script,name,{},{{SctEvidenceKind::UserAssertion,"Explicit named entrypoint."}},binding,true,SctNamedActivation{name,seq->actions.front().id}});
    return checked(std::move(next));
}
Result<SctAuthoringState> SctEntrypointAuthoring::associateResource(const SctAuthoringState& state, SctScriptId script, const smores::script::Resource& resource) {
    auto next = state; auto found = std::ranges::find(next.project.scripts,script,&SctScriptContext::id);
    if (found == next.project.scripts.end() || !smores::script::valid(resource.identity)) return Result<SctAuthoringState>::failure(error("Select an inspected resource and script."));
    std::erase_if(found->resources,[&](const auto& r) { return r.resource.asset == resource.identity.asset; });
    found->resources.push_back({resource.identity,resource.role});
    return checked(std::move(next));
}
}
