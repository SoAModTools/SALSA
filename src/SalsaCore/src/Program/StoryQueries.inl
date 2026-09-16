namespace {
std::string storyExpression(const ValueBuffer &buffer, ValueId id) {
    if (!id.valid())
        return {};
    auto value = ProgramValues::expression(buffer, id);
    return value ? SctExpressionLanguage::project(value.value()).text : "[expression unavailable]";
}
std::string variableText(SctVariableKey v) {
    const char *banks[] = {"BitVar", "ByteVar", "IntVar", "FloatVar"};
    return std::string(banks[static_cast<unsigned>(v.kind)]) + "[" + std::to_string(v.index) + "]";
}
const VariableMeaning *storyMeaning(const ProgramData &d, SctScriptId script, SctVariableKey v,
                                    std::optional<std::int64_t> value = {}) {
    const VariableMeaning *result = nullptr;
    for (const auto &m : d.metadata.meanings)
        if (m.variable == v && m.value == value && (!m.script.valid() || m.script == script))
            if (!result || m.script.valid())
                result = &m;
    return result;
}
std::string storyAlias(const ProgramData &d, editor::Subject subject, std::string fallback) {
    for (const auto &a : d.metadata.annotations)
        if (a.subject == subject && !a.alias.empty())
            return a.alias;
    return fallback;
}
std::string friendlyCondition(const ProgramData &d, SctScriptId script, std::string text) {
    std::set<SctVariableKey> variables;
    for (const auto &m : d.metadata.meanings)
        variables.insert(m.variable);
    for (auto v : variables) {
        auto token = variableText(v);
        for(const auto &candidate:d.metadata.meanings) {
            if(candidate.variable!=v || !candidate.value || storyMeaning(d,script,v,candidate.value)!=&candidate)continue;
            auto comparison=token+" == "+std::to_string(*candidate.value);
            std::size_t at=0;
            while((at=text.find(comparison,at))!=std::string::npos) {
                auto end=at+comparison.size();
                if(end<text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end]=='.')){at=end;continue;}
                auto label="["+candidate.name+"]";text.replace(at,comparison.size(),label);at+=label.size();
            }
        }
        auto *meaning = storyMeaning(d, script, v);
        if (!meaning)
            continue;
        std::size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.replace(pos, token.size(), meaning->name);
            pos += meaning->name.size();
        }
    }
    return text;
}
} // namespace
std::future<Result<editor::StoryCatalog>> ProgramService::storyCatalog(Revision r, std::stop_token stop) {
    return control_->submit<editor::StoryCatalog>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        editor::StoryCatalog out;
        out.revision = r;
        for (const auto &s : d.scripts)
            out.scripts.push_back({s.context.id, s.context.name});
        for (const auto &m : d.metadata.modules)
            out.modules.push_back({m.id, m.script, storyAlias(d, m.id, m.name), m.sequences});
        out.meanings = d.metadata.meanings;
        out.narrative = d.metadata.narrative;
        for (const auto &p : d.metadata.ports)
            out.endpoints.push_back(
                {p.id, p.owner, p.name, p.action, p.sequence, p.role, p.boundary, p.operand});
        for (const auto &c : d.metadata.connections) {
            editor::RouteItem row;
            row.id = c.id;
            row.evidence = c.evidence;
            if (auto p = std::get_if<SctPortId>(&c.source))
                row.source = *p;
            if (auto p = std::get_if<SctPortId>(&c.destination))
                row.destination = *p;
            if (c.cutscene) {
                auto &v = *c.cutscene;
                row.method = v.method;
                row.outcome = v.outcome;
                row.arrival = v.arrival;
                row.requirement = v.dispatchArm;
                row.implementation = v.implementation;
            }
            out.routes.push_back(row);
        }
        return out;
    });
}
std::future<Result<editor::StoryView>> ProgramService::story(Revision r, editor::StoryRequest request,
                                                             std::stop_token stop) {
    return control_->submit<editor::StoryView>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        editor::StoryView out;
        out.revision = r;
        out.narrative = d.metadata.narrative;
        out.enabled = d.metadata.storyEnabled;
        for (const auto &script : d.scripts)
            out.scripts.emplace_back(script.context.id, script.context.name);
        if (!out.enabled && !request.analyze) {
            for (const auto &m : d.metadata.modules)
                out.modules.push_back({m.id, m.script, storyAlias(d, m.id, m.name), m.sequences});
            return out; // Script editing and reopening do not start story analysis.
        }
        auto progress = [&](const char *phase, std::size_t done, std::size_t total,
                            const std::string &item = {}) {
            stopped(stop);
            if (request.progress)
                request.progress({phase, done, total, item});
        };
        progress("Reviewing whole-project organization", 0, d.scripts.size());
        std::set<SctSequenceId> occupied;
        for (const auto &m : d.metadata.modules)
            for (auto body : m.sequences) {
                auto *seq = d.sequence(body);
                while (seq && seq->owner)
                    seq = d.sequence(d.action(d.parentAction(*seq))->owner);
                if (seq)
                    occupied.insert(seq->id);
            }
        std::size_t reviewed = 0;
        for (const auto &script : d.scripts) {
            for (auto id : script.sections) {
                auto *section = d.lookup(id.value, EntityKind::Section, d.sections);
                if (!section || !section->body.valid() || occupied.contains(section->body) ||
                    section->name == "init" || section->name == "loop")
                    continue;
                ++out.unorganizedSections;
                if (request.analyze)
                    out.candidates.push_back({{}, script.context.id, section->name, {section->body}});
            }
            progress("Reviewing whole-project organization", ++reviewed, d.scripts.size(),
                     script.context.name);
        }
        std::map<SctActionId, std::set<editor::Subject>> owners;
        std::map<SctModuleId, std::shared_ptr<const ModuleSummary>> summaries;
        std::size_t analyzed = 0;
        for (const auto &m : d.metadata.modules) {
            stopped(stop);
            progress("Analyzing project modules", analyzed++, d.metadata.modules.size(), m.name);
            auto q = o.summarize(r, m.id, stop);
            summaries.emplace(m.id, q.summary);
            out.modules.push_back({m.id, m.script, storyAlias(d, m.id, m.name), m.sequences});
            for (auto action : q.summary->actions)
                owners[action].insert(m.id);
            out.diagnostics.insert(out.diagnostics.end(), q.summary->diagnostics.begin(),
                                   q.summary->diagnostics.end());
        }
        // Entry ownership follows the same nested bodies as the semantic outline.
        for (const auto &entry : d.metadata.entrypoints) {
            std::vector<SctSequenceId> pending{entry.body};
            std::set<SctSequenceId> seen;
            while (!pending.empty()) {
                stopped(stop);
                auto seq = pending.back();
                pending.pop_back();
                if (!seen.insert(seq).second)
                    continue;
                for (auto id : *d.sequence(seq)->order) {
                    owners[id].insert(entry.id);
                    auto *a = d.action(id);
                    auto condition = [&](ConditionId id) {
                        if (id.valid())
                            pending.push_back(
                                d.lookup(id.value, EntityKind::Condition, d.conditions)->preparation);
                    };
                    for (std::uint32_t n = 0; n < a->armCount; ++n) {
                        const auto &arm = d.arms[a->firstArm + n];
                        pending.push_back(arm.body);
                        condition(arm.condition);
                    }
                    if (a->kind == ActionKind::Loop) {
                        const auto &loop = d.loops[a->detail];
                        condition(loop.condition);
                        for (auto body : {loop.initialize, loop.body, loop.update})
                            if (body.valid())
                                pending.push_back(body);
                    }
                    if (a->kind == ActionKind::Switch)
                        condition(d.switches[a->detail].selector);
                    if (a->kind == ActionKind::Dispatch)
                        for (const auto &sample : d.dispatches[a->detail].samples)
                            condition(sample.condition);
                }
            }
        }
        // Bring guards in callers/handlers into the target cutscene's interface.
        // These remain references to existing conditions, not new predicates.
        std::map<SctModuleId, std::shared_ptr<const CutsceneSummary>> interfaces;
        analyzed = 0;
        for (const auto &m : out.modules) {
            progress("Relating project entry and exit routes", analyzed++, out.modules.size(), m.name);
            auto q = o.cutscene(r, m.id, stop);
            interfaces.emplace(m.id, q.summary);
            for (const auto &route : q.summary->incoming)
                for (const auto &path : route.paths)
                    for (const auto &guard : path.requirements)
                        owners[guard.site.action].insert(m.id);
        }
        for (const auto &[target, summary] : interfaces)
            for (const auto &route : summary->incoming) {
                auto found = owners.find(route.source);
                if (found == owners.end())
                    continue;
                for (auto owner : found->second)
                    if (auto source = std::get_if<SctModuleId>(&owner); source && *source != target)
                        out.links.push_back({*source,
                                             target,
                                             {},
                                             route.source,
                                             route.target,
                                             "Observed control-flow entry; inspect guards and scheduling "
                                             "before adopting narrative order"});
            }
        static const char *access[] = {"Read",   "Set to 1", "Clear to 0",
                                       "Assign", "Modify",   "Computed write"};
        for (const auto &module : out.modules) {
            auto &summary = *summaries.at(module.id);
            for (const auto &use : summary.effects) {
                stopped(stop);
                if (!use.variable)
                    continue;
                SctVariableKey key{static_cast<SctVariableKind>(use.variable->bank), use.variable->index};
                auto *meaning = storyMeaning(d, module.script, key);
                bool classified = meaning && meaning->storyState;
                for (const auto &m : d.metadata.meanings)
                    if (m.variable == key && m.value && storyMeaning(d, module.script, key, m.value) == &m &&
                        m.storyState)
                        classified = true;
                if (!request.allEffects && !classified &&
                    !(request.unclassifiedBits && key.kind == SctVariableKind::Bit))
                    continue;
                auto expression = storyExpression(summary.payload, use.expression);
                auto display = meaning ? meaning->name : variableText(key);
                std::optional<std::int64_t> literal;
                if (use.access == Access::Set)
                    literal = 1;
                else if (use.access == Access::Clear)
                    literal = 0;
                if (literal)
                    if (auto *value = storyMeaning(d, module.script, key, literal))
                        display += " — " + value->name;
                out.effects.push_back({module.id, module.script, key, use.site.action,
                                       access[static_cast<unsigned>(use.access)], expression, display,
                                       use.access == Access::Read});
                if (use.access == Access::Read)
                    continue;
                for (const auto &read : program::uses(d, *use.variable))
                    if (read.access == Access::Read) {
                        auto targets = owners.find(read.site.action);
                        if (targets == owners.end()) {
                            out.links.push_back({module.id, read.site.action, key, use.site.action,
                                                 read.site.action,
                                                 display + ": reader outside an organized module"});
                            continue;
                        }
                        for (auto target : targets->second)
                            if (target != editor::Subject(module.id))
                                out.links.push_back({module.id, target, key, use.site.action,
                                                     read.site.action,
                                                     display + ": write/read relationship; does not "
                                                               "establish execution order or final state"});
                    }
            }
        }
        progress("Whole-project analysis ready", d.scripts.size(), d.scripts.size());
        return out;
    });
}
std::future<Result<editor::InteractionView>> ProgramService::storyInteraction(Revision r, SctScriptId script,
                                                                              std::stop_token stop) {
    return control_->submit<editor::InteractionView>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        require(d.lookup(script.value, EntityKind::Script, d.scripts), "Missing interaction script");
        editor::InteractionView out;
        out.revision = r;
        for (const auto &e : d.metadata.entrypoints)
            if (e.script == script) {
                editor::EntryItem v{e.id,      e.script,    storyAlias(d, e.id, e.name), e.kind, e.body,
                                    e.enabled, e.activation};
                v.provider = e.provider;
                out.entries.push_back(v);
            }
        for (const auto &c : d.conditions)
            if (c.id.valid() && d.sequence(c.preparation)->script == script) {
                stopped(stop);
                ValueBuffer b;
                CopyValues copy{d, b};
                auto root = copy.copy(c.result);
                auto raw = storyExpression(b, root);
                std::optional<ArmId> route;
                if (c.arm && d.action(c.owner)->kind == ActionKind::Dispatch)
                    route = c.arm;
                out.conditions.push_back({c.id, c.owner, raw, friendlyCondition(d, script, raw), route});
            }
        // Unrecognized comparisons remain editable instructions with exact operands.
        for (std::uint32_t n = 0; n < d.actions.size(); ++n) {
            auto *a = d.actions.find(n);
            if (!a->id.valid() || a->kind != ActionKind::Instruction ||
                d.sequence(a->owner)->script != script)
                continue;
            auto op = d.values.find(a->value.value)->data;
            if (op != 0)
                continue;
            ValueBuffer b;
            CopyValues copy{d, b};
            auto root = copy.copy(featureParameter(d, a->id, {0, {}}));
            auto raw = storyExpression(b, root);
            out.conditions.push_back({a->id, a->id, raw, friendlyCondition(d, script, raw), {}});
        }
        for (auto &condition : out.conditions) {
            auto seq = d.action(condition.owner)->owner;
            std::set<SctSequenceId> parents;
            while (auto *body = d.sequence(seq)) {
                if (!parents.insert(seq).second)
                    break;
                if (!body->owner)
                    break;
                seq = d.action(d.parentAction(*body))->owner;
            }
            for (const auto &entry : out.entries)
                if (parents.contains(entry.body))
                    condition.entrypoints.push_back(entry.id);
        }
        for (const auto &record : d.metadata.interpretations)
            if (std::ranges::any_of(out.conditions,
                                    [&](const auto &c) { return c.subject == record.subject; }) ||
                std::ranges::any_of(out.entries,
                                    [&](const auto &e) { return editor::Subject(e.id) == record.subject; }))
                out.decisions.push_back(record);
        return out;
    });
}
std::future<Result<editor::ItemExplanation>>
ProgramService::storyExplanation(Revision r, editor::Subject subject, std::stop_token stop) {
    return control_->submit<editor::ItemExplanation>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        editor::ItemExplanation out;
        out.revision = r;
        auto &d = *o.data;
        for (const auto &a : d.metadata.annotations)
            if (a.subject == subject)
                out.evidence.insert(out.evidence.end(), a.evidence.begin(), a.evidence.end());
        for (const auto &m : d.metadata.modules)
            if (editor::Subject(m.id) == subject)
                out.evidence.insert(out.evidence.end(), m.evidence.begin(), m.evidence.end());
        for (const auto &e : d.metadata.entrypoints)
            if (editor::Subject(e.id) == subject)
                out.evidence.insert(out.evidence.end(), e.evidence.begin(), e.evidence.end());
        for (const auto &c : d.metadata.connections)
            if (editor::Subject(c.id) == subject)
                out.evidence.insert(out.evidence.end(), c.evidence.begin(), c.evidence.end());
        for (const auto &record : d.metadata.interpretations)
            if (record.subject == subject)
                out.decisions.push_back(record);
        return out;
    });
}
std::future<Result<editor::OutputReview>>
ProgramService::reviewOutput(Revision r, std::vector<SctScriptId> roots, std::stop_token stop) {
    return control_->submit<editor::OutputReview>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        editor::OutputReview out;
        out.revision = r;
        out.roots = roots;
        std::map<SctScriptId, std::string> filenames;
        std::map<std::string, std::vector<SctScriptId>> names;
        for (const auto &s : d.scripts) {
            auto *b = d.lookup(s.context.baseline.value, EntityKind::Baseline, d.baselines);
            auto name = std::filesystem::path(b->source.locator.identityKey()).filename().string();
            filenames[s.context.id] = name;
            names[name].push_back(s.context.id);
        }
        std::map<SctScriptId, std::set<SctScriptId>> dependencies;
        std::map<SctScriptId, std::vector<std::string>> gaps;
        for (std::uint32_t n = 0; n < d.actions.size(); ++n) {
            stopped(stop);
            auto *a = d.actions.find(n);
            if (!a->id.valid() || a->kind != ActionKind::Instruction) continue;
            auto opcode=d.values.find(a->value.value)->data;
            auto script=d.sequence(a->owner)->script;
            if(opcode==210 || opcode==257 || opcode==238){gaps[script].push_back("Transition destination needs manual output selection");continue;}
            if(opcode!=43)continue;
            auto ref = featureParameter(d, a->id, {0, {}});
            auto *v = d.values.find(ref.value);
            auto *t = v->kind == ValueKind::SupplementaryTextReference
                          ? d.lookup(v->data, EntityKind::Text, d.texts)
                          : nullptr;
            if (!t || d.values.find(t->value.value)->kind != ValueKind::PlainText) {
                gaps[script].push_back("Nonliteral script load requires manual output selection");
                continue;
            }
            auto *text = d.values.find(t->value.value);
            std::string name(d.characters.data() + text->data, text->detail);
            auto found = names.find(name);
            if (found == names.end() || found->second.size() != 1)
                gaps[script].push_back("Unresolved or ambiguous load: " + name);
            else
                dependencies[script].insert(found->second.front());
        }
        for (const auto &c : d.metadata.connections)
            if (c.cutscene && c.cutscene->implementation) {
                auto s = std::get_if<SctPortId>(&c.source), t = std::get_if<SctPortId>(&c.destination);
                if (!s || !t)
                    continue;
                auto a = slotScript(d, *d.lookup(s->value, EntityKind::Port, d.metadata.ports)),
                     b = slotScript(d, *d.lookup(t->value, EntityKind::Port, d.metadata.ports));
                if (a != b) {
                    dependencies[a].insert(b);
                    dependencies[b].insert(a);
                }
            }
        std::set<SctScriptId> seen;
        std::vector<SctScriptId> pending = roots;
        require(!pending.empty(), "Select output roots");
        while (!pending.empty()) {
            stopped(stop);
            auto id = pending.back();
            pending.pop_back();
            require(filenames.contains(id), "Missing output script");
            if (!seen.insert(id).second)
                continue;
            auto name = filenames.at(id);
            require(names.at(name).size() == 1,
                    "Output filenames collide; resolve duplicate script names before export");
            out.items.push_back({id, name,
                                 std::ranges::find(roots, id) != roots.end()
                                     ? "Selected"
                                     : "Literal load or connected route dependency"});
            for (auto dep : dependencies[id])
                pending.push_back(dep);
            for (const auto &gap : gaps[id])
                out.diagnostics.push_back({DiagnosticSeverity::Warning,
                                           DiagnosticCode::UnresolvedSctAuthoringBinding, name + ": " + gap});
        }
        std::ranges::sort(out.items, {}, &editor::OutputItem::filename);
        return out;
    });
}
