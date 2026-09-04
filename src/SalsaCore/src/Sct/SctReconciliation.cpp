#include "SalsaCore/Sct/SctReconciliation.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/SctReconciliationDecisionStore.h"
#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentValidator.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>

namespace salsa::core {
namespace {

using namespace spice::sct;

struct IdMaps final {
    std::unordered_map<std::uint64_t, std::uint64_t> sections;
    std::unordered_map<std::uint64_t, std::uint64_t> instructions;
    std::unordered_map<std::uint64_t, std::uint64_t> strings;
    std::unordered_map<std::uint64_t, std::uint64_t> footers;
    std::unordered_map<std::uint64_t, std::uint64_t> opaque;
    std::unordered_map<std::uint64_t, std::uint64_t> arms;
};

struct EntityMatch final {
    SctReconciliationEntityId baseline;
    SctReconciliationEntityId incoming;
    SctIdentityMatchStatus status = SctIdentityMatchStatus::Strong;
    SctIdentityMatchProvenance provenance =
        SctIdentityMatchProvenance::Automatic;
    std::vector<SctIdentityEvidence> evidence;
    bool representable = true;
};

struct AssetPair final {
    const SctReconciliationBaselineAsset* baseline = nullptr;
    const SctReconciliationIncomingAsset* incoming = nullptr;
    SctIdentityMatchStatus status = SctIdentityMatchStatus::Strong;
    SctIdentityMatchProvenance provenance =
        SctIdentityMatchProvenance::Automatic;
    std::vector<SctIdentityEvidence> evidence;
};

using DecisionProvenance =
    std::unordered_map<std::string, SctIdentityMatchProvenance>;

[[nodiscard]] Diagnostic error(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctReconciliation,
        std::move(message), std::nullopt};
}

[[nodiscard]] bool validDecisionShape(
    const SctReconciliationDecision& decision) {
    if (decision.id.empty()) return false;
    const bool baseline = decision.baselineAsset.has_value();
    const bool incoming = decision.incomingAssetKey.has_value()
        && !decision.incomingAssetKey->empty();
    const bool hasBaselineEntity = decision.baselineEntity.has_value();
    const bool hasIncomingEntity = decision.incomingEntity.has_value();
    switch (decision.kind) {
    case SctReconciliationDecisionKind::PairAsset:
        return baseline && incoming && !hasBaselineEntity && !hasIncomingEntity;
    case SctReconciliationDecisionKind::IncomingAssetAddition:
        return !baseline && incoming && !hasBaselineEntity && !hasIncomingEntity;
    case SctReconciliationDecisionKind::BaselineAssetRemoval:
        return baseline && !incoming && !hasBaselineEntity && !hasIncomingEntity;
    case SctReconciliationDecisionKind::PairEntity:
        return baseline && incoming && hasBaselineEntity && hasIncomingEntity
            && decision.baselineEntity->index() == decision.incomingEntity->index();
    case SctReconciliationDecisionKind::IncomingEntityAddition:
        return baseline && incoming && !hasBaselineEntity && hasIncomingEntity;
    case SctReconciliationDecisionKind::BaselineEntityRemoval:
        return baseline && incoming && hasBaselineEntity && !hasIncomingEntity;
    }
    return false;
}

[[nodiscard]] std::string digestText(const std::string_view value) {
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    const auto digest = sha256(bytes);
    return digest ? digest.value().toHex() : std::string();
}

[[nodiscard]] std::string entityKey(const SctReconciliationEntityId& entity) {
    return std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, SctSectionId>)
            return "section:" + std::to_string(id.value());
        else if constexpr (std::is_same_v<T, SctInstructionId>)
            return "instruction:" + std::to_string(id.value());
        else if constexpr (std::is_same_v<T, SctStringId>)
            return "string:" + std::to_string(id.value());
        else if constexpr (std::is_same_v<T, SctFooterEntryId>)
            return "footer:" + std::to_string(id.value());
        else if constexpr (std::is_same_v<T, SctOpaqueAttachmentId>)
            return "opaque:" + std::to_string(id.value());
        else return "arm:" + std::to_string(id.value);
    }, entity);
}

[[nodiscard]] std::optional<SctNavigationTarget> navigation(
    const SctReconciliationEntityId& entity) {
    return std::visit([](const auto id) -> std::optional<SctNavigationTarget> {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, SctSectionId>)
            return SctNavigationTarget{SctNavigationKind::Section, id.value()};
        else if constexpr (std::is_same_v<T, SctInstructionId>)
            return SctNavigationTarget{SctNavigationKind::Instruction, id.value()};
        else if constexpr (std::is_same_v<T, SctStringId>)
            return SctNavigationTarget{SctNavigationKind::String, id.value()};
        else if constexpr (std::is_same_v<T, SctFooterEntryId>)
            return SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
        else if constexpr (std::is_same_v<T, SctOpaqueAttachmentId>)
            return SctNavigationTarget{SctNavigationKind::OpaqueAttachment, id.value()};
        else return std::nullopt;
    }, entity);
}

template<typename Id>
[[nodiscard]] Id remapped(const std::unordered_map<std::uint64_t, std::uint64_t>& map,
    const Id id) {
    const auto found = map.find(id.value());
    return found == map.end() ? id : Id{found->second};
}

void remapParameter(SctDocumentParameter& parameter, const IdMaps& maps) {
    std::visit([&](auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SctInstructionReference>)
            value.target = remapped(maps.instructions, value.target);
        else if constexpr (std::is_same_v<T, SctStringReference>)
            value.target = remapped(maps.strings, value.target);
        else if constexpr (std::is_same_v<T, SctFooterEntryReference>)
            value.target = remapped(maps.footers, value.target);
    }, parameter.value);
}

void remapInstruction(SctDocumentInstruction& instruction, const IdMaps& maps) {
    instruction.id = remapped(maps.instructions, instruction.id);
    for (auto& parameter : instruction.fixedParameters) remapParameter(parameter, maps);
    for (auto& group : instruction.repeatedParameterGroups)
        for (auto& parameter : group.parameters) remapParameter(parameter, maps);
}

[[nodiscard]] SctOpaqueAnchor remapAnchor(SctOpaqueAnchor anchor,
    const IdMaps& maps) {
    return std::visit([&](const auto value) -> SctOpaqueAnchor {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SctDocumentAnchor>) return value;
        else if constexpr (std::is_same_v<T, SctSectionId>)
            return remapped(maps.sections, value);
        else if constexpr (std::is_same_v<T, SctInstructionId>)
            return remapped(maps.instructions, value);
        else if constexpr (std::is_same_v<T, SctStringId>)
            return remapped(maps.strings, value);
        else return remapped(maps.footers, value);
    }, anchor);
}

[[nodiscard]] SctTextTarget remapTextTarget(SctTextTarget target,
    const IdMaps& maps) {
    return std::visit([&](const auto id) -> SctTextTarget {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, SctStringId>)
            return remapped(maps.strings, id);
        else return remapped(maps.footers, id);
    }, target);
}

[[nodiscard]] SctAuthoredArm remapArm(SctAuthoredArm arm, const IdMaps& maps) {
    if (const auto found = maps.arms.find(arm.id.value); found != maps.arms.end())
        arm.id.value = found->second;
    arm.controller.section = remapped(maps.sections, arm.controller.section);
    arm.controller.instruction = remapped(maps.instructions,
        arm.controller.instruction);
    if (arm.expectedJoin)
        arm.expectedJoin = remapped(maps.instructions, *arm.expectedJoin);
    for (auto& id : arm.members) id = remapped(maps.instructions, id);
    for (auto& id : arm.managedScaffolding)
        id = remapped(maps.instructions, id);
    return arm;
}

[[nodiscard]] SctUnboundReferenceOrigin remapOriginSite(
    SctUnboundReferenceOrigin origin, const IdMaps& maps) {
    origin.site.instruction = remapped(maps.instructions, origin.site.instruction);
    return origin;
}

void remapDocument(SctDocument& document, const IdMaps& maps) {
    for (auto& section : document.sections) {
        section.id = remapped(maps.sections, section.id);
        if (auto* script = std::get_if<SctScriptSectionContent>(&section.content))
            for (auto& instruction : script->instructions)
                remapInstruction(instruction, maps);
        else if (auto* string = std::get_if<SctStringSectionContent>(&section.content))
            string->string.id = remapped(maps.strings, string->string.id);
    }
    for (auto& footer : document.footerEntries)
        footer.id = remapped(maps.footers, footer.id);
    for (auto& opaque : document.opaqueAttachments) {
        opaque.id = remapped(maps.opaque, opaque.id);
        opaque.anchor = remapAnchor(opaque.anchor, maps);
    }
}

[[nodiscard]] SalsaScriptPatch additionPatch(const SctSemanticState& state) {
    SalsaScriptPatch patch;
    if (!state.document) return patch;
    std::vector<SctSectionId> sectionOrder;
    for (const auto& section : state.document->sections) {
        patch.sections.push_back({std::nullopt, section});
        sectionOrder.push_back(section.id);
    }
    if (!sectionOrder.empty()) patch.sectionOrder = {{}, sectionOrder};
    std::vector<SctFooterEntryId> footerOrder;
    for (const auto& footer : state.document->footerEntries) {
        patch.footerEntries.push_back({std::nullopt, footer});
        footerOrder.push_back(footer.id);
    }
    if (!footerOrder.empty()) patch.footerOrder = {{}, footerOrder};
    for (const auto& arm : state.authoredArms)
        patch.authoredArms.push_back({std::nullopt, arm});
    for (const auto& repair : state.textRepairs)
        patch.textRepairs.push_back({repair.target, std::nullopt, repair.provenance});
    for (const auto& origin : state.unboundReferences)
        patch.unboundReferences.push_back({origin.site, std::nullopt, origin});
    return patch;
}

