// Shared source guards become independent route conditions. Old physical checks
// are provenance; samples only retain their evaluation phase for the compiler.
void SemanticTransaction::promoteDispatch(const PromoteDispatch &request) {
    using namespace dispatch_detail;
    const auto *header = d.action(request.header);
    require(header && header->kind == ActionKind::Conditional, "Select a structured If region to promote");
    const auto script = d.sequence(header->owner)->script;
    struct Leaf {
        Arm arm;
        Code predicate;
        std::vector<ConditionId> guards;
    };
    std::vector<Leaf> leaves;
    std::vector<SctActionId> guards;
    std::vector<Arm> oldArms;
    std::vector<std::pair<ConditionId, std::uint32_t>> phases;
    Dispatch dispatch;
    dispatch.owner = request.header;
    auto source = [&](ConditionId id) -> Code {
        auto *c = d.lookup(id.value, EntityKind::Condition, d.conditions);
        require(c, "Missing source condition");
        require(
            d.sequence(c->preparation)->order->empty(),
            "Keep instruction preparation explicit; this promotion accepts pure guards with empty preparation");
        auto tree = parse(expression(d, c->result));
        return tree->code;
    };
    auto phase = [&](ConditionId id) {
        if (std::ranges::none_of(phases, [&](auto item) { return item.first == id; }))
            phases.push_back({id, count32(leaves.size())});
    };
    std::function<void(SctActionId, Code, std::vector<ConditionId>, bool)> recover;
    recover = [&](SctActionId id, Code prefix, std::vector<ConditionId> inherited, bool sibling) {
        auto *a = d.action(id);
        require(a && a->kind == ActionKind::Conditional, "Dispatch recovery needs structured conditional guards");
        require(guards.size() < 32, "Dispatch recognition is limited to 32 source guards per region");
        guards.push_back(id);
        std::vector<Arm> arms(d.arms.begin() + a->firstArm, d.arms.begin() + a->firstArm + a->armCount);
        oldArms.insert(oldArms.end(), arms.begin(), arms.end());
        if (arms.size() == 1) {
            auto *body = d.sequence(arms.front().body);
            bool nested = !body->order->empty() && std::ranges::all_of(*body->order, [&](auto child) {
                return d.action(child)->kind == ActionKind::Conditional;
            });
            if (nested) {
                auto check = arms.front().condition;
                auto combined = conjunction(prefix, source(check));
                phase(check);
                inherited.push_back(check);
                const auto children = *body->order;
                for (auto child : children)
                    recover(child, combined, inherited, sibling || children.size() > 1);
                return;
            }
        }
        require(!sibling || arms.size() == 1,
                "Keep mixed independent/first-match groups explicit; select one shared group");
        if (arms.size() > 1)
            dispatch.mode = DispatchMode::FirstMatch;
        for (auto arm : arms) {
            auto predicate = prefix;
            auto checks = inherited;
            if (arm.condition.valid()) {
                predicate = conjunction(predicate, source(arm.condition));
                phase(arm.condition);
                checks.push_back(arm.condition);
            }
            if (predicate.empty())
                predicate = parse(*SctExpressionLanguage::parse("1").expression)->code;
            leaves.push_back({arm, std::move(predicate), std::move(checks)});
        }
    };
    recover(request.header, {}, {}, false);
    require(!leaves.empty() && leaves.size() <= 16, "Dispatch needs 1 to 16 selected route bodies");
    std::set<SctSequenceId> bodies;
    std::set<ArmId> keptArms;
    for (auto &leaf : leaves) {
        bodies.insert(leaf.arm.body);
        keptArms.insert(leaf.arm.id);
    }
    // Do not invent an entry into a duplicated dispatch continuation. Ordinary
    // calls to cutscene modules outside this region stay unchanged.
    for (auto guard : guards)
        if (guard != request.header)
            require(incoming(d, guard).empty(),
                    "A guard has an external execution entry; keep that region explicit");
    for (auto body : bodies) {
        std::set<SctActionId> local;
        std::vector<SctActionId> pending(*d.sequence(body)->order);
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!local.insert(id).second)
                continue;
            for (auto seq : children(id))
                for (auto child : *d.sequence(seq)->order)
                    pending.push_back(child);
        }
        for (auto id : local)
            for (auto use : incoming(d, id))
                require(local.contains(use.site.action),
                        "Route bodies with incoming cross-boundary jumps must remain explicit");
    }
    std::map<ConditionId, std::vector<ConditionId>> samples;
    for (auto [original, before] : phases) {
        auto tree = parse(expression(d, d.conditions[at(original.value, EntityKind::Condition)].result));
        atoms(*tree, [&](const Code &code) {
            auto id = condition(request.header, script, normalizeExpression(d, expression(code)));
            samples[original].push_back(id);
            dispatch.samples.push_back({id, before});
        });
    }
    require(dispatch.samples.size() <= 32, "Dispatch has too many evaluation samples; select a smaller region");
    for (auto &leaf : leaves) {
        auto &arm = leaf.arm;
        auto result = normalizeExpression(d, expression(leaf.predicate));
        if (arm.condition.valid()) {
            auto index = at(arm.condition.value, EntityKind::Condition);
            auto &c = touch(p.conditions, index, d.conditions[index]);
            c.owner = request.header;
            c.result = result;
        } else
            arm.condition = condition(request.header, script, result, arm.id);
        p.receipt.conditions.push_back(arm.condition);
        arm.owner = request.header;
        DispatchRoute route;
        route.arm = arm.id;
        for (auto source : leaf.guards)
            route.samples.insert(route.samples.end(), samples[source].begin(), samples[source].end());
        dispatch.routes.push_back(std::move(route));
    }
    // Remove consumed control structure without deleting the retained route bodies.
    for (const auto &arm : oldArms)
        if (!keptArms.contains(arm.id)) {
            if (arm.condition.valid())
                eraseCondition(arm.condition);
            putOrder(arm.body, {});
            eraseSequence(arm.body);
            locate(arm.id.value, {});
            p.receipt.arms.push_back(arm.id);
        }
    for (auto id : guards)
        if (id != request.header) {
            editAction(id) = {};
            locate(id.value, {});
        }
    auto &root = editAction(request.header);
    root.kind = ActionKind::Dispatch;
    root.value = {};
    root.firstArm = count32(d.arms.size());
    root.armCount = count32(leaves.size());
    for (auto &leaf : leaves) {
        auto index = count32(d.arms.size());
        d.arms.push_back(leaf.arm);
        locate(leaf.arm.id.value, {EntityKind::Arm, index});
        p.receipt.arms.push_back(leaf.arm.id);
    }
    root.detail = count32(d.dispatches.size());
    d.dispatches.push_back({});
    p.dispatches.push_back({root.detail, {}});
    d.dispatches[root.detail] = std::move(dispatch);
    std::set<SctConnectionId> bound;
    for (auto [body, id] : request.bindings) {
        require(bound.insert(id).second, "Connection is bound twice");
        auto leaf = std::ranges::find_if(leaves, [&](auto &row) { return row.arm.body == body; });
        require(leaf != leaves.end(), "Binding needs a recovered route body");
        auto *existing = d.lookup(id.value, EntityKind::Connection, d.metadata.connections);
        require(existing && existing->cutscene, "Binding needs an existing cutscene connection");
        auto connection = *existing;
        connection.cutscene->dispatchArm = leaf->arm.id;
        auto requirement = RouteRequirement{
            {request.header, {}, leaf->arm.id, 0, leaf->arm.condition}, RequirementMatch::True, {}};
        if (connection.cutscene->paths.empty())
            connection.cutscene->paths.push_back({});
        for (auto &path : connection.cutscene->paths) {
            std::erase_if(path.requirements,
                          [&](const auto &q) { return std::ranges::find(guards, q.site.action) != guards.end(); });
            path.requirements.push_back(requirement);
        }
        connection.evidence.push_back(
            {SctEvidenceKind::Inference,
             "Recovered route-owned predicate from shared pure guards; original evaluation phases retained"});
        put(std::move(connection));
    }
    validateDispatch(d, d.dispatches[root.detail]);
    Annotation note{request.header};
    if (auto old = std::ranges::find(d.metadata.annotations, Subject{request.header}, &Annotation::subject);
        old != d.metadata.annotations.end())
        note = *old;
    note.evidence.push_back({SctEvidenceKind::Inference,
                             "Promoted shared pure guards to independently owned route predicates; retained "
                             "source read phases and route priority without temporary game flags"});
    apply(SemanticOperation{PutAnnotation{std::move(note)}});
    p.receipt.metadata.push_back(request.header.value);
}
void SemanticTransaction::routeRequirement(const SetRouteRequirement &request) {
    auto *arm = d.lookup(request.route.value, EntityKind::Arm, d.arms);
    require(arm && d.action(arm->owner)->kind == ActionKind::Dispatch, "Select a dispatch route");
    auto value = importValue(d, request.value);
    dispatch_detail::parse(dispatch_detail::expression(d, value));
    collect(arm->owner);
    auto index = at(arm->condition.value, EntityKind::Condition);
    touch(p.conditions, index, d.conditions[index]).result = value;
    p.receipt.conditions.push_back(arm->condition);
}
