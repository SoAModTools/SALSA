// Helpers shared by scoped feature queries and atomic edits.
ValueId featureParameter(const ProgramData &d, SctActionId action, spice::sct::SctParameterAddress address) {
    auto *a = d.action(action);
    require(a && a->kind == ActionKind::Instruction, "Select an instruction operand");
    std::uint32_t group = 0;
    for (auto id : d.child(a->value)) {
        auto *v = d.values.find(id.value);
        if (v->kind == ValueKind::Parameter && !address.repeatedGroupOrdinal &&
            v->data == address.schemaIndex)
            return d.child(id).front();
        if (v->kind == ValueKind::RepeatedGroup) {
            if (address.repeatedGroupOrdinal && group == *address.repeatedGroupOrdinal)
                for (auto p : d.child(id))
                    if (d.values.find(p.value)->data == address.schemaIndex)
                        return d.child(p).front();
            ++group;
        }
    }
    throw std::runtime_error("Operand no longer exists");
}
template <class F> void featureOperands(const ProgramData &d, const Action &a, F f) {
    if (a.kind != ActionKind::Instruction)
        return;
    std::uint32_t group = 0;
    for (auto id : d.child(a.value)) {
        auto *v = d.values.find(id.value);
        if (v->kind == ValueKind::Parameter)
            f(spice::sct::SctParameterAddress{static_cast<std::uint32_t>(v->data), {}}, d.child(id).front());
        else if (v->kind == ValueKind::RepeatedGroup) {
            for (auto p : d.child(id))
                f(spice::sct::SctParameterAddress{static_cast<std::uint32_t>(d.values.find(p.value)->data),
                                                  group},
                  d.child(p).front());
            ++group;
        }
    }
}
std::vector<editor::FeatureSite> featureTextUses(ProgramData &d, SctTextResourceId text,
                                                 std::stop_token stop = {}) {
    auto *t = d.lookup(text.value, EntityKind::Text, d.texts);
    require(t, "Missing text");
    std::vector<editor::FeatureSite> out;
    if (t->supplementary) {
        auto index = d.directory.find(text.value).index;
        if (index < d.footerOwners.size()) if (auto *a = d.action(d.footerOwners[index]))
            featureOperands(d, *a, [&](auto address, auto id) {
                const auto *v = d.values.find(id.value);
                if (v->kind == ValueKind::SupplementaryTextReference && v->data == text.value)
                    out.push_back({a->id, address});
            });
        return out;
    }
    for (std::uint32_t n = 0; n < d.actions.size(); ++n) {
        stopped(stop);
        auto &a = *d.actions.find(n);
        if (!a.id.valid() || d.sequence(a.owner)->script != t->script)
            continue;
        featureOperands(d, a, [&](auto address, auto id) {
            auto *v = d.values.find(id.value);
            if ((v->kind == ValueKind::IndexedTextReference ||
                 v->kind == ValueKind::SupplementaryTextReference) &&
                v->data == text.value)
                out.push_back({a.id, address});
        });
    }
    return out;
}
std::vector<editor::LanguageSuggestion> featureLanguageSuggestions(const ProgramData &d, SctScriptId script) {
    std::vector<SctTextObservation> names;
    for (const auto &t : d.texts)
        if (t.id.valid() && t.script == script && !t.supplementary)
            names.push_back({spice::sct::SctStringId{t.id.value}, t.sourceName, t.sourceGroup});
    std::vector<editor::LanguageSuggestion> out;
    for (const auto &o : SctTextAuthoring::observeNames(std::move(names)))
        out.push_back({SctTextResourceId{std::get<spice::sct::SctStringId>(o.target).value()},
                       o.languageKey.value_or(""), o.correspondenceKey, o.explanation});
    return out;
}
std::optional<editor::SelectorRow> featureSelector(const ProgramData &d, const Action &a) {
    if (a.kind != ActionKind::Instruction || d.values.find(a.value.value)->data != 5)
        return {};
    auto dest = featureParameter(d, a.id, {0, {}});
    auto *v = d.values.find(dest.value);
    if (v->kind != ValueKind::Word || (v->data & 0xffff8000) != 0x10000000)
        return {};
    ValueBuffer b;
    CopyValues copy{d, b};
    auto root = copy.copy(featureParameter(d, a.id, {1, {}}));
    auto expr = ProgramValues::expression(b, root);
    require(bool(expr), "Invalid selector expression");
    bool scheduled = false;
    for (auto id : d.child(a.value))
        scheduled |= d.values.find(id.value)->kind == ValueKind::Expression;
    return editor::SelectorRow{a.id, static_cast<std::uint32_t>(v->data & 0x7fff), expr.value(), scheduled};
}
