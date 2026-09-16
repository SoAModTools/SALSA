std::future<Result<editor::LocalizationCatalog>> ProgramService::localizations(Revision r, bool recognize,
                                                                               std::stop_token stop) {
    return control_->submit<editor::LocalizationCatalog>([=](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        editor::LocalizationCatalog out;
        out.revision = r;
        out.languages = d.metadata.languages;
        out.settings = d.metadata.localization;
        for (const auto &logical : d.metadata.logicalTexts) {
            stopped(stop);
            editor::LocalizationMessage row;
            row.id = logical.id;
            row.script = logical.script;
            row.name = logical.name;
            row.scriptName = d.lookup(logical.script.value, EntityKind::Script, d.scripts)->context.name;
            for (auto id : logical.variants) {
                auto *text = d.lookup(id.value, EntityKind::Text, d.texts);
                row.correspondenceKeys.push_back(text->correspondenceKey.empty()
                                                     ? text->sourceGroup + "/" + text->sourceName
                                                     : text->correspondenceKey);
                ValueBuffer b;
                CopyValues cp{d, b};
                auto v = cp.copy(text->value);
                auto value = ProgramValues::text(b, v);
                if (!value) {
                    out.issues.push_back("Unavailable text in " + logical.name);
                    continue;
                }
                for (auto language : text->languages) {
                    auto *group = d.lookup(language.value, EntityKind::Language, d.metadata.languages);
                    if (std::ranges::find(row.translations, group->name,
                                          &editor::TranslationCell::language) != row.translations.end()) {
                        out.issues.push_back("Ambiguous language variants in " + logical.name);
                        continue;
                    }
                    row.translations.push_back({logical.id, group->name, value.value()});
                }
            }
            out.messages.push_back(std::move(row));
        }
        auto lookup = localizationLookup(d);
        std::set<SctActionId> covered;
        if (recognize)
            for (std::uint32_t n = 0; n < d.actions.size(); ++n) {
                stopped(stop);
                if (covered.contains(d.actions.find(n)->id))
                    continue;
                if (auto found = recognizeLocalization(d, d.actions.find(n)->id, &lookup)) {
                    auto *action = d.action(found->header);
                    auto *script =
                        d.lookup(d.sequence(action->owner)->script.value, EntityKind::Script, d.scripts);
                    std::string message;
                    auto describe = [&](const Action &action) {
                        featureOperands(d, action, [&](auto, auto value) {
                            auto *v = d.values.find(value.value);
                            if (message.empty() && v->kind == ValueKind::IndexedTextReference)
                                if (auto *logical = logicalFor(d, SctTextResourceId{v->data}, &lookup))
                                    message = logical->name;
                        });
                    };
                    if (found->single)
                        describe(*action);
                    else
                        for (auto id : *d.sequence(found->branches.front().arm.body)->order)
                            describe(*d.action(id));
                    out.candidates.emplace_back(found->header, script->context.name + " / " + message +
                                                                   " — " + found->reason);
                    for (auto &branch : found->branches)
                        covered.insert(branch.owner);
                }
            }
        return out;
    });
}

std::future<Result<std::optional<SctTextResourceId>>> ProgramService::translation(Revision r,
                                                                                  SctTextResourceId text,
                                                                                  SctLanguageGroupId language,
                                                                                  std::stop_token stop) {
    return control_->submit<std::optional<SctTextResourceId>>(
        [=](ServiceOwner &o) -> std::optional<SctTextResourceId> {
            o.match(r, stop);
            require(o.data != nullptr, "No project");
            auto &d = *o.data;
            auto *logical = logicalFor(d, text);
            if (!logical || !language.valid())
                return text;
            std::optional<SctTextResourceId> found;
            for (auto id : logical->variants) {
                auto *t = d.lookup(id.value, EntityKind::Text, d.texts);
                if (std::ranges::find(t->languages, language) != t->languages.end()) {
                    require(!found, "Ambiguous translation");
                    found = id;
                }
            }
            return found;
        });
}
