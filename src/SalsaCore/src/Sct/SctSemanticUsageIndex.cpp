#include "SalsaCore/Sct/SctSemanticUsageIndex.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

[[nodiscard]] std::optional<SctVariableKind> variableKind(
    const spice::sct::SctCanonicalExpressionNodeKind kind) {
    using enum spice::sct::SctCanonicalExpressionNodeKind;
    switch (kind) {
    case IntVariable: return SctVariableKind::Integer;
    case FloatVariable: return SctVariableKind::Float;
    case BitVariable: return SctVariableKind::Bit;
    case ByteVariable: return SctVariableKind::Byte;
    default: return std::nullopt;
    }
}

}  // namespace

SctSemanticUsageIndex SctSemanticUsageIndex::build(
    const spice::sct::SctDocument& document) {
    SctSemanticUsageIndex result;
    for (const auto& section : document.sections) {
        const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
            &section.content);
        if (script == nullptr) continue;
        for (const auto& instruction : script->instructions) {
            result.opcodeUsages_.push_back({ instruction.opcode, instruction.id });
            if (instruction.scheduledExpression.has_value()) {
                result.recordExpression(instruction.id, SctScheduledExpressionSite{},
                    *instruction.scheduledExpression);
            }
            for (const auto& parameter : instruction.fixedParameters) {
                result.recordParameter(instruction.id, parameter, std::nullopt);
            }
            for (std::size_t groupOrdinal = 0;
                groupOrdinal < instruction.repeatedParameterGroups.size(); ++groupOrdinal) {
                for (const auto& parameter
                    : instruction.repeatedParameterGroups[groupOrdinal].parameters) {
                    result.recordParameter(instruction.id, parameter,
                        static_cast<std::uint32_t>(groupOrdinal));
                }
            }
        }
    }
    return result;
}

SctInstructionSemanticContribution SctSemanticUsageIndex::contributionFor(
    const spice::sct::SctDocumentInstruction& instruction) {
    SctSemanticUsageIndex index;
    index.opcodeUsages_.push_back({instruction.opcode, instruction.id});
    if (instruction.scheduledExpression.has_value()) {
        index.recordExpression(instruction.id, SctScheduledExpressionSite{},
            *instruction.scheduledExpression);
    }
    for (const auto& parameter : instruction.fixedParameters)
        index.recordParameter(instruction.id, parameter, std::nullopt);
    for (std::size_t group = 0;
        group < instruction.repeatedParameterGroups.size(); ++group) {
        for (const auto& parameter
            : instruction.repeatedParameterGroups[group].parameters) {
            index.recordParameter(instruction.id, parameter,
                static_cast<std::uint32_t>(group));
        }
    }
    return {
        std::move(index.opcodeUsages_),
        std::move(index.referenceUsages_),
        std::move(index.variableUsages_),
        std::move(index.unresolvedReferences_),
        std::move(index.opaqueParameters_),
        std::move(index.opaqueExpressions_),
    };
}

std::span<const SctOpcodeUsage> SctSemanticUsageIndex::opcodeUsages() const noexcept {
    return opcodeUsages_;
}

std::span<const SctReferenceUsage> SctSemanticUsageIndex::referenceUsages() const noexcept {
    return referenceUsages_;
}

std::span<const SctVariableUsage> SctSemanticUsageIndex::variableUsages() const noexcept {
    return variableUsages_;
}

std::span<const SctUnresolvedReferenceUsage>
SctSemanticUsageIndex::unresolvedReferences() const noexcept {
    return unresolvedReferences_;
}

std::span<const SctOpaqueParameterUsage>
SctSemanticUsageIndex::opaqueParameters() const noexcept {
    return opaqueParameters_;
}

std::span<const SctOpaqueExpressionUsage>
SctSemanticUsageIndex::opaqueExpressions() const noexcept {
    return opaqueExpressions_;
}

std::vector<SctOpcodeUsage> SctSemanticUsageIndex::usagesForOpcode(
    const std::uint16_t opcode) const {
    std::vector<SctOpcodeUsage> result;
    for (const auto& usage : opcodeUsages_) {
        if (usage.opcode == opcode) result.push_back(usage);
    }
    return result;
}

