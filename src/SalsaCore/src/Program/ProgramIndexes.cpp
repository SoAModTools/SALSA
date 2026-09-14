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
void buildIndexes(ProgramData& d, std::vector<Diagnostic>& diagnostics) {
    // Shape checks and connection extraction happen once at bulk construction.
    // WP2 uses the same local contribution rules for changed records only.
    for (std::uint32_t i = 0; i < d.values.size(); ++i) shape(d, ValueId{i});
    auto inspect = [&](ValueId id, Site site, SctScriptId script) {
        const auto& v = node(d, id);
        if (v.kind == ValueKind::Expression) {
            std::uint32_t ordinal = 0;
            for (auto op : d.child(id)) {
                site.operation = ordinal++;
                if (auto key = read(node(d, op))) d.accesses.push_back({site, Access::Read, id, key, key->bank});
            }
        } else if (v.kind == ValueKind::ActionReference) {
            auto* target = d.action(SctActionId{v.data});
            require(target && d.sequence(target->owner)->script == script, "Dangling or cross-script resolved instruction target");
            d.targetUses.push_back({site, SctActionId{v.data}});
        } else if (v.kind == ValueKind::IndexedTextReference || v.kind == ValueKind::SupplementaryTextReference) {
            auto* target = d.lookup(v.data, EntityKind::Text, d.texts);
            require(target && target->script == script && target->supplementary == (v.kind == ValueKind::SupplementaryTextReference), "Dangling or incompatible resolved text target");
        } else if (v.kind == ValueKind::Unresolved) {
            diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding,
                "Unresolved operand in action " + std::to_string(site.action.value)});
        }
    };
    for (std::uint32_t i = 0; i < d.actions.size(); ++i) {
        const auto& a = *d.actions.find(i); const auto script = d.sequence(a.owner)->script;
        if (a.kind == ActionKind::Conditional) {
            for (std::uint32_t j = 0; j < a.armCount; ++j) {
                const auto& arm = d.arms[a.firstArm + j];
                if (arm.condition.valid()) inspect(arm.condition, {a.id, {}, arm.id}, script);
            }
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
                    d.accesses.push_back(use);
                } else { use.access = Access::ComputedWrite; d.computed.push_back(use); }
            }
        };
        for (; start < parameters.size(); ++start) {
            if (node(d, parameters[start]).kind == ValueKind::RepeatedGroup) {
                group = group ? *group + 1 : 0;
                for (auto p : d.child(parameters[start])) parameter(p, group);
            } else parameter(parameters[start], {});
        }
    }
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
    if (d.stats.unavailableItems || d.stats.excludedSourceBytes) diagnostics.push_back({DiagnosticSeverity::Warning,
        DiagnosticCode::SctSourceArtifactOmitted, "Source-only bytes were excluded; unavailable typed content remains explicit in this draft."});
}
Result<std::uint64_t> ProgramBuilder::allocateIdentity() {
    try { require(data_ != nullptr, "Builder already finished"); return Result<std::uint64_t>::success(data_->allocate()); }
    catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<std::uint64_t>::failure(diagnostics_.back()); }
}
void finishMetadata(ProgramData& d, std::vector<Diagnostic>& diagnostics) {
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
        require(!(site.arm && site.parameter), "Site cannot be an arm condition and a parameter");
        if (site.arm) { auto* arm = d.lookup(site.arm->value, EntityKind::Arm, d.arms); require(arm && arm->owner == site.action, "Foreign arm reference"); }
        if (site.parameter) operand(site.action, *site.parameter);
    };
    std::set<SctSequenceId> moduleOwners;
    for (std::uint32_t i = 0; i < d.metadata.modules.size(); ++i) {
        auto& m = d.metadata.modules[i]; script(m.script); m.id = SctModuleId{d.claim(m.id.value, EntityKind::Module, i)};
        for (auto id : m.sequences) { sequence(id, m.script); require(moduleOwners.insert(id).second, "Multiple module owners for a sequence"); }
    }
    for (auto id : moduleOwners) {
        auto* current = d.sequence(id);
        while (current->owner) {
            const auto* arm = d.lookup(current->owner->value, EntityKind::Arm, d.arms);
            current = d.sequence(d.action(arm->owner)->owner);
            require(!moduleOwners.contains(current->id), "Module ownership overlaps an enclosing sequence");
        }
    }
    for (std::uint32_t i = 0; i < d.metadata.languages.size(); ++i) {
        auto& l = d.metadata.languages[i]; l.id = SctLanguageGroupId{d.claim(l.id.value, EntityKind::Language, i)};
    }
    // Edge ownership includes the repeated group ordinal and physical fallthrough.
    std::set<std::tuple<std::uint64_t, std::optional<spice::sct::SctParameterAddress>>> edges;
    std::set<std::pair<SctScriptId, std::string>> named;
    for (std::uint32_t i = 0; i < d.metadata.entrypoints.size(); ++i) {
        auto& e = d.metadata.entrypoints[i]; script(e.script); sequence(e.body, e.script);
        e.id = SctEntrypointId{d.claim(e.id.value, EntityKind::Entry, i)};
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
        auto& p = d.metadata.ports[i]; p.id = SctPortId{d.claim(p.id.value, EntityKind::Port, i)};
        SctScriptId owner = std::visit([&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, SctModuleId>) { auto* m = d.lookup(id.value, EntityKind::Module, d.metadata.modules); require(m != nullptr, "Dangling port module"); return m->script; }
            else { auto* e = d.lookup(id.value, EntityKind::Entry, d.metadata.entrypoints); require(e != nullptr, "Dangling port entrypoint"); return e->script; }
        }, p.owner);
        if (p.action) action(*p.action, owner);
        if (p.sequence) sequence(*p.sequence, owner);
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
    for (std::uint32_t i = 0; i < d.metadata.connections.size(); ++i) {
        auto& c = d.metadata.connections[i]; c.id = SctConnectionId{d.claim(c.id.value, EntityKind::Connection, i)};
        for (const auto* reference : {&c.source, &c.destination}) {
            if (const auto* id = std::get_if<SctPortId>(reference)) require(d.directory.find(id->value).kind == EntityKind::Port, "Dangling resolved connection port");
            else diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding, "Unresolved connection " + std::to_string(c.id.value)});
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
        auto& l = d.metadata.logicalTexts[i]; script(l.script); l.id = SctLogicalTextId{d.claim(l.id.value, EntityKind::LogicalText, i)};
        for (auto id : l.variants) { const auto* t = d.lookup(id.value, EntityKind::Text, d.texts); require(t && t->script == l.script, "Dangling logical text variant"); }
        if (l.module) { const auto* m = d.lookup(l.module->value, EntityKind::Module, d.metadata.modules); require(m && m->script == l.script, "Dangling logical text module"); }
    }
    for (const auto& a : d.metadata.annotations) std::visit([&](auto id) {
        using T = decltype(id); auto actual = d.directory.find(id.value).kind;
        auto expected = [] {
            if constexpr (std::is_same_v<T, SctScriptId>) return EntityKind::Script;
            else if constexpr (std::is_same_v<T, SectionId>) return EntityKind::Section;
            else if constexpr (std::is_same_v<T, SctSequenceId>) return EntityKind::Sequence;
            else if constexpr (std::is_same_v<T, SctActionId>) return EntityKind::Action;
            else if constexpr (std::is_same_v<T, ArmId>) return EntityKind::Arm;
            else if constexpr (std::is_same_v<T, SctModuleId>) return EntityKind::Module;
            else if constexpr (std::is_same_v<T, SctEntrypointId>) return EntityKind::Entry;
            else return EntityKind::Text;
        }();
        require(actual == expected, "Dangling or mistyped annotation subject");
    }, a.subject);
}
} // namespace salsa::core::program
