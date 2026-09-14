#pragma once
#include "ProgramTypedValues.h"
#include <functional>
namespace salsa::core::program::dispatch_detail {
using Code = std::vector<spice::sct::SctScptOperation>;
struct Predicate {
    enum Kind { Atom, And, Or } kind = Atom;
    Code code;
    std::shared_ptr<const Predicate> left, right;
};
inline std::shared_ptr<const Predicate> parse(const spice::sct::SctCanonicalExpression &expression) {
    const auto *program = std::get_if<spice::sct::SctTypedScptProgram>(&expression.body);
    require(program && program->operations.size() <= 256,
            "Dispatch requires a bounded typed comparison expression");
    std::vector<std::shared_ptr<const Predicate>> stack;
    for (const auto &op : program->operations) {
        auto node = std::make_shared<Predicate>();
        if (auto *value = std::get_if<spice::sct::SctScptValueOperation>(&op)) {
            require(value->kind != spice::sct::SctScptValueKind::SecondaryValue,
                    "Dispatch cannot reschedule context-dependent SCPT secondary values");
            node->code = {op};
        } else if (auto *binary = std::get_if<spice::sct::SctScptBinaryOperation>(&op)) {
            require(stack.size() >= 2, "Invalid dispatch expression stack");
            node->right = stack.back();
            stack.pop_back();
            node->left = stack.back();
            stack.pop_back();
            node->code = node->left->code;
            node->code.insert(node->code.end(), node->right->code.begin(), node->right->code.end());
            node->code.push_back(op);
            if (binary->kind == spice::sct::SctScptBinaryOperationKind::Comparison &&
                (binary->encodingWord == 8 || binary->encodingWord == 9))
                node->kind = binary->encodingWord == 8 ? Predicate::And : Predicate::Or;
        } else
            require(false, "Dispatch recognition leaves stack-manipulating or inert SCPT programs explicit");
        stack.push_back(std::move(node));
    }
    require(stack.size() == 1, "Dispatch expression must have one result");
    return stack.back();
}
inline spice::sct::SctCanonicalExpression expression(const Code &code) {
    return {spice::sct::SctTypedScptProgram{code}, spice::sct::SctExpressionTermination::StopCode};
}
inline void atoms(const Predicate &p, const std::function<void(const Code &)> &visit) {
    if (p.kind == Predicate::Atom)
        visit(p.code);
    else {
        atoms(*p.left, visit);
        atoms(*p.right, visit);
    }
}
inline Code conjunction(const Code &a, const Code &b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    auto out = a;
    out.insert(out.end(), b.begin(), b.end());
    out.emplace_back(spice::sct::SctScptBinaryOperation{spice::sct::SctScptBinaryOperationKind::Comparison, 8});
    return out;
}
struct DataView {
    const ProgramData &d;
    const Value *find(ValueId id) const { return d.values.find(id.value); }
    std::span<const ValueId> children(ValueId id) const { return d.child(id); }
};
inline spice::sct::SctCanonicalExpression expression(const ProgramData &d, ValueId id) {
    return TypedValues<DataView>(DataView{d}).expression(id);
}
} // namespace salsa::core::program::dispatch_detail
