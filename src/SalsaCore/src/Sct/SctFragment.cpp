#include "SalsaCore/Sct/SctFragment.h"

#include "SalsaCore/Persistence/AtomicFile.h"
#include "SalsaCore/Persistence/SctScriptPatch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] Diagnostic fragmentError(std::string message,
    const DiagnosticCode code = DiagnosticCode::InvalidSctFragment,
    std::optional<std::filesystem::path> path = std::nullopt) {
    return {DiagnosticSeverity::Error, code, std::move(message), std::move(path)};
}

[[nodiscard]] bool isContiguous(const std::span<const spice::sct::SctInstructionId> order,
    const std::span<const spice::sct::SctInstructionId> selection) {
    if (selection.empty()) return false;
    const auto first = std::ranges::find(order, selection.front());
    if (first == order.end() || static_cast<std::size_t>(order.end() - first) < selection.size())
        return false;
    return std::ranges::equal(selection,
        std::span<const spice::sct::SctInstructionId>{first, selection.size()});
}

[[nodiscard]] bool isContiguous(const std::span<const spice::sct::SctSectionId> order,
    const std::span<const spice::sct::SctSectionId> selection) {
    if (selection.empty()) return false;
    const auto first = std::ranges::find(order, selection.front());
    if (first == order.end() || static_cast<std::size_t>(order.end() - first) < selection.size())
        return false;
    return std::ranges::equal(selection,
        std::span<const spice::sct::SctSectionId>{first, selection.size()});
}

[[nodiscard]] spice::sct::SctExpectedReferenceTarget expectedTarget(
    const SctWorkingState& state,
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([&](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return spice::sct::SctExpectedReferenceTarget{
                spice::sct::SctReferenceTargetStorage::Instruction, std::nullopt};
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return spice::sct::SctExpectedReferenceTarget{
                spice::sct::SctReferenceTargetStorage::IndexedString,
                state.stringKind(id)};
        else {
            const auto* entry = state.footerEntry(id);
            return spice::sct::SctExpectedReferenceTarget{
                spice::sct::SctReferenceTargetStorage::FooterEntry,
                entry ? std::optional{entry->kind} : std::nullopt};
        }
    }, target);
}

[[nodiscard]] std::optional<std::string> targetName(
    const SctWorkingState& state,
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([&](const auto id) -> std::optional<std::string> {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>) {
            const auto placement = state.placement(id);
            const auto* section = placement ? state.section(placement->section) : nullptr;
            if (section) return section->nameBytes;
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            const auto name = state.stringSectionName(id);
            if (name) return std::string(*name);
        }
        return std::nullopt;
    }, target);
}

template<typename Fn>
void visitParameters(spice::sct::SctDocumentInstruction& instruction, Fn&& visitor) {
    for (auto& parameter : instruction.fixedParameters)
        visitor(spice::sct::SctParameterSite{instruction.id,
            {parameter.schemaIndex, std::nullopt}}, parameter.value);
    for (std::uint32_t ordinal = 0;
        ordinal < instruction.repeatedParameterGroups.size(); ++ordinal) {
        for (auto& parameter : instruction.repeatedParameterGroups[ordinal].parameters)
            visitor(spice::sct::SctParameterSite{instruction.id,
                {parameter.schemaIndex, ordinal}}, parameter.value);
    }
}

template<typename Fn>
void visitParameters(const spice::sct::SctDocumentInstruction& instruction, Fn&& visitor) {
    for (const auto& parameter : instruction.fixedParameters)
        visitor(spice::sct::SctParameterSite{instruction.id,
            {parameter.schemaIndex, std::nullopt}}, parameter.value);
    for (std::uint32_t ordinal = 0;
        ordinal < instruction.repeatedParameterGroups.size(); ++ordinal) {
        for (const auto& parameter : instruction.repeatedParameterGroups[ordinal].parameters)
            visitor(spice::sct::SctParameterSite{instruction.id,
                {parameter.schemaIndex, ordinal}}, parameter.value);
    }
}

[[nodiscard]] std::optional<spice::sct::SctDocumentReferenceTarget> referenceTarget(
    const spice::sct::SctDocumentParameterValue& value) {
    return std::visit([](const auto& typed)
        -> std::optional<spice::sct::SctDocumentReferenceTarget> {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>
            || std::is_same_v<T, spice::sct::SctStringReference>
            || std::is_same_v<T, spice::sct::SctFooterEntryReference>)
            return typed.target;
        return std::nullopt;
    }, value);
}

[[nodiscard]] bool containsTarget(
    const std::unordered_set<spice::sct::SctInstructionId>& instructions,
    const std::unordered_set<spice::sct::SctStringId>& strings,
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([&](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return instructions.contains(id);
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return strings.contains(id);
        return false;
    }, target);
}

void collectDependencies(SctSemanticFragment& fragment, const SctWorkingState& state) {
    std::unordered_set<spice::sct::SctInstructionId> instructions;
    std::unordered_set<spice::sct::SctStringId> strings;
    for (const auto& instruction : fragment.instructions) instructions.insert(instruction.id);
    for (const auto& section : fragment.sections) {
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content))
            for (const auto& instruction : script->instructions)
                instructions.insert(instruction.id);
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                &section.content))
            strings.insert(text->string.id);
    }
    const auto inspect = [&](const spice::sct::SctDocumentInstruction& instruction) {
        visitParameters(instruction, [&](const auto& site, const auto& value) {
            const auto target = referenceTarget(value);
            if (!target || containsTarget(instructions, strings, *target)) return;
            fragment.dependencies.push_back({site, *target,
                expectedTarget(state, *target), targetName(state, *target)});
        });
    };
    for (const auto& instruction : fragment.instructions) inspect(instruction);
    for (const auto& section : fragment.sections)
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content))
            for (const auto& instruction : script->instructions) inspect(instruction);
}

