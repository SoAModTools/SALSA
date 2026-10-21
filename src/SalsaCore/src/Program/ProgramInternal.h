#pragma once
#include "SalsaCore/Program/Program.h"
#include "ProgramGrouping.h"
#include <array>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace salsa::core::program {
inline void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
inline std::uint32_t count32(std::size_t n) {
    require(n < UINT32_MAX, "Program table exceeds its 32-bit range"); return static_cast<std::uint32_t>(n);
}
inline Diagnostic failure(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)};
}
template<class T> class Pages {
public:
    static constexpr std::uint32_t PageSize = 256;
    Pages() = default;
    Pages(const Pages& other) : size_(other.size_) { for (const auto& p : other.pages_) pages_.push_back(std::make_unique<std::array<T, PageSize>>(*p)); }
    Pages(Pages&&) noexcept = default;
    Pages& operator=(Pages&&) noexcept = default;
    std::uint32_t size() const noexcept { return size_; }
    std::size_t pageCount() const noexcept { return pages_.size(); }
    const T* find(std::uint32_t i) const noexcept { return i < size_ ? &(*pages_[i / PageSize])[i % PageSize] : nullptr; }
    T* find(std::uint32_t i) noexcept { return const_cast<T*>(std::as_const(*this).find(i)); }
    std::uint32_t append(T row) {
        require(size_ != UINT32_MAX, "Program page table is full");
        if (size_ % PageSize == 0) pages_.push_back(std::make_unique<std::array<T, PageSize>>());
        (*pages_.back())[size_ % PageSize] = std::move(row); return size_++;
    }
private:
    std::vector<std::unique_ptr<std::array<T, PageSize>>> pages_;
    std::uint32_t size_ = 0;
};
enum class EntityKind : std::uint8_t { None, Script, Baseline, Section, Sequence, Action, Arm, Text, Module, Entry, Port, Language, Connection, LogicalText, Condition, Meaning };
struct Location { EntityKind kind = EntityKind::None; std::uint32_t index = 0; };
// Sparse 64-bit identity support, one allocation per occupied directory page,
// not a hash node/allocation per instruction. Zero is always absent.
class Directory {
    static constexpr std::uint64_t Width = 1024;
    std::unordered_map<std::uint64_t, std::unique_ptr<std::array<Location, Width>>> pages_;
public:
    Directory() = default;
    Directory(const Directory& other) { for (const auto& [key,p] : other.pages_) pages_.emplace(key, std::make_unique<std::array<Location, Width>>(*p)); }
    Directory(Directory&&) noexcept = default;
    Directory& operator=(Directory&&) noexcept = default;
    void reserve(std::uint64_t id) { auto& p = pages_[id / Width]; if (!p) p = std::make_unique<std::array<Location, Width>>(); }
    void set(std::uint64_t id, Location at) noexcept { (*pages_.find(id / Width)->second)[id % Width] = at; }
    Location find(std::uint64_t id) const noexcept {
        auto it = pages_.find(id / Width);
        return it == pages_.end() ? Location{} : (*it->second)[id % Width];
    }
    void insert(std::uint64_t id, Location location) {
        require(id != 0 && find(id).kind == EntityKind::None, "Duplicate or zero authored identity");
        auto& page = pages_[id / Width];
        if (!page) page = std::make_unique<std::array<Location, Width>>();
        (*page)[id % Width] = location;
    }
};
struct Range { std::uint32_t first = 0, count = 0; };
struct ProgramData {
    SctAuthoringProjectId id;
    std::uint64_t revision = 0, highWater = 0;
    Pages<Action> actions;
    Pages<Value> values;
    std::vector<ValueId> children;
    std::vector<char> characters;
    std::vector<Script> scripts;
    std::vector<SctAuthoringBaseline> baselines;
    std::vector<Section> sections;
    std::vector<Sequence> sequences;
    std::vector<std::vector<SctActionId>> pendingOrders;
    std::vector<Arm> arms;
    std::vector<std::int32_t> caseLabels;
    std::vector<ConditionRegion> conditions;
    std::vector<Loop> loops;
    std::vector<Switch> switches;
    std::vector<Dispatch> dispatches;
    std::vector<Transfer> transfers;
    std::vector<Text> texts;
    std::vector<SctActionId> footerOwners; // Derived, indexed by text slot; never serialized.
    Metadata metadata;
    Directory directory;
    std::map<Variable, Range> variableUses;
    std::vector<VariableUse> accesses;
    std::vector<VariableUse> computed;
    std::vector<TargetUse> targetUses;
    std::vector<Range> incomingRanges;
    std::vector<SctActionId> successors; // Indexed by action slot, not identity.
    Statistics stats;
    ControlFlowIssues initialFlowIssues; // Transient diagnostic seed only; groups persist as actions/arms/sequences.
    // Bulk postings stay pooled. Only edited buckets acquire replaceable ranges.
    std::map<Variable, std::shared_ptr<const std::vector<VariableUse>>> editedUses;
    std::map<SctActionId, std::shared_ptr<const std::vector<TargetUse>>> editedIncoming;
    std::shared_ptr<const std::vector<VariableUse>> editedComputed;
    std::uint64_t allocate() {
        require(highWater < UINT64_MAX, "Authored identity allocator exhausted"); return ++highWater;
    }
    std::uint64_t claim(std::uint64_t id, EntityKind kind, std::uint32_t index) {
        if (!id) id = allocate();
        highWater = std::max(highWater, id);
        directory.insert(id, {kind, index}); return id;
    }
    template<class T> const T* lookup(std::uint64_t id, EntityKind kind, const std::vector<T>& rows) const noexcept {
        auto at = directory.find(id); return at.kind == kind && at.index < rows.size() ? &rows[at.index] : nullptr;
    }
    Sequence* sequence(SctSequenceId id) {
        return const_cast<Sequence*>(lookup(id.value, EntityKind::Sequence, sequences));
    }
    const Action* action(SctActionId id) const noexcept {
        auto at = directory.find(id.value); return at.kind == EntityKind::Action ? actions.find(at.index) : nullptr;
    }
    std::span<const ValueId> child(ValueId id) const noexcept {
        auto* v = values.find(id.value); if (!v || !v->count) return {};
        return std::span<const ValueId>(children).subspan(v->first, v->count);
    }
    ValueId add(ValueKind kind, std::uint64_t data = 0, std::uint32_t detail = 0, std::span<const ValueId> items = {}) {
        for (auto id : items) require(values.find(id.value) != nullptr, "Invalid or forward value handle");
        auto first = count32(children.size()); count32(children.size() + items.size());
        children.insert(children.end(), items.begin(), items.end());
        return ValueId{values.append({data, first, count32(items.size()), detail, kind})};
    }
    ValueId string(ValueKind kind, std::string_view text) {
        require(kind == ValueKind::PlainText || kind == ValueKind::TextChunk, "Not a text value kind");
        auto first = count32(characters.size()); count32(characters.size() + text.size());
        characters.insert(characters.end(), text.begin(), text.end()); return add(kind, first, count32(text.size()));
    }
    SctSequenceId makeSequence(SctScriptId script, std::optional<SequenceOwner> owner = {}, SctSequenceId requested = {}, SequenceRole role = SequenceRole::SectionBody) {
        SctSequenceId id{claim(requested.value, EntityKind::Sequence, count32(sequences.size()))};
        sequences.push_back({id, script, owner, {}, role}); pendingOrders.emplace_back(); return id;
    }
    ConditionId makeCondition(SctScriptId script, SctActionId owner, ValueId result, std::optional<ArmId> arm = {}) {
        const auto* v = values.find(result.value);
        require(v && v->kind == ValueKind::Expression, "A condition needs a typed result expression");
        ConditionId id{claim(0, EntityKind::Condition, count32(conditions.size()))};
        auto preparation = makeSequence(script, id, {}, SequenceRole::ConditionPreparation);
        conditions.push_back({id, owner, arm, preparation, result}); return id;
    }
    const Section* sectionEntry(SctActionId target) const noexcept {
        const auto* a = action(target);
        const auto* s = a ? lookup(a->owner.value, EntityKind::Sequence, sequences) : nullptr;
        if (!s || s->owner) return nullptr;
        const auto slot = directory.find(s->id.value).index;
        const auto* order = s->order ? s->order.get() : slot < pendingOrders.size() ? &pendingOrders[slot] : nullptr;
        if (!order || order->empty() || order->front() != target) return nullptr;
        const auto* script = lookup(s->script.value, EntityKind::Script, scripts);
        if (script) for (auto id : script->sections) {
            const auto* section = lookup(id.value, EntityKind::Section, sections);
            if (section && section->body == s->id) return section;
        }
        return nullptr;
    }
    SctActionId parentAction(const Sequence& sequence) const {
        if (!sequence.owner) return {};
        return std::visit([&](auto id) -> SctActionId {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, ArmId>) {
                auto* a = lookup(id.value, EntityKind::Arm, arms); require(a != nullptr, "Missing arm owner"); return a->owner;
            } else if constexpr (std::is_same_v<T, ConditionId>) {
                auto* c = lookup(id.value, EntityKind::Condition, conditions); require(c != nullptr, "Missing condition owner"); return c->owner;
            } else return id;
        }, *sequence.owner);
    }
    SctScriptId beginScript(SctAuthoringBaseline baseline, SctScriptContext context) {
        require(baseline.importedDocument.valid(), "Imported document identity is required");
        require(!baseline.source.locator.identityKey().empty(), "Imported source locator is required");
        for (const auto& previous : baselines) require(previous.importedDocument != baseline.importedDocument, "Imported document identity was already used");
        baseline.id = SctBaselineId{claim(baseline.id.value, EntityKind::Baseline, count32(baselines.size()))};
        require(!context.baseline.valid() || context.baseline == baseline.id, "Script baseline does not match imported baseline");
        require(context.contentUses.empty(), "Legacy physical content uses require explicit migration");
        context.baseline = baseline.id;
        context.id = SctScriptId{claim(context.id.value, EntityKind::Script, count32(scripts.size()))};
        const auto id = context.id;
        baselines.push_back(std::move(baseline)); scripts.push_back({std::move(context), {}, {}}); return id;
    }
    SctActionId appendAction(SctSequenceId owner, ValueId value, std::uint64_t source = 0, SctActionId requested = {}) {
        initialFlowIssues.clear();
        require(sequence(owner) != nullptr, "Action owner is missing");
        require(values.find(value.value) && values.find(value.value)->kind == ValueKind::Instruction, "Instruction payload is missing");
        SctActionId id{claim(requested.value, EntityKind::Action, actions.size())};
        actions.append({id, owner, ActionKind::Instruction, value, 0, 0, source});
        pendingOrders[directory.find(owner.value).index].push_back(id); return id;
    }
};
// Transform only string operands; immutable text payloads may share pooled bytes,
// but each parameter has an independent editable text identity.
template<class Map> ValueId mapFooterOperands(ProgramData &d, ValueId root, Map map) {
    const auto *instruction = d.values.find(root.value);
    if (!instruction || instruction->kind != ValueKind::Instruction) return root;
    auto transform = [&](ValueId id, std::optional<std::uint32_t> group) {
        auto parameter = *d.values.find(id.value);
        if (parameter.kind != ValueKind::Parameter) return id;
        auto children = d.child(id);
        if (children.size() != 1) return id;
        auto ref = *d.values.find(children.front().value);
        if (ref.kind != ValueKind::SupplementaryTextReference) return id;
        auto target = map(SctTextResourceId{ref.data}, spice::sct::SctParameterAddress{static_cast<std::uint32_t>(parameter.data), group});
        if (target.value == ref.data) return id;
        auto value = d.add(ref.kind, target.value);
        return d.add(parameter.kind, parameter.data, parameter.detail, std::span(&value, 1));
    };
    auto value = *instruction;
    auto original = d.child(root);
    std::vector<ValueId> children(original.begin(), original.end());
    bool changed = false;
    std::uint32_t group = 0;
    for (auto &id : children) {
        auto before = id;
        auto v = *d.values.find(id.value);
        if (v.kind == ValueKind::RepeatedGroup) {
            auto source = d.child(id);
            std::vector<ValueId> items(source.begin(), source.end());
            bool edited = false;
            for (auto &item : items) { auto next = transform(item, group); edited |= next != item; item = next; }
            if (edited) id = d.add(v.kind, v.data, v.detail, items);
            ++group;
        } else id = transform(id, {});
        changed |= before != id;
    }
    return changed ? d.add(value.kind, value.data, value.detail, children) : root;
}
// Native normalization is also used by the public expression input operation.
ValueId normalizeExpression(ProgramData&, const spice::sct::SctCanonicalExpression&);
struct DirectFacts { std::vector<VariableUse> accesses, computed; std::vector<TargetUse> targets; };
void valueShape(const ProgramData&, ValueId);
DirectFacts actionFacts(ProgramData&, std::span<const SctActionId>, std::vector<Diagnostic>&, bool recordTextOwners = false);
void finalizePostings(ProgramData&);
void buildIndexes(ProgramData&, std::vector<Diagnostic>&, bool checkValueShapes = true);
void validateDispatch(const ProgramData &, const Dispatch &);
void finishMetadata(ProgramData &, std::vector<Diagnostic> &, bool allocateIds = true,
                    bool checkDispatches = true);
} // namespace salsa::core::program
