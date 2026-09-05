#include "SalsaCore/Sct/SctMerge.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SpiceSCT/SctDocumentValidator.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic mergeError(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctPatch,
        std::move(message), std::nullopt};
}

[[nodiscard]] Result<std::string> digestText(const std::string_view value) {
    const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
    const auto digest = sha256(bytes);
    if (!digest) return Result<std::string>::failure(digest.diagnostics());
    return Result<std::string>::success(digest.value().toHex());
}

[[nodiscard]] const SctChangeUnit* unitByKey(
    const SctScriptChangePlan& script, const std::string_view key) {
    const auto found = std::ranges::find(script.units, key, &SctChangeUnit::entityKey);
    return found == script.units.end() ? nullptr : &*found;
}

[[nodiscard]] const SctChangeUnit* unitById(
    const SctScriptChangePlan& script, const std::string_view id) {
    const auto found = std::ranges::find(script.units, id, &SctChangeUnit::id);
    return found == script.units.end() ? nullptr : &*found;
}

[[nodiscard]] bool semanticEquivalent(
    const SctSemanticState& left, const SctSemanticState& right) {
    const auto patch = SalsaScriptPatchService::diff(left, right, std::nullopt);
    return patch && patch.value().empty();
}

[[nodiscard]] std::unordered_map<std::string, std::string> dependencyGroups(
    const SctScriptChangePlan& script) {
    std::unordered_map<std::string, std::vector<std::string>> edges;
    for (const auto& unit : script.units) {
        auto& connected = edges[unit.id];
        connected.insert(connected.end(), unit.coupledUnitIds.begin(),
            unit.coupledUnitIds.end());
        for (const auto& other : unit.coupledUnitIds)
            edges[other].push_back(unit.id);
    }
    std::unordered_map<std::string, std::string> result;
    for (const auto& unit : script.units) {
        if (result.contains(unit.id)) continue;
        std::vector<std::string> pending{unit.id};
        std::vector<std::string> component;
        while (!pending.empty()) {
            auto current = std::move(pending.back());
            pending.pop_back();
            if (std::ranges::find(component, current) != component.end()) continue;
            component.push_back(current);
            for (const auto& next : edges[current]) pending.push_back(next);
        }
        std::ranges::sort(component);
        std::string seed = script.id;
        for (const auto& member : component) seed += '|' + member;
        const auto digest = digestText(seed);
        const auto group = digest ? digest.value() : seed;
        for (const auto& member : component) result.emplace(member, group);
    }
    return result;
}

[[nodiscard]] bool deleting(const SctChangeUnit& unit) {
    return unit.summary.starts_with("Delete");
}

[[nodiscard]] SctMergeConflictKind conflictKind(
    const SctChangeUnit& local, const SctChangeUnit& incoming) {
    if (deleting(local) && !deleting(incoming)) return SctMergeConflictKind::DeleteModify;
    if (!deleting(local) && deleting(incoming)) return SctMergeConflictKind::ModifyDelete;
    if (local.affectsOrder && incoming.affectsOrder)
        return SctMergeConflictKind::IncompatibleOrder;
    if (local.entityKind == SctChangeEntityKind::IndexedString
        || local.entityKind == SctChangeEntityKind::SupplementaryText)
        return SctMergeConflictKind::TextOwnership;
    if (local.entityKind == SctChangeEntityKind::OpaquePreservation)
        return SctMergeConflictKind::OpaquePreservation;
    if (local.category == SctChangeCategory::AuthoringMetadata)
        return SctMergeConflictKind::AuthoringMetadata;
    return SctMergeConflictKind::DivergentValue;
}

[[nodiscard]] std::optional<SctSemanticState> applySelection(
    const SctChangePlan& plan, const SctSemanticState& current,
    const std::unordered_set<std::string>& selectedIds,
    std::vector<SctChangeDiagnostic>& diagnostics) {
    SctChangeSelection selection;
    selection.planId = plan.id;
    selection.selectedUnitIds.assign(selectedIds.begin(), selectedIds.end());
    const auto& script = plan.scripts.front();
    for (const auto& unit : script.units)
        if (unit.acknowledgementRequired)
            selection.acknowledgedWarningIds.push_back(unit.id);
    const std::array states{SctCurrentScriptState{script.locator, current}};
    auto application = SctChangePlanService::preview(plan, states, selection);
    if (application.scripts.empty()) return std::nullopt;
    diagnostics.insert(diagnostics.end(),
        application.scripts.front().diagnostics.begin(),
        application.scripts.front().diagnostics.end());
    if (application.scripts.front().state)
        return application.scripts.front().state;
    if (semanticEquivalent(current, plan.scripts.front().after)) return current;
    return std::nullopt;
}

[[nodiscard]] std::unordered_set<std::string> keysForConflict(
    const SctMergeConflict& conflict) {
    return {conflict.entityKeys.begin(), conflict.entityKeys.end()};
}

[[nodiscard]] bool planChangesOnlyKeys(
    const SctChangePlan& plan, const std::unordered_set<std::string>& allowed) {
    return std::ranges::all_of(plan.scripts.front().units, [&](const auto& unit) {
        return allowed.contains(unit.entityKey)
            || unit.entityKind == SctChangeEntityKind::AllocatorState;
    });
}

}  // namespace