[[nodiscard]] bool dependenciesAreComplete(const SctSemanticFragment& fragment) {
    std::unordered_set<spice::sct::SctInstructionId> instructions;
    std::unordered_set<spice::sct::SctStringId> strings;
    for (const auto& instruction : fragment.instructions)
        if (!instructions.insert(instruction.id).second) return false;
    for (const auto& section : fragment.sections) {
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content))
            for (const auto& instruction : script->instructions)
                if (!instructions.insert(instruction.id).second) return false;
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                &section.content))
            if (!strings.insert(text->string.id).second) return false;
    }
    std::vector<bool> matched(fragment.dependencies.size(), false);
    const auto inspect = [&](const spice::sct::SctDocumentInstruction& instruction) {
        bool valid = true;
        visitParameters(instruction, [&](const auto& site, const auto& value) {
            const auto target = referenceTarget(value);
            if (!target) return;
            const bool internal = containsTarget(instructions, strings, *target);
            std::size_t matches = 0;
            for (std::size_t index = 0; index < fragment.dependencies.size(); ++index) {
                const auto& dependency = fragment.dependencies[index];
                if (dependency.sourceSite == site && dependency.target == *target) {
                    ++matches;
                    matched[index] = true;
                }
            }
            if ((internal && matches != 0u) || (!internal && matches != 1u))
                valid = false;
        });
        return valid;
    };
    for (const auto& instruction : fragment.instructions)
        if (!inspect(instruction)) return false;
    for (const auto& section : fragment.sections)
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content))
            for (const auto& instruction : script->instructions)
                if (!inspect(instruction)) return false;
    return std::ranges::all_of(matched, std::identity{});
}

[[nodiscard]] bool armTouches(const SctAuthoredArm& arm,
    const std::unordered_set<spice::sct::SctInstructionId>& selected) {
    if (selected.contains(arm.controller.instruction)) return true;
    return std::ranges::any_of(arm.members,
            [&](const auto id) { return selected.contains(id); })
        || std::ranges::any_of(arm.managedScaffolding,
            [&](const auto id) { return selected.contains(id); });
}

[[nodiscard]] bool armContained(const SctAuthoredArm& arm,
    const std::unordered_set<spice::sct::SctInstructionId>& selected) {
    return selected.contains(arm.controller.instruction)
        && (!arm.expectedJoin || selected.contains(*arm.expectedJoin))
        && std::ranges::all_of(arm.members,
            [&](const auto id) { return selected.contains(id); })
        && std::ranges::all_of(arm.managedScaffolding,
            [&](const auto id) { return selected.contains(id); });
}

[[nodiscard]] Result<void> collectArms(const SctStructuredAuthoringState& authoring,
    const std::unordered_set<spice::sct::SctInstructionId>& selected,
    SctSemanticFragment& fragment) {
    for (const auto& arm : authoring.arms()) {
        if (!armTouches(arm, selected)) continue;
        if (!armContained(arm, selected))
            return Result<void>::failure(fragmentError(
                "The selection cuts through a SALSA-authored control-flow arm."));
        fragment.authoredArms.push_back(arm);
    }
    return Result<void>::success();
}

[[nodiscard]] std::string encodeBase64(const std::span<const std::byte> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2u) / 3u) * 4u);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3u) {
        const auto first = std::to_integer<unsigned int>(bytes[offset]);
        const auto second = offset + 1u < bytes.size()
            ? std::to_integer<unsigned int>(bytes[offset + 1u]) : 0u;
        const auto third = offset + 2u < bytes.size()
            ? std::to_integer<unsigned int>(bytes[offset + 2u]) : 0u;
        const auto value = (first << 16u) | (second << 8u) | third;
        result.push_back(alphabet[(value >> 18u) & 63u]);
        result.push_back(alphabet[(value >> 12u) & 63u]);
        result.push_back(offset + 1u < bytes.size() ? alphabet[(value >> 6u) & 63u] : '=');
        result.push_back(offset + 2u < bytes.size() ? alphabet[value & 63u] : '=');
    }
    return result;
}

[[nodiscard]] std::optional<unsigned int> base64Value(const char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26u;
    if (value >= '0' && value <= '9') return value - '0' + 52u;
    if (value == '+') return 62u;
    if (value == '/') return 63u;
    return std::nullopt;
}

[[nodiscard]] std::vector<std::byte> decodeBase64(const std::string_view text) {
    if (text.size() % 4u != 0u) throw std::runtime_error("base64 length is invalid");
    std::vector<std::byte> result;
    result.reserve((text.size() / 4u) * 3u);
    for (std::size_t offset = 0; offset < text.size(); offset += 4u) {
        const auto first = base64Value(text[offset]);
        const auto second = base64Value(text[offset + 1u]);
        if (!first || !second) throw std::runtime_error("base64 character is invalid");
        const bool thirdPadding = text[offset + 2u] == '=';
        const bool fourthPadding = text[offset + 3u] == '=';
        const auto third = thirdPadding ? std::optional<unsigned int>{0u}
            : base64Value(text[offset + 2u]);
        const auto fourth = fourthPadding ? std::optional<unsigned int>{0u}
            : base64Value(text[offset + 3u]);
        if (!third || !fourth || (thirdPadding && !fourthPadding)
            || (offset + 4u != text.size() && (thirdPadding || fourthPadding)))
            throw std::runtime_error("base64 padding is invalid");
        const auto value = (*first << 18u) | (*second << 12u)
            | (*third << 6u) | *fourth;
        result.push_back(static_cast<std::byte>((value >> 16u) & 0xffu));
        if (!thirdPadding) result.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
        if (!fourthPadding) result.push_back(static_cast<std::byte>(value & 0xffu));
    }
    return result;
}

