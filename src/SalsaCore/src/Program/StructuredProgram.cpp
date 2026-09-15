#include "ProgramInternal.h"
#include <set>

namespace salsa::core::program {
const Arm *Program::find(ArmId id) const noexcept { return data_->lookup(id.value, EntityKind::Arm, data_->arms); }
const ConditionRegion* Program::find(ConditionId id) const noexcept {
    return data_->lookup(id.value, EntityKind::Condition, data_->conditions);
}
std::span<const std::int32_t> Program::caseLabels(ArmId id) const noexcept {
    const auto* arm = data_->lookup(id.value, EntityKind::Arm, data_->arms);
    if (!arm || !arm->labelCount) return {};
    return std::span<const std::int32_t>(data_->caseLabels).subspan(arm->firstLabel, arm->labelCount);
}
const Loop* Program::loop(SctActionId id) const noexcept {
    const auto* a = find(id);
    return a && a->kind == ActionKind::Loop && a->detail < data_->loops.size() ? &data_->loops[a->detail] : nullptr;
}
const Switch* Program::switchRegion(SctActionId id) const noexcept {
    const auto* a = find(id);
    return a && a->kind == ActionKind::Switch && a->detail < data_->switches.size() ? &data_->switches[a->detail] : nullptr;
}
const Dispatch *Program::dispatch(SctActionId id) const noexcept {
    const auto *a = find(id);
    return a && a->kind == ActionKind::Dispatch && a->detail < data_->dispatches.size()
               ? &data_->dispatches[a->detail]
               : nullptr;
}
const Transfer* Program::transfer(SctActionId id) const noexcept {
    const auto* a = find(id);
    return a && a->kind == ActionKind::Transfer && a->detail < data_->transfers.size() ? &data_->transfers[a->detail] : nullptr;
}
std::optional<TransferDestination> Program::transferDestination(SctActionId id) const noexcept {
    const auto* t = transfer(id);
    if (!t) return {};
    if (t->kind == TransferKind::Break) return TransferDestination{t->target, {}};
    const auto* target = loop(t->target);
    if (!target) return {};
    const auto* condition = find(target->condition);
    if (!condition) return {};
    return TransferDestination{t->target, target->kind == LoopKind::For ? target->update : condition->preparation};
}
std::vector<ConditionId> ProgramBuilder::branchConditions(SctActionId id) const {
    std::vector<ConditionId> result;
    if (data_) if (const auto* a = data_->action(id)) {
        for (std::uint32_t i = 0; i < a->armCount; ++i) result.push_back(data_->arms[a->firstArm + i].condition);
    }
    return result;
}
SctSequenceId ProgramBuilder::conditionPreparation(ConditionId id) const noexcept {
    if (data_) if (const auto* c = data_->lookup(id.value, EntityKind::Condition, data_->conditions)) return c->preparation;
    return {};
}
Result<SctActionId> ProgramBuilder::loop(SctSequenceId owner, LoopKind kind, ValueId condition) {
    try {
        require(data_ != nullptr, "Builder already finished");
        auto& d = *data_;
        const auto* sequence = d.sequence(owner);
        require(sequence != nullptr, "Missing loop owner");
        const auto script = sequence->script;
        require(kind == LoopKind::While || kind == LoopKind::DoWhile || kind == LoopKind::For, "Unknown loop kind");
        SctActionId id{d.claim(0, EntityKind::Action, d.actions.size())};
        Loop loop; loop.owner = id; loop.kind = kind;
        loop.condition = d.makeCondition(script, id, condition);
        if (kind == LoopKind::For) loop.initialize = d.makeSequence(script, id, {}, SequenceRole::LoopInitialize);
        loop.body = d.makeSequence(script, id, {}, SequenceRole::LoopBody);
        if (kind == LoopKind::For) loop.update = d.makeSequence(script, id, {}, SequenceRole::LoopUpdate);
        const auto index = count32(d.loops.size());
        d.loops.push_back(loop);
        d.actions.append({id, owner, ActionKind::Loop, {}, 0, 0, 0, index});
        d.pendingOrders[d.directory.find(owner.value).index].push_back(id);
        return Result<SctActionId>::success(id);
    } catch (const std::exception& e) {
        diagnostics_.push_back(failure(e.what())); return Result<SctActionId>::failure(diagnostics_.back());
    }
}
std::optional<Loop> ProgramBuilder::loop(SctActionId id) const {
    if (data_) if (const auto* a = data_->action(id); a && a->kind == ActionKind::Loop) return data_->loops.at(a->detail);
    return {};
}
Result<SctActionId> ProgramBuilder::switchRegion(SctSequenceId owner, ValueId selector, std::span<const Case> cases) {
    try {
        require(data_ != nullptr, "Builder already finished");
        auto& d = *data_;
        const auto* sequence = d.sequence(owner);
        require(sequence != nullptr, "Missing switch owner");
        const auto script = sequence->script;
        std::set<std::int32_t> labels;
        bool hasDefault = false;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            require(!cases[i].labels.empty() || cases[i].includesDefault, "Switch arm needs labels or Default");
            for (auto label : cases[i].labels) require(labels.insert(label).second, "Duplicate switch case label");
            require(!cases[i].includesDefault || !hasDefault, "Duplicate switch Default");
            hasDefault = hasDefault || cases[i].includesDefault;
            require(cases[i].exit == CaseExit::ExitSwitch || cases[i].exit == CaseExit::Fallthrough, "Unknown switch exit mode");
            require(cases[i].exit != CaseExit::Fallthrough || i + 1 < cases.size(), "Last switch arm cannot fall through");
        }
        SctActionId id{d.claim(0, EntityKind::Action, d.actions.size())};
        const auto check = d.makeCondition(script, id, selector);
        const auto first = count32(d.arms.size());
        for (const auto& c : cases) {
            ArmId arm{d.claim(0, EntityKind::Arm, count32(d.arms.size()))};
            const auto body = d.makeSequence(script, arm, {}, SequenceRole::ArmBody);
            const auto firstLabel = count32(d.caseLabels.size());
            count32(d.caseLabels.size() + c.labels.size());
            d.caseLabels.insert(d.caseLabels.end(), c.labels.begin(), c.labels.end());
            d.arms.push_back({arm, id, {}, body, firstLabel, count32(c.labels.size()), c.includesDefault, c.exit});
        }
        const auto index = count32(d.switches.size());
        d.switches.push_back({id, check});
        d.actions.append({id, owner, ActionKind::Switch, {}, first, count32(cases.size()), 0, index});
        d.pendingOrders[d.directory.find(owner.value).index].push_back(id);
        return Result<SctActionId>::success(id);
    } catch (const std::exception& e) {
        diagnostics_.push_back(failure(e.what())); return Result<SctActionId>::failure(diagnostics_.back());
    }
}
std::optional<Switch> ProgramBuilder::switchRegion(SctActionId id) const {
    if (data_) if (const auto* a = data_->action(id); a && a->kind == ActionKind::Switch) return data_->switches.at(a->detail);
    return {};
}
Result<SctActionId> ProgramBuilder::transfer(SctSequenceId owner, TransferKind kind, SctActionId target) {
    try {
        require(data_ != nullptr, "Builder already finished");
        auto& d = *data_;
        require(kind == TransferKind::Break || kind == TransferKind::Continue, "Unknown structured transfer kind");
        const auto* destination = d.action(target);
        require(destination && (destination->kind == ActionKind::Loop || (kind == TransferKind::Break && (destination->kind == ActionKind::Switch || destination->kind == ActionKind::Conditional))),
            "Break needs a loop/switch; Continue needs a loop");
        const auto* current = d.sequence(owner);
        require(current != nullptr, "Missing transfer owner");
        bool enclosed = false;
        while (current->owner) {
            auto parent = d.parentAction(*current);
            if (parent == target) {
                // Continue is a body operation, including nested conditions and
                // switches. It cannot restart its own initializer/check/update.
                require(kind != TransferKind::Continue || current->role == SequenceRole::LoopBody,
                    "Continue must occur inside the target loop body");
                enclosed = true; break;
            }
            const auto* action = d.action(parent);
            require(action != nullptr, "Missing enclosing construct");
            current = d.sequence(action->owner);
            require(current != nullptr, "Missing enclosing sequence");
        }
        require(enclosed, "Transfer target does not enclose its source");
        SctActionId id{d.claim(0, EntityKind::Action, d.actions.size())};
        const auto index = count32(d.transfers.size());
        d.transfers.push_back({id, target, kind});
        d.actions.append({id, owner, ActionKind::Transfer, {}, 0, 0, 0, index});
        d.pendingOrders[d.directory.find(owner.value).index].push_back(id);
        return Result<SctActionId>::success(id);
    } catch (const std::exception& e) {
        diagnostics_.push_back(failure(e.what())); return Result<SctActionId>::failure(diagnostics_.back());
    }
}
} // namespace salsa::core::program
