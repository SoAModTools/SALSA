#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SpiceMLD/MldDocumentImporter.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <set>
namespace salsa::core {
namespace {
namespace sct = spice::sct;
namespace rel = smores::script;
Diagnostic error(std::string message) { return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)}; }
const sct::SctDocumentParameter* parameter(const sct::SctDocument& d, const sct::SctParameterSite& site) {
    const auto index = sct::SctDocumentIndex::build(d); const auto* i = index.find(d, site.instruction); if (!i) return nullptr;
    if (site.parameter.repeatedGroupOrdinal && *site.parameter.repeatedGroupOrdinal >= i->repeatedParameterGroups.size()) return nullptr;
    const auto& ps = site.parameter.repeatedGroupOrdinal ? i->repeatedParameterGroups[*site.parameter.repeatedGroupOrdinal].parameters : i->fixedParameters;
    const auto found = std::ranges::find(ps, site.parameter.schemaIndex, &sct::SctDocumentParameter::schemaIndex);
    return found == ps.end() ? nullptr : &*found;
}
std::optional<sct::SctInstructionId> target(const SctAuthoringProject& p, SctPortId id, SctScriptId script) {
    const auto* port = p.find(id); if (!port || !port->action) return {};
    const auto* owner = std::get_if<SctModuleId>(&port->owner); const auto* module = owner ? p.find(*owner) : nullptr;
    const auto* action = p.find(*port->action); if (!module || module->script != script || !action) return {};
    return action->instruction;
}
}
Result<rel::Resource> SctEntrypointAuthoring::inspectResource(const GameProjectContext& context,
    const AssetLocator& locator, rel::ResourceRole role) {
    auto source = context.assets().loadAsset(locator); if (!source) return Result<rel::Resource>::failure(source.diagnostics());
    const auto hash = sha256(source.value().bytes); if (!hash) return Result<rel::Resource>::failure(hash.diagnostics());
    if (hash.value() != source.value().descriptor.revision.digest) return Result<rel::Resource>::failure(error("MLD source changed while loading."));
    spice::mld::MldImportOptions options;
    if (context.dataset().identity.platform) options.platformHint = *context.dataset().identity.platform == GamePlatform::GameCube ? spice::mld::MldPlatform::GameCube : spice::mld::MldPlatform::Dreamcast;
    const auto& bytes = source.value().bytes;
    auto imported = spice::mld::MldDocumentImporter::importBytes({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, options);
    if (!imported.ok() || !imported.document) return Result<rel::Resource>::failure(error("SPICE could not inspect this MLD."));
    // Stable deterministic import identity names this adapter contract + exact bytes.
    // A future incompatible importer bumps this discriminator, forcing re-resolution.
    rel::ResourceIdentity identity{context.dataset().identity.fingerprint.digest.toHex(), locator.identityKey(), hash.value().toHex(), "spice-mld-s6-v1:" + hash.value().toHex()};
    return Result<rel::Resource>::success(rel::describe(identity, role, *imported.document));
}
Result<void> SctEntrypointAuthoring::verifyResources(const SctAuthoringProject& project, SctScriptId script, const GameProjectContext& context) {
    rel::Catalog catalog;
    if (const auto* owner = project.find(script)) for (const auto& association : owner->resources) {
        auto locator = AssetLocator::fromRelativePath(association.resource.asset); if (!locator) continue;
        auto resource = inspectResource(context, locator.value(), association.role);
        if (resource) catalog.resources.push_back(std::move(resource).takeValue());
    }
    for (const auto& e : project.entrypoints) if (e.script == script && e.binding && e.binding->provider)
        if (const auto* entry = std::get_if<rel::Entry>(&*e.binding->provider)) {
            if (std::ranges::any_of(catalog.resources, [&](const auto& r) { return r.identity.asset == entry->resource.asset; })) continue;
            auto locator = AssetLocator::fromRelativePath(entry->resource.asset); if (!locator) continue;
            auto resource = inspectResource(context, locator.value(), e.binding->resourceRole);
            if (!resource) continue;
            catalog.resources.push_back(std::move(resource).takeValue());
        }
    auto diagnostics = inspectBindings(project, script, catalog);
    return Result<void>::success(std::move(diagnostics));
}
std::vector<Diagnostic> SctEntrypointAuthoring::inspectBindings(const SctAuthoringProject& project, SctScriptId script, const rel::Catalog& catalog) {
    std::vector<Diagnostic> result;
    if (const auto* context = project.find(script)) for (const auto& a : context->resources) {
        const auto matches = std::ranges::count_if(catalog.resources,[&](const auto& r) { return r.identity == a.resource; });
        if (matches != 1) result.push_back({DiagnosticSeverity::Warning,DiagnosticCode::UnresolvedSctAuthoringBinding,a.resource.asset + ": optional resource context is unavailable, changed, or ambiguous."});
    }
    for (const auto& e : project.entrypoints) if (e.script == script && e.binding && e.binding->provider) {
        if (const auto* source = std::get_if<rel::Entry>(&*e.binding->provider)) {
            const auto resolved = rel::resolve(*source, catalog);
            if (resolved.status != rel::Status::Exact) result.push_back({DiagnosticSeverity::Warning,
                DiagnosticCode::UnresolvedSctAuthoringBinding, e.name + ": " + resolved.explanation});
        } else result.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding, e.name + ": provider unresolved."});
    }
    return result;
}
Result<void> SctEntrypointAuthoring::validateProgram(const SctAuthoringProject& project, SctScriptId script, const sct::SctDocument& doc, bool forOutput) {
    const auto index = sct::SctDocumentIndex::build(doc);
    if (forOutput) for (const auto name : {"init", "loop"}) {
        const auto count = std::ranges::count_if(doc.sections, [&](const auto& s) {
            const auto* body = std::get_if<sct::SctScriptSectionContent>(&s.content);
            return s.nameBytes == name && body && !body->instructions.empty();
        });
        if (count != 1) return Result<void>::failure(error(std::string("Export requires one nonempty ") + name + " lifecycle section. Repair the imported draft explicitly."));
    }
    auto activationValid = validateActivations(project, script, doc);
    if (!activationValid) return activationValid;
    for (const auto& e : project.entrypoints) if (e.script == script) {
        if (!e.binding) { if (forOutput) return Result<void>::failure(error("Entrypoint has no execution binding.")); continue; }
        const auto& b = *e.binding; const auto* seq = project.find(b.sequence);
        if (!seq || seq->script != script) return Result<void>::failure(error("Entrypoint sequence is missing or belongs to another script."));
        const auto* section = index.find(doc, seq->section); const auto* body = section ? std::get_if<sct::SctScriptSectionContent>(&section->content) : nullptr;
        if (!body) return Result<void>::failure(error("Entrypoint section is missing."));
        if (b.kind == SctEntrypointKind::Lifecycle && section->nameBytes != "init" && section->nameBytes != "loop")
            return Result<void>::failure(error("Lifecycle entrypoints require an existing init or loop section."));
        if (b.kind == SctEntrypointKind::DirectHandler && b.provider) if (const auto* provider = std::get_if<rel::Entry>(&*b.provider)) {
            const auto name = rel::handlerName(provider->tableId);
            if (!name || section->nameBytes != *name) return Result<void>::failure(error("Handler section no longer matches the selected entry tblId."));
        }
        if (b.predicate) {
            const auto loc = index.instructionLocation(b.predicate->instruction); const auto* p = parameter(doc, *b.predicate);
            const auto* expr = p ? std::get_if<sct::SctCanonicalExpression>(&p->value) : nullptr;
            const auto* instruction = index.find(doc, b.predicate->instruction);
            if (!loc || loc->sectionId != seq->section || !expr || !instruction || (instruction->opcode != 0 && instruction->opcode != 3)
                || b.predicate->parameter.schemaIndex != 0 || b.predicate->parameter.repeatedGroupOrdinal)
                return Result<void>::failure(error("Entrypoint predicate must remain an existing condition in its sequence."));
            const auto reads = rel::reads(*expr);
            if (b.kind == SctEntrypointKind::SignalConsumer && (!b.signalBit || !std::ranges::any_of(reads, [&](const auto& r) { return r.kind == sct::SctScptValueKind::BitVariable && r.index == *b.signalBit; })))
                return Result<void>::failure(error("Signal predicate no longer reads the selected physical bit."));
            if (b.kind == SctEntrypointKind::Arrival && !std::ranges::any_of(reads, [](const auto& r) { return r.kind == sct::SctScptValueKind::IntegerVariableLow16Comparison && r.index == 15; }))
                return Result<void>::failure(error("Arrival predicate must read Low16IntVar[15]."));
        }
        if (b.responseSite) {
            const auto* p = parameter(doc, *b.responseSite);
            const auto loc = index.instructionLocation(b.responseSite->instruction);
            if (!p || !std::holds_alternative<sct::SctInstructionReference>(p->value) || !loc || loc->sectionId != seq->section)
                return Result<void>::failure(error("Response site is not an existing target operand in its sequence."));
            if (b.predicate) {
                const auto guard = index.instructionLocation(b.predicate->instruction);
                if (guard && loc->instructionOrdinal < guard->instructionOrdinal)
                    return Result<void>::failure(error("Response site precedes its predicate in source order."));
            }
        }
        if (const auto* port = std::get_if<SctPortId>(&b.response)) {
            const auto expected = target(project, *port, script); if (!expected) return Result<void>::failure(error("Response needs a nonempty module action port in the same script."));
            if (b.responseSite) {
                const auto* p = parameter(doc, *b.responseSite); const auto* ref = p ? std::get_if<sct::SctInstructionReference>(&p->value) : nullptr;
                const auto loc = index.instructionLocation(b.responseSite->instruction);
                if (!loc || loc->sectionId != seq->section || !ref || ref->target != *expected)
                    return Result<void>::failure(error("Entrypoint response operand no longer targets its selected module port."));
            } else if (body->instructions.empty() || body->instructions.front().id != *expected)
                return Result<void>::failure(error("Section entry no longer starts at its module response port."));
        } else if (forOutput && b.kind != SctEntrypointKind::Lifecycle && b.kind != SctEntrypointKind::NamedSection)
            return Result<void>::failure(error("Entrypoint response is unresolved."));
    }
    return Result<void>::success();
}
Result<SctAuthoringState> SctEntrypointAuthoring::bind(const SctAuthoringState& state, SctScriptId script,
    std::string name, SctEntrypointBinding binding, const rel::Catalog& catalog, std::optional<SctEntrypointId> replace) {
    const auto* context = state.project.find(script); if (!context) return Result<SctAuthoringState>::failure(error("Script is missing."));
    if (binding.provider) if (const auto* entry = std::get_if<rel::Entry>(&*binding.provider)) {
        const auto* previous = replace ? state.project.find(*replace) : nullptr;
        const auto* retained = previous && previous->binding && previous->binding->provider
            ? std::get_if<rel::Entry>(&*previous->binding->provider) : nullptr;
        // Editing a response retains its selected evidence even when the MLD is
        // unavailable. Only a new selection requires a fresh inspected snapshot.
        if (!retained || *retained != *entry || previous->binding->resourceRole != binding.resourceRole) {
            const auto resolution = rel::resolve(*entry, catalog);
            if (resolution.status != rel::Status::Exact) return Result<SctAuthoringState>::failure(error(resolution.explanation));
        }
        // The selected resource snapshot may be newer than the immutable SCT
        // import baseline. Explicit selection, not baseline fingerprint equality,
        // establishes this authored association.
    }
    auto next = state;
    if (replace) {
        const auto* old = next.project.find(*replace); if (!old || old->script != script) return Result<SctAuthoringState>::failure(error("Cannot reparent an entrypoint."));
        std::ranges::find(next.project.entrypoints, *replace, &SctAuthoringEntrypoint::id)->binding.reset();
    }
    if (binding.responseSite) if (const auto* port = std::get_if<SctPortId>(&binding.response)) {
        auto working = SctAuthoringMaterializer::workingState(next.project, next.programs, script); if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
        const auto destination = target(next.project, *port, script); if (!destination) return Result<SctAuthoringState>::failure(error("Choose a nonempty module response port."));
        auto document = std::make_shared<sct::SctDocument>(*working.value().document);
        const auto* p = parameter(*document, *binding.responseSite);
        if (!p || !std::holds_alternative<sct::SctInstructionReference>(p->value)) return Result<SctAuthoringState>::failure(error("Choose an existing call/branch target operand."));
        // Only the selected existing operand changes. Conditions, clears, timing,
        // caller preparation and continuations retain their exact source order.
        const_cast<sct::SctDocumentParameter*>(p)->value = sct::SctInstructionReference{*destination};
        working.value().document = document;
        auto changed = SctAuthoringMaterializer::replaceWorkingState(next.project, next.programs, script, working.value());
        if (!changed) return Result<SctAuthoringState>::failure(changed.diagnostics()); next.project = std::move(changed).takeValue();
    }
    auto id = replace ? Result<SctEntrypointId>::success(*replace) : next.project.allocate<SctEntrypointId>();
    if (!id) return Result<SctAuthoringState>::failure(id.diagnostics());
    std::vector<SctAuthoringEvidence> evidence{{SctEvidenceKind::UserAssertion, "Explicit entrypoint/response binding; existing predicate and consumption order retained."}};
    if (binding.provider && std::holds_alternative<rel::Entry>(*binding.provider)) {
        evidence.push_back({SctEvidenceKind::SourceObservation, "Captured MLD entry coordinates and function parameters from the selected immutable snapshot."});
        evidence.push_back({SctEvidenceKind::Inference, binding.kind == SctEntrypointKind::SignalConsumer
            ? "Eventhook function parameter 8 is a candidate bit binding; runtime signaling is not proven."
            : "MLD tblId matches the handler name; runtime dispatch and residency are not proven."});
    }
    if (replace) {
        auto& entry = *std::ranges::find(next.project.entrypoints, *replace, &SctAuthoringEntrypoint::id);
        entry.name = std::move(name); entry.binding = std::move(binding); entry.evidence.insert(entry.evidence.end(), evidence.begin(), evidence.end());
    } else next.project.entrypoints.push_back({id.value(), script, std::move(name), {}, std::move(evidence), std::move(binding)});
    auto valid = SctAuthoringSession::validate(next); if (!valid) return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<SctAuthoringState> SctEntrypointAuthoring::createDirectHandler(const SctAuthoringState& state, SctScriptId script,
    const rel::Entry& entry, rel::ResourceRole role, SctPortId response, const rel::Catalog& catalog) {
    const auto name = rel::handlerName(entry.tableId); const auto destination = target(state.project, response, script);
    if (!name || !destination) return Result<SctAuthoringState>::failure(error("Choose a valid handler entry and nonempty module response."));
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script); if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
    auto document = std::make_shared<sct::SctDocument>(*working.value().document);
    if (std::ranges::any_of(document->sections, [&](const auto& s) { return s.nameBytes == *name; }))
        return Result<SctAuthoringState>::failure(error("Handler already exists; bind its existing section or response operand explicitly."));
    if (!document->sections.empty()) if (const auto* tail = std::get_if<sct::SctScriptSectionContent>(&document->sections.back().content)) {
        if (tail->instructions.empty() || (tail->instructions.back().opcode != 12 && tail->instructions.back().opcode != 10 && tail->instructions.back().opcode != 3))
            return Result<SctAuthoringState>::failure(error("Adding a handler would extend an unterminated trailing sequence. Define its continuation explicitly first."));
    }
    const auto section = document->allocateSectionId();
    const auto guard = document->allocateInstructionId(), call = document->allocateInstructionId(), ret = document->allocateInstructionId();
    const auto yes = SctExpressionLanguage::parse("1");
    document->sections.push_back({section, *name, sct::SctScriptSectionContent{{
        {guard,0,true,{},{{0,*yes.expression},{1,sct::SctInstructionReference{ret}}}},
        {call, 11, false, {}, {{0, sct::SctInstructionReference{*destination}}}}, {ret, 12}}}});
    working.value().document = document;
    auto changed = SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, script, working.value());
    if (!changed) return Result<SctAuthoringState>::failure(changed.diagnostics());
    auto next = state; next.project = std::move(changed).takeValue();
    auto promoted = SctSequenceAuthoring::promoteSequence(next, script, section, *name); if (!promoted) return promoted;
    next = std::move(promoted).takeValue();
    const auto seq = std::ranges::find(next.project.sequences, section, &SctAuthoredSequence::section);
    SctEntrypointBinding b{SctEntrypointKind::DirectHandler, seq->id, sct::SctParameterSite{call, {0, {}}}, {}, response, entry, role};
    const auto body = seq->actions.front().id;
    auto bound = bind(next, script, *name, std::move(b), catalog);
    if (!bound) return bound;
    return setActivation(bound.value(),bound.value().project.entrypoints.back().id,SctNamedActivation{*name,body});
}
}