Result<SctMergePlan> SctMergePlanService::build(const SctMergeRequest& request) {
    if (!request.base.document || !request.local.document || !request.incoming.document)
        return Result<SctMergePlan>::failure(mergeError(
            "A merge requires complete base, local, and incoming semantic states."));

    const std::array localInput{SctScriptComparisonInput{request.locator,
        request.baseRevision, request.base, request.local,
        request.sourceTextConvention}};
    const std::array incomingInput{SctScriptComparisonInput{request.locator,
        request.baseRevision, request.base, request.incoming,
        request.sourceTextConvention}};
    auto local = SctChangePlanService::build(localInput);
    auto incoming = SctChangePlanService::build(incomingInput);
    if (!local) return Result<SctMergePlan>::failure(local.diagnostics());
    if (!incoming) return Result<SctMergePlan>::failure(incoming.diagnostics());

    SctMergePlan plan{"", request};
    plan.localChanges = std::move(local).takeValue();
    plan.incomingChanges = std::move(incoming).takeValue();
    auto& localScript = plan.localChanges.scripts.front();
    const auto& incomingScript = plan.incomingChanges.scripts.front();
    const auto groups = dependencyGroups(localScript);
    for (auto& unit : localScript.units) unit.dependencyGroupId = groups.at(unit.id);

    const std::array incomingToLocalInput{SctScriptComparisonInput{request.locator,
        request.baseRevision, request.incoming, request.local,
        request.sourceTextConvention}};
    auto incomingToLocal = SctChangePlanService::build(incomingToLocalInput);
    if (!incomingToLocal)
        return Result<SctMergePlan>::failure(incomingToLocal.diagnostics());
    const auto& deltaScript = incomingToLocal.value().scripts.front();

    std::unordered_set<std::string> conflictingGroups;
    std::unordered_map<std::string, std::vector<const SctChangeUnit*>> groupMembers;
    for (const auto& unit : localScript.units)
        groupMembers[unit.dependencyGroupId].push_back(&unit);
    for (const auto& localUnit : localScript.units) {
        const auto* incomingUnit = unitByKey(incomingScript, localUnit.entityKey);
        if (!incomingUnit) continue;
        if (!unitByKey(deltaScript, localUnit.entityKey)) continue;
        conflictingGroups.insert(localUnit.dependencyGroupId);
    }

    std::unordered_set<std::string> cleanIds;
    for (const auto& unit : localScript.units)
        if (!conflictingGroups.contains(unit.dependencyGroupId)
            && unit.selectable
            && unit.disposition != SctChangeDisposition::Conflict
            && unit.disposition != SctChangeDisposition::InvalidResult)
            cleanIds.insert(unit.id);

    std::vector<SctChangeDiagnostic> ignored;
    auto automatic = applySelection(plan.localChanges, request.incoming,
        cleanIds, ignored);
    if (!automatic) return Result<SctMergePlan>::failure(mergeError(
        "The nonconflicting merge changes could not be projected."));
    plan.automaticCandidate = std::move(*automatic);

    for (const auto& group : conflictingGroups) {
        SctMergeConflict conflict;
        conflict.dependencyGroupId = group;
        conflict.id = digestText(plan.localChanges.id + "|conflict|" + group).value();
        for (const auto* unit : groupMembers[group]) {
            conflict.unitIds.push_back(unit->id);
            conflict.entityKeys.push_back(unit->entityKey);
            if (!conflict.target) conflict.target = unit->target;
            conflict.entityKind = unit->entityKind;
            if (const auto* other = unitByKey(incomingScript, unit->entityKey))
                conflict.kind = conflictKind(*unit, *other);
        }
        std::ranges::sort(conflict.unitIds);
        std::ranges::sort(conflict.entityKeys);
        conflict.summary = request.mode == SctMergeMode::TrueThreeWay
            ? "Local and incoming changes require semantic resolution."
            : "Existing and compared changes require semantic integration.";
        conflict.details.push_back("Accepting incoming preserves the incoming state for this dependency group.");
        conflict.details.push_back("Keeping local transplants the local result onto the incoming state.");
        plan.conflicts.push_back(std::move(conflict));
    }
    std::ranges::sort(plan.conflicts, {}, &SctMergeConflict::id);

    const std::array automaticInput{SctScriptComparisonInput{request.locator,
        request.baseRevision, request.incoming, plan.automaticCandidate,
        request.sourceTextConvention}};
    auto automaticPlan = SctChangePlanService::build(automaticInput);
    if (!automaticPlan)
        return Result<SctMergePlan>::failure(automaticPlan.diagnostics());
    plan.automaticChangePlan = std::move(automaticPlan).takeValue();

    std::string seed = request.locator.identityKey() + '|'
        + request.baseRevision.digest.toHex() + '|' + request.contextToken + '|'
        + plan.localChanges.id + '|' + plan.incomingChanges.id + '|'
        + std::to_string(static_cast<int>(request.mode));
    const auto id = digestText(seed);
    if (!id) return Result<SctMergePlan>::failure(id.diagnostics());
    plan.id = id.value();
    return Result<SctMergePlan>::success(std::move(plan));
}