[[nodiscard]] Json encodeReferenceTarget(
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([](const auto id) -> Json {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return Json{{"kind", "instruction"}, {"id", id.value()}};
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return Json{{"kind", "string"}, {"id", id.value()}};
        return Json{{"kind", "footer"}, {"id", id.value()}};
    }, target);
}

[[nodiscard]] spice::sct::SctDocumentReferenceTarget decodeReferenceTarget(
    const Json& value) {
    const auto kind = value.at("kind").get<std::string>();
    const auto id = value.at("id").get<std::uint64_t>();
    if (id == 0u) throw std::runtime_error("reference ID zero is invalid");
    if (kind == "instruction") return spice::sct::SctInstructionId{id};
    if (kind == "string") return spice::sct::SctStringId{id};
    if (kind == "footer") return spice::sct::SctFooterEntryId{id};
    throw std::runtime_error("reference kind is invalid");
}

[[nodiscard]] Json encodeSite(const spice::sct::SctParameterSite& site) {
    return Json{{"instruction", site.instruction.value()},
        {"schemaIndex", site.parameter.schemaIndex},
        {"repeatedGroupOrdinal", site.parameter.repeatedGroupOrdinal
            ? Json(*site.parameter.repeatedGroupOrdinal) : Json(nullptr)}};
}

[[nodiscard]] spice::sct::SctParameterSite decodeSite(const Json& value) {
    spice::sct::SctParameterSite result;
    result.instruction = spice::sct::SctInstructionId{
        value.at("instruction").get<std::uint64_t>()};
    result.parameter.schemaIndex = value.at("schemaIndex").get<std::uint32_t>();
    if (!value.at("repeatedGroupOrdinal").is_null())
        result.parameter.repeatedGroupOrdinal =
            value.at("repeatedGroupOrdinal").get<std::uint32_t>();
    if (!result.instruction) throw std::runtime_error("parameter site ID zero is invalid");
    return result;
}

[[nodiscard]] Json encodeByteString(const std::optional<std::string>& value) {
    if (!value) return nullptr;
    Json result = Json::array();
    for (const unsigned char byte : *value) result.push_back(byte);
    return result;
}

[[nodiscard]] std::optional<std::string> decodeByteString(const Json& value) {
    if (value.is_null()) return std::nullopt;
    if (!value.is_array()) throw std::runtime_error("byte string is not an array");
    std::string result;
    for (const auto& item : value) {
        const auto byte = item.get<std::uint32_t>();
        if (byte > 255u) throw std::runtime_error("byte is outside its range");
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

[[nodiscard]] std::filesystem::path snippetPath(
    const std::filesystem::path& root, const std::string_view name) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(name.size() * 2u + 13u);
    for (const unsigned char value : name) {
        encoded.push_back(digits[value >> 4u]);
        encoded.push_back(digits[value & 0x0fu]);
    }
    encoded += ".snippet.json";
    return root / std::filesystem::path(encoded);
}

[[nodiscard]] Result<std::vector<std::byte>> readFile(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Result<std::vector<std::byte>>::failure(fragmentError(
        "The snippet could not be opened.", DiagnosticCode::PersistenceReadFailed, path));
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > SctFragmentCodec::MaximumBytes)
        return Result<std::vector<std::byte>>::failure(fragmentError(
            "The snippet exceeds the 64 MiB limit.", DiagnosticCode::SctFragmentTooLarge, path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!input) return Result<std::vector<std::byte>>::failure(fragmentError(
        "The snippet could not be read.", DiagnosticCode::PersistenceReadFailed, path));
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

}  // namespace

Result<SctSemanticFragment> SctFragmentService::captureInstructions(
    const SctWorkingState& state, const SctStructuredAuthoringState& authoring,
    std::string sourceAssetIdentity,
    const std::span<const spice::sct::SctInstructionId> instructions) {
    if (instructions.empty())
        return Result<SctSemanticFragment>::failure(fragmentError(
            "An instruction fragment requires at least one instruction."));
    const auto firstPlacement = state.placement(instructions.front());
    if (!firstPlacement || !isContiguous(state.instructionOrder(firstPlacement->section), instructions))
        return Result<SctSemanticFragment>::failure(fragmentError(
            "Instruction fragments must be one contiguous physical range in one section."));
    SctSemanticFragment fragment;
    fragment.kind = SctFragmentKind::InstructionRange;
    fragment.sourceAssetIdentity = std::move(sourceAssetIdentity);
    std::unordered_set<spice::sct::SctInstructionId> selected;
    for (const auto id : instructions) {
        const auto* instruction = state.instruction(id);
        const auto placement = state.placement(id);
        if (!instruction || !placement || placement->section != firstPlacement->section
            || !state.opaqueAttachments(id).empty())
            return Result<SctSemanticFragment>::failure(fragmentError(
                "The instruction selection contains unavailable or preservation-constrained content."));
        fragment.instructions.push_back(*instruction);
        selected.insert(id);
    }
    auto arms = collectArms(authoring, selected, fragment);
    if (!arms) return Result<SctSemanticFragment>::failure(arms.diagnostics());
    for (auto annotation : authoring.annotations()) {
        if (annotation.target.kind != SctAuthoringTargetKind::Instruction
            || !selected.contains(spice::sct::SctInstructionId(annotation.target.id))) continue;
        annotation.bookmarkLabel.reset();
        if (annotation.note || annotation.colorRgb)
            fragment.annotations.push_back(std::move(annotation));
    }
    collectDependencies(fragment, state);
    return Result<SctSemanticFragment>::success(std::move(fragment));
}

Result<SctSemanticFragment> SctFragmentService::captureSections(
    const SctWorkingState& state, const SctStructuredAuthoringState& authoring,
    std::string sourceAssetIdentity,
    const std::span<const spice::sct::SctSectionId> sections) {
    if (!isContiguous(state.sectionOrder(), sections))
        return Result<SctSemanticFragment>::failure(fragmentError(
            "Section fragments must be one contiguous physical range."));
    SctSemanticFragment fragment;
    fragment.kind = SctFragmentKind::SectionRange;
    fragment.sourceAssetIdentity = std::move(sourceAssetIdentity);
    std::unordered_set<spice::sct::SctInstructionId> selected;
    std::unordered_set<spice::sct::SctStringId> selectedStrings;
    std::unordered_set<spice::sct::SctSectionId> selectedSections(
        sections.begin(), sections.end());
    for (const auto id : sections) {
        const auto* section = state.section(id);
        if (!section || std::holds_alternative<spice::sct::SctOpaqueSectionContent>(
                section->content)
            || !state.opaqueAttachments(spice::sct::SctOpaqueAnchor{id}).empty())
            return Result<SctSemanticFragment>::failure(fragmentError(
                "The section selection contains opaque or preservation-constrained content."));
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section->content)) {
            for (const auto& instruction : script->instructions) {
                if (!state.opaqueAttachments(instruction.id).empty())
                    return Result<SctSemanticFragment>::failure(fragmentError(
                        "A selected section contains preservation-constrained instructions."));
                selected.insert(instruction.id);
            }
        }
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                &section->content);
            text && !state.opaqueAttachments(
                spice::sct::SctOpaqueAnchor{text->string.id}).empty())
            return Result<SctSemanticFragment>::failure(fragmentError(
                "A selected indexed string has preservation-constrained data."));
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                &section->content)) selectedStrings.insert(text->string.id);
        fragment.sections.push_back(*section);
    }
    auto arms = collectArms(authoring, selected, fragment);
    if (!arms) return Result<SctSemanticFragment>::failure(arms.diagnostics());
    for (auto annotation : authoring.annotations()) {
        const bool included = (annotation.target.kind == SctAuthoringTargetKind::Section
                && selectedSections.contains(spice::sct::SctSectionId(annotation.target.id)))
            || (annotation.target.kind == SctAuthoringTargetKind::Instruction
                && selected.contains(spice::sct::SctInstructionId(annotation.target.id)))
            || (annotation.target.kind == SctAuthoringTargetKind::String
                && selectedStrings.contains(spice::sct::SctStringId(annotation.target.id)));
        if (!included) continue;
        annotation.bookmarkLabel.reset();
        if (annotation.note || annotation.colorRgb)
            fragment.annotations.push_back(std::move(annotation));
    }
    for (auto folder : authoring.folders()) {
        if (!std::ranges::all_of(folder.sections, [&](const auto section) {
                return selectedSections.contains(section);
            })) continue;
        folder.bookmarkLabel.reset();
        fragment.folders.push_back(std::move(folder));
    }
    collectDependencies(fragment, state);
    return Result<SctSemanticFragment>::success(std::move(fragment));
}

