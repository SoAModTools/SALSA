#include "SalsaCore/Program/ProgramService.h"
#include "DispatchPredicates.h"
#include "ExplicitFlow.h"
#include "NativeCollection.h"
#include "ProgramInternal.h"
#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SpiceSCT/SctScptEncoding.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <regex>
#include <set>
#include <thread>
#include <unordered_set>

namespace salsa::core::program {
namespace {
void stopped(std::stop_token stop) {
    require(!stop.stop_requested(), "Program operation cancelled");
}
template <class T> void unique(std::vector<T> &v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
struct CopyValues {
    const ProgramData &from;
    ValueBuffer &to;
    std::unordered_map<std::uint32_t, ValueId> seen;
    ValueId copy(ValueId id) {
        if (!id.valid())
            return {};
        if (auto at = seen.find(id.value); at != seen.end())
            return at->second;
        std::vector<std::pair<ValueId, bool>> stack{{id, false}};
        while (!stack.empty()) {
            auto [at, exit] = stack.back();
            stack.pop_back();
            if (seen.contains(at.value))
                continue;
            auto *v = from.values.find(at.value);
            require(v != nullptr, "Missing scoped value");
            if (!exit) {
                stack.push_back({at, true});
                for (auto c : from.child(at))
                    if (!seen.contains(c.value))
                        stack.push_back({c, false});
                continue;
            }
            ValueId result;
            if (v->kind == ValueKind::PlainText || v->kind == ValueKind::TextChunk)
                result = to.text(v->kind, v->detail ? std::string_view(from.characters.data() + v->data, v->detail)
                                                    : std::string_view{});
            else {
                std::vector<ValueId> children;
                for (auto c : from.child(at))
                    children.push_back(seen.at(c.value));
                result = to.add(v->kind, v->data, v->detail, children);
            }
            seen.emplace(at.value, result);
        }
        return seen.at(id.value);
    }
};
ValueId importValue(ProgramData &d, const ValueBuffer &buffer) {
    require(buffer.root.valid() && buffer.root.value < buffer.values.size(), "Missing edit payload root");
    std::vector<ValueId> ids;
    ids.reserve(buffer.values.size());
    for (const auto &v : buffer.values) {
        require(v.first <= buffer.children.size() && v.count <= buffer.children.size() - v.first,
                "Invalid edit child range");
        std::vector<ValueId> children;
        for (auto c : std::span(buffer.children).subspan(v.first, v.count)) {
            require(c.value < ids.size(), "Forward or cyclic edit value");
            children.push_back(ids[c.value]);
        }
        ValueId id;
        if (v.kind == ValueKind::PlainText || v.kind == ValueKind::TextChunk) {
            require(children.empty() && v.data <= buffer.characters.size() &&
                        v.detail <= buffer.characters.size() - v.data,
                    "Invalid edit text range");
            id = d.string(v.kind, std::string_view(buffer.characters).substr(v.data, v.detail));
        } else
            id = d.add(v.kind, v.data, v.detail, children);
        valueShape(d, id);
        ids.push_back(id);
    }
    return ids[buffer.root.value];
}
std::span<const VariableUse> uses(const ProgramData &d, Variable key) {
    if (auto it = d.editedUses.find(key); it != d.editedUses.end() && it->second)
        return *it->second;
    auto it = d.variableUses.find(key);
    return it == d.variableUses.end() ? std::span<const VariableUse>{}
                                      : std::span(d.accesses).subspan(it->second.first, it->second.count);
}
std::span<const TargetUse> incoming(const ProgramData &d, SctActionId key) {
    if (auto it = d.editedIncoming.find(key); it != d.editedIncoming.end() && it->second)
        return *it->second;
    auto at = d.directory.find(key.value);
    if (at.kind != EntityKind::Action || at.index >= d.incomingRanges.size())
        return {};
    auto r = d.incomingRanges[at.index];
    return std::span(d.targetUses).subspan(r.first, r.count);
}
std::span<const VariableUse> computed(const ProgramData &d) {
    return d.editedComputed ? std::span<const VariableUse>(*d.editedComputed)
                            : std::span<const VariableUse>(d.computed);
}
template <class T> struct RowSwap {
    std::uint32_t index;
    T other;
};
template <class T> T &touch(std::vector<RowSwap<T>> &changes, std::uint32_t at, T &row) {
    if (std::ranges::none_of(changes, [&](auto &c) { return c.index == at; }))
        changes.push_back({at, row});
    return row;
}
template <class T> void swapRows(std::vector<RowSwap<T>> &changes, std::vector<T> &rows) noexcept {
    for (auto &c : changes)
        std::swap(c.other, rows[c.index]);
}
} // namespace
ValueId ValueBuffer::add(ValueKind kind, std::uint64_t data, std::uint32_t detail, std::span<const ValueId> items) {
    for (auto id : items)
        require(id.value < values.size(), "Invalid local value handle");
    auto first = count32(children.size());
    count32(children.size() + items.size());
    children.insert(children.end(), items.begin(), items.end());
    root = ValueId{count32(values.size())};
    values.push_back({data, first, count32(items.size()), detail, kind});
    return root;
}
ValueId ValueBuffer::text(ValueKind kind, std::string_view text) {
    require(kind == ValueKind::PlainText || kind == ValueKind::TextChunk, "Not a character payload");
    auto start = count32(characters.size());
    characters.append(text);
    return add(kind, start, count32(text.size()));
}
Result<ValueBuffer> ValueBuffer::expression(const spice::sct::SctCanonicalExpression &input) {
    try {
        ProgramData d;
        auto root = normalizeExpression(d, input);
        ValueBuffer out;
        CopyValues c{d, out};
        out.root = c.copy(root);
        return Result<ValueBuffer>::success(std::move(out));
    } catch (const std::exception &e) {
        return Result<ValueBuffer>::failure(failure(e.what()));
    }
}
struct PreparedData {
    Revision expected;
    std::shared_ptr<const unsigned char> owner;
    bool used = false;
    std::vector<RowSwap<Action>> actions;
    std::vector<RowSwap<Sequence>> sequences;
    std::vector<RowSwap<Section>> sections;
    std::vector<RowSwap<Script>> scripts;
    std::vector<RowSwap<ConditionRegion>> conditions;
    std::vector<RowSwap<Text>> texts;
    std::vector<RowSwap<Annotation>> annotations;
    std::vector<RowSwap<Arm>> arms;
    std::vector<RowSwap<Loop>> loops;
    std::vector<RowSwap<Switch>> switches;
    std::vector<RowSwap<Dispatch>> dispatches;
    std::vector<RowSwap<Transfer>> transfers;
    std::optional<std::vector<Module>> modules;
    std::optional<std::vector<Entrypoint>> entries;
    std::optional<std::vector<Port>> ports;
    std::optional<std::vector<SctLanguageGroup>> languages;
    std::optional<std::vector<Connection>> connections;
    std::optional<std::vector<SctLogicalText>> logicalTexts;
    std::optional<std::vector<VariableMeaning>> meanings;
    std::optional<bool> storyEnabled;
    std::optional<editor::LocalizationState> localization;
    std::optional<std::vector<SctModuleId>> narrative;
    std::optional<std::vector<editor::InterpretationRecord>> interpretations;
    std::optional<std::vector<Annotation>> annotationTable;
    std::optional<std::vector<editor::SectionFolder>> folders;
    std::optional<std::vector<editor::OpcodeCustomization>> opcodes;
    std::optional<std::set<std::uint64_t>> protection;
    std::vector<std::pair<std::uint64_t, Location>> locations;
    std::vector<std::pair<Variable, std::shared_ptr<const std::vector<VariableUse>>>> variables;
    std::vector<std::pair<SctActionId, std::shared_ptr<const std::vector<TargetUse>>>> targets;
    std::shared_ptr<const std::vector<VariableUse>> computed;
    bool changesComputed = false;
    std::uint64_t beforeState = 0, afterState = 0;
    ChangeReceipt receipt;
    std::vector<Diagnostic> diagnostics;
    void swap(ProgramData &d) noexcept {
        for (auto &c : actions)
            std::swap(c.other, *d.actions.find(c.index));
        swapRows(sequences, d.sequences);
        swapRows(sections, d.sections);
        swapRows(scripts, d.scripts);
        swapRows(conditions, d.conditions);
        swapRows(arms, d.arms);
        swapRows(loops, d.loops);
        swapRows(switches, d.switches);
        swapRows(dispatches, d.dispatches);
        swapRows(transfers, d.transfers);
        if (modules)
            modules->swap(d.metadata.modules);
        if (entries)
            entries->swap(d.metadata.entrypoints);
        if (ports)
            ports->swap(d.metadata.ports);
        if (languages)
            languages->swap(d.metadata.languages);
        if (connections)
            connections->swap(d.metadata.connections);
        if (logicalTexts)
            logicalTexts->swap(d.metadata.logicalTexts);
        if (localization)
            std::swap(*localization, d.metadata.localization);
        if (storyEnabled)
            std::swap(*storyEnabled, d.metadata.storyEnabled);
        if (narrative) narrative->swap(d.metadata.narrative);
        if (interpretations) interpretations->swap(d.metadata.interpretations);
        if (meanings)
            meanings->swap(d.metadata.meanings);
        if (folders) folders->swap(d.metadata.folders);
        if (opcodes) opcodes->swap(d.metadata.opcodes);
        if (annotationTable)
            annotationTable->swap(d.metadata.annotations);
        swapRows(texts, d.texts);
        swapRows(annotations, d.metadata.annotations);
        for (auto &[id, other] : locations) {
            auto at = d.directory.find(id);
            d.directory.set(id, other);
            other = at;
        }
        for (auto &[key, other] : variables)
            std::swap(other, d.editedUses.find(key)->second);
        for (auto &[key, other] : targets)
            std::swap(other, d.editedIncoming.find(key)->second);
        if (changesComputed)
            std::swap(computed, d.editedComputed);
        // Clear all affected old orders before rebuilding any new order. This
        // matters when an instruction moves between two sequences in one edit.
        for (auto &c : sequences)
            if (c.other.order)
                for (auto id : *c.other.order) {
                    auto at = d.directory.find(id.value);
                    if (at.kind == EntityKind::Action)
                        d.successors[at.index] = {};
                }
        for (auto &c : sequences) {
            auto &s = d.sequences[c.index];
            if (!s.order)
                continue;
            for (std::size_t i = 0; i < s.order->size(); ++i) {
                auto at = d.directory.find((*s.order)[i].value);
                if (at.kind == EntityKind::Action)
                    d.successors[at.index] = i + 1 < s.order->size() ? (*s.order)[i + 1] : SctActionId{};
            }
        }
    }
};
// Definitions below depend on this inclusion order.
// clang-format off
#include "CutsceneInternal.inl"
#include "FeatureHelpers.inl"
#include "LocalizationHelpers.inl"
#include "SemanticEdits.inl"
#include "FragmentEdits.inl"
#include "FeatureEdits.inl"
#include "LocalizationEdits.inl"
#include "StoryEdits.inl"
#include "CutsceneEdits.inl"
#include "DispatchEdits.inl"
// clang-format on

struct ServiceOwner {
    std::map<EditorSubject,std::string> outlineAliases;
    std::map<EditorSubject,std::pair<std::optional<std::uint32_t>,bool>> outlineStyles;
    bool outlineAliasesDirty = true;
    std::unique_ptr<ProgramData> data;
    std::shared_ptr<const unsigned char> token = std::make_shared<const unsigned char>(0);
    std::stop_token shutdown;
    std::map<std::uint64_t, std::uint64_t> checkpointTokens;
    std::uint64_t savedRevision = 0;
    std::vector<std::uint32_t> freeActions, freeSequences, freeSections;
    ControlFlowIssues flowIssues;
    // Derived row offsets only, built lazily for visited semantic sequences.
    // No expressions, instructions or alternative group model are cached here.
    std::map<SctSequenceId, std::vector<std::size_t>> semanticRowOffsets;
    const std::vector<std::size_t>& rowOffsets(SctSequenceId id, std::stop_token stop) {
        if (auto found = semanticRowOffsets.find(id); found != semanticRowOffsets.end()) return found->second;
        const auto *sequence = data->sequence(id);
        require(sequence && sequence->order, "Missing outline sequence");
        std::vector<std::size_t> offsets;
        offsets.reserve(sequence->order->size() + 1);
        offsets.push_back(0);
        for (auto id : *sequence->order) {
            stopped(stop);
            const auto *action = data->action(id);
            require(action, "Missing outline action");
            offsets.push_back(offsets.back() + (action->kind == ActionKind::Conditional ? std::max(1u, action->armCount) : 1u));
        }
        return semanticRowOffsets.emplace(id, std::move(offsets)).first->second;
    }
    const ControlFlowDiagnostics& flows(SctSequenceId sequence, std::stop_token stop) {
        if (auto found = flowIssues.find(sequence); found != flowIssues.end()) return *found->second;
        auto grouping = analyzeFlowIssues(*data, sequence, stop);
        return *flowIssues.emplace(sequence, std::move(grouping)).first->second;
    }
    void refreshStringOwners(const PreparedData &patch, ChangeReceipt &receipt) {
        auto &d = *data;
        d.footerOwners.resize(d.texts.size());
        for (const auto &change : patch.actions)
            mapFooterOperands(d, change.other.value, [&](auto text, auto) {
                auto at = d.directory.find(text.value);
                if (at.kind == EntityKind::Text) d.footerOwners[at.index] = {};
                return text;
            });
        for (const auto &change : patch.actions) {
            const auto &action = *d.actions.find(change.index);
            if (action.id.valid()) mapFooterOperands(d, action.value, [&](auto text, auto) {
                d.footerOwners[d.directory.find(text.value).index] = action.id;
                return text;
            });
        }
        for (auto text : receipt.texts) {
            auto at = d.directory.find(text.value);
            if (at.kind == EntityKind::Text && d.footerOwners[at.index].valid())
                receipt.actions.push_back(d.footerOwners[at.index]);
        }
        unique(receipt.actions);
    }
    void invalidateOutline(const ChangeReceipt &receipt) {
        for (auto seq : receipt.sequences) semanticRowOffsets.erase(seq);
        for (auto id : receipt.actions) if (auto *a = data->action(id)) semanticRowOffsets.erase(a->owner);
        if (!receipt.scriptLayouts.empty()) {
            std::erase_if(semanticRowOffsets, [&](const auto &entry) {
                auto *seq = data->sequence(entry.first);
                return !seq || std::ranges::find(receipt.scriptLayouts, seq->script) != receipt.scriptLayouts.end();
            });
        }
        for (auto seq:receipt.sequences) flowIssues.erase(seq);
        for (auto id:receipt.actions) if(auto *a=data->action(id)) flowIssues.erase(a->owner);
        // A new caller can invalidate recognition in a different sequence.
        for(auto id:receipt.oldTargets) if(auto *a=data->action(id)) flowIssues.erase(a->owner);
        for(auto id:receipt.newTargets) if(auto *a=data->action(id)) flowIssues.erase(a->owner);
        if (!receipt.scriptLayouts.empty()) {
            for (auto it = flowIssues.begin(); it != flowIssues.end();) {
                auto* sequence = data->sequence(it->first);
                if (!sequence || std::ranges::find(receipt.scriptLayouts, sequence->script) != receipt.scriptLayouts.end())
                    it = flowIssues.erase(it);
                else ++it;
            }
        }
    }
    std::uint64_t generation = 0, state = 0, nextState = 0, saved = 0;
    std::vector<std::shared_ptr<PreparedData>> history;
    std::size_t cursor = 0;
    std::vector<std::weak_ptr<PreparedData>> prepared;
    std::set<std::uint64_t> protectedIds;
    std::map<SctModuleId, std::shared_ptr<const ModuleSummary>> summaries;
    std::map<SctModuleId, std::shared_ptr<const CutsceneSummary>> cutscenes;
    std::uint64_t summaryBuild = 0;
    CutsceneQuery cutscene(Revision, SctModuleId, std::stop_token);
    void invalidate(const ChangeReceipt &receipt) noexcept;
    SummaryQuery summarize(Revision, SctModuleId, std::stop_token, bool cutsceneScope=false);
    ServiceStatus status() const {
        return {
            {data ? data->id : SctAuthoringProjectId{}, data ? data->revision : 0, generation}, state, state != saved};
    }
    void match(Revision expected, std::stop_token stop = {}) const {
        stopped(stop);
        stopped(shutdown);
        require(expected == status().current, "Stale project, revision or operation generation");
    }
    void protectMetadata() {
        protectedIds.clear();
        if (!data)
            return;
        auto &m = data->metadata;
        auto protect = [&](auto id) {
            if (id.valid())
                protectedIds.insert(id.value);
        };
        for (auto &x : m.localization.blocks) {
            protect(x.action);
            protect(x.body);
        }
        for (auto &x : m.modules)
            for (auto id : x.sequences)
                protect(id);
        for (auto &e : m.entrypoints) {
            protect(e.body);
            if (e.responseSite)
                protect(e.responseSite->action);
            if (e.predicate)
                protect(e.predicate->action);
            if (auto *n = std::get_if<SctNamedActivation>(&e.activation))
                protect(n->body);
            if (auto *l = std::get_if<SctLocalActivation>(&e.activation)) {
                protect(l->source);
                protect(l->start);
                protect(l->continuation);
            }
        }
        for (auto &p : m.ports) {
            if (p.action)
                protect(*p.action);
            if (p.sequence)
                protect(*p.sequence);
        }
        for (auto &c : m.connections)
            if (c.route) {
                protect(c.route->sourceAction);
                for (auto &o : c.route->observations)
                    protect(o.action);
            }
        for (auto &c : m.connections) if (c.cutscene) {
            const auto &r = *c.cutscene;
            if (r.dispatchArm)
                protect(*r.dispatchArm);
            if(r.implementation) protect(*r.implementation);
            if(r.boundTarget) protect(*r.boundTarget);
            for (auto &path : r.paths) {
                for(auto &q : path.requirements) { protect(q.site.action); if(q.site.condition) protect(*q.site.condition); if(q.site.arm) protect(*q.site.arm); }
                for(auto id : path.preparation) protect(id);
                for(auto id : path.consumption) protect(id);
            }
        }
        for (auto &a : m.annotations)
            std::visit(protect, a.subject);
    }
    ServiceStatus adopt(Revision expected, std::shared_ptr<const Program> p, std::stop_token stop,
                        const std::function<bool()> &current = {}) {
        match(expected, stop);
        require(p && p.use_count() == 1, "Adoption needs exclusive ownership of the imported Program");
        ServiceOwner next;
        next.data = std::move(const_cast<Program *>(p.get())->data_);
        next.protectMetadata();
        outlineAliases.clear();outlineAliasesDirty=true;flowIssues.clear();semanticRowOffsets.clear();
        stopped(stop);
        stopped(shutdown);
        require(generation < UINT64_MAX && nextState < UINT64_MAX, "Program generation/history state exhausted");
        ServiceStatus result{{next.data->id, next.data->revision, generation + 1}, nextState + 1, false};
        stopped(stop);
        stopped(shutdown);
        require(!current || current(), "Native load was superseded before installation");
        data = std::move(next.data);
        flowIssues = std::move(data->initialFlowIssues);
        protectedIds = std::move(next.protectedIds);
        history.clear();
        summaries.clear();
        cutscenes.clear();
        prepared.clear();
        cursor = 0;
        checkpointTokens.clear();
        freeActions.clear();
        freeSequences.clear();
        freeSections.clear();
        savedRevision = data->revision;
        ++generation;
        state = ++nextState;
        saved = state;
        return result;
    }
    PreparedChange prepare(EditRequest, std::stop_token);
    ChangeReceipt install(PreparedChange, std::stop_token);
    ChangeReceipt travel(Revision, bool, std::stop_token);
    std::shared_ptr<const ReadScope> read(Revision, const ReadSelection &, std::stop_token);
    Checkpoint checkpoint(Revision, std::stop_token);
    Maintenance maintain(Revision, bool);
};
PreparedChange ServiceOwner::prepare(EditRequest request, std::stop_token stop) {
    match(request.expected, stop);
    require(data != nullptr, "No program is open");
    require(!request.operations.empty(), "Empty edit request");
    auto &d = *data;
    auto p = std::make_shared<PreparedData>();
    p->expected = request.expected;
    p->owner = token;
    DirectFacts old;
    std::set<SctActionId> affected;
    std::vector<TargetUse> deletedIncoming;
    auto collectOld = [&](SctActionId id) {
        if (!affected.insert(id).second)
            return;
        std::vector<Diagnostic> previousDiagnostics;
        auto f = actionFacts(d, std::span(&id, 1), previousDiagnostics);
        old.accesses.insert(old.accesses.end(), f.accesses.begin(), f.accesses.end());
        old.computed.insert(old.computed.end(), f.computed.begin(), f.computed.end());
        old.targets.insert(old.targets.end(), f.targets.begin(), f.targets.end());
    };
    auto location = [&](std::uint64_t id, EntityKind kind) {
        auto at = d.directory.find(id);
        require(at.kind == kind, "Missing or mistyped edit target");
        return at.index;
    };
    auto changeLocation = [&](std::uint64_t id, Location to) {
        d.directory.reserve(id);
        if (std::ranges::none_of(p->locations, [&](auto &c) { return c.first == id; }))
            p->locations.push_back({id, d.directory.find(id)});
        d.directory.set(id, to);
    };
    auto action = [&](SctActionId id) -> Action & {
        auto at = location(id.value, EntityKind::Action);
        collectOld(id);
        return touch(p->actions, at, *d.actions.find(at));
    };
    auto order = [&](SctSequenceId id) {
        auto at = location(id.value, EntityKind::Sequence);
        auto &s = touch(p->sequences, at, d.sequences[at]);
        return std::vector<SctActionId>(s.order ? *s.order : std::vector<SctActionId>{});
    };
    auto putOrder = [&](SctSequenceId id, std::vector<SctActionId> v) {
        if (!v.empty()) {
            auto *seq = d.sequence(id);
            if (seq->owner && std::holds_alternative<ConditionId>(*seq->owner)) {
                auto condition = std::get<ConditionId>(*seq->owner);
                auto *c = d.lookup(condition.value, EntityKind::Condition, d.conditions);
                require(!c || d.action(c->owner)->kind != ActionKind::Dispatch,
                        "Dispatch predicates are pure; place instructions in the route body");
            }
        }
        p->receipt.copiedOrderItems += v.size();
        d.sequence(id)->order = std::make_shared<const std::vector<SctActionId>>(std::move(v));
    };
    auto named = [&](SctScriptId script, const std::string &name, SectionId except = {}) {
        require(!name.empty(), "Section name is empty");
        auto at = location(script.value, EntityKind::Script);
        for (auto id : d.scripts[at].sections)
            if (id != except)
                require(d.sections[location(id.value, EntityKind::Section)].name != name,
                        "Section name already exists in script");
    };
    SemanticTransaction semantic{
        d, *p, collectOld, [&](SctActionId id) {affected.insert(id); }, changeLocation, action, order, putOrder};
    bool rolled = false;
    try {
        // Capture original contributions before any row is staged. A batch can
        // remove a target first and repair its incoming operands later.
        for (const auto &op : request.operations) {
            stopped(stop);
            stopped(shutdown);
            std::visit(
                [&](const auto &edit) {
                    using T = std::decay_t<decltype(edit)>;
                    if constexpr (std::is_same_v<T, SemanticOperation>)
                        semantic.capture(edit);
                    else if constexpr (std::is_same_v<T, ReplaceParameter> || std::is_same_v<T, ReplaceInstruction> ||
                                       std::is_same_v<T, EraseInstruction> || std::is_same_v<T, MoveInstruction>)
                        collectOld(edit.action);
                    else if constexpr (std::is_same_v<T, ReplaceCondition>)
                        collectOld(d.conditions[location(edit.condition.value, EntityKind::Condition)].owner);
                },
                op);
        }
        for (auto &op : request.operations) {
            stopped(stop);
            stopped(shutdown);
            std::visit(
                [&](auto &edit) {
                    using T = std::decay_t<decltype(edit)>;
                    if constexpr (std::is_same_v<T, SemanticOperation>)
                        semantic.apply(edit);
                    else if constexpr (std::is_same_v<T, ReplaceParameter> || std::is_same_v<T, ReplaceInstruction>) {
                        auto &a = action(edit.action);
                        require(a.kind == ActionKind::Instruction, "Primitive edit requires an instruction leaf");
                        auto value = importValue(d, edit.value);
                        if constexpr (std::is_same_v<T, ReplaceInstruction>) {
                            require(d.values.find(value.value)->kind == ValueKind::Instruction,
                                    "Replacement is not an instruction");
                            a.value = value;
                        } else {
                            auto original = *d.values.find(a.value.value);
                            auto source = d.child(a.value);
                            std::vector<ValueId> children(source.begin(), source.end());
                            bool found = false;
                            std::uint32_t group = 0;
                            auto replace = [&](ValueId &id) {
                                auto v = *d.values.find(id.value);
                                if (v.kind == ValueKind::Parameter && v.data == edit.address.schemaIndex) {
                                    if (edit.stringKind) {
                                        const auto *schema = spice::sct::findSctOpcodeSchema(static_cast<std::uint16_t>(original.data));
                                        bool stringOperand = false;
                                        if (schema) for (unsigned n = 0; n < schema->textReferenceCount; ++n)
                                            stringOperand |= schema->textReferences[n].parameterIndex == v.data &&
                                                schema->textReferences[n].storage == spice::sct::SctTextStorage::Footer;
                                        require(stringOperand, "Expected a string-valued parameter");
                                        auto kind = d.values.find(value.value)->kind;
                                        require(kind == ValueKind::PlainText || kind == ValueKind::Message, "Invalid string parameter value");
                                        auto operand = d.child(id);
                                        auto *ref = operand.size() == 1 ? d.values.find(operand.front().value) : nullptr;
                                        if (ref && ref->kind == ValueKind::SupplementaryTextReference) {
                                            auto ti = location(ref->data, EntityKind::Text);
                                            auto &text = touch(p->texts, ti, d.texts[ti]);
                                            require(text.supplementary, "Expected a string parameter");
                                            text.value = value; text.kind = *edit.stringKind;
                                            value = d.add(ValueKind::SupplementaryTextReference, text.id.value);
                                        } else {
                                            require(ref != nullptr, "Missing string operand slot");
                                            auto script = d.sequence(a.owner)->script;
                                            auto text = SctTextResourceId{d.allocate()};
                                            auto ti = count32(d.texts.size());
                                            d.texts.push_back({text, script, value, *edit.stringKind, true});
                                            p->texts.push_back({ti, {}}); changeLocation(text.value, {EntityKind::Text, ti});
                                            auto si = location(script.value, EntityKind::Script);
                                            touch(p->scripts, si, d.scripts[si]).supplementaryText.push_back(text);
                                            value = d.add(ValueKind::SupplementaryTextReference, text.value);
                                        }
                                    }
                                    id = d.add(ValueKind::Parameter, v.data, v.detail, std::span(&value, 1));
                                    valueShape(d, id);
                                    found = true;
                                }
                            };
                            for (auto &id : children) {
                                auto v = *d.values.find(id.value);
                                if (v.kind == ValueKind::Parameter && !edit.address.repeatedGroupOrdinal)
                                    replace(id);
                                if (v.kind == ValueKind::RepeatedGroup) {
                                    if (edit.address.repeatedGroupOrdinal == group) {
                                        auto source = d.child(id);
                                        std::vector<ValueId> ps(source.begin(), source.end());
                                        for (auto &x : ps)
                                            replace(x);
                                        id = d.add(v.kind, v.data, v.detail, ps);
                                    }
                                    ++group;
                                }
                            }
                            require(found, "Parameter address does not exist");
                            a.value = d.add(original.kind, original.data, original.detail, children);
                        }
                    } else if constexpr (std::is_same_v<T, ReplaceCondition>) {
                        auto *localizedCondition =
                            d.lookup(edit.condition.value, EntityKind::Condition, d.conditions);
                        require(!localizedCondition ||
                                    std::ranges::none_of(
                                        d.metadata.localization.blocks,
                                        [&](auto &b) { return b.action == localizedCondition->owner; }),
                                "Edit the localization mapping rather than a generated language selector");
                        auto at = location(edit.condition.value, EntityKind::Condition);
                        const auto *owner = d.action(d.conditions[at].owner);
                        require(owner->kind != ActionKind::Dispatch || d.conditions[at].arm.has_value(),
                                "Edit a route requirement, not its derived evaluation-timing sample");
                        collectOld(d.conditions[at].owner);
                        auto value = importValue(d, edit.value);
                        require(d.values.find(value.value)->kind == ValueKind::Expression,
                                "Condition result must be an expression");
                        if (owner->kind == ActionKind::Dispatch)
                            dispatch_detail::parse(dispatch_detail::expression(d, value));
                        touch(p->conditions, at, d.conditions[at]).result = value;
                    } else if constexpr (std::is_same_v<T, ReplaceText>) {
                        auto at = location(edit.text.value, EntityKind::Text);
                        auto value = importValue(d, edit.value);
                        auto kind = d.values.find(value.value)->kind;
                        require(kind == ValueKind::PlainText || kind == ValueKind::Message ||
                                    kind == ValueKind::EmptyIndexedText || kind == ValueKind::Unavailable,
                                "Invalid text payload");
                        touch(p->texts, at, d.texts[at]).value = value;
                    } else if constexpr (std::is_same_v<T, CreateText>) {
                        auto scriptAt=location(edit.script.value,EntityKind::Script);
                        if(!edit.supplementary)named(edit.script,edit.name);
                        auto value=importValue(d,edit.value);auto kind=d.values.find(value.value)->kind;
                        require(kind==ValueKind::PlainText || kind==ValueKind::Message || kind==ValueKind::EmptyIndexedText,"Invalid new text value");
                        auto& script=touch(p->scripts,scriptAt,d.scripts[scriptAt]);
                        auto id=SctTextResourceId{d.allocate()};auto index=count32(d.texts.size());d.texts.push_back({});p->texts.push_back({index,{}});
                        changeLocation(id.value,{EntityKind::Text,index});d.texts[index]={id,edit.script,value,edit.kind,edit.supplementary,0,edit.name};
                        if(edit.supplementary)script.supplementaryText.push_back(id);
                        else {
                            require(edit.name!="init" && edit.name!="loop","Required roots must be code sections");
                            auto section=SectionId{d.allocate()};auto at=count32(d.sections.size());d.sections.push_back({});p->sections.push_back({at,{}});changeLocation(section.value,{EntityKind::Section,at});
                            std::vector<ValueId> words;for(auto word:spice::sct::SctStringSectionContent{}.preambleWords)words.push_back(d.add(ValueKind::Word,word));
                            auto preamble=d.add(ValueKind::Words,0,0,words);auto content=d.add(ValueKind::IndexedSection,id.value,0,std::span(&preamble,1));
                            d.sections[at]={section,edit.script,edit.name,{},content};script.sections.push_back(section);
                        }
                    } else if constexpr (std::is_same_v<T, RemoveText>) {
                        auto at=location(edit.text.value,EntityKind::Text);auto text=d.texts[at];
                        require(!protectedIds.contains(edit.text.value),"Text has authored metadata references; remove those bindings first");
                        // Deletion is uncommon and must reject execution references.
                        // Inspect only instruction payloads owned by this script.
                        for(std::size_t index=0;index<d.actions.size();++index){auto* a=d.actions.find(static_cast<std::uint32_t>(index));if(!a->id.valid() || a->kind!=ActionKind::Instruction || d.sequence(a->owner)->script!=text.script)continue;
                            std::vector<ValueId> pending{a->value};while(!pending.empty()){auto id=pending.back();pending.pop_back();auto* v=d.values.find(id.value);require(!((v->kind==ValueKind::IndexedTextReference || v->kind==ValueKind::SupplementaryTextReference) && v->data==text.id.value),"Text is referenced by an instruction");auto children=d.child(id);pending.insert(pending.end(),children.begin(),children.end());}
                        }
                        auto scriptAt=location(text.script.value,EntityKind::Script);auto& script=touch(p->scripts,scriptAt,d.scripts[scriptAt]);
                        if(text.supplementary)std::erase(script.supplementaryText,text.id);
                        else {auto sections=script.sections;for(auto id:sections){auto index=location(id.value,EntityKind::Section);auto& section=d.sections[index];auto* v=d.values.find(section.content.value);if(v && v->kind==ValueKind::IndexedSection && v->data==text.id.value){require(!protectedIds.contains(id.value),"Text section has authored references");std::erase(script.sections,id);touch(p->sections,index,section)={};changeLocation(id.value,{});break;}}}
                        touch(p->texts,at,d.texts[at])={};changeLocation(text.id.value,{});
                    } else if constexpr (std::is_same_v<T, InsertInstruction>) {
                        auto v = order(edit.sequence);
                        require(edit.position <= v.size(), "Insertion position is outside sequence");
                        auto value = importValue(d, edit.value);
                        require(d.values.find(value.value)->kind == ValueKind::Instruction,
                                "Insertion needs an instruction");
                        SctActionId id{d.allocate()};
                        auto at = freeActions.empty() ? d.actions.append({}) : freeActions.back();
                        if (!freeActions.empty())
                            freeActions.pop_back();
                        d.successors.resize(d.actions.size());
                        p->actions.push_back({at, {}});
                        changeLocation(id.value, {EntityKind::Action, at});
                        *d.actions.find(at) = {id, edit.sequence, ActionKind::Instruction, value};
                        affected.insert(id);
                        v.insert(v.begin() + edit.position, id);
                        putOrder(edit.sequence, std::move(v));
                    } else if constexpr (std::is_same_v<T, EraseInstruction>) {
                        auto &a = action(edit.action);
                        require(a.kind == ActionKind::Instruction,
                                "Structured deletion is a compound authoring operation");
                        require(!protectedIds.contains(a.id.value), "Instruction is referenced by authored metadata");
                        auto refs = incoming(d, a.id);
                        deletedIncoming.insert(deletedIncoming.end(), refs.begin(), refs.end());
                        auto v = order(a.owner);
                        std::erase(v, a.id);
                        putOrder(a.owner, std::move(v));
                        changeLocation(a.id.value, {});
                        a = {};
                    } else if constexpr (std::is_same_v<T, MoveInstruction>) {
                        auto &a = action(edit.action);
                        require(a.kind == ActionKind::Instruction,
                                "Moving constructs needs the structured authoring command");
                        auto *dest = d.sequence(edit.sequence);
                        require(dest && dest->script == d.sequence(a.owner)->script,
                                "Move must stay within its imported script");
                        auto source = order(a.owner);
                        std::erase(source, a.id);
                        putOrder(a.owner, std::move(source));
                        auto v = order(edit.sequence);
                        require(edit.position <= v.size(), "Move position is outside sequence");
                        v.insert(v.begin() + edit.position, a.id);
                        a.owner = edit.sequence;
                        putOrder(edit.sequence, std::move(v));
                    } else if constexpr (std::is_same_v<T, RenameSection>) {
                        auto at = location(edit.section.value, EntityKind::Section);
                        auto &s = d.sections[at];
                        require(s.name != "init" && s.name != "loop", "Required lifecycle sections cannot be renamed");
                        require(edit.name != "init" && edit.name != "loop",
                                "Lifecycle repair requires its explicit authoring operation");
                        require(!protectedIds.contains(s.body.value),
                                "Named entry/module section needs its coordinated authoring command");
                        named(s.script, edit.name, s.id);
                        touch(p->sections, at, s).name = edit.name;
                    } else if constexpr (std::is_same_v<T, ReorderSections>) {
                        auto at = location(edit.script.value, EntityKind::Script);
                        auto old = d.scripts[at].sections;
                        auto next = edit.order;
                        std::sort(old.begin(), old.end());
                        std::sort(next.begin(), next.end());
                        require(old == next, "Section order must contain each existing section exactly once");
                        touch(p->scripts, at, d.scripts[at]).sections = edit.order;
                    } else if constexpr (std::is_same_v<T, AddSection>) {
                        named(edit.script, edit.name);
                        require(edit.name != "init" && edit.name != "loop",
                                "Lifecycle repair needs an explicit authoring command");
                        auto scriptAt = location(edit.script.value, EntityKind::Script);
                        auto &script = touch(p->scripts, scriptAt, d.scripts[scriptAt]);
                        require(edit.position <= script.sections.size(),
                                "Section insertion position is outside script");
                        SectionId id{d.allocate()};
                        SctSequenceId body{d.allocate()};
                        auto seqAt = freeSequences.empty() ? count32(d.sequences.size()) : freeSequences.back();
                        if (freeSequences.empty())
                            d.sequences.push_back({});
                        else
                            freeSequences.pop_back();
                        p->sequences.push_back({seqAt, {}});
                        changeLocation(body.value, {EntityKind::Sequence, seqAt});
                        d.sequences[seqAt] = {
                            body, edit.script, {}, std::make_shared<const std::vector<SctActionId>>()};
                        auto secAt = freeSections.empty() ? count32(d.sections.size()) : freeSections.back();
                        if (freeSections.empty())
                            d.sections.push_back({});
                        else
                            freeSections.pop_back();
                        p->sections.push_back({secAt, {}});
                        changeLocation(id.value, {EntityKind::Section, secAt});
                        d.sections[secAt] = {id, edit.script, edit.name, body};
                        script.sections.insert(script.sections.begin() + edit.position, id);
                    } else if constexpr (std::is_same_v<T, EraseEmptySection>) {
                        auto at = location(edit.section.value, EntityKind::Section);
                        auto s = d.sections[at];
                        require(s.name != "init" && s.name != "loop", "Required lifecycle sections cannot be removed");
                        require(s.body.valid() && d.sequence(s.body)->order->empty(),
                                "Only empty code sections can be removed by this primitive");
                        require(!protectedIds.contains(s.id.value) && !protectedIds.contains(s.body.value),
                                "Section has authored metadata references");
                        auto scriptAt = location(s.script.value, EntityKind::Script);
                        std::erase(touch(p->scripts, scriptAt, d.scripts[scriptAt]).sections, s.id);
                        auto seqAt = location(s.body.value, EntityKind::Sequence);
                        touch(p->sequences, seqAt, d.sequences[seqAt]) = {};
                        touch(p->sections, at, d.sections[at]) = {};
                        changeLocation(s.id.value, {});
                        changeLocation(s.body.value, {});
                    } else if constexpr (std::is_same_v<T, EditAnnotation>) {
                        require(!p->annotationTable, "Use annotation upserts consistently within a batch");
                        require(edit.index < d.metadata.annotations.size(), "Missing annotation");
                        auto &a = touch(p->annotations, count32(edit.index), d.metadata.annotations[edit.index]);
                        a.alias = edit.alias;
                        a.evidence = edit.evidence;
                        p->receipt.presentationChanged = true;
                    }
                },
                op);
        }
        semantic.finish();
        // Newly introduced operands own copies. Unchanged operands keep their IDs,
        // including ordinary parameter/timing edits and moves. Only touched actions
        // are inspected; no whole-project scan is needed for this ownership rule.
        std::set<SctTextResourceId> freshTexts, claimedFresh, oldTexts, keptTexts;
        for (const auto &change : p->texts)
            if (!change.other.id.valid()) freshTexts.insert(d.texts[change.index].id);
        for (const auto &change : p->actions) {
            std::map<spice::sct::SctParameterAddress, SctTextResourceId> previous;
            if (change.other.id.valid())
                mapFooterOperands(d, change.other.value, [&](auto id, auto address) {
                    previous.emplace(address, id); oldTexts.insert(id); return id;
                });
            auto &current = *d.actions.find(change.index);
            if (!current.id.valid()) continue;
            const auto script = d.sequence(current.owner)->script;
            current.value = mapFooterOperands(d, current.value, [&](auto id, auto address) {
                auto *text = d.lookup(id.value, EntityKind::Text, d.texts);
                require(text && text->supplementary && text->script == script, "Missing string operand");
                if ((previous.contains(address) && previous.at(address) == id) ||
                    (freshTexts.contains(id) && claimedFresh.insert(id).second)) {
                    keptTexts.insert(id); return id;
                }
                auto copy = *text;
                copy.id = SctTextResourceId{d.allocate()};
                auto index = count32(d.texts.size());
                d.texts.push_back(copy); p->texts.push_back({index, {}});
                changeLocation(copy.id.value, {EntityKind::Text, index});
                auto si = location(script.value, EntityKind::Script);
                touch(p->scripts, si, d.scripts[si]).supplementaryText.push_back(copy.id);
                keptTexts.insert(copy.id);
                return copy.id;
            });
        }
        // Retired instruction-owned strings disappear with their operands. Keep
        // explicitly annotated resources available to inspection until unbound.
        for (auto id : oldTexts) if (!keptTexts.contains(id) && !protectedIds.contains(id.value)) {
            auto index = location(id.value, EntityKind::Text);
            auto script = d.texts[index].script;
            auto si = location(script.value, EntityKind::Script);
            std::erase(touch(p->scripts, si, d.scripts[si]).supplementaryText, id);
            touch(p->texts, index, d.texts[index]) = {};
            changeLocation(id.value, {});
        }
        if (semantic.changed) {
            finishMetadata(d, p->diagnostics, false, false);
            auto previous = std::move(protectedIds);
            try {
                protectMetadata();
                p->protection = std::move(protectedIds);
                protectedIds = std::move(previous);
            } catch (...) {
                protectedIds = std::move(previous);
                throw;
            }
        }
        std::vector<SctActionId> ids(affected.begin(), affected.end());
        auto now = actionFacts(d, ids, p->diagnostics);
        for (auto use : deletedIncoming)
            require(affected.contains(use.site.action), "Instruction still has incoming execution references");
        for (auto &c : p->actions)
            if (!semantic.changed && c.other.id.valid() && protectedIds.contains(c.other.id.value)) {
                auto *a = d.action(c.other.id);
                require(a, "Metadata target was removed");
                auto addresses = [&](ValueId root) {
                    std::set<spice::sct::SctParameterAddress> result;
                    std::uint32_t group = 0;
                    for (auto id : d.child(root)) {
                        auto *v = d.values.find(id.value);
                        if (v->kind == ValueKind::Parameter)
                            result.insert({static_cast<std::uint32_t>(v->data), {}});
                        if (v->kind == ValueKind::RepeatedGroup) {
                            for (auto param : d.child(id))
                                result.insert({static_cast<std::uint32_t>(d.values.find(param.value)->data), group});
                            ++group;
                        }
                    }
                    return result;
                };
                if (a->kind == ActionKind::Instruction)
                    require(addresses(c.other.value) == addresses(a->value),
                            "Metadata operand layout requires coordinated editing");
            }
        // Build only the old/new dependency buckets. Preparation temporarily owns
        // these unpublished row changes; rollback restores them before returning.
        std::set<Variable> variables;
        std::set<SctActionId> targets;
        for (auto &f : old.accesses) {
            variables.insert(*f.variable);
            p->receipt.oldVariables.push_back(*f.variable);
        }
        for (auto &f : now.accesses) {
            variables.insert(*f.variable);
            p->receipt.newVariables.push_back(*f.variable);
        }
        for (auto &f : old.targets) {
            targets.insert(f.target);
            p->receipt.oldTargets.push_back(f.target);
        }
        for (auto &f : now.targets) {
            targets.insert(f.target);
            p->receipt.newTargets.push_back(f.target);
        }
        // First restore directory/records so deleted targets' original pooled
        // incoming ranges remain available while constructing replacements.
        for (auto &c : p->actions) {
            auto &a = *d.actions.find(c.index);
            p->receipt.actions.push_back(a.id.valid() ? a.id : c.other.id);
        }
        p->receipt.actions.insert(p->receipt.actions.end(), ids.begin(), ids.end());
        unique(p->receipt.actions);
        for (auto &c : p->sequences) {
            auto &s = d.sequences[c.index];
            p->receipt.sequences.push_back(s.id.valid() ? s.id : c.other.id);
        }
        for (auto &c : p->sections) {
            auto &s = d.sections[c.index];
            p->receipt.sections.push_back(s.id.valid() ? s.id : c.other.id);
        }
        for (auto &c : p->scripts)
            if (c.other.sections != d.scripts[c.index].sections)
                p->receipt.scriptLayouts.push_back(d.scripts[c.index].context.id);
        unique(p->receipt.sections);
        for (auto &c : p->conditions)
            p->receipt.conditions.push_back(d.conditions[c.index].id.valid() ? d.conditions[c.index].id : c.other.id);
        for (auto &c : p->texts) {
            if (c.other.value != d.texts[c.index].value)
                p->receipt.texts.push_back(d.texts[c.index].id.valid()?d.texts[c.index].id:c.other.id);
            else
                p->receipt.presentationTexts.push_back(d.texts[c.index].id);
        }
        for (auto &c : p->annotations)
            p->receipt.annotations.push_back(c.index);
        auto identities = [](auto &items) {
            std::erase_if(items, [](auto id) { return !id.valid(); });
            unique(items);
        };
        identities(p->receipt.actions);
        identities(p->receipt.sequences);
        identities(p->receipt.sections);
        identities(p->receipt.conditions);
        identities(p->receipt.arms);
        identities(p->receipt.modules);
        unique(p->receipt.metadata);
        p->receipt.examinedActions = ids.size();
        unique(p->receipt.oldVariables);
        unique(p->receipt.newVariables);
        unique(p->receipt.oldTargets);
        unique(p->receipt.newTargets);
        p->swap(d);
        rolled = true;
        for (auto key : variables) {
            auto source = uses(d, key);
            auto replacement = std::make_shared<std::vector<VariableUse>>();
            for (auto f : source)
                if (!affected.contains(f.site.action))
                    replacement->push_back(f);
            for (auto f : now.accesses)
                if (f.variable == key)
                    replacement->push_back(f);
            std::stable_sort(replacement->begin(), replacement->end(),
                             [](auto &a, auto &b) { return a.site.action < b.site.action; });
            d.editedUses.try_emplace(key);
            p->variables.push_back({key, std::move(replacement)});
        }
        for (auto key : targets) {
            auto source = incoming(d, key);
            auto replacement = std::make_shared<std::vector<TargetUse>>();
            for (auto f : source)
                if (!affected.contains(f.site.action))
                    replacement->push_back(f);
            for (auto f : now.targets)
                if (f.target == key)
                    replacement->push_back(f);
            std::stable_sort(replacement->begin(), replacement->end(),
                             [](auto &a, auto &b) { return a.site.action < b.site.action; });
            d.editedIncoming.try_emplace(key);
            p->targets.push_back({key, std::move(replacement)});
        }
        if (!old.computed.empty() || !now.computed.empty()) {
            auto replacement = std::make_shared<std::vector<VariableUse>>();
            for (auto f : computed(d))
                if (!affected.contains(f.site.action))
                    replacement->push_back(f);
            replacement->insert(replacement->end(), now.computed.begin(), now.computed.end());
            p->computed = std::move(replacement);
            p->changesComputed = true;
        }
        stopped(stop);
        stopped(shutdown);
        p->receipt.diagnostics = p->diagnostics;
        prepared.push_back(p);
        PreparedChange result;
        result.data_ = std::move(p);
        return result;
    } catch (...) {
        if (!rolled)
            p->swap(d);
        throw;
    }
}
ChangeReceipt ServiceOwner::install(PreparedChange change, std::stop_token stop) {
    auto p = change.data_;
    require(p && p->owner == token && !p->used, "Invalid or already consumed prepared change");
    match(p->expected, stop);
    require(data->revision < UINT64_MAX && nextState < UINT64_MAX, "Program revision/history state exhausted");
    ChangeReceipt receipt = p->receipt;
    receipt.status = status();
    ++receipt.status.current.revision;
    receipt.status.historyState = nextState + 1;
    receipt.status.dirty = receipt.status.historyState != saved;
    history.reserve(cursor + 1);
    stopped(stop);
    stopped(shutdown);
    p->beforeState = state;
    p->afterState = ++nextState;
    p->used = true;
    p->swap(*data);
    if (p->protection)
        p->protection->swap(protectedIds);
    history.resize(cursor);
    history.push_back(p);
    ++cursor;
    state = p->afterState;
    ++data->revision;
    if(receipt.presentationChanged || !receipt.annotations.empty()) outlineAliasesDirty = true;
    receipt.projectionSequences=receipt.sequences;
    for(const auto &row:p->actions) {
        const auto *current=data->actions.find(row.index);const auto &before=row.other;
        bool shape=current->kind!=before.kind || !current->id.valid() || !before.id.valid();
        if(current->kind==ActionKind::Instruction && before.kind==ActionKind::Instruction && current->value.valid() && before.value.valid()) {
            const auto *a=data->values.find(current->value.value),*b=data->values.find(before.value.value);
            shape|=a->data!=b->data || a->detail!=b->detail;
        }
        shape|=!receipt.oldTargets.empty() || !receipt.newTargets.empty();
        if(shape){if(current->owner.valid())receipt.projectionSequences.push_back(current->owner);if(before.owner.valid())receipt.projectionSequences.push_back(before.owner);}
    }
    for(auto id:receipt.oldTargets)if(auto *a=data->action(id))receipt.projectionSequences.push_back(a->owner);
    for(auto id:receipt.newTargets)if(auto *a=data->action(id))receipt.projectionSequences.push_back(a->owner);
    unique(receipt.projectionSequences);
    refreshStringOwners(*p, receipt);
    invalidateOutline(receipt);
    invalidate(receipt);
    return receipt;
}
ChangeReceipt ServiceOwner::travel(Revision expected, bool redo, std::stop_token stop) {
    match(expected, stop);
    require(data && (redo ? cursor < history.size() : cursor > 0), redo ? "Nothing to redo" : "Nothing to undo");
    require(data->revision < UINT64_MAX, "Program revision exhausted");
    auto p = history[redo ? cursor : cursor - 1];
    auto receipt = p->receipt;
    receipt.status = status();
    ++receipt.status.current.revision;
    receipt.status.historyState = redo ? p->afterState : p->beforeState;
    receipt.status.dirty = receipt.status.historyState != saved;
    if (!redo) {
        std::swap(receipt.oldVariables, receipt.newVariables);
        std::swap(receipt.oldTargets, receipt.newTargets);
    }
    stopped(stop);
    stopped(shutdown);
    p->swap(*data);
    if (p->protection)
        p->protection->swap(protectedIds);
    if (redo)
        ++cursor;
    else
        --cursor;
    state = redo ? p->afterState : p->beforeState;
    ++data->revision;
    if(receipt.presentationChanged || !receipt.annotations.empty()) outlineAliasesDirty = true;
    receipt.projectionSequences=receipt.sequences;
    for(const auto &row:p->actions) {
        const auto *current=data->actions.find(row.index);const auto &before=row.other;
        bool shape=current->kind!=before.kind || !current->id.valid() || !before.id.valid();
        if(current->kind==ActionKind::Instruction && before.kind==ActionKind::Instruction && current->value.valid() && before.value.valid()) {
            const auto *a=data->values.find(current->value.value),*b=data->values.find(before.value.value);
            shape|=a->data!=b->data || a->detail!=b->detail;
        }
        shape|=!receipt.oldTargets.empty() || !receipt.newTargets.empty();
        if(shape){if(current->owner.valid())receipt.projectionSequences.push_back(current->owner);if(before.owner.valid())receipt.projectionSequences.push_back(before.owner);}
    }
    for(auto id:receipt.oldTargets)if(auto *a=data->action(id))receipt.projectionSequences.push_back(a->owner);
    for(auto id:receipt.newTargets)if(auto *a=data->action(id))receipt.projectionSequences.push_back(a->owner);
    unique(receipt.projectionSequences);
    refreshStringOwners(*p, receipt);
    invalidateOutline(receipt);
    invalidate(receipt);
    return receipt;
}
std::shared_ptr<const ReadScope> ServiceOwner::read(Revision expected, const ReadSelection &selection,
                                                    std::stop_token stop) {
    match(expected, stop);
    require(data != nullptr, "No program is open");
    auto &d = *data;
    auto out = std::make_shared<ReadScope>();
    out->status = status();
    CopyValues copy{d, out->payload};
    std::set<SctActionId> wanted;
    std::set<SctSequenceId> sequences, expanded;
    std::set<SctTextResourceId> texts(selection.texts.begin(),selection.texts.end());
    std::vector<SctActionId> pending = selection.actions;
    auto addSequence = [&](SctSequenceId id, bool contents) {
        auto *s = d.sequence(id);
        require(s, "Missing scope sequence");
        sequences.insert(id);
        if (contents && expanded.insert(id).second && s->order)
            pending.insert(pending.end(), s->order->begin(), s->order->end());
    };
    require(selection.script || selection.module || !selection.actions.empty() || !selection.texts.empty() || !selection.sections.empty() || !selection.conditions.empty() || !selection.sequences.empty(),
            "A view needs an explicit scope; use checkpoint for the whole project");
    if (selection.script) {
        auto *script = d.lookup(selection.script->value, EntityKind::Script, d.scripts);
        require(script, "Missing scope script");
        for (auto id : script->sections) {
            auto section = d.sections[d.directory.find(id.value).index];
            if (section.content.valid()) {
                const auto *content = d.values.find(section.content.value);
                if (content->kind == ValueKind::IndexedSection)
                    texts.insert(SctTextResourceId{content->data});
            }
            section.content = copy.copy(section.content);
            out->sections.push_back(section);
            if (section.body.valid())
                addSequence(section.body, true);
        }
        texts.insert(script->supplementaryText.begin(), script->supplementaryText.end());
    }
    for (auto id : selection.sections) {
        auto *source = d.lookup(id.value, EntityKind::Section, d.sections);
        require(source, "Missing selected section");
        auto section = *source;
        if (section.content.valid()) {
            const auto *v = d.values.find(section.content.value);
            if (v && v->kind == ValueKind::IndexedSection) texts.insert(SctTextResourceId{v->data});
            section.content = copy.copy(section.content);
        }
        out->sections.push_back(section);
        if (section.body.valid()) addSequence(section.body, selection.descend);
    }
    for (auto id : selection.conditions) {
        auto *source = d.lookup(id.value, EntityKind::Condition, d.conditions);
        require(source, "Missing selected condition");
        auto condition = *source;
        condition.result = copy.copy(condition.result);
        out->conditions.push_back(condition);
        addSequence(condition.preparation, selection.descend);
    }
    for (auto id : selection.sequences) addSequence(id, selection.descend);
    if (selection.module) {
        auto *m = d.lookup(selection.module->value, EntityKind::Module, d.metadata.modules);
        require(m, "Missing scope module");
        for (auto id : m->sequences)
            addSequence(id, true);
    }
    while (!pending.empty()) {
        stopped(stop);
        stopped(shutdown);
        auto id = pending.back();
        pending.pop_back();
        if (!wanted.insert(id).second)
            continue;
        auto *source = d.action(id);
        require(source, "Missing scope action");
        auto a = *source;
        addSequence(a.owner, false);
        a.value = copy.copy(a.value);
        a.firstArm = count32(out->arms.size());
        for (std::uint32_t i = 0; i < source->armCount; ++i) {
            auto arm = d.arms[source->firstArm + i];
            auto labels = std::span(d.caseLabels).subspan(arm.firstLabel, arm.labelCount);
            arm.firstLabel = count32(out->caseLabels.size());
            out->caseLabels.insert(out->caseLabels.end(), labels.begin(), labels.end());
            out->arms.push_back(arm);
            addSequence(arm.body, selection.descend);
        }
        if (a.kind == ActionKind::Loop) {
            auto l = d.loops.at(a.detail);
            a.detail = count32(out->loops.size());
            out->loops.push_back(l);
            for (auto seq : {l.initialize, l.body, l.update})
                if (seq.valid())
                    addSequence(seq, selection.descend);
        }
        if (a.kind == ActionKind::Dispatch) {
            a.detail = count32(out->dispatches.size());
            out->dispatches.push_back(d.dispatches.at(source->detail));
        }
        if (a.kind == ActionKind::Switch) {
            a.detail = count32(out->switches.size());
            out->switches.push_back(d.switches.at(source->detail));
        }
        if (a.kind == ActionKind::Transfer) {
            a.detail = count32(out->transfers.size());
            out->transfers.push_back(d.transfers.at(source->detail));
        }
        std::vector<ConditionId> checks;
        for (std::uint32_t i = 0; i < source->armCount; ++i)
            if (auto c = d.arms[source->firstArm + i].condition; c.valid())
                checks.push_back(c);
        if (source->kind == ActionKind::Loop)
            checks.push_back(d.loops.at(source->detail).condition);
        if (source->kind == ActionKind::Switch)
            checks.push_back(d.switches.at(source->detail).selector);
        if (source->kind == ActionKind::Dispatch)
            for (const auto &sample : d.dispatches.at(source->detail).samples)
                checks.push_back(sample.condition);
        for (auto id : checks) {
            auto c = d.conditions[d.directory.find(id.value).index];
            c.result = copy.copy(c.result);
            out->conditions.push_back(c);
            addSequence(c.preparation, selection.descend);
        }
        out->actions.push_back(a);
    }
    for (auto &v : out->payload.values)
        if (selection.referencedText && (v.kind == ValueKind::IndexedTextReference || v.kind == ValueKind::SupplementaryTextReference))
            texts.insert(SctTextResourceId{v.data});
    for (auto id : texts) {
        const auto* original = d.lookup(id.value,EntityKind::Text,d.texts);
        require(original != nullptr,"Missing requested text");
        auto t = *original;
        t.value = copy.copy(t.value);
        out->texts.push_back(std::move(t));
    }
    for (auto id : sequences)
        out->sequences.push_back(*d.sequence(id));
    std::set<SctScriptId> owners;
    for (auto &s : out->sequences)
        owners.insert(s.script);
    if (selection.script)
        owners.insert(*selection.script);
    for (const auto& text : out->texts) owners.insert(text.script);
    for (const auto& section : out->sections) owners.insert(section.script);
    for (auto id : owners)
        out->scripts.push_back(d.scripts[d.directory.find(id.value).index]);
    for (auto &block : d.metadata.localization.blocks)
        if (std::ranges::find(out->actions, block.action, &Action::id) != out->actions.end())
            out->localized.push_back(block);
    for(const auto &custom : d.metadata.opcodes)
        if(std::ranges::any_of(out->actions,[&](const auto &a){return a.kind==ActionKind::Instruction && out->payload.values.at(a.value.value).data==custom.opcode;}))out->catalog.push_back(custom);
    std::sort(out->actions.begin(), out->actions.end(), [](auto &a, auto &b) { return a.id < b.id; });
    return out;
}
Checkpoint ServiceOwner::checkpoint(Revision expected, std::stop_token stop) {
    match(expected, stop);
    require(data != nullptr, "No program is open");
    auto copy = std::make_unique<ProgramData>(*data);
    Pages<Action> actions;
    std::vector<Range> ranges;
    std::vector<SctActionId> successors;
    for (std::uint32_t i = 0; i < copy->actions.size(); ++i) {
        auto a = *copy->actions.find(i);
        if (!a.id.valid())
            continue;
        copy->directory.set(a.id.value, {EntityKind::Action, actions.size()});
        actions.append(a);
        ranges.push_back(i < copy->incomingRanges.size() ? copy->incomingRanges[i] : Range{});
        successors.push_back(copy->successors[i]);
    }
    copy->actions = std::move(actions);
    copy->incomingRanges = std::move(ranges);
    copy->successors = std::move(successors);
    std::erase_if(copy->sections, [](auto &s) { return !s.id.valid(); });
    std::erase_if(copy->sequences, [](auto &s) { return !s.id.valid(); });
    for (std::uint32_t i = 0; i < copy->sections.size(); ++i)
        copy->directory.set(copy->sections[i].id.value, {EntityKind::Section, i});
    for (std::uint32_t i = 0; i < copy->sequences.size(); ++i)
        copy->directory.set(copy->sequences[i].id.value, {EntityKind::Sequence, i});
    stopped(stop);
    stopped(shutdown);
    checkpointTokens[data->revision] = state;
    return {status(), std::shared_ptr<const Program>(new Program(std::move(copy)))};
}
Maintenance ServiceOwner::maintain(Revision expected, bool discard) {
    match(expected);
    require(data != nullptr, "No program is open");
    auto &d = *data;
    std::vector<std::shared_ptr<PreparedData>> retained;
    if (!discard)
        retained = history;
    for (auto &weak : prepared)
        if (auto p = weak.lock(); p && !p->used && p->expected == expected)
            retained.push_back(p);
    std::sort(retained.begin(), retained.end());
    retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
    ValueBuffer buffer;
    CopyValues copy{d, buffer};
    std::vector<std::pair<ValueId *, ValueId>> edits;
    auto root = [&](ValueId &id) {
        if (id.valid())
            edits.push_back({&id, copy.copy(id)});
    };
    for (std::uint32_t i = 0; i < d.actions.size(); ++i)
        if (d.actions.find(i)->id.valid())
            root(d.actions.find(i)->value);
    for (auto &s : d.sections)
        if (s.id.valid())
            root(s.content);
    for (auto &c : d.conditions)
        root(c.result);
    for (auto &t : d.texts)
        root(t.value);
    std::vector<VariableUse> accesses, computeds(computed(d).begin(), computed(d).end());
    std::map<Variable, Range> ranges;
    std::set<Variable> keys;
    for (auto &[k, r] : d.variableUses)
        keys.insert(k);
    for (auto &[k, r] : d.editedUses)
        keys.insert(k);
    for (auto key : keys) {
        auto range = uses(d, key);
        if (range.empty())
            continue;
        ranges[key] = {count32(accesses.size()), count32(range.size())};
        for (auto f : range) {
            f.expression = copy.copy(f.expression);
            accesses.push_back(f);
        }
    }
    for (auto &f : computeds)
        f.expression = copy.copy(f.expression);
    std::vector<TargetUse> targets;
    std::vector<Range> incomingRanges(d.actions.size());
    for (std::uint32_t i = 0; i < d.actions.size(); ++i)
        if (auto id = d.actions.find(i)->id; id.valid()) {
            auto range = incoming(d, id);
            incomingRanges[i] = {count32(targets.size()), count32(range.size())};
            targets.insert(targets.end(), range.begin(), range.end());
        }
    // Null inactive buckets mean the old bulk range. Materialize those ranges
    // before replacing the bulk bases. Readers/checkpoints retain their own pools.
    struct BucketEdit {
        std::shared_ptr<const std::vector<VariableUse>> *place;
        std::shared_ptr<const std::vector<VariableUse>> value;
    };
    std::vector<BucketEdit> buckets;
    std::vector<
        std::pair<std::shared_ptr<const std::vector<TargetUse>> *, std::shared_ptr<const std::vector<TargetUse>>>>
        targetBuckets;
    auto bucket = [&](auto &ptr, std::span<const VariableUse> fallback) {
        auto rows = std::make_shared<std::vector<VariableUse>>(
            ptr ? *ptr : std::vector<VariableUse>(fallback.begin(), fallback.end()));
        for (auto &f : *rows)
            f.expression = copy.copy(f.expression);
        buckets.push_back({&ptr, std::move(rows)});
    };
    for (auto &p : retained) {
        for (auto &r : p->actions)
            root(r.other.value);
        for (auto &r : p->sections)
            root(r.other.content);
        for (auto &r : p->conditions)
            root(r.other.result);
        for (auto &r : p->texts)
            root(r.other.value);
        for (auto &[key, ptr] : p->variables) {
            auto it = d.variableUses.find(key);
            auto range = it == d.variableUses.end() ? std::span<const VariableUse>{}
                                                    : std::span(d.accesses).subspan(it->second.first, it->second.count);
            bucket(ptr, range);
        }
        if (p->changesComputed)
            bucket(p->computed, d.computed);
        for (auto &[key, ptr] : p->targets)
            if (!ptr) {
                auto at = d.directory.find(key.value);
                if (at.kind != EntityKind::Action) {
                    // A deleted target's inverse still names its original slot.
                    for (auto &[id, location] : p->locations)
                        if (id == key.value && location.kind == EntityKind::Action)
                            at = location;
                }
                std::span<const TargetUse> range;
                if (at.kind == EntityKind::Action && at.index < d.incomingRanges.size()) {
                    auto r = d.incomingRanges[at.index];
                    range = std::span(d.targetUses).subspan(r.first, r.count);
                }
                targetBuckets.push_back(
                    {&ptr, std::make_shared<const std::vector<TargetUse>>(range.begin(), range.end())});
            }
    }
    std::set<std::uint32_t> heldActions, heldSequences, heldSections;
    for (auto &p : retained) {
        for (auto &c : p->actions)
            heldActions.insert(c.index);
        for (auto &c : p->sequences)
            heldSequences.insert(c.index);
        for (auto &c : p->sections)
            heldSections.insert(c.index);
    }
    std::vector<std::uint32_t> actionSlots, sequenceSlots, sectionSlots;
    for (std::uint32_t i = 0; i < d.actions.size(); ++i)
        if (!d.actions.find(i)->id.valid() && !heldActions.contains(i))
            actionSlots.push_back(i);
    for (std::uint32_t i = 0; i < d.sequences.size(); ++i)
        if (!d.sequences[i].id.valid() && !heldSequences.contains(i))
            sequenceSlots.push_back(i);
    for (std::uint32_t i = 0; i < d.sections.size(); ++i)
        if (!d.sections[i].id.valid() && !heldSections.contains(i))
            sectionSlots.push_back(i);
    // Arm edits replace only an owner's small ordered descriptor range. Repack
    // current and retained ranges here, so discarded label versions do not grow
    // forever. Stable arm identities and history directory locations survive.
    std::vector<Arm> compactArms;
    std::vector<std::int32_t> compactLabels;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> armRanges;
    std::map<std::uint32_t, std::uint32_t> armSlots;
    std::vector<std::pair<std::uint32_t *, std::uint32_t>> armHandles;
    auto retainArms = [&](Action &action) {
        if (!action.armCount)
            return;
        auto key = std::pair(action.firstArm, action.armCount);
        auto [found, inserted] = armRanges.try_emplace(key, count32(compactArms.size()));
        if (inserted) {
            for (std::uint32_t i = 0; i < action.armCount; ++i) {
                auto index = action.firstArm + i;
                auto arm = d.arms[index];
                auto labels = std::span(d.caseLabels).subspan(arm.firstLabel, arm.labelCount);
                arm.firstLabel = count32(compactLabels.size());
                compactLabels.insert(compactLabels.end(), labels.begin(), labels.end());
                armSlots[index] = count32(compactArms.size());
                compactArms.push_back(arm);
            }
        }
        armHandles.push_back({&action.firstArm, found->second});
    };
    for (std::uint32_t i = 0; i < d.actions.size(); ++i)
        if (d.actions.find(i)->id.valid())
            retainArms(*d.actions.find(i));
    for (auto &p : retained)
        for (auto &change : p->actions)
            retainArms(change.other);
    std::vector<std::pair<std::uint64_t, Location>> armDirectory;
    for (auto [from, to] : armSlots) {
        auto id = d.arms[from].id;
        auto location = d.directory.find(id.value);
        if (location.kind == EntityKind::Arm && location.index == from)
            armDirectory.push_back({id.value, {EntityKind::Arm, to}});
    }
    std::vector<std::pair<Location *, Location>> historyArms;
    for (auto &p : retained)
        for (auto &[id, location] : p->locations)
            if (location.kind == EntityKind::Arm) {
                auto found = armSlots.find(location.index);
                require(found != armSlots.end(), "Missing retained arm range");
                historyArms.push_back({&location, {EntityKind::Arm, found->second}});
            }
    ProgramData compact;
    for (auto v : buffer.values)
        compact.values.append(v);
    compact.children = std::move(buffer.children);
    compact.characters.assign(buffer.characters.begin(), buffer.characters.end());
    Maintenance result{d.values.size(), compact.values.size()};
    // No semantic analysis and no authored-ID changes. All allocations have
    // succeeded before the first live handle is changed.
    for (auto &[id, to] : edits)
        *id = to;
    for (auto &b : buckets)
        *b.place = std::move(b.value);
    for (auto &[place, value] : targetBuckets)
        *place = std::move(value);
    for (auto [place, to] : armHandles)
        *place = to;
    for (auto [id, to] : armDirectory)
        d.directory.set(id, to);
    for (auto [place, to] : historyArms)
        *place = to;
    d.arms = std::move(compactArms);
    d.caseLabels = std::move(compactLabels);
    d.values = std::move(compact.values);
    d.children = std::move(compact.children);
    d.characters = std::move(compact.characters);
    d.accesses = std::move(accesses);
    d.computed = std::move(computeds);
    d.variableUses = std::move(ranges);
    d.targetUses = std::move(targets);
    d.incomingRanges = std::move(incomingRanges);
    for (auto &[key, ptr] : d.editedUses)
        ptr.reset();
    for (auto &[key, ptr] : d.editedIncoming)
        ptr.reset();
    d.editedComputed.reset();
    freeActions = std::move(actionSlots);
    freeSequences = std::move(sequenceSlots);
    freeSections = std::move(sectionSlots);
    if (discard) {
        history.clear();
        cursor = 0;
    }
    std::erase_if(prepared, [&](auto &w) {
        auto p = w.lock();
        return !p || p->used || p->expected != expected;
    });
    return result;
}

#include "ProgramRelationships.inl"
#include "CutsceneRelationships.inl"

struct ProgramService::Control : std::enable_shared_from_this<Control> {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void(ServiceOwner &)>> queue;
    bool closing = false;
    std::stop_source shutdownStop;
    std::atomic<std::uint64_t> loadGeneration{0};
    void run() {
        ServiceOwner owner;
        owner.shutdown = shutdownStop.get_token();
        for (;;) {
            std::function<void(ServiceOwner &)> task;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return closing || !queue.empty(); });
                if (queue.empty())
                    break;
                task = std::move(queue.front());
                queue.pop_front();
            }
            task(owner);
        }
    }
    template <class T, class F> std::future<Result<T>> submit(F f) {
        auto promise = std::make_shared<std::promise<Result<T>>>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(mutex);
            if (closing) {
                promise->set_value(Result<T>::failure(failure("Program service is shutting down")));
                return future;
            }
            queue.push_back([promise, f = std::move(f), self = shared_from_this()](ServiceOwner &owner) mutable {
                try {
                    stopped(self->shutdownStop.get_token());
                    promise->set_value(Result<T>::success(f(owner)));
                } catch (const std::exception &e) {
                    promise->set_value(Result<T>::failure(failure(e.what())));
                } catch (...) {
                    promise->set_value(Result<T>::failure(failure("Program operation failed")));
                }
            });
        }
        cv.notify_one();
        return future;
    }
};
const ChangeReceipt &PreparedChange::preview() const {
    require(data_ != nullptr, "Empty prepared change");
    return data_->receipt;
}
ProgramService::ProgramService() : control_(std::make_shared<Control>()) {
    std::thread([c = control_] { c->run(); }).detach();
}
ProgramService::~ProgramService() {
    shutdown();
}
std::future<Result<Catalog>> ProgramService::catalog(Revision r) {
    return control_->submit<Catalog>([r](auto &o) {
        o.match(r);
        require(o.data != nullptr, "No program is open");
        Catalog result{o.status()};
        result.storage=o.data->stats;result.storage.actions=o.data->actions.size();result.storage.values=o.data->values.size();
        result.storage.actionPages=o.data->actions.pageCount();result.storage.valuePages=o.data->values.pageCount();
        result.childReferences=o.data->children.size();result.characterBytes=o.data->characters.size();
        for (auto &s : o.data->scripts) {
            const auto *baseline = o.data->lookup(s.context.baseline.value, EntityKind::Baseline, o.data->baselines);
            result.scripts.push_back({s.context.id, s.context.name, s.sections.size(), s.context,
                baseline ? std::optional(baseline->source.locator) : std::nullopt, baseline ? baseline->textConvention : std::nullopt,
                baseline ? std::optional(baseline->source) : std::nullopt});
        }
        result.modules = o.data->metadata.modules;
        return result;
    });
}
std::future<Result<ServiceStatus>> ProgramService::status() {
    return control_->submit<ServiceStatus>([](auto &o) { return o.status(); });
}
std::future<Result<ServiceStatus>> ProgramService::adopt(Revision expected, std::shared_ptr<const Program> p,
                                                         std::stop_token stop) {
    return control_->submit<ServiceStatus>(
        [expected, p = std::move(p), stop](auto &o) mutable { return o.adopt(expected, std::move(p), stop); });
}
std::future<Result<PreparedChange>> ProgramService::prepare(EditRequest r, std::stop_token stop) {
    return control_->submit<PreparedChange>(
        [r = std::move(r), stop](auto &o) mutable { return o.prepare(std::move(r), stop); });
}
std::future<Result<ChangeReceipt>> ProgramService::install(PreparedChange p, std::stop_token stop) {
    return control_->submit<ChangeReceipt>(
        [p = std::move(p), stop](auto &o) mutable { return o.install(std::move(p), stop); });
}
std::future<Result<ChangeReceipt>> ProgramService::edit(EditRequest r, std::stop_token stop) {
    return control_->submit<ChangeReceipt>(
        [r = std::move(r), stop](auto &o) mutable { return o.install(o.prepare(std::move(r), stop), stop); });
}
std::future<Result<ChangeReceipt>> ProgramService::undo(Revision r, std::stop_token stop) {
    return control_->submit<ChangeReceipt>([r, stop](auto &o) { return o.travel(r, false, stop); });
}
std::future<Result<ChangeReceipt>> ProgramService::redo(Revision r, std::stop_token stop) {
    return control_->submit<ChangeReceipt>([r, stop](auto &o) { return o.travel(r, true, stop); });
}
std::future<Result<std::shared_ptr<const ReadScope>>> ProgramService::read(Revision r, ReadSelection s,
                                                                           std::stop_token stop) {
    return control_->submit<std::shared_ptr<const ReadScope>>(
        [r, s = std::move(s), stop](auto &o) { return o.read(r, s, stop); });
}
#include "FeatureQueries.inl"
#include "LocalizationQueries.inl"
#include "ProgramBrowse.inl"
#include "ProgramEditor.inl"
#include "ProgramProductivity.inl"
#include "StoryQueries.inl"
std::future<Result<VariableQuery>> ProgramService::uses(Revision r, Variable key) {
    return control_->submit<VariableQuery>([r, key](auto &o) {
        o.match(r);
        require(o.data != nullptr, "No program is open");
        VariableQuery q{o.status(), key};
        CopyValues copy{*o.data, q.payload};
        for (auto f : program::uses(*o.data, key)) {
            f.expression = copy.copy(f.expression);
            q.uses.push_back(f);
        }
        return q;
    });
}
std::future<Result<TargetQuery>> ProgramService::incoming(Revision r, SctActionId key) {
    return control_->submit<TargetQuery>([r, key](auto &o) {
        o.match(r);
        require(o.data && o.data->action(key), "Missing target action");
        auto range = program::incoming(*o.data, key);
        return TargetQuery{o.status(), key, {range.begin(), range.end()}};
    });
}
std::future<Result<CutsceneQuery>> ProgramService::cutscene(Revision r, SctModuleId module, std::stop_token stop) {
    return control_->submit<CutsceneQuery>([r, module, stop](auto &o) { return o.cutscene(r, module, stop); });
}
std::future<Result<SummaryQuery>> ProgramService::summarize(Revision r, SctModuleId module, std::stop_token stop) {
    return control_->submit<SummaryQuery>([r, module, stop](auto &o) { return o.summarize(r, module, stop); });
}
std::future<Result<InteractionQuery>> ProgramService::interactions(Revision r, SctScriptId script) {
    return control_->submit<InteractionQuery>([r, script](auto &o) {
        o.match(r);
        require(o.data && o.data->directory.find(script.value).kind == EntityKind::Script,
                "Missing interaction script");
        auto &d = *o.data;
        InteractionQuery out;
        out.status = o.status();
        for (auto &e : d.metadata.entrypoints)
            if (e.script == script)
                out.entrypoints.push_back(e);
        for (auto &c : d.metadata.connections)
            if (c.route && (c.route->sourceScript == script || c.route->destinationScript == script))
                out.connections.push_back(c);
        for (auto &a : d.metadata.annotations) {
            auto belongs = std::visit(
                [&](auto id) {
                    auto at = d.directory.find(id.value);
                    switch (at.kind) {
                    case EntityKind::Script:
                        return id.value == script.value;
                    case EntityKind::Action:
                        return d.sequence(d.actions.find(at.index)->owner)->script == script;
                    case EntityKind::Module:
                        return d.metadata.modules[at.index].script == script;
                    case EntityKind::Entry:
                        return d.metadata.entrypoints[at.index].script == script;
                    case EntityKind::Section:
                        return d.sections[at.index].script == script;
                    case EntityKind::Sequence:
                        return d.sequences[at.index].script == script;
                    case EntityKind::Text:
                        return d.texts[at.index].script == script;
                    case EntityKind::Condition:
                        return d.sequence(d.action(d.conditions[at.index].owner)->owner)->script == script;
                    case EntityKind::Arm:
                        return d.sequence(d.action(d.arms[at.index].owner)->owner)->script == script;
                    default:
                        return false;
                    }
                },
                a.subject);
            if (belongs)
                out.annotations.push_back(a);
        }
        for (auto &meaning : d.metadata.meanings)
            if (!meaning.script.valid() || meaning.script == script)
                out.meanings.push_back(meaning);
        out.languages = d.metadata.languages;
        return out;
    });
}
std::future<Result<ExplanationQuery>> ProgramService::explain(Revision r, decltype(Annotation::subject) subject) {
    return control_->submit<ExplanationQuery>([r, subject](auto &o) {
        o.match(r);
        require(o.data != nullptr, "No program is open");
        ExplanationQuery out;
        out.status = o.status();
        std::visit(
            [&](auto id) {
                using T = decltype(id);
                constexpr auto kind = [] {
                    if constexpr (std::is_same_v<T, SctScriptId>)
                        return EntityKind::Script;
                    else if constexpr (std::is_same_v<T, SectionId>)
                        return EntityKind::Section;
                    else if constexpr (std::is_same_v<T, SctSequenceId>)
                        return EntityKind::Sequence;
                    else if constexpr (std::is_same_v<T, SctActionId>)
                        return EntityKind::Action;
                    else if constexpr (std::is_same_v<T, ArmId>)
                        return EntityKind::Arm;
                    else if constexpr (std::is_same_v<T, ConditionId>)
                        return EntityKind::Condition;
                    else if constexpr (std::is_same_v<T, SctModuleId>)
                        return EntityKind::Module;
                    else if constexpr (std::is_same_v<T, SctEntrypointId>)
                        return EntityKind::Entry;
                    else if constexpr (std::is_same_v<T, SctPortId>) return EntityKind::Port;
                    else if constexpr (std::is_same_v<T, SctConnectionId>) return EntityKind::Connection;
                    else
                        return EntityKind::Text;
                }();
                require(o.data->directory.find(id.value).kind == kind, "Missing or mistyped explanation subject");
                out.label = std::to_string(id.value);
            },
            subject);
        if(auto* id=std::get_if<SctPortId>(&subject))out.label=o.data->lookup(id->value,EntityKind::Port,o.data->metadata.ports)->name;
        if(auto* id=std::get_if<SctConnectionId>(&subject))out.records=o.data->lookup(id->value,EntityKind::Connection,o.data->metadata.connections)->evidence;
        for (auto &annotation : o.data->metadata.annotations)
            if (annotation.subject == subject) {
                if (!annotation.alias.empty())
                    out.label = annotation.alias;
                out.records.insert(out.records.end(), annotation.evidence.begin(), annotation.evidence.end());
            }
        return out;
    });
}
std::future<Result<Checkpoint>> ProgramService::checkpoint(Revision r, std::stop_token stop) {
    return control_->submit<Checkpoint>([r, stop](auto &o) { return o.checkpoint(r, stop); });
}
std::future<Result<ServiceStatus>> ProgramService::markSaved(Revision captured, std::uint64_t state) {
    return control_->submit<ServiceStatus>([captured, state](auto &o) {
        auto current = o.status().current;
        require(captured.project == current.project && captured.generation == current.generation &&
                    captured.revision <= current.revision,
                "Stale save completion");
        auto at = o.checkpointTokens.find(captured.revision);
        require(at != o.checkpointTokens.end() && at->second == state && captured.revision >= o.savedRevision,
                "Invalid or superseded checkpoint token");
        o.saved = state;
        o.savedRevision = captured.revision;
        o.checkpointTokens.erase(o.checkpointTokens.begin(), std::next(at));
        return o.status();
    });
}
std::future<Result<Maintenance>> ProgramService::maintain(Revision r, bool discard) {
    return control_->submit<Maintenance>([r, discard](auto &o) { return o.maintain(r, discard); });
}
std::future<Result<ServiceStatus>> ProgramService::load(NativeLoadRequest request, std::stop_token stop) {
    auto c = control_;
    auto operation = ++c->loadGeneration;
    auto promise = std::make_shared<std::promise<Result<ServiceStatus>>>();
    auto future = promise->get_future();
    std::thread([c, operation, request = std::move(request), stop, promise]() mutable {
        try {
            auto expected = request.expected;
            auto accepted = c->submit<bool>([expected, stop](auto &o) {
                                 o.match(expected, stop);
                                 return true;
                             }).get();
            if (!accepted) {
                promise->set_value(Result<ServiceStatus>::failure(accepted.diagnostics()));
                return;
            }
            auto loaded = loadNativeCollection(std::move(request), stop, c->shutdownStop.get_token(),
                                               [c, operation] { return c->loadGeneration == operation; });
            if (!loaded) {
                promise->set_value(Result<ServiceStatus>::failure(loaded.diagnostics()));
                return;
            }
            auto diagnostics = loaded.diagnostics();
            auto result = c->submit<ServiceStatus>([expected, c, operation, stop,
                                                    p = std::move(loaded).takeValue()](auto &o) mutable {
                               require(c->loadGeneration == operation, "Native load was superseded");
                               stopped(c->shutdownStop.get_token());
                               return o.adopt(expected, std::move(p), stop,
                                              [c, operation] { return c->loadGeneration == operation; });
                           }).get();
            if (result) {
                diagnostics.insert(diagnostics.end(), result.diagnostics().begin(), result.diagnostics().end());
                promise->set_value(Result<ServiceStatus>::success(result.value(), std::move(diagnostics)));
            } else
                promise->set_value(std::move(result));
        } catch (const std::exception &e) {
            promise->set_value(Result<ServiceStatus>::failure(failure(e.what())));
        } catch (...) {
            promise->set_value(Result<ServiceStatus>::failure(failure("Native collection load failed")));
        }
    }).detach();
    return future;
}
std::future<Result<ProgramOpenResult>> ProgramService::openProject(ProgramOpenRequest request, std::stop_token stop) {
    auto c = control_;
    auto operation = ++c->loadGeneration;
    auto promise = std::make_shared<std::promise<Result<ProgramOpenResult>>>();
    auto future = promise->get_future();
    std::thread([c, operation, request = std::move(request), stop, promise]() mutable {
        try {
            std::stop_source combined;
            std::stop_callback cancel(stop, [&] { combined.request_stop(); });
            std::stop_callback close(c->shutdownStop.get_token(), [&] { combined.request_stop(); });
            auto token = combined.get_token();
            auto expected = request.expected;
            auto accepted = c->submit<bool>([expected, token](auto &o) {
                                 o.match(expected, token);
                                 return true;
                             }).get();
            if (!accepted) {
                promise->set_value(Result<ProgramOpenResult>::failure(accepted.diagnostics()));
                return;
            }
            if (request.recovery) {
                stopped(token);
                if (request.progress)
                    request.progress({"Recovering workspace transactions", 0, 0, {}, false});
                const auto &recovery = *request.recovery;
                for (const auto &result :
                     WorkspaceTransactionService::recoverAll(recovery.workspaceRoot, recovery.transactionsRelativeRoot))
                    require(result.status == WorkspaceTransactionStatus::Verified ||
                                result.status == WorkspaceTransactionStatus::RolledBack,
                            "Workspace transaction recovery is blocked");
                stopped(token);
            }
            auto loaded = ProgramPersistence::load(request.path, request.limits, token, request.progress);
            if (!loaded) {
                promise->set_value(Result<ProgramOpenResult>::failure(loaded.diagnostics()));
                return;
            }
            auto diagnostics = loaded.diagnostics();
            auto file = std::move(loaded).takeValue();
            auto result =
                c->submit<ProgramOpenResult>([expected, token, c, operation, file = std::move(file)](auto &o) mutable {
                     require(c->loadGeneration == operation, "Program load was superseded");
                     auto status = o.adopt(expected, std::move(file.program), token,
                                           [c, operation] { return c->loadGeneration == operation; });
                     return ProgramOpenResult{status, std::move(file.presentation),file.encodedDigest};
                 }).get();
            if (result)
                promise->set_value(
                    Result<ProgramOpenResult>::success(std::move(result).takeValue(), std::move(diagnostics)));
            else
                promise->set_value(std::move(result));
        } catch (const std::exception &e) {
            promise->set_value(Result<ProgramOpenResult>::failure(failure(e.what())));
        } catch (...) {
            promise->set_value(Result<ProgramOpenResult>::failure(failure("Program project loading failed")));
        }
    }).detach();
    return future;
}
std::future<Result<bool>> ProgramService::shutdown() {
    auto promise = std::make_shared<std::promise<Result<bool>>>();
    auto future = promise->get_future();
    auto c = control_;
    {
        std::lock_guard lock(c->mutex);
        if (c->closing) {
            promise->set_value(Result<bool>::success(true));
            return future;
        }
        c->closing = true;
        ++c->loadGeneration;
        c->shutdownStop.request_stop();
        c->queue.push_back([promise](auto &) { promise->set_value(Result<bool>::success(true)); });
    }
    c->cv.notify_all();
    return future;
}
} // namespace salsa::core::program
