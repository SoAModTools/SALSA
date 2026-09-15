std::future<Result<editor::FeatureInventory>> ProgramService::features(Revision r, SctScriptId script,
                                                                       std::stop_token stop) {
    return control_->submit<editor::FeatureInventory>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        auto *s = d.lookup(script.value, EntityKind::Script, d.scripts);
        require(s, "Missing script");
        editor::FeatureInventory out{r, script};
        out.resources = s->context.resources;
        out.languages = d.metadata.languages;
        std::map<SctSequenceId, const Section *> sectionsByBody;
        for (auto id : s->sections) {
            const auto *section = d.lookup(id.value, EntityKind::Section, d.sections);
            if (section->body.valid()) sectionsByBody.emplace(section->body, section);
        }
        for (const auto &m : d.metadata.modules)
            if (m.script == script)
                out.modules.push_back({m.id, m.script, m.name, m.sequences});
        for (const auto &e : d.metadata.entrypoints)
            if (e.script == script) {
                editor::EntryItem row{e.id, e.script, e.name, e.kind, e.body, e.enabled, e.activation};
                if (auto *ref = std::get_if<SctPortId>(&e.response))
                    row.response = *ref;
                row.provider = e.provider;
                out.entries.push_back(row);
            }
        for (const auto &t : d.texts)
            if (t.id.valid() && t.script == script)
                out.texts.push_back(
                    {t.id, t.sourceName, t.sourceGroup, t.supplementary, t.explicitAssignment, t.languages});
        for (const auto &l : d.metadata.logicalTexts)
            if (l.script == script)
                out.logical.push_back(l);
        for (const auto &seq : d.sequences)
            if (seq.id.valid() && seq.script == script) {
                stopped(stop);
                editor::BodyItem b{seq.id, "Body " + std::to_string(seq.id.value)};
                if (auto found = sectionsByBody.find(seq.id); found != sectionsByBody.end()) {
                    b.section = found->second->id;
                    b.name = found->second->name;
                }
                out.bodies.push_back(b);
                editor::DefinitionItem item{seq.id, b.name};
                if (!seq.order->empty())
                    for (const auto &use : program::incoming(d, seq.order->front()))
                        item.uses.push_back(use.site.action);
                out.definitions.push_back(std::move(item));
            }
        for (const auto &c : d.conditions)
            if (c.id.valid() && d.sequence(c.preparation)->script == script)
                out.definitions.push_back({c.id, "Condition " + std::to_string(c.id.value), {c.owner}});
        std::map<editor::Subject, std::string_view> aliases;
        for (const auto &note : d.metadata.annotations)
            if (!note.alias.empty()) aliases.emplace(note.subject, note.alias);
        for (auto &def : out.definitions)
            if (auto alias = aliases.find(def.subject); alias != aliases.end()) def.name = alias->second;
        return out;
    });
}
std::future<Result<std::vector<editor::FeatureSite>>>
ProgramService::textUses(Revision r, SctTextResourceId text, std::stop_token stop) {
    return control_->submit<std::vector<editor::FeatureSite>>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        return featureTextUses(*o.data, text, stop);
    });
}
std::future<Result<std::vector<editor::LanguageSuggestion>>>
ProgramService::suggestLanguages(Revision r, SctScriptId script, std::stop_token stop) {
    return control_->submit<std::vector<editor::LanguageSuggestion>>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        return featureLanguageSuggestions(*o.data, script);
    });
}
std::future<Result<editor::SelectorPreview>>
ProgramService::previewSelectors(Revision r, SctActionId first, SctActionId last,
                                 smores::script::KnownBytes values, std::stop_token stop) {
    return control_->submit<editor::SelectorPreview>(
        [=, values = std::move(values)](ServiceOwner &o) mutable {
            o.match(r, stop);
            require(o.data != nullptr, "No project");
            auto &d = *o.data;
            auto *a = d.action(first);
            auto *b = d.action(last);
            require(a && b && a->owner == b->owner, "Select a straight-line range in one body");
            auto *seq = d.sequence(a->owner);
            auto begin = std::ranges::find(*seq->order, first), end = std::ranges::find(*seq->order, last);
            require(begin <= end, "Range is reversed");
            editor::SelectorPreview out;
            out.revision = r;
            for (auto it = begin; it != end + 1; ++it) {
                stopped(stop);
                auto *action = d.action(*it);
                auto row = featureSelector(d, *action);
                if (row) {
                    values[row->destination] =
                        row->scheduled ? std::nullopt : smores::script::evaluateByte(row->expression, values);
                    out.assignments.push_back(*row);
                } else
                    require(action->kind == ActionKind::Instruction &&
                                (d.values.find(action->value.value)->data == 15 ||
                                 d.values.find(action->value.value)->data == 16),
                            "Preview accepts byte assignments and waits only; exclude branches, calls and "
                            "other effects");
            }
            out.values = values;
            smores::script::Catalog catalog;
            auto *script = d.lookup(seq->script.value, EntityKind::Script, d.scripts);
            std::vector<smores::script::ResourceIdentity> identities;
            for (const auto &a : script->context.resources) {
                catalog.resources.push_back({a.resource, a.role, {}});
                identities.push_back(a.resource);
            }
            for (const auto &family : smores::script::tileFamilies())
                out.tiles.push_back(smores::script::selectTile(family.key, values, catalog));
            out.overworldScript = smores::script::selectOverworld(true, values);
            out.overworldBase = smores::script::selectOverworld(false, values, identities);
            return out;
        });
}

std::future<Result<editor::ModuleDetail>> ProgramService::featureModule(Revision r, SctModuleId id,
                                                                        std::stop_token stop) {
    return control_->submit<editor::ModuleDetail>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        auto *module = d.lookup(id.value, EntityKind::Module, d.metadata.modules);
        require(module, "Missing module");
        auto summary = o.summarize(r, id, stop);
        editor::ModuleDetail out{r, {module->id, module->script, module->name, module->sequences}};
        const auto &s = *summary.summary;
        for (auto body : module->sequences) {
            auto *seq = d.sequence(body);
            if (seq->order->empty())
                continue;
            for (const auto &use : program::incoming(d, seq->order->front()))
                if (std::ranges::find(s.actions, use.site.action) == s.actions.end())
                    out.callers.push_back(use.site.action);
        }
        for (auto text : s.texts)
            out.dependencies.push_back(text);
        for (auto body : s.sequences)
            if (std::ranges::find(module->sequences, body) == module->sequences.end())
                out.dependencies.push_back(body);
        return out;
    });
}
std::future<Result<std::vector<SctLocalActivation>>>
ProgramService::activationSuggestions(Revision r, SctScriptId script, std::stop_token stop) {
    return control_->submit<std::vector<SctLocalActivation>>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        std::vector<SctLocalActivation> out;
        for (const auto &seq : d.sequences)
            if (seq.id.valid() && seq.script == script)
                for (std::size_t i = 0; i + 1 < seq.order->size(); ++i) {
                    stopped(stop);
                    auto *a = d.action((*seq.order)[i]);
                    if (a->kind != ActionKind::Conditional || !a->armCount)
                        continue;
                    const auto &arm = d.arms[a->firstArm];
                    auto *body = d.sequence(arm.body);
                    if (!body->order->empty())
                        out.push_back({a->id, {}, body->order->front(), (*seq.order)[i + 1], false});
                }
        return out;
    });
}