std::vector<std::string> SctFragmentService::suggestSectionNames(
    const SctWorkingState& state, const SctSemanticFragment& fragment) {
    std::unordered_set<std::string> used;
    for (const auto id : state.sectionOrder())
        if (const auto* section = state.section(id)) used.insert(section->nameBytes);
    std::vector<std::string> result;
    for (const auto& section : fragment.sections) {
        auto candidate = section.nameBytes;
        if (used.contains(candidate)) {
            const auto suffix = [](const std::size_t ordinal) {
                return ordinal == 1u ? std::string("_COPY")
                    : std::string("_COPY") + std::to_string(ordinal);
            };
            for (std::size_t ordinal = 1u;; ++ordinal) {
                auto base = section.nameBytes;
                const auto addition = suffix(ordinal);
                if (base.size() + addition.size() > 16u)
                    base.resize(16u - std::min<std::size_t>(16u, addition.size()));
                candidate = base + addition;
                if (!used.contains(candidate)) break;
            }
        }
        used.insert(candidate);
        result.push_back(std::move(candidate));
    }
    return result;
}

Result<SctFragmentPastePlan> SctFragmentService::planPaste(
    const SctWorkingState& state, const SctStructuredAuthoringState& authoring,
    const std::string_view destinationAssetIdentity,
    const SctSemanticFragment& fragment,
    const SctFragmentPasteDestination& destination) {
    SctFragmentPastePlan result;
    std::unordered_map<spice::sct::SctInstructionId, spice::sct::SctInstructionId>
        instructionIds;
    std::unordered_map<spice::sct::SctStringId, spice::sct::SctStringId> stringIds;
    std::unordered_map<spice::sct::SctSectionId, spice::sct::SctSectionId> sectionIds;
    auto nextInstruction = state.nextInstructionIdValue();
    auto nextString = state.nextStringIdValue();
    auto nextSection = state.nextSectionIdValue();
    const auto registerInstruction = [&](const auto& instruction) {
        instructionIds.emplace(instruction.id,
            spice::sct::SctInstructionId{nextInstruction++});
    };
    for (const auto& instruction : fragment.instructions) registerInstruction(instruction);
    for (const auto& section : fragment.sections) {
        sectionIds.emplace(section.id, spice::sct::SctSectionId{nextSection++});
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &section.content))
            for (const auto& instruction : script->instructions) registerInstruction(instruction);
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                &section.content))
            stringIds.emplace(text->string.id, spice::sct::SctStringId{nextString++});
    }
    const bool sameDocument = fragment.sourceAssetIdentity == destinationAssetIdentity;
    const auto remapInstruction = [&](spice::sct::SctDocumentInstruction instruction) {
        const auto oldId = instruction.id;
        instruction.id = instructionIds.at(oldId);
        visitParameters(instruction, [&](const auto& site, auto& value) {
            const auto oldTarget = referenceTarget(value);
            if (!oldTarget) return;
            const auto internal = std::visit([&](const auto id)
                -> std::optional<spice::sct::SctDocumentReferenceTarget> {
                using T = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>) {
                    const auto found = instructionIds.find(id);
                    if (found != instructionIds.end()) return found->second;
                } else if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
                    const auto found = stringIds.find(id);
                    if (found != stringIds.end()) return found->second;
                }
                return std::nullopt;
            }, *oldTarget);
            if (internal) {
                value = std::visit([](const auto id)
                    -> spice::sct::SctDocumentParameterValue {
                    using T = std::decay_t<decltype(id)>;
                    if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
                        return spice::sct::SctInstructionReference{id};
                    else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
                        return spice::sct::SctStringReference{id};
                    else return spice::sct::SctFooterEntryReference{id};
                }, *internal);
                return;
            }
            if (sameDocument) return;
            const auto dependency = std::ranges::find_if(fragment.dependencies,
                [&](const auto& candidate) {
                    return candidate.sourceSite.instruction == oldId
                        && candidate.sourceSite.parameter == site.parameter
                        && candidate.target == *oldTarget;
                });
            const auto expected = dependency != fragment.dependencies.end()
                ? dependency->expectedTarget
                : spice::sct::SctExpectedReferenceTarget{};
            value = spice::sct::SctUnresolvedReferenceValue{expected, {0u}};
            const SctUnboundReferenceOrigin origin{
                site, fragment.sourceAssetIdentity, *oldTarget,
                dependency != fragment.dependencies.end()
                    ? dependency->targetNameBytes : std::nullopt};
            result.authoring.unboundReferences.push_back(
                {site, std::nullopt, origin});
            ++result.unboundReferenceCount;
        });
        return instruction;
    };

    if (fragment.kind == SctFragmentKind::InstructionRange) {
        if (!destination.instructionAfter
            || state.instruction(*destination.instructionAfter) == nullptr)
            return Result<SctFragmentPastePlan>::failure(fragmentError(
                "Instruction paste requires a valid physical instruction anchor."));
        auto anchor = *destination.instructionAfter;
        for (const auto& source : fragment.instructions) {
            auto instruction = remapInstruction(source);
            result.document.operations.push_back(
                SctInsertInstructionAfterOperation{anchor, instruction});
            anchor = instruction.id;
            result.insertedSelection.push_back({
                SctNavigationKind::Instruction, instruction.id.value()});
        }
    } else {
        if (destination.sectionNames.size() != fragment.sections.size())
            return Result<SctFragmentPastePlan>::failure(fragmentError(
                "Section paste requires one reviewed destination name per section."));
        std::unordered_set<std::string> usedNames;
        for (const auto id : state.sectionOrder())
            if (const auto* section = state.section(id))
                usedNames.insert(section->nameBytes);
        for (const auto& name : destination.sectionNames) {
            const bool valid = !name.empty() && name.size() <= 16u
                && std::ranges::all_of(name, [](const unsigned char value) {
                    return (value >= 'A' && value <= 'Z')
                        || (value >= 'a' && value <= 'z')
                        || (value >= '0' && value <= '9') || value == '_';
                });
            if (!valid || !usedNames.insert(name).second)
                return Result<SctFragmentPastePlan>::failure(fragmentError(
                    "Pasted section names must be unique and match [A-Za-z0-9_]{1,16}."));
        }
        auto sectionAnchor = destination.sectionAfter;
        for (std::size_t ordinal = 0; ordinal < fragment.sections.size(); ++ordinal) {
            auto section = fragment.sections[ordinal];
            section.id = sectionIds.at(section.id);
            section.nameBytes = destination.sectionNames[ordinal];
            if (auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                    &section.content))
                for (auto& instruction : script->instructions)
                    instruction = remapInstruction(std::move(instruction));
            if (auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                    &section.content))
                text->string.id = stringIds.at(text->string.id);
            result.document.operations.push_back(
                SctInsertSectionAfterOperation{sectionAnchor, section});
            sectionAnchor = section.id;
            result.insertedSelection.push_back({
                SctNavigationKind::Section, section.id.value()});
        }
    }

    auto nextArm = authoring.nextId().value;
    for (auto arm : fragment.authoredArms) {
        const auto oldId = arm.id;
        arm.id = SctAuthoredArmId{nextArm++};
        arm.controller.section = sectionIds.contains(arm.controller.section)
            ? sectionIds.at(arm.controller.section) : arm.controller.section;
        arm.controller.instruction = instructionIds.at(arm.controller.instruction);
        if (arm.expectedJoin && instructionIds.contains(*arm.expectedJoin))
            arm.expectedJoin = instructionIds.at(*arm.expectedJoin);
        for (auto& id : arm.members) id = instructionIds.at(id);
        for (auto& id : arm.managedScaffolding) id = instructionIds.at(id);
        result.authoring.operations.push_back(
            {arm.id, std::nullopt, std::move(arm)});
        (void)oldId;
    }
    for (auto annotation : fragment.annotations) {
        switch (annotation.target.kind) {
        case SctAuthoringTargetKind::Section:
            if (!sectionIds.contains(spice::sct::SctSectionId(annotation.target.id))) continue;
            annotation.target.id = sectionIds.at(
                spice::sct::SctSectionId(annotation.target.id)).value();
            break;
        case SctAuthoringTargetKind::Instruction:
            if (!instructionIds.contains(spice::sct::SctInstructionId(annotation.target.id))) continue;
            annotation.target.id = instructionIds.at(
                spice::sct::SctInstructionId(annotation.target.id)).value();
            break;
        case SctAuthoringTargetKind::String:
            if (!stringIds.contains(spice::sct::SctStringId(annotation.target.id))) continue;
            annotation.target.id = stringIds.at(
                spice::sct::SctStringId(annotation.target.id)).value();
            break;
        default:
            continue;
        }
        annotation.bookmarkLabel.reset();
        result.authoring.annotations.push_back(
            {annotation.target, std::nullopt, std::move(annotation)});
    }
    std::unordered_map<std::uint64_t, std::uint64_t> folderIds;
    auto nextFolder = authoring.nextFolderId().value;
    for (const auto& folder : fragment.folders)
        folderIds.emplace(folder.id.value, nextFolder++);
    for (auto folder : fragment.folders) {
        folder.id = SctSectionFolderId{folderIds.at(folder.id.value)};
        folder.parent = folder.parent && folderIds.contains(folder.parent->value)
            ? std::optional{SctSectionFolderId{folderIds.at(folder.parent->value)}}
            : std::nullopt;
        for (auto& section : folder.sections) section = sectionIds.at(section);
        folder.bookmarkLabel.reset();
        result.authoring.folders.push_back(
            {folder.id, std::nullopt, std::move(folder)});
    }
    return Result<SctFragmentPastePlan>::success(std::move(result));
}

