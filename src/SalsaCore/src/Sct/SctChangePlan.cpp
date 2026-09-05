#include "SalsaCore/Sct/SctChangePlan.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SpiceSCT/SctDocumentValidator.h"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic planError(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctPatch,
        std::move(message), std::nullopt};
}

[[nodiscard]] Result<std::string> digestText(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    auto digest = sha256(bytes);
    if (!digest) return Result<std::string>::failure(digest.diagnostics());
    return Result<std::string>::success(digest.value().toHex());
}

[[nodiscard]] std::string textKey(const SctTextTarget& target) {
    return std::visit([](const auto id) {
        using T = std::decay_t<decltype(id)>;
        return std::string(std::is_same_v<T, spice::sct::SctStringId>
            ? "string:" : "footer:") + std::to_string(id.value());
    }, target);
}

template<typename T>
[[nodiscard]] std::string idKey(const char* prefix, const T id) {
    return std::string(prefix) + ':' + std::to_string(id.value());
}

[[nodiscard]] std::string armKey(const SctAuthoredArmId id) {
    return "arm:" + std::to_string(id.value);
}

[[nodiscard]] std::string repairKey(const SctTextTarget& target) {
    return "repair:" + textKey(target);
}

[[nodiscard]] std::string unboundKey(const spice::sct::SctParameterSite& site) {
    return "unbound:" + std::to_string(site.instruction.value()) + ':'
        + std::to_string(site.parameter.schemaIndex) + ':'
        + (site.parameter.repeatedGroupOrdinal
            ? std::to_string(*site.parameter.repeatedGroupOrdinal) : "fixed");
}

[[nodiscard]] std::string aliasKey(const SctVariableKey& key) {
    return "alias:" + std::to_string(static_cast<unsigned>(key.kind)) + ':'
        + std::to_string(key.index);
}

[[nodiscard]] std::string annotationKey(const SctAuthoringTarget& target) {
    return "annotation:" + std::to_string(static_cast<unsigned>(target.kind)) + ':'
        + std::to_string(target.id) + ':'
        + (target.variableKind ? std::to_string(static_cast<unsigned>(*target.variableKind)) : "-");
}

[[nodiscard]] std::string folderKey(const SctSectionFolderId id) {
    return "folder:" + std::to_string(id.value);
}

[[nodiscard]] bool sameOpaque(const spice::sct::SctOpaqueAttachment& left,
    const spice::sct::SctOpaqueAttachment& right) {
    return left.id == right.id && left.bytes == right.bytes
        && left.anchor == right.anchor && left.placement == right.placement
        && left.fixedOffset == right.fixedOffset && left.alignment == right.alignment
        && left.relocation == right.relocation && left.reason == right.reason;
}

[[nodiscard]] bool opaqueChanged(const spice::sct::SctDocument& before,
    const spice::sct::SctDocument& after) {
    return before.opaqueAttachments.size() != after.opaqueAttachments.size()
        || !std::ranges::equal(before.opaqueAttachments, after.opaqueAttachments,
            sameOpaque);
}

[[nodiscard]] std::string dispositionName(const SctChangeDisposition value) {
    switch (value) {
    case SctChangeDisposition::Informational: return "information";
    case SctChangeDisposition::Warning: return "warning";
    case SctChangeDisposition::Conflict: return "conflict";
    case SctChangeDisposition::InvalidResult: return "invalid result";
    }
    return "unknown";
}

template<typename Id>
[[nodiscard]] std::optional<Id> predecessor(const std::vector<Id>& order,
    const Id id) {
    const auto found = std::ranges::find(order, id);
    if (found == order.end() || found == order.begin()) return std::nullopt;
    return *std::prev(found);
}

template<typename Id>
[[nodiscard]] std::unordered_set<Id> orderAffected(
    const std::vector<Id>& before, const std::vector<Id>& after) {
    std::unordered_set<Id> result;
    std::unordered_set<Id> all(before.begin(), before.end());
    all.insert(after.begin(), after.end());
    for (const auto id : all) {
        const bool beforePresent = std::ranges::find(before, id) != before.end();
        const bool afterPresent = std::ranges::find(after, id) != after.end();
        if (beforePresent != afterPresent
            || (beforePresent && predecessor(before, id) != predecessor(after, id)))
            result.insert(id);
    }
    return result;
}

