#include "SalsaCore/Program/ProgramMaterializer.h"
#include "DispatchPredicates.h"
#include "ProgramInternal.h"
#include "ProgramTypedValues.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include <set>

namespace salsa::core::program {
namespace sct = spice::sct;
namespace {
enum class LabelKind { Action, After, Sequence, Check, Arm, Dispatch };
struct Label {
    LabelKind kind{};
    std::uint64_t id = 0;
    std::uint64_t ordinal = 0, space = 0;
    auto operator<=>(const Label &) const = default;
};
Label actionLabel(SctActionId id) {
    return {LabelKind::Action, id.value};
}
Label afterLabel(SctActionId id) {
    return {LabelKind::After, id.value};
}
Label sequenceLabel(SctSequenceId id) {
    return {LabelKind::Sequence, id.value};
}
struct Fixup {
    sct::SctParameterAddress parameter;
    Label target;
};
struct Step {
    enum Kind {
        Mark,
        Expand,
        Emit
    } kind = Mark;
    Label label;
    SctSequenceId sequence;
    SctSequenceId normalExitFrom;
    sct::SctDocumentInstruction instruction;
    std::vector<Fixup> fixups;
    std::vector<Subject> subjects;
    bool generated = true;
    bool clone = false;
};
struct Template {
    std::vector<std::uint64_t> signature;
    std::vector<Step> steps;
};

class Values : public TypedValues<Program> {
public:
    explicit Values(const Program &program): TypedValues(program) {}
    sct::SctDocumentParameter parameter(ValueId id, std::optional<std::uint32_t> group, Step &step) const {
        const auto &v = get(id, ValueKind::Parameter);
        auto children = p.children(id);
        require(children.size() == 1, "Parameter needs one value");
        const auto child = children.front();
        const auto *value = p.find(child);
        require(value != nullptr, "Missing parameter value");
        sct::SctDocumentParameter result;
        result.schemaIndex = static_cast<std::uint32_t>(v.data);
        switch (value->kind) {
        case ValueKind::Word:
            result.value = sct::SctEncodedWordValue{static_cast<std::uint32_t>(value->data)};
            break;
        case ValueKind::Words:
            result.value = sct::SctTerminatedWordSequenceValue{words(child)};
            break;
        case ValueKind::Expression:
            result.value = expression(child);
            break;
        case ValueKind::ActionReference:
            result.value = sct::SctInstructionReference{};
            step.fixups.push_back({{result.schemaIndex, group}, actionLabel(SctActionId{value->data})});
            break;
        // Text references use temporary authored IDs, remapped during assembly.
        case ValueKind::IndexedTextReference:
            result.value = sct::SctStringReference{sct::SctStringId{value->data}};
            break;
        case ValueKind::SupplementaryTextReference:
            result.value = sct::SctSupplementaryTextReference{sct::SctSupplementaryTextId{value->data}};
            break;
        default:
            require(false, "Unresolved or unsupported executable operand; repair it before export");
        }
        return result;
    }
    Step instruction(const Action &a) const {
        const auto &v = get(a.value, ValueKind::Instruction);
        Step step;
        step.kind = Step::Emit;
        step.generated = false;
        step.subjects.push_back(a.id);
        auto &i = step.instruction;
        i.opcode = static_cast<std::uint16_t>(v.data);
        i.skipRefresh = (v.detail & 1) != 0;
        auto children = p.children(a.value);
        if (v.detail & 2) {
            require(!children.empty(), "Missing schedule");
            i.scheduledExpression = expression(children.front());
            children = children.subspan(1);
        }
        for (auto child : children) {
            const auto *value = p.find(child);
            require(value != nullptr, "Missing instruction child");
            if (value->kind == ValueKind::Parameter)
                i.fixedParameters.push_back(parameter(child, {}, step));
            else {
                get(child, ValueKind::RepeatedGroup);
                const auto ordinal = static_cast<std::uint32_t>(i.repeatedParameterGroups.size());
                sct::SctDocumentRepeatedParameterGroup group;
                for (auto param : p.children(child))
                    group.parameters.push_back(parameter(param, ordinal, step));
                i.repeatedParameterGroups.push_back(std::move(group));
            }
        }
        return step;
    }
};
// Exact local dependency signatures, not hashes or pool addresses. Maintenance,
// reload and undo may repack pools without changing the authored meaning.
void valueSignature(const Program &p, ValueId id, std::vector<std::uint64_t> &out) {
    const auto *v = p.find(id);
    require(v != nullptr, "Missing lowering dependency");
    out.insert(out.end(), {static_cast<std::uint64_t>(v->kind), v->data, v->detail, v->count});
    for (auto child : p.children(id))
        valueSignature(p, child, out);
}
std::vector<std::uint64_t> signature(const Program &p, const Action &a) {
    std::vector<std::uint64_t> out{static_cast<std::uint64_t>(a.kind)};
    auto condition = [&](ConditionId id) {
        const auto *c = p.find(id);
        require(c != nullptr, "Missing condition");
        out.insert(out.end(), {id.value, c->preparation.value, c->skipRefresh});
        valueSignature(p, c->result, out);
    };
    if (a.kind == ActionKind::Instruction)
        valueSignature(p, a.value, out);
    for (const auto &arm : p.arms(a.id)) {
        out.insert(out.end(), {arm.id.value, arm.body.value, arm.condition.value, arm.includesDefault,
                               static_cast<std::uint64_t>(arm.caseExit), arm.labelCount});
        for (auto label : p.caseLabels(arm.id))
            out.push_back(static_cast<std::uint32_t>(label));
        if (arm.condition.valid())
            condition(arm.condition);
    }
    if (const auto *loop = p.loop(a.id)) {
        out.insert(out.end(), {static_cast<std::uint64_t>(loop->kind), loop->initialize.value, loop->body.value,
                               loop->update.value});
        condition(loop->condition);
    }
    if (const auto *sw = p.switchRegion(a.id))
        condition(sw->selector);
    if (const auto *region = p.dispatch(a.id)) {
        out.push_back(static_cast<std::uint64_t>(region->mode));
        for (auto sample : region->samples) {
            out.push_back(sample.beforeRoute);
            condition(sample.condition);
        }
        for (auto &route : region->routes) {
            out.push_back(route.arm.value);
            out.push_back(route.samples.size());
            for (auto id : route.samples)
                out.push_back(id.value);
        }
    }
    if (const auto *transfer = p.transfer(a.id)) {
        out.insert(out.end(), {transfer->target.value, static_cast<std::uint64_t>(transfer->kind), transfer->skipRefresh});
        if (const auto *loop = p.loop(transfer->target))
            out.insert(out.end(), {static_cast<std::uint64_t>(loop->kind), loop->update.value, loop->condition.value});
    }
    return out;
}
bool fallsThrough(const Program &p, SctSequenceId id) {
    const auto *seq = p.find(id);
    require(seq && seq->order, "Missing normal exit sequence");
    if (seq->order->empty())
        return true;
    const auto *a = p.find(seq->order->back());
    require(a != nullptr, "Missing normal exit action");
    if (a->kind == ActionKind::Transfer)
        return false;
    if (a->kind == ActionKind::Instruction) {
        const auto *value = p.find(a->value);
        const auto *schema = value ? sct::findSctOpcodeSchema(static_cast<std::uint16_t>(value->data)) : nullptr;
        return !schema || (schema->semantic.controlRole != sct::SctOpcodeControlRole::Jump &&
                           schema->semantic.controlRole != sct::SctOpcodeControlRole::Return);
    }
    if (a->kind == ActionKind::Conditional) {
        auto arms = p.arms(a->id);
        return arms.empty() || arms.back().condition.valid() ||
               std::ranges::any_of(arms, [&](const auto &arm) { return fallsThrough(p, arm.body); });
    }
    return true;
}
#include "DispatchLowering.inl"
Template compile(const Program &p, const Action &a, std::vector<std::uint64_t> sig) {
    Template t{std::move(sig)};
    Values values(p);
    auto mark = [&](Label l) {
        Step s;
        s.label = l;
        t.steps.push_back(std::move(s));
    };
    auto expand = [&](SctSequenceId id) {
        if (id.valid()) {
            Step s;
            s.kind = Step::Expand;
            s.sequence = id;
            t.steps.push_back(std::move(s));
        }
    };
    auto jump = [&](Label target, std::optional<Subject> extra = {}, SctSequenceId normalExitFrom = {}) {
        Step s;
        s.kind = Step::Emit;
        s.subjects.push_back(a.id);
        if (extra)
            s.subjects.push_back(*extra);
        s.instruction = {{}, 10, true, {}, {{0, sct::SctInstructionReference{}}}};
        s.normalExitFrom = normalExitFrom;
        s.fixups.push_back({{0, {}}, target});
        t.steps.push_back(std::move(s));
    };
    auto check = [&](ConditionId id, Label no, std::optional<ArmId> arm = {}) {
        const auto *c = p.find(id);
        require(c != nullptr, "Missing condition region");
        mark({LabelKind::Check, id.value});
        expand(c->preparation);
        Step s;
        s.kind = Step::Emit;
        s.subjects = {a.id, id};
        if (arm)
            s.subjects.push_back(*arm);
        s.instruction = {{}, 0, c->skipRefresh, {}, {{0, values.expression(c->result)}, {1, sct::SctInstructionReference{}}}};
        s.fixups.push_back({{1, {}}, no});
        t.steps.push_back(std::move(s));
    };
    switch (a.kind) {
    case ActionKind::Instruction:
        t.steps.push_back(values.instruction(a));
        break;
    case ActionKind::Dispatch:
        compileDispatch(p, a, t);
        break;
    case ActionKind::Conditional: {
        const auto arms = p.arms(a.id);
        for (std::size_t i = 0; i < arms.size(); ++i) {
            const auto &arm = arms[i];
            mark({LabelKind::Arm, arm.id.value});
            if (arm.condition.valid())
                check(arm.condition,
                      i + 1 < arms.size() ? Label{LabelKind::Arm, arms[i + 1].id.value} : afterLabel(a.id), arm.id);
            expand(arm.body);
            jump(afterLabel(a.id), arm.id, arm.body);
        }
        break;
    }
    case ActionKind::Loop: {
        const auto *l = p.loop(a.id);
        require(l != nullptr, "Missing loop");
        expand(l->initialize);
        if (l->kind != LoopKind::DoWhile)
            check(l->condition, afterLabel(a.id));
        expand(l->body);
        expand(l->update);
        if (l->kind == LoopKind::DoWhile) {
            check(l->condition, afterLabel(a.id));
            jump(sequenceLabel(l->body));
        } else
            jump({LabelKind::Check, l->condition.value});
        break;
    }
    case ActionKind::Switch: {
        const auto *sw = p.switchRegion(a.id);
        require(sw != nullptr, "Missing switch");
        const auto *c = p.find(sw->selector);
        require(c != nullptr, "Missing selector");
        expand(c->preparation);
        Step s;
        s.kind = Step::Emit;
        s.subjects = {a.id, c->id};
        s.instruction = {{}, 3, c->skipRefresh, {}, {{0, values.expression(c->result)}}};
        auto row = [&](std::uint32_t label, Label target) {
            const auto ordinal = static_cast<std::uint32_t>(s.instruction.repeatedParameterGroups.size());
            s.instruction.repeatedParameterGroups.push_back(
                {{{2, sct::SctEncodedWordValue{label}}, {3, sct::SctInstructionReference{}}}});
            s.fixups.push_back({{3, ordinal}, target});
        };
        Label defaultTarget = afterLabel(a.id);
        for (const auto &arm : p.arms(a.id)) {
            for (auto label : p.caseLabels(arm.id)) {
                require(label != -1, "SCT reserves case -1 for Default; use the authored Default label");
                row(static_cast<std::uint32_t>(label), sequenceLabel(arm.body));
            }
            if (arm.includesDefault)
                defaultTarget = sequenceLabel(arm.body);
        }
        // Body order is authored order; the native fallback row follows every
        // numeric label even when Default shares an earlier body.
        row(UINT32_MAX, defaultTarget);
        t.steps.push_back(std::move(s));
        for (const auto &arm : p.arms(a.id)) {
            expand(arm.body);
            if (arm.caseExit == CaseExit::ExitSwitch)
                jump(afterLabel(a.id), arm.id, arm.body);
        }
        break;
    }
    case ActionKind::Transfer: {
        const auto *tr = p.transfer(a.id);
        require(tr != nullptr, "Missing scoped transfer");
        if (tr->kind == TransferKind::Break)
            jump(afterLabel(tr->target));
        else {
            const auto *l = p.loop(tr->target);
            require(l != nullptr, "Continue needs an enclosing loop");
            jump(l->kind == LoopKind::For ? sequenceLabel(l->update) : Label{LabelKind::Check, l->condition.value});
        }
        t.steps.back().instruction.skipRefresh = tr->skipRefresh;
        break;
    }
    }
    return t;
}
} // namespace
struct ProgramMaterializer::Cache {
    SctAuthoringProjectId project;
    SctScriptId script;
    std::map<std::pair<SctScriptId, SctActionId>, Template> templates;
};
ProgramMaterializer::ProgramMaterializer() : cache_(std::make_unique<Cache>()) {
}
ProgramMaterializer::~ProgramMaterializer() = default;

Result<MaterializedScript> ProgramMaterializer::materialize(const Checkpoint &captured, const Revision &expected,
                                                            SctScriptId selected, SctRealizationId realization,
                                                            std::stop_token stop) {
    try {
        require(captured.program && captured.status.current == expected,
                "Stale export capture or operation generation");
        const auto &p = *captured.program;
        require(p.id() == expected.project && p.revision() == expected.revision,
                "Export capture does not identify its program revision");
        require(realization.valid(), "A realization identity is required");
        const auto *script = p.find(selected);
        require(script != nullptr, "Selected script is missing");
        auto cancelled = [&] { require(!stop.stop_requested(), "Script materialization cancelled"); };
        cancelled();
        if (cache_->project != p.id() || cache_->script != selected) {
            cache_->templates.clear();
            cache_->project = p.id();
            cache_->script = selected;
        }
        std::vector<Diagnostic> diagnostics;
        auto omitted = [&](std::string message) {
            diagnostics.push_back(
                {DiagnosticSeverity::Warning, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)});
        };
        auto supportedText = [&](SctTextResourceId id) {
            const auto *text = p.find(id);
            require(text && text->script == selected, "Missing or foreign text");
            const auto *v = p.find(text->value);
            require(v != nullptr, "Missing text payload");
            return v->kind != ValueKind::Unavailable;
        };
        Values values(p);
        sct::SctDocument document;
        MaterializedScript result;
        result.realization = {expected, selected, std::move(realization), {}};
        auto record = [&](Subject subject, sct::SctDiagnosticLocation location, bool generated = false) {
            result.realization.locations.push_back({std::move(subject), std::move(location), generated});
        };
        std::map<SctTextResourceId, sct::SctStringId> indexed;
        std::map<SctTextResourceId, sct::SctSupplementaryTextId> supplementary;
        for (auto id : script->supplementaryText)
            if (supportedText(id))
                supplementary.emplace(id, document.allocateSupplementaryTextId());
            else
                omitted("Excluded unavailable supplementary text " + std::to_string(id.value));
        for (auto id : script->sections) {
            const auto *section = p.find(id);
            require(section && section->script == selected, "Missing or foreign script section");
            if (const auto *v = p.find(section->content); v && v->kind == ValueKind::IndexedSection)
                if (supportedText(SctTextResourceId{v->data}))
                    indexed.emplace(SctTextResourceId{v->data}, document.allocateStringId());
        }
        struct Position {
            std::size_t section, instruction;
        };
        struct Pending {
            Position from;
            Fixup fixup;
        };
        std::map<Label, Position> labels;
        std::vector<Pending> pending;
        std::set<SctActionId> visited;
        std::set<SctSequenceId> visitedSequences;
        std::size_t currentSection = 0;
        auto position = [&] {
            return Position{
                currentSection,
                std::get<sct::SctScriptSectionContent>(document.sections[currentSection].content).instructions.size()};
        };
        std::uint64_t nextSpace = 0;
        std::vector<std::pair<SctSequenceId, std::uint64_t>> clones;
        std::set<std::pair<SctSequenceId, std::uint64_t>> sequenceOccurrences;
        auto contained = [&](SctSequenceId id, SctSequenceId root) {
            std::set<SctSequenceId> seen;
            while (id.valid() && seen.insert(id).second) {
                if (id == root)
                    return true;
                const auto *seq = p.find(id);
                if (!seq || !seq->owner)
                    return false;
                auto parent = std::visit(
                    [&](auto owner) -> SctActionId {
                        using T = decltype(owner);
                        if constexpr (std::is_same_v<T, SctActionId>)
                            return owner;
                        else {
                            const auto *row = p.find(owner);
                            return row ? row->owner : SctActionId{};
                        }
                    },
                    *seq->owner);
                const auto *a = p.find(parent);
                id = a ? a->owner : SctSequenceId{};
            }
            return false;
        };
        auto labelSequence = [&](Label l) -> SctSequenceId {
            if (l.kind == LabelKind::Sequence)
                return SctSequenceId{l.id};
            SctActionId action{l.id};
            if (l.kind == LabelKind::Check) {
                auto *c = p.find(ConditionId{l.id});
                if (!c)
                    return {};
                action = c->owner;
            }
            if (l.kind == LabelKind::Arm) {
                auto *arm = p.find(ArmId{l.id});
                if (!arm)
                    return {};
                action = arm->owner;
            }
            auto *a = p.find(action);
            return a ? a->owner : SctSequenceId{};
        };
        auto qualify = [&](Label l) {
            for (auto at = clones.rbegin(); at != clones.rend(); ++at)
                if (contained(labelSequence(l), at->first)) {
                    l.space = at->second;
                    break;
                }
            return l;
        };
        auto mark = [&](Label l) {
            auto actual = qualify(l);
            require(labels.emplace(actual, position()).second, "Duplicate lowering label");
            if (actual.space)
                labels.try_emplace(l, position()); // first occurrence for presentation only
        };
        auto remapText = [&](sct::SctDocumentParameter &parameter) {
            if (auto *ref = std::get_if<sct::SctStringReference>(&parameter.value)) {
                const auto found = indexed.find(SctTextResourceId{ref->target.value()});
                require(found != indexed.end(), "Dangling or cross-script indexed text reference");
                ref->target = found->second;
            } else if (auto *ref = std::get_if<sct::SctSupplementaryTextReference>(&parameter.value)) {
                const auto found = supplementary.find(SctTextResourceId{ref->target.value()});
                require(found != supplementary.end(), "Dangling or cross-script supplementary text reference");
                ref->target = found->second;
            }
        };
        std::function<void(SctSequenceId)> sequence;
        sequence = [&](SctSequenceId id) {
            cancelled();
            const auto *seq = p.find(id);
            require(seq && seq->script == selected && seq->order, "Missing or foreign sequence");
            require(sequenceOccurrences.emplace(id, qualify(sequenceLabel(id)).space).second,
                    "Repeated or cyclic sequence ownership");
            visitedSequences.insert(id);
            mark(sequenceLabel(id));
            for (auto actionId : *seq->order) {
                cancelled();
                const auto *a = p.find(actionId);
                require(a && a->owner == id, "Dangling action or invalid sequence owner");
                visited.insert(actionId);
                if (!clones.empty())
                    for (auto use : p.incoming(actionId)) {
                        auto *source = p.find(use.site.action);
                        require(source && contained(source->owner, clones.back().first),
                                "Dispatch route body has an incoming cross-boundary execution reference; keep it "
                                "explicit");
                    }
                mark(actionLabel(actionId));
                auto sig = signature(p, *a);
                const auto key = std::pair(selected, actionId);
                auto found = cache_->templates.find(key);
                if (found == cache_->templates.end() || found->second.signature != sig) {
                    Template t;
                    try {
                        t = compile(p, *a, std::move(sig));
                    } catch (const std::exception &e) {
                        throw std::runtime_error("Action " + std::to_string(actionId.value) + ": " + e.what());
                    }
                    found = cache_->templates.insert_or_assign(key, std::move(t)).first;
                    ++result.loweredActions;
                } else
                    ++result.reusedActions;
                for (const auto &step : found->second.steps) {
                    if (step.kind == Step::Mark)
                        mark(step.label);
                    else if (step.kind == Step::Expand) {
                        if (step.clone)
                            clones.emplace_back(step.sequence, ++nextSpace);
                        sequence(step.sequence);
                        if (step.clone)
                            clones.pop_back();
                    } else {
                        if (step.normalExitFrom.valid() && !fallsThrough(p, step.normalExitFrom))
                            continue;
                        const auto from = position();
                        auto instruction = step.instruction;
                        instruction.id = document.allocateInstructionId();
                        for (auto &parameter : instruction.fixedParameters)
                            remapText(parameter);
                        for (auto &group : instruction.repeatedParameterGroups)
                            for (auto &parameter : group.parameters)
                                remapText(parameter);
                        for (auto fixup : step.fixups) {
                            fixup.target = qualify(fixup.target);
                            pending.push_back({from, fixup});
                        }
                        for (const auto &subject : step.subjects)
                            record(subject, sct::SctDocumentEntityId{instruction.id}, step.generated);
                        std::get<sct::SctScriptSectionContent>(document.sections[currentSection].content)
                            .instructions.push_back(std::move(instruction));
                    }
                }
                mark(afterLabel(actionId));
            }
        };
        for (auto sectionId : script->sections) {
            cancelled();
            const auto &section = *p.find(sectionId);
            if (!section.body.valid()) {
                const auto *content = p.find(section.content);
                require(content != nullptr, "Missing section content");
                if (content->kind == ValueKind::Unavailable || (content->kind == ValueKind::IndexedSection &&
                                                                !indexed.contains(SctTextResourceId{content->data}))) {
                    omitted("Excluded unavailable source section " + section.name);
                    continue;
                }
            }
            const auto nativeId = document.allocateSectionId();
            record(section.id, sct::SctDocumentEntityId{nativeId});
            document.sections.push_back({nativeId, section.name, sct::SctScriptSectionContent{}});
            currentSection = document.sections.size() - 1;
            if (section.body.valid())
                sequence(section.body);
            else {
                const auto *v = p.find(section.content);
                require(v != nullptr, "Missing section content");
                if (v->kind == ValueKind::IndexedSection) {
                    const auto *text = p.find(SctTextResourceId{v->data});
                    require(text && text->script == selected && !text->supplementary, "Missing indexed text");
                    auto children = p.children(section.content);
                    require(children.size() == 1, "Missing indexed text preamble");
                    const auto id = indexed.at(text->id);
                    document.sections.back().content = sct::SctStringSectionContent{
                        {id, values.text(text->value), text->kind}, values.words(children.front())};
                    record(text->id, sct::SctDocumentEntityId{id});
                } else if (v->kind == ValueKind::StringGroup)
                    document.sections.back().content =
                        sct::SctStringGroupMarkerSectionContent{values.words(section.content)};
                else
                    require(false, "Unsupported section content; repair or remove the section before export");
            }
        }
        for (auto textId : script->supplementaryText) {
            if (!supplementary.contains(textId))
                continue;
            cancelled();
            const auto *text = p.find(textId);
            require(text && text->script == selected && text->supplementary, "Missing supplementary text");
            const auto id = supplementary.at(textId);
            document.supplementaryText.push_back({id, text->kind, values.text(text->value)});
            record(textId, sct::SctDocumentEntityId{id});
        }
        // A label at a sequence end aliases the next physical instruction, even
        // in the following code section. Never route through a data section or
        // invent a Return body for a missing continuation.
        auto resolve = [&](Label label) -> std::optional<sct::SctInstructionId> {
            const auto found = labels.find(label);
            if (found == labels.end())
                return {};
            auto pos = found->second;
            while (pos.section < document.sections.size()) {
                const auto *code = std::get_if<sct::SctScriptSectionContent>(&document.sections[pos.section].content);
                if (!code)
                    return {};
                if (pos.instruction < code->instructions.size())
                    return code->instructions[pos.instruction].id;
                ++pos.section;
                pos.instruction = 0;
            }
            return {};
        };
        for (const auto &item : pending) {
            auto target = resolve(item.fixup.target);
            if (!target)
                throw std::runtime_error("Missing execution target/continuation for authored identity " +
                                         std::to_string(item.fixup.target.id));
            auto &instruction = std::get<sct::SctScriptSectionContent>(document.sections[item.from.section].content)
                                    .instructions[item.from.instruction];
            auto &parameters =
                item.fixup.parameter.repeatedGroupOrdinal
                    ? instruction.repeatedParameterGroups.at(*item.fixup.parameter.repeatedGroupOrdinal).parameters
                    : instruction.fixedParameters;
            auto found = std::ranges::find(parameters, item.fixup.parameter.schemaIndex,
                                           &sct::SctDocumentParameter::schemaIndex);
            require(found != parameters.end(), "Missing generated target operand");
            found->value = sct::SctInstructionReference{*target};
        }
        for (auto id : visitedSequences)
            if (auto target = resolve(sequenceLabel(id)))
                record(id, sct::SctDocumentEntityId{*target});
        for (auto id : visited) {
            if (auto target = resolve(actionLabel(id)))
                record(id, sct::SctDocumentEntityId{*target}, p.find(id)->kind != ActionKind::Instruction);
            for (const auto &arm : p.arms(id))
                if (auto target = resolve(sequenceLabel(arm.body)))
                    record(arm.id, sct::SctDocumentEntityId{*target}, true);
        }
        for (const auto &module : p.metadata().modules)
            if (module.script == selected)
                for (auto seq : module.sequences)
                    if (auto target = resolve(sequenceLabel(seq)))
                        record(module.id, sct::SctDocumentEntityId{*target});
        for(const auto& port:p.metadata().ports) {
            if(port.action && visited.contains(*port.action)) {
                if(auto target=resolve(actionLabel(*port.action)))record(port.id,sct::SctDocumentEntityId{*target});
            } else if(port.sequence && p.find(*port.sequence) && p.find(*port.sequence)->script==selected) {
                if(auto target=resolve(sequenceLabel(*port.sequence)))record(port.id,sct::SctDocumentEntityId{*target});
            }
        }
        for(const auto& connection:p.metadata().connections)if(connection.cutscene && connection.cutscene->implementation && visited.contains(*connection.cutscene->implementation))
            if(auto target=resolve(actionLabel(*connection.cutscene->implementation)))record(connection.id,sct::SctDocumentEntityId{*target});
        for (const auto name : {"init", "loop"}) {
            const auto count = std::ranges::count_if(document.sections, [&](const auto &section) {
                const auto *code = std::get_if<sct::SctScriptSectionContent>(&section.content);
                return section.nameBytes == name && code && !code->instructions.empty();
            });
            require(count == 1,
                    "Export requires one nonempty init and loop section; repair the imported draft explicitly");
        }
        std::vector<SctActivationEntry> entries;
        // Resolve before moving the document into shared activation lowering.
        std::map<SctActionId, sct::SctInstructionId> actions;
        for (auto id : visited)
            if (auto target = resolve(actionLabel(id)))
                actions.emplace(id, *target);
        for (const auto &entry : p.metadata().entrypoints)
            if (entry.script == selected) {
                require(!entry.required() || entry.enabled, "init and loop must remain enabled");
                // Engine roots enter the current body, independently of any
                // imported first-action observation retained in their metadata.
                if (!entry.required())
                    entries.push_back({entry.id, entry.name, entry.enabled, entry.activation});
                if (auto target = resolve(sequenceLabel(entry.body)))
                    record(entry.id, sct::SctDocumentEntityId{*target});
            }
        cancelled();
        auto activated = SctEntrypointAuthoring::lower(
            entries,
            [&](SctActionId id) -> std::optional<sct::SctInstructionId> {
                auto found = actions.find(id);
                return found == actions.end() ? std::nullopt : std::optional(found->second);
            },
            std::move(document));
        if (!activated)
            return Result<MaterializedScript>::failure(activated.diagnostics());
        for (const auto &generated : activated.value().generated) {
            const auto *entry = std::get_if<SctEntrypointId>(&generated.entity);
            require(entry != nullptr, "Unexpected activation owner");
            for (const auto &location : generated.locations)
                record(*entry, location, true);
        }
        cancelled();
        result.document = std::make_shared<const sct::SctDocument>(std::move(activated.value().document));
        std::erase_if(cache_->templates, [&](const auto &item) {
            return item.first.first == selected && !visited.contains(item.first.second);
        });
        return Result<MaterializedScript>::success(std::move(result), std::move(diagnostics));
    } catch (const std::exception &e) {
        return Result<MaterializedScript>::failure(failure(std::string("Script materialization: ") + e.what()));
    }
}
Result<ExportedScript> ProgramMaterializer::exportScript(const Checkpoint &captured, const Revision &expected,
                                                         SctScriptId script, SctRealizationId realization,
                                                         const sct::SctDocumentExportOptions &options,
                                                         std::stop_token stop) {
    try {
        auto materialized = materialize(captured, expected, script, std::move(realization), stop);
        if (!materialized)
            return Result<ExportedScript>::failure(materialized.diagnostics());
        auto exported = sct::SctDocumentExporter::exportDocument(*materialized.value().document, options);
        std::vector<Diagnostic> diagnostics = materialized.diagnostics();
        for (const auto &d : exported.diagnostics)
            diagnostics.push_back({d.severity == sct::SctDiagnosticSeverity::Error ? DiagnosticSeverity::Error
                                                                                   : DiagnosticSeverity::Warning,
                                   DiagnosticCode::InvalidSctAuthoringProject, d.message});
        if (!exported.success || !exported.layout || stop.stop_requested()) {
            if (stop.stop_requested())
                diagnostics.push_back(failure("Script export cancelled; completed parser/encoder result discarded"));
            return Result<ExportedScript>::failure(std::move(diagnostics));
        }
        auto digest = sha256(std::as_bytes(std::span(exported.bytes)));
        if (!digest)
            return Result<ExportedScript>::failure(digest.diagnostics());
        return Result<ExportedScript>::success({std::move(materialized).takeValue(), std::move(exported.bytes),
                                                std::move(*exported.layout), digest.value()},
                                               std::move(diagnostics));
    } catch (const std::exception &e) {
        return Result<ExportedScript>::failure(failure(std::string("Script export: ") + e.what()));
    }
}
} // namespace salsa::core::program
