// Included by ProgramService.cpp inside the program namespace.
// Only direct children are visited. Payloads and descendant bodies stay resident.
namespace {
std::size_t localizedBodySize(const ProgramData &d, const editor::LocalizedBlock &block) {
    auto *body = d.lookup(block.body.value, EntityKind::Sequence, d.sequences);
    auto size = body->order->size();
    if (size) {
        auto *last = d.action(body->order->back());
        if (last->kind == ActionKind::Transfer) {
            auto &transfer = d.transfers[last->detail];
            if (transfer.kind == TransferKind::Break && transfer.target == block.action)
                --size;
        }
    }
    return size;
}
bool sectionContentIs(const ProgramData &data, const Section &section, ValueKind kind) {
    const auto *value = data.values.find(section.content.value);
    return !section.body.valid() && value && value->kind == kind;
}
// The existing marker is the group identity. As in SPICE's current string-group
// index, it owns the following contiguous indexed sections, stopping at any
// other section. This is an outline over authored order, not a second authority.
std::optional<SectionId> indexedStringParent(const ProgramData &data, const Section &section) {
    if (!sectionContentIs(data, section, ValueKind::IndexedSection)) return {};
    auto *script = data.lookup(section.script.value, EntityKind::Script, data.scripts);
    require(script, "Missing string-group script");
    std::optional<SectionId> marker;
    for (auto id : script->sections) {
        if (id == section.id) return marker;
        auto *current = data.lookup(id.value, EntityKind::Section, data.sections);
        require(current, "Missing string-group section");
        if (sectionContentIs(data, *current, ValueKind::StringGroup)) marker = id;
        else if (!sectionContentIs(data, *current, ValueKind::IndexedSection)) marker.reset();
    }
    return {};
}
} // namespace
std::future<Result<OutlinePage>> ProgramService::outline(Revision r, OutlineRequest request, std::stop_token stop) {
    return control_->submit<OutlinePage>([r, request = std::move(request), stop](auto &owner) {
        const auto started = std::chrono::steady_clock::now();
        owner.match(r, stop);
        require(owner.data != nullptr, "No program is open");
        require(request.limit > 0 && request.limit <= 512, "Outline page size must be between 1 and 512");
        auto &d = *owner.data;
        if (owner.outlineAliasesDirty) {
            owner.outlineAliases.clear();
            owner.outlineStyles.clear();
            for(const auto &a:d.metadata.annotations)owner.outlineStyles[a.subject]={a.color,!a.bookmark.empty()};
            for (const auto &annotation : d.metadata.annotations)
                if (!annotation.alias.empty())
                    owner.outlineAliases[annotation.subject] = annotation.alias;
            owner.outlineAliasesDirty = false;
        }
        OutlinePage out;
        out.status = owner.status();
        out.parent = request.parent;
        out.offset = request.offset;
        auto append = [&](OutlineRow row) {
            if (request.semantic) if (auto* id = std::get_if<SctActionId>(&row.subject)) {
                if (auto* action = d.action(*id)) {
                    const auto& grouping = owner.flows(action->owner, stop);
                    if (auto at = grouping.issues.find(*id); at != grouping.issues.end()) row.groupingIssue = at->second;
                }
            }
            if(auto *id=std::get_if<SctActionId>(&row.subject))if(auto *a=d.action(*id);a && a->kind==ActionKind::Instruction) {
                auto opcode=d.values.find(a->value.value)->data;
                auto c=std::ranges::find(d.metadata.opcodes,opcode,&editor::OpcodeCustomization::opcode);
                if(c!=d.metadata.opcodes.end()){if(!c->name.empty())row.label=c->name;row.color=c->color;}
            }
            if(auto style=owner.outlineStyles.find(row.subject);style!=owner.outlineStyles.end()){if(style->second.first)row.color=style->second.first;row.bookmarked=style->second.second;}
            if (auto alias = owner.outlineAliases.find(row.subject); alias != owner.outlineAliases.end())
                row.label = alias->second;
            const auto ordinal = out.total++;
            if (ordinal >= request.offset && out.rows.size() < request.limit) {
                if (request.semantic && row.condition) {
                    auto *c = d.lookup(row.condition->value, EntityKind::Condition, d.conditions);
                    require(c, "Missing header condition");
                    ValueBuffer values; CopyValues copy{d, values};
                    auto root = copy.copy(c->result);
                    auto expression = ProgramValues::expression(values, root);
                    require(bool(expression), "Invalid header expression");
                    auto text = SctExpressionLanguage::project(expression.value()).text;
                    std::replace(text.begin(), text.end(), '\n', ' ');
                    std::replace(text.begin(), text.end(), '\r', ' ');
                    std::replace(text.begin(), text.end(), '\t', ' ');
                    if (text.size() > 180) {
                        auto end = std::size_t{180};
                        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
                        text.resize(end); text += "...";
                    }
                    row.label += " " + text;
                    auto *prep = d.sequence(c->preparation);
                    if (prep && !prep->order->empty()) row.label += " [" + std::to_string(prep->order->size()) + " before check]";
                    row.preparation = c->preparation;
                }
                out.rows.push_back(std::move(row));
            }
        };
        auto logicalSection = [&](const Section &section) -> const SctLogicalText * {
            if (!sectionContentIs(d, section, ValueKind::IndexedSection))
                return nullptr;
            return logicalFor(d, SctTextResourceId{d.values.find(section.content.value)->data});
        };
        auto primarySection = [&](const Section &section) {
            auto *logical = logicalSection(section);
            return !logical || logical->variants.empty() ||
                   logical->variants.front().value == d.values.find(section.content.value)->data;
        };
        std::function<void(SctActionId, SctSequenceId)> action;
        action = [&](SctActionId id, SctSequenceId seq) {
            const auto *a = d.action(id);
            require(a != nullptr, "Missing outline action");
            std::string label;
            switch (a->kind) {
            case ActionKind::Instruction: {
                const auto *value = d.values.find(a->value.value);
                require(value != nullptr, "Missing instruction payload");
                const auto *schema = spice::sct::findSctOpcodeSchema(static_cast<std::uint16_t>(value->data));
                label = schema ? std::string(schema->semantic.mnemonic) : "Opcode " + std::to_string(value->data);
                break;
            }
            case ActionKind::Dispatch:
                label = "Route dispatch";
                break;
            case ActionKind::Conditional:
                label = request.semantic ? "If" : "If / Else If / Else";
                break;
            case ActionKind::Loop: {
                const auto kind = d.loops.at(a->detail).kind;
                label = kind == LoopKind::For ? "For" : kind == LoopKind::DoWhile ? "Do While" : "While";
                break;
            }
            case ActionKind::Switch:
                label = "Switch";
                break;
            case ActionKind::Transfer:
                label = d.transfers.at(a->detail).kind == TransferKind::Break ? "Break" : "Continue";
                break;
            }
            const auto localized =
                std::ranges::find(d.metadata.localization.blocks, id, &editor::LocalizedBlock::action);
            if (localized != d.metadata.localization.blocks.end()) {
                auto *body = d.sequence(localized->body);
                if (request.semantic && localizedBodySize(d, *localized) == 1 &&
                    d.action(body->order->front())->kind == ActionKind::Instruction) {
                    action(body->order->front(), localized->body);
                    return;
                }
                label = "Localized sequence";
            }
            if (request.semantic && a->kind == ActionKind::Instruction)
                featureOperands(d, *a, [&](auto address, auto value) {
                    auto *v = d.values.find(value.value);
                    if (v->kind != ValueKind::IndexedTextReference)
                        return;
                    auto *message = logicalFor(d, SctTextResourceId{v->data});
                    if (!message)
                        return;
                    label += " : " + message->name;
                    const Text *text = d.lookup(v->data, EntityKind::Text, d.texts);
                    if (request.language.valid()) {
                        text = nullptr;
                        for (auto variant : message->variants) {
                            auto *t = d.lookup(variant.value, EntityKind::Text, d.texts);
                            if (std::ranges::find(t->languages, request.language) != t->languages.end()) {
                                if (text) {
                                    text = nullptr;
                                    break;
                                }
                                text = t;
                            }
                        }
                    }
                    if (!text) {
                        label += " — Missing translation";
                        return;
                    }
                    ValueBuffer b;
                    CopyValues cp{d, b};
                    auto root = cp.copy(text->value);
                    auto tv = ProgramValues::text(b, root);
                    if (!tv)
                        return;
                    std::string preview;
                    if (auto p = std::get_if<spice::sct::SctPlainText>(&tv.value()))
                        preview = p->utf8;
                    if (auto p = std::get_if<spice::sct::SctMessage>(&tv.value()))
                        for (const auto &element : p->body.elements)
                            if (auto t = std::get_if<spice::sct::SctTextChunk>(&element))
                                preview += t->utf8;
                    std::replace(preview.begin(), preview.end(), '\n', ' ');
                    if (preview.size() > 100) {
                        std::size_t end = 100;
                        while (end && (static_cast<unsigned char>(preview[end]) & 0xc0) == 0x80)
                            --end;
                        preview.resize(end);
                        preview += "…";
                    }
                    label += " — " + preview;
                });
            OutlineRow row{id, seq, std::move(label), a->kind != ActionKind::Instruction && a->kind != ActionKind::Transfer, a->kind};
            if (request.semantic && localized == d.metadata.localization.blocks.end()) {
                if (a->kind == ActionKind::Conditional && a->armCount) {
                    const auto &first = d.arms[a->firstArm];
                    row.chain = id;
                    row.condition = first.condition;
                    row.sequence = first.body;
                } else if (a->kind == ActionKind::Loop)
                    row.condition = d.loops.at(a->detail).condition;
                else if (a->kind == ActionKind::Switch)
                    row.condition = d.switches.at(a->detail).selector;
            }
            append(std::move(row));
            if (request.semantic && a->kind == ActionKind::Conditional) {
                for (std::uint32_t n = 1; n < a->armCount; ++n) {
                    auto &arm = d.arms[a->firstArm + n];
                    OutlineRow branch{arm.id, arm.body, arm.condition.valid() ? "Else If" : "Else", true};
                    branch.chain = id;
                    if (arm.condition.valid())
                        branch.condition = arm.condition;
                    append(std::move(branch));
                }
            }
        };
        auto sequence = [&](SctSequenceId id) {
            const auto *seq = d.sequence(id);
            require(seq && seq->order, "Missing outline sequence");
            out.script = seq->script;
            if (request.semantic) {
                const auto &offsets = owner.rowOffsets(id, stop);
                const auto base = out.total;
                const auto start = std::max(base, request.offset) - base;
                if (start < offsets.back() && out.rows.size() < request.limit) {
                    auto at = static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), start) - offsets.begin() - 1);
                    out.total = base + offsets[at];
                    for (; at < seq->order->size() && out.rows.size() < request.limit; ++at) {
                        stopped(stop);
                        action((*seq->order)[at], id);
                    }
                }
                out.total = base + offsets.back();
                return;
            }
            // Direct indexing gives constant work per requested row, independent
            // of how many sibling actions precede the requested page.
            const auto start = std::max(out.total, request.offset) - out.total;
            const auto end = std::min(seq->order->size(), start + (request.limit - out.rows.size()));
            out.total += std::min(start, seq->order->size());
            for (auto i = start; i < end; ++i) {
                stopped(stop);
                action((*seq->order)[i], id);
            }
            out.total += seq->order->size() - std::min(end, seq->order->size());
        };
        auto bodyRow = [&](SctSequenceId id, const char *name) {
            if (!id.valid())
                return;
            const auto *seq = d.sequence(id);
            require(seq && seq->order, "Missing outline body");
            append({id, id, name, true});
        };
        auto condition = [&](ConditionId id) {
            auto *c = d.lookup(id.value, EntityKind::Condition, d.conditions);
            require(c != nullptr, "Missing outline condition");
            const auto *seq = d.sequence(c->preparation);
            require(seq != nullptr, "Missing condition preparation");
            if (request.semantic && seq->order->empty()) return;
            append({id, c->preparation, request.semantic ? "Before check" : "Condition", true});
        };
        std::visit(
            [&](auto id) {
                using T = decltype(id);
                if constexpr (std::is_same_v<T, SctScriptId>) {
                    auto *script = d.lookup(id.value, EntityKind::Script, d.scripts);
                    require(script != nullptr, "Missing outline script");
                    out.script = id;
                    std::vector<SectionId> semanticSections;
                    std::span<const SectionId> sections = script->sections;
                    if (request.semantic) {
                        semanticSections.reserve(script->sections.size());
                        bool inStringGroup = false;
                        for (auto sid : script->sections) {
                            stopped(stop);
                            auto *section = d.lookup(sid.value, EntityKind::Section, d.sections);
                            require(section, "Missing outline section");
                            if (!primarySection(*section))
                                continue;
                            if (sectionContentIs(d, *section, ValueKind::IndexedSection) && inStringGroup)
                                continue;
                            semanticSections.push_back(sid);
                            inStringGroup = sectionContentIs(d, *section, ValueKind::StringGroup);
                        }
                        sections = semanticSections;
                    }
                    out.total = sections.size() + script->supplementaryText.size();
                    auto total = out.total;
                    out.total = request.offset;
                    for (auto at = request.offset; at < std::min(total, request.offset + request.limit); ++at) {
                        stopped(stop);
                        if (at < sections.size()) {
                            auto *s = d.lookup(sections[at].value, EntityKind::Section, d.sections);
                            require(s != nullptr, "Missing outline section");
                            auto *logical = request.semantic ? logicalSection(*s) : nullptr;
                            append(
                                {s->id, s->body, logical ? logical->name : s->name,
                                 s->body.valid() || logical ||
                                     (request.semantic && sectionContentIs(d, *s, ValueKind::StringGroup))});
                        } else {
                            auto *t = d.lookup(script->supplementaryText[at - sections.size()].value,
                                               EntityKind::Text, d.texts);
                            require(t != nullptr, "Missing footer text");
                            append(
                                {t->id,
                                 {},
                                 t->sourceName.empty() ? "Footer text " + std::to_string(t->id.value) : t->sourceName,
                                 false});
                        }
                    }
                    out.total = total;
                } else if constexpr (std::is_same_v<T, SctModuleId>) {
                    auto *m=d.lookup(id.value,EntityKind::Module,d.metadata.modules);require(m,"Missing module");out.script=m->script;
                    for(auto body:m->sequences){std::string label="Body "+std::to_string(body.value);for(const auto& sec:d.sections)if(sec.body==body){label=sec.name;break;}bodyRow(body,label.c_str());}
                } else if constexpr (std::is_same_v<T, SectionId>) {
                    const auto *s = d.lookup(id.value, EntityKind::Section, d.sections);
                    require(s != nullptr, "Missing outline section");
                    out.script = s->script;
                    if (s->body.valid())
                        sequence(s->body);
                    else if (request.semantic && logicalSection(*s)) {
                        auto *logical = logicalSection(*s);
                        for (auto id : logical->variants) {
                            auto *text = d.lookup(id.value, EntityKind::Text, d.texts);
                            std::string label;
                            for (auto l : text->languages) {
                                auto *language =
                                    d.lookup(l.value, EntityKind::Language, d.metadata.languages);
                                if (!label.empty())
                                    label += ", ";
                                label += language->name;
                            }
                            append({id, {}, label.empty() ? "Unassigned" : label, false});
                        }
                    } else if (request.semantic && sectionContentIs(d, *s, ValueKind::StringGroup)) {
                        const auto *script = d.lookup(s->script.value, EntityKind::Script, d.scripts);
                        require(script, "Missing string-group script");
                        auto at = std::ranges::find(script->sections, id);
                        require(at != script->sections.end(), "String group is not in its script");
                        for (++at; at != script->sections.end(); ++at) {
                            stopped(stop);
                            const auto *member = d.lookup(at->value, EntityKind::Section, d.sections);
                            require(member, "Missing string-group member");
                            if (!sectionContentIs(d, *member, ValueKind::IndexedSection)) break;
                            if (!primarySection(*member))
                                continue;
                            // Do not allocate labels for members outside the requested page.
                            if (out.total < request.offset || out.rows.size() == request.limit)
                                ++out.total;
                            else {
                                auto *logical = logicalSection(*member);
                                append({member->id,
                                        {},
                                        logical ? logical->name : member->name,
                                        logical != nullptr});
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, SctSequenceId>)
                    sequence(id);
                else if constexpr (std::is_same_v<T, SctActionId>) {
                    const auto *a = d.action(id);
                    require(a != nullptr, "Missing outline construct");
                    out.script = d.sequence(a->owner)->script;
                    auto localized = std::ranges::find(d.metadata.localization.blocks, id,
                                                       &editor::LocalizedBlock::action);
                    if (request.semantic && localized != d.metadata.localization.blocks.end())
                        sequence(localized->body);
                    else if (request.semantic && a->kind == ActionKind::Conditional) {
                        if (a->armCount) {
                            const auto &first = d.arms[a->firstArm];
                            if (first.condition.valid()) condition(first.condition);
                            sequence(first.body);
                        }
                    } else if (a->kind == ActionKind::Conditional || a->kind == ActionKind::Switch ||
                               a->kind == ActionKind::Dispatch) {
                        if (a->kind == ActionKind::Switch)
                            condition(d.switches.at(a->detail).selector);
                        for (std::uint32_t i = 0; i < a->armCount; ++i) {
                            const auto &arm = d.arms[a->firstArm + i];
                            std::string label;
                            if (a->kind == ActionKind::Dispatch)
                                label = "Route " + std::to_string(i + 1);
                            else if (a->kind == ActionKind::Conditional)
                                label = arm.condition.valid() ? (i ? "Else If" : "If") : "Else";
                            else {
                                label = "Case ";
                                for (auto n = 0u; n < arm.labelCount; ++n) {
                                    if (n)
                                        label += ", ";
                                    label += std::to_string(d.caseLabels[arm.firstLabel + n]);
                                }
                                if (arm.includesDefault)
                                    label += " Default";
                            }
                            OutlineRow row{arm.id, arm.body, std::move(label), true};
                            if (request.semantic && arm.condition.valid()) row.condition = arm.condition;
                            append(std::move(row));
                        }
                    } else if (a->kind == ActionKind::Loop) {
                        const auto &l = d.loops.at(a->detail);
                        bodyRow(l.initialize, "Initialize");
                        if (l.kind != LoopKind::DoWhile)
                            condition(l.condition);
                        bodyRow(l.body, "Body");
                        bodyRow(l.update, "Update");
                        if (l.kind == LoopKind::DoWhile)
                            condition(l.condition);
                    }
                } else if constexpr (std::is_same_v<T, ArmId>) {
                    auto *arm = d.lookup(id.value, EntityKind::Arm, d.arms);
                    require(arm != nullptr, "Missing outline arm");
                    if (arm->condition.valid())
                        condition(arm->condition);
                    sequence(arm->body);
                } else if constexpr (std::is_same_v<T, ConditionId>) {
                    auto *c = d.lookup(id.value, EntityKind::Condition, d.conditions);
                    require(c != nullptr, "Missing outline condition");
                    sequence(c->preparation);
                } else
                    require(false, "This item has no outline children");
            },
            request.parent);
        out.elapsedMicroseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        return out;
    });
}