template<typename Id>
[[nodiscard]] std::vector<Id> projectOrder(const std::vector<Id>& current,
    const std::vector<Id>& desired, const std::unordered_set<Id>& selected) {
    std::vector<Id> result;
    result.reserve(current.size() + selected.size());
    for (const auto id : current)
        if (!selected.contains(id)) result.push_back(id);
    for (std::size_t index = 0; index < desired.size(); ++index) {
        if (!selected.contains(desired[index])) continue;
        auto insertion = result.end();
        for (auto next = index + 1; next < desired.size(); ++next) {
            const auto anchor = std::ranges::find(result, desired[next]);
            if (anchor != result.end()) {
                insertion = anchor;
                break;
            }
        }
        result.insert(insertion, desired[index]);
    }
    return result;
}

template<typename T, typename Id>
[[nodiscard]] const T* findById(const std::vector<T>& values, const Id id) {
    const auto found = std::ranges::find(values, id, &T::id);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] const SctScriptChangePlan* findScript(const SctChangePlan& plan,
    const std::string& identity) {
    const auto found = std::ranges::find_if(plan.scripts, [&](const auto& script) {
        return script.locator.identityKey() == identity;
    });
    return found == plan.scripts.end() ? nullptr : &*found;
}

[[nodiscard]] const SctSemanticState* findCurrent(
    const std::span<const SctCurrentScriptState> current,
    const std::string& identity) {
    const auto found = std::ranges::find_if(current, [&](const auto& script) {
        return script.locator.identityKey() == identity;
    });
    return found == current.end() ? nullptr : &found->state;
}

[[nodiscard]] std::vector<spice::sct::SctSectionId> sectionOrder(
    const spice::sct::SctDocument& document) {
    std::vector<spice::sct::SctSectionId> result;
    for (const auto& section : document.sections) result.push_back(section.id);
    return result;
}

[[nodiscard]] std::vector<spice::sct::SctFooterEntryId> footerOrder(
    const spice::sct::SctDocument& document) {
    std::vector<spice::sct::SctFooterEntryId> result;
    for (const auto& entry : document.footerEntries) result.push_back(entry.id);
    return result;
}

[[nodiscard]] std::vector<spice::sct::SctInstructionId> instructionOrder(
    const spice::sct::SctDocument& document, const spice::sct::SctSectionId section) {
    const auto* value = findById(document.sections, section);
    if (!value) return {};
    const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&value->content);
    if (!script) return {};
    std::vector<spice::sct::SctInstructionId> result;
    for (const auto& instruction : script->instructions) result.push_back(instruction.id);
    return result;
}

[[nodiscard]] SctPatchedAllocatorState allocatorState(
    const spice::sct::SctDocument& document) {
    return {document.nextSectionIdValue(), document.nextInstructionIdValue(),
        document.nextStringIdValue(), document.nextFooterEntryIdValue(),
        document.nextOpaqueAttachmentIdValue()};
}

[[nodiscard]] SctPatchedAllocatorState mergeAllocatorState(
    const SctPatchedAllocatorState& current,
    const SctPatchedAllocatorState& proposed) {
    return {
        std::max(current.nextSectionId, proposed.nextSectionId),
        std::max(current.nextInstructionId, proposed.nextInstructionId),
        std::max(current.nextStringId, proposed.nextStringId),
        std::max(current.nextFooterEntryId, proposed.nextFooterEntryId),
        std::max(current.nextOpaqueAttachmentId, proposed.nextOpaqueAttachmentId),
    };
}

void addOrMergeUnit(SctScriptChangePlan& script,
    std::unordered_map<std::string, std::size_t>& byKey,
    SctChangeUnit unit, const std::string_view scriptIdentity) {
    if (const auto found = byKey.find(unit.entityKey); found != byKey.end()) {
        auto& existing = script.units[found->second];
        existing.details.insert(existing.details.end(), unit.details.begin(), unit.details.end());
        existing.affectsOrder = existing.affectsOrder || unit.affectsOrder;
        return;
    }
    const auto digest = digestText(std::string(scriptIdentity) + '|' + unit.entityKey);
    unit.id = digest ? digest.value() : std::string(scriptIdentity) + '|' + unit.entityKey;
    byKey.emplace(unit.entityKey, script.units.size());
    script.units.push_back(std::move(unit));
}

