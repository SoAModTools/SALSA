// Shared selected-content operations; no full-program snapshot or second group tree.
std::tuple<SctScriptId, SctSequenceId, std::size_t> fragmentDestination(ProgramData &d,
                                                                        editor::Subject anchor) {
    SctSequenceId seq;
    std::size_t pos = 0;
    SctScriptId script;
    if (auto id = std::get_if<SctScriptId>(&anchor)) {
        require(d.lookup(id->value, EntityKind::Script, d.scripts), "Missing script");
        return {*id, {}, 0};
    }
    if (auto id = std::get_if<SectionId>(&anchor)) {
        auto *s = d.lookup(id->value, EntityKind::Section, d.sections);
        require(s, "Missing section");
        script = s->script;
        seq = s->body;
    }
    if (auto id = std::get_if<SctSequenceId>(&anchor))
        seq = *id;
    if (auto id = std::get_if<ArmId>(&anchor)) {
        auto *a = d.lookup(id->value, EntityKind::Arm, d.arms);
        require(a, "Missing arm");
        seq = a->body;
    }
    if (auto id = std::get_if<ConditionId>(&anchor)) {
        auto *c = d.lookup(id->value, EntityKind::Condition, d.conditions);
        require(c, "Missing condition");
        seq = c->preparation;
    }
    if (auto id = std::get_if<SctActionId>(&anchor)) {
        auto *a = d.action(*id);
        require(a, "Missing insertion action");
        seq = a->owner;
        auto &order = *d.sequence(seq)->order;
        pos = std::ranges::find(order, *id) - order.begin() + 1;
    }
    auto *s = d.sequence(seq);
    require(s, "Select an executable body or insertion action");
    return {s->script, seq, pos};
}
void SemanticTransaction::organize(const Organize &op) {
    auto &e = op.value;
    auto index = at(e.script.value, EntityKind::Script);
    auto &script = d.scripts[index];
    auto old = script.sections, next = e.order;
    std::sort(old.begin(), old.end());
    std::sort(next.begin(), next.end());
    require(old == next, "Section order must contain every section exactly once");
    if (script.sections != e.order) {
        touch(p.scripts, index, script).sections = e.order;
        p.receipt.scriptLayouts.push_back(e.script);
    }
    if (!p.folders)
        p.folders = d.metadata.folders;
    std::vector<editor::SectionFolder> rows;
    std::map<std::uint32_t, std::uint32_t> kept;
    for (std::uint32_t i = 0; i < d.metadata.folders.size(); ++i)
        if (d.metadata.folders[i].script != e.script) {
            auto row = d.metadata.folders[i];
            kept[i] = count32(rows.size());
            if (row.parent)
                row.parent = kept.at(*row.parent);
            rows.push_back(std::move(row));
        }
    auto offset = count32(rows.size());
    for (std::size_t i = 0; i < e.folders.size(); ++i) {
        auto row = e.folders[i];
        require(row.script == e.script, "Folder belongs to another script");
        if (row.parent) {
            require(*row.parent < i, "Folder parent must precede child");
            *row.parent += offset;
        }
        rows.push_back(std::move(row));
    }
    d.metadata.folders = std::move(rows);
    p.receipt.presentationChanged = true;
}
void SemanticTransaction::customize(const CustomizeOpcode &op) {
    const auto *schema = spice::sct::findSctOpcodeSchema(op.value.opcode);
    require(schema, "Unknown opcode");
    std::set<std::uint32_t> names, defaults;
    for (auto &[index, label] : op.value.parameters)
        require(index < schema->parameterCatalogCount && names.insert(index).second,
                "Unknown or repeated parameter label");
    spice::sct::SctInstructionFactoryRequest request{op.value.opcode};
    for (auto [index, value] : op.value.defaults) {
        require(index < schema->parameterCatalogCount && defaults.insert(index).second,
                "Unknown or repeated parameter default");
        request.parameterOverrides.push_back({{index, {}}, spice::sct::SctEncodedWordValue{value}});
    }
    if (!op.value.defaults.empty()) {
        auto built = spice::sct::SctInstructionFactory::createDraft(request);
        require(built.draft.has_value(), "Invalid parameter defaults for this opcode");
    }
    if (!p.opcodes)
        p.opcodes = d.metadata.opcodes;
    auto &rows = d.metadata.opcodes;
    auto found = std::ranges::find(rows, op.value.opcode, &editor::OpcodeCustomization::opcode);
    if (found == rows.end())
        rows.push_back(op.value);
    else
        *found = op.value;
    std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.opcode < b.opcode; });
    p.receipt.presentationChanged = true;
}
void SemanticTransaction::paste(const PasteFragment &e) {
    require(bool(e.fragment), "Missing clipboard content");
    const auto &f = *e.fragment;
    const auto &s = f.content;
    auto [script, destination, position] = fragmentDestination(d, e.anchor);
    require(!s.actions.empty() || !s.sections.empty(), "Empty fragment");
    std::map<std::uint64_t, std::uint64_t> ids;
    auto claim = [&](std::uint64_t old, EntityKind kind, std::uint32_t index) {
        require(old && !ids.contains(old), "Duplicate fragment identity");
        auto id = d.allocate();
        ids.emplace(old, id);
        locate(id, {kind, index});
        return id;
    };
    auto remap = [&]<class Id>(Id old) -> Id { return old.valid() ? Id{ids.at(old.value)} : Id{}; };
    auto scriptAt = at(script.value, EntityKind::Script);
    auto uniqueName = [&](std::string name) {
        auto base = name;
        std::uint32_t suffix = 1;
        auto exists = [&] {
            for (auto id : d.scripts[scriptAt].sections)
                if (d.sections[at(id.value, EntityKind::Section)].name == name)
                    return true;
            return false;
        };
        while (exists())
            name = base + "_copy" + std::to_string(suffix++);
        return name;
    };
    std::map<std::uint64_t, std::uint32_t> actionRows, seqRows, armRows, conditionRows;
    for (auto &a : s.actions) {
        auto index = d.actions.append({});
        d.successors.resize(d.actions.size());
        p.actions.push_back({index, {}});
        claim(a.id.value, EntityKind::Action, index);
        actionRows[a.id.value] = index;
    }
    if (s.sections.empty()) {
        require(destination.valid() && !f.roots.empty(), "Missing insertion body");
        auto found = std::ranges::find(s.actions, f.roots.front(), &Action::id);
        require(found != s.actions.end(), "Missing fragment root");
        ids.emplace(found->owner.value, destination.value);
    }
    for (auto &seq : s.sequences)
        if (!ids.contains(seq.id.value)) {
            auto index = count32(d.sequences.size());
            d.sequences.push_back({});
            p.sequences.push_back({index, {}});
            claim(seq.id.value, EntityKind::Sequence, index);
            seqRows[seq.id.value] = index;
        }
    for (auto &a : s.arms) {
        auto index = count32(d.arms.size());
        d.arms.push_back({});
        p.arms.push_back({index, {}});
        claim(a.id.value, EntityKind::Arm, index);
        armRows[a.id.value] = index;
    }
    for (auto &c : s.conditions) {
        auto index = count32(d.conditions.size());
        d.conditions.push_back({});
        p.conditions.push_back({index, {}});
        claim(c.id.value, EntityKind::Condition, index);
        conditionRows[c.id.value] = index;
    }
    std::map<editor::Subject, editor::Subject> bindings;
    for (auto &b : e.bindings) {
        require(b.target && b.source.index() == b.target->index(),
                "Choose a target for every external binding");
        require(bindings.emplace(b.source, *b.target).second, "Duplicate clipboard binding");
    }
    auto external = [&]<class Id>(Id original) -> Id {
        editor::Subject source = original;
        auto found = bindings.find(source);
        Id target;
        if (found != bindings.end())
            target = std::get<Id>(found->second);
        else {
            require(f.project == d.id && f.script == script,
                    "Cross-script references require explicit binding");
            target = original;
        }
        if constexpr (std::is_same_v<Id, SctActionId>) {
            auto *a = d.action(target);
            require(a && d.sequence(a->owner)->script == script,
                    "External action is unavailable in destination script");
        } else {
            auto *t = d.lookup(target.value, EntityKind::Text, d.texts);
            require(t && t->script == script, "External text is unavailable in destination script");
        }
        return target;
    };
    auto target = [&](SctActionId id) { return ids.contains(id.value) ? remap(id) : external(id); };
    // Footer values travel with copied instructions, including across projects.
    // Indexed strings remain explicit external bindings.
    std::map<SctTextResourceId, SctTextResourceId> ownedTexts;
    std::vector<std::pair<std::uint32_t, ValueId>> copiedTextValues;
    for (const auto &source : s.texts) if (source.supplementary) {
        auto copy = source;
        copy.id = SctTextResourceId{d.allocate()};
        copy.script = script;
        if (f.project != d.id || f.script != script) copy.sourceText = 0;
        if (f.project != d.id) { copy.languages.clear(); copy.explicitAssignment = false; }
        auto index = count32(d.texts.size());
        d.texts.push_back(copy); p.texts.push_back({index, {}});
        locate(copy.id.value, {EntityKind::Text, index});
        auto si = at(script.value, EntityKind::Script);
        touch(p.scripts, si, d.scripts[si]).supplementaryText.push_back(copy.id);
        ownedTexts.emplace(source.id, copy.id);
        copiedTextValues.emplace_back(index, source.value);
    }
    auto payload = s.payload;
    for (auto &v : payload.values) {
        if (v.kind == ValueKind::ActionReference)
            v.data = target(SctActionId{v.data}).value;
        if (v.kind == ValueKind::SupplementaryTextReference) {
            require(ownedTexts.contains(SctTextResourceId{v.data}), "Missing copied string operand");
            v.data = ownedTexts.at(SctTextResourceId{v.data}).value;
        } else if (v.kind == ValueKind::IndexedTextReference) {
            auto id = external(SctTextResourceId{v.data});
            auto *t = d.lookup(id.value, EntityKind::Text, d.texts);
            require(t->supplementary == (v.kind == ValueKind::SupplementaryTextReference),
                    "Text binding storage differs");
            v.data = id.value;
        }
    }
    auto base = d.values.size();
    if (!payload.values.empty()) {
        payload.root = ValueId{count32(payload.values.size() - 1)};
        importValue(d, payload);
    }
    auto value = [&](ValueId id) {
        require(!id.valid() || id.value < payload.values.size(), "Invalid fragment value");
        return id.valid() ? ValueId{base + id.value} : id;
    };
    for (auto [index, source] : copiedTextValues) d.texts[index].value = value(source);
    for (auto &seq : s.sequences)
        if (auto it = seqRows.find(seq.id.value); it != seqRows.end()) {
            auto row = seq;
            row.id = remap(seq.id);
            row.script = script;
            if (row.owner)
                row.owner = std::visit([&](auto id) -> SequenceOwner { return remap(id); }, *row.owner);
            std::vector<SctActionId> order;
            require(bool(seq.order), "Missing fragment order");
            for (auto id : *seq.order)
                order.push_back(remap(id));
            row.order = std::make_shared<const std::vector<SctActionId>>(std::move(order));
            d.sequences[it->second] = std::move(row);
            p.receipt.sequences.push_back(remap(seq.id));
        }
    for (auto &c : s.conditions) {
        auto row = c;
        row.id = remap(c.id);
        row.owner = remap(c.owner);
        if (row.arm)
            row.arm = remap(*row.arm);
        row.preparation = remap(c.preparation);
        row.result = value(c.result);
        d.conditions[conditionRows.at(c.id.value)] = row;
        p.receipt.conditions.push_back(row.id);
    }
    for (auto &arm : s.arms) {
        auto row = arm;
        row.id = remap(arm.id);
        row.owner = remap(arm.owner);
        row.condition = remap(arm.condition);
        row.body = remap(arm.body);
        require(arm.firstLabel <= s.caseLabels.size() &&
                    arm.labelCount <= s.caseLabels.size() - arm.firstLabel,
                "Invalid case labels");
        row.firstLabel = count32(d.caseLabels.size());
        d.caseLabels.insert(d.caseLabels.end(), s.caseLabels.begin() + arm.firstLabel,
                            s.caseLabels.begin() + arm.firstLabel + arm.labelCount);
        d.arms[armRows.at(arm.id.value)] = row;
        p.receipt.arms.push_back(row.id);
    }
    for (auto &a : s.actions) {
        auto row = a;
        row.id = remap(a.id);
        row.owner = remap(a.owner);
        row.value = value(a.value);
        row.sourceInstruction = 0;
        row.importedCallTarget = {};
        require(a.firstArm <= s.arms.size() && a.armCount <= s.arms.size() - a.firstArm,
                "Invalid fragment arm range");
        row.firstArm = a.armCount ? armRows.at(s.arms.at(a.firstArm).id.value) : 0;
        if (a.kind == ActionKind::Loop) {
            auto l = s.loops.at(a.detail);
            require(l.owner == a.id, "Invalid loop owner");
            l.owner = row.id;
            l.condition = remap(l.condition);
            l.initialize = remap(l.initialize);
            l.body = remap(l.body);
            l.update = remap(l.update);
            row.detail = count32(d.loops.size());
            d.loops.push_back(l);
            p.loops.push_back({row.detail, {}});
        }
        if (a.kind == ActionKind::Switch) {
            auto sw = s.switches.at(a.detail);
            require(sw.owner == a.id, "Invalid switch owner");
            sw.owner = row.id;
            sw.selector = remap(sw.selector);
            row.detail = count32(d.switches.size());
            d.switches.push_back(sw);
            p.switches.push_back({row.detail, {}});
        }
        if (a.kind == ActionKind::Transfer) {
            auto t = s.transfers.at(a.detail);
            require(t.owner == a.id, "Invalid transfer owner");
            t.owner = row.id;
            t.target = target(t.target);
            row.detail = count32(d.transfers.size());
            d.transfers.push_back(t);
            p.transfers.push_back({row.detail, {}});
        }
        if (a.kind == ActionKind::Dispatch) {
            auto dispatch = s.dispatches.at(a.detail);
            require(dispatch.owner == a.id, "Invalid dispatch owner");
            dispatch.owner = row.id;
            for (auto &sample : dispatch.samples)
                sample.condition = remap(sample.condition);
            for (auto &route : dispatch.routes) {
                route.arm = remap(route.arm);
                for (auto &sample : route.samples)
                    sample = remap(sample);
            }
            row.detail = count32(d.dispatches.size());
            d.dispatches.push_back(dispatch);
            p.dispatches.push_back({row.detail, {}});
        }
        *d.actions.find(actionRows.at(a.id.value)) = row;
        created(row.id);
        checks.insert(row.id);
        p.receipt.actions.push_back(row.id);
    }
    if (s.sections.empty()) {
        auto orderIds = order(destination);
        require(position <= orderIds.size(), "Invalid paste position");
        std::vector<SctActionId> roots;
        for (auto id : f.roots)
            roots.push_back(remap(id));
        orderIds.insert(orderIds.begin() + position, roots.begin(), roots.end());
        putOrder(destination, std::move(orderIds));
        for(auto id:roots)p.receipt.selected.push_back(id);
    } else {
        auto &scriptRow = touch(p.scripts, scriptAt, d.scripts[scriptAt]);
        auto insertion = scriptRow.sections.size();
        if (auto anchor = std::get_if<SectionId>(&e.anchor))
            insertion = std::ranges::find(scriptRow.sections, *anchor) - scriptRow.sections.begin() + 1;
        for (auto section : s.sections) {
            require(section.body.valid() && !section.content.valid(),
                    "Clipboard sections must contain executable bodies");
            auto old = section.id;
            auto index = count32(d.sections.size());
            section.id = SectionId{claim(old.value, EntityKind::Section, index)};
            section.script = script;
            section.body = remap(section.body);
            section.sourceSection = 0;
            section.name = uniqueName(section.name);
            d.sections.push_back(section);
            p.sections.push_back({index, {}});
            scriptRow.sections.insert(scriptRow.sections.begin() + insertion++, section.id);
            p.receipt.sections.push_back(section.id);
            p.receipt.selected.push_back(section.id);
        }
        p.receipt.scriptLayouts.push_back(script);
    }
    // Verify just the pasted ownership closure before it enters live history.
    std::set<SctActionId> seen;
    std::vector<std::pair<SctActionId, SctSequenceId>> pending;
    if (s.sections.empty())
        for (auto id : f.roots)
            pending.push_back({remap(id), destination});
    else
        for (auto &sec : s.sections)
            for (auto id : *d.sequence(remap(sec.body))->order)
                pending.push_back({id, remap(sec.body)});
    while (!pending.empty()) {
        auto [id, owner] = pending.back();
        pending.pop_back();
        auto *a = d.action(id);
        require(a && a->owner == owner && seen.insert(id).second, "Invalid clipboard ownership or cycle");
        for (auto seq : children(id)) {
            auto *child = d.sequence(seq);
            require(child && child->script == script && child->owner && d.parentAction(*child) == id,
                    "Invalid clipboard body owner");
            for (auto nested : *child->order)
                pending.push_back({nested, seq});
        }
    }
    require(seen.size() == s.actions.size(), "Fragment contains unreachable actions");
    for (auto a : f.annotations) {
        a.subject = std::visit([&](auto id) -> editor::Subject { return remap(id); }, a.subject);
        apply(SemanticOperation{PutAnnotation{std::move(a)}});
    }
}