std::future<Result<NavigationPath>> ProgramService::locate(Revision revision, EditorSubject subject, bool semantic) {
    return control_->submit<NavigationPath>([revision, subject, semantic](auto &owner) mutable {
        owner.match(revision, {});
        require(owner.data != nullptr, "No program is open");
        auto &d = *owner.data;
        NavigationPath out{owner.status()};
        auto sequenceParent = [&](SctSequenceId id) -> EditorSubject {
            const auto *seq = d.sequence(id);
            require(seq, "Missing navigation sequence");
            out.script = seq->script;
            if (seq->role == SequenceRole::LoopBody || seq->role == SequenceRole::LoopInitialize ||
                seq->role == SequenceRole::LoopUpdate)
                return id;
            if (seq->owner)
                return std::visit([](auto id) -> EditorSubject { return id; }, *seq->owner);
            auto *script = d.lookup(seq->script.value, EntityKind::Script, d.scripts);
            require(script, "Missing navigation script");
            for (auto id : script->sections) {
                auto *s = d.lookup(id.value, EntityKind::Section, d.sections);
                if (s && s->body == seq->id)
                    return id;
            }
            throw std::runtime_error("Sequence has no section");
        };
        for (unsigned depth = 0; depth < 512; ++depth) {
            out.subjects.push_back(subject);
            bool done = false;
            subject = std::visit(
                [&](auto id) -> EditorSubject {
                    using T = decltype(id);
                    if constexpr (std::is_same_v<T, SctScriptId>) {
                        require(d.lookup(id.value, EntityKind::Script, d.scripts), "Missing script");
                        out.script = id;
                        done = true;
                        return id;
                    } else if constexpr (std::is_same_v<T, SectionId>) {
                        auto *s = d.lookup(id.value, EntityKind::Section, d.sections);
                        require(s, "Missing section");
                        if (semantic) if (auto group = indexedStringParent(d, *s)) return *group;
                        return s->script;
                    } else if constexpr (std::is_same_v<T, SctActionId>) {
                        auto *a = d.action(id);
                        require(a, "Missing action");
                        return sequenceParent(a->owner);
                    } else if constexpr (std::is_same_v<T, SctSequenceId>) {
                        auto *s = d.sequence(id);
                        require(s, "Missing sequence");
                        if (s->owner)
                            return std::visit([](auto id) -> EditorSubject { return id; }, *s->owner);
                        return sequenceParent(id);
                    } else if constexpr (std::is_same_v<T, ArmId>) {
                        auto *a = d.lookup(id.value, EntityKind::Arm, d.arms);
                        require(a, "Missing arm");
                        return a->owner;
                    } else if constexpr (std::is_same_v<T, ConditionId>) {
                        auto *c = d.lookup(id.value, EntityKind::Condition, d.conditions);
                        require(c, "Missing condition");
                        return c->arm ? EditorSubject(*c->arm) : EditorSubject(c->owner);
                    } else if constexpr (std::is_same_v<T, SctTextResourceId>) {
                        auto *t = d.lookup(id.value, EntityKind::Text, d.texts);
                        require(t, "Missing text");
                        if (semantic && !t->supplementary)
                            if (auto *logical = logicalFor(d, id); logical && !logical->variants.empty()) {
                                auto *script = d.lookup(t->script.value, EntityKind::Script, d.scripts);
                                for (auto sid : script->sections) {
                                    auto *section = d.lookup(sid.value, EntityKind::Section, d.sections);
                                    auto *value = d.values.find(section->content.value);
                                    if (value && value->kind == ValueKind::IndexedSection &&
                                        value->data == logical->variants.front().value)
                                        return sid;
                                }
                            }
                        if (!t->supplementary) {
                            auto *script = d.lookup(t->script.value, EntityKind::Script, d.scripts);
                            for (auto sid : script->sections) {
                                auto *s = d.lookup(sid.value, EntityKind::Section, d.sections);
                                auto *v = d.values.find(s->content.value);
                                if (v && v->kind == ValueKind::IndexedSection && v->data == id.value) {
                                    out.subjects.pop_back();
                                    return sid;
                                }
                            }
                        }
                        return t->script;
                    } else
                        throw std::runtime_error(
                            "This subject is shown in authoring tools rather than the script outline");
                },
                subject);
            if (done) {
                std::reverse(out.subjects.begin(), out.subjects.end());
                if (semantic) {
                    std::vector<EditorSubject> visible;
                    for (std::size_t n = 0; n < out.subjects.size(); ++n) {
                        const auto &item = out.subjects[n];
                        if (auto id = std::get_if<SctActionId>(&item)) {
                            auto localized = std::ranges::find(d.metadata.localization.blocks, *id,
                                                               &editor::LocalizedBlock::action);
                            if (localized != d.metadata.localization.blocks.end()) {
                                auto *body = d.sequence(localized->body);
                                if (localizedBodySize(d, *localized) == 1 &&
                                    d.action(body->order->front())->kind == ActionKind::Instruction)
                                    continue;
                            }
                        }
                        if (auto id = std::get_if<ArmId>(&item)) {
                            auto *arm = d.lookup(id->value, EntityKind::Arm, d.arms);
                            auto *action = d.action(arm->owner);
                            if (std::ranges::find(d.metadata.localization.blocks, action->id,
                                                  &editor::LocalizedBlock::action) !=
                                d.metadata.localization.blocks.end())
                                continue;
                            if (action->kind == ActionKind::Conditional) {
                                if (d.arms[action->firstArm].id == *id) continue;
                                if (!visible.empty() && visible.back() == EditorSubject(action->id)) visible.pop_back();
                            }
                        }
                        if (auto id = std::get_if<ConditionId>(&item)) {
                            auto *c = d.lookup(id->value, EntityKind::Condition, d.conditions);
                            if (d.sequence(c->preparation)->order->empty()) continue;
                        }
                        visible.push_back(item);
                    }
                    out.subjects = std::move(visible);
                }
                return out;
            }
        }
        throw std::runtime_error("Navigation ancestry is cyclic or too deep");
    });
}