[[nodiscard]] std::unordered_set<std::string> changedKeys(
    const SalsaScriptPatch& patch) {
    std::unordered_set<std::string> result;
    for (const auto& section : patch.sections) {
        const auto id = section.before ? section.before->id : section.after->id;
        result.insert(idKey("section", id));
    }
    for (const auto& section : patch.scriptSections) {
        if (section.nameBytes) result.insert(idKey("section", section.section));
        for (const auto& instruction : section.instructions) {
            const auto id = instruction.before ? instruction.before->id : instruction.after->id;
            result.insert(idKey("instruction", id));
        }
        if (section.instructionOrder)
            for (const auto id : orderAffected(section.instructionOrder->before,
                    section.instructionOrder->after))
                result.insert(idKey("instruction", id));
    }
    for (const auto& value : patch.textValues) result.insert(textKey(value.target));
    for (const auto& entry : patch.footerEntries) {
        const auto id = entry.before ? entry.before->id : entry.after->id;
        result.insert(idKey("footer", id));
    }
    for (const auto& arm : patch.authoredArms) {
        const auto id = arm.before ? arm.before->id : arm.after->id;
        result.insert(armKey(id));
    }
    for (const auto& repair : patch.textRepairs) result.insert(repairKey(repair.target));
    for (const auto& origin : patch.unboundReferences)
        result.insert(unboundKey(origin.site));
    for (const auto& value : patch.aliases)
        result.insert(aliasKey((value.before ? value.before : value.after)->variable));
    for (const auto& value : patch.annotations)
        result.insert(annotationKey((value.before ? value.before : value.after)->target));
    for (const auto& value : patch.folders)
        result.insert(folderKey((value.before ? value.before : value.after)->id));
    if (patch.sectionOrder)
        for (const auto id : orderAffected(patch.sectionOrder->before, patch.sectionOrder->after))
            result.insert(idKey("section", id));
    if (patch.footerOrder)
        for (const auto id : orderAffected(patch.footerOrder->before, patch.footerOrder->after))
            result.insert(idKey("footer", id));
    return result;
}

[[nodiscard]] SalsaScriptPatch selectedPatch(const SctScriptChangePlan& plan,
    const SctSemanticState& current, const std::unordered_set<std::string>& selectedKeys,
    const std::unordered_set<std::string>& selectedOrderKeys) {
    SalsaScriptPatch result;
    result.sourceTextConvention = plan.completePatch.sourceTextConvention;
    if (plan.completePatch.allocatorState && current.document) {
        const auto before = allocatorState(*current.document);
        const auto after = mergeAllocatorState(before,
            *plan.completePatch.allocatorState->after);
        if (before != after)
            result.allocatorState = SctValueDelta<SctPatchedAllocatorState>{before, after};
    }
    for (const auto& value : plan.completePatch.sections) {
        const auto id = value.before ? value.before->id : value.after->id;
        if (selectedKeys.contains(idKey("section", id))) result.sections.push_back(value);
    }
    for (const auto& section : plan.completePatch.scriptSections) {
        SctPatchedScriptSection filtered;
        filtered.section = section.section;
        if (section.nameBytes && selectedKeys.contains(idKey("section", section.section)))
            filtered.nameBytes = section.nameBytes;
        for (const auto& instruction : section.instructions) {
            const auto id = instruction.before ? instruction.before->id : instruction.after->id;
            if (selectedKeys.contains(idKey("instruction", id)))
                filtered.instructions.push_back(instruction);
        }
        if (section.instructionOrder && current.document) {
            std::unordered_set<spice::sct::SctInstructionId> selected;
            for (const auto id : orderAffected(section.instructionOrder->before,
                    section.instructionOrder->after))
                if (selectedOrderKeys.contains(idKey("instruction", id))) selected.insert(id);
            if (!selected.empty()) {
                const auto before = instructionOrder(*current.document, section.section);
                const auto after = projectOrder(before, section.instructionOrder->after, selected);
                if (before != after) filtered.instructionOrder =
                    SctOrderDelta<spice::sct::SctInstructionId>{before, after};
            }
        }
        if (filtered.nameBytes || filtered.instructionOrder || !filtered.instructions.empty())
            result.scriptSections.push_back(std::move(filtered));
    }
    for (const auto& value : plan.completePatch.textValues)
        if (selectedKeys.contains(textKey(value.target))) result.textValues.push_back(value);
    for (const auto& value : plan.completePatch.footerEntries) {
        const auto id = value.before ? value.before->id : value.after->id;
        if (selectedKeys.contains(idKey("footer", id))) result.footerEntries.push_back(value);
    }
    for (const auto& value : plan.completePatch.authoredArms) {
        const auto id = value.before ? value.before->id : value.after->id;
        if (selectedKeys.contains(armKey(id))) result.authoredArms.push_back(value);
    }
    for (const auto& value : plan.completePatch.textRepairs)
        if (selectedKeys.contains(repairKey(value.target))) result.textRepairs.push_back(value);
    for (const auto& value : plan.completePatch.unboundReferences)
        if (selectedKeys.contains(unboundKey(value.site)))
            result.unboundReferences.push_back(value);
    for (const auto& value : plan.completePatch.aliases)
        if (selectedKeys.contains(aliasKey((value.before ? value.before : value.after)->variable)))
            result.aliases.push_back(value);
    for (const auto& value : plan.completePatch.annotations)
        if (selectedKeys.contains(annotationKey((value.before ? value.before : value.after)->target)))
            result.annotations.push_back(value);
    for (const auto& value : plan.completePatch.folders)
        if (selectedKeys.contains(folderKey((value.before ? value.before : value.after)->id)))
            result.folders.push_back(value);

    if (plan.completePatch.sectionOrder && current.document) {
        std::unordered_set<spice::sct::SctSectionId> selected;
        for (const auto id : orderAffected(plan.completePatch.sectionOrder->before,
                plan.completePatch.sectionOrder->after))
            if (selectedOrderKeys.contains(idKey("section", id))) selected.insert(id);
        if (!selected.empty()) {
            const auto before = sectionOrder(*current.document);
            const auto after = projectOrder(before, plan.completePatch.sectionOrder->after, selected);
            if (before != after) result.sectionOrder =
                SctOrderDelta<spice::sct::SctSectionId>{before, after};
        }
    }
    if (plan.completePatch.footerOrder && current.document) {
        std::unordered_set<spice::sct::SctFooterEntryId> selected;
        for (const auto id : orderAffected(plan.completePatch.footerOrder->before,
                plan.completePatch.footerOrder->after))
            if (selectedOrderKeys.contains(idKey("footer", id))) selected.insert(id);
        if (!selected.empty()) {
            const auto before = footerOrder(*current.document);
            const auto after = projectOrder(before, plan.completePatch.footerOrder->after, selected);
            if (before != after) result.footerOrder =
                SctOrderDelta<spice::sct::SctFooterEntryId>{before, after};
        }
    }
    return result;
}

}  // namespace

