#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctSemanticUsageIndex.h"

#include <variant>
#include <type_traits>

namespace salsa::core {

using SctInspectionLocation = std::variant<
    SctNavigationTarget,
    SctParameterSite,
    SctExpressionSite>;

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
