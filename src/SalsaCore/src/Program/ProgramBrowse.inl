// Included by ProgramService.cpp within the program namespace.
std::future<Result<FlowAssessment>> ProgramService::assessFlow(Revision r, SctActionId id, std::stop_token stop) {
    return control_->submit<FlowAssessment>([r, id, stop](auto& owner) {
        owner.match(r, stop);
        require(owner.data != nullptr, "No project");
        auto* action = owner.data->action(id); require(action, "Missing action");
        const auto& grouping = owner.flows(action->owner, stop);
        FlowAssessment result;
        if (auto issue = grouping.issues.find(id); issue != grouping.issues.end()) result.issue = issue->second;
        return result;
    });
}
std::future<Result<editor::BrowsePage>> ProgramService::browse(Revision r, editor::BrowseRequest request,
                                                               std::stop_token stop) {
    return control_->submit<editor::BrowsePage>([r, request, stop](ServiceOwner &o) -> editor::BrowsePage {
        o.match(r, stop);
        require(o.data != nullptr, "No project");
        auto &d = *o.data;
        require(request.limit && request.limit <= 512, "Invalid browse page size");
        editor::BrowsePage out{r};
        auto append = [&](EditorSubject id, auto label) {
            if (out.total++ >= request.offset && out.rows.size() < request.limit)
                out.rows.push_back({id, label()});
        };
        auto label = [&](SctActionId id) {
            auto *a = d.action(id);
            const auto *seq = d.sequence(a->owner);
            auto *script = d.lookup(seq->script.value, EntityKind::Script, d.scripts);
            std::string name;
            if (a->kind == ActionKind::Instruction) {
                const auto *v = d.values.find(a->value.value);
                if (const auto *schema = spice::sct::findSctOpcodeSchema(static_cast<std::uint16_t>(v->data)))
                    name = std::string(schema->semantic.mnemonic);
            } else if (a->kind == ActionKind::Loop)
                name = "Loop";
            else if (a->kind == ActionKind::Switch)
                name = "Switch";
            else if (a->kind == ActionKind::Conditional)
                name = "If / Else If / Else";
            else if (a->kind == ActionKind::Transfer)
                name = "Break / Continue";
            else
                name = "Dispatch";
            return script->context.name + " — " + name + " (action " + std::to_string(id.value) + ")";
        };
        if(request.kind==editor::BrowseKind::Containers) {
            for(const auto &seq:d.sequences)if(seq.id.valid() && (!request.script.valid() || seq.script==request.script)) {
                std::string name="Body "+std::to_string(seq.id.value);
                if(!seq.owner)for(const auto &section:d.sections)if(section.body==seq.id){name=section.name;break;}
                // Nested bodies remain selectable even when empty.
                if(seq.owner)name+=" ("+std::string(seq.role==SequenceRole::ConditionPreparation?"condition preparation":seq.role==SequenceRole::LoopBody?"loop body":seq.role==SequenceRole::LoopUpdate?"loop update":seq.role==SequenceRole::LoopInitialize?"loop initialization":"branch body")+")";
                append(seq.id,[&]{return name;});
            }
            return out;
        }
        if(request.kind==editor::BrowseKind::Sections) {
            auto *script=d.lookup(request.script.value,EntityKind::Script,d.scripts);require(script,"Choose a script");
            for(auto id:script->sections)append(id,[&]{return d.sections[d.directory.find(id.value).index].name;});return out;
        }
        if (request.kind == editor::BrowseKind::Texts) {
            for (const auto &t : d.texts)
                if (t.id.valid() && (!request.script.valid() || t.script == request.script))
                    append(t.id,
                           [&] { return t.sourceName.empty() ? "Text " + std::to_string(t.id.value) : t.sourceName; });
            return out;
        }
        if (request.kind == editor::BrowseKind::Variable) {
            Variable key{static_cast<Bank>(request.bank), request.number};
            for (const auto &use : program::uses(d, key)) {
                stopped(stop);
                auto *a = d.action(use.site.action);
                if (!a)
                    continue;
                if (request.script.valid() && d.sequence(a->owner)->script != request.script)
                    continue;
                append(a->id, [&] { return label(a->id); });
            }
            return out;
        }
        if (request.kind == editor::BrowseKind::Incoming && request.target &&
            std::holds_alternative<SctActionId>(*request.target)) {
            for (const auto &use : program::incoming(d, std::get<SctActionId>(*request.target))) {
                stopped(stop);
                const auto *a = d.action(use.site.action);
                if (!a)
                    continue;
                if (request.script.valid() && d.sequence(a->owner)->script != request.script)
                    continue;
                append(a->id, [&] { return label(a->id); });
            }
            return out;
        }
        // Scan only on an explicit navigator request; output remains paged.
        for (std::size_t n = 0; n < d.actions.size(); ++n) {
            stopped(stop);
            auto *a = d.actions.find(count32(n));
            if (!a || !a->id.valid())
                continue;
            if (request.script.valid() && d.sequence(a->owner)->script != request.script)
                continue;
            bool match = request.kind == editor::BrowseKind::Actions;
            if (a->kind == ActionKind::Instruction) {
                auto *root = d.values.find(a->value.value);
                if (request.kind == editor::BrowseKind::Opcodes)
                    match = root->data == request.number;
                if (request.kind == editor::BrowseKind::Incomplete || request.kind == editor::BrowseKind::Incoming) {
                    std::vector<ValueId> pending{a->value};
                    while (!pending.empty()) {
                        auto id = pending.back();
                        pending.pop_back();
                        auto *v = d.values.find(id.value);
                        if (request.kind == editor::BrowseKind::Incomplete)
                            match |= v->kind == ValueKind::Unresolved || v->kind == ValueKind::Unavailable;
                        else if (request.target)
                            std::visit(
                                [&](auto target) {
                                    using T = decltype(target);
                                    if constexpr (std::is_same_v<T, SctActionId>)
                                        match |= v->kind == ValueKind::ActionReference && v->data == target.value;
                                    if constexpr (std::is_same_v<T, SctTextResourceId>)
                                        match |= (v->kind == ValueKind::IndexedTextReference ||
                                                  v->kind == ValueKind::SupplementaryTextReference) &&
                                                 v->data == target.value;
                                },
                                *request.target);
                        for (auto child : d.child(id))
                            pending.push_back(child);
                    }
                }
            }
            if (match)
                append(a->id, [&] { return label(a->id); });
        }
        return out;
    });
}