void SemanticTransaction::moveMany(const MoveActions &e) {
    require(!e.actions.empty(), "Select actions to move");
    auto *dest = d.sequence(e.destination);
    require(dest, "Missing destination");
    std::set<SctActionId> selected(e.actions.begin(), e.actions.end());
    require(selected.size() == e.actions.size(), "Repeated move target");
    std::map<SctSequenceId, std::vector<SctActionId>> orders;
    for (auto id : e.actions) {
        auto *a = d.action(id);
        require(a && d.sequence(a->owner)->script == dest->script, "Move stays within a script");
        auto *ancestor = dest;
        while (ancestor->owner) {
            auto parent = d.parentAction(*ancestor);
            require(!selected.contains(parent), "Cannot move a group into itself");
            ancestor = d.sequence(d.action(parent)->owner);
        }
        orders.try_emplace(a->owner, order(a->owner));
    }
    orders.try_emplace(e.destination, order(e.destination));
    auto &target = orders[e.destination];
    require(e.position <= target.size(), "Move position is outside body");
    auto position = e.position;
    for (std::size_t i = 0; i < e.position; ++i)
        if (selected.contains(target[i]))
            --position;
    for (auto &[seq, ids] : orders)
        std::erase_if(ids, [&](auto id) { return selected.contains(id); });
    target.insert(target.begin() + position, e.actions.begin(), e.actions.end());
    for (auto id : e.actions) {
        editAction(id).owner = e.destination;
        p.receipt.selected.push_back(id);
    }
    for (auto &[seq, ids] : orders)
        putOrder(seq, std::move(ids));
}
