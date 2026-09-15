#pragma once
#include "ProgramInternal.h"
namespace salsa::core::program {
struct ExplicitFlow {
    SctActionId header, continuation, exit;
    SctSequenceId sequence;
    std::size_t start = 0, alternate = 0, closing = 0, end = 0;
    bool loop = false;
    ValueId condition;
};
template <class Incoming>
ExplicitFlow recognizeExplicit(ProgramData &d, SctActionId header, SctActionId continuation, Incoming incoming) {
    const auto *found = d.action(header);
    require(found, "Missing flow header");
    auto original = *found;
    require(original.kind == ActionKind::Instruction, "Flow header is already structured");
    auto node = *d.values.find(original.value.value);
    require(node.data == 0 && node.detail == 0, "Promotion requires an unscheduled, normal-refresh If");
    const auto &source = *d.sequence(original.owner)->order;
    auto start = std::ranges::find(source, header), end = std::ranges::find(source, continuation);
    require(start != source.end() && end != source.end() && start < end,
            "Promotion needs a forward continuation in the same sequence");
    auto parameter = [&](const Action &a, std::uint32_t index) -> ValueId {
        for (auto child : d.child(a.value)) {
            auto *v = d.values.find(child.value);
            if (v->kind == ValueKind::Parameter && v->data == index)
                return d.child(child).front();
        }
        throw std::runtime_error("Flow operand is missing");
    };
    auto conditionValue = parameter(original, 0), falseValue = parameter(original, 1);
    require(d.values.find(conditionValue.value)->kind == ValueKind::Expression &&
                d.values.find(falseValue.value)->kind == ValueKind::ActionReference,
            "Promotion needs a typed check and resolved false edge");
    SctActionId falseId{d.values.find(falseValue.value)->data};
    auto alternate = std::ranges::find(source, falseId);
    require(alternate > start && alternate <= end, "False edge is outside promotion range");
    auto closing = alternate - 1;
    require(closing > start, "Promotion needs an explicit body exit");
    auto exit = *d.action(*closing);
    require(exit.kind == ActionKind::Instruction, "Body exit is not an explicit jump");
    auto exitNode = *d.values.find(exit.value.value);
    require(exitNode.data == 10 && exitNode.detail == 0, "Promotion needs an unscheduled plain jump exit");
    auto jump = parameter(exit, 0);
    require(d.values.find(jump.value)->kind == ValueKind::ActionReference, "Unresolved body exit");
    SctActionId exitTarget{d.values.find(jump.value)->data};
    const bool loop = exitTarget == header && alternate == end;
    require(loop || exitTarget == continuation, "Irregular body exit remains explicit");
    // Only the header and consumed exit may cross the new body boundary. Calls
    // or jumps into its interior would make structural promotion misleading.
    for (auto i = start + 1; i < end; ++i) {
        auto *a = d.action(*i);
        require(a->kind == ActionKind::Instruction, "Nested explicit promotion is not recognized here");
        auto v = *d.values.find(a->value.value);
        require(v.detail == 0, "Scheduled/refresh-modified regions remain explicit");
        for (auto ref : incoming(d, *i))
            require(ref.site.action == header || ref.site.action == exit.id,
                    "A promoted body has other incoming edges");
        if (*i != exit.id)
            require(v.data != 0 && v.data != 3 && v.data != 10, "Irregular internal control flow remains explicit");
    }
    for (auto ref : incoming(d, exit.id))
        require(false, "Jump exit has incoming callers");
    return {header,
            continuation,
            exit.id,
            original.owner,
            static_cast<std::size_t>(start - source.begin()),
            static_cast<std::size_t>(alternate - source.begin()),
            static_cast<std::size_t>(closing - source.begin()),
            static_cast<std::size_t>(end - source.begin()),
            loop,
            conditionValue};
}
} // namespace salsa::core::program
