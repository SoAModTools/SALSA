#include "SalsaCore/Authoring/SctAuthoringProject.h"
#include "SpiceSCT/SctScptEncoding.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cmath>
#include <set>
#include <type_traits>

namespace salsa::core {
namespace {
Diagnostic error(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)};
}
template<class T, class Id> const T* lookup(const std::vector<T>& items, Id id) noexcept {
    const auto found = std::ranges::find(items, id, &T::id);
    return found == items.end() ? nullptr : &*found;
}
std::string context(std::string_view kind, std::uint64_t id) {
    return std::string(kind) + " " + std::to_string(id) + ": ";
}
void checkEvidence(const std::vector<SctAuthoringEvidence>& evidence,
    const std::string& where, std::vector<Diagnostic>& out) {
    for (const auto& item : evidence) {
        if ((item.kind != SctEvidenceKind::SourceObservation && item.kind != SctEvidenceKind::Inference
            && item.kind != SctEvidenceKind::UserAssertion) || item.description.empty())
            out.push_back(error(where + "invalid evidence kind or empty description."));
    }
}
bool exists(const SctAuthoringProject& project, const SctAuthoringEntityId& id) {
    return std::visit([&](auto value) { return project.find(value) != nullptr; }, id);
}
} // namespace

bool SctLiteralConstant::valid() const noexcept {
    using namespace spice::sct;
    if (operation.kind != SctScptValueKind::InlineValue && operation.kind != SctScptValueKind::DecimalLiteral
        && operation.kind != SctScptValueKind::FloatLiteral) return false;
    return isSctScptOperationEncodingValid(operation)
        && termination == (operation.kind == SctScptValueKind::InlineValue
            ? SctExpressionTermination::InlineValue : SctExpressionTermination::StopCode);
}
spice::sct::SctCanonicalExpression SctLiteralConstant::expression() const {
    return {spice::sct::SctTypedScptProgram{{operation}}, termination};
}
std::optional<SctLiteralConstant> SctLiteralConstant::fromExpression(const spice::sct::SctCanonicalExpression& expression) {
    const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&expression.body);
    if (!program || program->operations.size() != 1) return {};
    const auto* value = std::get_if<spice::sct::SctScptValueOperation>(&program->operations.front());
    if (!value) return {};
    SctLiteralConstant result{*value, expression.termination};
    return result.valid() ? std::optional{result} : std::nullopt;
}

bool validSctAuthoringUuid(std::string_view value) noexcept {
    if (value.size() != 36 || value == "00000000-0000-0000-0000-000000000000") return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}
Result<std::string> generateSctAuthoringUuid() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return Result<std::string>::failure(error("Cannot generate authoring identity."));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return Result<std::string>::success(std::move(result));
}
Result<SctAuthoringProject> SctAuthoringProject::create() {
    auto uuid = generateSctAuthoringUuid();
    if (!uuid) return Result<SctAuthoringProject>::failure(uuid.diagnostics());
    SctAuthoringProject result;
    result.id.value = std::move(uuid).takeValue();
    return Result<SctAuthoringProject>::success(std::move(result));
}
const SctAuthoringBaseline* SctAuthoringProject::find(SctBaselineId value) const noexcept { return lookup(baselines, value); }
const SctScriptContext* SctAuthoringProject::find(SctScriptId value) const noexcept { return lookup(scripts, value); }
const SctAuthoringModule* SctAuthoringProject::find(SctModuleId value) const noexcept { return lookup(modules, value); }
const SctAuthoringEntrypoint* SctAuthoringProject::find(SctEntrypointId value) const noexcept { return lookup(entrypoints, value); }
const SctAuthoringPort* SctAuthoringProject::find(SctPortId value) const noexcept { return lookup(ports, value); }
const SctAuthoringConnection* SctAuthoringProject::find(SctConnectionId value) const noexcept { return lookup(connections, value); }
const SctAuthoringContent* SctAuthoringProject::find(SctContentId value) const noexcept { return lookup(contents, value); }
const SctAuthoredSequence* SctAuthoringProject::find(SctSequenceId value) const noexcept { return lookup(sequences, value); }
const SctNamedPredicate* SctAuthoringProject::find(SctPredicateId value) const noexcept { return lookup(predicates, value); }
const SctSequenceAction* SctAuthoringProject::find(SctActionId value) const noexcept {
    for (const auto& sequence : sequences) if (auto* action = lookup(sequence.actions, value)) return action;
    return nullptr;
}
std::optional<SctScriptId> SctAuthoringProject::effectiveScript(const SctContentOwner& owner) const noexcept {
    return std::visit([&](auto value) -> std::optional<SctScriptId> {
        const auto* item = find(value);
        if (!item) return std::nullopt;
        if constexpr (std::is_same_v<decltype(value), SctScriptId>) return value;
        else return find(item->script) ? std::optional{item->script} : std::nullopt;
    }, owner);
}

