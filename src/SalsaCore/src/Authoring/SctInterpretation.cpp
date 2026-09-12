#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctScptEncoding.h"
#include <charconv>
#include <bit>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string text) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(text)};
}
const sct::SctDocument *document(const SctAuthoringState &state, SctScriptId script) {
    for (const auto &c : state.project.contents)
        if (state.project.effectiveScript(c.owner) == script && c.authoredState && c.authoredState->document)
            return c.authoredState->document.get();
    return nullptr;
}
const sct::SctDocumentInstruction *instruction(const sct::SctDocument &d, sct::SctInstructionId id) {
    for (const auto &s : d.sections)
        if (const auto *b = std::get_if<sct::SctScriptSectionContent>(&s.content))
            for (const auto &i : b->instructions)
                if (i.id == id)
                    return &i;
    return nullptr;
}
std::string variableName(SctVariableKey key) {
    constexpr const char *names[] = {"BitVar", "ByteVar", "IntVar", "FloatVar"};
    return std::string(names[static_cast<unsigned>(key.kind)]) + "[" + std::to_string(key.index) + "]";
}
std::optional<std::int64_t> number(const sct::SctDocumentParameter &p) {
    if (const auto *w = std::get_if<sct::SctEncodedWordValue>(&p.value))
        return w->value;
    if (const auto *e = std::get_if<sct::SctCanonicalExpression>(&p.value)) {
        if (auto literal = SctLiteralConstant::fromExpression(*e)) {
            if (literal->operation.kind == sct::SctScptValueKind::FloatLiteral && literal->operation.payloadWords.size() == 1) {
                const auto value = std::bit_cast<float>(literal->operation.payloadWords.front());
                if (std::isfinite(value) && std::floor(value) == value && value >= 0 && value <= UINT32_MAX)
                    return static_cast<std::int64_t>(value);
            }
            if (literal->operation.kind == sct::SctScptValueKind::InlineValue) return literal->operation.encodingWord;
        }

        auto text = SctExpressionLanguage::project(*e).text;
        std::int64_t n = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), n);
        if (r.ec == std::errc{} && r.ptr == text.data() + text.size())
            return n;
    }
    return {};
}
std::optional<SctVariableKey> written(const sct::SctDocumentInstruction &i) {
    if (i.fixedParameters.empty())
        return {};
    if (i.opcode >= 17 && i.opcode <= 19) {
        auto n = number(i.fixedParameters.front());
        if (n && *n >= 0 && *n <= 65535)
            return SctVariableKey{SctVariableKind::Bit, static_cast<std::uint32_t>(*n)};
    }
    if (i.opcode >= 5 && i.opcode <= 7) {
        auto n = number(i.fixedParameters.front());
        if (n)
            return SctVariableKey{i.opcode == 5   ? SctVariableKind::Byte
                                  : i.opcode == 6 ? SctVariableKind::Integer
                                                  : SctVariableKind::Float,
                                  static_cast<std::uint32_t>(*n) & 65535};
    }
    return {};
}
std::string signature(const sct::SctDocumentInstruction &i) {
    std::ostringstream o;
    o << i.id.value() << ':' << i.opcode << ':' << i.skipRefresh;
    const auto words = [&](const auto &ws) {
        for (auto w : ws)
            o << ',' << w;
    };
    if (i.scheduledExpression) {
        o << " schedule";
        words(sct::encodeSctCanonicalExpressionWords(*i.scheduledExpression));
    }
    const auto parameters = [&](const auto &ps) {
        for (const auto &p : ps) {
            o << " p" << p.schemaIndex << ':' << p.value.index() << ':';
            std::visit(
                [&](const auto &v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, sct::SctCanonicalExpression>)
                        words(sct::encodeSctCanonicalExpressionWords(v));
                    else if constexpr (requires { v.target; })
                        o << v.target.value();
                    else if constexpr (requires { v.words; })
                        words(v.words);
                    else if constexpr (requires { v.encodedWords; }) {
                        words(v.encodedWords);
                        o << ':' << static_cast<int>(v.expectedTarget.storage);
                    } else
                        o << v.value;
                },
                p.value);
        }
    };
    parameters(i.fixedParameters);
    for (const auto &g : i.repeatedParameterGroups) {
        o << " group";
        parameters(g.parameters);
    }
    return o.str();
}
const SctExplanationRecord *explanation(const SctAuthoringProject &p, SctExplanationId id) {
    auto i = std::ranges::find(p.explanations, id, &SctExplanationRecord::id);
    return i == p.explanations.end() ? nullptr : &*i;
}
Result<SctAuthoringState> checked(SctAuthoringState state) {
    auto ds = state.project.validate();
    if (hasErrors(ds))
        return Result<SctAuthoringState>::failure(std::move(ds));
    return Result<SctAuthoringState>::success(std::move(state));
}
} // namespace
SctMeaningTarget SctInterpretation::sectionTarget(const SctAuthoringProject &p, SctScriptId script,
                                                  sct::SctSectionId section) {
    SctMeaningTarget t;
    t.script = script;
    t.section = section;
    if (const auto *s = p.find(script))
        if (const auto *b = p.find(s->baseline))
            t.document = b->importedDocument;
    return t;
}
SctMeaningTarget SctInterpretation::siteTarget(const SctAuthoringProject &p, SctScriptId script,
                                               sct::SctParameterSite site) {
    auto t = sectionTarget(p, script, {});
    t.section.reset();
    t.site = site;
    return t;
}
std::vector<Diagnostic> SctInterpretation::validate(const SctAuthoringProject &p) {
    std::vector<Diagnostic> ds;
    const auto target = [&](const SctMeaningTarget &t) {
        const auto count = t.entity.has_value() + t.variable.has_value() + t.section.has_value() + t.site.has_value() +
                           t.resource.has_value();
        if (count != 1 || (t.value && !t.variable) || (!t.script.valid() && !t.variable) ||
            (t.script.valid() && !p.find(t.script)))
            ds.push_back(error("Invalid interpretation target or scope."));
        if (t.variable && (static_cast<unsigned>(t.variable->kind) > 3 || t.variable->index > 65535 ||
                           (t.value && t.variable->kind == SctVariableKind::Bit && (*t.value < 0 || *t.value > 1)) ||
                           (t.value && t.variable->kind == SctVariableKind::Byte && (*t.value < 0 || *t.value > 255))))
            ds.push_back(error("Invalid variable/value definition."));
        if ((t.section || t.site) &&
            (!t.document.valid() || (t.section && !t.section->value()) || (t.site && !t.site->instruction.value())))
            ds.push_back(error("Physical interpretation point requires qualified identity."));
        if (!t.section && !t.site && !t.document.value.empty())
            ds.push_back(error("Unexpected document qualifier."));
        if (t.resource && !smores::script::valid(t.resource->resource))
            ds.push_back(error("Invalid resource evidence identity."));
        // Deleted items remain historical targets. They never bind to a new item by name.
        if (t.entity && !std::visit([](auto id) { return id.valid(); }, *t.entity))
            ds.push_back(error("Invalid authored interpretation identity."));
    };
    std::set<SctMeaningTarget> named;
    for (const auto &m : p.meanings) {
        target(m.target);
        if (m.name.empty() || !named.insert(m.target).second)
            ds.push_back(error("Empty or duplicate meaning definition."));
        if (m.explanation && !explanation(p, *m.explanation))
            ds.push_back(error("Meaning explanation is missing."));
    }
    for (const auto &e : p.explanations) {
        if (e.key.empty() || e.rule.empty() || e.ruleVersion.empty() || e.interpretation.empty() ||
            e.reasoning.empty() || !e.revision.valid() || e.subjects.empty() ||
            e.evidence.size() != e.observations.size() || static_cast<unsigned>(e.origin) > 2 ||
            static_cast<unsigned>(e.decision) > 3)
            ds.push_back(error("Invalid explanation record."));
        for (const auto &t : e.subjects)
            target(t);
        for (const auto &t : e.evidence)
            target(t);
        if (e.supersedes && (!explanation(p, *e.supersedes) || e.supersedes->value >= e.id.value))
            ds.push_back(error("Explanation history must reference an earlier record."));
    }
    return ds;
}
std::string SctInterpretation::describe(const SctAuthoringState &state, const SctMeaningTarget &t) {
    const auto *s = state.project.find(t.script);
    std::string prefix = s ? s->name + ": " : "Project: ";
    if (t.variable)
        return prefix + variableName(*t.variable) + (t.value ? " == " + std::to_string(*t.value) : "");
    if (t.resource)
        return prefix + t.resource->resource.asset + " / entry " + std::to_string(t.resource->tableIndex);
    if (t.entity)
        return prefix + std::visit(
                            [&](auto id) -> std::string {
                                auto *v = state.project.find(id);
                                if (!v)
                                    return "Removed item " + std::to_string(id.value);
                                if constexpr (requires { v->name; })
                                    return v->name;
                                else
                                    return "Item " + std::to_string(id.value);
                            },
                            *t.entity);
    if (t.site)
        return prefix + "instruction " + std::to_string(t.site->instruction.value()) + " / parameter " +
               std::to_string(t.site->parameter.schemaIndex);
    if (t.section)
        if (const auto *d = document(state, t.script))
            for (const auto &sec : d->sections)
                if (sec.id == *t.section)
                    return prefix + sec.nameBytes;
    return prefix + "Removed content";
}
std::string SctInterpretation::observe(const SctAuthoringState &state, const SctMeaningTarget &t) {
    if (t.variable) {
        std::string result = "Writers of " + variableName(*t.variable);
        for (const auto &s : state.project.scripts)
            if (!t.script.valid() || s.id == t.script)
                if (const auto *d = document(state, s.id))
                    for (const auto &sec : d->sections)
                        if (const auto *b = std::get_if<sct::SctScriptSectionContent>(&sec.content))
                            for (const auto &i : b->instructions)
                                if (written(i) == t.variable || (i.opcode >= 17 && i.opcode <= 19 && !written(i)))
                                    result += '\n' + std::to_string(s.id.value) + ":" + signature(i);
        return result;
    }
    if (t.entity)
        return std::visit(
            [&](auto id) -> std::string { return state.project.find(id) ? describe(state, t) : std::string{}; },
            *t.entity);
    if (t.resource)
        return t.resource->resource.revision + ":" + t.resource->resource.document + ":" +
               std::to_string(t.resource->tableIndex);
    const auto *script = state.project.find(t.script);
    if (!script || state.project.find(script->baseline)->importedDocument != t.document)
        return {};
    const auto *d = document(state, t.script);
    if (!d)
        return {};
    if (t.site) {
        auto *i = instruction(*d, t.site->instruction);
        return i ? signature(*i) : std::string{};
    }
    if (t.section)
        for (const auto &sec : d->sections)
            if (sec.id == *t.section) {
                std::string r = sec.nameBytes;
                if (const auto *b = std::get_if<sct::SctScriptSectionContent>(&sec.content))
                    for (const auto &i : b->instructions)
                        r += '\n' + signature(i);
                return r;
            }
    return {};
}
bool SctInterpretation::fresh(const SctAuthoringState &state, const SctExplanationRecord &e) {
    if (e.evidence.size() != e.observations.size())
        return false;
    for (std::size_t i = 0; i < e.evidence.size(); ++i)
        if (observe(state, e.evidence[i]) != e.observations[i] || e.observations[i].empty())
            return false;
    return true;
}
std::string SctInterpretation::label(const SctAuthoringState &state, const SctMeaningTarget &target,
                                     std::string fallback) {
    for (const auto &m : state.project.meanings)
        if (m.target == target) {
            if (m.explanation) {
                const auto *e = explanation(state.project, *m.explanation);
                if (!e || e->decision != SctInterpretationDecision::Accepted ||
                    ((e->origin != SctEvidenceKind::UserAssertion || target.site.has_value()) && !fresh(state, *e)))
                    continue;
            }
            return m.name;
        }
    return fallback;
}
std::string SctInterpretation::condition(const SctAuthoringState &state, SctScriptId script,
                                         const sct::SctCanonicalExpression &expression, bool friendly) {
    auto text = SctExpressionLanguage::project(expression).text;
    if (!friendly)
        return text;
    // Match complete tokens in SPICE's conventional-expression projection. No
    // expression rewriting or algebraic simplification enters authored content.
    const std::regex comparison(R"(\b(BitVar|ByteVar|IntVar)\[(\d+)\] (==|!=) (-?\d+)(?:\.0+f)?(?![\w.]))");
    std::string result;
    std::vector<std::pair<std::size_t,std::size_t>> protectedNames;
    std::size_t position = 0;
    for (std::sregex_iterator i(text.begin(), text.end(), comparison), end; i != end; ++i) {
        const auto &m = *i;
        result += text.substr(position, m.position() - position);
        auto part = m.str();
        SctMeaningTarget t;
        t.script = script;
        t.variable = SctVariableKey{m[1] == "BitVar"    ? SctVariableKind::Bit
                                    : m[1] == "ByteVar" ? SctVariableKind::Byte
                                                        : SctVariableKind::Integer,
                                    static_cast<std::uint32_t>(std::stoul(m[2]))};
        t.value = std::stoll(m[4]);
        auto name = label(state, t, "");
        if (name.empty()) {
            t.script = {};
            name = label(state, t, "");
        }
        bool negate = m[3] == "!=";
        if (name.empty() && t.variable->kind == SctVariableKind::Bit && (*t.value == 0 || *t.value == 1)) {
            t.value = 1 - *t.value;
            t.script = script;
            name = label(state, t, "");
            if (name.empty()) {
                t.script = {};
                name = label(state, t, "");
            }
            negate = !negate;
        }
        if (!name.empty()) {
            part = negate ? "NOT {" + name + "}" : "{" + name + "}";
            protectedNames.emplace_back(result.size(),result.size()+part.size());
        }
        result += part;
        position = m.position() + m.length();
    }
    result += text.substr(position);
    // Rename remaining variable tokens, including mixed/partially understood conditions.
    const std::regex variable(R"(\b(BitVar|ByteVar|IntVar|FloatVar)\[(\d+)\])");
    text = result;
    result.clear();
    position = 0;
    for (std::sregex_iterator i(text.begin(), text.end(), variable), end; i != end; ++i) {
        const auto &m = *i;
        result += text.substr(position, m.position() - position);
        if(std::ranges::any_of(protectedNames,[&](const auto& span){return static_cast<std::size_t>(m.position())>=span.first && static_cast<std::size_t>(m.position())<span.second;})) {
            result+=m.str();position=m.position()+m.length();continue;
        }
        SctMeaningTarget t;
        t.script = script;
        t.variable = SctVariableKey{m[1] == "BitVar"    ? SctVariableKind::Bit
                                    : m[1] == "ByteVar" ? SctVariableKind::Byte
                                    : m[1] == "IntVar"  ? SctVariableKind::Integer
                                                        : SctVariableKind::Float,
                                    static_cast<std::uint32_t>(std::stoul(m[2]))};
        auto name = label(state, t, "");
        if (name.empty()) {
            t.script = {};
            name = label(state, t, "");
        }
        if (name.empty()) {
            auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
            if (working)
                for (const auto &a : working.value().aliases)
                    if (a.variable == *t.variable)
                        name = a.alias;
        }
        if (name.empty())
            for (const auto &a : state.project.workspaceAuthoring.projectAliases)
                if (a.variable == *t.variable)
                    name = a.alias;
        result += name.empty() ? m.str() : "{" + name + "}";
        position = m.position() + m.length();
    }
    result += text.substr(position);
    return result;
}
SctInterpretationAnalysis SctInterpretation::analyze(const SctAuthoringState &state) {
    SctInterpretationAnalysis a{state.project.id, state.project.revision};
    std::vector<std::pair<SctMeaningTarget, SctStateAccess>> entries;
    for (const auto &script : state.project.scripts) {
        const auto *d = document(state, script.id);
        if (!d) {
            a.diagnostics.push_back(error("Current authored program unavailable for " + script.name));
            continue;
        }
        for (const auto &sec : d->sections)
            if (const auto *b = std::get_if<sct::SctScriptSectionContent>(&sec.content)) {
                for (std::size_t ordinal = 0; ordinal < b->instructions.size(); ++ordinal) {
                    const auto &i = b->instructions[ordinal];
                    const auto scan = [&](const auto &ps, std::optional<std::uint32_t> group) {
                        for (const auto &p : ps)
                            if (const auto *e = std::get_if<sct::SctCanonicalExpression>(&p.value)) {
                                for (const auto &read : SctSequenceAuthoring::stateReads(*e))
                                    a.accesses.push_back(
                                        {siteTarget(state.project, script.id, {i.id, {p.schemaIndex, group}}),
                                         read.storage, SctStateAccessKind::Read,
                                         SctExpressionLanguage::project(*e).text,
                                         (i.opcode == 0 || i.opcode == 3) && p.schemaIndex == 0 && !group});
                            }
                    };
                    scan(i.fixedParameters, {});
                    for (std::uint32_t g = 0; g < i.repeatedParameterGroups.size(); ++g)
                        scan(i.repeatedParameterGroups[g].parameters, g);
                    if (i.scheduledExpression)
                        for (const auto &read : SctSequenceAuthoring::stateReads(*i.scheduledExpression))
                            a.accesses.push_back({siteTarget(state.project, script.id, {i.id, {UINT32_MAX, {}}}),
                                                  read.storage, SctStateAccessKind::Read,
                                                  SctExpressionLanguage::project(*i.scheduledExpression).text, false});
                    if (auto v = written(i)) {
                        auto kind = i.opcode == 17   ? SctStateAccessKind::Set
                                    : i.opcode == 18 ? SctStateAccessKind::Clear
                                    : i.opcode == 19 ? SctStateAccessKind::Toggle
                                                     : SctStateAccessKind::Assign;
                        SctStateAccess access{siteTarget(state.project, script.id, {i.id, {0, {}}}), *v, kind,
                                              signature(i), false};
                        a.accesses.push_back(access);
                        std::size_t entryOrdinal = 0;
                    if (!b->instructions.empty() && b->instructions[0].opcode == 9) ++entryOrdinal;
                    if (entryOrdinal < b->instructions.size() && b->instructions[entryOrdinal].opcode == 0) ++entryOrdinal;
                    const bool entry = ordinal == entryOrdinal;
                        const bool candidate =
                            std::regex_match(sec.nameBytes, std::regex("me[0-9]{3}[a-z][a-z][0-9]+"));
                        const bool owned = std::ranges::any_of(state.project.modules, [&](const auto &m) {
                            return m.script == script.id && !m.sections.empty() && m.sections.front() == sec.id;
                        });
                        if (entry && !i.scheduledExpression && kind == SctStateAccessKind::Set && (candidate || owned))
                            entries.push_back({sectionTarget(state.project, script.id, sec.id), access});
                    } else if (i.opcode >= 17 && i.opcode <= 19)
                        a.diagnostics.push_back({DiagnosticSeverity::Warning,
                                                 DiagnosticCode::UnresolvedSctAuthoringBinding,
                                                 "Dynamic bit write at " + script.name + " / " +
                                                     std::to_string(i.id.value()) + "; writer identity is unknown."});
                }
            }
    }
    std::map<SctVariableKey, std::vector<const SctStateAccess *>> writers, readers;
    for (const auto &x : a.accesses)
        (x.kind == SctStateAccessKind::Read ? readers : writers)[x.variable].push_back(&x);
    std::set<SctVariableKey> storyVariables;
    for (const auto &entry : entries)
        storyVariables.insert(entry.second.variable);
    for (const auto &v : storyVariables)
        for (const auto *w : writers[v])
            for (const auto *r : readers[v])
                if (r->condition)
                    a.dependencies.push_back({w->target, r->target, v});
    for (const auto &[scene, write] : entries) {
        SctMeaningTarget variable;
        variable.variable = write.variable;
        variable.value = 1;
        SctExplanationRecord e;
        e.key = "entry-bit:" + std::to_string(scene.script.value) + ":" + std::to_string(scene.section->value());
        e.rule = "cutscene-entry-bit";
        e.ruleVersion = "1";
        e.revision = state.project.revision;
        e.interpretation = label(state, scene, describe(state, scene)) + " has been entered";
        e.reasoning = "A literal SetBit occurs at the section entry, optionally after its label prefix and leading condition. Section "
                      "naming or explicit module ownership identifies a scene candidate.";
        e.qualification = "Entered does not mean completed. Static state dependency, not runtime reachability. Helpers "
                          "and other writers can change this state.";
        if (writers[write.variable].size() != 1)
            e.qualification += " Multiple writers: a scene-specific alias needs user interpretation.";
        if (!a.diagnostics.empty())
            e.qualification += " Analysis is incomplete; dynamic writes or unavailable scripts may affect this flag.";
        e.subjects = {scene, write.target, variable};
        e.evidence = {scene, variable};
        for (const auto &t : e.evidence)
            e.observations.push_back(observe(state, t));
        a.suggestions.push_back(std::move(e));
    }
    return a;
}
Result<SctAuthoringState> SctInterpretation::record(const SctAuthoringState &state,
                                                    const SctInterpretationAnalysis &a) {
    if (a.project != state.project.id || a.revision != state.project.revision)
        return Result<SctAuthoringState>::failure(error("Analysis belongs to a different project revision."));
    auto next = state;
    for (auto e : a.suggestions) {
        if (!fresh(state, e))
            return Result<SctAuthoringState>::failure(error("Analysis evidence has changed."));
        const SctExplanationRecord *previous = nullptr;
        for (const auto &old : next.project.explanations)
            if (old.key == e.key)
                previous = &old;
        if (previous && previous->observations == e.observations && previous->ruleVersion == e.ruleVersion)
            continue;
        if (previous) {
            e.supersedes = previous->id;
            for (auto &old : next.project.explanations)
                if (old.id == previous->id)
                    old.decision = SctInterpretationDecision::Superseded;
        }
        auto id = next.project.allocate<SctExplanationId>();
        if (!id)
            return Result<SctAuthoringState>::failure(id.diagnostics());
        e.id = id.value();
        next.project.explanations.push_back(std::move(e));
    }
    return checked(std::move(next));
}
Result<SctAuthoringState> SctInterpretation::decide(const SctAuthoringState &state, SctExplanationId id,
                                                    SctInterpretationDecision decision) {
    const auto *old = explanation(state.project, id);
    if (!old)
        return Result<SctAuthoringState>::failure(error("Interpretation no longer exists."));
    if (decision != SctInterpretationDecision::Accepted && decision != SctInterpretationDecision::Rejected)
        return Result<SctAuthoringState>::failure(error("Choose accept or reject."));
    if (!fresh(state, *old))
        return Result<SctAuthoringState>::failure(error("Evidence changed. Refresh analysis before deciding."));
    auto next = state;
    auto e = *old;
    auto allocated = next.project.allocate<SctExplanationId>();
    if (!allocated)
        return Result<SctAuthoringState>::failure(allocated.diagnostics());
    e.id = allocated.value();
    e.supersedes = old->id;
    e.decision = decision;
    e.revision = state.project.revision;
    for (auto &r : next.project.explanations)
        if (r.id == id)
            r.decision = SctInterpretationDecision::Superseded;
    if (decision == SctInterpretationDecision::Accepted)
        for (const auto &t : e.subjects)
            if (t.variable && t.value) {
                auto m = std::ranges::find(next.project.meanings, t, &SctMeaning::target);
                // Explicit names always win over rescanning or accepting a suggestion.
                if (m != next.project.meanings.end()) {
                    const auto *oldMeaning = m->explanation ? explanation(next.project, *m->explanation) : nullptr;
                    if (!oldMeaning || oldMeaning->origin == SctEvidenceKind::UserAssertion)
                        continue;
                    m->name = e.interpretation;
                    m->explanation = e.id;
                    continue;
                }
                auto mid = next.project.allocate<SctMeaningId>();
                if (!mid)
                    return Result<SctAuthoringState>::failure(mid.diagnostics());
                next.project.meanings.push_back({mid.value(), t, e.interpretation, e.id});
            }
    next.project.explanations.push_back(std::move(e));
    return checked(std::move(next));
}
Result<SctAuthoringState> SctInterpretation::setName(const SctAuthoringState &state, SctMeaningTarget target,
                                                     std::string name) {
    if (name.empty())
        return Result<SctAuthoringState>::failure(error("A display name is required."));
    auto next = state;
    auto found = std::ranges::find(next.project.meanings, target, &SctMeaning::target);
    SctMeaning m;
    if (found != next.project.meanings.end())
        m = *found;
    else {
        auto id = next.project.allocate<SctMeaningId>();
        if (!id)
            return Result<SctAuthoringState>::failure(id.diagnostics());
        m.id = id.value();
        m.target = target;
    }
    auto id = next.project.allocate<SctExplanationId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    SctExplanationRecord e;
    e.id = id.value();
    e.key = "name:" + std::to_string(m.id.value);
    e.rule = "user-name";
    e.ruleVersion = "1";
    e.origin = SctEvidenceKind::UserAssertion;
    e.decision = SctInterpretationDecision::Accepted;
    e.revision = state.project.revision;
    e.interpretation = name;
    e.reasoning = "User assigned a display name. Executable content and resource binding are unchanged.";
    e.subjects = {target};
    e.evidence = {target};
    e.observations = {observe(state, target)};
    e.supersedes = m.explanation;
    if (m.explanation)
        for (auto &r : next.project.explanations)
            if (r.id == *m.explanation)
                r.decision = SctInterpretationDecision::Superseded;
    m.name = std::move(name);
    m.explanation = e.id;
    if (found != next.project.meanings.end())
        *found = m;
    else
        next.project.meanings.push_back(m);
    next.project.explanations.push_back(std::move(e));
    return checked(std::move(next));
}
} // namespace salsa::core
