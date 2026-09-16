#include "DispatchPredicates.h"
#include "ProgramInternal.h"
#include <bit>
#include <cmath>
#include <set>

namespace salsa::core::program {
namespace {
namespace sct = spice::sct;
const Value& node(const ProgramData& d, ValueId id) {
    auto* v = d.values.find(id.value); require(v != nullptr, "Invalid program value handle"); return *v;
}
void shape(const ProgramData& d, ValueId id) {
    const auto& v = node(d, id); auto children = d.child(id);
    auto all = [&](auto predicate) { for (auto child : children) require(predicate(node(d, child).kind), "Invalid typed value child"); };
    auto words = [&] { all([](ValueKind kind) { return kind == ValueKind::Word; }); };
    auto leaf = [&] { require(children.empty(), "Leaf value has children"); };
    switch (v.kind) {
    case ValueKind::Instruction: {
        require(v.data <= UINT16_MAX && v.detail <= 3, "Invalid opcode/refresh/schedule flags");
        std::size_t start = 0;
        if (v.detail & 2) {
            require(!children.empty(), "Missing schedule expression"); auto k = node(d, children[0]).kind;
            require(k == ValueKind::Expression || k == ValueKind::Unavailable, "Invalid schedule expression"); start = 1;
        }
        std::set<std::uint64_t> fixed; bool repeated = false;
        for (; start < children.size(); ++start) {
            const auto& p = node(d, children[start]);
            if (p.kind == ValueKind::RepeatedGroup) repeated = true;
            else require(p.kind == ValueKind::Parameter && !repeated && fixed.insert(p.data).second, "Invalid/duplicate fixed parameter");
        }
        break;
    }
    case ValueKind::RepeatedGroup: {
        std::set<std::uint64_t> indices;
        for (auto child : children) { const auto& p = node(d, child); require(p.kind == ValueKind::Parameter && indices.insert(p.data).second, "Invalid/duplicate repeated parameter"); }
        break;
    }
    case ValueKind::Parameter: {
        require(v.data <= UINT32_MAX && children.size() == 1, "Invalid parameter");
        auto k = node(d, children[0]).kind;
        require(k == ValueKind::Word || k == ValueKind::Expression || k == ValueKind::Words || k == ValueKind::ActionReference ||
            k == ValueKind::IndexedTextReference || k == ValueKind::SupplementaryTextReference || k == ValueKind::Unresolved || k == ValueKind::Unavailable, "Invalid parameter value"); break;
    }
    case ValueKind::Expression:
        require(v.data <= static_cast<unsigned>(sct::SctExpressionTermination::StopCode), "Invalid expression termination");
        all([](ValueKind k) { return k == ValueKind::ValueOperation || k == ValueKind::BinaryOperation || k == ValueKind::StackOperation || k == ValueKind::InertOperation; }); break;
    case ValueKind::ValueOperation:
        require(v.data <= UINT32_MAX && v.detail <= static_cast<unsigned>(sct::SctScptValueKind::SecondaryValue), "Invalid SCPT value operation"); words(); break;
    case ValueKind::BinaryOperation:
        require(v.detail <= static_cast<unsigned>(sct::SctScptBinaryOperationKind::Arithmetic), "Invalid binary operation kind");
        [[fallthrough]];
    case ValueKind::Word: case ValueKind::StackOperation: case ValueKind::InertOperation:
        leaf(); require(v.data <= UINT32_MAX, "Encoded word exceeds 32 bits"); break;
    case ValueKind::Words: case ValueKind::StringGroup: words(); break;
    case ValueKind::ActionReference: case ValueKind::IndexedTextReference: case ValueKind::SupplementaryTextReference:
        leaf(); require(v.data != 0, "Zero resolved target identity"); break;
    case ValueKind::Unresolved:
        require(v.data <= static_cast<unsigned>(sct::SctReferenceTargetStorage::SupplementaryText), "Invalid unresolved target storage"); words(); break;
    case ValueKind::Unavailable: leaf(); break;
    case ValueKind::PlainText: case ValueKind::TextChunk:
        leaf(); require(v.data <= d.characters.size() && v.detail <= d.characters.size() - v.data, "Invalid text range"); break;
    case ValueKind::Message:
        require(v.data <= 1 && (v.data == 0 || !children.empty()), "Invalid message header");
        for (std::size_t i = 0; i < children.size(); ++i) {
            auto k = node(d, children[i]).kind;
            require(v.data && i == 0 ? k == ValueKind::PlainText : k == ValueKind::TextChunk || k == ValueKind::TextCommand, "Invalid message element");
        }
        break;
    case ValueKind::TextCommand:
        require(v.data <= static_cast<unsigned>(sct::SctMessageCommandCode::Wo) && v.detail <= 2, "Invalid text command"); words();
        require(v.detail != 0 || children.empty(), "No-argument command has arguments");
        require(v.detail != 1 || children.size() <= 1, "Decimal command has multiple arguments");
        if (v.detail == 2) for (auto child : children) require(node(d, child).data <= UINT8_MAX, "Text command byte exceeds 255");
        break;
    case ValueKind::EmptyIndexedText: leaf(); break;
    case ValueKind::IndexedSection:
        require(children.size() == 1 && node(d, children[0]).kind == ValueKind::Words, "Invalid indexed text preamble"); break;
    default: throw std::runtime_error("Unknown typed value kind");
    }
}
std::optional<std::uint32_t> literalIndex(const ProgramData& d, ValueId id) {
    const auto& e = node(d, id);
    if (e.kind == ValueKind::Word) return static_cast<std::uint32_t>(e.data);
    if (e.kind != ValueKind::Expression || d.child(id).size() != 1) return {};
    const auto op = d.child(id).front(); const auto& v = node(d, op);
    if (v.kind != ValueKind::ValueOperation) return {};
    const auto kind = static_cast<sct::SctScptValueKind>(v.detail);
    if (kind == sct::SctScptValueKind::DecimalLiteral && d.child(op).empty() && (v.data & 0x8000ff) == 0)
        return static_cast<std::uint32_t>((v.data >> 8) & 0x7fff);
    if (kind == sct::SctScptValueKind::FloatLiteral && d.child(op).size() == 1) {
        const double value = std::bit_cast<float>(static_cast<std::uint32_t>(node(d, d.child(op)[0]).data));
        if (std::isfinite(value) && value >= 0 && value <= UINT32_MAX && std::floor(value) == value) return static_cast<std::uint32_t>(value);
    }
    return {}; // Inline sentinels and computed expressions are not fixed indices.
}
std::optional<Variable> read(const Value& op) {
    if (op.kind != ValueKind::ValueOperation) return {};
    auto kind = static_cast<sct::SctScptValueKind>(op.detail);
    if (kind == sct::SctScptValueKind::BitVariable) return Variable{Bank::Bit, static_cast<std::uint32_t>(op.data & 0x1fffffff)};
    if (kind == sct::SctScptValueKind::ByteVariable) return Variable{Bank::Byte, static_cast<std::uint32_t>(op.data & 0x0fffffff)};
    if (kind == sct::SctScptValueKind::FloatVariable) return Variable{Bank::Float, static_cast<std::uint32_t>(op.data & 0x0fffffff)};
    if (kind == sct::SctScptValueKind::IntegerVariable || kind == sct::SctScptValueKind::IntegerVariableLow16Comparison || kind == sct::SctScptValueKind::FloatBackedIntegerVariable)
        return Variable{Bank::Integer, static_cast<std::uint32_t>(op.data & 0x00ffffff)};
    return {};
}
}
void valueShape(const ProgramData& d, ValueId id) { shape(d, id); }
DirectFacts actionFacts(ProgramData& d, std::span<const SctActionId> actions, std::vector<Diagnostic>& diagnostics) {
    DirectFacts facts;
    auto inspect = [&](ValueId id, Site site, SctScriptId script) {
        const auto& v = node(d, id);
        if (v.kind == ValueKind::Expression) {
            std::uint32_t ordinal = 0;
            for (auto op : d.child(id)) {
                site.operation = ordinal++;
                if (auto key = read(node(d, op))) facts.accesses.push_back({site, Access::Read, id, key, key->bank});
            }
        } else if (v.kind == ValueKind::ActionReference) {
            auto* target = d.action(SctActionId{v.data});
            require(target && d.sequence(target->owner)->script == script, "Dangling or cross-script resolved instruction target");
            facts.targets.push_back({site, SctActionId{v.data}});
        } else if (v.kind == ValueKind::IndexedTextReference || v.kind == ValueKind::SupplementaryTextReference) {
            auto* target = d.lookup(v.data, EntityKind::Text, d.texts);
            require(target && target->script == script && target->supplementary == (v.kind == ValueKind::SupplementaryTextReference), "Dangling or incompatible resolved text target");
        } else if (v.kind == ValueKind::Unresolved) {
            diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding,
                "Unresolved operand in action " + std::to_string(site.action.value)});
        }
    };
    for (auto id : actions) {
        const auto* found = d.action(id); if (!found) continue;
        const auto& a = *found; const auto script = d.sequence(a.owner)->script;
        auto condition = [&](ConditionId id) {
            const auto* region = d.lookup(id.value, EntityKind::Condition, d.conditions);
            require(region && region->owner == a.id,
                    ("Missing or foreign condition region " + std::to_string(id.value) + " for action " +
                     std::to_string(a.id.value))
                        .c_str());
            inspect(region->result, {a.id, {}, region->arm, 0, id}, script);
        };
        if (a.kind == ActionKind::Conditional || a.kind == ActionKind::Dispatch) {
            for (std::uint32_t j = 0; j < a.armCount; ++j) {
                const auto& arm = d.arms[a.firstArm + j];
                if (arm.condition.valid()) condition(arm.condition);
            }
            continue;
        }
        if (a.kind == ActionKind::Loop) { condition(d.loops.at(a.detail).condition); continue; }
        if (a.kind == ActionKind::Switch) { condition(d.switches.at(a.detail).selector); continue; }
        if (a.kind == ActionKind::Transfer) {
            const auto& transfer = d.transfers.at(a.detail);
            facts.targets.push_back({{a.id}, transfer.target, transfer.kind == TransferKind::Break ? TargetUseKind::Break : TargetUseKind::Continue});
            continue;
        }
        const auto& instruction = node(d, a.value);
        auto parameters = d.child(a.value); std::size_t start = 0;
        if (instruction.detail & 2) { inspect(parameters.front(), {a.id}, script); start = 1; }
        std::optional<std::uint32_t> group;
        auto parameter = [&](ValueId id, std::optional<std::uint32_t> repeated) {
            const auto& p = node(d, id); const auto value = d.child(id).front();
            Site site{a.id, sct::SctParameterAddress{static_cast<std::uint32_t>(p.data), repeated}};
            inspect(value, site, script);
            const auto opcode = instruction.data;
            // SALSA's existing destination rules: 5/6/7 assign Byte/Int/Float,
            // 17/18 set/clear Bit; 19 is a read-modify write with unspecified result.
            if (!repeated && p.data == 0 && ((opcode >= 5 && opcode <= 7) || (opcode >= 17 && opcode <= 19))) {
                Bank bank = opcode >= 17 ? Bank::Bit : opcode == 5 ? Bank::Byte : opcode == 6 ? Bank::Integer : Bank::Float;
                auto index = literalIndex(d, value);
                VariableUse use{site, opcode == 17 ? Access::Set : opcode == 18 ? Access::Clear : opcode == 19 ? Access::Modify : Access::Assign, value, {}, bank};
                if (index) {
                    use.variable = Variable{bank, bank == Bank::Bit ? *index : *index & 65535};
                    facts.accesses.push_back(use);
                } else { use.access = Access::ComputedWrite; facts.computed.push_back(use); }
            }
        };
        for (; start < parameters.size(); ++start) {
            if (node(d, parameters[start]).kind == ValueKind::RepeatedGroup) {
                group = group ? *group + 1 : 0;
                for (auto p : d.child(parameters[start])) parameter(p, group);
            } else parameter(parameters[start], {});
        }
    }
    return facts;
}
void buildIndexes(ProgramData& d, std::vector<Diagnostic>& diagnostics, bool checkValueShapes) {
    if (checkValueShapes) for (std::uint32_t i = 0; i < d.values.size(); ++i) shape(d, ValueId{i});
    std::vector<SctActionId> ids;
    for (std::uint32_t i = 0; i < d.actions.size(); ++i) if (auto id = d.actions.find(i)->id; id.valid()) ids.push_back(id);
    auto facts = actionFacts(d, ids, diagnostics);
    d.accesses = std::move(facts.accesses); d.computed = std::move(facts.computed); d.targetUses = std::move(facts.targets);
    d.variableUses.clear(); d.incomingRanges.clear();
    finalizePostings(d);
    if (d.stats.unavailableItems || d.stats.excludedSourceBytes) diagnostics.push_back({DiagnosticSeverity::Warning,
        DiagnosticCode::SctSourceArtifactOmitted, "Source-only bytes were excluded; unavailable typed content remains explicit in this draft."});
}
void finalizePostings(ProgramData& d) {
    d.variableUses.clear(); d.incomingRanges.clear();
    // Contiguous postings avoid one vector allocation per referenced action.
    // Range replacement/reclamation is supplied by WP2; these are bulk indexes.
    std::stable_sort(d.accesses.begin(), d.accesses.end(), [](const VariableUse& a, const VariableUse& b) { return a.variable < b.variable; });
    for (std::uint32_t i = 0; i < count32(d.accesses.size()); ++i) {
        auto [it, inserted] = d.variableUses.try_emplace(*d.accesses[i].variable, Range{i, 0});
        ++it->second.count;
    }
    std::stable_sort(d.targetUses.begin(), d.targetUses.end(), [](const TargetUse& a, const TargetUse& b) { return a.target < b.target; });
    d.incomingRanges.resize(d.actions.size());
    for (std::uint32_t i = 0; i < count32(d.targetUses.size()); ++i) {
        auto& range = d.incomingRanges[d.directory.find(d.targetUses[i].target.value).index];
        if (!range.count) range.first = i;
        ++range.count;
    }
}
Result<std::uint64_t> ProgramBuilder::allocateIdentity() {
    try { require(data_ != nullptr, "Builder already finished"); return Result<std::uint64_t>::success(data_->allocate()); }
    catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<std::uint64_t>::failure(diagnostics_.back()); }
}
void validateDispatch(const ProgramData &d, const Dispatch &region) {
    const auto *root = d.action(region.owner);
    require(root && root->kind == ActionKind::Dispatch && region.routes.size() == root->armCount,
            "Invalid dispatch structure");
    require(!region.routes.empty() && region.routes.size() <= 16 && region.samples.size() <= 32,
            "Dispatch exceeds its bounded region limits");
    std::set<ConditionId> samples;
    for (const auto &sample : region.samples) {
        const auto *c = d.lookup(sample.condition.value, EntityKind::Condition, d.conditions);
        require(c && c->owner == root->id && !c->arm && sample.beforeRoute < region.routes.size() &&
                    samples.insert(c->id).second,
                "Invalid dispatch evaluation sample");
        require(d.lookup(c->preparation.value, EntityKind::Sequence, d.sequences)->order->empty(), "Dispatch timing samples cannot contain instructions");
        dispatch_detail::parse(dispatch_detail::expression(d, c->result));
    }
    for (std::size_t i = 0; i < region.routes.size(); ++i) {
        const auto &route = region.routes[i];
        const auto &arm = d.arms[root->firstArm + i];
        require(route.arm == arm.id && arm.owner == root->id && arm.condition.valid(),
                "Invalid dispatch route ownership");
        const auto *c = d.lookup(arm.condition.value, EntityKind::Condition, d.conditions);
        require(c && c->owner == root->id && c->arm == arm.id, "Invalid route predicate ownership");
        require(d.lookup(c->preparation.value, EntityKind::Sequence, d.sequences)->order->empty(),
                "Dispatch route preparation belongs in its body, not in its pure predicate");
        dispatch_detail::parse(dispatch_detail::expression(d, c->result));
        std::map<dispatch_detail::Code, std::uint32_t> phases;
        for (auto id : route.samples) {
            require(samples.contains(id), "Foreign dispatch sample binding");
            auto phase = std::ranges::find(region.samples, id, &DispatchSample::condition)->beforeRoute;
            require(phase <= i, "Dispatch sample occurs after its route");
            auto code =
                dispatch_detail::parse(dispatch_detail::expression(
                                           d, d.lookup(id.value, EntityKind::Condition, d.conditions)->result))
                    ->code;
            auto [at, inserted] = phases.emplace(std::move(code), phase);
            require(inserted || at->second == phase,
                    "Repeated comparison at different evaluation phases needs an explicit dispatch region");
        }
    }
}
void finishMetadata(ProgramData &d, std::vector<Diagnostic> &diagnostics, bool allocateIds, bool checkDispatches) {
    auto claim = [&](std::uint64_t id, EntityKind kind, std::uint32_t index) {
        if (allocateIds) return d.claim(id, kind, index);
        auto at=d.directory.find(id); require(id && at.kind==kind && at.index==index, "Invalid metadata identity directory"); return id;
    };
    auto script = [&](SctScriptId id) { require(d.directory.find(id.value).kind == EntityKind::Script, "Missing metadata script owner"); };
    auto sequence = [&](SctSequenceId id, SctScriptId owner) { auto* s = d.sequence(id); require(s && s->script == owner, "Missing or foreign sequence owner"); };
    auto action = [&](SctActionId id, SctScriptId owner) { auto* a = d.action(id); require(a && d.sequence(a->owner)->script == owner, "Missing or foreign action reference"); };
    auto operand = [&](SctActionId id, spice::sct::SctParameterAddress address) {
        const auto* a = d.action(id); require(a && a->kind == ActionKind::Instruction, "Operand needs an instruction action");
        auto children = d.child(a->value); std::uint32_t group = 0;
        for (auto child : children) {
            const auto& v = node(d, child);
            if (!address.repeatedGroupOrdinal && v.kind == ValueKind::Parameter && v.data == address.schemaIndex) return;
            if (v.kind == ValueKind::RepeatedGroup) {
                if (address.repeatedGroupOrdinal == group) for (auto p : d.child(child))
                    if (node(d, p).kind == ValueKind::Parameter && node(d, p).data == address.schemaIndex) return;
                ++group;
            }
        }
        throw std::runtime_error("Authored operand site does not exist");
    };
    auto site = [&](const Site& site, SctScriptId owner) {
        action(site.action, owner);
        require(!((site.arm || site.condition) && site.parameter), "Site cannot be an arm condition and a parameter");
        if (site.arm) { auto* arm = d.lookup(site.arm->value, EntityKind::Arm, d.arms); require(arm && arm->owner == site.action, "Foreign arm reference"); }
        if (site.condition) {
            const auto* c = d.lookup(site.condition->value, EntityKind::Condition, d.conditions);
            require(c && c->owner == site.action && c->arm == site.arm, "Foreign condition site");
        }
        if (site.parameter) operand(site.action, *site.parameter);
    };
    std::set<SctSequenceId> moduleOwners;
    for (std::uint32_t i = 0; i < d.metadata.modules.size(); ++i) {
        auto& m = d.metadata.modules[i]; script(m.script); m.id = SctModuleId{claim(m.id.value, EntityKind::Module, i)};
        for (auto id : m.sequences) { sequence(id, m.script); require(moduleOwners.insert(id).second, "Multiple module owners for a sequence"); }
    }
    for (auto id : moduleOwners) {
        auto* current = d.sequence(id);
        while (current->owner) {
            current = d.sequence(d.action(d.parentAction(*current))->owner);
            require(!moduleOwners.contains(current->id), "Module ownership overlaps an enclosing sequence");
        }
    }
    for (std::uint32_t i = 0; i < d.metadata.languages.size(); ++i) {
        auto& l = d.metadata.languages[i]; l.id = SctLanguageGroupId{claim(l.id.value, EntityKind::Language, i)};
    }
    // Edge ownership includes the repeated group ordinal and physical fallthrough.
    std::set<std::tuple<std::uint64_t, std::optional<spice::sct::SctParameterAddress>>> edges;
    std::set<std::pair<SctScriptId, std::string>> named;
    for (std::uint32_t i = 0; i < d.metadata.entrypoints.size(); ++i) {
        auto& e = d.metadata.entrypoints[i]; script(e.script); sequence(e.body, e.script);
        e.id = SctEntrypointId{claim(e.id.value, EntityKind::Entry, i)};
        if (e.kind == SctEntrypointKind::Lifecycle) {
            require(e.required() && e.enabled, "init and loop are required, always-enabled lifecycle roots");
            require(std::ranges::any_of(d.sections, [&](const Section& s) { return s.script == e.script && s.name == e.name && s.body == e.body; }), "Lifecycle body must be its named root section");
        }
        if (e.responseSite) site(*e.responseSite, e.script);
        if (e.predicate) site(*e.predicate, e.script);
        if (auto* n = std::get_if<SctNamedActivation>(&e.activation)) {
            action(n->body, e.script); require(named.emplace(e.script, n->externalName).second, "Duplicate named activation owner");
            if (e.required()) require(n->externalName == e.name, "Lifecycle external name changed");
        } else if (auto* local = std::get_if<SctLocalActivation>(&e.activation)) {
            require(!e.required(), "Lifecycle roots are engine managed");
            action(local->source, e.script); action(local->start, e.script); action(local->continuation, e.script);
            if (local->operand) operand(local->source, *local->operand);
            require(edges.emplace(local->source.value, local->operand).second, "Duplicate activation edge owner");
        } else if (!e.required()) diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding,
            "Unresolved activation for entrypoint " + std::to_string(e.id.value)});
    }
    for (std::uint32_t i = 0; i < d.metadata.ports.size(); ++i) {
        auto& p = d.metadata.ports[i]; p.id = SctPortId{claim(p.id.value, EntityKind::Port, i)};
        SctScriptId owner = std::visit([&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, SctModuleId>) { auto* m = d.lookup(id.value, EntityKind::Module, d.metadata.modules); require(m != nullptr, "Dangling port module"); return m->script; }
            else { auto* e = d.lookup(id.value, EntityKind::Entry, d.metadata.entrypoints); require(e != nullptr, "Dangling port entrypoint"); return e->script; }
        }, p.owner);
        if (p.action) action(*p.action, owner);
        if (p.sequence) sequence(*p.sequence, owner);
        if(p.role!=SlotRole::Generic) {
            require(p.action || p.sequence,"A typed slot needs an action or sequence anchor");
            if(p.action && p.sequence) require(d.action(*p.action)->owner==*p.sequence,"Slot anchors disagree");
            if(auto id=std::get_if<SctModuleId>(&p.owner)) {
                const auto* m=d.lookup(id->value,EntityKind::Module,d.metadata.modules);
                auto seq=p.sequence.value_or(p.action?d.action(*p.action)->owner:SctSequenceId{});bool belongs=false;
                while(auto* current=d.sequence(seq)) {
                    if(std::ranges::find(m->sequences,seq)!=m->sequences.end()){belongs=true;break;}
                    if(!current->owner)break;seq=d.action(d.parentAction(*current))->owner;
                }
                require(belongs,"Slot is outside its module");
            }
            if(p.role==SlotRole::Exit && p.boundary==SlotBoundary::Operand) {require(p.action && p.operand,"Operand exit needs an exact operand");operand(*p.action,*p.operand);}
            if(p.role==SlotRole::Exit && p.boundary==SlotBoundary::SequenceEnd) require(p.sequence.has_value(),"Sequence-end exit needs a sequence");
            if(p.role==SlotRole::Exit && (p.boundary==SlotBoundary::Terminal || p.boundary==SlotBoundary::Fallthrough)) require(p.action.has_value(),"Action boundary needs an action");
        }
    }
    for (const auto& e : d.metadata.entrypoints) if (const auto* port = std::get_if<SctPortId>(&e.response))
        require(d.directory.find(port->value).kind == EntityKind::Port, "Dangling resolved response port");
    std::set<SctTextResourceId> indexedOwners;
    for (const auto& section : d.sections) {
        if (!section.content.valid()) continue;
        const auto& v = node(d, section.content);
        if (v.kind == ValueKind::IndexedSection) {
            const auto* t = d.lookup(v.data, EntityKind::Text, d.texts);
            require(t && !t->supplementary && t->script == section.script && indexedOwners.insert(t->id).second, "Invalid indexed text section ownership");
        }
    }
    for (const auto& t : d.texts) {
        const auto& v = node(d, t.value);
        require(v.kind == ValueKind::PlainText || v.kind == ValueKind::Message || v.kind == ValueKind::EmptyIndexedText || v.kind == ValueKind::Unavailable, "Invalid text entity payload");
        require(t.supplementary || indexedOwners.contains(t.id), "Indexed text has no section owner");
        for (auto language : t.languages) require(d.directory.find(language.value).kind == EntityKind::Language, "Dangling language membership");
    }
    for (const auto& s : d.scripts) {
        for (auto name : {"init", "loop"}) {
            auto count = std::ranges::count_if(d.metadata.entrypoints, [&](const Entrypoint& e) { return e.script == s.context.id && e.required() && e.name == name; });
            require(count <= 1, "Duplicate lifecycle root");
            if (!count) diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding,
                s.context.name + " is an incomplete draft: missing " + name});
        }
    }
    std::set<std::tuple<std::uint64_t,SlotBoundary,std::optional<spice::sct::SctParameterAddress>>> connectionEdges;
    std::set<std::pair<SctActionId,std::optional<spice::sct::SctParameterAddress>>> implementedEdges;
    for (std::uint32_t i = 0; i < d.metadata.connections.size(); ++i) {
        auto& c = d.metadata.connections[i]; c.id = SctConnectionId{claim(c.id.value, EntityKind::Connection, i)};
        for (const auto* reference : {&c.source, &c.destination}) {
            if (const auto* id = std::get_if<SctPortId>(reference)) require(d.directory.find(id->value).kind == EntityKind::Port, "Dangling resolved connection port");
            else if(!(reference==&c.destination && c.cutscene && (c.cutscene->outcome==RouteOutcome::ReturnToCaller || c.cutscene->outcome==RouteOutcome::ResumeLoop))) diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding, "Unresolved connection " + std::to_string(c.id.value)});
        }
        if(c.cutscene) {
            auto port=[&](const auto& ref)->const Port* {auto* id=std::get_if<SctPortId>(&ref);return id?d.lookup(id->value,EntityKind::Port,d.metadata.ports):nullptr;};
            auto* source=port(c.source);auto* destination=port(c.destination);
            if(source) require(source->role==SlotRole::Exit,"Cutscene route source must be an exit slot");
            if(destination) require(destination->role==SlotRole::Entry,"Cutscene route destination must be an entry slot");
            const auto& r=*c.cutscene;
            auto anyAction=[&](SctActionId id) {require(d.action(id),"Dangling route action");};
            if(r.implementation) {
                require(source && r.outcome!=RouteOutcome::Observe,"Executable connection needs a source and outcome");anyAction(*r.implementation);
                require(implementedEdges.emplace(*r.implementation,r.targetOperand).second,"Connections overlap the same implemented edge");
                auto ownerScript=std::visit([&](auto id){using T=decltype(id);if constexpr(std::is_same_v<T,SctModuleId>)return d.lookup(id.value,EntityKind::Module,d.metadata.modules)->script;else return d.lookup(id.value,EntityKind::Entry,d.metadata.entrypoints)->script;},source->owner);
                action(*r.implementation,ownerScript);
                auto anchor=source->boundary==SlotBoundary::SequenceEnd?source->sequence->value:source->action->value;
                require(connectionEdges.emplace(anchor,source->boundary,source->operand).second,"Duplicate executable connection edge owner");
            }
            if (r.dispatchArm) {
                auto *arm = d.lookup(r.dispatchArm->value, EntityKind::Arm, d.arms);
                require(arm && d.action(arm->owner)->kind == ActionKind::Dispatch,
                        "Dangling route-owned predicate");
                if(source) {
                    auto ownerScript=std::visit([&](auto id){using T=decltype(id);if constexpr(std::is_same_v<T,SctModuleId>)return d.lookup(id.value,EntityKind::Module,d.metadata.modules)->script;else return d.lookup(id.value,EntityKind::Entry,d.metadata.entrypoints)->script;},source->owner);
                    action(arm->owner,ownerScript);
                }
            }
            if(r.boundTarget)anyAction(*r.boundTarget);
            if(r.targetOperand){require(r.implementation.has_value(),"Route operand needs an implementation action");operand(*r.implementation,*r.targetOperand);}
            for(auto id:{r.entrypoint,r.arrival}) if(id) require(d.directory.find(id->value).kind==EntityKind::Entry,"Dangling route entrypoint");
            for(const auto& path:r.paths) {
                for(const auto& q:path.requirements){anyAction(q.site.action);site(q.site,d.sequence(d.action(q.site.action)->owner)->script);}
                for(auto id:path.preparation)anyAction(id);
                for(auto id:path.consumption)anyAction(id);
            }
        }
        if (c.route) {
            const auto& r = *c.route; script(r.sourceScript); script(r.destinationScript); action(r.sourceAction, r.sourceScript);
            operand(r.sourceAction, r.sourceOperand);
            const auto* e = d.lookup(r.arrival.value, EntityKind::Entry, d.metadata.entrypoints);
            require(e && e->script == r.destinationScript, "Dangling route arrival");
            for (const auto& o : r.observations) action(o.action, o.script);
        }
    }
    for (std::uint32_t i = 0; i < d.metadata.logicalTexts.size(); ++i) {
        auto& l = d.metadata.logicalTexts[i]; script(l.script); l.id = SctLogicalTextId{claim(l.id.value, EntityKind::LogicalText, i)};
        for (auto id : l.variants) { const auto* t = d.lookup(id.value, EntityKind::Text, d.texts); require(t && t->script == l.script, "Dangling logical text variant"); }
        if (l.module) { const auto* m = d.lookup(l.module->value, EntityKind::Module, d.metadata.modules); require(m && m->script == l.script, "Dangling logical text module"); }
    }
    std::set<SctLanguageGroupId> configuredLanguages;
    std::set<std::uint32_t> selectorValues;
    for (const auto &rule : d.metadata.localization.languages) {
        require(d.lookup(rule.language.value, EntityKind::Language, d.metadata.languages) &&
                    configuredLanguages.insert(rule.language).second,
                "Missing or repeated localization language");
        if (rule.selector)
            require(*rule.selector <= 255 && selectorValues.insert(*rule.selector).second,
                    "ByteVar[87] values must be unique bytes");
    }
    std::set<SctActionId> localizedActions;
    for (const auto &block : d.metadata.localization.blocks) {
        auto *a = d.action(block.action);
        require(a && (a->kind == ActionKind::Conditional || a->kind == ActionKind::Switch) &&
                    a->armCount == 1 && d.arms[a->firstArm].body == block.body &&
                    localizedActions.insert(block.action).second,
                "Localized block needs its single canonical body; edit localization settings rather than its "
                "dispatch arms");
        require(block.switchSelection == (a->kind == ActionKind::Switch) &&
                    (!block.independent || !block.switchSelection) &&
                    (!block.unconditional || (!block.switchSelection && !block.independent)),
                "Invalid localization selection mode");
        std::set<SctLanguageGroupId> languages;
        require(!block.languages.empty(), "Localized block needs its source language");
        for (auto language : block.languages)
            require(d.lookup(language.value, EntityKind::Language, d.metadata.languages) &&
                        languages.insert(language).second,
                    "Missing or repeated localized block language");
    }
    std::set<std::pair<SctActionId, spice::sct::SctParameterAddress>> localizedUses;
    for (const auto &use : d.metadata.localization.uses) {
        auto *a = d.action(use.action);
        auto *m = d.lookup(use.message.value, EntityKind::LogicalText, d.metadata.logicalTexts);
        require(a && m && d.sequence(a->owner)->script == m->script &&
                    localizedUses.emplace(use.action, use.operand).second,
                "Invalid logical message use");
        operand(use.action, use.operand);
    }
    std::set<std::tuple<SctScriptId,SctVariableKey,std::optional<std::int64_t>>> meaningKeys;
    for(std::uint32_t i=0;i<d.metadata.meanings.size();++i) {
        auto& meaning=d.metadata.meanings[i];meaning.id=SctMeaningId{claim(meaning.id.value,EntityKind::Meaning,i)};
        if(meaning.script.valid())script(meaning.script);
        require(static_cast<unsigned>(meaning.variable.kind)<=3 && !meaning.name.empty(),"Invalid variable meaning");
        require(meaningKeys.emplace(meaning.script,meaning.variable,meaning.value).second,"Duplicate variable meaning scope and value");
    }
    std::set<SctModuleId> narrative;
    for(auto id:d.metadata.narrative) require(d.lookup(id.value,EntityKind::Module,d.metadata.modules) && narrative.insert(id).second,"Invalid narrative module");
    for(const auto &record:d.metadata.interpretations) require(static_cast<unsigned>(record.decision)<=2 && !record.explanation.empty(),"Invalid interpretation record");
    if (checkDispatches)
        for (const auto &region : d.dispatches)
            if (region.owner.valid())
                validateDispatch(d, region);
    std::set<SectionId> folderSections;
    for (std::size_t i = 0; i < d.metadata.folders.size(); ++i) {
        const auto &f = d.metadata.folders[i];
        require(d.lookup(f.script.value, EntityKind::Script, d.scripts) && !f.name.empty(), "Invalid folder script or name");
        if (f.parent) require(*f.parent < i && d.metadata.folders[*f.parent].script == f.script, "Invalid folder parent");
        for (auto id : f.sections) {
            auto *section = d.lookup(id.value, EntityKind::Section, d.sections);
            require(section && section->script == f.script && folderSections.insert(id).second, "Invalid or repeated folder section");
        }
    }
    std::set<std::uint16_t> customized;
    for (const auto &c : d.metadata.opcodes) {
        require(spice::sct::findSctOpcodeSchema(c.opcode) && customized.insert(c.opcode).second, "Invalid or duplicate catalog opcode");
        require(!c.color || *c.color <= 0xffffff, "Invalid opcode color");
    }
    for (const auto &a : d.metadata.annotations)
        require(!a.color || *a.color <= 0xffffff, "Invalid item color");
    for (const auto& a : d.metadata.annotations) std::visit([&](auto id) {
        using T = decltype(id); auto actual = d.directory.find(id.value).kind;
        auto expected = [] {
            if constexpr (std::is_same_v<T, SctScriptId>) return EntityKind::Script;
            else if constexpr (std::is_same_v<T, SectionId>) return EntityKind::Section;
            else if constexpr (std::is_same_v<T, SctSequenceId>) return EntityKind::Sequence;
            else if constexpr (std::is_same_v<T, SctActionId>) return EntityKind::Action;
            else if constexpr (std::is_same_v<T, ArmId>) return EntityKind::Arm;
            else if constexpr (std::is_same_v<T, ConditionId>) return EntityKind::Condition;
            else if constexpr (std::is_same_v<T, SctModuleId>) return EntityKind::Module;
            else if constexpr (std::is_same_v<T, SctEntrypointId>) return EntityKind::Entry;
            else if constexpr (std::is_same_v<T, SctPortId>) return EntityKind::Port;
            else if constexpr (std::is_same_v<T, SctConnectionId>) return EntityKind::Connection;
            else return EntityKind::Text;
        }();
        require(actual == expected, "Dangling or mistyped annotation subject");
    }, a.subject);
}
} // namespace salsa::core::program
