#pragma once

#include "SpiceSCT/SctDocumentIndex.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace salsa::core {

struct SctParameterSite final {
    spice::sct::SctInstructionId instruction;
    spice::sct::SctParameterAddress parameter;
    auto operator<=>(const SctParameterSite&) const = default;
};

struct SctScheduledExpressionSite final {
    auto operator<=>(const SctScheduledExpressionSite&) const = default;
};

using SctExpressionOwner = std::variant<
    SctScheduledExpressionSite,
    spice::sct::SctParameterAddress>;

struct SctExpressionSite final {
    spice::sct::SctInstructionId instruction;
    SctExpressionOwner owner;
    std::vector<std::uint32_t> childPath{};
    auto operator<=>(const SctExpressionSite&) const = default;
};

struct SctOpcodeUsage final {
    std::uint16_t opcode = 0;
    spice::sct::SctInstructionId instruction;
    auto operator<=>(const SctOpcodeUsage&) const = default;
};

struct SctReferenceUsage final {
    SctParameterSite source;
    spice::sct::SctDocumentReferenceTarget target;
    auto operator<=>(const SctReferenceUsage&) const = default;
};

enum class SctVariableKind {
    Integer,
    Float,
    Bit,
    Byte,
};

struct SctVariableIdentity final {
    SctVariableKind kind = SctVariableKind::Integer;
    std::uint32_t index = 0;
    auto operator<=>(const SctVariableIdentity&) const = default;
};

struct SctVariableUsage final {
    SctVariableIdentity variable;
    SctExpressionSite source;
    auto operator<=>(const SctVariableUsage&) const = default;
};

struct SctUnresolvedReferenceUsage final {
    SctParameterSite source;
    spice::sct::SctExpectedReferenceTarget expectedTarget;
    std::size_t encodedWordCount = 0;
    auto operator<=>(const SctUnresolvedReferenceUsage&) const = default;
};

struct SctOpaqueParameterUsage final {
    SctParameterSite source;
    std::size_t wordCount = 0;
    auto operator<=>(const SctOpaqueParameterUsage&) const = default;
};

struct SctOpaqueExpressionUsage final {
    SctExpressionSite source;
    std::size_t wordCount = 0;
    auto operator<=>(const SctOpaqueExpressionUsage&) const = default;
};

class SctSemanticUsageIndex final {
public:
    [[nodiscard]] static SctSemanticUsageIndex build(
        const spice::sct::SctDocument& document);

    [[nodiscard]] std::span<const SctOpcodeUsage> opcodeUsages() const noexcept;
    [[nodiscard]] std::span<const SctReferenceUsage> referenceUsages() const noexcept;
    [[nodiscard]] std::span<const SctVariableUsage> variableUsages() const noexcept;
    [[nodiscard]] std::span<const SctUnresolvedReferenceUsage>
        unresolvedReferences() const noexcept;
    [[nodiscard]] std::span<const SctOpaqueParameterUsage>
        opaqueParameters() const noexcept;
    [[nodiscard]] std::span<const SctOpaqueExpressionUsage>
        opaqueExpressions() const noexcept;

    [[nodiscard]] std::vector<SctOpcodeUsage> usagesForOpcode(
        std::uint16_t opcode) const;
    [[nodiscard]] std::vector<SctReferenceUsage> outboundReferences(
        spice::sct::SctInstructionId source) const;
    [[nodiscard]] std::vector<SctReferenceUsage> inboundReferences(
        const spice::sct::SctDocumentReferenceTarget& target) const;
    [[nodiscard]] std::vector<SctVariableUsage> usagesForVariable(
        SctVariableIdentity variable) const;

private:
    void recordExpression(
        spice::sct::SctInstructionId instruction,
        SctExpressionOwner owner,
        const spice::sct::SctCanonicalExpression& expression);
    void recordExpressionNode(
        spice::sct::SctInstructionId instruction,
        const SctExpressionOwner& owner,
        const spice::sct::SctCanonicalExpressionNode& node,
        std::vector<std::uint32_t>& childPath);
    void recordParameter(
        spice::sct::SctInstructionId instruction,
        const spice::sct::SctDocumentParameter& parameter,
        std::optional<std::uint32_t> repeatedGroupOrdinal);

    std::vector<SctOpcodeUsage> opcodeUsages_{};
    std::vector<SctReferenceUsage> referenceUsages_{};
    std::vector<SctVariableUsage> variableUsages_{};
    std::vector<SctUnresolvedReferenceUsage> unresolvedReferences_{};
    std::vector<SctOpaqueParameterUsage> opaqueParameters_{};
    std::vector<SctOpaqueExpressionUsage> opaqueExpressions_{};
};

}  // namespace salsa::core
