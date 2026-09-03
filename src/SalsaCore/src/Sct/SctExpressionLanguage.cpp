#include "SalsaCore/Sct/SctExpressionLanguage.h"

#include "SpiceSCT/SctInstructionFactory.h"
#include "SpiceSCT/SctScptEncoding.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace salsa::core {
namespace {

using spice::sct::SctExpressionBinaryOperator;
using spice::sct::SctScptBinaryOperation;
using spice::sct::SctScptOperation;
using spice::sct::SctScptValueOperation;

enum class NodeKind { Fixed, Float, Variable, IntegerInput, Secondary, Inline, Binary };

struct Node final {
    NodeKind kind = NodeKind::Fixed;
    std::int32_t fixedUnits = 0;
    float floatValue = 0;
    spice::sct::SctScptValueKind valueKind =
        spice::sct::SctScptValueKind::DecimalLiteral;
    std::uint32_t value = 0;
    SctExpressionBinaryOperator binary = SctExpressionBinaryOperator::Add;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
};

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string hexWord(const std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << value;
    return stream.str();
}

std::string fixedText(const std::int32_t units) {
    const bool negative = units < 0;
    const auto magnitude = static_cast<std::uint32_t>(negative
        ? -static_cast<std::int64_t>(units) : units);
    const auto whole = magnitude / 256u;
    const auto fraction = magnitude % 256u;
    std::string result = negative ? "-" : "";
    result += std::to_string(whole);
    if (fraction == 0u) return result;
    auto digits = std::to_string((static_cast<std::uint64_t>(fraction)
        * 100000000ull) / 256ull);
    if (digits.size() < 8u) digits.insert(0, 8u - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    return result + "." + digits;
}

std::string floatText(const float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    auto result = stream.str();
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result + "f";
}

std::optional<SctExpressionBinaryOperator> binaryKind(const std::uint32_t word) {
    using enum SctExpressionBinaryOperator;
    switch (word) {
    case 0x00u: return Less;
    case 0x01u: return LessOrEqual;
    case 0x02u: return Greater;
    case 0x03u: return GreaterOrEqual;
    case 0x04u: return Equal;
    case 0x05u: return NotEqual;
    case 0x06u: case 0x10u: return BitAnd;
    case 0x07u: case 0x11u: return BitOr;
    case 0x08u: return LogicalAnd;
    case 0x09u: return LogicalOr;
    case 0x0bu: case 0x12u: return Multiply;
    case 0x0cu: case 0x13u: return Divide;
    case 0x0du: case 0x14u: return Modulo;
    case 0x0eu: case 0x15u: return Add;
    case 0x0fu: case 0x16u: return Subtract;
    default: return std::nullopt;
    }
}

bool alternateOperator(const std::uint32_t word) {
    return word >= 0x10u && word <= 0x16u;
}

int precedence(const SctExpressionBinaryOperator value) {
    using enum SctExpressionBinaryOperator;
    switch (value) {
    case Multiply: case Divide: case Modulo: return 80;
    case Add: case Subtract: return 70;
    case Less: case LessOrEqual: case Greater: case GreaterOrEqual: return 60;
    case Equal: case NotEqual: return 50;
    case BitAnd: return 40;
    case BitOr: return 30;
    case LogicalAnd: return 20;
    case LogicalOr: return 10;
    }
    return 0;
}

std::string symbol(const SctExpressionBinaryOperator value) {
    using enum SctExpressionBinaryOperator;
    switch (value) {
    case Less: return "<";
    case LessOrEqual: return "<=";
    case Greater: return ">";
    case GreaterOrEqual: return ">=";
    case Equal: return "==";
    case NotEqual: return "!=";
    case BitAnd: return "&";
    case BitOr: return "|";
    case LogicalAnd: return "&&";
    case LogicalOr: return "||";
    case Multiply: return "*";
    case Divide: return "/";
    case Modulo: return "%";
    case Add: return "+";
    case Subtract: return "-";
    }
    return "?";
}

std::optional<Node> valueNode(const SctScptValueOperation& operation) {
    Node result;
    result.valueKind = operation.kind;
    using enum spice::sct::SctScptValueKind;
    switch (operation.kind) {
    case DecimalLiteral: {
        auto encoded = static_cast<std::int32_t>(operation.encodingWord & 0x00ffffffu);
        if ((encoded & 0x00800000) != 0)
            encoded |= static_cast<std::int32_t>(0xff000000u);
        result.kind = NodeKind::Fixed;
        result.fixedUnits = encoded;
        return result;
    }
    case FloatLiteral:
        if (operation.payloadWords.size() != 1u) return std::nullopt;
        result.floatValue = std::bit_cast<float>(operation.payloadWords.front());
        if (!std::isfinite(result.floatValue)) return std::nullopt;
        result.kind = NodeKind::Float;
        return result;
    case ByteVariable:
        result.kind = NodeKind::Variable;
        result.value = operation.encodingWord & 0x0fffffffu;
        return result;
    case BitVariable:
        result.kind = NodeKind::Variable;
        result.value = operation.encodingWord & 0x1fffffffu;
        return result;
    case FloatVariable:
        result.kind = NodeKind::Variable;
        result.value = operation.encodingWord & 0x0fffffffu;
        return result;
    case FloatBackedIntegerVariable: case IntegerVariable:
    case IntegerVariableLow16Comparison:
        result.kind = NodeKind::Variable;
        result.value = operation.encodingWord & 0x00ffffffu;
        return result;
    case SecondaryValue:
        result.kind = NodeKind::Secondary;
        result.value = operation.encodingWord & 0x00ffffffu;
        return result;
    case InlineValue:
        result.kind = NodeKind::Inline;
        result.value = operation.encodingWord;
        return result;
    }
    return std::nullopt;
}

std::optional<Node> projectNode(
    const spice::sct::SctScptDerivedExpressionNode& source,
    const std::vector<SctScptOperation>& operations,
    std::unordered_set<std::uint32_t>& seen,
    bool& alternate,
    const bool rejectDuplicateOrdinals = true) {
    if (source.operationOrdinal >= operations.size()
        || (rejectDuplicateOrdinals
            && !seen.insert(source.operationOrdinal).second)) return std::nullopt;
    seen.insert(source.operationOrdinal);
    const auto& operation = operations[source.operationOrdinal];
    if (const auto* value = std::get_if<SctScptValueOperation>(&operation)) {
        if (!source.children.empty()) return std::nullopt;
        return valueNode(*value);
    }
    const auto* binary = std::get_if<SctScptBinaryOperation>(&operation);
    if (binary == nullptr || source.children.size() != 2u) return std::nullopt;
    const auto kind = binaryKind(binary->encodingWord);
    if (!kind) return std::nullopt;
    auto left = projectNode(source.children[0], operations, seen, alternate,
        rejectDuplicateOrdinals);
    auto right = projectNode(source.children[1], operations, seen, alternate,
        rejectDuplicateOrdinals);
    if (!left || !right) return std::nullopt;
    alternate = alternate || alternateOperator(binary->encodingWord);
    Node result;
    result.kind = NodeKind::Binary;
    result.binary = *kind;
    result.left = std::make_unique<Node>(std::move(*left));
    result.right = std::make_unique<Node>(std::move(*right));
    return result;
}

std::string secondaryText(const std::uint32_t value) {
    switch (value) {
    case 0u: return "Gold";
    case 1u: return "Reputation";
    case 2u: return "VyseCurrentHP";
    case 3u: return "AikaCurrentHP";
    case 4u: return "FinaCurrentHP";
    case 5u: return "DrachmaCurrentHP";
    case 6u: return "EnriqueCurrentHP";
    case 7u: return "GilderCurrentHP";
    case 0x4au: return "VyseLevel";
    default: return "SecondaryValue[" + std::to_string(value) + "]";
    }
}

std::string nodeText(const Node& node, const int parentPrecedence = 0,
    const bool rightChild = false) {
    using enum spice::sct::SctScptValueKind;
    switch (node.kind) {
    case NodeKind::Fixed: return fixedText(node.fixedUnits);
    case NodeKind::Float: return floatText(node.floatValue);
    case NodeKind::Secondary: return secondaryText(node.value);
    case NodeKind::IntegerInput: return "IntInput[" + std::to_string(node.value) + "]";
    case NodeKind::Inline: return "InlineValue[" + hexWord(node.value) + "]";
    case NodeKind::Variable: {
        std::string name;
        switch (node.valueKind) {
        case ByteVariable: name = "ByteVar"; break;
        case BitVariable: name = "BitVar"; break;
        case FloatVariable: name = "FloatVar"; break;
        case FloatBackedIntegerVariable: name = "FloatBackedIntVar"; break;
        case IntegerVariable: name = "IntVar"; break;
        case IntegerVariableLow16Comparison: name = "Low16IntVar"; break;
        default: name = "Value"; break;
        }
        return name + "[" + std::to_string(node.value) + "]";
    }
    case NodeKind::Binary: break;
    }
    const auto own = precedence(node.binary);
    auto result = nodeText(*node.left, own, false) + " " + symbol(node.binary)
        + " " + nodeText(*node.right, own, true);
    // Parenthesize the right child at equal precedence so subtraction/division
    // and mixed operators retain the exact tree. The left child follows normal
    // left associativity.
    if (own < parentPrecedence || (rightChild && own == parentPrecedence))
        result = "(" + result + ")";
    return result;
}

enum class TokenKind {
    End, Number, Identifier, LParen, RParen, LBracket, RBracket,
    Plus, Minus, Star, Slash, Percent, Less, LessEqual, Greater,
    GreaterEqual, EqualEqual, NotEqual, Amp, Pipe, AmpAmp, PipePipe,
    Invalid,
};

struct Token final {
    TokenKind kind = TokenKind::End;
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string text;
};

class Lexer final {
public:
    explicit Lexer(std::string_view text) : text_(text) {}
    Token next() {
        while (position_ < text_.size()
            && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
        const auto start = position_;
        if (position_ == text_.size()) return {TokenKind::End, start, 0, {}};
        const char c = text_[position_++];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (position_ < text_.size()) {
                const auto next = static_cast<unsigned char>(text_[position_]);
                if (!std::isalnum(next) && text_[position_] != '_') break;
                ++position_;
            }
            return token(TokenKind::Identifier, start);
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            if (c == '0' && position_ < text_.size()
                && (text_[position_] == 'x' || text_[position_] == 'X')) {
                ++position_;
                while (position_ < text_.size()
                    && std::isxdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
            } else {
                bool exponent = false;
                while (position_ < text_.size()) {
                    const char next = text_[position_];
                    if (std::isdigit(static_cast<unsigned char>(next)) || next == '.') {
                        ++position_; continue;
                    }
                    if (!exponent && (next == 'e' || next == 'E')) {
                        exponent = true; ++position_;
                        if (position_ < text_.size()
                            && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
                        continue;
                    }
                    break;
                }
                if (position_ < text_.size()
                    && (text_[position_] == 'f' || text_[position_] == 'F')) ++position_;
            }
            return token(TokenKind::Number, start);
        }
        const auto paired = [&](const char expected, const TokenKind two,
                                const TokenKind one) {
            if (position_ < text_.size() && text_[position_] == expected) {
                ++position_; return token(two, start);
            }
            return token(one, start);
        };
        switch (c) {
        case '(': return token(TokenKind::LParen, start);
        case ')': return token(TokenKind::RParen, start);
        case '[': return token(TokenKind::LBracket, start);
        case ']': return token(TokenKind::RBracket, start);
        case '+': return token(TokenKind::Plus, start);
        case '-': return token(TokenKind::Minus, start);
        case '*': return token(TokenKind::Star, start);
        case '/': return token(TokenKind::Slash, start);
        case '%': return token(TokenKind::Percent, start);
        case '<': return paired('=', TokenKind::LessEqual, TokenKind::Less);
        case '>': return paired('=', TokenKind::GreaterEqual, TokenKind::Greater);
        case '=': return paired('=', TokenKind::EqualEqual, TokenKind::Invalid);
        case '!': return paired('=', TokenKind::NotEqual, TokenKind::Invalid);
        case '&': return paired('&', TokenKind::AmpAmp, TokenKind::Amp);
        case '|': return paired('|', TokenKind::PipePipe, TokenKind::Pipe);
        default: return token(TokenKind::Invalid, start);
        }
    }
private:
    Token token(const TokenKind kind, const std::size_t start) const {
        return {kind, start, position_ - start,
            std::string(text_.substr(start, position_ - start))};
    }
    std::string_view text_;
    std::size_t position_ = 0;
};

std::optional<std::uint32_t> parseUnsigned(const std::string_view text) {
    std::uint64_t result = 0;
    int base = 10;
    auto first = text.data();
    if (text.size() > 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        first += 2; base = 16;
    }
    const auto [end, error] = std::from_chars(first, text.data() + text.size(), result, base);
    if (error != std::errc{} || end != text.data() + text.size()
        || result > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(result);
}

std::optional<std::int32_t> parseFixed(std::string text, const bool negative) {
    if (text.find_first_of("eEfF") != std::string::npos || text.starts_with("0x"))
        return std::nullopt;
    const auto dot = text.find('.');
    auto wholeText = dot == std::string::npos ? text : text.substr(0, dot);
    auto fractionText = dot == std::string::npos ? std::string{} : text.substr(dot + 1u);
    if (wholeText.empty()) wholeText = "0";
    if (fractionText.size() > 9u || (dot != std::string::npos && fractionText.empty()))
        return std::nullopt;
    std::uint64_t whole = 0;
    if (const auto [end, error] = std::from_chars(wholeText.data(),
            wholeText.data() + wholeText.size(), whole);
        error != std::errc{} || end != wholeText.data() + wholeText.size()) return std::nullopt;
    std::uint64_t fraction = 0;
    std::uint64_t denominator = 1;
    if (!fractionText.empty()) {
        const auto [end, error] = std::from_chars(fractionText.data(),
            fractionText.data() + fractionText.size(), fraction);
        if (error != std::errc{} || end != fractionText.data() + fractionText.size())
            return std::nullopt;
        for (std::size_t i = 0; i < fractionText.size(); ++i) denominator *= 10u;
    }
    if ((fraction * 256u) % denominator != 0u) return std::nullopt;
    const auto magnitude = whole * 256u + (fraction * 256u) / denominator;
    const auto signedValue = negative ? -static_cast<std::int64_t>(magnitude)
                                      : static_cast<std::int64_t>(magnitude);
    if (signedValue < -0x800000ll || signedValue > 0x7fffffll) return std::nullopt;
    return static_cast<std::int32_t>(signedValue);
}

class Parser final {
public:
    explicit Parser(std::string text) : text_(std::move(text)), lexer_(text_) {
        advance();
    }

    SctExpressionTextParseResult run() {
        auto node = expression(0);
        if (node && current_.kind != TokenKind::End)
            issue(current_, "Unexpected text after the expression.");
        SctExpressionTextParseResult result;
        result.issues = std::move(issues_);
        if (!node || !result.issues.empty()) return result;
        auto built = build(*node);
        if (!built.expression) {
            const auto message = built.diagnostics.empty()
                ? "The expression cannot be represented as a conventional SCPT program."
                : built.diagnostics.front().message;
            result.issues.push_back({0, text_.size(), message});
            return result;
        }
        result.expression = std::move(*built.expression);
        return result;
    }

private:
    static std::optional<std::pair<int, SctExpressionBinaryOperator>> binary(
        const TokenKind kind) {
        using enum SctExpressionBinaryOperator;
        switch (kind) {
        case TokenKind::Star: return {{80, Multiply}};
        case TokenKind::Slash: return {{80, Divide}};
        case TokenKind::Percent: return {{80, Modulo}};
        case TokenKind::Plus: return {{70, Add}};
        case TokenKind::Minus: return {{70, Subtract}};
        case TokenKind::Less: return {{60, Less}};
        case TokenKind::LessEqual: return {{60, LessOrEqual}};
        case TokenKind::Greater: return {{60, Greater}};
        case TokenKind::GreaterEqual: return {{60, GreaterOrEqual}};
        case TokenKind::EqualEqual: return {{50, Equal}};
        case TokenKind::NotEqual: return {{50, NotEqual}};
        case TokenKind::Amp: return {{40, BitAnd}};
        case TokenKind::Pipe: return {{30, BitOr}};
        case TokenKind::AmpAmp: return {{20, LogicalAnd}};
        case TokenKind::PipePipe: return {{10, LogicalOr}};
        default: return std::nullopt;
        }
    }

    std::unique_ptr<Node> expression(const int minimum) {
        auto left = primary();
        while (left) {
            const auto operation = binary(current_.kind);
            if (!operation || operation->first < minimum) break;
            const auto kind = operation->second;
            const auto precedence = operation->first;
            advance();
            auto right = expression(precedence + 1);
            if (!right) return nullptr;
            auto parent = std::make_unique<Node>();
            parent->kind = NodeKind::Binary;
            parent->binary = kind;
            parent->left = std::move(left);
            parent->right = std::move(right);
            left = std::move(parent);
        }
        return left;
    }

    std::unique_ptr<Node> primary() {
        if (++nodeCount_ > 512u) {
            issue(current_, "Expression is too large."); return nullptr;
        }
        if (current_.kind == TokenKind::LParen) {
            advance();
            auto result = expression(0);
            if (current_.kind != TokenKind::RParen) {
                issue(current_, "Expected ')'."); return nullptr;
            }
            advance(); return result;
        }
        bool negative = false;
        if (current_.kind == TokenKind::Minus) {
            negative = true; advance();
            if (current_.kind != TokenKind::Number) {
                issue(current_, "A leading '-' is supported only for numeric literals.");
                return nullptr;
            }
        }
        if (current_.kind == TokenKind::Number) {
            const auto token = current_; advance();
            auto node = std::make_unique<Node>();
            const bool floating = token.text.ends_with('f') || token.text.ends_with('F');
            if (floating) {
                auto number = token.text.substr(0, token.text.size() - 1u);
                if (negative) number.insert(number.begin(), '-');
                std::istringstream stream(number);
                stream.imbue(std::locale::classic());
                float value = 0;
                stream >> value;
                if (!stream || !stream.eof() || !std::isfinite(value)) {
                    issue(token, "Enter a finite IEEE-754 float followed by 'f'.");
                    return nullptr;
                }
                node->kind = NodeKind::Float; node->floatValue = value;
            } else {
                const auto fixed = parseFixed(token.text, negative);
                if (!fixed) {
                    issue(token, "Fixed decimals must fit the signed 24-bit domain exactly in increments of 1/256; use an 'f' suffix for floats.");
                    return nullptr;
                }
                node->kind = NodeKind::Fixed; node->fixedUnits = *fixed;
            }
            return node;
        }
        if (negative) return nullptr;
        if (current_.kind != TokenKind::Identifier) {
            issue(current_, "Expected a literal, named value, variable, or parenthesized expression.");
            return nullptr;
        }
        const auto identifier = current_; advance();
        const auto name = lower(identifier.text);
        const auto secondary = secondaryValue(name);
        if (secondary) {
            auto node = std::make_unique<Node>(); node->kind = NodeKind::Secondary;
            node->value = *secondary; return node;
        }
        if (current_.kind != TokenKind::LBracket) {
            issue(identifier, "Unknown named SCPT value."); return nullptr;
        }
        advance();
        if (current_.kind != TokenKind::Number) {
            issue(current_, "Expected an index or encoded word inside brackets.");
            return nullptr;
        }
        const auto number = current_; advance();
        if (current_.kind != TokenKind::RBracket) {
            issue(current_, "Expected ']'."); return nullptr;
        }
        advance();
        const auto parsed = parseUnsigned(number.text);
        if (!parsed) { issue(number, "Index is outside the 32-bit unsigned domain."); return nullptr; }
        auto node = std::make_unique<Node>(); node->value = *parsed;
        if (name == "inlinevalue") node->kind = NodeKind::Inline;
        else if (name == "intinput") node->kind = NodeKind::IntegerInput;
        else {
            node->kind = NodeKind::Variable;
            if (name == "bytevar") node->valueKind = spice::sct::SctScptValueKind::ByteVariable;
            else if (name == "bitvar") node->valueKind = spice::sct::SctScptValueKind::BitVariable;
            else if (name == "floatvar") node->valueKind = spice::sct::SctScptValueKind::FloatVariable;
            else if (name == "floatbackedintvar") node->valueKind = spice::sct::SctScptValueKind::FloatBackedIntegerVariable;
            else if (name == "intvar") node->valueKind = spice::sct::SctScptValueKind::IntegerVariable;
            else if (name == "low16intvar") node->valueKind = spice::sct::SctScptValueKind::IntegerVariableLow16Comparison;
            else { issue(identifier, "Unknown SCPT variable kind."); return nullptr; }
        }
        return node;
    }

    static std::optional<std::uint32_t> secondaryValue(const std::string& name) {
        if (name == "gold") return 0u;
        if (name == "reputation") return 1u;
        if (name == "vysecurrenthp") return 2u;
        if (name == "aikacurrenthp") return 3u;
        if (name == "finacurrenthp") return 4u;
        if (name == "drachmacurrenthp") return 5u;
        if (name == "enriquecurrenthp") return 6u;
        if (name == "gildercurrenthp") return 7u;
        if (name == "vyselevel") return 0x4au;
        return std::nullopt;
    }

    spice::sct::SctExpressionBuildResult build(const Node& node) const {
        using enum spice::sct::SctScptValueKind;
        switch (node.kind) {
        case NodeKind::Fixed:
            return spice::sct::SctExpressionFactory::scaledDecimalLiteral(node.fixedUnits);
        case NodeKind::Float: {
            spice::sct::SctExpressionBuildResult result;
            result.expression = spice::sct::SctExpressionFactory::floatLiteral(node.floatValue);
            return result;
        }
        case NodeKind::Secondary: {
            spice::sct::SctExpressionBuildResult result;
            result.expression = spice::sct::SctExpressionFactory::secondaryValue(
                static_cast<spice::sct::SctExpressionSecondaryValue>(node.value));
            return result;
        }
        case NodeKind::IntegerInput:
            return spice::sct::SctExpressionFactory::integerInput(node.value);
        case NodeKind::Inline: {
            spice::sct::SctExpressionBuildResult result;
            if (!spice::sct::isSctScptInlineValue(node.value)) return result;
            result.expression = spice::sct::SctExpressionFactory::oneWordValue(
                static_cast<spice::sct::SctExpressionOneWordValue>(node.value));
            return result;
        }
        case NodeKind::Variable:
            switch (node.valueKind) {
            case ByteVariable: return spice::sct::SctExpressionFactory::byteVariable(node.value);
            case BitVariable: return spice::sct::SctExpressionFactory::bitVariable(node.value);
            case FloatVariable: return spice::sct::SctExpressionFactory::floatVariable(node.value);
            case FloatBackedIntegerVariable: return spice::sct::SctExpressionFactory::floatBackedIntegerVariable(node.value);
            case IntegerVariable: return spice::sct::SctExpressionFactory::integerVariable(node.value);
            case IntegerVariableLow16Comparison:
                return spice::sct::SctExpressionFactory::low16ComparisonIntegerVariable(node.value);
            default: return {};
            }
        case NodeKind::Binary: {
            auto left = build(*node.left);
            auto right = build(*node.right);
            if (!left.expression) return left;
            if (!right.expression) return right;
            return spice::sct::SctExpressionFactory::binaryOperator(node.binary,
                std::move(*left.expression), std::move(*right.expression));
        }
        }
        return {};
    }

    void issue(const Token& token, std::string message) {
        if (issues_.empty()) issues_.push_back({token.offset,
            std::max<std::size_t>(token.length, 1u), std::move(message)});
    }
    void advance() { current_ = lexer_.next(); }

    std::string text_;
    Lexer lexer_;
    Token current_;
    std::vector<SctExpressionTextIssue> issues_;
    std::size_t nodeCount_ = 0;
};

} // namespace

SctExpressionTextProjection SctExpressionLanguage::project(
    const spice::sct::SctCanonicalExpression& expression) {
    SctExpressionTextProjection result;
    result.encodedWords = spice::sct::encodeSctCanonicalExpressionWords(expression);
    const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&expression.body);
    if (program == nullptr) {
        result.explanation = "Opaque SCPT words have no typed expression projection.";
        return result;
    }
    const auto analysis = spice::sct::analyzeSctScptProgram(*program);
    if (!analysis.conventionalTree) {
        if (analysis.returnedExpression) {
            std::unordered_set<std::uint32_t> previewSeen;
            bool previewAlternate = false;
            const auto preview = projectNode(*analysis.returnedExpression,
                program->operations, previewSeen, previewAlternate, false);
            if (preview) {
                result.text = nodeText(*preview);
                result.availability = SctExpressionTextAvailability::DerivedPreviewOnly;
                result.usesAlternateOperatorEncoding = previewAlternate;
                result.explanation = "The semantic result is a preview only; the ordered program does not have one exact conventional representation.";
                return result;
            }
        }
        result.explanation = "The ordered program does not produce one conventional result.";
        return result;
    }
    std::unordered_set<std::uint32_t> seen;
    bool alternate = false;
    auto node = projectNode(*analysis.conventionalTree, program->operations, seen, alternate);
    if (!node || seen.size() != program->operations.size()) {
        // The derived result is still useful for inspection, but duplicated or
        // noncontributing operation ordinals cannot round-trip through text.
        result.availability = SctExpressionTextAvailability::DerivedPreviewOnly;
        if (node) result.text = nodeText(*node);
        result.explanation = "The derived result depends on stack or noncontributing operations and is preview-only.";
        return result;
    }
    if (node->kind == NodeKind::Inline) {
        if (program->operations.size() != 1u
            || expression.termination != spice::sct::SctExpressionTermination::InlineValue) {
            result.availability = SctExpressionTextAvailability::DerivedPreviewOnly;
            result.explanation = "The inline-value termination cannot round-trip through a compound expression.";
            return result;
        }
    } else if (expression.termination != spice::sct::SctExpressionTermination::StopCode) {
        result.availability = SctExpressionTextAvailability::DerivedPreviewOnly;
        result.explanation = "The expression uses a nonconventional termination mode.";
        return result;
    }
    result.text = nodeText(*node);
    result.availability = SctExpressionTextAvailability::Editable;
    result.usesAlternateOperatorEncoding = alternate;
    if (alternate)
        result.explanation = "Editing will replace alternate operator encodings with canonical SpiceSCT encodings.";
    return result;
}

SctExpressionTextParseResult SctExpressionLanguage::parse(std::string text) {
    if (text.empty()) return {{}, {{0, 1, "Enter an SCPT expression."}}};
    return Parser(std::move(text)).run();
}

} // namespace salsa::core