std::vector<SctReferenceUsage> SctSemanticUsageIndex::outboundReferences(
    const spice::sct::SctInstructionId source) const {
    std::vector<SctReferenceUsage> result;
    for (const auto& usage : referenceUsages_) {
        if (usage.source.instruction == source) result.push_back(usage);
    }
    return result;
}

std::vector<SctReferenceUsage> SctSemanticUsageIndex::inboundReferences(
    const spice::sct::SctDocumentReferenceTarget& target) const {
    std::vector<SctReferenceUsage> result;
    for (const auto& usage : referenceUsages_) {
        if (usage.target == target) result.push_back(usage);
    }
    return result;
}

std::vector<SctVariableUsage> SctSemanticUsageIndex::usagesForVariable(
    const SctVariableIdentity variable) const {
    std::vector<SctVariableUsage> result;
    for (const auto& usage : variableUsages_) {
        if (usage.variable == variable) result.push_back(usage);
    }
    return result;
}

void SctSemanticUsageIndex::recordExpression(
    const spice::sct::SctInstructionId instruction,
    SctExpressionOwner owner,
    const spice::sct::SctCanonicalExpression& expression) {
    if (const auto* opaque = std::get_if<spice::sct::SctOpaqueExpression>(
            &expression.root)) {
        opaqueExpressions_.push_back({
            SctExpressionSite{ instruction, std::move(owner), {} },
            opaque->words.size(),
        });
        return;
    }
    std::vector<std::uint32_t> childPath;
    recordExpressionNode(instruction, owner,
        std::get<spice::sct::SctCanonicalExpressionNode>(expression.root), childPath);
}

void SctSemanticUsageIndex::recordExpressionNode(
    const spice::sct::SctInstructionId instruction,
    const SctExpressionOwner& owner,
    const spice::sct::SctCanonicalExpressionNode& node,
    std::vector<std::uint32_t>& childPath) {
    if (const auto kind = variableKind(node.kind); kind.has_value()) {
        variableUsages_.push_back({
            SctVariableIdentity{ *kind, node.encodingCode & 0x00ffffffu },
            SctExpressionSite{ instruction, owner, childPath },
        });
    }
    for (std::size_t childOrdinal = 0; childOrdinal < node.children.size(); ++childOrdinal) {
        childPath.push_back(static_cast<std::uint32_t>(childOrdinal));
        recordExpressionNode(instruction, owner, node.children[childOrdinal], childPath);
        childPath.pop_back();
    }
}

void SctSemanticUsageIndex::recordParameter(
    const spice::sct::SctInstructionId instruction,
    const spice::sct::SctDocumentParameter& parameter,
    const std::optional<std::uint32_t> repeatedGroupOrdinal) {
    const SctParameterSite site{
        instruction,
        spice::sct::SctParameterAddress{ parameter.schemaIndex, repeatedGroupOrdinal },
    };
    std::visit([this, &site](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>) {
            referenceUsages_.push_back({ site, spice::sct::SctDocumentReferenceTarget{ value.target } });
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>) {
            referenceUsages_.push_back({ site, spice::sct::SctDocumentReferenceTarget{ value.target } });
        } else if constexpr (std::is_same_v<T, spice::sct::SctFooterEntryReference>) {
            referenceUsages_.push_back({ site, spice::sct::SctDocumentReferenceTarget{ value.target } });
        } else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>) {
            recordExpression(site.instruction, site.parameter, value);
        } else if constexpr (std::is_same_v<T, spice::sct::SctUnresolvedReferenceValue>) {
            unresolvedReferences_.push_back({ site, value.expectedTarget, value.encodedWords.size() });
        } else if constexpr (std::is_same_v<T, spice::sct::SctOpaqueParameterValue>) {
            opaqueParameters_.push_back({ site, value.words.size() });
        }
    }, parameter.value);
}

}  // namespace salsa::core
