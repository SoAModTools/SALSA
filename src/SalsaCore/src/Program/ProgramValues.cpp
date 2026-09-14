#include "SalsaCore/Program/ProgramValues.h"
#include "ProgramTypedValues.h"
#include <map>
namespace salsa::core::program {
namespace {
struct View {
    const ValueBuffer &b;
    const Value *find(ValueId id) const { return id.value < b.values.size() ? &b.values[id.value] : nullptr; }
    std::span<const ValueId> children(ValueId id) const {
        const auto *v = find(id);
        require(v && v->first <= b.children.size() && v->count <= b.children.size() - v->first,
                "Invalid value child range");
        return std::span(b.children).subspan(v->first, v->count);
    }
    std::string_view text(ValueId id) const {
        const auto *v = find(id);
        require(v && v->data <= b.characters.size() && v->detail <= b.characters.size() - v->data,
                "Invalid text range");
        return std::string_view(b.characters).substr(v->data, v->detail);
    }
};
template <class F> auto guarded(F f) -> Result<decltype(f())> {
    try {
        return Result<decltype(f())>::success(f());
    } catch (const std::exception &e) {
        return Result<decltype(f())>::failure(failure(e.what()));
    }
}
ValueId copy(ValueBuffer &out, const ValueBuffer &from, ValueId root, ValueId target = {},
             const ValueBuffer *replacement = nullptr) {
    View view{from};
    std::map<std::uint32_t, ValueId> mapped;
    std::function<ValueId(ValueId, unsigned)> visit = [&](ValueId id, unsigned depth) -> ValueId {
        require(depth < 512, "Selected value nesting is too deep");
        if (replacement && id == target)
            return copy(out, *replacement, replacement->root);
        if (auto i = mapped.find(id.value); i != mapped.end())
            return i->second;
        const auto *v = view.find(id);
        require(v, "Missing selected value");
        ValueId result;
        if (v->kind == ValueKind::PlainText || v->kind == ValueKind::TextChunk)
            result = out.text(v->kind, view.text(id));
        else {
            std::vector<ValueId> children;
            for (auto c : view.children(id)) {
                require(c.value < id.value, "Invalid forward value");
                children.push_back(visit(c, depth + 1));
            }
            result = out.add(v->kind, v->data, v->detail, children);
        }
        mapped.emplace(id.value, result);
        return result;
    };
    return visit(root, 0);
}
ValueId encodeText(ValueBuffer &d, const sct::SctTextValue &text) {
    return std::visit(
        [&](const auto &value) -> ValueId {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, sct::SctPlainText>)
                return d.text(ValueKind::PlainText, value.utf8);
            else if constexpr (std::is_same_v<T, sct::SctEmptyIndexedText>)
                return d.add(ValueKind::EmptyIndexedText);
            else if constexpr (std::is_same_v<T, sct::SctOpaqueText>) {
                return d.add(ValueKind::Unavailable, value.bytes.size(), 3);
            } else {
                std::vector<ValueId> elements;
                if (value.headerUtf8)
                    elements.push_back(d.text(ValueKind::PlainText, *value.headerUtf8));
                for (const auto &element : value.body.elements)
                    elements.push_back(std::visit(
                        [&](const auto &v) -> ValueId {
                            using E = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<E, sct::SctTextChunk>)
                                return d.text(ValueKind::TextChunk, v.utf8);
                            else {
                                std::vector<ValueId> arguments;
                                std::visit(
                                    [&](const auto &argument) {
                                        using A = std::decay_t<decltype(argument)>;
                                        if constexpr (std::is_same_v<A, sct::SctDecimalCommandArgument>) {
                                            if (argument.value)
                                                arguments.push_back(d.add(ValueKind::Word, *argument.value));
                                        } else if constexpr (std::is_same_v<A, sct::SctByteListCommandArgument>) {
                                            for (auto byte : argument.values)
                                                arguments.push_back(d.add(ValueKind::Word, byte));
                                        }
                                    },
                                    v.argument);
                                return d.add(ValueKind::TextCommand, static_cast<std::uint64_t>(v.code),
                                             static_cast<std::uint32_t>(v.argument.index()), arguments);
                            }
                        },
                        element));
                return d.add(ValueKind::Message, value.headerUtf8.has_value(), 0, elements);
            }
        },
        text);
}
ValueId parameter(ValueBuffer &out, const sct::SctDocumentParameterValue &input) {
    return std::visit(
        [&](const auto &v) -> ValueId {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, sct::SctEncodedWordValue>)
                return out.add(ValueKind::Word, v.value);
            else if constexpr (std::is_same_v<T, sct::SctCanonicalExpression>) {
                auto expression = ValueBuffer::expression(v);
                require(bool(expression), "Cannot encode expression draft");
                return copy(out, expression.value(), expression.value().root);
            } else if constexpr (std::is_same_v<T, sct::SctTerminatedWordSequenceValue> ||
                                 std::is_same_v<T, sct::SctUnresolvedReferenceValue>) {
                std::vector<ValueId> words;
                if constexpr (std::is_same_v<T, sct::SctTerminatedWordSequenceValue>) {
                    for (auto w : v.words)
                        words.push_back(out.add(ValueKind::Word, w));
                    return out.add(ValueKind::Words, 0, 0, words);
                } else {
                    for (auto w : v.encodedWords)
                        words.push_back(out.add(ValueKind::Word, w));
                    return out.add(
                        ValueKind::Unresolved, static_cast<std::uint64_t>(v.expectedTarget.storage),
                        v.expectedTarget.textKind ? 1 + static_cast<unsigned>(*v.expectedTarget.textKind) : 0, words);
                }
            } else if constexpr (std::is_same_v<T, sct::SctInstructionReference> ||
                                 std::is_same_v<T, sct::SctStringReference> ||
                                 std::is_same_v<T, sct::SctSupplementaryTextReference>) {
                throw std::runtime_error("Bind references with authored IDs after creating the instruction draft");
            } else
                return out.add(ValueKind::Unavailable, v.words.size(), 2);
        },
        input);
}
} // namespace
Result<ValueBuffer> ProgramValues::compose(ValueKind kind, std::uint64_t data, std::uint32_t detail,
                                           std::span<const ValueBuffer> values) {
    return guarded([&] {
        ValueBuffer out;
        std::vector<ValueId> children;
        for (const auto &value : values)
            children.push_back(copy(out, value, value.root));
        out.root = out.add(kind, data, detail, children);
        return out;
    });
}
Result<ValueBuffer> ProgramValues::slice(const ValueBuffer &b, ValueId root) {
    return guarded([&] {
        ValueBuffer out;
        out.root = copy(out, b, root);
        return out;
    });
}
Result<ValueBuffer> ProgramValues::replace(const ValueBuffer &b, ValueId root, ValueId target,
                                           const ValueBuffer &replacement) {
    return guarded([&] {
        ValueBuffer out;
        out.root = copy(out, b, root, target, &replacement);
        return out;
    });
}
Result<sct::SctCanonicalExpression> ProgramValues::expression(const ValueBuffer &b, ValueId id) {
    return guarded([&] { return TypedValues<View>(View{b}).expression(id); });
}
Result<sct::SctTextValue> ProgramValues::text(const ValueBuffer &b, ValueId id) {
    return guarded([&] { return TypedValues<View>(View{b}).text(id); });
}
Result<ValueBuffer> ProgramValues::text(const sct::SctTextValue &value) {
    return guarded([&] {
        ValueBuffer out;
        out.root = encodeText(out, value);
        return out;
    });
}
Result<ValueBuffer> ProgramValues::instruction(const sct::SctInstructionFactoryRequest &request) {
    return guarded([&] {
        auto draft = sct::SctInstructionFactory::createDraft(request);
        require(draft.draft.has_value(),
                draft.diagnostics.empty() ? "Instruction is unavailable" : draft.diagnostics.front().message.c_str());
        ValueBuffer out;
        std::vector<ValueId> children;
        std::map<unsigned, std::vector<ValueId>> groups;
        if (draft.draft->scheduledExpression)
            children.push_back(parameter(out, *draft.draft->scheduledExpression));
        for (const auto &p : draft.draft->parameters) {
            auto value = p.value ? p.value : p.suggestedValue;
            auto id = value ? parameter(out, *value) : out.add(ValueKind::Unavailable, 0, 2);
            auto param = out.add(ValueKind::Parameter, p.address.schemaIndex, 0, std::span(&id, 1));
            if (p.address.repeatedGroupOrdinal)
                groups[*p.address.repeatedGroupOrdinal].push_back(param);
            else
                children.push_back(param);
        }
        for (auto &[ordinal, items] : groups)
            children.push_back(out.add(ValueKind::RepeatedGroup, 0, 0, items));
        out.root = out.add(ValueKind::Instruction, request.opcode,
                           (request.skipRefresh ? 1 : 0) | (request.scheduledExpression ? 2 : 0), children);
        return out;
    });
}
} // namespace salsa::core::program