std::vector<Diagnostic> SctAuthoringProject::validate() const {
    std::vector<Diagnostic> out;
    if (!id.valid()) out.push_back(error("Project UUID is invalid."));
    if (!revision.valid()) out.push_back(error("Project revision must be nonzero."));
    std::set<std::uint64_t> ids;
    const auto registerIds = [&](const auto& items, std::string_view kind) {
        for (const auto& item : items)
            if (!item.id.valid() || !ids.insert(item.id.value).second)
                out.push_back(error(context(kind, item.id.value) + "invalid or duplicate entity ID."));
    };
    registerIds(baselines, "Baseline"); registerIds(scripts, "Script"); registerIds(modules, "Module");
    registerIds(entrypoints, "Entrypoint"); registerIds(ports, "Port");
    registerIds(connections, "Connection"); registerIds(contents, "Content");
    registerIds(sequences, "Sequence"); registerIds(predicates, "Predicate");
    for (const auto& sequence : sequences) registerIds(sequence.actions, "Action");
    if (nextEntityId == 0 || (!ids.empty() && nextEntityId <= *ids.rbegin()))
        out.push_back(error("Allocator must be nonzero and above every allocated ID."));

    std::set<SctImportedDocumentId> documents;
    for (const auto& item : baselines) {
        const auto where = context("Baseline", item.id.value);
        if (!item.importedDocument.valid() || !documents.insert(item.importedDocument).second)
            out.push_back(error(where + "invalid or duplicate imported-document identity."));
        if (item.source.byteSize == 0) out.push_back(error(where + "source baseline cannot be empty."));
        if (item.textConvention && !std::ranges::any_of(spice::sct::kSctKnownTextConventions,
            [&](const auto& convention) { return convention.convention == *item.textConvention; }))
            out.push_back(error(where + "invalid text convention."));
        checkEvidence(item.importEvidence, where, out);
        if (item.recipe.platform && *item.recipe.platform != GamePlatform::GameCube && *item.recipe.platform != GamePlatform::Dreamcast)
            out.push_back(error(where + "invalid declared import platform."));
        if (item.recipe.trustSelectedTextEncoding && !item.textConvention)
            out.push_back(error(where + "trusted text promotion requires an explicit encoding."));
    }
    const auto checkReference = [&](const auto& ref, const std::string& where) {
        std::visit([&](const auto& target) {
            using T = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<T, SctUnresolvedBinding>) {
                if (target.originalEvidence.empty() || target.explanation.empty())
                    out.push_back(error(where + "unresolved binding requires original evidence and explanation."));
                checkEvidence(target.evidence, where, out);
                out.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding,
                    where + target.explanation});
            } else if (!find(target)) out.push_back(error(where + "dangling resolved reference " + std::to_string(target.value) + "."));
        }, ref);
    };
    const auto checkUses = [&](const auto& uses, SctScriptId script, const std::string& where) {
        for (const auto& use : uses) {
            checkReference(use, where);
            if (const auto* target = std::get_if<SctContentId>(&use)) {
                if (const auto* content = find(*target); content && effectiveScript(content->owner) != script)
                    out.push_back(error(where + "content use crosses parent script ownership."));
            }
        }
    };
    for (const auto& item : scripts) {
        const auto where = context("Script", item.id.value);
        if (!find(item.baseline)) out.push_back(error(where + "imported baseline is required."));
        if (item.platform && *item.platform != GamePlatform::GameCube && *item.platform != GamePlatform::Dreamcast)
            out.push_back(error(where + "invalid platform."));
        if (item.region && *item.region != GameRegion::NorthAmerica && *item.region != GameRegion::Europe && *item.region != GameRegion::Japan)
            out.push_back(error(where + "invalid region."));
        if (item.legacyOrigin && (item.legacyOrigin->capsuleId.empty() || item.legacyOrigin->scriptKey.empty()))
            out.push_back(error(where + "legacy origin requires capsule identity and script key."));
        checkUses(item.contentUses, item.id, where);
    }
    const auto checkOwned = [&](const auto& items, std::string_view kind) {
        for (const auto& item : items) {
            const auto where = context(kind, item.id.value);
            if (!find(item.script)) out.push_back(error(where + "parent script does not exist."));
            checkUses(item.contentUses, item.script, where);
            checkEvidence(item.evidence, where, out);
        }
    };
    checkOwned(modules, "Module"); checkOwned(entrypoints, "Entrypoint");
    for (const auto& item : ports) {
        if (!std::visit([&](auto owner) { return effectiveScript(SctContentOwner{owner}).has_value(); }, item.owner))
            out.push_back(error(context("Port", item.id.value) + "owner does not exist or has no valid script."));
    }
    for (const auto& item : connections) {
        const auto where = context("Connection", item.id.value);
        checkReference(item.source, where + "source: "); checkReference(item.destination, where + "destination: ");
        checkEvidence(item.evidence, where, out);
    }
    for (const auto& item : contents) {
        const auto where = context("Content", item.id.value);
        if (item.physicalPatch) {
            if (!item.literalOverrides.empty()) out.push_back(error(where + "physical patches and literal overrides are mutually exclusive."));
            auto encoded = SalsaScriptPatchCodec::serialize(*item.physicalPatch);
            if (!encoded) out.insert(out.end(), encoded.diagnostics().begin(), encoded.diagnostics().end());
        }
        std::set<spice::sct::SctParameterSite> sites;
        for (const auto& edit : item.literalOverrides) {
            if (!edit.site.instruction || edit.site.parameter.repeatedGroupOrdinal
                || !sites.insert(edit.site).second || edit.value == edit.baselineValue)
                out.push_back(error(where + "invalid, duplicate, or redundant preserved literal override."));
            if (!edit.baselineValue.valid() || !edit.value.valid()
                || edit.baselineValue.operation.kind != edit.value.operation.kind
                || edit.baselineValue.termination != edit.value.termination)
                out.push_back(error(where + "preserved literal override must retain its literal encoding family and termination."));
        }
        const auto script = effectiveScript(item.owner);
        const auto* baseline = find(item.region.baseline);
        if (!script) out.push_back(error(where + "owner has no valid script."));
        if (!baseline || item.region.importedDocument != baseline->importedDocument)
            out.push_back(error(where + "preserved region has an invalid baseline/document binding."));
        if (script && find(*script)->baseline != item.region.baseline)
            out.push_back(error(where + "preserved region belongs to another script baseline."));
        if (const auto* selection = std::get_if<SctOrderedSourceSelection>(&item.region.coverage)) {
            if (selection->entities.empty()) out.push_back(error(where + "explicit source selection cannot be empty."));
            std::set<spice::sct::SctDocumentEntityId> selected;
            for (const auto& entity : selection->entities) {
                const bool valid = std::visit([](auto value) {
                    if constexpr (std::is_same_v<decltype(value), std::monostate>) return false;
                    else return static_cast<bool>(value);
                }, entity);
                if (!valid || !selected.insert(entity).second)
                    out.push_back(error(where + "source selection contains invalid or duplicate entities."));
            }
        }
        checkEvidence(item.evidence, where, out);
    }
    const auto checkBinding = [&](const auto& item) {
        const auto* script = find(item.script);
        const auto* baseline = find(item.binding.baseline);
        if (item.name.empty() || !script || !baseline || script->baseline != item.binding.baseline
            || baseline->importedDocument != item.binding.importedDocument)
            out.push_back(error("Named semantic definition has an empty name or stale script/baseline binding."));
    };
    std::set<std::pair<SctScriptId, spice::sct::SctSectionId>> sections;
    for (const auto& sequence : sequences) {
        checkBinding(sequence);
        if (!sequence.section || !sections.emplace(sequence.script, sequence.section).second)
            out.push_back(error("Invalid or duplicate sequence section."));
        std::set<spice::sct::SctInstructionId> actions;
        for (const auto& action : sequence.actions) if (!action.instruction || !actions.insert(action.instruction).second)
            out.push_back(error("Invalid or duplicate action binding."));
    }
    std::set<std::pair<SctScriptId, spice::sct::SctParameterSite>> uses;
    for (const auto& predicate : predicates) {
        checkBinding(predicate);
        if (predicate.uses.empty()) out.push_back(error("A named predicate requires at least one use."));
        for (const auto& use : predicate.uses) if (!use.instruction || use.parameter.repeatedGroupOrdinal
            || !uses.emplace(predicate.script, use).second)
            out.push_back(error("Invalid or multiply owned predicate use."));
    }
    return out;
}

bool SctRealizationMap::isFresh(const SctRealizationKey& expected) const noexcept {
    return key.project.valid() && key.revision.valid() && key.script.valid()
        && key.realization.valid() && key == expected;
}
std::vector<Diagnostic> SctAuthoringPresentation::validate(const SctAuthoringProject& semantic) const {
    std::vector<Diagnostic> out;
    const auto report = [&](std::string message) {
        out.push_back({DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringPresentation, std::move(message)});
    };
    if (!project.valid() || project != semantic.id) report("Presentation project identity does not match.");
    std::set<SctAuthoringEntityId> placed;
    for (const auto& item : placements) {
        if (!exists(semantic, item.entity) || !placed.insert(item.entity).second)
            report("Presentation placement references a missing or duplicate entity.");
        if (!std::isfinite(item.x) || !std::isfinite(item.y)) report("Presentation coordinates must be finite.");
    }
    std::set<SctAuthoringEntityId> selected;
    for (const auto& item : selection)
        if (!exists(semantic, item) || !selected.insert(item).second)
            report("Presentation selection references a missing or duplicate entity.");
    return out;
}

} // namespace salsa::core
