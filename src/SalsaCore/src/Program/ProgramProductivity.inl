// Scoped productivity queries on the serialized program owner.
std::future<Result<std::shared_ptr<const ProgramFragment>>>
ProgramService::copy(Revision r, editor::CopySelection request, std::stop_token stop) {
    return control_->submit<std::shared_ptr<const ProgramFragment>>(
        [r, request = std::move(request), stop](ServiceOwner &o) {
            o.match(r, stop);
            require(o.data && !request.items.empty(), "Select content to copy");
            auto &d = *o.data;
            ReadSelection selection;
            selection.referencedText = false;
            std::set<SctActionId> actions;
            std::set<SectionId> sections;
            SctScriptId script;
            for (auto subject : request.items) {
                if (auto id = std::get_if<SctActionId>(&subject)) {
                    require(d.action(*id), "Missing selected action");
                    actions.insert(*id);
                } else if (auto id = std::get_if<SectionId>(&subject)) {
                    require(d.lookup(id->value, EntityKind::Section, d.sections), "Missing selected section");
                    sections.insert(*id);
                } else if (auto id = std::get_if<ArmId>(&subject)) {
                    auto *arm = d.lookup(id->value, EntityKind::Arm, d.arms);
                    require(arm && d.action(arm->owner)->kind == ActionKind::Conditional &&
                                std::ranges::find(request.items, EditorSubject(arm->owner)) != request.items.end(),
                            "Select the If header to copy the complete chain; use Edit branches for individual branches");
                    // The flattened branch is already included by its If header.
                } else
                    throw std::runtime_error("Select actions, complete constructs, or code sections to copy");
            }
            require(actions.empty() || sections.empty(),
                    "Copy actions or sections together, not a mixed selection");
            auto out = std::make_shared<ProgramFragment>();
            out->project = r.project;
            if (!sections.empty()) {
                script = d.sections[d.directory.find(sections.begin()->value).index].script;
                for (auto id : sections) {
                    const auto &s = d.sections[d.directory.find(id.value).index];
                    require(s.script == script && s.body.valid(), "Select code sections in one script");
                }
                for (auto id : d.scripts[d.directory.find(script.value).index].sections)
                    if (sections.contains(id))
                        selection.sections.push_back(id);
            } else {
                // Selecting a construct includes its descendants once, even in extended selection.
                auto roots = actions;
                for (auto id : actions) {
                    auto *seq = d.sequence(d.action(id)->owner);
                    while (seq->owner) {
                        auto parent = d.parentAction(*seq);
                        if (actions.contains(parent)) {
                            roots.erase(id);
                            break;
                        }
                        seq = d.sequence(d.action(parent)->owner);
                    }
                }
                require(!roots.empty(), "Empty action selection");
                auto *seq = d.sequence(d.action(*roots.begin())->owner);
                script = seq->script;
                for (auto id : roots)
                    require(d.action(id)->owner == seq->id, "Select sibling items within one body");
                for (auto id : *seq->order)
                    if (roots.contains(id))
                        out->roots.push_back(id);
                selection.actions = out->roots;
            }
            out->script = script;
            out->content = *o.read(r, selection, stop);
            for (const auto &block : d.metadata.localization.blocks)
                require(std::ranges::find(out->content.actions, block.action, &Action::id) ==
                            out->content.actions.end(),
                        "Expand localized blocks to explicit instructions before copying them");
            out->content.scripts.clear();
            if (!out->roots.empty()) {
                const auto owner = d.action(out->roots.front())->owner;
                for (auto &seq : out->content.sequences)
                    if (seq.id == owner) {
                        seq.order = std::make_shared<const std::vector<SctActionId>>(out->roots);
                        seq.owner.reset();
                        seq.role = SequenceRole::SectionBody;
                    }
            }
            std::set<std::uint64_t> ids;
            for (auto &a : out->content.actions)
                ids.insert(a.id.value);
            for (auto &a : out->content.sections)
                ids.insert(a.id.value);
            for (auto &a : out->content.sequences)
                ids.insert(a.id.value);
            for (auto &a : out->content.arms)
                ids.insert(a.id.value);
            for (auto &a : out->content.conditions)
                ids.insert(a.id.value);
            if (!out->roots.empty())
                ids.erase(d.action(out->roots.front())->owner.value);
            for (auto &a : d.metadata.annotations)
                if (ids.contains(std::visit([](auto id) { return id.value; }, a.subject)))
                    out->annotations.push_back(a);
            return std::shared_ptr<const ProgramFragment>(std::move(out));
        });
}
std::future<Result<editor::FragmentReview>>
ProgramService::reviewPaste(Revision r, editor::Subject anchor,
                            std::shared_ptr<const ProgramFragment> fragment, std::stop_token stop) {
    return control_->submit<editor::FragmentReview>([r, anchor, fragment = std::move(fragment),
                                                     stop](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data && fragment, "No fragment or program");
        auto &d = *o.data;
        auto [script, sequence, position] = fragmentDestination(d, anchor);
        (void)sequence;
        (void)position;
        editor::FragmentReview result{r};
        std::set<editor::Subject> dependencies;
        std::set<std::uint64_t> internal;
        for (auto &a : fragment->content.actions)
            internal.insert(a.id.value);
        for (auto &v : fragment->content.payload.values) {
            if (v.kind == ValueKind::ActionReference && !internal.contains(v.data))
                dependencies.insert(SctActionId{v.data});
            if (v.kind == ValueKind::IndexedTextReference || v.kind == ValueKind::SupplementaryTextReference)
                dependencies.insert(SctTextResourceId{v.data});
        }
        for (auto &t : fragment->content.transfers)
            if (!internal.contains(t.target.value))
                dependencies.insert(t.target);
        for (auto source : dependencies) {
            editor::FragmentBinding binding{
                source, std::holds_alternative<SctActionId>(source) ? "External action" : "Shared text"};
            binding.label += " " + std::to_string(std::visit([](auto id) { return id.value; }, source));
            if (fragment->project == r.project && fragment->script == script) {
                if (auto id = std::get_if<SctActionId>(&source);
                    id && d.action(*id) && d.sequence(d.action(*id)->owner)->script == script)
                    binding.target = source;
                if (auto id = std::get_if<SctTextResourceId>(&source); id) {
                    auto *t = d.lookup(id->value, EntityKind::Text, d.texts);
                    if (t && t->script == script)
                        binding.target = source;
                }
            }
            result.bindings.push_back(std::move(binding));
        }
        return result;
    });
}
std::future<Result<editor::Organization>> ProgramService::organization(Revision r, SctScriptId script,
                                                                       std::stop_token stop) {
    return control_->submit<editor::Organization>([r, script, stop](ServiceOwner &o) {
        o.match(r, stop);
        require(o.data != nullptr, "No program");
        auto &d = *o.data;
        editor::Organization result{r, script};
        if (script.valid()) {
            auto *s = d.lookup(script.value, EntityKind::Script, d.scripts);
            require(s, "Missing script");
            for (auto id : s->sections)
                result.sections.push_back({id, d.sections[d.directory.find(id.value).index].name});
        }
        std::map<std::uint32_t, std::uint32_t> folders;
        for (std::uint32_t i = 0; i < d.metadata.folders.size(); ++i)
            if (d.metadata.folders[i].script == script) {
                auto row = d.metadata.folders[i];
                folders[i] = count32(result.folders.size());
                if (row.parent)
                    row.parent = folders.at(*row.parent);
                result.folders.push_back(std::move(row));
            }
        for (const auto &a : d.metadata.annotations)
            result.notes.push_back({a.subject, a.alias, a.note, a.bookmark, a.color});
        result.opcodes = d.metadata.opcodes;
        return result;
    });
}
