#include "ProgramInternal.h"
#include <set>

namespace salsa::core::program {
Program::Program(std::unique_ptr<ProgramData> data) : data_(std::move(data)) {}
Program::~Program() = default;
const SctAuthoringProjectId& Program::id() const noexcept { return data_->id; }
std::uint64_t Program::revision() const noexcept { return data_->revision; }
std::uint64_t Program::highWater() const noexcept { return data_->highWater; }
std::span<const Script> Program::scripts() const noexcept { return data_->scripts; }
std::span<const SctAuthoringBaseline> Program::baselines() const noexcept { return data_->baselines; }
std::span<const Section> Program::sections() const noexcept { return data_->sections; }
std::span<const Text> Program::texts() const noexcept { return data_->texts; }
const Metadata& Program::metadata() const noexcept { return data_->metadata; }
const Action* Program::find(SctActionId id) const noexcept { return data_->action(id); }
const Sequence* Program::find(SctSequenceId id) const noexcept { return data_->lookup(id.value, EntityKind::Sequence, data_->sequences); }
const Script* Program::find(SctScriptId id) const noexcept { return data_->lookup(id.value, EntityKind::Script, data_->scripts); }
const Section* Program::find(SectionId id) const noexcept { return data_->lookup(id.value, EntityKind::Section, data_->sections); }
const Text* Program::find(SctTextResourceId id) const noexcept { return data_->lookup(id.value, EntityKind::Text, data_->texts); }
const Value* Program::find(ValueId id) const noexcept { return data_->values.find(id.value); }
std::span<const ValueId> Program::children(ValueId id) const noexcept { return data_->child(id); }
std::string_view Program::text(ValueId id) const noexcept {
    auto* v = find(id);
    if (!v || (v->kind != ValueKind::PlainText && v->kind != ValueKind::TextChunk) || !v->detail) return {};
    return {data_->characters.data() + v->data, v->detail};
}
std::span<const Arm> Program::arms(SctActionId id) const noexcept {
    auto* a = find(id); if (!a || !a->armCount) return {};
    return std::span<const Arm>(data_->arms).subspan(a->firstArm, a->armCount);
}
std::span<const VariableUse> Program::uses(Variable variable) const noexcept {
    auto it = data_->variableUses.find(variable);
    return it == data_->variableUses.end() ? std::span<const VariableUse>{} : std::span<const VariableUse>(data_->accesses).subspan(it->second.first, it->second.count);
}
std::span<const VariableUse> Program::computedWrites() const noexcept { return data_->computed; }
std::span<const TargetUse> Program::incoming(SctActionId id) const noexcept {
    auto at = data_->directory.find(id.value);
    if (at.kind != EntityKind::Action || at.index >= data_->incomingRanges.size()) return {};
    const auto range = data_->incomingRanges[at.index];
    return std::span<const TargetUse>(data_->targetUses).subspan(range.first, range.count);
}
std::optional<SctActionId> Program::nextInSequence(SctActionId id) const noexcept {
    auto at = data_->directory.find(id.value);
    if (at.kind != EntityKind::Action || at.index >= data_->successors.size() || !data_->successors[at.index].valid()) return {};
    return data_->successors[at.index];
}
Statistics Program::statistics() const noexcept {
    auto s = data_->stats; s.actions = data_->actions.size(); s.values = data_->values.size();
    s.actionPages = data_->actions.pageCount(); s.valuePages = data_->values.pageCount(); return s;
}
ProgramBuilder::ProgramBuilder(SctAuthoringProjectId id, std::uint64_t revision, std::uint64_t highWater)
    : data_(std::make_unique<ProgramData>()) {
    data_->id = std::move(id); data_->revision = revision; data_->highWater = highWater;
    if (!data_->id.valid()) diagnostics_.push_back(failure("Invalid program project identity"));
}
ProgramBuilder::~ProgramBuilder() = default;
Result<SctScriptId> ProgramBuilder::importedScript(SctAuthoringBaseline baseline, SctScriptContext context) {
    try { require(data_ != nullptr, "Builder already finished"); return Result<SctScriptId>::success(data_->beginScript(std::move(baseline), std::move(context))); }
    catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<SctScriptId>::failure(diagnostics_.back()); }
}
Result<SectionId> ProgramBuilder::section(Section row, SctSequenceId requestedBody) {
    try {
        require(data_ != nullptr, "Builder already finished"); auto& d = *data_;
        auto at = d.directory.find(row.script.value); require(at.kind == EntityKind::Script, "Missing section script owner");
        require(!row.body.valid(), "Section body is assigned by the builder");
        row.id = SectionId{d.claim(row.id.value, EntityKind::Section, count32(d.sections.size()))};
        if (!row.content.valid()) row.body = d.makeSequence(row.script, {}, requestedBody);
        else {
            auto* v = d.values.find(row.content.value);
            require(!requestedBody.valid() && v && (v->kind == ValueKind::IndexedSection || v->kind == ValueKind::StringGroup || v->kind == ValueKind::Unavailable), "Invalid non-code section content");
        }
        auto id = row.id; d.scripts[at.index].sections.push_back(id); d.sections.push_back(std::move(row));
        return Result<SectionId>::success(id);
    } catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<SectionId>::failure(diagnostics_.back()); }
}
Result<SctTextResourceId> ProgramBuilder::textEntity(Text row) {
    try {
        require(data_ != nullptr, "Builder already finished"); auto& d = *data_;
        auto at = d.directory.find(row.script.value); require(at.kind == EntityKind::Script, "Missing text script owner");
        require(d.values.find(row.value.value) != nullptr, "Missing text payload");
        row.id = SctTextResourceId{d.claim(row.id.value, EntityKind::Text, count32(d.texts.size()))};
        auto id = row.id; if (row.supplementary) d.scripts[at.index].supplementaryText.push_back(id);
        d.texts.push_back(std::move(row)); return Result<SctTextResourceId>::success(id);
    } catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<SctTextResourceId>::failure(diagnostics_.back()); }
}
std::vector<SectionId> ProgramBuilder::scriptSections(SctScriptId id) const {
    if (data_) if (auto* s = data_->lookup(id.value, EntityKind::Script, data_->scripts)) return s->sections;
    return {};
}
SctSequenceId ProgramBuilder::sectionBody(SectionId id) const noexcept {
    if (data_) if (auto* s = data_->lookup(id.value, EntityKind::Section, data_->sections)) return s->body;
    return {};
}
Result<ValueId> ProgramBuilder::value(ValueKind kind, std::uint64_t data, std::uint32_t detail, std::span<const ValueId> children) {
    try {
        require(data_ != nullptr, "Builder already finished");
        require(kind != ValueKind::PlainText && kind != ValueKind::TextChunk, "Use text input for character ranges");
        return Result<ValueId>::success(data_->add(kind, data, detail, children));
    } catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<ValueId>::failure(diagnostics_.back()); }
}
Result<ValueId> ProgramBuilder::text(ValueKind kind, std::string_view text) {
    try { require(data_ != nullptr, "Builder already finished"); return Result<ValueId>::success(data_->string(kind, text)); }
    catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<ValueId>::failure(diagnostics_.back()); }
}
Result<ValueId> ProgramBuilder::expression(const spice::sct::SctCanonicalExpression& expression) {
    try { require(data_ != nullptr, "Builder already finished"); return Result<ValueId>::success(normalizeExpression(*data_, expression)); }
    catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<ValueId>::failure(diagnostics_.back()); }
}
Result<SctActionId> ProgramBuilder::instruction(SctSequenceId owner, ValueId value, SctActionId requested) {
    try { require(data_ != nullptr, "Builder already finished"); return Result<SctActionId>::success(data_->appendAction(owner, value, 0, requested)); }
    catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<SctActionId>::failure(diagnostics_.back()); }
}
Result<SctActionId> ProgramBuilder::conditional(SctSequenceId owner, std::span<const ValueId> conditions) {
    try {
        require(data_ != nullptr, "Builder already finished"); auto& d = *data_;
        auto* sequence = d.sequence(owner); require(sequence != nullptr, "Conditional owner is missing");
        const auto script = sequence->script;
        require(!conditions.empty() && conditions.front().valid(), "A conditional needs its first condition");
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            const auto* v = d.values.find(conditions[i].value);
            require((v && v->kind == ValueKind::Expression) || (!conditions[i].valid() && i + 1 == conditions.size()), "Invalid conditional arm");
        }
        SctActionId id{d.claim(0, EntityKind::Action, d.actions.size())};
        const auto first = count32(d.arms.size());
        for (auto condition : conditions) {
            ArmId arm{d.claim(0, EntityKind::Arm, count32(d.arms.size()))};
            const auto body = d.makeSequence(script, arm, {}, SequenceRole::ArmBody);
            const auto check = condition.valid() ? d.makeCondition(script, id, condition, arm) : ConditionId{};
            d.arms.push_back({arm, id, check, body});
        }
        d.actions.append({id, owner, ActionKind::Conditional, {}, first, count32(conditions.size()), 0});
        d.pendingOrders[d.directory.find(owner.value).index].push_back(id);
        return Result<SctActionId>::success(id);
    } catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<SctActionId>::failure(diagnostics_.back()); }
}
std::vector<SctSequenceId> ProgramBuilder::branchBodies(SctActionId id) const {
    std::vector<SctSequenceId> result;
    if (data_) if (auto* a = data_->action(id)) for (std::uint32_t i = 0; i < a->armCount; ++i) result.push_back(data_->arms[a->firstArm + i].body);
    return result;
}
Metadata& ProgramBuilder::metadata() { require(data_ != nullptr, "Builder already finished"); return data_->metadata; }
Result<std::shared_ptr<const Program>> ProgramBuilder::finish() {
    if (hasErrors(diagnostics_)) return Result<std::shared_ptr<const Program>>::failure(diagnostics_);
    try {
        require(data_ != nullptr, "Builder already finished");
        finishMetadata(*data_, diagnostics_);
        buildIndexes(*data_, diagnostics_);
        data_->successors.resize(data_->actions.size());
        for (std::size_t i = 0; i < data_->sequences.size(); ++i) {
            auto& order = data_->pendingOrders[i];
            for (std::size_t j = 1; j < order.size(); ++j) data_->successors[data_->directory.find(order[j - 1].value).index] = order[j];
            data_->sequences[i].order = std::make_shared<const std::vector<SctActionId>>(std::move(order));
        }
        data_->pendingOrders.clear(); data_->pendingOrders.shrink_to_fit();
        return Result<std::shared_ptr<const Program>>::success(std::shared_ptr<const Program>(new Program(std::move(data_))), diagnostics_);
    } catch (const std::exception& e) { diagnostics_.push_back(failure(e.what())); return Result<std::shared_ptr<const Program>>::failure(diagnostics_); }
}
} // namespace salsa::core::program