[[nodiscard]] Result<std::string> exactFingerprint(const SctSemanticState& state) {
    if (!state.document) return Result<std::string>::failure(
        error("A reconciliation fingerprint requires a document."));
    auto bytes = SalsaScriptPatchCodec::serialize(additionPatch(state));
    if (!bytes) return Result<std::string>::failure(bytes.diagnostics());
    auto hasher = Sha256Hasher::create();
    if (!hasher) return Result<std::string>::failure(hasher.diagnostics());
    auto update = hasher.value().update(bytes.value());
    if (!update) return Result<std::string>::failure(update.diagnostics());
    std::ostringstream opaque;
    for (const auto& value : state.document->opaqueAttachments) {
        opaque << value.id.value() << ':' << value.anchor.index() << ':';
        std::visit([&](const auto anchor) {
            using T = std::decay_t<decltype(anchor)>;
            if constexpr (!std::is_same_v<T, SctDocumentAnchor>) opaque << anchor.value();
        }, value.anchor);
        opaque << ':' << static_cast<int>(value.placement) << ':'
            << value.fixedOffset.value_or(0) << ':' << value.alignment << ':'
            << static_cast<int>(value.relocation) << ':' << static_cast<int>(value.reason)
            << ':';
        for (const auto byte : value.bytes) opaque << static_cast<unsigned int>(byte) << ',';
        opaque << ';';
    }
    const auto opaqueText = opaque.str();
    update = hasher.value().update(std::as_bytes(std::span{
        opaqueText.data(), opaqueText.size()}));
    if (!update) return Result<std::string>::failure(update.diagnostics());
    auto digest = hasher.value().finish();
    if (!digest) return Result<std::string>::failure(digest.diagnostics());
    return Result<std::string>::success(digest.value().toHex());
}

[[nodiscard]] SctSemanticState canonicalized(const SctSemanticState& source) {
    if (!source.document) return {};
    IdMaps maps;
    std::uint64_t section = 1, instruction = 1, string = 1, footer = 1, opaque = 1,
        arm = 1;
    for (const auto& value : source.document->sections) {
        maps.sections.emplace(value.id.value(), section++);
        if (const auto* scriptValue = std::get_if<SctScriptSectionContent>(&value.content))
            for (const auto& item : scriptValue->instructions)
                maps.instructions.emplace(item.id.value(), instruction++);
        else if (const auto* stringValue = std::get_if<SctStringSectionContent>(&value.content))
            maps.strings.emplace(stringValue->string.id.value(), string++);
    }
    for (const auto& value : source.document->footerEntries)
        maps.footers.emplace(value.id.value(), footer++);
    for (const auto& value : source.document->opaqueAttachments)
        maps.opaque.emplace(value.id.value(), opaque++);
    for (const auto& value : source.authoredArms)
        maps.arms.emplace(value.id.value, arm++);
    auto document = std::make_shared<SctDocument>(*source.document);
    remapDocument(*document, maps);
    auto rebuilt = SctDocumentBuilder::reconstitute(*document);
    if (!rebuilt.document) return {};
    document = std::make_shared<SctDocument>(std::move(*rebuilt.document));
    SctSemanticState result{document};
    for (const auto& value : source.authoredArms)
        result.authoredArms.push_back(remapArm(value, maps));
    for (auto value : source.textRepairs) {
        value.target = remapTextTarget(value.target, maps);
        result.textRepairs.push_back(std::move(value));
    }
    for (auto value : source.unboundReferences)
        result.unboundReferences.push_back(remapOriginSite(std::move(value), maps));
    return result;
}

[[nodiscard]] std::string neutralFingerprint(const SctSemanticState& state) {
    const auto result = exactFingerprint(canonicalized(state));
    return result ? result.value() : std::string();
}

[[nodiscard]] std::string sectionSignature(const SctDocumentSection& section) {
    auto document = std::make_shared<SctDocument>();
    document->sections.push_back(section);
    return neutralFingerprint({document, {}, {}});
}

[[nodiscard]] std::string instructionSignature(
    const SctDocumentInstruction& instruction) {
    SctDocumentSection section{SctSectionId{1}, {},
        SctScriptSectionContent{{instruction}}};
    return sectionSignature(section);
}

[[nodiscard]] std::string instructionShape(
    const SctDocumentInstruction& instruction) {
    std::ostringstream result;
    result << instruction.opcode << '|' << instruction.scheduledExpression.has_value();
    for (const auto& parameter : instruction.fixedParameters)
        result << '|'
            << parameter.schemaIndex << ':' << parameter.value.index();
    result << "|groups:" << instruction.repeatedParameterGroups.size();
    for (const auto& group : instruction.repeatedParameterGroups) {
        result << '[';
        for (const auto& parameter : group.parameters)
            result << parameter.schemaIndex << ':' << parameter.value.index() << ',';
        result << ']';
    }
    return result.str();
}

[[nodiscard]] std::string footerSignature(
    const SctDocumentFooterEntry& footer);

[[nodiscard]] std::string instructionLocalSignature(
    SctDocumentInstruction instruction) {
    instruction.id = SctInstructionId{1};
    const auto normalize = [](SctDocumentParameter& parameter) {
        std::visit([](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, SctInstructionReference>)
                value.target = SctInstructionId{1};
            else if constexpr (std::is_same_v<T, SctStringReference>)
                value.target = SctStringId{1};
            else if constexpr (std::is_same_v<T, SctFooterEntryReference>)
                value.target = SctFooterEntryId{1};
        }, parameter.value);
    };
    for (auto& parameter : instruction.fixedParameters) normalize(parameter);
    for (auto& group : instruction.repeatedParameterGroups)
        for (auto& parameter : group.parameters) normalize(parameter);
    return instructionSignature(instruction);
}

[[nodiscard]] std::unordered_map<std::uint64_t, std::string>
instructionGraphSignatures(const SctDocument& document) {
    std::unordered_map<std::uint64_t, std::string> signatures;
    std::unordered_map<std::uint64_t, std::string> strings, footers;
    std::size_t instructionCount = 0;
    for (const auto& section : document.sections) {
        if (const auto* script = std::get_if<SctScriptSectionContent>(&section.content)) {
            for (const auto& instruction : script->instructions) {
                signatures[instruction.id.value()] =
                    instructionLocalSignature(instruction);
                ++instructionCount;
            }
        } else if (const auto* string =
                std::get_if<SctStringSectionContent>(&section.content)) {
            auto neutral = section;
            neutral.id = SctSectionId{1};
            std::get<SctStringSectionContent>(neutral.content).string.id =
                SctStringId{1};
            strings[string->string.id.value()] = sectionSignature(neutral);
        }
    }
    for (auto footer : document.footerEntries) {
        const auto id = footer.id;
        footer.id = SctFooterEntryId{1};
        footers[id.value()] = footerSignature(footer);
    }
    const auto appendReferences = [&](std::ostringstream& output,
                                      const SctDocumentParameter& parameter,
                                      const auto& current) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, SctInstructionReference>) {
                const auto found = current.find(value.target.value());
                output << "|i:" << (found == current.end() ? "missing" : found->second);
            } else if constexpr (std::is_same_v<T, SctStringReference>) {
                const auto found = strings.find(value.target.value());
                output << "|s:" << (found == strings.end() ? "missing" : found->second);
            } else if constexpr (std::is_same_v<T, SctFooterEntryReference>) {
                const auto found = footers.find(value.target.value());
                output << "|f:" << (found == footers.end() ? "missing" : found->second);
            }
        }, parameter.value);
    };
    for (std::size_t round = 0; round <= instructionCount; ++round) {
        auto next = signatures;
        for (const auto& section : document.sections) {
            const auto* script = std::get_if<SctScriptSectionContent>(&section.content);
            if (!script) continue;
            for (const auto& instruction : script->instructions) {
                std::ostringstream value;
                value << instructionLocalSignature(instruction);
                for (const auto& parameter : instruction.fixedParameters)
                    appendReferences(value, parameter, signatures);
                for (const auto& group : instruction.repeatedParameterGroups)
                    for (const auto& parameter : group.parameters)
                        appendReferences(value, parameter, signatures);
                next[instruction.id.value()] = digestText(value.str());
            }
        }
        if (next == signatures) break;
        signatures = std::move(next);
    }
    return signatures;
}

[[nodiscard]] std::string footerSignature(const SctDocumentFooterEntry& footer) {
    auto document = std::make_shared<SctDocument>();
    document->footerEntries.push_back(footer);
    return neutralFingerprint({document, {}, {}});
}