SctMergePreview SctMergePlanService::preview(
    const SctMergePlan& plan,
    const std::span<const SctMergeResolution> resolutions,
    const std::string_view currentContextToken) {
    SctMergePreview result;
    result.planId = plan.id;
    if (currentContextToken != plan.request.contextToken) {
        result.status = SctMergePreviewStatus::Stale;
        result.diagnostics.push_back({SctChangeDisposition::Conflict,
            "StaleMergePlan", "The source or workspace state changed after the merge plan was created."});
        return result;
    }

    std::unordered_map<std::string, const SctMergeResolution*> byConflict;
    for (const auto& resolution : resolutions) {
        if (!std::ranges::any_of(plan.conflicts, [&](const auto& conflict) {
                return conflict.id == resolution.conflictId;
            }) || !byConflict.emplace(resolution.conflictId, &resolution).second) {
            result.status = SctMergePreviewStatus::Invalid;
            result.diagnostics.push_back({SctChangeDisposition::Conflict,
                "InvalidMergeResolution", "A merge resolution is unknown or duplicated."});
            return result;
        }
    }

    auto candidate = plan.automaticCandidate;
    for (const auto& conflict : plan.conflicts) {
        const auto resolution = byConflict.find(conflict.id);
        if (resolution == byConflict.end()) {
            result.unresolvedConflicts.push_back(conflict);
            continue;
        }
        const auto& value = *resolution->second;
        if (value.kind == SctMergeResolutionKind::AcceptIncoming
            || value.kind == SctMergeResolutionKind::DropLocal) continue;

        if (value.kind == SctMergeResolutionKind::UseEditedCandidate) {
            if (!value.editedCandidate || !value.editedCandidate->document) {
                result.status = SctMergePreviewStatus::Invalid;
                result.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                    "MissingEditedCandidate", "The edited merge resolution has no semantic candidate."});
                return result;
            }
            const std::array editInput{SctScriptComparisonInput{plan.request.locator,
                plan.request.baseRevision, candidate, *value.editedCandidate,
                plan.request.sourceTextConvention}};
            const auto editedPlan = SctChangePlanService::build(editInput);
            if (!editedPlan || !planChangesOnlyKeys(editedPlan.value(), keysForConflict(conflict))) {
                result.status = SctMergePreviewStatus::Invalid;
                result.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                    "EditedCandidateEscapedDependencyGroup",
                    "The edited resolution changed content outside its dependency group."});
                return result;
            }
            candidate = *value.editedCandidate;
            continue;
        }

        const std::array overrideInput{SctScriptComparisonInput{plan.request.locator,
            plan.request.baseRevision, plan.request.incoming, plan.request.local,
            plan.request.sourceTextConvention}};
        const auto overridePlan = SctChangePlanService::build(overrideInput);
        if (!overridePlan) {
            result.status = SctMergePreviewStatus::Invalid;
            result.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                "LocalResolutionUnavailable", "The local resolution could not be represented."});
            return result;
        }
        std::unordered_set<std::string> selected;
        const auto allowed = keysForConflict(conflict);
        for (const auto& unit : overridePlan.value().scripts.front().units)
            if (allowed.contains(unit.entityKey)) selected.insert(unit.id);
        std::vector<SctChangeDiagnostic> diagnostics;
        auto applied = applySelection(overridePlan.value(), candidate, selected, diagnostics);
        if (!applied) {
            result.status = SctMergePreviewStatus::Invalid;
            result.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                "LocalResolutionFailed", "The local resolution could not be applied."});
            return result;
        }
        candidate = std::move(*applied);
    }

    if (!result.unresolvedConflicts.empty()) {
        result.status = SctMergePreviewStatus::Conflicted;
        result.candidate = candidate;
        return result;
    }

    const auto validation = spice::sct::SctDocumentValidator::validateDocument(
        *candidate.document);
    if (!validation.validDocument) {
        result.status = SctMergePreviewStatus::Invalid;
        for (const auto& diagnostic : validation.diagnostics)
            result.diagnostics.push_back({SctChangeDisposition::InvalidResult,
                "DocumentValidation/" + std::to_string(static_cast<int>(diagnostic.code)),
                diagnostic.message});
        return result;
    }
    const std::array finalInput{SctScriptComparisonInput{plan.request.locator,
        plan.request.baseRevision, plan.request.incoming, candidate,
        plan.request.sourceTextConvention}};
    auto changePlan = SctChangePlanService::build(finalInput);
    if (!changePlan) {
        result.status = SctMergePreviewStatus::Invalid;
        result.diagnostics.push_back({SctChangeDisposition::InvalidResult,
            "FinalChangePlanFailed", "The resolved merge result is not representable as a patch."});
        return result;
    }
    result.status = SctMergePreviewStatus::Ready;
    result.candidate = std::move(candidate);
    result.changePlan = std::move(changePlan).takeValue();
    return result;
}

}  // namespace salsa::core
