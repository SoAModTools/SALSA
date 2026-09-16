void SemanticTransaction::featureReplace(editor::FeatureSite site, ValueId value) {
    featureParameter(d, site.action, site.operand);
    collect(site.action);
    checks.insert(site.action);
    parameter(site.action, site.operand, value);
}
void SemanticTransaction::feature(const editor::FeatureEdit &edit) {
    namespace ed = editor;
    namespace sct = spice::sct;
    auto assertion =
        std::vector<SctAuthoringEvidence>{{SctEvidenceKind::UserAssertion, "Edited through authoring tools"}};
    auto uniqueName = [&](SctScriptId script, const std::string &name) {
        require(!name.empty() && name != "init" && name != "loop", "Choose a non-lifecycle name");
        for (auto id : d.scripts[at(script.value, EntityKind::Script)].sections)
            require(d.sections[at(id.value, EntityKind::Section)].name != name,
                    "Section name already exists");
    };
    auto value = [&](const auto &result) {
        if (!result)
            throw std::runtime_error(result.diagnostics().front().message);
        return importValue(d, result.value());
    };
    std::visit(
        [&](const auto &e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ed::LocalizationChange>)
                localizationEdit(e);
            else if constexpr (std::is_same_v<T, ed::SetArrival>) {
                auto index = at(e.condition.value, EntityKind::Condition);
                auto c = d.conditions[index];
                auto *owner = d.action(c.owner);
                require(owner->kind != ActionKind::Dispatch || c.arm.has_value(),
                        "Select a route requirement rather than a shared timing sample");
                ValueBuffer buffer;
                CopyValues copy{d, buffer};
                auto root = copy.copy(c.result);
                auto current = ProgramValues::expression(buffer, root);
                require(bool(current), "Missing arrival predicate");
                auto *program = std::get_if<sct::SctTypedScptProgram>(&current.value().body);
                require(program && program->operations.size() == 3,
                        "Arrival helper requires a simple existing comparison; edit compound requirements "
                        "explicitly");
                auto expected = SctExpressionLanguage::parse("Low16IntVar[15]");
                auto *read = std::get_if<sct::SctScptValueOperation>(&program->operations[0]);
                auto *expectedProgram = std::get_if<sct::SctTypedScptProgram>(&expected.expression->body);
                require(read && *read ==
                                    std::get<sct::SctScptValueOperation>(expectedProgram->operations.front()),
                        "Condition does not compare previous location");
                auto *literal = std::get_if<sct::SctScptValueOperation>(&program->operations[1]);
                auto *comparison = std::get_if<sct::SctScptBinaryOperation>(&program->operations[2]);
                require(literal &&
                            (literal->kind == sct::SctScptValueKind::DecimalLiteral ||
                             literal->kind == sct::SctScptValueKind::FloatLiteral) &&
                            comparison && sct::sctScptOperatorSymbol(comparison->encodingWord) == "==",
                        "Arrival helper requires an existing literal equality");
                std::uint32_t encoded = 0;
                if (e.encoded)
                    encoded = *e.encoded;
                else {
                    auto location = SctSequenceAuthoring::arrivalLocation(e.stem);
                    if (!location)
                        throw std::runtime_error(location.diagnostics().front().message);
                    encoded = location.value().encoded;
                }
                auto next = SctExpressionLanguage::parse("Low16IntVar[15] == " + std::to_string(encoded));
                require(next.succeeded(), "Arrival value is not representable");
                auto *nextProgram = std::get_if<sct::SctTypedScptProgram>(&next.expression->body);
                program->operations[1] = nextProgram->operations[1];
                collect(c.owner);
                touch(p.conditions, index, d.conditions[index]).result =
                    value(ValueBuffer::expression(current.value()));
            } else if constexpr (std::is_same_v<T, ed::WriteModule>) {
                Module row{e.module.id, e.module.script, e.module.name, e.module.bodies, assertion};
                if (row.id.valid())
                    row.evidence = d.metadata.modules[at(row.id.value, EntityKind::Module)].evidence;
                if (e.createEmptyBody) {
                    require(!row.id.valid() && row.sequences.empty(),
                            "Empty module creation needs a new module");
                    uniqueName(row.script, row.name);
                    auto body = sequence(row.script, {}, SequenceRole::SectionBody);
                    d.sequences[at(body.value, EntityKind::Sequence)].owner.reset();
                    auto id = SectionId{d.allocate()};
                    auto index = count32(d.sections.size());
                    d.sections.push_back({id, row.script, row.name, body});
                    p.sections.push_back({index, {}});
                    locate(id.value, {EntityKind::Section, index});
                    touch(p.scripts, at(row.script.value, EntityKind::Script),
                          d.scripts[at(row.script.value, EntityKind::Script)])
                        .sections.push_back(id);
                    p.receipt.scriptLayouts.push_back(row.script);
                    row.sequences.push_back(body);
                }
                put(row);
            } else if constexpr (std::is_same_v<T, ed::RemoveModule>)
                eraseMetadata<Module>(e.id);
            else if constexpr (std::is_same_v<T, ed::WriteEntry>) {
                const auto &v = e.entry;
                Entrypoint row;
                if (v.id.valid())
                    row = d.metadata.entrypoints[at(v.id.value, EntityKind::Entry)];
                row.id = v.id;
                row.script = v.script;
                row.name = v.name;
                row.kind = v.kind;
                row.body = v.body;
                row.enabled = v.enabled;
                row.activation = v.activation;
                row.evidence.insert(row.evidence.end(), assertion.begin(), assertion.end());
                if (e.responseBody) {
                    require(e.responseSite.has_value(), "Select an existing response target operand");
                    auto *body = d.sequence(*e.responseBody);
                    require(body && body->script == v.script && !body->order->empty(),
                            "Select a nonempty response body in this script");
                    auto idValue = featureParameter(d, e.responseSite->action, e.responseSite->operand);
                    require(d.values.find(idValue.value)->kind == ValueKind::ActionReference,
                            "Response site must be an execution target operand");
                    SctModuleId owner;
                    for (const auto &m : d.metadata.modules)
                        if (std::ranges::find(m.sequences, *e.responseBody) != m.sequences.end()) {
                            owner = m.id;
                            break;
                        }
                    require(owner.valid(), "Response body must belong to a module");
                    Port port;
                    port.owner = owner;
                    port.name = "Entry";
                    port.action = body->order->front();
                    port.sequence = body->id;
                    port.role = SlotRole::Entry;
                    port.boundary = SlotBoundary::SequenceEnd;
                    for (const auto &p : d.metadata.ports)
                        if (p.owner == port.owner && p.sequence == port.sequence &&
                            p.role == SlotRole::Entry) {
                            port = p;
                            break;
                        }
                    if (!port.id.valid()) {
                        put(port);
                        port = d.metadata.ports.back();
                    }
                    row.response = port.id;
                    row.responseSite = Site{e.responseSite->action, e.responseSite->operand};
                    featureReplace(*e.responseSite,
                                   d.add(ValueKind::ActionReference, body->order->front().value));
                }
                put(row);
            } else if constexpr (std::is_same_v<T, ed::RemoveEntry>)
                eraseMetadata<Entrypoint>(e.id);
            else if constexpr (std::is_same_v<T, ed::RepairRoot>)
                apply(SemanticOperation{BindLifecycle{e.section, e.name}});
            else if constexpr (std::is_same_v<T, ed::WriteLanguage>)
                put(e.group);
            else if constexpr (std::is_same_v<T, ed::RemoveLanguage>)
                eraseMetadata<SctLanguageGroup>(e.id);
            else if constexpr (std::is_same_v<T, ed::AssignTextGroups>) {
                for (auto id : e.texts)
                    apply(SemanticOperation{AssignLanguages{id, e.languages}});
            } else if constexpr (std::is_same_v<T, ed::ApplyLanguageSuggestions>) {
                auto suggestions = featureLanguageSuggestions(d, e.script);
                for (auto id : e.texts) {
                    auto found = std::ranges::find(suggestions, id, &ed::LanguageSuggestion::text);
                    require(found != suggestions.end() && !found->key.empty(),
                            "No unambiguous naming suggestion for text");
                    if (d.texts[at(id.value, EntityKind::Text)].explicitAssignment)
                        continue;
                    auto key = "legacy:" + found->key;
                    auto language =
                        std::ranges::find(d.metadata.languages, key, &SctLanguageGroup::recognitionKey);
                    SctLanguageGroupId group;
                    if (language == d.metadata.languages.end()) {
                        put(SctLanguageGroup{{},
                                             found->key == "base" ? "Unmarked"
                                             : found->key == "F"  ? "French"
                                             : found->key == "S"  ? "Spanish"
                                             : found->key == "G"  ? "German"
                                                                  : found->key,
                                             key});
                        group = d.metadata.languages.back().id;
                    } else
                        group = language->id;
                    auto &text = touch(p.texts, at(id.value, EntityKind::Text),
                                       d.texts[at(id.value, EntityKind::Text)]);
                    if (text.explicitAssignment)
                        continue;
                    text.languages = {group};
                    text.correspondenceKey = found->correspondence;
                    Annotation note;
                    auto existing =
                        std::ranges::find(d.metadata.annotations, editor::Subject{id}, &Annotation::subject);
                    if (existing != d.metadata.annotations.end())
                        note = *existing;
                    note.subject = id;
                    note.evidence.push_back({SctEvidenceKind::Inference, found->explanation});
                    apply(SemanticOperation{PutAnnotation{note}});
                    p.receipt.presentationChanged = true;
                }
            } else if constexpr (std::is_same_v<T, ed::WriteLogicalText>)
                put(e.item);
            else if constexpr (std::is_same_v<T, ed::RemoveLogicalText>)
                eraseMetadata<SctLogicalText>(e.id);
            else if constexpr (std::is_same_v<T, ed::WriteTextBatch>) {
                std::set<SctTextResourceId> seen;
                for (const auto &text : e.edits) {
                    require(seen.insert(text.text).second, "Repeated batch text");
                    auto index = at(text.text.value, EntityKind::Text);
                    touch(p.texts, index, d.texts[index]).value = value(ProgramValues::text(text.value));
                }
            } else if constexpr (std::is_same_v<T, ed::SplitText>) {
                auto original = d.texts[at(e.text.value, EntityKind::Text)];
                at(e.language.value, EntityKind::Language);
                require(!original.supplementary, "Split an indexed text");
                require(std::ranges::find(original.languages, e.language) != original.languages.end(),
                        "Text is not assigned to this language");
                require(!e.uses.empty(), "Select uses for the split text");
                uniqueName(original.script, e.name);
                auto id = SctTextResourceId{d.allocate()};
                auto index = count32(d.texts.size());
                original.id = id;
                original.sourceText = 0;
                original.sourceName = e.name;
                original.languages = {e.language};
                original.explicitAssignment = true;
                d.texts.push_back(original);
                p.texts.push_back({index, {}});
                locate(id.value, {EntityKind::Text, index});
                auto section = SectionId{d.allocate()};
                auto si = count32(d.sections.size());
                std::vector<ValueId> words;
                for (auto w : sct::SctStringSectionContent{}.preambleWords)
                    words.push_back(d.add(ValueKind::Word, w));
                auto preamble = d.add(ValueKind::Words, 0, 0, words);
                d.sections.push_back(
                    {section,
                     original.script,
                     e.name,
                     {},
                     d.add(ValueKind::IndexedSection, id.value, 0, std::span(&preamble, 1))});
                p.sections.push_back({si, {}});
                locate(section.value, {EntityKind::Section, si});
                auto scriptAt = at(original.script.value, EntityKind::Script);
                touch(p.scripts, scriptAt, d.scripts[scriptAt]).sections.push_back(section);
                p.receipt.scriptLayouts.push_back(original.script);
                for (auto site : e.uses) {
                    auto *ref = d.values.find(featureParameter(d, site.action, site.operand).value);
                    require(ref->kind == ValueKind::IndexedTextReference && ref->data == e.text.value,
                            "Selected use no longer references shared text");
                    featureReplace(site, d.add(ValueKind::IndexedTextReference, id.value));
                }
                auto oi = at(e.text.value, EntityKind::Text);
                auto &previous = touch(p.texts, oi, d.texts[oi]);
                std::erase(previous.languages, e.language);
                previous.explicitAssignment = true;
                for (auto logical : d.metadata.logicalTexts)
                    if (std::ranges::find(logical.variants, e.text) != logical.variants.end()) {
                        logical.variants.push_back(id);
                        put(logical);
                    }
                p.receipt.selected.push_back(id);
            } else if constexpr (std::is_same_v<T, ed::InsertTextUses>) {
                for (const auto &use : e.edits) {
                    auto *target = d.lookup(use.text.value, EntityKind::Text, d.texts);
                    require(target, "Missing inserted text");
                    auto *prototype = d.action(use.prototype.action);
                    auto *before = d.action(use.before);
                    require(prototype && before && d.sequence(before->owner)->script == target->script &&
                                d.sequence(prototype->owner)->script == target->script,
                            "Insertion stays in one script");
                    auto root = prototype->value;
                    auto ref = *d.values.find(
                        featureParameter(d, use.prototype.action, use.prototype.operand).value);
                    require(ref.kind == (target->supplementary ? ValueKind::SupplementaryTextReference
                                                               : ValueKind::IndexedTextReference),
                            "Incompatible text storage");
                    auto orderIds = order(before->owner);
                    auto id =
                        create(before->owner, std::ranges::find(orderIds, use.before) - orderIds.begin(),
                               ActionKind::Instruction);
                    editAction(id).value = root;
                    featureReplace({id, use.prototype.operand}, d.add(ref.kind, target->id.value));
                    if (use.incoming) {
                        auto *old = d.values.find(
                            featureParameter(d, use.incoming->action, use.incoming->operand).value);
                        require(old->kind == ValueKind::ActionReference && old->data == use.before.value,
                                "Incoming edge changed");
                        featureReplace(*use.incoming, d.add(ValueKind::ActionReference, id.value));
                    }
                    p.receipt.selected.push_back(id);
                }
            } else if constexpr (std::is_same_v<T, ed::SetResources>) {
                auto i = at(e.script.value, EntityKind::Script);
                auto context = d.scripts[i].context;
                apply(SemanticOperation{
                    ScriptSettings{e.script, context.name, context.outputProfile, e.associations}});
            } else if constexpr (std::is_same_v<T, ed::SelectResourceEntry>) {
                auto entry = d.metadata.entrypoints[at(e.entry.value, EntityKind::Entry)];
                auto found =
                    std::ranges::find(e.snapshot.entries, e.tableIndex, &smores::script::Entry::tableIndex);
                require(found != e.snapshot.entries.end() && found->resource == e.snapshot.identity,
                        "Resource entry not in inspected snapshot");
                entry.provider = *found;
                entry.evidence.insert(entry.evidence.end(), assertion.begin(), assertion.end());
                put(entry);
            } else if constexpr (std::is_same_v<T, ed::WriteSelectors>) {
                auto *anchor = d.action(e.before);
                require(anchor, "Choose an insertion action");
                auto owner = anchor->owner;
                std::set<SctActionId> replaced;
                for (const auto &row : e.edits) {
                    require(smores::script::isOverworldSelector(row.destination),
                            "Select a known overworld selector");
                    auto expr = value(ValueBuffer::expression(row.expression));
                    if (row.replace) {
                        auto *a = d.action(*row.replace);
                        require(a && a->owner == owner && featureSelector(d, *a) &&
                                    replaced.insert(*row.replace).second,
                                "Choose a byte assignment in this body once");
                        featureReplace({*row.replace, {0, {}}},
                                       d.add(ValueKind::Word, 0x10000000 | row.destination));
                        featureReplace({*row.replace, {1, {}}}, expr);
                    } else {
                        auto ids = order(owner);
                        auto id = create(owner, std::ranges::find(ids, e.before) - ids.begin(),
                                         ActionKind::Instruction);
                        sct::SctInstructionFactoryRequest request{5};
                        request.skipRefresh = true;
                        request.parameterOverrides = {
                            {{0, {}}, sct::SctEncodedWordValue{0x10000000 | row.destination}},
                            {{1, {}}, row.expression}};
                        editAction(id).value = value(ProgramValues::instruction(request));
                        p.receipt.selected.push_back(id);
                    }
                }
            }
        },
        edit);
}