Result<SctChangePlan> SctChangePlanService::build(
    const std::span<const SctScriptComparisonInput> inputs) {
    SctChangePlan plan;
    std::unordered_set<std::string> identities;
    for (const auto& input : inputs) {
        const auto identity = input.locator.identityKey();
        if (!identities.insert(identity).second)
            return Result<SctChangePlan>::failure(planError(
                "A change plan cannot contain the same script more than once."));
        if (!input.before.document || !input.after.document)
            return Result<SctChangePlan>::failure(planError(
                "Every compared script requires complete before and after states."));

        SctScriptChangePlan script{"", input.locator, input.baselineRevision};
        script.before = input.before;
        script.after = input.after;
        const bool preservedOpaque = opaqueChanged(*input.before.document, *input.after.document);
        if (preservedOpaque) {
            auto normalized = std::make_shared<spice::sct::SctDocument>(*input.after.document);
            normalized->opaqueAttachments = input.before.document->opaqueAttachments;
            script.after.document = std::move(normalized);
        }
        auto patch = SalsaScriptPatchService::diff(script.before, script.after,
            input.sourceTextConvention);
        std::unordered_map<std::string, std::size_t> byKey;
        if (!patch) {
            SctChangeUnit invalid;
            invalid.entityKey = "invalid:script";
            invalid.category = SctChangeCategory::Preservation;
            invalid.disposition = SctChangeDisposition::InvalidResult;
            invalid.entityKind = SctChangeEntityKind::OpaquePreservation;
            invalid.summary = patch.diagnostics().empty()
                ? "The script comparison is not representable."
                : patch.diagnostics().front().message;
            invalid.selectable = false;
            addOrMergeUnit(script, byKey, std::move(invalid), identity);
        } else {
            script.completePatch = std::move(patch).takeValue();
            for (const auto& value : script.completePatch.sections) {
                const auto id = value.before ? value.before->id : value.after->id;
                SctChangeUnit unit;
                unit.entityKey = idKey("section", id);
                unit.category = SctChangeCategory::Structure;
                unit.entityKind = SctChangeEntityKind::Section;
                unit.summary = value.before ? (value.after ? "Change section" : "Delete section")
                    : "Insert section";
                unit.target = SctNavigationTarget{SctNavigationKind::Section, id.value()};
                unit.affectsOrder = true;
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& section : script.completePatch.scriptSections) {
                if (section.nameBytes) {
                    SctChangeUnit unit;
                    unit.entityKey = idKey("section", section.section);
                    unit.category = SctChangeCategory::Structure;
                    unit.entityKind = SctChangeEntityKind::Section;
                    unit.summary = "Change section";
                    unit.details.push_back("Rename section");
                    unit.target = SctNavigationTarget{SctNavigationKind::Section,
                        section.section.value()};
                    addOrMergeUnit(script, byKey, std::move(unit), identity);
                }
                for (const auto& value : section.instructions) {
                    const auto id = value.before ? value.before->id : value.after->id;
                    SctChangeUnit unit;
                    unit.entityKey = idKey("instruction", id);
                    unit.category = SctChangeCategory::Instruction;
                    unit.entityKind = SctChangeEntityKind::Instruction;
                    unit.summary = value.before
                        ? (value.after ? "Change instruction" : "Delete instruction")
                        : "Insert instruction";
                    unit.details.push_back("Opcode, expression, parameters, and repeated groups are atomic.");
                    unit.target = SctNavigationTarget{SctNavigationKind::Instruction, id.value()};
                    unit.affectsOrder = !value.before || !value.after;
                    addOrMergeUnit(script, byKey, std::move(unit), identity);
                }
                if (section.instructionOrder)
                    for (const auto id : orderAffected(section.instructionOrder->before,
                            section.instructionOrder->after)) {
                        SctChangeUnit unit;
                        unit.entityKey = idKey("instruction", id);
                        unit.category = SctChangeCategory::Instruction;
                        unit.entityKind = SctChangeEntityKind::Instruction;
                        unit.summary = "Move instruction";
                        unit.details.push_back("Change instruction order");
                        unit.target = SctNavigationTarget{SctNavigationKind::Instruction, id.value()};
                        unit.affectsOrder = true;
                        addOrMergeUnit(script, byKey, std::move(unit), identity);
                    }
            }
            for (const auto& value : script.completePatch.textValues) {
                SctChangeUnit unit;
                unit.entityKey = textKey(value.target);
                unit.category = std::holds_alternative<spice::sct::SctStringId>(value.target)
                    ? SctChangeCategory::Text : SctChangeCategory::Footer;
                unit.entityKind = std::holds_alternative<spice::sct::SctStringId>(value.target)
                    ? SctChangeEntityKind::IndexedString : SctChangeEntityKind::FooterEntry;
                unit.summary = "Change text";
                std::visit([&](const auto id) {
                    using T = std::decay_t<decltype(id)>;
                    unit.target = SctNavigationTarget{
                        std::is_same_v<T, spice::sct::SctStringId>
                            ? SctNavigationKind::String : SctNavigationKind::FooterEntry,
                        id.value()};
                }, value.target);
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.footerEntries) {
                const auto id = value.before ? value.before->id : value.after->id;
                SctChangeUnit unit;
                unit.entityKey = idKey("footer", id);
                unit.category = SctChangeCategory::Footer;
                unit.entityKind = SctChangeEntityKind::FooterEntry;
                unit.summary = value.before
                    ? (value.after ? "Change footer entry" : "Delete footer entry")
                    : "Insert footer entry";
                unit.target = SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
                unit.affectsOrder = !value.before || !value.after;
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.authoredArms) {
                const auto id = value.before ? value.before->id : value.after->id;
                SctChangeUnit unit;
                unit.entityKey = armKey(id);
                unit.category = SctChangeCategory::AuthoringMetadata;
                unit.entityKind = SctChangeEntityKind::AuthoredArm;
                unit.summary = value.before
                    ? (value.after ? "Change authored arm" : "Remove authored arm")
                    : "Add authored arm";
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.textRepairs) {
                SctChangeUnit unit;
                unit.entityKey = repairKey(value.target);
                unit.category = SctChangeCategory::AuthoringMetadata;
                unit.entityKind = SctChangeEntityKind::TextRepair;
                unit.summary = value.before
                    ? (value.after ? "Change text repair provenance" : "Remove text repair provenance")
                    : "Add text repair provenance";
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.unboundReferences) {
                SctChangeUnit unit;
                unit.entityKey = unboundKey(value.site);
                unit.category = SctChangeCategory::AuthoringMetadata;
                unit.entityKind = SctChangeEntityKind::UnboundReference;
                unit.summary = value.before
                    ? (value.after ? "Change unbound reference provenance"
                                   : "Remove unbound reference provenance")
                    : "Add unbound reference provenance";
                unit.target = SctNavigationTarget{SctNavigationKind::Instruction,
                    value.site.instruction.value()};
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.aliases) {
                const auto& record = *(value.before ? value.before : value.after);
                SctChangeUnit unit;
                unit.entityKey = aliasKey(record.variable);
                unit.category = SctChangeCategory::AuthoringMetadata;
                unit.entityKind = SctChangeEntityKind::VariableAlias;
                unit.summary = value.before
                    ? (value.after ? "Change variable alias" : "Remove variable alias")
                    : "Add variable alias";
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.annotations) {
                const auto& record = *(value.before ? value.before : value.after);
                SctChangeUnit unit;
                unit.entityKey = annotationKey(record.target);
                unit.category = SctChangeCategory::AuthoringMetadata;
                unit.entityKind = SctChangeEntityKind::EntityAnnotation;
                unit.summary = value.before
                    ? (value.after ? "Change entity annotation" : "Remove entity annotation")
                    : "Add entity annotation";
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            for (const auto& value : script.completePatch.folders) {
                const auto& record = *(value.before ? value.before : value.after);
                SctChangeUnit unit;
                unit.entityKey = folderKey(record.id);
                unit.category = SctChangeCategory::AuthoringMetadata;
                unit.entityKind = SctChangeEntityKind::SectionFolder;
                unit.summary = value.before
                    ? (value.after ? "Change section folder" : "Remove section folder")
                    : "Add section folder";
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            if (script.completePatch.allocatorState) {
                SctChangeUnit unit;
                unit.entityKey = "allocator";
                unit.category = SctChangeCategory::Preservation;
                unit.entityKind = SctChangeEntityKind::AllocatorState;
                unit.summary = "Preserve stable-ID allocator high-water marks";
                unit.details.push_back(
                    "Allocator preservation is automatically coupled to selected semantic changes.");
                addOrMergeUnit(script, byKey, std::move(unit), identity);
            }
            if (script.completePatch.sectionOrder)
                for (const auto id : orderAffected(script.completePatch.sectionOrder->before,
                        script.completePatch.sectionOrder->after)) {
                    SctChangeUnit unit;
                    unit.entityKey = idKey("section", id);
                    unit.category = SctChangeCategory::Structure;
                    unit.entityKind = SctChangeEntityKind::Section;
                    unit.summary = "Move section";
                    unit.target = SctNavigationTarget{SctNavigationKind::Section, id.value()};
                    unit.affectsOrder = true;
                    addOrMergeUnit(script, byKey, std::move(unit), identity);
                }
            if (script.completePatch.footerOrder)
                for (const auto id : orderAffected(script.completePatch.footerOrder->before,
                        script.completePatch.footerOrder->after)) {
                    SctChangeUnit unit;
                    unit.entityKey = idKey("footer", id);
                    unit.category = SctChangeCategory::Footer;
                    unit.entityKind = SctChangeEntityKind::FooterEntry;
                    unit.summary = "Move footer entry";
                    unit.target = SctNavigationTarget{SctNavigationKind::FooterEntry, id.value()};
                    unit.affectsOrder = true;
                    addOrMergeUnit(script, byKey, std::move(unit), identity);
                }

            for (const auto& repair : script.completePatch.textRepairs) {
                const auto text = byKey.find(textKey(repair.target));
                const auto metadata = byKey.find(repairKey(repair.target));
                if (text == byKey.end() || metadata == byKey.end()) continue;
                auto& textUnit = script.units[text->second];
                auto& metadataUnit = script.units[metadata->second];
                textUnit.coupledUnitIds.push_back(metadataUnit.id);
                metadataUnit.coupledUnitIds.push_back(textUnit.id);
            }
            for (const auto& origin : script.completePatch.unboundReferences) {
                const auto instruction = byKey.find(idKey(
                    "instruction", origin.site.instruction));
                const auto metadata = byKey.find(unboundKey(origin.site));
                if (instruction == byKey.end() || metadata == byKey.end()) continue;
                auto& instructionUnit = script.units[instruction->second];
                auto& metadataUnit = script.units[metadata->second];
                instructionUnit.coupledUnitIds.push_back(metadataUnit.id);
                metadataUnit.coupledUnitIds.push_back(instructionUnit.id);
            }
        }
        if (preservedOpaque) {
            SctChangeUnit unit;
            unit.entityKey = "opaque:preserved";
            unit.category = SctChangeCategory::Preservation;
            unit.disposition = SctChangeDisposition::Warning;
            unit.entityKind = SctChangeEntityKind::OpaquePreservation;
            unit.summary = "Discard incoming opaque changes and preserve the baseline bytes";
            unit.selectable = false;
            unit.acknowledgementRequired = true;
            addOrMergeUnit(script, byKey, std::move(unit), identity);
        }
        std::ranges::sort(script.units, {}, &SctChangeUnit::entityKey);
        auto serialized = SalsaScriptPatchCodec::serialize(script.completePatch);
        if (!serialized) return Result<SctChangePlan>::failure(serialized.diagnostics());
        std::string identityMaterial = identity + '|'
            + input.baselineRevision.digest.toHex() + '|';
        identityMaterial.append(reinterpret_cast<const char*>(serialized.value().data()),
            serialized.value().size());
        for (const auto& unit : script.units)
            identityMaterial += '|' + unit.id + ':' + std::to_string(
                static_cast<int>(unit.disposition));
        auto scriptId = digestText(identityMaterial);
        if (!scriptId) return Result<SctChangePlan>::failure(scriptId.diagnostics());
        script.id = std::move(scriptId).takeValue();
        plan.scripts.push_back(std::move(script));
    }
    std::ranges::sort(plan.scripts, {}, [](const auto& script) {
        return script.locator.identityKey();
    });
    std::string root;
    for (const auto& script : plan.scripts) root += script.id;
    auto planId = digestText(root);
    if (!planId) return Result<SctChangePlan>::failure(planId.diagnostics());
    plan.id = std::move(planId).takeValue();
    return Result<SctChangePlan>::success(std::move(plan));
}

SctChangeSelection SctChangePlanService::selectAll(const SctChangePlan& plan) {
    SctChangeSelection selection;
    selection.planId = plan.id;
    for (const auto& script : plan.scripts)
        for (const auto& unit : script.units)
            if (unit.selectable
                && unit.disposition != SctChangeDisposition::Conflict
                && unit.disposition != SctChangeDisposition::InvalidResult)
                selection.selectedUnitIds.push_back(unit.id);
    std::ranges::sort(selection.selectedUnitIds);
    return selection;
}

SctChangeApplication SctChangePlanService::preview(const SctChangePlan& plan,
    const std::span<const SctCurrentScriptState> current,
    const SctChangeSelection& selection) {
    SctChangeApplication result;
    result.planId = plan.id;
    const std::unordered_set<std::string> requested(
        selection.selectedUnitIds.begin(), selection.selectedUnitIds.end());
    const std::unordered_set<std::string> acknowledged(
        selection.acknowledgedWarningIds.begin(), selection.acknowledgedWarningIds.end());
    if (selection.planId != plan.id) {
        for (const auto& script : plan.scripts) {
            SctScriptChangeApplication failed{script.locator};
            failed.status = SctChangeApplicationStatus::Blocked;
            failed.diagnostics.push_back({SctChangeDisposition::Conflict,
                "StalePlan", "The selection belongs to a different change plan."});
            result.scripts.push_back(std::move(failed));
        }
        return result;
    }

    std::unordered_set<std::string> knownIds;
    for (const auto& script : plan.scripts)
        for (const auto& unit : script.units) knownIds.insert(unit.id);
    const auto unknownSelection = std::ranges::find_if(requested,
        [&](const auto& id) { return !knownIds.contains(id); });
    const auto unknownAcknowledgement = std::ranges::find_if(acknowledged,
        [&](const auto& id) { return !knownIds.contains(id); });
    if (unknownSelection != requested.end()
        || unknownAcknowledgement != acknowledged.end()) {
        for (const auto& script : plan.scripts) {
            SctScriptChangeApplication failed{script.locator};
            failed.status = SctChangeApplicationStatus::Blocked;
            failed.diagnostics.push_back({SctChangeDisposition::Conflict,
                "UnknownChangeUnit",
                "The selection or acknowledgement contains a change that is not in this plan."});
            result.scripts.push_back(std::move(failed));
        }
        return result;
    }

    for (const auto& script : plan.scripts) {
        SctScriptChangeApplication applied{script.locator};
        const auto* currentState = findCurrent(current, script.locator.identityKey());
        if (!currentState || !currentState->document) {
            applied.status = SctChangeApplicationStatus::Blocked;
            applied.diagnostics.push_back({SctChangeDisposition::Conflict,
                "MissingCurrentState", "The current script state is unavailable."});
            result.scripts.push_back(std::move(applied));
            continue;
        }
        std::unordered_map<std::string, const SctChangeUnit*> byId;
        std::unordered_set<std::string> selectedIds;
        std::unordered_set<std::string> selectedKeys;
        std::unordered_set<std::string> selectedOrderKeys;
        for (const auto& unit : script.units) byId.emplace(unit.id, &unit);
        bool invalidSelection = false;
        for (const auto& id : requested) {
            const auto found = byId.find(id);
            if (found == byId.end()) continue;
            if (!found->second->selectable) {
                invalidSelection = true;
                applied.diagnostics.push_back({SctChangeDisposition::Conflict,
                    "UnselectableChangeUnit",
                    "This change is diagnostic-only and cannot be selected.",
                    found->second->id, found->second->target});
                continue;
            }
            selectedIds.insert(id);
        }
        if (invalidSelection) {
            applied.status = SctChangeApplicationStatus::Blocked;
            result.scripts.push_back(std::move(applied));
            continue;
        }
        bool expanded = true;
        while (expanded) {
            expanded = false;
            const auto snapshot = selectedIds;
            for (const auto& id : snapshot) {
                for (const auto& coupled : byId.at(id)->coupledUnitIds)
                    if (selectedIds.insert(coupled).second) expanded = true;
            }
        }
        for (const auto& id : selectedIds) {
            const auto& unit = *byId.at(id);
            selectedKeys.insert(unit.entityKey);
            if (unit.affectsOrder) selectedOrderKeys.insert(unit.entityKey);
        }
        if (selectedIds.empty()) {
            applied.status = SctChangeApplicationStatus::Unchanged;
            applied.state = *currentState;
            result.scripts.push_back(std::move(applied));
            continue;
        }
        bool missingPermanentAcknowledgement = false;
        for (const auto& unit : script.units) {
            if (unit.acknowledgementRequired && !acknowledged.contains(unit.id)) {
                missingPermanentAcknowledgement = true;
                applied.diagnostics.push_back({SctChangeDisposition::Warning,
                    "AcknowledgementRequired", unit.summary, unit.id, unit.target});
            }
        }
        if (missingPermanentAcknowledgement) {
            applied.status = SctChangeApplicationStatus::Blocked;
            result.scripts.push_back(std::move(applied));
            continue;
        }

        auto drift = SalsaScriptPatchService::diff(script.before, *currentState,
            script.completePatch.sourceTextConvention);
        if (!drift) {
            applied.status = SctChangeApplicationStatus::Blocked;
            applied.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                "CurrentStateUnrepresentable",
                drift.diagnostics().empty() ? "The current state cannot be compared."
                    : drift.diagnostics().front().message});
            result.scripts.push_back(std::move(applied));
            continue;
        }
        const auto conflicts = changedKeys(drift.value());
        std::unordered_set<std::string> cleanKeys = selectedKeys;
        for (const auto& id : selectedIds) {
            const auto& unit = *byId.at(id);
            if (!conflicts.contains(unit.entityKey)) continue;
            cleanKeys.erase(unit.entityKey);
            selectedOrderKeys.erase(unit.entityKey);
            applied.diagnostics.push_back({SctChangeDisposition::Conflict,
                "ExpectedBeforeMismatch",
                "The current entity no longer matches this change's expected-before value.",
                unit.id, unit.target});
        }
        auto subset = selectedPatch(script, *currentState, cleanKeys, selectedOrderKeys);
        auto state = SalsaScriptPatchService::apply(*currentState, subset);
        if (!state) {
            applied.status = SctChangeApplicationStatus::Blocked;
            applied.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                "ApplicationFailed", state.diagnostics().empty()
                    ? "The selected changes could not be represented."
                    : state.diagnostics().front().message});
            result.scripts.push_back(std::move(applied));
            continue;
        }
        for (const auto& id : selectedIds)
            if (cleanKeys.contains(byId.at(id)->entityKey)) applied.appliedUnitIds.push_back(id);
        std::ranges::sort(applied.appliedUnitIds);
        if (applied.appliedUnitIds.empty()) {
            applied.status = SctChangeApplicationStatus::Blocked;
            result.scripts.push_back(std::move(applied));
            continue;
        }
        const auto validation = spice::sct::SctDocumentValidator::validateDocument(
            *state.value().document);
        if (!validation.validDocument) {
            bool missingRepairAcknowledgement = false;
            for (const auto& id : applied.appliedUnitIds) {
                if (acknowledged.contains(id)) continue;
                missingRepairAcknowledgement = true;
                const auto& unit = *byId.at(id);
                applied.diagnostics.push_back({SctChangeDisposition::Warning,
                    "RepairStateAcknowledgementRequired",
                    "This selection produces a document that must be repaired before checkpoint or export.",
                    id, unit.target});
            }
            if (missingRepairAcknowledgement) {
                applied.status = SctChangeApplicationStatus::Blocked;
                applied.appliedUnitIds.clear();
                result.scripts.push_back(std::move(applied));
                continue;
            }
            for (const auto& diagnostic : validation.diagnostics)
                applied.diagnostics.push_back({SctChangeDisposition::Warning,
                    "DocumentValidation/" + std::to_string(static_cast<int>(diagnostic.code)),
                    diagnostic.message});
            applied.status = SctChangeApplicationStatus::RepairRequired;
        } else applied.status = SctChangeApplicationStatus::Applied;
        applied.state = std::move(state).takeValue();
        result.scripts.push_back(std::move(applied));
    }
    return result;
}

SctChangeApplication SctChangePlanService::apply(const SctChangePlan& plan,
    const std::span<const SctCurrentScriptState> current,
    const SctChangeSelection& selection) {
    return preview(plan, current, selection);
}

std::string SctChangeDiagnosticFormatter::format(
    const SctChangeDiagnostic& diagnostic) {
    std::ostringstream text;
    text << '[' << dispositionName(diagnostic.disposition) << "] "
        << diagnostic.code << ": " << diagnostic.message;
    if (diagnostic.unitId) text << " (change " << *diagnostic.unitId << ')';
    return text.str();
}

}  // namespace salsa::core
