// A connection command changes ordinary executable IR in the same journal entry.
// Metadata records the chosen interface and installed action; export never replays it.
void SemanticTransaction::parameter(SctActionId id, spice::sct::SctParameterAddress address, ValueId value) {
    auto &a = editAction(id);
    require(a.kind == ActionKind::Instruction, "Route operand needs an instruction");
    auto root = *d.values.find(a.value.value);
    auto span = d.child(a.value);
    std::vector<ValueId> children(span.begin(), span.end());
    std::uint32_t group = 0;
    bool found = false;
    auto replace = [&](ValueId &id) {
        auto v = *d.values.find(id.value);
        if (v.kind == ValueKind::Parameter && v.data == address.schemaIndex) {
            id = d.add(v.kind, v.data, v.detail, std::span(&value, 1));
            found = true;
        }
    };
    for (auto &child : children) {
        auto v = *d.values.find(child.value);
        if (v.kind == ValueKind::Parameter && !address.repeatedGroupOrdinal)
            replace(child);
        if (v.kind == ValueKind::RepeatedGroup) {
            if (address.repeatedGroupOrdinal == group) {
                auto ps = d.child(child);
                std::vector<ValueId> changed(ps.begin(), ps.end());
                for (auto &p : changed)
                    replace(p);
                child = d.add(v.kind, v.data, v.detail, changed);
            }
            ++group;
        }
    }
    require(found, "Route operand does not exist");
    a.value = d.add(root.kind, root.data, root.detail, children);
}
void SemanticTransaction::connect(const ConnectSlots &edit) {
    auto connection = edit.connection;
    auto *sourceId = std::get_if<SctPortId>(&connection.source);
    require(sourceId, "Connect needs a resolved source slot");
    auto *src = d.lookup(sourceId->value, EntityKind::Port, d.metadata.ports);
    require(src && src->role == SlotRole::Exit, "Select an exit slot");
    const auto source = *src;
    const auto sourceScript = slotScript(d, source);
    auto route = connection.cutscene.value_or(RouteDefinition{});
    route.outcome = edit.outcome;
    // Do not trust a caller-provided implementation anchor. Reconnect only our stored one.
    route.implementation.reset();
    route.boundTarget.reset();
    route.targetOperand.reset();
    route.boundFilename.clear();
    if (connection.id.valid()) {
        auto *old = d.lookup(connection.id.value, EntityKind::Connection, d.metadata.connections);
        require(old && std::get_if<SctPortId>(&old->source) && std::get<SctPortId>(old->source) == *sourceId,
                "Reconnect must keep its source slot");
        if (old->cutscene)
            route.implementation = old->cutscene->implementation;
    }
    SctActionId target;
    SctScriptId destinationScript = sourceScript;
    if (edit.outcome == RouteOutcome::LocalTransfer || edit.outcome == RouteOutcome::ScheduledCall ||
        edit.outcome == RouteOutcome::ScriptTransition) {
        auto *destinationId = std::get_if<SctPortId>(&connection.destination);
        require(destinationId, "Connect needs a resolved destination slot");
        auto *destination = d.lookup(destinationId->value, EntityKind::Port, d.metadata.ports);
        require(destination && destination->role == SlotRole::Entry, "Select an entry slot");
        destinationScript = slotScript(d, *destination);
        target = slotEntry(d, *destination);
        require(target.valid(), "Selected entry has no body action yet");
    } else if (edit.outcome == RouteOutcome::ResumeLoop) {
        for (auto &e : d.metadata.entrypoints)
            if (e.script == sourceScript && e.required() && e.name == "loop") {
                auto *s = d.sequence(e.body);
                if (!s->order->empty())
                    target = s->order->front();
            }
        require(target.valid(), "Script needs an explicit loop body");
    } else
        require(edit.outcome == RouteOutcome::ReturnToCaller, "Observation-only routes do not edit execution");
    require(edit.outcome == RouteOutcome::ScriptTransition || destinationScript == sourceScript,
            "Local transfers cannot cross script files");
    require(edit.outcome == RouteOutcome::ScheduledCall || !edit.schedule, "Schedule belongs to a scheduled call");
    if (edit.outcome == RouteOutcome::ScheduledCall)
        require(edit.schedule.has_value(), "Scheduled call needs an explicit schedule expression");
    SctActionId implementation;
    if (route.implementation) {
        require(slotImplementation(d, source, *route.implementation),
                "Route action no longer occupies its selected boundary; select the boundary again explicitly");
        implementation = *route.implementation;
    } else if (source.boundary == SlotBoundary::Operand || source.boundary == SlotBoundary::Terminal) {
        require(source.action.has_value(), "Exit needs a source action");
        implementation = *source.action;
    } else {
        auto seq = source.sequence;
        std::size_t position = 0;
        if (source.boundary == SlotBoundary::Fallthrough) {
            require(source.action.has_value(), "Fallthrough needs a source action");
            auto *a = d.action(*source.action);
            require(a, "Missing source action");
            // Jump/return/switch have no physical fallthrough to replace.
            if (a->kind == ActionKind::Instruction) {
                auto op = d.values.find(a->value.value)->data;
                require(op != 10 && op != 12 && op != 3, "Source has no physical fallthrough");
            }
            seq = a->owner;
            auto ids = order(*seq);
            position = std::ranges::find(ids, *source.action) - ids.begin() + 1;
        } else {
            require(seq.has_value(), "Sequence-end exit needs a sequence");
            position = order(*seq).size();
        }
        implementation = create(*seq, position, ActionKind::Instruction);
    }
    auto *current = d.action(implementation);
    require(current && current->kind == ActionKind::Instruction &&
                d.sequence(current->owner)->script == sourceScript,
            "Invalid route implementation");
    const auto oldValue = current->value;
    const auto *old = oldValue.valid() ? d.values.find(oldValue.value) : nullptr;
    if (edit.outcome == RouteOutcome::ScriptTransition) {
        require(source.boundary == SlotBoundary::Operand &&
                    source.operand == spice::sct::SctParameterAddress{0, {}},
                "Script transition selects the filename operand of an existing load");
        require(old && old->data == 43, "Only literal load opcode 43 is authorable as a script transition");
        auto value = routeParameter(d, implementation, *source.operand);
        auto *v = d.values.find(value.value);
        require(v && v->kind == ValueKind::SupplementaryTextReference, "Load needs a literal footer filename");
        auto *previous = d.lookup(v->data, EntityKind::Text, d.texts);
        require(previous && d.values.find(previous->value.value)->kind == ValueKind::PlainText,
                "Load filename must be plain text");
        require(route.arrival.has_value(), "Select an existing destination arrival handler");
        auto *entry = d.lookup(route.arrival->value, EntityKind::Entry, d.metadata.entrypoints);
        require(entry && entry->script == destinationScript && entry->kind == SctEntrypointKind::Arrival &&
                    entry->responseSite && entry->responseSite->parameter,
                "Arrival needs an explicit response operand");
        const auto response = *entry->responseSite;
        auto responseValue = routeParameter(d, response.action, *response.parameter);
        auto *responseRef = d.values.find(responseValue.value);
        require(responseRef && responseRef->kind == ValueKind::ActionReference,
                "Arrival response must target an action");
        const auto &script = d.scripts[at(destinationScript.value, EntityKind::Script)];
        const auto *baseline = d.lookup(script.context.baseline.value, EntityKind::Baseline, d.baselines);
        require(baseline, "Missing destination baseline");
        auto path = baseline->source.locator.identityKey();
        auto filename = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        const auto *oldText = d.values.find(previous->value.value);
        if (std::string_view(d.characters.data() + oldText->data, oldText->detail) != filename) {
            auto text = SctTextResourceId{d.allocate()};
            auto index = count32(d.texts.size());
            d.texts.push_back({});
            p.texts.push_back({index, {}});
            locate(text.value, {EntityKind::Text, index});
            d.texts[index] = {text, sourceScript, d.string(ValueKind::PlainText, filename),
                              spice::sct::SctTextKind::PlainString, true};
            auto scriptIndex = at(sourceScript.value, EntityKind::Script);
            touch(p.scripts, scriptIndex, d.scripts[scriptIndex]).supplementaryText.push_back(text);
            parameter(implementation, *source.operand, d.add(ValueKind::SupplementaryTextReference, text.value));
        }
        parameter(response.action, *response.parameter, d.add(ValueKind::ActionReference, target.value));
        // Conditions remain selected authored logic. A caller may edit them explicitly in the same batch.
        preserveTable<Entrypoint>();
        d.metadata.entrypoints[at(entry->id.value, EntityKind::Entry)].response = connection.destination;
        p.receipt.metadata.push_back(entry->id.value);
        route.boundFilename = filename;
        route.targetOperand = source.operand;
    } else if (source.boundary == SlotBoundary::Operand) {
        require(edit.outcome == RouteOutcome::LocalTransfer || edit.outcome == RouteOutcome::ResumeLoop,
                "Operand retargeting preserves the source instruction kind");
        require(source.operand.has_value(), "Select the exact target operand");
        auto oldRef = routeParameter(d, implementation, *source.operand);
        auto *v = d.values.find(oldRef.value);
        require(v && v->kind == ValueKind::ActionReference, "Selected operand is not an action target");
        parameter(implementation, *source.operand, d.add(ValueKind::ActionReference, target.value));
        route.targetOperand = source.operand;
    } else {
        if (source.boundary == SlotBoundary::Terminal && !route.implementation)
            require(old && (old->data == 12 || old->data == 10 || old->data == 11),
                    "Terminal replacement needs a return, jump or call");
        std::vector<ValueId> children;
        std::uint32_t modifiers = old ? old->detail : 0;
        if (edit.schedule) {
            auto schedule = importValue(d, *edit.schedule);
            require(d.values.find(schedule.value)->kind == ValueKind::Expression, "Schedule needs an expression");
            children.push_back(schedule);
            modifiers |= 2;
        } else if (old && (old->detail & 2))
            children.push_back(d.child(oldValue).front());
        if (target.valid()) {
            auto ref = d.add(ValueKind::ActionReference, target.value);
            children.push_back(d.add(ValueKind::Parameter, 0, 0, std::span(&ref, 1)));
            route.targetOperand = spice::sct::SctParameterAddress{0, {}};
        }
        auto opcode = edit.outcome == RouteOutcome::ReturnToCaller  ? 12
                      : edit.outcome == RouteOutcome::ScheduledCall ? 11
                                                                    : 10;
        editAction(implementation).value = d.add(ValueKind::Instruction, opcode, modifiers, children);
    }
    route.implementation = implementation;
    route.implementationOpcode =
        static_cast<std::uint32_t>(d.values.find(d.action(implementation)->value.value)->data);
    if (target.valid())
        route.boundTarget = target;
    connection.cutscene = std::move(route);
    put(std::move(connection));
}