Result<std::vector<std::byte>> SctFragmentCodec::serialize(
    const SctSemanticFragment& fragment) {
    try {
        if (fragment.sourceAssetIdentity.empty()
            || (fragment.kind == SctFragmentKind::InstructionRange
                && (fragment.instructions.empty() || !fragment.sections.empty()))
            || (fragment.kind == SctFragmentKind::SectionRange
                && (fragment.sections.empty() || !fragment.instructions.empty()))
            || !dependenciesAreComplete(fragment))
            throw std::runtime_error("fragment semantic shape is invalid");
        SalsaScriptPatch payload;
        if (fragment.kind == SctFragmentKind::InstructionRange) {
            spice::sct::SctDocumentSection section;
            section.id = spice::sct::SctSectionId{1};
            section.nameBytes = "__FRAGMENT__";
            section.content = spice::sct::SctScriptSectionContent{fragment.instructions};
            payload.sections.push_back({std::nullopt, std::move(section)});
        } else {
            for (const auto& section : fragment.sections)
                payload.sections.push_back({std::nullopt, section});
        }
        for (const auto& arm : fragment.authoredArms)
            payload.authoredArms.push_back({std::nullopt, arm});
        for (const auto& annotation : fragment.annotations)
            payload.annotations.push_back({std::nullopt, annotation});
        for (const auto& folder : fragment.folders)
            payload.folders.push_back({std::nullopt, folder});
        auto encodedPayload = SalsaScriptPatchCodec::serialize(payload);
        if (!encodedPayload)
            return Result<std::vector<std::byte>>::failure(encodedPayload.diagnostics());
        Json dependencies = Json::array();
        for (const auto& dependency : fragment.dependencies) {
            dependencies.push_back({{"sourceSite", encodeSite(dependency.sourceSite)},
                {"target", encodeReferenceTarget(dependency.target)},
                {"expectedStorage", static_cast<std::uint32_t>(
                    dependency.expectedTarget.storage)},
                {"expectedTextKind", dependency.expectedTarget.textKind
                    ? Json(static_cast<std::uint32_t>(*dependency.expectedTarget.textKind))
                    : Json(nullptr)},
                {"targetNameBytes", encodeByteString(dependency.targetNameBytes)}});
        }
        Json document{{"formatId", FormatId}, {"schemaVersion", SchemaVersion},
            {"kind", fragment.kind == SctFragmentKind::InstructionRange
                ? "instructionRange" : "sectionRange"},
            {"sourceAssetIdentity", fragment.sourceAssetIdentity},
            {"payloadEncoding", "base64"},
            {"payload", encodeBase64(encodedPayload.value())},
            {"dependencies", std::move(dependencies)}};
        auto text = document.dump(2);
        text.push_back('\n');
        if (text.size() > MaximumBytes)
            return Result<std::vector<std::byte>>::failure(fragmentError(
                "The semantic fragment exceeds the 64 MiB limit.",
                DiagnosticCode::SctFragmentTooLarge));
        const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
        return Result<std::vector<std::byte>>::success({bytes.begin(), bytes.end()});
    } catch (const std::exception& error) {
        return Result<std::vector<std::byte>>::failure(fragmentError(
            std::string("The semantic fragment could not be serialized: ") + error.what()));
    }
}

