// Scoped derived data; no model pointers or live pool handles escape.
void ServiceOwner::invalidate(const ChangeReceipt &receipt) noexcept {
    auto intersects = [](const auto &a, const auto &b) {
        // Summary dependencies are sorted; walk the usually much smaller receipt.
        for (const auto &x : b)
            if (std::binary_search(a.begin(), a.end(), x))
                return true;
        return false;
    };
    for(auto it=cutscenes.begin();it!=cutscenes.end();) {
        const auto &s=*it->second;
        if(intersects(s.actions,receipt.actions) || intersects(s.sequences,receipt.sequences) ||
           intersects(s.sections,receipt.sections) || !receipt.scriptLayouts.empty() || intersects(s.texts,receipt.texts) ||
           intersects(s.entryTargets,receipt.oldTargets) || intersects(s.entryTargets,receipt.newTargets) ||
           !receipt.metadata.empty() || !receipt.modules.empty()) it=cutscenes.erase(it);
        else ++it;
    }
    for (auto it = summaries.begin(); it != summaries.end();) {
        const auto &s = *it->second;
        if (std::find(receipt.modules.begin(), receipt.modules.end(), it->first) != receipt.modules.end() ||
            intersects(s.actions, receipt.actions) || intersects(s.sequences, receipt.sequences) ||
            intersects(s.sections, receipt.sections) || intersects(s.texts, receipt.texts))
            it = summaries.erase(it);
        else
            ++it;
    }
}
SummaryQuery ServiceOwner::summarize(Revision expected, SctModuleId module, std::stop_token stop, bool cutsceneScope) {
    match(expected, stop);
    require(data != nullptr, "No program is open");
    auto &d = *data;
    const auto *m = d.lookup(module.value, EntityKind::Module, d.metadata.modules);
    require(m, "Missing module");
    if (auto it = summaries.find(module); !cutsceneScope && it != summaries.end())
        return {status(), it->second, true};
    auto result = std::make_shared<ModuleSummary>();
    result->module = module;
    result->build = ++summaryBuild;
    CopyValues copy{d, result->payload};
    std::set<SctActionId> visited;
    std::set<SctSequenceId> seqs;
    std::set<SectionId> sections;
    std::set<SctTextResourceId> texts;
    std::vector<SctActionId> pending;
    auto sequence = [&](SctSequenceId id, bool all) -> SctActionId {
        if (!id.valid())
            return {};
        const auto *seq = d.sequence(id);
        require(seq, "Missing summary sequence");
        seqs.insert(id);
        if (!seq->order || seq->order->empty())
            return {};
        if (all)
            pending.insert(pending.end(), seq->order->begin(), seq->order->end());
        return seq->order->front();
    };
    std::function<SctActionId(SctActionId)> next = [&](SctActionId id) -> SctActionId {
        auto at = d.directory.find(id.value);
        const auto *a = d.action(id);
        if (!a)
            return {};
        if (at.index < d.successors.size() && d.successors[at.index].valid())
            return d.successors[at.index];
        auto *seq = d.sequence(a->owner);
        seqs.insert(seq->id);
        if (seq->owner) {
            auto parent = d.parentAction(*seq);
            auto *owner = d.action(parent);
            if (owner->kind == ActionKind::Loop) {
                auto l = d.loops[owner->detail];
                if (seq->role == SequenceRole::LoopBody && l.kind == LoopKind::For)
                    if (auto action = sequence(l.update, false); action.valid())
                        return action;
                return parent;
            }
            if (owner->kind == ActionKind::Switch && seq->role == SequenceRole::ArmBody) {
                for (std::uint32_t i = 0; i < owner->armCount; ++i) {
                    const auto &arm = d.arms[owner->firstArm + i];
                    if (arm.body == seq->id && arm.caseExit == CaseExit::Fallthrough && i + 1 < owner->armCount) {
                        if (auto action = sequence(d.arms[owner->firstArm + i + 1].body, false); action.valid())
                            return action;
                    }
                }
            }
            return next(parent);
        }
        const auto &script = d.scripts[d.directory.find(seq->script.value).index];
        bool found = false;
        for (auto id : script.sections) {
            const auto &sec = d.sections[d.directory.find(id.value).index];
            if (sec.body == seq->id) {
                sections.insert(id);
                found = true;
                continue;
            }
            if (!found)
                continue;
            sections.insert(id);
            if (!sec.body.valid()) {
                result->unknown = true;
                return {};
            }
            if (auto action = sequence(sec.body, false); action.valid())
                return action;
        }
        return {};
    };
    for (auto seq : m->sequences)
        sequence(seq, true);
    auto valueAt = [&](const Action &a, std::uint32_t schema, std::optional<std::uint32_t> ordinal = {}) -> ValueId {
        std::uint32_t group = 0;
        for (auto child : d.child(a.value)) {
            auto *v = d.values.find(child.value);
            if (v->kind == ValueKind::Parameter && !ordinal && v->data == schema)
                return d.child(child).front();
            if (v->kind == ValueKind::RepeatedGroup) {
                if (ordinal == group)
                    for (auto param : d.child(child))
                        if (d.values.find(param.value)->data == schema)
                            return d.child(param).front();
                ++group;
            }
        }
        return {};
    };
    while (!pending.empty()) {
        stopped(stop);
        stopped(shutdown);
        auto id = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second)
            continue;
        const auto *a = d.action(id);
        require(a, "Dangling summary action");
        seqs.insert(a->owner);
        auto script = d.sequence(a->owner)->script;
        auto facts = actionFacts(d, std::span(&id, 1), result->diagnostics);
        for (auto f : facts.accesses) {
            f.expression = copy.copy(f.expression);
            result->effects.push_back(f);
            result->variables.push_back(*f.variable);
        }
        for (auto f : facts.computed) {
            f.expression = copy.copy(f.expression);
            result->effects.push_back(f);
            result->unknown = true;
        }
        auto edge = [&](FlowRole role, SctActionId target, SctSequenceId seq = {}, ValueId predicate = {},
                        ConditionId condition = {}, std::uint64_t arm = 0,
                        std::uint32_t ordinal = 0) -> FlowRelationship & {
            FlowRelationship relation;
            relation.key = {script, id, role, arm, arm ? 0 : ordinal};
            relation.target = target;
            relation.sequence = seq;
            relation.predicate = copy.copy(predicate);
            relation.condition = condition;
            if (condition.valid())
                relation.preparation = d.conditions[d.directory.find(condition.value).index].preparation;
            relation.sourceInstruction = a->sourceInstruction;
            if (target.valid()) {
                bool boundary=false;
                if(cutsceneScope)for(const auto& other:d.metadata.modules)
                    if(other.id!=module && other.script==m->script && moduleContains(d,other,d.action(target)->owner)){boundary=true;break;}
                if(!boundary)pending.push_back(target);
            }
            result->flow.push_back(std::move(relation));
            return result->flow.back();
        };
        if (a->kind == ActionKind::Instruction) {
            auto instruction = *d.values.find(a->value.value);
            auto opcode = instruction.data;
            const auto schedule = (instruction.detail & 2) ? d.child(a->value).front() : ValueId{};
            auto annotate = [&](FlowRelationship &e) {
                e.schedule = copy.copy(schedule);
                e.skipRefresh = (instruction.detail & 1) != 0;
            };
            if (opcode == 12) {
                annotate(edge(FlowRole::Return, {}));
                continue;
            }
            auto predicate = (opcode == 0 || opcode == 3) ? valueAt(*a, 0) : ValueId{};
            for (auto ref : facts.targets) {
                auto role = opcode == 11   ? FlowRole::Call
                            : opcode == 10 ? FlowRole::Jump
                            : opcode == 0  ? FlowRole::FalseBranch
                            : opcode == 3  ? FlowRole::Case
                                           : FlowRole::Unknown;
                auto &e = edge(role, ref.target, {}, predicate, {}, 0,
                               ref.site.parameter && ref.site.parameter->repeatedGroupOrdinal
                                   ? *ref.site.parameter->repeatedGroupOrdinal
                                   : 0);
                e.operand = ref.site.parameter;
                annotate(e);
                if (opcode == 3 && ref.site.parameter) {
                    auto value = valueAt(*a, 2, ref.site.parameter->repeatedGroupOrdinal);
                    if (value.valid() && d.values.find(value.value)->kind == ValueKind::Word)
                        e.labels.push_back(static_cast<std::int32_t>(d.values.find(value.value)->data));
                    else
                        result->unknown = true;
                }
                if (role == FlowRole::Unknown)
                    result->unknown = true;
            }
            if (opcode == 43 || opcode == 210 || opcode == 257 || opcode == 238) {
                auto &e = edge(FlowRole::Transition, {});
                auto value = valueAt(*a, 0);
                e.predicate = copy.copy(value);
                annotate(e);
                result->unknown = true;
            }
            if (opcode != 10 && opcode != 3)
                annotate(edge(opcode == 0 ? FlowRole::TrueBranch : FlowRole::Fallthrough, next(id), {}, predicate));
            if ((opcode == 0 || opcode == 3 || opcode == 10 || opcode == 11) && facts.targets.empty())
                result->unknown = true;
            for (auto child : d.child(a->value)) {
                auto check = [&](ValueId parameter) {
                    auto *v = d.values.find(parameter.value);
                    if (v->kind != ValueKind::Parameter)
                        return;
                    auto content = d.child(parameter).front();
                    auto *payload = d.values.find(content.value);
                    if (payload->kind == ValueKind::IndexedTextReference ||
                        payload->kind == ValueKind::SupplementaryTextReference)
                        texts.insert(SctTextResourceId{payload->data});
                    if (payload->kind == ValueKind::Unresolved || payload->kind == ValueKind::Unavailable)
                        result->unknown = true;
                };
                if (d.values.find(child.value)->kind == ValueKind::RepeatedGroup)
                    for (auto parameter : d.child(child))
                        check(parameter);
                else
                    check(child);
            }
        } else if (a->kind == ActionKind::Transfer) {
            auto t = d.transfers[a->detail];
            auto *target = d.action(t.target);
            require(target, "Dangling transfer");
            SctSequenceId seq;
            if (t.kind == TransferKind::Continue) {
                auto l = d.loops[target->detail];
                seq = l.kind == LoopKind::For ? l.update
                                              : d.conditions[d.directory.find(l.condition.value).index].preparation;
            }
            edge(t.kind == TransferKind::Break ? FlowRole::Break : FlowRole::Continue,
                 t.kind == TransferKind::Break ? next(t.target) : sequence(seq, false), seq)
                .scopeTarget = t.target;
        } else {
            if (a->kind == ActionKind::Loop) {
                const auto &loop = d.loops[a->detail];
                result->loops.push_back(loop);
                const auto &condition = d.conditions[d.directory.find(loop.condition.value).index];
                sequence(condition.preparation, true);
                sequence(loop.initialize, true);
                sequence(loop.update, true);
                edge(FlowRole::LoopBody, sequence(loop.body, true), loop.body, condition.result, condition.id);
                edge(FlowRole::LoopExit, next(id), {}, condition.result, condition.id);
            } else {
                ValueId selector;
                ConditionId condition;
                if (a->kind == ActionKind::Switch) {
                    condition = d.switches[a->detail].selector;
                    auto c = d.conditions[d.directory.find(condition.value).index];
                    selector = c.result;
                    sequence(c.preparation, true);
                }
                for (std::uint32_t i = 0; i < a->armCount; ++i) {
                    auto arm = d.arms[a->firstArm + i];
                    ValueId predicate = selector;
                    auto check = condition;
                    if (arm.condition.valid()) {
                        auto c = d.conditions[d.directory.find(arm.condition.value).index];
                        predicate = c.result;
                        check = c.id;
                        sequence(c.preparation, true);
                    }
                    auto &relation = edge(a->kind == ActionKind::Switch ? FlowRole::Case
                                          : arm.condition.valid()       ? FlowRole::TrueBranch
                                                                        : FlowRole::FalseBranch,
                                          sequence(arm.body, true), arm.body, predicate, check, arm.id.value, i);
                    if (a->kind == ActionKind::Switch) {
                        auto labels = std::span(d.caseLabels).subspan(arm.firstLabel, arm.labelCount);
                        relation.labels.assign(labels.begin(), labels.end());
                        if (arm.includesDefault)
                            edge(FlowRole::Default, sequence(arm.body, false), arm.body, predicate, check, arm.id.value,
                                 i);
                    }
                    if (a->kind == ActionKind::Conditional && arm.condition.valid()) {
                        if (i + 1 < a->armCount) {
                            const auto &following = d.arms[a->firstArm + i + 1];
                            if (following.condition.valid()) {
                                const auto &c = d.conditions[d.directory.find(following.condition.value).index];
                                auto &edgeToCheck =
                                    edge(FlowRole::FalseBranch, {}, c.preparation, predicate, check, arm.id.value, i);
                                edgeToCheck.targetCondition = c.id;
                            } else
                                edge(FlowRole::FalseBranch, sequence(following.body, false), following.body, predicate,
                                     check, arm.id.value, i);
                        } else
                            edge(FlowRole::FalseBranch, next(id), {}, predicate, check, arm.id.value, i);
                    }
                }
                edge(FlowRole::Fallthrough, next(id));
            }
        }
    }
    result->actions.assign(visited.begin(), visited.end());
    result->sequences.assign(seqs.begin(), seqs.end());
    result->sections.assign(sections.begin(), sections.end());
    result->texts.assign(texts.begin(), texts.end());
    unique(result->variables);
    // Directed cycles are an explicit qualification, not an inferred storyline.
    std::map<SctActionId, std::vector<SctActionId>> adjacency;
    for (auto &e : result->flow)
        if (e.target.valid())
            adjacency[e.key.source].push_back(e.target);
    std::map<SctActionId, unsigned> color;
    for (auto root : result->actions) {
        if (color[root])
            continue;
        std::vector<std::pair<SctActionId, std::size_t>> stack{{root, 0}};
        color[root] = 1;
        while (!stack.empty()) {
            auto &[node, index] = stack.back();
            auto &out = adjacency[node];
            if (index == out.size()) {
                color[node] = 2;
                stack.pop_back();
                continue;
            }
            auto target = out[index++];
            if (color[target] == 1)
                result->cyclic = true;
            else if (!color[target]) {
                color[target] = 1;
                stack.push_back({target, 0});
            }
        }
    }
    stopped(stop);
    stopped(shutdown);
    if(!cutsceneScope)summaries[module] = result;
    return {status(), std::move(result), false};
}
