void SemanticTransaction::localizationEdit(const editor::LocalizationChange &request) {
    namespace ed = editor;
    auto value = [&](const Result<ValueBuffer> &r) {
        if (!r)
            throw std::runtime_error(r.diagnostics().front().message);
        return importValue(d, r.value());
    };
    if (!p.localization)
        p.localization = d.metadata.localization;
    auto &state = d.metadata.localization;
    std::set<SctLanguageGroupId> configured;
    for (auto &rule : state.languages)
        configured.insert(rule.language);
    p.receipt.presentationChanged = true;
    if (request.discover) {
        std::set<SctTextResourceId> linked;
        for (auto &m : d.metadata.logicalTexts)
            linked.insert(m.variants.begin(), m.variants.end());
        std::set<std::pair<SctActionId, spice::sct::SctParameterAddress>> sites;
        for (auto &u : state.uses)
            sites.emplace(u.action, u.operand);
        for (const auto &script : d.scripts) {
            auto suggestions = featureLanguageSuggestions(d, script.context.id);
            std::vector<SctTextResourceId> ids;
            for (const auto &s : suggestions)
                if (!s.key.empty())
                    ids.push_back(s.text);
            feature(ed::ApplyLanguageSuggestions{script.context.id, ids});
            // An editable neutral group for strings without a language-name pattern.
            // This is not a claim about the source platform or locale.
            for (std::uint32_t n = 0; n < d.texts.size(); ++n) {
                auto &t = d.texts[n];
                if (!t.id.valid() || t.script != script.context.id || t.supplementary ||
                    t.explicitAssignment || !t.languages.empty())
                    continue;
                auto group =
                    std::ranges::find(d.metadata.languages, std::string("Source"), &SctLanguageGroup::name);
                SctLanguageGroupId language;
                if (group == d.metadata.languages.end()) {
                    put(SctLanguageGroup{{}, "Source", "localization:source"});
                    language = d.metadata.languages.back().id;
                } else
                    language = group->id;
                touch(p.texts, n, t).languages = {language};
                p.receipt.texts.push_back(t.id);
            }
            std::map<std::string, std::vector<SctTextResourceId>> families;
            for (const auto &t : d.texts)
                if (t.id.valid() && t.script == script.context.id && !t.supplementary &&
                    !linked.contains(t.id))
                    families[t.correspondenceKey.empty() ? t.sourceGroup + "/" + t.sourceName
                                                         : t.correspondenceKey]
                        .push_back(t.id);
            for (auto &[key, variants] : families) {
                std::set<SctLanguageGroupId> unique;
                bool ambiguous = false;
                for (auto id : variants)
                    for (auto lang : d.lookup(id.value, EntityKind::Text, d.texts)->languages)
                        if (!unique.insert(lang).second)
                            ambiguous = true;
                if (!ambiguous) {
                    linked.insert(variants.begin(), variants.end());
                    put(SctLogicalText{{}, script.context.id, key, std::move(variants), {}});
                }
            }
        }
        auto lookup = localizationLookup(d);
        for (std::uint32_t n = 0; n < d.actions.size(); ++n) {
            auto *a = d.actions.find(n);
            if (!a->id.valid())
                continue;
            featureOperands(d, *a, [&](auto address, auto value) {
                auto *v = d.values.find(value.value);
                if (v->kind != ValueKind::IndexedTextReference)
                    return;
                auto *logical = logicalFor(d, SctTextResourceId{v->data}, &lookup);
                if (logical && sites.emplace(a->id, address).second)
                    state.uses.push_back({a->id, address, logical->id});
            });
        }
        // Mapping evidence does not require equivalent execution bodies.
        // A guard may prepare more work for one language and still identify its selector value.
        for (std::uint32_t n = 0; n < d.actions.size(); ++n) {
            auto *action = d.actions.find(n);
            if (!action->id.valid() || action->kind != ActionKind::Conditional)
                continue;
            for (std::uint32_t j = 0; j < action->armCount; ++j) {
                auto arm = d.arms[action->firstArm + j];
                auto selector = languageValue(d, arm.condition);
                if (!selector)
                    continue;
                for (auto child : *d.sequence(arm.body)->order)
                    featureOperands(d, *d.action(child), [&](auto, auto value) {
                        auto *v = d.values.find(value.value);
                        if (v->kind != ValueKind::IndexedTextReference)
                            return;
                        auto *t = d.lookup(v->data, EntityKind::Text, d.texts);
                        if (t->languages.size() != 1)
                            return;
                        auto language = t->languages.front();
                        if (configured.contains(language))
                            return;
                        auto it =
                            std::ranges::find(state.languages, language, &ed::LocalizationLanguage::language);
                        if (it == state.languages.end())
                            state.languages.push_back({language, *selector, true});
                        else if (it->selector != selector)
                            it->selector.reset();
                    });
            }
        }
    }
    if (request.discover) {
        auto lookup = localizationLookup(d);
        for (std::uint32_t n = 0; n < d.actions.size(); ++n)
            if (auto r = recognizeLocalization(d, d.actions.find(n)->id, &lookup); r && r->sw)
                for (auto &branch : r->branches) {
                    if (configured.contains(branch.language))
                        continue;
                    auto it = std::ranges::find(state.languages, branch.language,
                                                &ed::LocalizationLanguage::language);
                    if (it == state.languages.end())
                        state.languages.push_back({branch.language, branch.selector, true});
                    else if (it->selector != branch.selector)
                        it->selector.reset();
                }
    }
    if (request.discover) {
        std::map<std::uint32_t, std::vector<std::size_t>> values;
        for (std::size_t n = 0; n < state.languages.size(); ++n)
            if (state.languages[n].selector)
                values[*state.languages[n].selector].push_back(n);
        for (auto &[_, rows] : values)
            if (rows.size() > 1)
                for (auto n : rows)
                    if (!configured.contains(state.languages[n].language))
                        state.languages[n].selector.reset();
    }
    for (auto &group : d.metadata.languages)
        if (std::ranges::find(state.languages, group.id, &ed::LocalizationLanguage::language) ==
            state.languages.end())
            state.languages.push_back(
                {group.id,
                 {},
                 group.recognitionKey != "localization:source" || d.metadata.languages.size() == 1});
    if (request.languages)
        state.languages = *request.languages;
    for (const auto &cell : request.translations) {
        auto message = cell.message;
        if (!message.valid()) {
            require(cell.script.valid() && !cell.messageName.empty(),
                    "New translation needs an existing script and message name");
            auto found = std::ranges::find_if(d.metadata.logicalTexts, [&](auto &m) {
                return m.script == cell.script && m.name == cell.messageName;
            });
            if (found == d.metadata.logicalTexts.end()) {
                put(SctLogicalText{{}, cell.script, cell.messageName, {}, {}});
                message = d.metadata.logicalTexts.back().id;
            } else
                message = found->id;
        }
        auto mi = at(message.value, EntityKind::LogicalText);
        auto logical = d.metadata.logicalTexts[mi];
        auto lg = std::ranges::find(d.metadata.languages, cell.language, &SctLanguageGroup::name);
        SctLanguageGroupId language;
        if (lg == d.metadata.languages.end()) {
            require(!cell.language.empty(), "Language name is empty");
            put(SctLanguageGroup{{}, cell.language, {}});
            language = d.metadata.languages.back().id;
            state.languages.push_back({language, {}, true});
        } else
            language = lg->id;
        std::vector<SctTextResourceId> matches;
        for (auto id : logical.variants) {
            auto *t = d.lookup(id.value, EntityKind::Text, d.texts);
            if (std::ranges::find(t->languages, language) != t->languages.end())
                matches.push_back(id);
        }
        require(matches.size() <= 1,
                "More than one variant is assigned to this language; resolve its assignments first");
        if (!cell.value) {
            for (auto id : matches) {
                auto ti = at(id.value, EntityKind::Text);
                auto &t = touch(p.texts, ti, d.texts[ti]);
                std::erase(t.languages, language);
                t.explicitAssignment = true;
                p.receipt.texts.push_back(id);
            }
            continue;
        }
        if (!matches.empty() &&
            d.lookup(matches.front().value, EntityKind::Text, d.texts)->languages.size() == 1) {
            auto ti = at(matches.front().value, EntityKind::Text);
            touch(p.texts, ti, d.texts[ti]).value = value(ProgramValues::text(*cell.value));
            p.receipt.texts.push_back(matches.front());
            continue;
        }
        for (auto id : matches) {
            auto ti = at(id.value, EntityKind::Text);
            auto &t = touch(p.texts, ti, d.texts[ti]);
            std::erase(t.languages, language);
            p.receipt.texts.push_back(id);
        }
        auto scriptAt = at(logical.script.value, EntityKind::Script);
        auto id = SctTextResourceId{d.allocate()};
        auto name = "L" + std::to_string(id.value);
        while (std::ranges::any_of(d.sections,
                                   [&](auto &s) { return s.script == logical.script && s.name == name; }))
            name += "_";
        auto ti = count32(d.texts.size());
        d.texts.push_back({});
        p.texts.push_back({ti, {}});
        locate(id.value, {EntityKind::Text, ti});
        d.texts[ti] = {id,
                       logical.script,
                       value(ProgramValues::text(*cell.value)),
                       spice::sct::SctTextKind::SctString,
                       false,
                       0,
                       name,
                       {},
                       logical.name,
                       {language},
                       true};
        auto section = SectionId{d.allocate()};
        auto si = count32(d.sections.size());
        d.sections.push_back({});
        p.sections.push_back({si, {}});
        locate(section.value, {EntityKind::Section, si});
        std::vector<ValueId> preamble;
        for (auto word : spice::sct::SctStringSectionContent{}.preambleWords)
            preamble.push_back(d.add(ValueKind::Word, word));
        auto words = d.add(ValueKind::Words, 0, 0, preamble);
        d.sections[si] = {section,
                          logical.script,
                          name,
                          {},
                          d.add(ValueKind::IndexedSection, id.value, 0, std::span(&words, 1))};
        touch(p.scripts, scriptAt, d.scripts[scriptAt]).sections.push_back(section);
        p.receipt.texts.push_back(id);
        p.receipt.sections.push_back(section);
        p.receipt.scriptLayouts.push_back(logical.script);
        logical.variants.push_back(id);
        put(logical);
    }
    // Recognize against the captured edit revision before neighboring routes change shape.
    std::vector<LocalizationRecognition> promotions;
    std::set<SctActionId> promotedOwners;
    for (auto id : request.promote) {
        auto r = recognizeLocalization(d, id);
        require(r.has_value(),
                "This region is not an equivalent localized sequence; keep its instructions explicit");
        std::set<SctActionId> owners;
        for (auto &branch : r->branches)
            owners.insert(branch.owner);
        for (auto owner : owners) {
            require(promotedOwners.insert(owner).second, "Overlapping localization selections");
            captureTree(owner);
        }
        promotions.push_back(std::move(*r));
    }
    for (const auto &r : promotions) {
        auto id = r.header;
        auto first = r.branches.front();
        if (r.single) {
            auto original = *d.action(id);
            auto ids = order(original.owner);
            auto position = static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
            auto root = create(original.owner, position, ActionKind::Conditional);
            ArmEdit arm;
            arm.condition =
                ValueBuffer::expression(*SctExpressionLanguage::parse("ByteVar[87] == 0").expression).value();
            arms(root, {arm});
            auto body = d.arms[d.action(root)->firstArm].body;
            move(id, body, 0);
            state.blocks.push_back({root, body, false, false, true, {first.language}, r.reason, true});
            p.receipt.actions.push_back(root);
            p.receipt.projectionSequences.push_back(original.owner);
            continue;
        }

        for (const auto &branch : r.branches) {
            auto mapping =
                std::ranges::find(state.languages, branch.language, &ed::LocalizationLanguage::language);
            require(mapping != state.languages.end() && mapping->selector == branch.selector,
                    "Resolve conflicting runtime selector mappings before organizing this region");
        }
        // Keep the first arm and its instruction identities. All other bodies are derived output.
        ArmEdit kept;
        kept.id = first.arm.id;
        if (r.sw)
            kept.labels = {static_cast<std::int32_t>(first.selector)};
        else {
            ValueBuffer b;
            CopyValues cp{d, b};
            b.root =
                cp.copy(d.lookup(first.arm.condition.value, EntityKind::Condition, d.conditions)->result);
            kept.condition = std::move(b);
        }
        if (!r.independent)
            arms(id, {kept});
        else {
            auto orderIds = order(d.action(id)->owner);
            for (std::size_t n = 1; n < r.branches.size(); ++n) {
                eraseTree(r.branches[n].owner);
                std::erase(orderIds, r.branches[n].owner);
            }
            putOrder(d.action(id)->owner, std::move(orderIds));
        }
        std::erase_if(state.uses, [&](const auto &use) { return !d.action(use.action); });
        ed::LocalizedBlock block{id, first.arm.body, r.sw, r.independent, r.refresh, {}, r.reason};
        for (const auto &b : r.branches)
            block.languages.push_back(b.language);
        state.blocks.push_back(std::move(block));
        p.receipt.actions.push_back(id);
        p.receipt.sequences.push_back(d.action(id)->owner);
        p.receipt.projectionSequences.push_back(d.action(id)->owner);
    }
    for (auto id : request.expand) {
        captureTree(id);
        auto found = std::ranges::find(state.blocks, id, &ed::LocalizedBlock::action);
        require(found != state.blocks.end(), "Select a localized block");
        auto block = *found;
        auto sourceOrder = *d.sequence(block.body)->order;
        std::vector<Action> originals;
        for (auto child : sourceOrder) {
            auto a = *d.action(child);
            require(a.kind == ActionKind::Instruction || a.kind == ActionKind::Transfer,
                    "Expand nested constructs separately before expanding localization");
            originals.push_back(a);
        }
        std::vector<ed::LocalizationLanguage> rules;
        for (auto language : block.languages) {
            auto l = std::ranges::find(state.languages, language, &ed::LocalizationLanguage::language);
            if (l != state.languages.end() && l->included)
                rules.push_back(*l);
        }
        for (auto &l : state.languages)
            if (l.included && std::ranges::find(block.languages, l.language) == block.languages.end())
                rules.push_back(l);
        require(!rules.empty(), "Include a language before expanding localization");
        const bool unwrap = block.unconditional && rules.size() == 1;
        for (auto &l : rules)
            require(unwrap || l.selector.has_value(), "Assign selector values before expanding localization");
        state.blocks.erase(found);
        auto root = *d.action(id);
        auto script = d.sequence(root.owner)->script;
        auto first = d.arms[root.firstArm];
        std::vector<ArmEdit> edits;
        auto conditionValue = [&](std::uint32_t n) {
            return ValueBuffer::expression(
                       *SctExpressionLanguage::parse("ByteVar[87] == " + std::to_string(n)).expression)
                .value();
        };
        for (std::size_t n = 0; n < rules.size(); ++n) {
            ArmEdit e;
            if (n == 0)
                e.id = first.id;
            if (block.switchSelection)
                e.labels = {static_cast<std::int32_t>(*rules[n].selector)};
            else
                e.condition = conditionValue(rules[n].selector.value_or(0));
            edits.push_back(std::move(e));
        }
        if (!block.independent)
            arms(id, edits);
        else
            arms(id, {edits.front()});
        for (std::size_t n = 0; n < rules.size(); ++n) {
            SctActionId owner = id;
            SctSequenceId body;
            if (block.independent && n) {
                auto orderIds = order(root.owner);
                auto at = std::ranges::find(orderIds, id) - orderIds.begin();
                owner = create(root.owner, static_cast<std::size_t>(at) + n, ActionKind::Conditional);
                auto e = edits[n];
                e.id = {};
                arms(owner, {e});
            }
            auto *a = d.action(owner);
            auto arm = d.arms[a->firstArm + (block.independent ? 0 : static_cast<std::uint32_t>(n))];
            body = arm.body;
            if (arm.condition.valid()) {
                auto ci = at(arm.condition.value, EntityKind::Condition);
                touch(p.conditions, ci, d.conditions[ci]).skipRefresh = block.skipRefresh;
            }
            for (std::size_t ordinal = 0; ordinal < originals.size(); ++ordinal) {
                auto source = originals[ordinal];
                auto target = source.id;
                if (n) {
                    target = create(body, ordinal, source.kind);
                    auto &copy = editAction(target);
                    copy.value = source.value;
                    if (source.kind == ActionKind::Transfer) {
                        auto transfer = d.transfers[source.detail];
                        transfer.owner = target;
                        transfer.target = owner;
                        copy.detail = count32(d.transfers.size());
                        d.transfers.push_back(transfer);
                        p.transfers.push_back({copy.detail, {}});
                    }
                }
                if (source.kind != ActionKind::Instruction)
                    continue;
                std::vector<std::pair<spice::sct::SctParameterAddress, SctTextResourceId>> replacements;
                featureOperands(d, source, [&](auto address, auto v) {
                    auto *value = d.values.find(v.value);
                    if (value->kind != ValueKind::IndexedTextReference)
                        return;
                    auto *message = logicalFor(d, SctTextResourceId{value->data});
                    if (!message)
                        return;
                    SctTextResourceId selected;
                    for (auto variant : message->variants) {
                        auto *text = d.lookup(variant.value, EntityKind::Text, d.texts);
                        if (std::ranges::find(text->languages, rules[n].language) != text->languages.end()) {
                            require(!selected.valid(), "Ambiguous translation");
                            selected = variant;
                        }
                    }
                    require(selected.valid(), "Missing translation for explicit expansion");
                    replacements.emplace_back(address, selected);
                });
                for (auto [address, text] : replacements)
                    featureReplace({target, address}, d.add(ValueKind::IndexedTextReference, text.value));
            }
        }
        if (unwrap) {
            auto ids = order(root.owner);
            auto position = static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
            for (auto child : sourceOrder)
                move(child, root.owner, position++);
            auto remaining = order(root.owner);
            std::erase(remaining, id);
            putOrder(root.owner, std::move(remaining));
            eraseTree(id);
        }
        p.receipt.projectionSequences.push_back(root.owner);
        p.receipt.actions.push_back(id);
    }
}