Result<SctSemanticFragment> SctFragmentCodec::deserialize(
    const std::span<const std::byte> bytes) {
    if (bytes.size() > MaximumBytes)
        return Result<SctSemanticFragment>::failure(fragmentError(
            "The semantic fragment exceeds the 64 MiB limit.",
            DiagnosticCode::SctFragmentTooLarge));
    try {
        const auto document = Json::parse(
            reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data()) + bytes.size());
        static const std::array fields{"formatId", "schemaVersion", "kind",
            "sourceAssetIdentity", "payloadEncoding", "payload", "dependencies"};
        if (!document.is_object() || document.size() != fields.size()
            || !std::ranges::all_of(fields,
                [&](const auto field) { return document.contains(field); }))
            throw std::runtime_error("fragment has missing or unknown fields");
        if (document.at("formatId").get<std::string>() != FormatId)
            throw std::runtime_error("fragment format ID is invalid");
        if (document.at("schemaVersion").get<std::uint32_t>() != SchemaVersion)
            return Result<SctSemanticFragment>::failure(fragmentError(
                "The semantic fragment schema version is unsupported.",
                DiagnosticCode::UnsupportedSctFragmentSchema));
        if (document.at("payloadEncoding").get<std::string>() != "base64")
            throw std::runtime_error("fragment payload encoding is invalid");
        if (!document.at("dependencies").is_array())
            throw std::runtime_error("fragment dependencies are not an array");
        auto payloadBytes = decodeBase64(document.at("payload").get<std::string>());
        auto payload = SalsaScriptPatchCodec::deserialize(payloadBytes);
        if (!payload)
            return Result<SctSemanticFragment>::failure(payload.diagnostics());
        const auto& decodedPatch = payload.value();
        if (decodedPatch.sourceTextConvention || decodedPatch.allocatorState
            || decodedPatch.sectionOrder || !decodedPatch.scriptSections.empty()
            || !decodedPatch.textValues.empty() || decodedPatch.footerOrder
            || !decodedPatch.footerEntries.empty() || !decodedPatch.textRepairs.empty()
            || !decodedPatch.unboundReferences.empty() || !decodedPatch.aliases.empty())
            throw std::runtime_error("fragment payload contains unrelated patch data");
        SctSemanticFragment fragment;
        fragment.sourceAssetIdentity = document.at("sourceAssetIdentity").get<std::string>();
        if (fragment.sourceAssetIdentity.empty())
            throw std::runtime_error("fragment source asset identity is empty");
        const auto kind = document.at("kind").get<std::string>();
        if (kind == "instructionRange") {
            fragment.kind = SctFragmentKind::InstructionRange;
            if (decodedPatch.sections.size() != 1u
                || !decodedPatch.sections.front().after
                || decodedPatch.sections.front().before)
                throw std::runtime_error("instruction fragment payload shape is invalid");
            const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                &decodedPatch.sections.front().after->content);
            if (!script) throw std::runtime_error("instruction fragment is not a script");
            fragment.instructions = script->instructions;
        } else if (kind == "sectionRange") {
            fragment.kind = SctFragmentKind::SectionRange;
            for (const auto& section : decodedPatch.sections) {
                if (!section.after || section.before)
                    throw std::runtime_error("section fragment delta is invalid");
                fragment.sections.push_back(*section.after);
            }
        } else throw std::runtime_error("fragment kind is invalid");
        for (const auto& arm : decodedPatch.authoredArms) {
            if (!arm.after || arm.before)
                throw std::runtime_error("fragment arm delta is invalid");
            fragment.authoredArms.push_back(*arm.after);
        }
        for (const auto& annotation : decodedPatch.annotations) {
            if (!annotation.after || annotation.before || annotation.after->bookmarkLabel)
                throw std::runtime_error("fragment annotation delta is invalid");
            fragment.annotations.push_back(*annotation.after);
        }
        for (const auto& folder : decodedPatch.folders) {
            if (!folder.after || folder.before || folder.after->bookmarkLabel)
                throw std::runtime_error("fragment folder delta is invalid");
            fragment.folders.push_back(*folder.after);
        }
        for (const auto& encoded : document.at("dependencies")) {
            static const std::array dependencyFields{"sourceSite", "target",
                "expectedStorage", "expectedTextKind", "targetNameBytes"};
            if (!encoded.is_object() || encoded.size() != dependencyFields.size()
                || !std::ranges::all_of(dependencyFields,
                    [&](const auto field) { return encoded.contains(field); }))
                throw std::runtime_error("fragment dependency has missing or unknown fields");
            SctFragmentDependency dependency;
            dependency.sourceSite = decodeSite(encoded.at("sourceSite"));
            dependency.target = decodeReferenceTarget(encoded.at("target"));
            const auto storage = encoded.at("expectedStorage").get<std::uint32_t>();
            if (storage > static_cast<std::uint32_t>(
                    spice::sct::SctReferenceTargetStorage::FooterEntry))
                throw std::runtime_error("fragment dependency storage is invalid");
            dependency.expectedTarget.storage =
                static_cast<spice::sct::SctReferenceTargetStorage>(storage);
            if (!encoded.at("expectedTextKind").is_null()) {
                const auto kind = encoded.at("expectedTextKind").get<std::uint32_t>();
                if (kind > static_cast<std::uint32_t>(spice::sct::SctTextKind::SctString))
                    throw std::runtime_error("fragment dependency text kind is invalid");
                dependency.expectedTarget.textKind =
                    static_cast<spice::sct::SctTextKind>(kind);
            }
            const auto targetStorage = std::visit([](const auto id) {
                using T = std::decay_t<decltype(id)>;
                if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
                    return spice::sct::SctReferenceTargetStorage::Instruction;
                if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
                    return spice::sct::SctReferenceTargetStorage::IndexedString;
                return spice::sct::SctReferenceTargetStorage::FooterEntry;
            }, dependency.target);
            if (dependency.expectedTarget.storage != targetStorage)
                throw std::runtime_error(
                    "fragment dependency storage contradicts its target");
            dependency.targetNameBytes = decodeByteString(encoded.at("targetNameBytes"));
            fragment.dependencies.push_back(std::move(dependency));
        }
        if (!dependenciesAreComplete(fragment))
            throw std::runtime_error(
                "fragment dependencies do not match its external references");
        return Result<SctSemanticFragment>::success(std::move(fragment));
    } catch (const std::exception& error) {
        return Result<SctSemanticFragment>::failure(fragmentError(
            std::string("The semantic fragment is malformed: ") + error.what()));
    }
}

