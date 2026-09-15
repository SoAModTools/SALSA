// Included by ProgramService.cpp inside the program namespace.
// Only direct children are visited. Payloads and descendant bodies stay resident.
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
            if (ordinal >= request.offset && out.rows.size() < request.limit)
                out.rows.push_back(std::move(row));
        };
        auto action = [&](SctActionId id, SctSequenceId seq) {
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
                label = "If / Else If / Else";
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
            append({id, seq, std::move(label), a->kind != ActionKind::Instruction && a->kind != ActionKind::Transfer,
                    a->kind});
        };
        auto sequence = [&](SctSequenceId id) {
            const auto *seq = d.sequence(id);
            require(seq && seq->order, "Missing outline sequence");
            out.script = seq->script;
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
            append({id, c->preparation, "Condition", true});
        };
        std::visit(
            [&](auto id) {
                using T = decltype(id);
                if constexpr (std::is_same_v<T, SctScriptId>) {
                    auto *script = d.lookup(id.value, EntityKind::Script, d.scripts);
                    require(script != nullptr, "Missing outline script");
                    out.script = id;
                    out.total = script->sections.size() + script->supplementaryText.size();
                    auto total = out.total;
                    out.total = request.offset;
                    for (auto at = request.offset; at < std::min(total, request.offset + request.limit); ++at) {
                        stopped(stop);
                        if (at < script->sections.size()) {
                            auto *s = d.lookup(script->sections[at].value, EntityKind::Section, d.sections);
                            require(s != nullptr, "Missing outline section");
                            append({s->id, s->body, s->name, s->body.valid()});
                        } else {
                            auto *t = d.lookup(script->supplementaryText[at - script->sections.size()].value,
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
                } else if constexpr (std::is_same_v<T, SectionId>) {
                    const auto *s = d.lookup(id.value, EntityKind::Section, d.sections);
                    require(s != nullptr, "Missing outline section");
                    out.script = s->script;
                    if (s->body.valid())
                        sequence(s->body);
                } else if constexpr (std::is_same_v<T, SctSequenceId>)
                    sequence(id);
                else if constexpr (std::is_same_v<T, SctActionId>) {
                    const auto *a = d.action(id);
                    require(a != nullptr, "Missing outline construct");
                    out.script = d.sequence(a->owner)->script;
                    if (a->kind == ActionKind::Conditional || a->kind == ActionKind::Switch ||
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
                            append({arm.id, arm.body, std::move(label), true});
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
                return out;
            }
        }
        throw std::runtime_error("Navigation ancestry is cyclic or too deep");
    });
}