[[nodiscard]] std::string stem(const AssetLocator& locator) {
    auto value = locator.path().stem().generic_u8string();
    std::string text(reinterpret_cast<const char*>(value.data()), value.size());
    std::ranges::transform(text, text.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

[[nodiscard]] bool structurallyCompatible(const SctSemanticState& left,
    const SctSemanticState& right) {
    if (!left.document || !right.document) return false;
    if (left.document->sections.empty() || right.document->sections.empty())
        return left.document->sections.empty() == right.document->sections.empty();
    return std::ranges::any_of(left.document->sections, [&](const auto& source) {
        return std::ranges::any_of(right.document->sections, [&](const auto& target) {
            return source.content.index() == target.content.index();
        });
    });
}

template<typename T, typename Id>
[[nodiscard]] const T* findById(const std::vector<T>& values, const Id id) {
    const auto found = std::ranges::find(values, id, &T::id);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] const SctDocumentInstruction* findInstruction(
    const SctDocument& document, const SctInstructionId id,
    SctSectionId* owner = nullptr) {
    for (const auto& section : document.sections) {
        const auto* script = std::get_if<SctScriptSectionContent>(&section.content);
        if (!script) continue;
        const auto* result = findById(script->instructions, id);
        if (result) {
            if (owner) *owner = section.id;
            return result;
        }
    }
    return nullptr;
}

[[nodiscard]] const SctDocumentString* findString(const SctDocument& document,
    const SctStringId id, SctSectionId* owner = nullptr) {
    for (const auto& section : document.sections) {
        const auto* value = std::get_if<SctStringSectionContent>(&section.content);
        if (value && value->string.id == id) {
            if (owner) *owner = section.id;
            return &value->string;
        }
    }
    return nullptr;
}

[[nodiscard]] const SctAuthoredArm* findArm(const std::vector<SctAuthoredArm>& values,
    const SctAuthoredArmId id) {
    const auto found = std::ranges::find(values, id, &SctAuthoredArm::id);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] bool entityExists(const SctSemanticState& state,
    const SctReconciliationEntityId& entity) {
    if (!state.document) return false;
    return std::visit([&](const auto id) {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, SctSectionId>)
            return findById(state.document->sections, id) != nullptr;
        else if constexpr (std::is_same_v<T, SctInstructionId>)
            return findInstruction(*state.document, id) != nullptr;
        else if constexpr (std::is_same_v<T, SctStringId>)
            return findString(*state.document, id) != nullptr;
        else if constexpr (std::is_same_v<T, SctFooterEntryId>)
            return findById(state.document->footerEntries, id) != nullptr;
        else if constexpr (std::is_same_v<T, SctOpaqueAttachmentId>)
            return findById(state.document->opaqueAttachments, id) != nullptr;
        else return findArm(state.authoredArms, id) != nullptr;
    }, entity);
}

[[nodiscard]] bool hasDecision(const std::vector<SctReconciliationDecision>& decisions,
    const DecisionProvenance& decisionProvenance,
    const SctReconciliationDecisionKind kind, const AssetLocator& baselineAsset,
    const std::string& incomingKey, const std::optional<SctReconciliationEntityId>& baseline,
    const std::optional<SctReconciliationEntityId>& incoming,
    SctIdentityMatchProvenance* provenance = nullptr) {
    const auto found = std::ranges::find_if(decisions, [&](const auto& value) {
        return value.kind == kind && value.baselineAsset
            && *value.baselineAsset == baselineAsset && value.incomingAssetKey
            && *value.incomingAssetKey == incomingKey
            && value.baselineEntity == baseline && value.incomingEntity == incoming;
    });
    if (found == decisions.end()) return false;
    if (provenance) *provenance = decisionProvenance.at(found->id);
    return true;
}

template<typename Id, typename Value, typename Signature>
void matchUniqueExact(const std::vector<Value>& baseline,
    const std::vector<Value>& incoming, std::unordered_set<std::uint64_t>& usedBaseline,
    std::unordered_set<std::uint64_t>& usedIncoming, std::vector<EntityMatch>& matches,
    Signature signature) {
    std::map<std::string, std::vector<const Value*>> left, right;
    for (const auto& value : baseline)
        if (!usedBaseline.contains(value.id.value())) left[signature(value)].push_back(&value);
    for (const auto& value : incoming)
        if (!usedIncoming.contains(value.id.value())) right[signature(value)].push_back(&value);
    for (const auto& [key, values] : left) {
        const auto found = right.find(key);
        if (values.size() != 1u || found == right.end() || found->second.size() != 1u)
            continue;
        const auto* source = values.front();
        const auto* target = found->second.front();
        usedBaseline.insert(source->id.value());
        usedIncoming.insert(target->id.value());
        matches.push_back({Id{source->id.value()}, Id{target->id.value()},
            SctIdentityMatchStatus::Exact, SctIdentityMatchProvenance::Automatic,
            {{"exact-semantic", "Unique identity-neutral semantic content matches."}}});
    }
}

template<typename Id, typename Value, typename BaselineSignature,
    typename IncomingSignature>
void matchUniqueExactWith(const std::vector<Value>& baseline,
    const std::vector<Value>& incoming, std::unordered_set<std::uint64_t>& usedBaseline,
    std::unordered_set<std::uint64_t>& usedIncoming, std::vector<EntityMatch>& matches,
    BaselineSignature baselineSignature, IncomingSignature incomingSignature,
    std::string code, std::string evidence) {
    std::map<std::string, std::vector<const Value*>> left, right;
    for (const auto& value : baseline)
        if (!usedBaseline.contains(value.id.value()))
            left[baselineSignature(value)].push_back(&value);
    for (const auto& value : incoming)
        if (!usedIncoming.contains(value.id.value()))
            right[incomingSignature(value)].push_back(&value);
    for (const auto& [key, values] : left) {
        const auto found = right.find(key);
        if (values.size() != 1u || found == right.end()
            || found->second.size() != 1u) continue;
        const auto* source = values.front();
        const auto* target = found->second.front();
        usedBaseline.insert(source->id.value());
        usedIncoming.insert(target->id.value());
        matches.push_back({Id{source->id.value()}, Id{target->id.value()},
            SctIdentityMatchStatus::Exact,
            SctIdentityMatchProvenance::Automatic,
            {{code, evidence}}});
    }
}

template<typename Id, typename Value, typename Shape>
void matchMutualUniqueStrong(const std::vector<Value>& baseline,
    const std::vector<Value>& incoming, std::unordered_set<std::uint64_t>& usedBaseline,
    std::unordered_set<std::uint64_t>& usedIncoming, std::vector<EntityMatch>& matches,
    Shape shape, std::string evidence) {
    for (const auto& source : baseline) {
        if (usedBaseline.contains(source.id.value())) continue;
        std::vector<const Value*> candidates;
        for (const auto& target : incoming)
            if (!usedIncoming.contains(target.id.value())
                && shape(source) == shape(target)) candidates.push_back(&target);
        if (candidates.size() != 1u) continue;
        const auto* target = candidates.front();
        std::size_t reverse = 0;
        for (const auto& alternative : baseline)
            if (!usedBaseline.contains(alternative.id.value())
                && shape(alternative) == shape(*target)) ++reverse;
        if (reverse != 1u) continue;
        usedBaseline.insert(source.id.value());
        usedIncoming.insert(target->id.value());
        matches.push_back({Id{source.id.value()}, Id{target->id.value()},
            SctIdentityMatchStatus::Strong, SctIdentityMatchProvenance::Automatic,
            {{"compatible-shape", evidence}}});
    }
}

template<typename Id>
[[nodiscard]] std::optional<Id> entityAs(
    const std::optional<SctReconciliationEntityId>& value) {
    if (!value) return std::nullopt;
    if (const auto* id = std::get_if<Id>(&*value)) return *id;
    return std::nullopt;
}

template<typename Id>
void applyExplicitPairs(const std::vector<SctReconciliationDecision>& decisions,
    const DecisionProvenance& decisionProvenance,
    const AssetLocator& baselineAsset, const std::string& incomingKey,
    std::unordered_set<std::uint64_t>& usedBaseline,
    std::unordered_set<std::uint64_t>& usedIncoming,
    std::vector<EntityMatch>& matches) {
    for (const auto& decision : decisions) {
        if (decision.kind != SctReconciliationDecisionKind::PairEntity
            || !decision.baselineAsset || *decision.baselineAsset != baselineAsset
            || !decision.incomingAssetKey || *decision.incomingAssetKey != incomingKey)
            continue;
        const auto source = entityAs<Id>(decision.baselineEntity);
        const auto target = entityAs<Id>(decision.incomingEntity);
        if (!source || !target) continue;
        if (usedBaseline.contains(source->value())
            || usedIncoming.contains(target->value())) continue;
        usedBaseline.insert(source->value());
        usedIncoming.insert(target->value());
        matches.push_back({*source, *target, SctIdentityMatchStatus::Strong,
            decisionProvenance.at(decision.id),
            {{"explicit-decision", "The user explicitly paired these entities."}}});
    }
}

[[nodiscard]] std::vector<SctDocumentInstruction> instructions(
    const SctDocumentSection& section) {
    const auto* script = std::get_if<SctScriptSectionContent>(&section.content);
    return script ? script->instructions : std::vector<SctDocumentInstruction>{};
}

template<typename Id>
void setMap(std::unordered_map<std::uint64_t, std::uint64_t>& map,
    const std::vector<EntityMatch>& matches) {
    for (const auto& match : matches) {
        const auto* source = std::get_if<Id>(&match.incoming);
        const auto* target = std::get_if<Id>(&match.baseline);
        if (source && target) map[source->value()] = target->value();
    }
}

template<typename Id>
[[nodiscard]] bool removed(const std::vector<SctReconciliationDecision>& decisions,
    const DecisionProvenance& decisionProvenance,
    const AssetLocator& asset, const std::string& incoming,
    const Id id) {
    return hasDecision(decisions, decisionProvenance,
        SctReconciliationDecisionKind::BaselineEntityRemoval,
        asset, incoming, SctReconciliationEntityId{id}, std::nullopt);
}

template<typename Id>
[[nodiscard]] std::optional<SctIdentityMatchProvenance> confirmedAddition(
    const std::vector<SctReconciliationDecision>& decisions,
    const DecisionProvenance& decisionProvenance,
    const AssetLocator& asset, const std::string& incoming,
    const Id id) {
    SctIdentityMatchProvenance provenance{};
    if (!hasDecision(decisions, decisionProvenance,
            SctReconciliationDecisionKind::IncomingEntityAddition,
            asset, incoming, std::nullopt, SctReconciliationEntityId{id},
            &provenance))
        return std::nullopt;
    return provenance;
}

template<typename Value, typename Id>
void mergeUnmatchedBaseline(std::vector<Value>& projected,
    const std::vector<Value>& baseline, const std::unordered_set<std::uint64_t>& matched,
    const std::vector<SctReconciliationDecision>& decisions,
    const DecisionProvenance& decisionProvenance,
    const AssetLocator& asset, const std::string& incoming) {
    for (std::size_t index = 0; index < baseline.size(); ++index) {
        const auto& value = baseline[index];
        if (matched.contains(value.id.value())
            || removed(decisions, decisionProvenance, asset, incoming,
                Id{value.id.value()})) continue;
        auto insertion = projected.end();
        for (auto next = index + 1; next < baseline.size(); ++next) {
            const auto found = std::ranges::find(projected,
                baseline[next].id, &Value::id);
            if (found != projected.end()) { insertion = found; break; }
        }
        projected.insert(insertion, value);
    }
}

void notify(const SctReconciliationRequest& request,
    const SctReconciliationProgressPhase phase, const std::size_t completed,
    const std::size_t total, const AssetPair* pair = nullptr) {
    if (!request.progress) return;
    SctReconciliationProgress progress{phase, completed, total};
    if (pair) {
        progress.baselineAsset = pair->baseline->locator;
        progress.incomingAssetKey = pair->incoming->key;
    }
    request.progress(progress);
}

[[nodiscard]] SctReconciliationResult cancelledResult() {
    SctReconciliationResult result;
    result.cancelled = true;
    result.diagnostics.push_back({SctChangeDisposition::Informational,
        "Cancelled", "SCT reconciliation was cancelled."});
    return result;
}

}  // namespace

