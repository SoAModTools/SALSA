#pragma once
#include "ProgramInternal.h"
namespace salsa::core::program {
namespace sct = spice::sct;
template <class Source> class TypedValues {
  protected:
    const Source &p;

  public:
    explicit TypedValues(const Source &program) : p(program) {}
    const Value &get(ValueId id, ValueKind kind) const {
        const auto *v = p.find(id);
        require(v && v->kind == kind, "Unsupported or incomplete executable value; repair it before export");
        return *v;
    }
    std::vector<std::uint32_t> words(ValueId id) const {
        std::vector<std::uint32_t> result;
        for (auto child : p.children(id))
            result.push_back(static_cast<std::uint32_t>(get(child, ValueKind::Word).data));
        return result;
    }
    sct::SctCanonicalExpression expression(ValueId id) const {
        const auto &v = get(id, ValueKind::Expression);
        sct::SctTypedScptProgram result;
        for (auto child : p.children(id)) {
            const auto *op = p.find(child);
            require(op != nullptr, "Missing expression operation");
            switch (op->kind) {
            case ValueKind::ValueOperation:
                result.operations.emplace_back(
                    sct::SctScptValueOperation{static_cast<sct::SctScptValueKind>(op->detail),
                                               static_cast<std::uint32_t>(op->data), words(child)});
                break;
            case ValueKind::BinaryOperation:
                result.operations.emplace_back(sct::SctScptBinaryOperation{
                    static_cast<sct::SctScptBinaryOperationKind>(op->detail), static_cast<std::uint32_t>(op->data)});
                break;
            case ValueKind::StackOperation:
                result.operations.emplace_back(
                    sct::SctScptStackOverwritePreviousWithTopOperation{static_cast<std::uint32_t>(op->data)});
                break;
            case ValueKind::InertOperation:
                result.operations.emplace_back(sct::SctScptInertOperation{static_cast<std::uint32_t>(op->data)});
                break;
            default:
                require(false, "Unsupported executable expression operation");
            }
        }
        return {std::move(result), static_cast<sct::SctExpressionTermination>(v.data)};
    }
    sct::SctTextValue text(ValueId id) const {
        const auto *v = p.find(id);
        require(v != nullptr, "Missing text value");
        if (v->kind == ValueKind::PlainText)
            return sct::SctPlainText{std::string(p.text(id))};
        if (v->kind == ValueKind::EmptyIndexedText)
            return sct::SctEmptyIndexedText{};
        get(id, ValueKind::Message);
        sct::SctMessage result;
        auto children = p.children(id);
        if (v->data) {
            require(!children.empty(), "Missing message header");
            result.headerUtf8 = p.text(children.front());
            children = children.subspan(1);
        }
        for (auto child : children) {
            const auto *element = p.find(child);
            require(element != nullptr, "Missing message element");
            if (element->kind == ValueKind::TextChunk)
                result.body.elements.emplace_back(sct::SctTextChunk{std::string(p.text(child))});
            else {
                get(child, ValueKind::TextCommand);
                sct::SctInlineCommand command;
                command.code = static_cast<sct::SctMessageCommandCode>(element->data);
                auto args = words(child);
                if (element->detail == 1)
                    command.argument =
                        sct::SctDecimalCommandArgument{args.empty() ? std::nullopt : std::optional(args.front())};
                else if (element->detail == 2) {
                    sct::SctByteListCommandArgument a;
                    for (auto w : args) {
                        require(w <= 255, "Message byte exceeds its range");
                        a.values.push_back(static_cast<std::uint8_t>(w));
                    }
                    command.argument = std::move(a);
                } else
                    require(element->detail == 0, "Unsupported message command argument");
                result.body.elements.emplace_back(std::move(command));
            }
        }
        return result;
    }
};
} // namespace salsa::core::program
