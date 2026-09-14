// Included by ProgramMaterializer.cpp. A decision graph retains sampled truth
// through control flow; no temporary game variables or second editable body.
void compileDispatch(const Program &p, const Action &action, Template &out) {
    using namespace dispatch_detail;
    const auto &dispatch = *p.dispatch(action.id);
    Values values(p);
    struct Node {
        Code check;
        int yes = 0, no = 0;
        SctSequenceId body;
        ArmId arm;
        ConditionId condition;
    };
    std::vector<Node> nodes(1); // zero is the region continuation
    auto add = [&](Node node) {
        require(nodes.size() < 2048, "Dispatch expansion exceeds 2048 decisions; split the region");
        nodes.push_back(std::move(node));
        return static_cast<int>(nodes.size() - 1);
    };
    std::vector<Code> samples;
    for (auto sample : dispatch.samples)
        samples.push_back(parse(values.expression(p.find(sample.condition)->result))->code);
    std::vector<std::shared_ptr<const Predicate>> predicates;
    std::vector<std::map<Code, std::size_t>> bindings;
    std::vector<std::uint32_t> needed(dispatch.routes.size() + 1);
    for (std::size_t i = 0; i < dispatch.routes.size(); ++i) {
        auto *arm = p.find(dispatch.routes[i].arm);
        predicates.push_back(parse(values.expression(p.find(arm->condition)->result)));
        std::map<Code, std::size_t> allowed;
        for (auto id : dispatch.routes[i].samples) {
            auto at = static_cast<std::size_t>(
                std::ranges::find(dispatch.samples, id, &DispatchSample::condition) - dispatch.samples.begin());
            require(at < dispatch.samples.size() && dispatch.samples[at].beforeRoute <= i,
                    "Invalid dispatch sample phase");
            // Duplicate atoms at the same source phase share one truth value.
            // Distinct phases for an identical atom are rejected on promotion.
            allowed.try_emplace(samples[at], at);
        }
        bindings.push_back({});
        atoms(*predicates.back(), [&](const Code &code) {
            if (auto at = allowed.find(code); at != allowed.end()) {
                bindings.back().insert(*at);
                needed[i] |= std::uint32_t{1} << at->second;
            }
        });
    }
    for (std::size_t i = dispatch.routes.size(); i-- > 0;)
        needed[i] |= needed[i + 1];
    using State = std::tuple<std::size_t, std::uint32_t, std::uint32_t>;
    std::map<State, int> memo;
    std::function<int(std::size_t, std::uint32_t, std::uint32_t)> route;
    route = [&](std::size_t index, std::uint32_t known, std::uint32_t truth) -> int {
        if (index == dispatch.routes.size())
            return 0;
        known &= needed[index];
        truth &= known;
        State state{index, known, truth};
        if (auto at = memo.find(state); at != memo.end())
            return at->second;
        require(memo.size() < 2048, "Dispatch expansion exceeds 2048 states; split the region");
        // Reserve before recursion to bound work as well as emitted nodes.
        memo.emplace(state, 0);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            auto bit = std::uint32_t{1} << i;
            if ((needed[index] & bit) && !(known & bit) && dispatch.samples[i].beforeRoute <= index) {
                int yes = route(index, known | bit, truth | bit), no = route(index, known | bit, truth);
                auto result = yes == no ? yes : add({samples[i], yes, no, {}, {}, dispatch.samples[i].condition});
                memo[state] = result;
                return result;
            }
        }
        const auto &r = dispatch.routes[index];
        auto *arm = p.find(r.arm);
        int next = route(index + 1, known, truth);
        int body =
            add({{}, dispatch.mode == DispatchMode::FirstMatch ? 0 : next, 0, arm->body, arm->id, arm->condition});
        std::function<int(const Predicate &, int, int)> predicate;
        predicate = [&](const Predicate &expression, int yes, int no) -> int {
            if (yes == no)
                return yes;
            if (expression.kind == Predicate::And)
                return predicate(*expression.left, predicate(*expression.right, yes, no), no);
            if (expression.kind == Predicate::Or)
                return predicate(*expression.left, yes, predicate(*expression.right, yes, no));
            if (auto at = bindings[index].find(expression.code); at != bindings[index].end()) {
                auto bit = std::uint32_t{1} << at->second;
                require((known & bit) != 0, "Missing dispatch evaluation sample");
                return truth & bit ? yes : no;
            }
            return add({expression.code, yes, no, {}, arm->id, arm->condition});
        };
        auto result = predicate(*predicates[index], body, next);
        memo[state] = result;
        return result;
    };
    const auto entry = route(0, 0, 0);
    auto label = [&](int id) {
        return id ? Label{LabelKind::Dispatch, action.id.value, static_cast<std::uint64_t>(id)}
                  : afterLabel(action.id);
    };
    auto jump = [&](int target, SctSequenceId from = {}) {
        Step s;
        s.kind = Step::Emit;
        s.subjects = {action.id};
        s.instruction = {{}, 10, true, {}, {{0, sct::SctInstructionReference{}}}};
        s.fixups.push_back({{0, {}}, label(target)});
        s.normalExitFrom = from;
        out.steps.push_back(std::move(s));
    };
    jump(entry);
    // Emit only reachable graph nodes. Constant-folded branches may leave
    // unused construction nodes; they are not executable output.
    std::set<int> live;
    std::vector<int> todo{entry};
    while (!todo.empty()) {
        auto id = todo.back();
        todo.pop_back();
        if (!id || !live.insert(id).second)
            continue;
        todo.push_back(nodes[id].yes);
        if (!nodes[id].body.valid())
            todo.push_back(nodes[id].no);
    }
    for (auto id : live) {
        const auto &node = nodes[id];
        Step mark;
        mark.label = label(id);
        out.steps.push_back(std::move(mark));
        if (node.body.valid()) {
            Step s;
            s.kind = Step::Expand;
            s.sequence = node.body;
            s.clone = true;
            out.steps.push_back(std::move(s));
            jump(node.yes, node.body);
        } else {
            Step s;
            s.kind = Step::Emit;
            s.subjects = {action.id, node.condition};
            if (node.arm.valid())
                s.subjects.push_back(node.arm);
            s.instruction = {{}, 0, true, {}, {{0, expression(node.check)}, {1, sct::SctInstructionReference{}}}};
            s.fixups.push_back({{1, {}}, label(node.no)});
            out.steps.push_back(std::move(s));
            jump(node.yes);
        }
    }
}
