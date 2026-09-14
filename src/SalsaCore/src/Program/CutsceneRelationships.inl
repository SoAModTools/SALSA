// Cutscene views are disposable, scoped projections over authoritative IR.
CutsceneQuery ServiceOwner::cutscene(Revision revision, SctModuleId module, std::stop_token stop) {
    match(revision, stop);
    require(data != nullptr, "No program is open");
    auto &d = *data;
    auto *m = d.lookup(module.value, EntityKind::Module, d.metadata.modules);
    require(m, "Missing cutscene module");
    if (auto it = cutscenes.find(module); it != cutscenes.end())
        return {status(), it->second, true};
    auto base = summarize(revision, module, stop, true).summary;
    auto out = std::make_shared<CutsceneSummary>();
    out->module = module;
    out->build = ++summaryBuild;
    out->diagnostics = base->diagnostics;
    out->incomplete = base->unknown;
    out->metadata.push_back(module.value);
    out->payload = base->payload;
    CopyValues copy{d, out->payload};
    std::set<SctActionId> owned, allowed, dependencies;
    std::set<SctSequenceId> sequences;
    for (auto id : base->actions)
        if (moduleContains(d, *m, d.action(id)->owner))
            owned.insert(id);
    out->entryTargets.assign(owned.begin(), owned.end());
    std::map<SctActionId, std::vector<const FlowRelationship *>> flow;
    for (auto &e : base->flow)
        flow[e.key.source].push_back(&e);
    auto otherModule = [&](SctActionId id) {
        auto *a = d.action(id);
        if (!a)
            return false;
        for (auto &other : d.metadata.modules)
            if (other.id != module && other.script == m->script && moduleContains(d, other, a->owner))
                return true;
        return false;
    };
    std::vector<SctActionId> pending(owned.begin(), owned.end());
    while (!pending.empty()) {
        stopped(stop);
        auto id = pending.back();
        pending.pop_back();
        if (!allowed.insert(id).second)
            continue;
        for (auto *e : flow[id])
            if (e->target.valid() && !otherModule(e->target))
                pending.push_back(e->target);
    }
    dependencies = allowed;
    for (auto f : base->effects)
        if (allowed.contains(f.site.action))
            out->effects.push_back(f);
    for (auto seq : m->sequences)
        sequences.insert(seq);
    auto belongs = [&](const Port &p) {
        auto *owner = std::get_if<SctModuleId>(&p.owner);
        return owner && *owner == module;
    };
    for (auto &p : d.metadata.ports)
        if (belongs(p)) {
            out->ports.push_back(p);
            out->metadata.push_back(p.id.value);
        }
    auto requirement = [&](const RouteRequirement &q) {
        dependencies.insert(q.site.action);
        ValueId value;
        if (q.site.condition) {
            auto *c = d.lookup(q.site.condition->value, EntityKind::Condition, d.conditions);
            if (c)
                value = c->result;
        } else if (q.site.parameter)
            value = routeParameter(d, q.site.action, *q.site.parameter);
        if (!value.valid())
            out->incomplete = true;
        return copy.copy(value);
    };
    auto requirements = [&](CutsceneRouteView &view) {
        for (auto &path : view.paths) {
            for (auto &q : path.requirements) {
                view.requirementValues.push_back(requirement(q));
                if (q.site.condition)
                    if (auto *condition = d.lookup(q.site.condition->value, EntityKind::Condition, d.conditions)) {
                        auto *prep = d.sequence(condition->preparation);
                        sequences.insert(prep->id);
                        for (auto id : *prep->order)
                            if (std::ranges::find(path.preparation, id) == path.preparation.end())
                                path.preparation.push_back(id);
                    }
            }
            dependencies.insert(path.preparation.begin(), path.preparation.end());
            dependencies.insert(path.consumption.begin(), path.consumption.end());
        }
    };
    auto dispatchRequirements = [&](const Arm &selected) {
        std::vector<RouteRequirement> result;
        const auto *root = d.action(selected.owner);
        const auto &region = d.dispatches[root->detail];
        for (std::uint32_t i = 0; i < root->armCount; ++i) {
            const auto &arm = d.arms[root->firstArm + i];
            if (region.mode == DispatchMode::FirstMatch || arm.id == selected.id)
                result.push_back({{root->id, {}, arm.id, 0, arm.condition},
                                  arm.id == selected.id ? RequirementMatch::True : RequirementMatch::False,
                                  {}});
            if (arm.id == selected.id)
                break;
        }
        return result;
    };
    // Collect enclosing structured checks and raw branches on the selected caller's
    // straight-line region. Cycles/too many alternatives remain explicitly incomplete.
    auto pathsTo = [&](SctActionId source) {
        std::vector<RoutePath> paths(1);
        auto seq = d.action(source)->owner;
        sequences.insert(seq);
        while (auto *s = d.sequence(seq)) {
            if (!s->owner)
                break;
            const auto parent = d.parentAction(*s);
            const auto *a = d.action(parent);
            dependencies.insert(parent);
            if (a->kind == ActionKind::Dispatch && s->role == SequenceRole::ArmBody) {
                for (std::uint32_t i = 0; i < a->armCount; ++i) {
                    const auto &arm = d.arms[a->firstArm + i];
                    if (arm.body == seq) {
                        auto required = dispatchRequirements(arm);
                        for (auto &path : paths)
                            path.requirements.insert(path.requirements.end(), required.begin(), required.end());
                    }
                }
            } else if (a->kind == ActionKind::Conditional && s->role == SequenceRole::ArmBody) {
                for (std::uint32_t i = 0; i < a->armCount; ++i) {
                    const auto &arm = d.arms[a->firstArm + i];
                    if (arm.condition.valid()) {
                        RouteRequirement q{{parent, {}, arm.id, 0, arm.condition},
                                           arm.body == seq ? RequirementMatch::True : RequirementMatch::False,
                                           {}};
                        for (auto &path : paths)
                            path.requirements.push_back(q);
                    }
                    if (arm.body == seq)
                        break;
                }
            } else if (a->kind == ActionKind::Switch && s->role == SequenceRole::ArmBody) {
                for (std::uint32_t i = 0; i < a->armCount; ++i) {
                    auto &arm = d.arms[a->firstArm + i];
                    if (arm.body != seq)
                        continue;
                    auto c = d.switches[a->detail].selector;
                    RouteRequirement q{{parent, {}, {}, 0, c},
                                       arm.includesDefault ? RequirementMatch::Default : RequirementMatch::Case,
                                       {}};
                    auto labels = std::span(d.caseLabels).subspan(arm.firstLabel, arm.labelCount);
                    q.labels.assign(labels.begin(), labels.end());
                    for (auto &path : paths)
                        path.requirements.push_back(q);
                }
            } else if (a->kind == ActionKind::Loop && s->role != SequenceRole::LoopInitialize) {
                auto &loop = d.loops[a->detail];
                if (loop.kind != LoopKind::DoWhile)
                    for (auto &path : paths)
                        path.requirements.push_back(
                            {{parent, {}, {}, 0, loop.condition}, RequirementMatch::True, {}});
                else
                    out->incomplete = true;
            }
            seq = a->owner;
            sequences.insert(seq);
        }
        const auto *s = d.sequence(d.action(source)->owner);
        if (!s->order || s->order->empty() || s->order->front() == source)
            return paths;
        struct Walk {
            SctActionId id;
            RoutePath path;
            std::set<SctActionId> seen;
        };
        std::vector<Walk> work{{s->order->front(), {}, {}}};
        std::vector<RoutePath> raw;
        std::size_t count = 0;
        while (!work.empty() && count++ < 4096 && raw.size() < 64) {
            stopped(stop);
            auto w = std::move(work.back());
            work.pop_back();
            if (w.id == source) {
                raw.push_back(std::move(w.path));
                continue;
            }
            if (!w.seen.insert(w.id).second) {
                out->incomplete = true;
                continue;
            }
            auto *a = d.action(w.id);
            if (!a || a->owner != s->id) {
                out->incomplete = true;
                continue;
            }
            dependencies.insert(w.id);
            auto index = d.directory.find(w.id.value).index;
            SctActionId next = index < d.successors.size() ? d.successors[index] : SctActionId{};
            auto follow = [&](SctActionId target, std::optional<RouteRequirement> q = {}) {
                if (!target.valid())
                    return;
                auto nw = w;
                nw.id = target;
                if (q)
                    nw.path.requirements.push_back(*q);
                work.push_back(std::move(nw));
            };
            if (a->kind != ActionKind::Instruction) {
                follow(next);
                continue;
            }
            auto opcode = d.values.find(a->value.value)->data;
            auto facts = actionFacts(d, std::span(&w.id, 1), out->diagnostics);
            if (opcode == 0 || opcode == 3 || opcode == 10) {
                for (auto &target : facts.targets) {
                    std::optional<RouteRequirement> q;
                    if (opcode == 0)
                        q = RouteRequirement{
                            {w.id, spice::sct::SctParameterAddress{0, {}}}, RequirementMatch::False, {}};
                    if (opcode == 3) {
                        q = RouteRequirement{
                            {w.id, spice::sct::SctParameterAddress{0, {}}}, RequirementMatch::Case, {}};
                        auto value =
                            routeParameter(d, w.id,
                                           {2, target.site.parameter ? target.site.parameter->repeatedGroupOrdinal
                                                                     : std::nullopt});
                        if (auto *v = d.values.find(value.value); v && v->kind == ValueKind::Word)
                            q->labels.push_back(static_cast<std::int32_t>(v->data));
                        else
                            out->incomplete = true;
                    }
                    follow(target.target, q);
                }
                if (opcode == 0)
                    follow(next, RouteRequirement{
                                     {w.id, spice::sct::SctParameterAddress{0, {}}}, RequirementMatch::True, {}});
            } else if (opcode != 12)
                follow(next);
        }
        if (!work.empty() || raw.empty())
            out->incomplete = true;
        if (raw.empty())
            return paths;
        for (auto &path : raw)
            path.requirements.insert(path.requirements.begin(), paths.front().requirements.begin(),
                                     paths.front().requirements.end());
        return raw;
    };
    auto methodFor = [&](const Entrypoint &e) {
        switch (e.kind) {
        case SctEntrypointKind::Arrival:
            return EntryMethod::Arrival;
        case SctEntrypointKind::DirectHandler:
            return EntryMethod::Handler;
        case SctEntrypointKind::SignalConsumer:
            return EntryMethod::Signal;
        case SctEntrypointKind::NamedSection:
            return EntryMethod::NamedEntry;
        default:
            return EntryMethod::Unknown;
        }
    };
    for (auto target : owned)
        for (auto &ref : incoming(d, target)) {
            if (owned.contains(ref.site.action))
                continue;
            CutsceneRouteView view;
            view.source = ref.site.action;
            view.target = target;
            view.operand = ref.site.parameter;
            view.method = EntryMethod::InternalCall;
            const auto *action = d.action(view.source);
            auto opcode =
                action->kind == ActionKind::Instruction ? d.values.find(action->value.value)->data : UINT64_MAX;
            view.role = opcode == 11   ? FlowRole::Call
                        : opcode == 10 ? FlowRole::Jump
                        : opcode == 0  ? FlowRole::FalseBranch
                        : opcode == 3  ? FlowRole::Case
                                       : FlowRole::Unknown;
            view.paths = pathsTo(view.source);
            dependencies.insert(view.source);
            view.evidence.push_back({SctEvidenceKind::Inference,
                                     "Exact incoming action reference; guards are bounded paths through "
                                     "the caller region and enclosing constructs"});
            if (opcode == 0)
                for (auto &path : view.paths)
                    path.requirements.push_back(
                        {{view.source, spice::sct::SctParameterAddress{0, {}}}, RequirementMatch::False, {}});
            if (opcode == 3 && ref.site.parameter) {
                RouteRequirement q{
                    {view.source, spice::sct::SctParameterAddress{0, {}}}, RequirementMatch::Case, {}};
                auto value = routeParameter(d, view.source, {2, ref.site.parameter->repeatedGroupOrdinal});
                if (auto *v = d.values.find(value.value); v && v->kind == ValueKind::Word)
                    q.labels.push_back(static_cast<std::int32_t>(v->data));
                else
                    out->incomplete = true;
                for (auto &path : view.paths)
                    path.requirements.push_back(q);
            }
            if (action->kind == ActionKind::Instruction && (d.values.find(action->value.value)->detail & 2))
                view.schedule = copy.copy(d.child(action->value).front());
            for (auto &e : d.metadata.entrypoints)
                if (e.responseSite && e.responseSite->action == view.source &&
                    e.responseSite->parameter == view.operand) {
                    view.method = methodFor(e);
                    out->metadata.push_back(e.id.value);
                    view.evidence = e.evidence;
                }
            for (auto &p : out->ports)
                if (p.role == SlotRole::Entry && slotEntry(d, p) == target) {
                    view.slot = p.id;
                    break;
                }
            requirements(view);
            out->incoming.push_back(std::move(view));
        }
    // Named engine entries and physical section adjacency have no operand posting.
    for (const auto &entry : d.metadata.entrypoints) {
        auto *named = std::get_if<SctNamedActivation>(&entry.activation);
        if (!named || !owned.contains(named->body))
            continue;
        CutsceneRouteView view;
        view.target = named->body;
        view.method = methodFor(entry);
        view.role = FlowRole::Unknown;
        view.evidence = entry.evidence;
        view.evidence.push_back(
            {SctEvidenceKind::Inference, "Declared external section activation targets this module body"});
        out->metadata.push_back(entry.id.value);
        out->incoming.push_back(std::move(view));
    }
    const auto &script = d.scripts[d.directory.find(m->script.value).index];
    for (std::size_t i = 1; i < script.sections.size(); ++i) {
        auto &section = d.sections[d.directory.find(script.sections[i].value).index];
        if (!section.body.valid())
            continue;
        auto *seq = d.sequence(section.body);
        if (seq->order->empty() || !owned.contains(seq->order->front()))
            continue;
        auto &previous = d.sections[d.directory.find(script.sections[i - 1].value).index];
        out->sections.push_back(previous.id);
        out->sections.push_back(section.id);
        if (!previous.body.valid())
            continue;
        auto *prior = d.sequence(previous.body);
        sequences.insert(prior->id);
        if (prior->order->empty())
            continue;
        auto source = prior->order->back();
        if (owned.contains(source))
            continue;
        auto *a = d.action(source);
        dependencies.insert(source);
        if (a->kind != ActionKind::Instruction) {
            out->incomplete = true;
            continue;
        }
        auto opcode = d.values.find(a->value.value)->data;
        if (opcode == 10 || opcode == 12 || opcode == 3)
            continue;
        CutsceneRouteView view;
        view.source = source;
        view.target = seq->order->front();
        view.role = FlowRole::Fallthrough;
        view.paths = pathsTo(source);
        if (opcode == 0)
            for (auto &path : view.paths)
                path.requirements.push_back(
                    {{source, spice::sct::SctParameterAddress{0, {}}}, RequirementMatch::True, {}});
        view.evidence.push_back(
            {SctEvidenceKind::Inference, "Physical fallthrough from the preceding script section"});
        requirements(view);
        out->incoming.push_back(std::move(view));
    }
    for (auto &e : base->flow) {
        if (!owned.contains(e.key.source))
            continue;
        const bool outside = e.target.valid() && !owned.contains(e.target);
        if (!outside && e.key.role != FlowRole::Return && e.key.role != FlowRole::Transition &&
            !(e.key.role == FlowRole::Fallthrough && !e.target.valid()))
            continue;
        CutsceneRouteView view;
        view.source = e.key.source;
        view.target = e.target;
        view.role = e.key.role;
        view.operand = e.operand;
        view.schedule = e.schedule;
        view.outcome = e.key.role == FlowRole::Return       ? RouteOutcome::ReturnToCaller
                       : e.key.role == FlowRole::Transition ? RouteOutcome::ScriptTransition
                       : e.key.role == FlowRole::Call
                           ? (e.schedule.valid() ? RouteOutcome::ScheduledCall : RouteOutcome::Observe)
                           : RouteOutcome::LocalTransfer;
        view.unresolved =
            e.key.role == FlowRole::Transition || (e.key.role == FlowRole::Fallthrough && !e.target.valid());
        view.evidence.push_back(
            {SctEvidenceKind::Inference,
             "Control-flow edge leaves the selected module body; helper returns are excluded"});
        for (auto &entry : d.metadata.entrypoints)
            if (entry.script == m->script && entry.required() && entry.name == "loop") {
                auto *seq = d.sequence(entry.body);
                if (!seq->order->empty() && seq->order->front() == view.target && view.role != FlowRole::Call)
                    view.outcome = RouteOutcome::ResumeLoop;
            }
        view.paths = pathsTo(view.source);
        requirements(view);
        out->exits.push_back(std::move(view));
    }
    for (auto &c : d.metadata.connections)
        if (c.cutscene) {
            auto findPort = [&](auto &ref) -> const Port * {
                auto *id = std::get_if<SctPortId>(&ref);
                return id ? d.lookup(id->value, EntityKind::Port, d.metadata.ports) : nullptr;
            };
            const auto *src = findPort(c.source);
            const auto *dst = findPort(c.destination);
            const bool hasDestination = c.cutscene->outcome != RouteOutcome::ReturnToCaller &&
                                        c.cutscene->outcome != RouteOutcome::ResumeLoop;
            bool input = hasDestination && dst && belongs(*dst), output = src && belongs(*src);
            if (!input && !output)
                continue;
            out->metadata.push_back(c.id.value);
            if (src)
                out->metadata.push_back(src->id.value);
            if (dst)
                out->metadata.push_back(dst->id.value);
            const auto &r = *c.cutscene;
            CutsceneRouteView view;
            view.connection = c.id;
            view.inferred = false;
            view.outcome = r.outcome;
            view.method = r.method;
            view.evidence = c.evidence;
            view.paths = r.paths;
            if (r.dispatchArm) {
                const auto *arm = d.lookup(r.dispatchArm->value, EntityKind::Arm, d.arms);
                if (view.paths.empty())
                    view.paths.push_back({});
                auto current=dispatchRequirements(*arm);
                for (auto &path : view.paths) {
                    std::erase_if(path.requirements,[&](const auto& q){return q.site.action==arm->owner;});
                    path.requirements.insert(path.requirements.end(),current.begin(),current.end());
                }
            }
            view.source = r.implementation.value_or(src ? slotEntry(d, *src) : SctActionId{});
            view.target = hasDestination && dst ? slotEntry(d, *dst) : r.boundTarget.value_or(SctActionId{});
            view.operand = r.targetOperand;
            view.unresolved = !src || (!dst && r.outcome != RouteOutcome::ReturnToCaller &&
                                       r.outcome != RouteOutcome::ResumeLoop);
            if (hasDestination && dst)
                view.destinationScript = slotScript(d, *dst);
            if (r.entrypoint)
                out->metadata.push_back(r.entrypoint->value);
            if (r.arrival)
                out->metadata.push_back(r.arrival->value);
            if (view.source.valid()) {
                dependencies.insert(view.source);
                auto *a = d.action(view.source);
                if (a && a->kind == ActionKind::Instruction) {
                    auto *instruction = d.values.find(a->value.value);
                    view.role = instruction->data == 11   ? FlowRole::Call
                                : instruction->data == 10 ? FlowRole::Jump
                                : instruction->data == 12 ? FlowRole::Return
                                : instruction->data == 43 ? FlowRole::Transition
                                : instruction->data == 0  ? FlowRole::FalseBranch
                                : instruction->data == 3  ? FlowRole::Case
                                                          : FlowRole::Unknown;
                    if (instruction->detail & 2)
                        view.schedule = copy.copy(d.child(a->value).front());
                    if (r.implementation && r.targetOperand) {
                        auto value = routeParameter(d, view.source, *r.targetOperand);
                        auto *v = d.values.find(value.value);
                        if (r.outcome == RouteOutcome::ScriptTransition) {
                            const auto *text = v && v->kind == ValueKind::SupplementaryTextReference
                                                   ? d.lookup(v->data, EntityKind::Text, d.texts)
                                                   : nullptr;
                            if (text) {
                                out->texts.push_back(text->id);
                                auto *t = d.values.find(text->value.value);
                                view.changed =
                                    t->kind != ValueKind::PlainText ||
                                    std::string_view(d.characters.data() + t->data, t->detail) != r.boundFilename;
                            } else
                                view.changed = true;
                            view.changed |= instruction->data != 43;
                            if (r.arrival) {
                                auto *entry =
                                    d.lookup(r.arrival->value, EntityKind::Entry, d.metadata.entrypoints);
                                if (entry && entry->responseSite && entry->responseSite->parameter) {
                                    dependencies.insert(entry->responseSite->action);
                                    auto response = routeParameter(d, entry->responseSite->action,
                                                                   *entry->responseSite->parameter);
                                    auto *ref = d.values.find(response.value);
                                    view.changed |= !ref || ref->kind != ValueKind::ActionReference ||
                                                    ref->data != view.target.value;
                                } else
                                    view.changed = true;
                            }
                        } else
                            view.changed = !v || v->kind != ValueKind::ActionReference || !r.boundTarget ||
                                           v->data != r.boundTarget->value ||
                                           (hasDestination && dst && view.target != *r.boundTarget);
                    } else if (r.implementation && r.outcome == RouteOutcome::ReturnToCaller)
                        view.changed = instruction->data != 12;
                    if (r.implementationOpcode)
                        view.changed |= instruction->data != *r.implementationOpcode;
                } else
                    view.changed = true;
            }
            if (src && r.implementation)
                view.changed |= !slotImplementation(d, *src, *r.implementation);
            requirements(view);
            // Authored and inferred records remain distinguishable by stable connection ID.
            if (input) {
                view.slot = dst->id;
                out->incoming.push_back(view);
            }
            if (output) {
                view.slot = src->id;
                out->exits.push_back(std::move(view));
            }
        }
    out->actions.assign(dependencies.begin(), dependencies.end());
    for (auto id : out->actions)
        if (auto *a = d.action(id))
            sequences.insert(a->owner);
    out->sequences.assign(sequences.begin(), sequences.end());
    out->sections.insert(out->sections.end(), base->sections.begin(), base->sections.end());
    unique(out->sections);
    out->texts.insert(out->texts.end(), base->texts.begin(), base->texts.end());
    unique(out->texts);
    unique(out->metadata);
    stopped(stop);
    stopped(shutdown);
    cutscenes[module] = out;
    return {status(), std::move(out), false};
}
