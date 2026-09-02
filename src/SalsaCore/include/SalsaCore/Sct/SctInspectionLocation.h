#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SpiceSCT/SctDocumentAnalysis.h"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace salsa::core {

using SctInspectionLocation = std::variant<
    SctNavigationTarget,
    spice::sct::SctParameterSite,
    spice::sct::SctExpressionSite>;

[[nodiscard]] inline std::optional<SctInspectionLocation>
inspectionLocationForDiagnostic(
    const spice::sct::SctDiagnosticLocation& location) noexcept {
    return std::visit([](const auto& typed) -> std::optional<SctInspectionLocation> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctDocumentEntityId>) {
            return std::visit([](const auto& id) -> std::optional<SctInspectionLocation> {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, std::monostate>) return std::nullopt;
                else if constexpr (std::is_same_v<Id, spice::sct::SctSectionId>)
                    return SctNavigationTarget{SctNavigationKind::Section, id.value()};
                else if constexpr (std::is_same_v<Id, spice::sct::SctInstructionId>)
                    return SctNavigationTarget{SctNavigationKind::Instruction, id.value()};
                else if constexpr (std::is_same_v<Id, spice::sct::SctStringId>)
                    return SctNavigationTarget{SctNavigationKind::String, id.value()};
                else if constexpr (std::is_same_v<Id, spice::sct::SctFooterEntryId>)
                    return SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
                else return SctNavigationTarget{
                    SctNavigationKind::OpaqueAttachment, id.value()};
            }, typed);
        } else if constexpr (std::is_same_v<T, spice::sct::SctParameterSite>
            || std::is_same_v<T, spice::sct::SctExpressionSite>) {
            return SctInspectionLocation{typed};
        } else if constexpr (std::is_same_v<T, spice::sct::SctTextSite>) {
            return std::visit([](const auto& id) -> SctInspectionLocation {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, spice::sct::SctStringId>)
                    return SctNavigationTarget{SctNavigationKind::String, id.value()};
                else return SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
            }, typed.text);
        } else {
            return std::nullopt;
        }
    }, location);
}

[[nodiscard]] inline std::string formatSctParameterAddress(
    const spice::sct::SctParameterAddress& parameter) {
    auto result = "parameter " + std::to_string(parameter.schemaIndex);
    if (parameter.repeatedGroupOrdinal)
        result += " group " + std::to_string(*parameter.repeatedGroupOrdinal);
    return result;
}

[[nodiscard]] inline std::string formatSctExpressionOwner(
    const spice::sct::SctExpressionOwner& owner) {
    if (std::holds_alternative<spice::sct::SctScheduledExpressionSite>(owner))
        return "scheduled expression";
    return "expression in "
        + formatSctParameterAddress(std::get<spice::sct::SctParameterAddress>(owner));
}

[[nodiscard]] inline std::string appendSctExpressionPath(
    std::string value, const std::vector<std::uint32_t>& childPath) {
    for (const auto child : childPath) value += "/" + std::to_string(child);
    return value;
}

[[nodiscard]] inline std::string formatSctDiagnosticLocation(
    const spice::sct::SctDiagnosticLocation& location) {
    return std::visit([](const auto& typed) -> std::string {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctDocumentEntityId>) {
            return std::visit([](const auto& id) -> std::string {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, std::monostate>) return "document";
                else if constexpr (std::is_same_v<Id, spice::sct::SctSectionId>)
                    return "section " + std::to_string(id.value());
                else if constexpr (std::is_same_v<Id, spice::sct::SctInstructionId>)
                    return "instruction " + std::to_string(id.value());
                else if constexpr (std::is_same_v<Id, spice::sct::SctStringId>)
                    return "indexed string " + std::to_string(id.value());
                else if constexpr (std::is_same_v<Id, spice::sct::SctFooterEntryId>)
                    return "footer entry " + std::to_string(id.value());
                else return "opaque attachment " + std::to_string(id.value());
            }, typed);
        } else if constexpr (std::is_same_v<T, spice::sct::SctParameterSite>) {
            return "instruction " + std::to_string(typed.instruction.value()) + ", "
                + formatSctParameterAddress(typed.parameter);
        } else if constexpr (std::is_same_v<T, spice::sct::SctExpressionSite>) {
            return appendSctExpressionPath(
                "instruction " + std::to_string(typed.instruction.value()) + ", "
                    + formatSctExpressionOwner(typed.owner), typed.childPath);
        } else if constexpr (std::is_same_v<T, spice::sct::SctTextSite>) {
            const auto identity = std::visit([](const auto& id) -> std::string {
                using Id = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, spice::sct::SctStringId>)
                    return "indexed string " + std::to_string(id.value());
                else return "footer entry " + std::to_string(id.value());
            }, typed.text);
            const auto region = typed.region == spice::sct::SctTextRegion::Header
                ? "header" : "body";
            return identity + " " + region + " bytes ["
                + std::to_string(typed.utf8Range.offset) + ".."
                + std::to_string(typed.utf8Range.offset + typed.utf8Range.size) + ")";
        } else if constexpr (std::is_same_v<T, spice::sct::SctDraftParameterSite>) {
            return "draft opcode " + std::to_string(typed.opcode) + ", "
                + formatSctParameterAddress(typed.parameter);
        } else {
            auto result = typed.opcode
                ? "draft opcode " + std::to_string(*typed.opcode) + " "
                : std::string("draft ");
            if (typed.owner) result += formatSctExpressionOwner(*typed.owner);
            else result += "expression";
            return appendSctExpressionPath(std::move(result), typed.childPath);
        }
    }, location);
}

[[nodiscard]] inline SctNavigationTarget owningNavigationTarget(
    const SctInspectionLocation& location) noexcept {
    return std::visit([](const auto& typed) -> SctNavigationTarget {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, SctNavigationTarget>) {
            return typed;
        } else {
            return { SctNavigationKind::Instruction, typed.instruction.value() };
        }
    }, location);
}

[[nodiscard]] inline SctNavigationTarget navigationTargetForOpaqueAnchor(
    const spice::sct::SctOpaqueAnchor& anchor) noexcept {
    return std::visit([](const auto& typed) -> SctNavigationTarget {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctDocumentAnchor>) {
            return { SctNavigationKind::Document, 0 };
        } else if constexpr (std::is_same_v<T, spice::sct::SctSectionId>) {
            return { SctNavigationKind::Section, typed.value() };
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>) {
            return { SctNavigationKind::Instruction, typed.value() };
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            return { SctNavigationKind::String, typed.value() };
        } else {
            return { SctNavigationKind::FooterEntry, typed.value() };
        }
    }, anchor);
}

}  // namespace salsa::core