Result<std::vector<std::byte>> SctSnippetCodec::serialize(const SctSnippet& snippet) {
    if (snippet.name.empty() || snippet.name.size() > 120u)
        return Result<std::vector<std::byte>>::failure(fragmentError(
            "A snippet name must contain 1 to 120 bytes."));
    auto fragment = SctFragmentCodec::serialize(snippet.fragment);
    if (!fragment) return Result<std::vector<std::byte>>::failure(fragment.diagnostics());
    Json document{{"formatId", FormatId}, {"schemaVersion", SchemaVersion},
        {"name", snippet.name}, {"description", snippet.description},
        {"fragmentEncoding", "base64"}, {"fragment", encodeBase64(fragment.value())}};
    auto text = document.dump(2);
    text.push_back('\n');
    if (text.size() > SctFragmentCodec::MaximumBytes)
        return Result<std::vector<std::byte>>::failure(fragmentError(
            "The snippet exceeds the 64 MiB limit.",
            DiagnosticCode::SctFragmentTooLarge));
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return Result<std::vector<std::byte>>::success({bytes.begin(), bytes.end()});
}

Result<SctSnippet> SctSnippetCodec::deserialize(const std::span<const std::byte> bytes) {
    if (bytes.size() > SctFragmentCodec::MaximumBytes)
        return Result<SctSnippet>::failure(fragmentError(
            "The snippet exceeds the 64 MiB limit.",
            DiagnosticCode::SctFragmentTooLarge));
    try {
        const auto document = Json::parse(
            reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data()) + bytes.size());
        static const std::array fields{"formatId", "schemaVersion", "name",
            "description", "fragmentEncoding", "fragment"};
        if (!document.is_object() || document.size() != fields.size()
            || !std::ranges::all_of(fields,
                [&](const auto field) { return document.contains(field); }))
            throw std::runtime_error("snippet has missing or unknown fields");
        if (document.at("formatId").get<std::string>() != FormatId
            || document.at("fragmentEncoding").get<std::string>() != "base64")
            throw std::runtime_error("snippet header is unsupported");
        if (document.at("schemaVersion").get<std::uint32_t>() != SchemaVersion)
            return Result<SctSnippet>::failure(fragmentError(
                "The snippet schema version is unsupported.",
                DiagnosticCode::UnsupportedSctFragmentSchema));
        auto fragmentBytes = decodeBase64(document.at("fragment").get<std::string>());
        auto fragment = SctFragmentCodec::deserialize(fragmentBytes);
        if (!fragment) return Result<SctSnippet>::failure(fragment.diagnostics());
        SctSnippet result{document.at("name").get<std::string>(),
            document.at("description").get<std::string>(),
            std::move(fragment).takeValue()};
        if (result.name.empty() || result.name.size() > 120u)
            throw std::runtime_error("snippet name is invalid");
        return Result<SctSnippet>::success(std::move(result));
    } catch (const std::exception& error) {
        return Result<SctSnippet>::failure(fragmentError(
            std::string("The snippet is malformed: ") + error.what()));
    }
}

