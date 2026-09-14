// Included by ProgramService.cpp after the private transaction record types.
// All helpers operate only while the serialized owner is preparing a delta.
struct SemanticTransaction {
    ProgramData &d;
    PreparedData &p;
    std::function<void(SctActionId)> collect;
    std::function<void(SctActionId)> created;
    std::function<void(std::uint64_t, Location)> locate;
    std::function<Action &(SctActionId)> editAction;
    std::function<std::vector<SctActionId>(SctSequenceId)> order;
    std::function<void(SctSequenceId, std::vector<SctActionId>)> putOrder;
    bool changed = false;
    std::vector<TargetUse> removedIncoming;
    std::set<SctActionId> checks;
    std::uint32_t at(std::uint64_t id, EntityKind kind) {
        auto loc = d.directory.find(id);
        require(loc.kind == kind, "Missing semantic edit target");
        return loc.index;
    }
    std::vector<SctSequenceId> children(SctActionId id) {
        auto *a = d.action(id);
        require(a, "Missing construct");
        std::vector<SctSequenceId> result;
        auto condition = [&](ConditionId id) {
            if (id.valid())
                result.push_back(d.conditions[at(id.value, EntityKind::Condition)].preparation);
        };
        for (std::uint32_t i = 0; i < a->armCount; ++i) {
            auto &arm = d.arms[a->firstArm + i];
            result.push_back(arm.body);
            condition(arm.condition);
        }
        if (a->kind == ActionKind::Loop) {
            auto &l = d.loops[a->detail];
            condition(l.condition);
            for (auto seq : {l.initialize, l.body, l.update})
                if (seq.valid())
                    result.push_back(seq);
        }
        if (a->kind == ActionKind::Switch)
            condition(d.switches[a->detail].selector);
        return result;
    }
    void captureTree(SctActionId root) {
        std::vector<SctActionId> pending{root};
        std::set<SctActionId> seen;
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!seen.insert(id).second)
                continue;
            collect(id);
            checks.insert(id);
            for (auto seq : children(id))
                if (auto *s = d.sequence(seq); s && s->order)
                    pending.insert(pending.end(), s->order->begin(), s->order->end());
        }
    }
    void capture(const SemanticOperation &op) {
        std::visit(
            [&](auto &edit) {
                using T = std::decay_t<decltype(edit)>;
                if constexpr (std::is_same_v<T, ConnectSlots>) {
                    if (auto id = std::get_if<SctPortId>(&edit.connection.source)) {
                        auto* port=d.lookup(id->value,EntityKind::Port,d.metadata.ports);
                        require(port, "Missing source slot"); if(port->action) collect(*port->action);
                    }
                    if(edit.connection.cutscene && edit.connection.cutscene->implementation) collect(*edit.connection.cutscene->implementation);
                }
                if constexpr (std::is_same_v<T, MoveAction> || std::is_same_v<T, EraseAction> ||
                              std::is_same_v<T, RetargetTransfer>)
                    captureTree(edit.action);
                if constexpr (std::is_same_v<T, ReplaceArms>) {
                    collect(edit.action);
                    auto *a = d.action(edit.action);
                    require(a, "Missing arm owner");
                    for (std::uint32_t i = 0; i < a->armCount; ++i) {
                        auto old = d.arms[a->firstArm + i];
                        auto found = std::ranges::find_if(edit.arms, [&](auto &arm) { return arm.id == old.id; });
                        auto captureSequence = [&](SctSequenceId seq) {
                            for (auto id : *d.sequence(seq)->order)
                                captureTree(id);
                        };
                        if (found == edit.arms.end()) {
                            captureSequence(old.body);
                            if (old.condition.valid())
                                captureSequence(
                                    d.conditions[at(old.condition.value, EntityKind::Condition)].preparation);
                        } else if (old.condition.valid() && !found->condition)
                            captureSequence(d.conditions[at(old.condition.value, EntityKind::Condition)].preparation);
                    }
                }
                if constexpr (std::is_same_v<T, PromoteFlow>) {
                    auto *a = d.action(edit.header);
                    require(a, "Missing flow header");
                    auto *s = d.sequence(a->owner);
                    auto begin = std::ranges::find(*s->order, edit.header),
                         end = std::ranges::find(*s->order, edit.continuation);
                    require(begin != s->order->end() && end != s->order->end() && begin < end,
                            "Invalid promotion bounds");
                    for (auto it = begin; it != end; ++it)
                        captureTree(*it);
                }
            },
            op);
    }
    SctSequenceId sequence(SctScriptId script, SequenceOwner owner, SequenceRole role) {
        auto index = count32(d.sequences.size());
        d.sequences.push_back({});
        p.sequences.push_back({index, {}});
        SctSequenceId id{d.allocate()};
        locate(id.value, {EntityKind::Sequence, index});
        d.sequences[index] = {id, script, owner, std::make_shared<const std::vector<SctActionId>>(), role};
        p.receipt.sequences.push_back(id);
        return id;
    }
    ConditionId condition(SctActionId owner, SctScriptId script, ValueId value, std::optional<ArmId> arm = {}) {
        require(d.values.find(value.value) && d.values.find(value.value)->kind == ValueKind::Expression,
                "Construct needs a typed condition");
        auto index = count32(d.conditions.size());
        d.conditions.push_back({});
        p.conditions.push_back({index, {}});
        ConditionId id{d.allocate()};
        locate(id.value, {EntityKind::Condition, index});
        auto prep = sequence(script, id, SequenceRole::ConditionPreparation);
        d.conditions[index] = {id, owner, arm, prep, value};
        p.receipt.conditions.push_back(id);
        return id;
    }
    SctActionId create(SctSequenceId owner, std::size_t position, ActionKind kind) {
        auto ids = order(owner);
        require(position <= ids.size(), "Construct insertion position is outside sequence");
        auto index = d.actions.append({});
        d.successors.resize(d.actions.size());
        p.actions.push_back({index, {}});
        SctActionId id{d.allocate()};
        locate(id.value, {EntityKind::Action, index});
        *d.actions.find(index) = {id, owner, kind};
        ids.insert(ids.begin() + position, id);
        putOrder(owner, std::move(ids));
        created(id);
        checks.insert(id);
        return id;
    }
    void eraseSequence(SctSequenceId id) {
        auto index = at(id.value, EntityKind::Sequence);
        auto ids = *d.sequences[index].order;
        for (auto action : ids)
            eraseTree(action);
        touch(p.sequences, index, d.sequences[index]) = {};
        locate(id.value, {});
        p.receipt.sequences.push_back(id);
    }
    void eraseCondition(ConditionId id) {
        if (!id.valid())
            return;
        auto index = at(id.value, EntityKind::Condition);
        eraseSequence(d.conditions[index].preparation);
        touch(p.conditions, index, d.conditions[index]) = {};
        locate(id.value, {});
        p.receipt.conditions.push_back(id);
    }
    void eraseArm(Arm arm) {
        eraseSequence(arm.body);
        eraseCondition(arm.condition);
        locate(arm.id.value, {});
        p.receipt.arms.push_back(arm.id);
    }
    void eraseTree(SctActionId id) {
        auto a = *d.action(id);
        auto refs = incoming(d, id);
        removedIncoming.insert(removedIncoming.end(), refs.begin(), refs.end());
        // Copy descriptors before growing any vectors during recursive changes.
        std::vector<Arm> arms;
        for (std::uint32_t i = 0; i < a.armCount; ++i)
            arms.push_back(d.arms[a.firstArm + i]);
        for (auto arm : arms)
            eraseArm(arm);
        if (a.kind == ActionKind::Loop) {
            auto l = d.loops[a.detail];
            eraseCondition(l.condition);
            for (auto seq : {l.initialize, l.body, l.update})
                if (seq.valid())
                    eraseSequence(seq);
            touch(p.loops, a.detail, d.loops[a.detail]) = {};
        }
        if (a.kind == ActionKind::Switch) {
            eraseCondition(d.switches[a.detail].selector);
            touch(p.switches, a.detail, d.switches[a.detail]) = {};
        }
        if (a.kind == ActionKind::Transfer)
            touch(p.transfers, a.detail, d.transfers[a.detail]) = {};
        editAction(id) = {};
        locate(id.value, {});
        p.receipt.actions.push_back(id);
    }
    void arms(SctActionId id, const std::vector<ArmEdit> &edits) {
        auto &a = editAction(id);
        require(a.kind == ActionKind::Conditional || a.kind == ActionKind::Switch, "Arms need a conditional or switch");
        const auto script = d.sequence(a.owner)->script;
        require(!edits.empty(), "A construct needs at least one arm");
        std::map<ArmId, Arm> existing;
        for (std::uint32_t i = 0; i < a.armCount; ++i)
            existing.emplace(d.arms[a.firstArm + i].id, d.arms[a.firstArm + i]);
        std::set<ArmId> kept;
        std::set<std::int32_t> labels;
        bool hasDefault = false;
        std::vector<Arm> next;
        for (std::size_t i = 0; i < edits.size(); ++i) {
            auto &e = edits[i];
            Arm row;
            if (e.id.valid()) {
                require(existing.contains(e.id) && kept.insert(e.id).second, "Foreign or duplicate arm identity");
                row = existing.at(e.id);
            } else {
                row.id = ArmId{d.allocate()};
                row.owner = id;
                row.body = sequence(script, row.id, SequenceRole::ArmBody);
            }
            if (a.kind == ActionKind::Conditional) {
                require(e.condition || (i > 0 && i + 1 == edits.size()), "Only the final conditional arm may be Else");
                require(e.labels.empty() && !e.includesDefault, "Conditional arm has case labels");
                if (e.condition) {
                    auto value = importValue(d, *e.condition);
                    require(d.values.find(value.value)->kind == ValueKind::Expression, "Arm needs an expression");
                    if (row.condition.valid()) {
                        auto index = at(row.condition.value, EntityKind::Condition);
                        touch(p.conditions, index, d.conditions[index]).result = value;
                    } else
                        row.condition = condition(id, script, value, row.id);
                } else {
                    eraseCondition(row.condition);
                    row.condition = {};
                }
            } else {
                require(!e.condition, "Switch arm conditions belong to the selector");
                require(!e.labels.empty() || e.includesDefault, "Case needs labels or Default");
                for (auto label : e.labels)
                    require(labels.insert(label).second, "Duplicate switch label");
                require(!e.includesDefault || !hasDefault, "Duplicate Default arm");
                hasDefault |= e.includesDefault;
                require(e.exit == CaseExit::ExitSwitch || (e.exit == CaseExit::Fallthrough && i + 1 < edits.size()),
                        "Invalid final case fallthrough");
                row.firstLabel = count32(d.caseLabels.size());
                row.labelCount = count32(e.labels.size());
                d.caseLabels.insert(d.caseLabels.end(), e.labels.begin(), e.labels.end());
                row.includesDefault = e.includesDefault;
                row.caseExit = e.exit;
            }
            next.push_back(row);
            p.receipt.arms.push_back(row.id);
        }
        for (auto &[old, row] : existing)
            if (!kept.contains(old))
                eraseArm(row);
        a.firstArm = count32(d.arms.size());
        a.armCount = count32(next.size());
        for (auto row : next) {
            auto index = count32(d.arms.size());
            d.arms.push_back(row);
            locate(row.id.value, {EntityKind::Arm, index});
        }
    }
    void insert(const InsertConstruct &e) {
        require(e.kind == ActionKind::Conditional || e.kind == ActionKind::Loop || e.kind == ActionKind::Switch,
                "Invalid structured construct kind");
        auto id = create(e.sequence, e.position, e.kind);
        auto &a = *d.actions.find(at(id.value, EntityKind::Action));
        auto script = d.sequence(e.sequence)->script;
        if (e.kind == ActionKind::Loop) {
            require(e.loopKind == LoopKind::While || e.loopKind == LoopKind::DoWhile || e.loopKind == LoopKind::For,
                    "Invalid loop kind");
            auto value = importValue(d, e.condition);
            Loop l{id, e.loopKind, condition(id, script, value)};
            if (e.loopKind == LoopKind::For)
                l.initialize = sequence(script, id, SequenceRole::LoopInitialize);
            l.body = sequence(script, id, SequenceRole::LoopBody);
            if (e.loopKind == LoopKind::For)
                l.update = sequence(script, id, SequenceRole::LoopUpdate);
            a.detail = count32(d.loops.size());
            d.loops.push_back({});
            p.loops.push_back({a.detail, {}});
            d.loops[a.detail] = l;
        } else {
            if (e.kind == ActionKind::Switch) {
                auto value = importValue(d, e.condition);
                auto c = condition(id, script, value);
                a.detail = count32(d.switches.size());
                d.switches.push_back({});
                p.switches.push_back({a.detail, {}});
                d.switches[a.detail] = {id, c};
            }
            arms(id, e.arms);
        }
    }
    void move(SctActionId id, SctSequenceId destination, std::size_t position) {
        auto &a = editAction(id);
        auto *dest = d.sequence(destination);
        require(dest && d.sequence(a.owner)->script == dest->script, "Actions stay in their parent script");
        auto *ancestor = dest;
        while (ancestor->owner) {
            auto parent = d.parentAction(*ancestor);
            require(parent != id, "Cannot move a construct inside itself");
            ancestor = d.sequence(d.action(parent)->owner);
        }
        auto source = order(a.owner);
        std::erase(source, id);
        putOrder(a.owner, std::move(source));
        auto to = order(destination);
        require(position <= to.size(), "Move is outside body sequence");
        to.insert(to.begin() + position, id);
        a.owner = destination;
        putOrder(destination, std::move(to));
    }
    template <class T> auto &table() {
        if constexpr (std::is_same_v<T, Module>)
            return d.metadata.modules;
        else if constexpr (std::is_same_v<T, Entrypoint>)
            return d.metadata.entrypoints;
        else if constexpr (std::is_same_v<T, Port>)
            return d.metadata.ports;
        else if constexpr (std::is_same_v<T, SctLanguageGroup>)
            return d.metadata.languages;
        else if constexpr (std::is_same_v<T, Connection>)
            return d.metadata.connections;
        else if constexpr (std::is_same_v<T, SctLogicalText>)
            return d.metadata.logicalTexts;
        else
            return d.metadata.meanings;
    }
    template <class T> void preserveTable() {
        auto save = [&](auto &other) {
            if (!other)
                other = table<T>();
        };
        if constexpr (std::is_same_v<T, Module>)
            save(p.modules);
        else if constexpr (std::is_same_v<T, Entrypoint>)
            save(p.entries);
        else if constexpr (std::is_same_v<T, Port>)
            save(p.ports);
        else if constexpr (std::is_same_v<T, SctLanguageGroup>)
            save(p.languages);
        else if constexpr (std::is_same_v<T, Connection>)
            save(p.connections);
        else if constexpr (std::is_same_v<T, SctLogicalText>)
            save(p.logicalTexts);
        else
            save(p.meanings);
    }
    template <class T> static EntityKind kind() {
        if constexpr (std::is_same_v<T, Module>)
            return EntityKind::Module;
        else if constexpr (std::is_same_v<T, Entrypoint>)
            return EntityKind::Entry;
        else if constexpr (std::is_same_v<T, Port>)
            return EntityKind::Port;
        else if constexpr (std::is_same_v<T, SctLanguageGroup>)
            return EntityKind::Language;
        else if constexpr (std::is_same_v<T, Connection>)
            return EntityKind::Connection;
        else if constexpr (std::is_same_v<T, SctLogicalText>)
            return EntityKind::LogicalText;
        else
            return EntityKind::Meaning;
    }
    template <class T> void put(T row) {
        preserveTable<T>();
        auto &rows = table<T>();
        auto index = row.id.valid() ? at(row.id.value, kind<T>()) : count32(rows.size());
        if (index < rows.size()) {
            if constexpr (std::is_same_v<T, Entrypoint>)
                if (rows[index].required())
                    require(row.required() && row.name == rows[index].name && row.enabled &&
                                row.script == rows[index].script && row.body == rows[index].body,
                            "Required lifecycle identity cannot change");
            if constexpr (std::is_same_v<T, Module> || std::is_same_v<T, Entrypoint> ||
                          std::is_same_v<T, SctLogicalText>)
                require(row.script == rows[index].script, "Metadata cannot change its parent script");
        } else
            row.id = decltype(row.id){d.allocate()};
        locate(row.id.value, {kind<T>(), index});
        p.receipt.metadata.push_back(row.id.value);
        if constexpr (std::is_same_v<T, Module>)
            if (index == rows.size() || rows[index].sequences != row.sequences)
                p.receipt.modules.push_back(row.id);
        if (index == rows.size())
            rows.push_back(std::move(row));
        else
            rows[index] = std::move(row);
        p.receipt.presentationChanged = true;
    }
    template <class T, class Id> void eraseMetadata(Id id) {
        preserveTable<T>();
        auto &rows = table<T>();
        auto index = at(id.value, kind<T>());
        if constexpr (std::is_same_v<T, Entrypoint>)
            require(!rows[index].required(), "Required lifecycle identity cannot be removed");
        rows.erase(rows.begin() + index);
        locate(id.value, {});
        for (std::uint32_t i = index; i < rows.size(); ++i)
            locate(rows[i].id.value, {kind<T>(), i});
        p.receipt.metadata.push_back(id.value);
        if constexpr (std::is_same_v<T, Module>)
            p.receipt.modules.push_back(id);
        p.receipt.presentationChanged = true;
    }
    void promote(const PromoteFlow &);
    void connect(const ConnectSlots &);
    void parameter(SctActionId, spice::sct::SctParameterAddress, ValueId);
    void apply(const SemanticOperation &op) {
        changed = true;
        std::visit(
            [&](const auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ConnectSlots>)
                    connect(e);
                else if constexpr (std::is_same_v<T, InsertConstruct>)
                    insert(e);
                else if constexpr (std::is_same_v<T, ReplaceArms>)
                    arms(e.action, e.arms);
                else if constexpr (std::is_same_v<T, MoveAction>)
                    move(e.action, e.sequence, e.position);
                else if constexpr (std::is_same_v<T, EraseAction>) {
                    auto owner = d.action(e.action)->owner;
                    auto ids = order(owner);
                    std::erase(ids, e.action);
                    putOrder(owner, std::move(ids));
                    eraseTree(e.action);
                } else if constexpr (std::is_same_v<T, InsertTransfer>) {
                    auto id = create(e.sequence, e.position, ActionKind::Transfer);
                    auto &a = *d.actions.find(at(id.value, EntityKind::Action));
                    a.detail = count32(d.transfers.size());
                    d.transfers.push_back({});
                    p.transfers.push_back({a.detail, {}});
                    d.transfers[a.detail] = {id, e.target, e.kind};
                } else if constexpr (std::is_same_v<T, RetargetTransfer>) {
                    auto &a = editAction(e.action);
                    require(a.kind == ActionKind::Transfer, "Not a scoped transfer");
                    touch(p.transfers, a.detail, d.transfers[a.detail]).target = e.target;
                } else if constexpr (std::is_same_v<T, PromoteFlow>)
                    promote(e);
                else if constexpr (std::is_same_v<T, PutMetadata>)
                    std::visit([&](auto row) { put(std::move(row)); }, e.entity);
                else if constexpr (std::is_same_v<T, EraseMetadata>)
                    std::visit(
                        [&](auto id) {
                            using I = decltype(id);
                            if constexpr (std::is_same_v<I, SctModuleId>)
                                eraseMetadata<Module>(id);
                            else if constexpr (std::is_same_v<I, SctEntrypointId>)
                                eraseMetadata<Entrypoint>(id);
                            else if constexpr (std::is_same_v<I, SctPortId>)
                                eraseMetadata<Port>(id);
                            else if constexpr (std::is_same_v<I, SctLanguageGroupId>)
                                eraseMetadata<SctLanguageGroup>(id);
                            else if constexpr (std::is_same_v<I, SctConnectionId>)
                                eraseMetadata<Connection>(id);
                            else if constexpr (std::is_same_v<I, SctLogicalTextId>)
                                eraseMetadata<SctLogicalText>(id);
                            else
                                eraseMetadata<VariableMeaning>(id);
                        },
                        e.id);
                else if constexpr (std::is_same_v<T, PutAnnotation>) {
                    require(p.annotations.empty(), "Use annotation upserts consistently within a batch");
                    if (!p.annotationTable)
                        p.annotationTable = d.metadata.annotations;
                    auto &rows = d.metadata.annotations;
                    auto found =
                        std::ranges::find_if(rows, [&](auto &row) { return row.subject == e.annotation.subject; });
                    if (found == rows.end())
                        rows.push_back(e.annotation);
                    else
                        *found = e.annotation;
                    p.receipt.presentationChanged = true;
                    std::visit([&](auto id) { p.receipt.metadata.push_back(id.value); }, e.annotation.subject);
                } else if constexpr (std::is_same_v<T, BindLifecycle>) {
                    require(e.name == "init" || e.name == "loop", "Unknown lifecycle root");
                    auto index = at(e.section.value, EntityKind::Section);
                    auto &section = d.sections[index];
                    require(section.body.valid(), "Lifecycle repair requires an explicitly selected code body");
                    for (auto &entry : d.metadata.entrypoints)
                        require(!(entry.script == section.script && entry.required() && entry.name == e.name),
                                "Lifecycle root already exists");
                    require(section.name != "init" && section.name != "loop" || section.name == e.name,
                            "Cannot rename another lifecycle root");
                    for (auto id : d.scripts[at(section.script.value, EntityKind::Script)].sections)
                        if (id != section.id)
                            require(d.sections[at(id.value, EntityKind::Section)].name != e.name,
                                    "Lifecycle name is already assigned");
                    touch(p.sections, index, section).name = e.name;
                    Entrypoint entry;
                    entry.script = section.script;
                    entry.name = e.name;
                    entry.kind = SctEntrypointKind::Lifecycle;
                    entry.body = section.body;
                    entry.activation =
                        SctUnresolvedBinding{"Engine lifecycle", "Engine enters the selected body directly", {}};
                    put(std::move(entry));
                } else if constexpr (std::is_same_v<T, ScriptSettings>) {
                    auto index = at(e.script.value, EntityKind::Script);
                    auto &script = touch(p.scripts, index, d.scripts[index]);
                    script.context.name = e.name;
                    script.context.outputProfile = e.output;
                    script.context.resources = e.resources;
                    p.receipt.presentationChanged = true;
                    p.receipt.metadata.push_back(e.script.value);
                } else if constexpr (std::is_same_v<T, AssignLanguages>) {
                    auto index = at(e.text.value, EntityKind::Text);
                    auto languages = e.languages;
                    unique(languages);
                    for (auto id : languages)
                        at(id.value, EntityKind::Language);
                    auto &row = touch(p.texts, index, d.texts[index]);
                    row.languages = std::move(languages);
                    row.explicitAssignment = true;
                    p.receipt.presentationChanged = true;
                }
            },
            op);
    }
    void finish() {
        for (auto use : removedIncoming)
            if (d.action(use.site.action))
                require(std::ranges::any_of(p.actions,
                                            [&](auto &row) {
                                                return row.other.id == use.site.action ||
                                                       d.actions.find(row.index)->id == use.site.action;
                                            }),
                        "Deleted construct still has an incoming reference");
        for (auto id : checks) {
            auto *a = d.action(id);
            if (!a || a->kind != ActionKind::Transfer)
                continue;
            auto t = d.transfers[a->detail];
            auto *dest = d.action(t.target);
            require(dest && (dest->kind == ActionKind::Loop ||
                             (t.kind == TransferKind::Break && dest->kind == ActionKind::Switch)),
                    "Invalid Break/Continue target");
            require(t.kind == TransferKind::Break || t.kind == TransferKind::Continue, "Unknown transfer kind");
            auto *seq = d.sequence(a->owner);
            bool found = false;
            while (seq->owner) {
                auto parent = d.parentAction(*seq);
                if (parent == t.target) {
                    require(t.kind != TransferKind::Continue || seq->role == SequenceRole::LoopBody,
                            "Continue must be in the target loop body");
                    found = true;
                    break;
                }
                seq = d.sequence(d.action(parent)->owner);
            }
            require(found, "Transfer target does not enclose source");
        }
    }
};
void SemanticTransaction::promote(const PromoteFlow &e) {
    auto original = *d.action(e.header);
    require(original.kind == ActionKind::Instruction, "Flow header is already structured");
    auto node = *d.values.find(original.value.value);
    require(node.data == 0 && node.detail == 0, "Promotion requires an unscheduled, normal-refresh If");
    auto source = *d.sequence(original.owner)->order;
    auto start = std::ranges::find(source, e.header), end = std::ranges::find(source, e.continuation);
    require(start != source.end() && end != source.end() && start < end,
            "Promotion needs a forward continuation in the same sequence");
    auto parameter = [&](const Action &a, std::uint32_t index) -> ValueId {
        for (auto child : d.child(a.value)) {
            auto *v = d.values.find(child.value);
            if (v->kind == ValueKind::Parameter && v->data == index)
                return d.child(child).front();
        }
        throw std::runtime_error("Flow operand is missing");
    };
    auto conditionValue = parameter(original, 0), falseValue = parameter(original, 1);
    require(d.values.find(conditionValue.value)->kind == ValueKind::Expression &&
                d.values.find(falseValue.value)->kind == ValueKind::ActionReference,
            "Promotion needs a typed check and resolved false edge");
    SctActionId falseId{d.values.find(falseValue.value)->data};
    auto alternate = std::ranges::find(source, falseId);
    require(alternate > start && alternate <= end, "False edge is outside promotion range");
    auto closing = alternate - 1;
    require(closing > start, "Promotion needs an explicit body exit");
    auto exit = *d.action(*closing);
    require(exit.kind == ActionKind::Instruction, "Body exit is not an explicit jump");
    auto exitNode = *d.values.find(exit.value.value);
    require(exitNode.data == 10 && exitNode.detail == 0, "Promotion needs an unscheduled plain jump exit");
    auto jump = parameter(exit, 0);
    require(d.values.find(jump.value)->kind == ValueKind::ActionReference, "Unresolved body exit");
    SctActionId exitTarget{d.values.find(jump.value)->data};
    const bool loop = exitTarget == e.header && alternate == end;
    require(loop || exitTarget == e.continuation, "Irregular body exit remains explicit");
    // Only the header and consumed exit may cross the new body boundary. Calls
    // or jumps into its interior would make structural promotion misleading.
    for (auto i = start + 1; i < end; ++i) {
        auto *a = d.action(*i);
        require(a->kind == ActionKind::Instruction, "Nested explicit promotion is not recognized here");
        auto v = *d.values.find(a->value.value);
        require(v.detail == 0, "Scheduled/refresh-modified regions remain explicit");
        for (auto ref : incoming(d, *i))
            require(ref.site.action == e.header || ref.site.action == exit.id,
                    "A promoted body has other incoming edges");
        if (*i != exit.id)
            require(v.data != 0 && v.data != 3 && v.data != 10, "Irregular internal control flow remains explicit");
    }
    for (auto ref : incoming(d, exit.id))
        require(false, "Jump exit has incoming callers");
    // The header retains authored identity and source provenance. Consumed glue
    // exists in history, while executable structured content owns the bodies.
    auto script = d.sequence(original.owner)->script;
    auto &root = editAction(e.header);
    root.kind = loop ? ActionKind::Loop : ActionKind::Conditional;
    root.value = {};
    root.armCount = 0;
    auto appendBody = [&](SctSequenceId seq, auto first, auto last) {
        std::vector<SctActionId> ids(first, last);
        for (auto id : ids)
            editAction(id).owner = seq;
        putOrder(seq, std::move(ids));
    };
    if (loop) {
        Loop l{e.header, LoopKind::While, condition(e.header, script, conditionValue)};
        l.body = sequence(script, e.header, SequenceRole::LoopBody);
        appendBody(l.body, start + 1, closing);
        root.detail = count32(d.loops.size());
        d.loops.push_back({});
        p.loops.push_back({root.detail, {}});
        d.loops[root.detail] = l;
    } else {
        root.firstArm = count32(d.arms.size());
        std::vector<Arm> rows;
        Arm first;
        first.id = ArmId{d.allocate()};
        first.owner = e.header;
        first.body = sequence(script, first.id, SequenceRole::ArmBody);
        first.condition = condition(e.header, script, conditionValue, first.id);
        appendBody(first.body, start + 1, closing);
        rows.push_back(first);
        if (alternate != end) {
            Arm last;
            last.id = ArmId{d.allocate()};
            last.owner = e.header;
            last.body = sequence(script, last.id, SequenceRole::ArmBody);
            appendBody(last.body, alternate, end);
            rows.push_back(last);
        }
        root.armCount = count32(rows.size());
        for (auto row : rows) {
            locate(row.id.value, {EntityKind::Arm, count32(d.arms.size())});
            d.arms.push_back(row);
            p.receipt.arms.push_back(row.id);
        }
    }
    editAction(exit.id) = {};
    locate(exit.id.value, {});
    std::vector<SctActionId> remaining(source.begin(), start + 1);
    remaining.insert(remaining.end(), end, source.end());
    order(original.owner);
    putOrder(original.owner, std::move(remaining));
    PutAnnotation note{
        {e.header,
         "",
         {{SctEvidenceKind::Inference, loop ? "Recognized plain If/back-jump loop with a forward false continuation"
                                            : "Recognized plain If/jump diamond with a shared continuation"}}}};
    apply(SemanticOperation{std::move(note)});
}
