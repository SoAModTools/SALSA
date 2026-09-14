// Shared scoped helpers; internal values never escape the serialized owner.
ValueId routeParameter(const ProgramData &d, SctActionId id, spice::sct::SctParameterAddress address) {
    auto *a = d.action(id);
    if (!a || a->kind != ActionKind::Instruction)
        return {};
    std::uint32_t group = 0;
    for (auto child : d.child(a->value)) {
        auto *v = d.values.find(child.value);
        if (v->kind == ValueKind::Parameter && !address.repeatedGroupOrdinal && v->data == address.schemaIndex)
            return d.child(child).front();
        if (v->kind == ValueKind::RepeatedGroup) {
            if (address.repeatedGroupOrdinal == group)
                for (auto p : d.child(child))
                    if (d.values.find(p.value)->data == address.schemaIndex)
                        return d.child(p).front();
            ++group;
        }
    }
    return {};
}
SctScriptId slotScript(const ProgramData &d, const Port &p) {
    return std::visit(
        [&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, SctModuleId>) {
                auto *m = d.lookup(id.value, EntityKind::Module, d.metadata.modules);
                require(m, "Missing slot module");
                return m->script;
            } else {
                auto *e = d.lookup(id.value, EntityKind::Entry, d.metadata.entrypoints);
                require(e, "Missing slot entrypoint");
                return e->script;
            }
        },
        p.owner);
}
SctActionId slotEntry(const ProgramData &d, const Port &p) {
    if (p.action)
        return *p.action;
    if (p.sequence)
        if (auto *s = d.lookup(p.sequence->value, EntityKind::Sequence, d.sequences);
            s && s->order && !s->order->empty())
            return s->order->front();
    return {};
}
bool moduleContains(const ProgramData &d, const Module &m, SctSequenceId id) {
    while (auto *s = d.lookup(id.value, EntityKind::Sequence, d.sequences)) {
        if (std::ranges::find(m.sequences, id) != m.sequences.end())
            return true;
        if (!s->owner)
            break;
        id = d.action(d.parentAction(*s))->owner;
    }
    return false;
}

// A low-level move can detach an inserted route from its selected boundary.
// Detect that instead of reconnecting an unrelated action at its new location.
bool slotImplementation(const ProgramData &d, const Port &p, SctActionId id) {
    if (p.boundary == SlotBoundary::Operand || p.boundary == SlotBoundary::Terminal)
        return p.action == id;
    auto *a = d.action(id);
    if (!a)
        return false;
    auto *seq = d.lookup(a->owner.value, EntityKind::Sequence, d.sequences);
    if (!seq || seq->order->empty())
        return false;
    if (p.boundary == SlotBoundary::SequenceEnd)
        return p.sequence == seq->id && seq->order->back() == id;
    if (!p.action || !d.action(*p.action) || d.action(*p.action)->owner != seq->id)
        return false;
    auto at = std::ranges::find(*seq->order, *p.action);
    return at != seq->order->end() && ++at != seq->order->end() && *at == id;
}