SctSnippetStore::SctSnippetStore(std::filesystem::path root)
    : root_(std::move(root)) {}

const std::filesystem::path& SctSnippetStore::root() const noexcept { return root_; }

Result<std::vector<SctSnippet>> SctSnippetStore::loadAll() const {
    std::error_code error;
    if (!std::filesystem::exists(root_, error))
        return Result<std::vector<SctSnippet>>::success({});
    if (error || !std::filesystem::is_directory(root_, error))
        return Result<std::vector<SctSnippet>>::failure(fragmentError(
            "The snippet directory is unavailable.",
            DiagnosticCode::PersistenceReadFailed, root_));
    std::vector<SctSnippet> result;
    for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
        if (error) break;
        if (!entry.is_regular_file() || !entry.path().filename().wstring().ends_with(
                L".snippet.json")) continue;
        auto bytes = readFile(entry.path());
        if (!bytes) return Result<std::vector<SctSnippet>>::failure(bytes.diagnostics());
        auto snippet = SctSnippetCodec::deserialize(bytes.value());
        if (!snippet) return Result<std::vector<SctSnippet>>::failure(snippet.diagnostics());
        result.push_back(std::move(snippet).takeValue());
    }
    if (error) return Result<std::vector<SctSnippet>>::failure(fragmentError(
        "The snippet directory could not be enumerated: " + error.message(),
        DiagnosticCode::PersistenceReadFailed, root_));
    std::ranges::sort(result, {}, &SctSnippet::name);
    return Result<std::vector<SctSnippet>>::success(std::move(result));
}

Result<void> SctSnippetStore::save(const SctSnippet& snippet) const {
    auto bytes = SctSnippetCodec::serialize(snippet);
    if (!bytes) return Result<void>::failure(bytes.diagnostics());
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) return Result<void>::failure(fragmentError(
        "The snippet directory could not be created: " + error.message(),
        DiagnosticCode::PersistenceWriteFailed, root_));
    return replaceFileAtomically(snippetPath(root_, snippet.name), bytes.value());
}

Result<void> SctSnippetStore::remove(const std::string_view name) const {
    std::error_code error;
    const auto path = snippetPath(root_, name);
    const bool removed = std::filesystem::remove(path, error);
    if (error || !removed) return Result<void>::failure(fragmentError(
        error ? "The snippet could not be removed: " + error.message()
              : "The snippet does not exist.",
        DiagnosticCode::PersistenceWriteFailed, path));
    return Result<void>::success();
}

}  // namespace salsa::core
