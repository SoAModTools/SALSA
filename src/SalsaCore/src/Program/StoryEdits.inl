void SemanticTransaction::storyEdit(const editor::StoryEdit &edit) {
    namespace ed = editor;
    auto run = [&](SemanticOperation op) {
        capture(op);
        apply(op);
    };
    std::visit(
        [&](const auto &e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ed::BindRouteRequirement>) {
                auto c = d.metadata.connections[at(e.connection.value, EntityKind::Connection)];
                auto arm = d.arms[at(e.route.value, EntityKind::Arm)];
                require(d.action(arm.owner)->kind == ActionKind::Dispatch, "Select a route-owned condition");
                if (!c.cutscene)
                    c.cutscene = RouteDefinition{};
                c.cutscene->dispatchArm = e.route;
                put(c);
            } else if constexpr (std::is_same_v<T, ed::WriteMeaning>)
                put(e.meaning);
            else if constexpr (std::is_same_v<T, ed::RemoveMeaning>)
                eraseMetadata<VariableMeaning>(e.id);
            else if constexpr (std::is_same_v<T, ed::WriteNarrative>) {
                std::set<SctModuleId> seen;
                for (auto id : e.modules) {
                    at(id.value, EntityKind::Module);
                    require(seen.insert(id).second, "Repeated narrative module");
                }
                if (!p.narrative)
                    p.narrative = d.metadata.narrative;
                d.metadata.narrative = e.modules;
                p.receipt.presentationChanged = true;
            } else if constexpr (std::is_same_v<T, ed::DecideInterpretation>) {
                auto record = e.record;
                require(!record.explanation.empty(), "Explain the interpretation decision");
                require(static_cast<unsigned>(record.decision) <= 2, "Invalid interpretation decision");
                std::visit(
                    [&](auto id) {
                        require(d.directory.find(id.value).kind != EntityKind::None,
                                "Missing interpretation subject");
                    },
                    record.subject);
                record.revision = d.revision;
                if (!p.interpretations)
                    p.interpretations = d.metadata.interpretations;
                d.metadata.interpretations.push_back(std::move(record));
                p.receipt.presentationChanged = true;
            } else if constexpr (std::is_same_v<T, ed::WriteSlot>) {
                auto &s = e.slot;
                put(Port{s.id, s.owner, s.name, s.action, s.sequence, s.role, s.boundary, s.operand});
            } else if constexpr (std::is_same_v<T, ed::RemoveSlot>)
                eraseMetadata<Port>(e.id);
            else if constexpr (std::is_same_v<T, ed::DetachRoute>)
                eraseMetadata<Connection>(e.id);
            else if constexpr (std::is_same_v<T, ed::RecoverDispatch>)
                run(PromoteDispatch{e.header});
            else if constexpr (std::is_same_v<T, ed::WriteRouteRequirement>) {
                auto v = ValueBuffer::expression(e.expression);
                require(bool(v), "Invalid route requirement");
                run(SetRouteRequirement{e.route, std::move(v).takeValue()});
            } else if constexpr (std::is_same_v<T, ed::ConnectRoute>) {
                Connection c;
                if (e.route.id.valid())
                    c = d.metadata.connections[at(e.route.id.value, EntityKind::Connection)];
                c.id = e.route.id;
                c.source = e.route.source ? SctAuthoringReference<SctPortId>{*e.route.source}
                                          : SctUnresolvedBinding{"", "Choose exit slot", {}};
                c.destination = e.route.destination
                                    ? SctAuthoringReference<SctPortId>{*e.route.destination}
                                    : SctUnresolvedBinding{"", "No destination slot selected", {}};
                auto route = c.cutscene.value_or(RouteDefinition{});
                route.method = e.route.method;
                route.arrival = e.route.arrival;
                c.cutscene = route;
                c.evidence.push_back({SctEvidenceKind::UserAssertion, "Connected through route review"});
                ConnectSlots op{c, e.route.outcome};
                if (e.schedule) {
                    auto v = ValueBuffer::expression(*e.schedule);
                    require(bool(v), "Invalid schedule");
                    op.schedule = std::move(v).takeValue();
                }
                run(std::move(op));
            }
        },
        edit);
}