Result<std::string> SctDocumentReconciler::semanticFingerprint(
    const SctSemanticState& state) {
    return exactFingerprint(state);
}

Result<SctReconciliationResult> SctDocumentReconciler::reconcile(
    const SctReconciliationRequest& request) {
    if (request.decisionScopeId.empty() || request.targetScopeKey.empty())
        return Result<SctReconciliationResult>::failure(error(
            "Reconciliation requires decision and target scope identities."));
    notify(request, SctReconciliationProgressPhase::Preflight, 0,
        request.baselineAssets.size() + request.incomingAssets.size());
    if (request.stopToken.stop_requested())
        return Result<SctReconciliationResult>::success(cancelledResult());

    std::unordered_map<std::string, const SctReconciliationBaselineAsset*> baselines;
    std::unordered_map<std::string, const SctReconciliationIncomingAsset*> incoming;
    std::unordered_map<std::string, std::string> incomingFingerprints;
    for (const auto& asset : request.baselineAssets) {
        if (!asset.state.document || !baselines.emplace(
                asset.locator.identityKey(), &asset).second)
            return Result<SctReconciliationResult>::failure(error(
                "Baseline asset identities must be unique and contain documents."));
    }
    for (const auto& asset : request.incomingAssets) {
        if (asset.key.empty() || !asset.state.document
            || !incoming.emplace(asset.key, &asset).second)
            return Result<SctReconciliationResult>::failure(error(
                "Incoming asset keys must be unique and contain documents."));
        auto fingerprint = exactFingerprint(asset.state);
        if (!fingerprint) return Result<SctReconciliationResult>::failure(
            fingerprint.diagnostics());
        incomingFingerprints.emplace(asset.key, std::move(fingerprint).takeValue());
    }

    SctReconciliationResult result;
    std::vector<SctReconciliationDecision> decisions;
    std::unordered_map<std::string, SctIdentityMatchProvenance> decisionProvenance;
    std::unordered_set<std::string> decisionIds;
    for (const auto& decision : request.decisions) {
        if (!validDecisionShape(decision)
            || !decisionIds.insert(decision.id).second)
            return Result<SctReconciliationResult>::failure(error(
                "Current reconciliation decisions must be well-formed with unique IDs."));
        decisions.push_back(decision);
        decisionProvenance[decision.id] =
            SctIdentityMatchProvenance::CurrentUserDecision;
    }
    if (request.persistedDecisions) {
        const auto& artifact = request.persistedDecisions->get();
        if (artifact.decisionScopeId != request.decisionScopeId
            || artifact.targetScopeKey != request.targetScopeKey
            || artifact.reconciliationContractVersion != ContractVersion)
            return Result<SctReconciliationResult>::failure(error(
                "The persisted reconciliation decisions have the wrong context."));
        for (const auto& binding : artifact.assets) {
            bool valid = true;
            if (binding.baselineAsset) {
                const auto found = baselines.find(binding.baselineAsset->identityKey());
                valid = found != baselines.end() && binding.baselineRevision
                    && found->second->revision == *binding.baselineRevision;
            }
            if (valid && binding.incomingAssetKey) {
                const auto found = incomingFingerprints.find(*binding.incomingAssetKey);
                valid = found != incomingFingerprints.end() && binding.incomingFingerprint
                    && found->second == *binding.incomingFingerprint;
            }
            if (!valid) {
                result.diagnostics.push_back({SctChangeDisposition::Warning,
                    "PersistedDecisionInvalidated",
                    "Persisted decisions for one changed asset pair were ignored."});
                continue;
            }
            for (const auto& decision : binding.decisions) {
                if (!validDecisionShape(decision))
                    return Result<SctReconciliationResult>::failure(error(
                        "A persisted reconciliation decision is malformed."));
                if (!decisionIds.insert(decision.id).second) continue;
                decisions.push_back(decision);
                decisionProvenance[decision.id] =
                    SctIdentityMatchProvenance::PersistedUserDecision;
            }
        }
    }

    std::unordered_set<std::string> decidedBaselineEntities,
        decidedIncomingEntities;
    for (const auto& decision : decisions) {
        const SctReconciliationBaselineAsset* baseline = nullptr;
        const SctReconciliationIncomingAsset* incomingAsset = nullptr;
        if (decision.baselineAsset) {
            const auto found = baselines.find(decision.baselineAsset->identityKey());
            if (found == baselines.end())
                return Result<SctReconciliationResult>::failure(error(
                    "A reconciliation decision references an unknown baseline asset."));
            baseline = found->second;
        }
        if (decision.incomingAssetKey) {
            const auto found = incoming.find(*decision.incomingAssetKey);
            if (found == incoming.end())
                return Result<SctReconciliationResult>::failure(error(
                    "A reconciliation decision references an unknown incoming asset."));
            incomingAsset = found->second;
        }
        if (decision.baselineEntity
            && (!baseline || !entityExists(baseline->state,
                *decision.baselineEntity)))
            return Result<SctReconciliationResult>::failure(error(
                "A reconciliation decision references an unknown baseline entity."));
        if (decision.incomingEntity
            && (!incomingAsset || !entityExists(incomingAsset->state,
                *decision.incomingEntity)))
            return Result<SctReconciliationResult>::failure(error(
                "A reconciliation decision references an unknown incoming entity."));
        if (decision.kind == SctReconciliationDecisionKind::PairEntity) {
            const auto baselineKey = decision.baselineAsset->identityKey() + '|'
                + entityKey(*decision.baselineEntity);
            const auto incomingKey = *decision.incomingAssetKey + '|'
                + entityKey(*decision.incomingEntity);
            if (!decidedBaselineEntities.insert(baselineKey).second
                || !decidedIncomingEntities.insert(incomingKey).second)
                return Result<SctReconciliationResult>::failure(error(
                    "Entity decisions pair one entity more than once."));
        }
    }

    notify(request, SctReconciliationProgressPhase::AssetMatching, 0,
        request.baselineAssets.size());
    if (request.stopToken.stop_requested())
        return Result<SctReconciliationResult>::success(cancelledResult());
    std::vector<AssetPair> pairs;
    std::unordered_set<std::string> pairedBaseline, pairedIncoming;
    std::unordered_set<std::string> excludedBaseline, excludedIncoming;
    for (const auto& decision : decisions) {
        if (decision.kind == SctReconciliationDecisionKind::BaselineAssetRemoval)
            excludedBaseline.insert(decision.baselineAsset->identityKey());
        else if (decision.kind
                == SctReconciliationDecisionKind::IncomingAssetAddition)
            excludedIncoming.insert(*decision.incomingAssetKey);
    }
    for (const auto& decision : decisions) {
        if (decision.kind != SctReconciliationDecisionKind::PairAsset
            || !decision.baselineAsset || !decision.incomingAssetKey) continue;
        const auto left = baselines.find(decision.baselineAsset->identityKey());
        const auto right = incoming.find(*decision.incomingAssetKey);
        if (left == baselines.end() || right == incoming.end()) continue;
        if (excludedBaseline.contains(left->first)
            || excludedIncoming.contains(right->first))
            return Result<SctReconciliationResult>::failure(error(
                "An asset cannot be both paired and explicitly added or removed."));
        if (!pairedBaseline.insert(left->first).second
            || !pairedIncoming.insert(right->first).second)
            return Result<SctReconciliationResult>::failure(error(
                "Asset decisions pair one asset more than once."));
        const auto compatible = structurallyCompatible(
            left->second->state, right->second->state);
        pairs.push_back({left->second, right->second,
            compatible ? SctIdentityMatchStatus::Strong
                       : SctIdentityMatchStatus::Contradictory,
            decisionProvenance[decision.id],
            {{"explicit-decision", "The user explicitly paired these assets."}}});
    }
    const auto autoPair = [&](auto predicate, const SctIdentityMatchStatus status,
                              std::string code, std::string detail) {
        for (const auto& source : request.baselineAssets) {
            if (pairedBaseline.contains(source.locator.identityKey())
                || excludedBaseline.contains(source.locator.identityKey())) continue;
            std::vector<const SctReconciliationIncomingAsset*> candidates;
            for (const auto& target : request.incomingAssets)
                if (!pairedIncoming.contains(target.key)
                    && !excludedIncoming.contains(target.key)
                    && predicate(source, target))
                    candidates.push_back(&target);
            if (candidates.size() != 1u) continue;
            const auto* target = candidates.front();
            std::size_t reverse = 0;
            for (const auto& alternative : request.baselineAssets)
                if (!pairedBaseline.contains(alternative.locator.identityKey())
                    && !excludedBaseline.contains(alternative.locator.identityKey())
                    && predicate(alternative, *target)) ++reverse;
            if (reverse != 1u) continue;
            pairedBaseline.insert(source.locator.identityKey());
            pairedIncoming.insert(target->key);
            pairs.push_back({&source, target, status,
                SctIdentityMatchProvenance::Automatic,
                {{code, detail}}});
        }
    };
    autoPair([&](const auto& left, const auto& right) {
        return neutralFingerprint(left.state) == neutralFingerprint(right.state);
    }, SctIdentityMatchStatus::Exact, "exact-semantic",
        "The complete identity-neutral semantic states are equal.");
    autoPair([&](const auto& left, const auto& right) {
        return right.proposedLocator
            && left.locator == *right.proposedLocator
            && structurallyCompatible(left.state, right.state);
    }, SctIdentityMatchStatus::Strong, "exact-locator",
        "The proposed locator and compatible structure uniquely agree.");
    autoPair([&](const auto& left, const auto& right) {
        return right.proposedLocator && stem(left.locator) == stem(*right.proposedLocator)
            && structurallyCompatible(left.state, right.state);
    }, SctIdentityMatchStatus::Strong, "exact-stem",
        "The target-scoped stem and compatible structure uniquely agree.");
    std::ranges::sort(pairs, {}, [](const auto& value) {
        return value.baseline->locator.identityKey();
    });
    for (const auto& pair : pairs) result.assets.push_back({pair.baseline->locator,
        pair.incoming->key, pair.status, pair.provenance, pair.evidence, true});
    for (const auto& asset : request.baselineAssets) {
        if (pairedBaseline.contains(asset.locator.identityKey())) continue;
        const auto removal = std::ranges::find_if(decisions, [&](const auto& decision) {
            return decision.kind == SctReconciliationDecisionKind::BaselineAssetRemoval
                && decision.baselineAsset && *decision.baselineAsset == asset.locator;
        });
        result.assets.push_back({asset.locator, std::nullopt,
            SctIdentityMatchStatus::Unmatched,
            removal == decisions.end() ? SctIdentityMatchProvenance::Automatic
                : decisionProvenance.at(removal->id),
            {{removal == decisions.end() ? "unresolved-asset" : "explicit-removal",
                removal == decisions.end() ? "No unique incoming asset was proven."
                    : "The user confirmed removal of this baseline asset."}},
            removal != decisions.end()});
    }
    for (const auto& asset : request.incomingAssets) {
        if (pairedIncoming.contains(asset.key)) continue;
        const auto addition = std::ranges::find_if(decisions, [&](const auto& decision) {
            return decision.kind == SctReconciliationDecisionKind::IncomingAssetAddition
                && decision.incomingAssetKey && *decision.incomingAssetKey == asset.key;
        });
        result.assets.push_back({std::nullopt, asset.key,
            SctIdentityMatchStatus::Unmatched,
            addition == decisions.end() ? SctIdentityMatchProvenance::Automatic
                : decisionProvenance.at(addition->id),
            {{addition == decisions.end() ? "unresolved-asset" : "explicit-addition",
                addition == decisions.end() ? "No unique baseline asset was proven."
                    : "The user confirmed this incoming asset as new."}},
            addition != decisions.end()});
    }
    std::ranges::sort(result.assets, {}, [](const auto& asset) {
        return asset.baseline ? asset.baseline->identityKey()
            : "~" + asset.incomingKey.value_or("");
    });

    std::vector<SctScriptComparisonInput> comparisons;
    std::unordered_map<std::string,
        std::unordered_map<std::string, SctEntityCorrespondence>> annotations;
    std::ostringstream identity;
    identity << request.decisionScopeId << '|' << request.targetScopeKey;
    for (const auto& asset : result.assets)
        identity << "|asset:"
            << (asset.baseline ? asset.baseline->identityKey() : "none") << ':'
            << asset.incomingKey.value_or("none") << ':'
            << static_cast<int>(asset.status) << ':'
            << static_cast<int>(asset.provenance) << ':' << asset.resolved;
    for (std::size_t pairIndex = 0; pairIndex < pairs.size(); ++pairIndex) {
        if (request.stopToken.stop_requested())
            return Result<SctReconciliationResult>::success(cancelledResult());
        const auto& pair = pairs[pairIndex];
        notify(request, SctReconciliationProgressPhase::EntityMatching,
            pairIndex, pairs.size(), &pair);
        if (request.stopToken.stop_requested())
            return Result<SctReconciliationResult>::success(cancelledResult());
        const auto& baselineDocument = *pair.baseline->state.document;
        const auto& incomingDocument = *pair.incoming->state.document;
        std::vector<EntityMatch> matches;
        std::unordered_set<std::uint64_t> usedBaselineSections,
            usedIncomingSections;
        applyExplicitPairs<SctSectionId>(decisions, decisionProvenance,
            pair.baseline->locator,
            pair.incoming->key, usedBaselineSections, usedIncomingSections, matches);
        matchUniqueExact<SctSectionId>(baselineDocument.sections,
            incomingDocument.sections, usedBaselineSections, usedIncomingSections,
            matches, sectionSignature);
        matchMutualUniqueStrong<SctSectionId>(baselineDocument.sections,
            incomingDocument.sections, usedBaselineSections, usedIncomingSections,
            matches, [](const auto& section) {
                return std::to_string(section.content.index()) + '|' + section.nameBytes;
            }, "A unique section kind and exact name agree.");
        matchMutualUniqueStrong<SctSectionId>(baselineDocument.sections,
            incomingDocument.sections, usedBaselineSections, usedIncomingSections,
            matches, [](const auto& section) {
                return std::to_string(section.content.index());
            }, "A unique compatible section kind agrees within the paired asset.");

        const auto baselineGraphSignatures =
            instructionGraphSignatures(baselineDocument);
        const auto incomingGraphSignatures =
            instructionGraphSignatures(incomingDocument);
        std::vector<EntityMatch> instructionMatches;
        for (const auto& sectionMatch : matches) {
            const auto* baselineSectionId = std::get_if<SctSectionId>(&sectionMatch.baseline);
            const auto* incomingSectionId = std::get_if<SctSectionId>(&sectionMatch.incoming);
            if (!baselineSectionId || !incomingSectionId) continue;
            const auto* left = findById(baselineDocument.sections, *baselineSectionId);
            const auto* right = findById(incomingDocument.sections, *incomingSectionId);
            if (!left || !right || !std::holds_alternative<SctScriptSectionContent>(left->content)
                || !std::holds_alternative<SctScriptSectionContent>(right->content)) continue;
            auto leftInstructions = instructions(*left);
            auto rightInstructions = instructions(*right);
            std::unordered_set<std::uint64_t> usedLeft, usedRight;
            for (const auto& decision : decisions) {
                if (decision.kind != SctReconciliationDecisionKind::PairEntity
                    || !decision.baselineAsset
                    || *decision.baselineAsset != pair.baseline->locator
                    || !decision.incomingAssetKey
                    || *decision.incomingAssetKey != pair.incoming->key) continue;
                const auto source = entityAs<SctInstructionId>(decision.baselineEntity);
                const auto target = entityAs<SctInstructionId>(decision.incomingEntity);
                if (!source || !target) continue;
                SctSectionId sourceOwner, targetOwner;
                if (!findInstruction(baselineDocument, *source, &sourceOwner)
                    || !findInstruction(incomingDocument, *target, &targetOwner)) continue;
                if (sourceOwner != *baselineSectionId || targetOwner != *incomingSectionId)
                    continue;
                if (usedLeft.insert(source->value()).second
                    && usedRight.insert(target->value()).second)
                    instructionMatches.push_back({*source, *target,
                        SctIdentityMatchStatus::Strong,
                        decisionProvenance.at(decision.id),
                        {{"explicit-decision", "The user explicitly paired these instructions."}}});
            }
            matchUniqueExact<SctInstructionId>(leftInstructions, rightInstructions,
                usedLeft, usedRight, instructionMatches, instructionSignature);
            matchUniqueExactWith<SctInstructionId>(leftInstructions,
                rightInstructions, usedLeft, usedRight, instructionMatches,
                [&](const auto& instruction) {
                    return baselineGraphSignatures.at(instruction.id.value());
                }, [&](const auto& instruction) {
                    return incomingGraphSignatures.at(instruction.id.value());
                }, "reference-graph",
                "Unique identity-neutral reference neighborhoods match.");
            matchMutualUniqueStrong<SctInstructionId>(leftInstructions,
                rightInstructions, usedLeft, usedRight, instructionMatches,
                instructionShape,
                "A mutually unique opcode and parameter-schema shape agrees in the matched section.");
        }
        matches.insert(matches.end(), instructionMatches.begin(), instructionMatches.end());

        std::vector<SctDocumentFooterEntry> leftFooters = baselineDocument.footerEntries;
        std::vector<SctDocumentFooterEntry> rightFooters = incomingDocument.footerEntries;
        std::unordered_set<std::uint64_t> usedLeftFooters, usedRightFooters;
        applyExplicitPairs<SctFooterEntryId>(decisions, decisionProvenance,
            pair.baseline->locator,
            pair.incoming->key, usedLeftFooters, usedRightFooters, matches);
        matchUniqueExact<SctFooterEntryId>(leftFooters, rightFooters,
            usedLeftFooters, usedRightFooters, matches, footerSignature);
        matchMutualUniqueStrong<SctFooterEntryId>(leftFooters, rightFooters,
            usedLeftFooters, usedRightFooters, matches,
            [](const auto& value) { return std::to_string(static_cast<int>(value.kind)); },
            "A mutually unique footer storage kind agrees.");

        std::unordered_set<std::uint64_t> usedLeftStrings, usedRightStrings;
        applyExplicitPairs<SctStringId>(decisions, decisionProvenance,
            pair.baseline->locator,
            pair.incoming->key, usedLeftStrings, usedRightStrings, matches);
        for (const auto& sectionMatch : matches) {
            const auto* sourceSection = std::get_if<SctSectionId>(&sectionMatch.baseline);
            const auto* targetSection = std::get_if<SctSectionId>(&sectionMatch.incoming);
            if (!sourceSection || !targetSection) continue;
            const auto* left = findById(baselineDocument.sections, *sourceSection);
            const auto* right = findById(incomingDocument.sections, *targetSection);
            if (!left || !right) continue;
            const auto* source = std::get_if<SctStringSectionContent>(&left->content);
            const auto* target = std::get_if<SctStringSectionContent>(&right->content);
            if (!source || !target || usedLeftStrings.contains(source->string.id.value())
                || usedRightStrings.contains(target->string.id.value())) continue;
            usedLeftStrings.insert(source->string.id.value());
            usedRightStrings.insert(target->string.id.value());
            const bool exact = sectionSignature(*left) == sectionSignature(*right);
            matches.push_back({source->string.id, target->string.id,
                exact ? SctIdentityMatchStatus::Exact : SctIdentityMatchStatus::Strong,
                SctIdentityMatchProvenance::Automatic,
                {{"mapped-owner", "The indexed strings belong to matched sections."}}});
        }

        notify(request, SctReconciliationProgressPhase::GraphRefinement,
            pairIndex, pairs.size(), &pair);
        if (request.stopToken.stop_requested())
            return Result<SctReconciliationResult>::success(cancelledResult());
        IdMaps maps;
        setMap<SctSectionId>(maps.sections, matches);
        setMap<SctInstructionId>(maps.instructions, matches);
        setMap<SctStringId>(maps.strings, matches);
        setMap<SctFooterEntryId>(maps.footers, matches);

        std::unordered_set<std::uint64_t> matchedOpaqueBaseline,
            matchedOpaqueIncoming;
        for (const auto& source : baselineDocument.opaqueAttachments) {
            for (const auto& target : incomingDocument.opaqueAttachments) {
                if (matchedOpaqueIncoming.contains(target.id.value())) continue;
                auto remappedTarget = target;
                remappedTarget.anchor = remapAnchor(remappedTarget.anchor, maps);
                if (source.bytes == remappedTarget.bytes
                    && source.anchor == remappedTarget.anchor
                    && source.placement == remappedTarget.placement
                    && source.fixedOffset == remappedTarget.fixedOffset
                    && source.alignment == remappedTarget.alignment
                    && source.relocation == remappedTarget.relocation
                    && source.reason == remappedTarget.reason) {
                    matchedOpaqueBaseline.insert(source.id.value());
                    matchedOpaqueIncoming.insert(target.id.value());
                    maps.opaque[target.id.value()] = source.id.value();
                    matches.push_back({source.id, target.id,
                        SctIdentityMatchStatus::Exact,
                        SctIdentityMatchProvenance::Automatic,
                        {{"exact-opaque", "Opaque bytes and mapped placement are exactly equal."}}});
                    break;
                }
            }
        }

        std::uint64_t nextSection = baselineDocument.nextSectionIdValue();
        std::uint64_t nextInstruction = baselineDocument.nextInstructionIdValue();
        std::uint64_t nextString = baselineDocument.nextStringIdValue();
        std::uint64_t nextFooter = baselineDocument.nextFooterEntryIdValue();
        std::uint64_t nextArm = 1;
        for (const auto& arm : pair.baseline->state.authoredArms)
            nextArm = std::max(nextArm, arm.id.value + 1u);
        for (const auto& section : incomingDocument.sections) {
            if (!maps.sections.contains(section.id.value()))
                maps.sections[section.id.value()] = nextSection++;
            if (const auto* script = std::get_if<SctScriptSectionContent>(&section.content))
                for (const auto& instruction : script->instructions)
                    if (!maps.instructions.contains(instruction.id.value()))
                        maps.instructions[instruction.id.value()] = nextInstruction++;
            else if (const auto* string = std::get_if<SctStringSectionContent>(&section.content))
                if (!maps.strings.contains(string->string.id.value()))
                    maps.strings[string->string.id.value()] = nextString++;
        }
        for (const auto& footer : incomingDocument.footerEntries)
            if (!maps.footers.contains(footer.id.value()))
                maps.footers[footer.id.value()] = nextFooter++;

        std::unordered_set<std::uint64_t> usedBaselineArms, usedIncomingArms;
        for (const auto& decision : decisions) {
            if (decision.kind != SctReconciliationDecisionKind::PairEntity
                || !decision.baselineAsset
                || *decision.baselineAsset != pair.baseline->locator
                || !decision.incomingAssetKey
                || *decision.incomingAssetKey != pair.incoming->key) continue;
            const auto source = entityAs<SctAuthoredArmId>(decision.baselineEntity);
            const auto target = entityAs<SctAuthoredArmId>(decision.incomingEntity);
            if (!source || !target
                || !usedBaselineArms.insert(source->value).second
                || !usedIncomingArms.insert(target->value).second) continue;
            maps.arms[target->value] = source->value;
            matches.push_back({*source, *target, SctIdentityMatchStatus::Strong,
                decisionProvenance.at(decision.id),
                {{"explicit-decision",
                    "The user explicitly paired these authored arms."}}});
        }
        const auto compatibleArm = [&](const SctAuthoredArm& source,
                                       const SctAuthoredArm& target) {
            const auto remappedTarget = remapArm(target, maps);
            return source.controller == remappedTarget.controller
                && source.kind == remappedTarget.kind
                && source.caseValue == remappedTarget.caseValue;
        };
        for (const auto& source : pair.baseline->state.authoredArms) {
            if (usedBaselineArms.contains(source.id.value)) continue;
            std::vector<const SctAuthoredArm*> candidates;
            for (const auto& target : pair.incoming->state.authoredArms)
                if (!usedIncomingArms.contains(target.id.value)
                    && compatibleArm(source, target)) candidates.push_back(&target);
            if (candidates.size() != 1u) continue;
            const auto* target = candidates.front();
            std::size_t reverse = 0;
            for (const auto& alternative : pair.baseline->state.authoredArms)
                if (!usedBaselineArms.contains(alternative.id.value)
                    && compatibleArm(alternative, *target)) ++reverse;
            if (reverse != 1u) continue;
            usedBaselineArms.insert(source.id.value);
            usedIncomingArms.insert(target->id.value);
            maps.arms[target->id.value] = source.id.value;
            auto remappedTarget = remapArm(*target, maps);
            matches.push_back({source.id, target->id,
                source == remappedTarget ? SctIdentityMatchStatus::Exact
                                         : SctIdentityMatchStatus::Strong,
                SctIdentityMatchProvenance::Automatic,
                {{"mapped-controller",
                    "The authored arms have the same mapped controller and role."}}});
        }
        for (const auto& arm : pair.incoming->state.authoredArms)
            if (!maps.arms.contains(arm.id.value)) maps.arms[arm.id.value] = nextArm++;

        std::unordered_set<std::string> preMatchedBaselineKeys,
            preMatchedIncomingKeys, ambiguousBaselineKeys, ambiguousIncomingKeys;
        for (const auto& match : matches) {
            preMatchedBaselineKeys.insert(entityKey(match.baseline));
            preMatchedIncomingKeys.insert(entityKey(match.incoming));
        }
        const auto markAmbiguous = [&](const auto baselineId,
                                       const auto incomingId) {
            ambiguousBaselineKeys.insert(entityKey(
                SctReconciliationEntityId{baselineId}));
            ambiguousIncomingKeys.insert(entityKey(
                SctReconciliationEntityId{incomingId}));
        };
        for (const auto& source : baselineDocument.sections) {
            if (preMatchedBaselineKeys.contains(entityKey(
                    SctReconciliationEntityId{source.id}))) continue;
            for (const auto& target : incomingDocument.sections) {
                if (preMatchedIncomingKeys.contains(entityKey(
                        SctReconciliationEntityId{target.id}))) continue;
                if (source.content.index() == target.content.index())
                    markAmbiguous(source.id, target.id);
            }
        }
        for (const auto& sectionMatch : matches) {
            const auto* baselineSectionId =
                std::get_if<SctSectionId>(&sectionMatch.baseline);
            const auto* incomingSectionId =
                std::get_if<SctSectionId>(&sectionMatch.incoming);
            if (!baselineSectionId || !incomingSectionId) continue;
            const auto* baselineSection = findById(
                baselineDocument.sections, *baselineSectionId);
            const auto* incomingSection = findById(
                incomingDocument.sections, *incomingSectionId);
            if (!baselineSection || !incomingSection) continue;
            const auto baselineInstructions = instructions(*baselineSection);
            const auto incomingInstructions = instructions(*incomingSection);
            for (const auto& source : baselineInstructions) {
                if (preMatchedBaselineKeys.contains(entityKey(
                        SctReconciliationEntityId{source.id}))) continue;
                for (const auto& target : incomingInstructions) {
                    if (preMatchedIncomingKeys.contains(entityKey(
                            SctReconciliationEntityId{target.id}))) continue;
                    if (instructionShape(source) == instructionShape(target))
                        markAmbiguous(source.id, target.id);
                }
            }
        }
        for (const auto& source : baselineDocument.footerEntries) {
            if (preMatchedBaselineKeys.contains(entityKey(
                    SctReconciliationEntityId{source.id}))) continue;
            for (const auto& target : incomingDocument.footerEntries) {
                if (preMatchedIncomingKeys.contains(entityKey(
                        SctReconciliationEntityId{target.id}))) continue;
                if (source.kind == target.kind) markAmbiguous(source.id, target.id);
            }
        }

        notify(request, SctReconciliationProgressPhase::CandidateConstruction,
            pairIndex, pairs.size(), &pair);
        if (request.stopToken.stop_requested())
            return Result<SctReconciliationResult>::success(cancelledResult());
        auto candidateDocument = std::make_shared<SctDocument>(incomingDocument);
        remapDocument(*candidateDocument, maps);
        candidateDocument->opaqueAttachments = baselineDocument.opaqueAttachments;

        std::unordered_set<std::uint64_t> matchedBaselineSections;
        for (const auto& match : matches)
            if (const auto* value = std::get_if<SctSectionId>(&match.baseline))
                matchedBaselineSections.insert(value->value());
        for (auto& projectedSection : candidateDocument->sections) {
            const auto sourceMatch = std::ranges::find_if(matches, [&](const auto& value) {
                const auto* source = std::get_if<SctSectionId>(&value.baseline);
                const auto* target = std::get_if<SctSectionId>(&value.incoming);
                return source && target && source->value() == projectedSection.id.value();
            });
            if (sourceMatch == matches.end()) continue;
            const auto sourceId = std::get<SctSectionId>(sourceMatch->baseline);
            const auto* baselineSection = findById(baselineDocument.sections, sourceId);
            if (!baselineSection) continue;
            if (baselineSection->content.index() != projectedSection.content.index()) {
                projectedSection = *baselineSection;
                sourceMatch->status = SctIdentityMatchStatus::Contradictory;
                sourceMatch->representable = false;
                continue;
            }
            auto* projectedScript = std::get_if<SctScriptSectionContent>(
                &projectedSection.content);
            const auto* baselineScript = std::get_if<SctScriptSectionContent>(
                &baselineSection->content);
            if (projectedScript && baselineScript) {
                std::unordered_set<std::uint64_t> matchedBaselineInstructions;
                for (const auto& match : matches) {
                    const auto* value = std::get_if<SctInstructionId>(&match.baseline);
                    if (!value) continue;
                    SctSectionId owner;
                    if (findInstruction(baselineDocument, *value, &owner)
                        && owner == sourceId)
                        matchedBaselineInstructions.insert(value->value());
                }
                mergeUnmatchedBaseline<SctDocumentInstruction, SctInstructionId>(
                    projectedScript->instructions, baselineScript->instructions,
                    matchedBaselineInstructions, decisions, decisionProvenance,
                    pair.baseline->locator,
                    pair.incoming->key);
            }
        }
        mergeUnmatchedBaseline<SctDocumentSection, SctSectionId>(
            candidateDocument->sections, baselineDocument.sections,
            matchedBaselineSections, decisions, decisionProvenance,
            pair.baseline->locator,
            pair.incoming->key);
        std::unordered_set<std::uint64_t> matchedBaselineFooters;
        for (const auto& match : matches)
            if (const auto* value = std::get_if<SctFooterEntryId>(&match.baseline))
                matchedBaselineFooters.insert(value->value());
        mergeUnmatchedBaseline<SctDocumentFooterEntry, SctFooterEntryId>(
            candidateDocument->footerEntries, baselineDocument.footerEntries,
            matchedBaselineFooters, decisions, decisionProvenance,
            pair.baseline->locator,
            pair.incoming->key);
        auto rebuilt = SctDocumentBuilder::reconstitute(*candidateDocument);
        if (!rebuilt.document)
            return Result<SctReconciliationResult>::failure(error(
                "The reconciled candidate has invalid identity state."));
        candidateDocument = std::make_shared<SctDocument>(
            std::move(*rebuilt.document));
        while (candidateDocument->nextSectionIdValue()
            < baselineDocument.nextSectionIdValue()) (void)candidateDocument->allocateSectionId();
        while (candidateDocument->nextInstructionIdValue()
            < baselineDocument.nextInstructionIdValue()) (void)candidateDocument->allocateInstructionId();
        while (candidateDocument->nextStringIdValue()
            < baselineDocument.nextStringIdValue()) (void)candidateDocument->allocateStringId();
        while (candidateDocument->nextFooterEntryIdValue()
            < baselineDocument.nextFooterEntryIdValue()) (void)candidateDocument->allocateFooterEntryId();
        while (candidateDocument->nextOpaqueAttachmentIdValue()
            < baselineDocument.nextOpaqueAttachmentIdValue()) (void)candidateDocument->allocateOpaqueAttachmentId();

        SctSemanticState candidate{candidateDocument};
        std::unordered_set<std::uint64_t> matchedBaselineArms;
        for (const auto& arm : pair.incoming->state.authoredArms) {
            auto projected = remapArm(arm, maps);
            if (const auto found = maps.arms.find(arm.id.value); found != maps.arms.end())
                if (findArm(pair.baseline->state.authoredArms,
                        SctAuthoredArmId{found->second}))
                    matchedBaselineArms.insert(found->second);
            candidate.authoredArms.push_back(std::move(projected));
        }
        for (const auto& arm : pair.baseline->state.authoredArms)
            if (!matchedBaselineArms.contains(arm.id.value)
                && !removed(decisions, decisionProvenance,
                    pair.baseline->locator, pair.incoming->key, arm.id))
                candidate.authoredArms.push_back(arm);
        std::ranges::sort(candidate.authoredArms, {}, [](const auto& value) {
            return value.id.value;
        });
        for (auto repair : pair.incoming->state.textRepairs) {
            repair.target = remapTextTarget(repair.target, maps);
            candidate.textRepairs.push_back(std::move(repair));
        }
        for (const auto& repair : pair.baseline->state.textRepairs) {
            const auto exists = std::ranges::find(candidate.textRepairs,
                repair.target, &SctTextRepairRecord::target);
            if (exists == candidate.textRepairs.end()) candidate.textRepairs.push_back(repair);
        }
        for (auto origin : pair.incoming->state.unboundReferences)
            candidate.unboundReferences.push_back(
                remapOriginSite(std::move(origin), maps));
        for (const auto& origin : pair.baseline->state.unboundReferences) {
            const auto exists = std::ranges::find(candidate.unboundReferences,
                origin.site, &SctUnboundReferenceOrigin::site);
            if (exists == candidate.unboundReferences.end())
                candidate.unboundReferences.push_back(origin);
        }
        std::ranges::sort(candidate.unboundReferences, {},
            &SctUnboundReferenceOrigin::site);

        SctReconciledScript script{pair.baseline->locator, pair.incoming->key};
        script.candidate = candidate;
        const auto validation = SctDocumentValidator::validateDocument(
            *candidate.document);
        if (!validation.validDocument)
            script.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                "CandidateInvalid",
                "The reconciled candidate does not satisfy SCT document invariants."});
        auto matchedBaselineKeys = preMatchedBaselineKeys;
        auto matchedIncomingKeys = preMatchedIncomingKeys;
        for (const auto& match : matches) {
            const auto baselineKey = entityKey(match.baseline);
            const auto incomingKey = entityKey(match.incoming);
            matchedBaselineKeys.insert(baselineKey);
            matchedIncomingKeys.insert(incomingKey);
            SctReconciliationEntityId candidateId = match.baseline;
            script.entities.push_back({match.baseline, match.incoming, candidateId,
                match.status, match.provenance, match.evidence, true, false,
                match.representable});
            annotations[pair.baseline->locator.identityKey()][entityKey(candidateId)] =
                script.entities.back();
        }
        const auto addUnmatched = [&](const SctReconciliationEntityId& item,
                                      const SctReconciliationEntityId& projected) {
            const auto confirmation = std::visit([&](const auto id) {
                return confirmedAddition(decisions, decisionProvenance,
                    pair.baseline->locator, pair.incoming->key, id);
            }, item);
            const bool ambiguous = ambiguousIncomingKeys.contains(entityKey(item));
            script.entities.push_back({std::nullopt, item, projected,
                ambiguous ? SctIdentityMatchStatus::Ambiguous
                          : SctIdentityMatchStatus::Unmatched,
                confirmation.value_or(SctIdentityMatchProvenance::Automatic),
                {{confirmation ? "explicit-addition"
                               : (ambiguous ? "ambiguous-candidates"
                                            : "provisional-addition"),
                    confirmation ? "The user confirmed this incoming entity as new."
                        : (ambiguous ? "Multiple correspondence possibilities remain."
                                     : "No unique correspondence was proven.")}},
                confirmation.has_value(), !confirmation.has_value(), true});
            annotations[pair.baseline->locator.identityKey()][entityKey(projected)] =
                script.entities.back();
        };
        const auto addUnmatchedBaseline = [&](const SctReconciliationEntityId& item) {
            SctIdentityMatchProvenance provenance{};
            const bool confirmed = hasDecision(decisions, decisionProvenance,
                SctReconciliationDecisionKind::BaselineEntityRemoval,
                pair.baseline->locator, pair.incoming->key, item, std::nullopt,
                &provenance);
            script.entities.push_back({item, std::nullopt,
                confirmed ? std::nullopt
                          : std::optional<SctReconciliationEntityId>{item},
                ambiguousBaselineKeys.contains(entityKey(item))
                    ? SctIdentityMatchStatus::Ambiguous
                    : SctIdentityMatchStatus::Unmatched,
                confirmed ? provenance : SctIdentityMatchProvenance::Automatic,
                {{confirmed ? "explicit-removal" : "unresolved-baseline",
                    confirmed ? "The user confirmed removal of this baseline entity."
                              : "No unique incoming correspondence was proven."}},
                confirmed, false, true});
            annotations[pair.baseline->locator.identityKey()][entityKey(item)] =
                script.entities.back();
        };
        for (const auto& section : incomingDocument.sections) {
            if (!matchedIncomingKeys.contains(entityKey(SctReconciliationEntityId{section.id})))
                addUnmatched(section.id, SctSectionId{maps.sections.at(section.id.value())});
            if (const auto* scriptValue = std::get_if<SctScriptSectionContent>(&section.content))
                for (const auto& instruction : scriptValue->instructions)
                    if (!matchedIncomingKeys.contains(entityKey(
                            SctReconciliationEntityId{instruction.id})))
                        addUnmatched(instruction.id, SctInstructionId{
                            maps.instructions.at(instruction.id.value())});
            if (const auto* stringValue = std::get_if<SctStringSectionContent>(&section.content))
                if (!matchedIncomingKeys.contains(entityKey(
                        SctReconciliationEntityId{stringValue->string.id})))
                    addUnmatched(stringValue->string.id, SctStringId{
                        maps.strings.at(stringValue->string.id.value())});
        }
        for (const auto& footer : incomingDocument.footerEntries)
            if (!matchedIncomingKeys.contains(entityKey(SctReconciliationEntityId{footer.id})))
                addUnmatched(footer.id, SctFooterEntryId{maps.footers.at(footer.id.value())});
        for (const auto& arm : pair.incoming->state.authoredArms)
            if (!matchedIncomingKeys.contains(entityKey(
                    SctReconciliationEntityId{arm.id})))
                addUnmatched(arm.id, SctAuthoredArmId{maps.arms.at(arm.id.value)});
        for (const auto& opaque : incomingDocument.opaqueAttachments)
            if (!matchedIncomingKeys.contains(entityKey(SctReconciliationEntityId{opaque.id}))) {
                script.entities.push_back({std::nullopt, opaque.id, std::nullopt,
                    SctIdentityMatchStatus::Contradictory,
                    SctIdentityMatchProvenance::Automatic,
                    {{"opaque-mismatch", "Opaque content lacks exact authoritative equivalence."}},
                    false, false, false});
                script.diagnostics.push_back({SctChangeDisposition::Conflict,
                    "OpaqueEquivalenceUnavailable",
                    "Non-identical opaque content cannot be reconciled."});
            }
        for (const auto& source : baselineDocument.sections) {
            if (!matchedBaselineKeys.contains(entityKey(SctReconciliationEntityId{source.id})))
                addUnmatchedBaseline(source.id);
            if (const auto* scriptValue = std::get_if<SctScriptSectionContent>(&source.content))
                for (const auto& instruction : scriptValue->instructions)
                    if (!matchedBaselineKeys.contains(entityKey(
                            SctReconciliationEntityId{instruction.id})))
                        addUnmatchedBaseline(instruction.id);
            if (const auto* stringValue = std::get_if<SctStringSectionContent>(
                    &source.content))
                if (!matchedBaselineKeys.contains(entityKey(
                        SctReconciliationEntityId{stringValue->string.id})))
                    addUnmatchedBaseline(stringValue->string.id);
        }
        for (const auto& footer : baselineDocument.footerEntries)
            if (!matchedBaselineKeys.contains(entityKey(SctReconciliationEntityId{footer.id})))
                addUnmatchedBaseline(footer.id);
        for (const auto& arm : pair.baseline->state.authoredArms)
            if (!matchedBaselineKeys.contains(entityKey(
                    SctReconciliationEntityId{arm.id})))
                addUnmatchedBaseline(arm.id);
        for (const auto& opaque : baselineDocument.opaqueAttachments)
            if (!matchedBaselineKeys.contains(entityKey(
                    SctReconciliationEntityId{opaque.id})))
                script.entities.push_back({opaque.id, std::nullopt, opaque.id,
                    SctIdentityMatchStatus::Contradictory,
                    SctIdentityMatchProvenance::Automatic,
                    {{"opaque-mismatch",
                        "No incoming opaque attachment has exact mapped equivalence."}},
                    false, false, false});
        std::ranges::sort(script.entities, {}, [](const auto& value) {
            return value.candidate ? entityKey(*value.candidate)
                : (value.baseline ? entityKey(*value.baseline)
                                  : entityKey(*value.incoming));
        });
        identity << '|' << pair.baseline->locator.identityKey() << ':'
            << pair.baseline->revision.digest.toHex() << ':'
            << incomingFingerprints.at(pair.incoming->key);
        for (const auto& entity : script.entities)
            identity << ':' << static_cast<int>(entity.status) << ':'
                << static_cast<int>(entity.provenance) << ':'
                << entity.resolved << ':' << entity.provisional << ':'
                << entity.representable << ':'
                << (entity.baseline ? entityKey(*entity.baseline) : "none") << ':'
                << (entity.incoming ? entityKey(*entity.incoming) : "none") << ':'
                << (entity.candidate ? entityKey(*entity.candidate) : "none");
        comparisons.push_back({pair.baseline->locator, pair.baseline->revision,
            pair.baseline->state, candidate, std::nullopt});
        result.scripts.push_back(std::move(script));
    }

    if (request.stopToken.stop_requested())
        return Result<SctReconciliationResult>::success(cancelledResult());
    result.id = digestText(identity.str());
    notify(request, SctReconciliationProgressPhase::ChangePlan,
        pairs.size(), pairs.size());
    if (request.stopToken.stop_requested())
        return Result<SctReconciliationResult>::success(cancelledResult());
    if (!comparisons.empty()) {
        auto plan = SctChangePlanService::build(comparisons);
        if (!plan) return Result<SctReconciliationResult>::failure(plan.diagnostics());
        for (auto& script : plan.value().scripts) {
            const auto found = annotations.find(script.locator.identityKey());
            if (found == annotations.end()) continue;
            for (auto& unit : script.units) {
                const auto annotation = found->second.find(unit.entityKey);
                if (annotation == found->second.end()) continue;
                const auto& correspondence = annotation->second;
                unit.identity = SctChangeIdentity{correspondence.status,
                    correspondence.provenance,
                    correspondence.baseline
                        ? navigation(*correspondence.baseline) : std::nullopt,
                    correspondence.incoming
                        ? navigation(*correspondence.incoming) : std::nullopt,
                    correspondence.evidence};
                if (correspondence.provisional || !correspondence.representable) {
                    unit.disposition = correspondence.representable
                        ? SctChangeDisposition::Conflict
                        : SctChangeDisposition::InvalidResult;
                    unit.selectable = false;
                } else if (correspondence.status
                    == SctIdentityMatchStatus::Contradictory) {
                    unit.disposition = SctChangeDisposition::Warning;
                }
            }
        }
        plan.value().id = digestText(plan.value().id + '|' + result.id);
        result.changePlan = std::move(plan).takeValue();
    }

    SctReconciliationDecisionArtifact bound;
    bound.decisionScopeId = request.decisionScopeId;
    bound.targetScopeKey = request.targetScopeKey;
    bound.reconciliationContractVersion = ContractVersion;
    for (const auto& decision : decisions) {
        auto found = std::ranges::find_if(bound.assets, [&](const auto& value) {
            return value.baselineAsset == decision.baselineAsset
                && value.incomingAssetKey == decision.incomingAssetKey;
        });
        if (found == bound.assets.end()) {
            SctReconciliationDecisionBinding binding;
            binding.baselineAsset = decision.baselineAsset;
            binding.incomingAssetKey = decision.incomingAssetKey;
            if (decision.baselineAsset) {
                const auto source = baselines.find(decision.baselineAsset->identityKey());
                if (source != baselines.end()) binding.baselineRevision = source->second->revision;
            }
            if (decision.incomingAssetKey) {
                const auto source = incomingFingerprints.find(*decision.incomingAssetKey);
                if (source != incomingFingerprints.end())
                    binding.incomingFingerprint = source->second;
            }
            bound.assets.push_back(std::move(binding));
            found = std::prev(bound.assets.end());
        }
        found->decisions.push_back(decision);
    }
    result.boundDecisions = std::move(bound);
    return Result<SctReconciliationResult>::success(std::move(result));
}

}  // namespace salsa::core
