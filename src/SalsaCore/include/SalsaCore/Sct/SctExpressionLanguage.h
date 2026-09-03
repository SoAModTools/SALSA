#pragma once

#include "SpiceSCT/SctDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace salsa::core {

enum class SctExpressionTextAvailability {
    Editable,
    DerivedPreviewOnly,
    Unavailable,
};

struct SctExpressionTextIssue final {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string message;
};

struct SctExpressionTextProjection final {
    SctExpressionTextAvailability availability =
        SctExpressionTextAvailability::Unavailable;
    std::string text;
    std::vector<std::uint32_t> encodedWords;
    std::string explanation;
    bool usesAlternateOperatorEncoding = false;
};

struct SctExpressionTextParseResult final {
    std::optional<spice::sct::SctCanonicalExpression> expression;
    std::vector<SctExpressionTextIssue> issues;

    [[nodiscard]] bool succeeded() const noexcept {
        return expression.has_value() && issues.empty();
    }
};

// SALSA's deliberately small authoring language for conventional SCPT
// expressions. The private AST is temporary parser state; the resulting
// SpiceSCT ordered program remains the only document representation.
class SctExpressionLanguage final {
public:
    [[nodiscard]] static SctExpressionTextProjection project(
        const spice::sct::SctCanonicalExpression& expression);
    [[nodiscard]] static SctExpressionTextParseResult parse(
        std::string text);
};

} // namespace salsa::core
