#include "SalsaCore/Authoring/SctRouteAuthoring.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "SalsaCore/Authoring/SctInterpretation.h"
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
const SctLanguageGroup* SctAuthoringProject::find(SctLanguageGroupId value) const noexcept { return lookup(languageGroups, value); }
const SctAuthoredText* SctAuthoringProject::find(SctTextResourceId value) const noexcept { return lookup(texts, value); }
const SctLogicalText* SctAuthoringProject::find(SctLogicalTextId value) const noexcept { return lookup(logicalTexts, value); }
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
    registerIds(languageGroups,"Language group"); registerIds(texts,"Text resource"); registerIds(logicalTexts,"Logical text");
    registerIds(storylines,"Storyline"); registerIds(meanings,"Meaning"); registerIds(explanations,"Explanation");
    for (const auto& sequence : sequences) registerIds(sequence.actions, "Action");
    for (const auto& story : storylines) registerIds(story.steps, "Story occurrence");
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
    std::set<std::string> recognitionKeys;
    for (const auto& group : languageGroups) {
        if (group.name.empty()) out.push_back(error("Language group name cannot be empty."));
        if (!group.recognitionKey.empty() && !recognitionKeys.insert(group.recognitionKey).second)
            out.push_back(error("Duplicate language recognition key."));
    }
    std::set<std::pair<SctScriptId,spice::sct::SctTextEntityId>> textTargets;
    for (const auto& text : texts) {
        if (!find(text.script) || !std::visit([](auto id){return id.value()!=0;},text.target)
            || !textTargets.emplace(text.script,text.target).second) out.push_back(error("Text target is invalid or multiply owned."));
        std::set<SctLanguageGroupId> groups;
        for (auto group : text.languages) if (!find(group) || !groups.insert(group).second) out.push_back(error("Invalid or duplicate text language assignment."));
        checkEvidence(text.evidence,"Text: ",out);
    }
    for (const auto& logical : logicalTexts) {
        if (!find(logical.script) || logical.name.empty()) out.push_back(error("Invalid logical text identity."));
        if (logical.module && (!find(*logical.module) || find(*logical.module)->script != logical.script)) out.push_back(error("Logical text module belongs to another script."));
        std::set<SctTextResourceId> variants;
        for (auto id : logical.variants) if (!find(id) || find(id)->script != logical.script || !variants.insert(id).second)
            out.push_back(error("Logical text has a missing, foreign, or duplicate resource."));
    }
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
    for (const auto& script : scripts) {
        std::set<std::string> resources;
        for (const auto& r : script.resources)
            if (!smores::script::valid(r.resource) || static_cast<unsigned>(r.role) > 3 || !resources.insert(r.resource.asset).second)
                out.push_back(error("Invalid or duplicate optional resource association."));
    }
    for (const auto& e : entrypoints) {
        const auto actionOwned = [&](SctActionId id) {
            return std::ranges::any_of(sequences,[&](const auto& seq) { return seq.script == e.script
                && std::ranges::any_of(seq.actions,[&](const auto& a) { return a.id == id; }); });
        };
        if (const auto* a = std::get_if<SctNamedActivation>(&e.activation)) {
            if (!actionOwned(a->body) || a->externalName.empty()) out.push_back(error("Named activation has a dangling or foreign body."));
            if ((a->externalName == "init" || a->externalName == "loop") && !e.enabled) out.push_back(error("Required lifecycle entrypoints are always enabled."));
        }
        if (const auto* a = std::get_if<SctLocalActivation>(&e.activation))
            if (!actionOwned(a->source) || !actionOwned(a->start) || !actionOwned(a->continuation)) out.push_back(error("Local activation has a dangling or foreign action."));
        if (e.binding && e.binding->kind == SctEntrypointKind::Lifecycle && !e.enabled)
            out.push_back(error("Required lifecycle entrypoints are always enabled."));
        if (const auto* u = std::get_if<SctUnresolvedBinding>(&e.activation)) {
            if (u->originalEvidence.empty() || u->explanation.empty()) out.push_back(error("Unresolved activation requires an explanation."));
            checkEvidence(u->evidence, "Activation: ", out);
            out.push_back({DiagnosticSeverity::Warning,DiagnosticCode::UnresolvedSctAuthoringBinding,e.name + ": " + u->explanation});
        }
    }
    for (const auto& entry : entrypoints) if (entry.binding) {
        const auto& b = *entry.binding;
        const auto* sequence = find(b.sequence);
        if (!sequence || sequence->script != entry.script || static_cast<unsigned>(b.kind) > 4 || static_cast<unsigned>(b.resourceRole) > 3)
            out.push_back(error("Entrypoint has invalid kind, role, or sequence ownership."));
        checkReference(b.response, "Entrypoint response: ");
        if (const auto* id = std::get_if<SctPortId>(&b.response)) {
            const auto* port = find(*id); const auto* moduleId = port ? std::get_if<SctModuleId>(&port->owner) : nullptr;
            const auto* module = moduleId ? find(*moduleId) : nullptr;
            if (!module || module->script != entry.script) out.push_back(error("Entrypoint response must belong to a module in its parent script."));
        }
        if ((b.kind == SctEntrypointKind::SignalConsumer || b.kind == SctEntrypointKind::Arrival) && !b.predicate)
            out.push_back(error("Signal and arrival entrypoints require their existing predicate."));
        if ((b.kind == SctEntrypointKind::SignalConsumer || b.kind == SctEntrypointKind::DirectHandler) && !b.provider)
            out.push_back(error("Resource entrypoint requires a selected or explicitly unresolved provider."));
        if (b.kind == SctEntrypointKind::SignalConsumer && (!b.signalBit || *b.signalBit > 0xffffff))
            out.push_back(error("Signal entrypoint requires a valid physical bit."));
        if (b.kind != SctEntrypointKind::SignalConsumer && b.signalBit) out.push_back(error("Only signal consumers have a signal bit."));
        if (b.provider) {
            if (const auto* provider = std::get_if<smores::script::Entry>(&*b.provider)) {
                if (!smores::script::valid(provider->resource) || !provider->localId)
                    out.push_back(error("Entrypoint provider has invalid source qualification."));
                if (b.kind == SctEntrypointKind::SignalConsumer && (smores::script::assetKey(provider->function) != "eventhook" || provider->parameters.size() <= 8 || provider->parameters[8] != b.signalBit))
                    out.push_back(error("Eventhook provider does not agree with the selected candidate bit."));
            } else {
                const auto& u = std::get<SctUnresolvedBinding>(*b.provider);
                if (u.originalEvidence.empty() || u.explanation.empty()) out.push_back(error("Unknown provider requires evidence and explanation."));
                checkEvidence(u.evidence, "Entrypoint provider: ", out);
                out.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding, u.explanation});
            }
        }
    }
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
        if (item.authoredState) {
            if (item.physicalPatch || !item.literalOverrides.empty()) out.push_back(error(where + "authored content cannot have a second patch authority."));
            auto encoded = SctProgramStateCodec::encode(*item.authoredState);
            if (!encoded) out.insert(out.end(), encoded.diagnostics().begin(), encoded.diagnostics().end());
        }
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
    std::set<std::pair<SctScriptId, spice::sct::SctSectionId>> moduleSections;
    for (const auto& module : modules) for (const auto section : module.sections) {
        if (!section || !moduleSections.emplace(module.script, section).second)
            out.push_back(error("A module section has invalid or multiple ownership."));
        const auto seq = std::ranges::find_if(sequences, [&](const auto& item) { return item.script == module.script && item.section == section; });
        if (seq == sequences.end()) out.push_back(error("A module section requires an authored sequence."));
        bool found = false;
        for (const auto& content : contents) if (effectiveScript(content.owner) == module.script && content.authoredState && content.authoredState->document)
            for (const auto& item : content.authoredState->document->sections)
                if (item.id == section && std::holds_alternative<spice::sct::SctScriptSectionContent>(item.content)) found = true;
        if (!found) out.push_back(error("Module section is absent from its parent's authored program."));
    }
    for (const auto& port : ports) if (port.sequence) {
        const auto* sequence = find(*port.sequence);
        const auto* owner = std::get_if<SctModuleId>(&port.owner);
        const auto* module = owner ? find(*owner) : nullptr;
        if (!sequence || !module || sequence->script != module->script
            || std::ranges::find(module->sections, sequence->section) == module->sections.end()
            || (port.action && std::ranges::find(sequence->actions, *port.action, &SctSequenceAction::id) == sequence->actions.end()))
            out.push_back(error("Port sequence/action binding does not belong to its module."));
    }
    for (const auto& port : ports) if (port.action) {
        const auto* ownerId = std::get_if<SctModuleId>(&port.owner);
        const auto* module = ownerId ? find(*ownerId) : nullptr;
        bool found = false;
        if (module) for (const auto& seq : sequences)
            if (seq.script == module->script && std::ranges::find(module->sections, seq.section) != module->sections.end())
                for (const auto& action : seq.actions) if (action.id == *port.action) found = true;
        if (!found) out.push_back(error("Port action does not belong to its module."));
    }
    for (const auto& content : contents) if (content.authoredState && content.authoredState->document) {
        const auto script = effectiveScript(content.owner);
        if (script) { auto valid = SctEntrypointAuthoring::validateProgram(*this, *script, *content.authoredState->document);
            if (valid) valid = SctTextAuthoring::validateProgram(*this,*script,*content.authoredState->document);
            if (!valid) out.insert(out.end(), valid.diagnostics().begin(), valid.diagnostics().end()); }
    }
    auto interpretations = SctInterpretation::validate(*this);
    out.insert(out.end(), interpretations.begin(), interpretations.end());
    const auto routes = SctRouteAuthoring::validate(*this);
    out.insert(out.end(),routes.begin(),routes.end());
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
    if (!std::isfinite(storyZoom) || storyZoom <= 0) report("Story zoom must be finite and positive.");
    std::set<std::pair<SctMeaningTarget,std::optional<SctStoryStepId>>> storyTargets;
    auto probe = semantic;
    probe.meanings.clear(); probe.explanations.clear(); probe.storylines.clear();
    for (const auto& item : storyPlacements) {
        if (item.occurrence) {
            if (!item.occurrence->valid() || item.occurrence->value >= semantic.nextEntityId)
                report("Invalid story occurrence identity.");
            for (const auto& story : semantic.storylines) for (const auto& step : story.steps)
                if (step.id == *item.occurrence && step.target != item.target) report("Story occurrence target mismatch.");
        }
        // Historical placements may outlive their source item, like aliases.
        probe.meanings.clear();
        probe.meanings.push_back({{}, item.target, "placement"});
        if (hasErrors(SctInterpretation::validate(probe)) || !storyTargets.insert({item.target,item.occurrence}).second)
            report("Story placement has an invalid or duplicate target.");
        if (!std::isfinite(item.x) || !std::isfinite(item.y)) report("Story coordinates must be finite.");
    }
    std::set<SctAuthoringEntityId> selected;
    for (const auto& item : selection)
        if (!exists(semantic, item) || !selected.insert(item).second)
            report("Presentation selection references a missing or duplicate entity.");
    return out;
}

} // namespace salsa::core
