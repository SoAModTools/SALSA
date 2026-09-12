#include "SalsaCore/Authoring/SctRouteAuthoring.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctScptEncoding.h"
#include "SpiceSCT/SctStructuredControlFlow.h"
#include <map>
#include <set>
namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string s) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(s)};
}
const SctAuthoredSequence *sequence(const SctAuthoringProject &p, SctActionId a) {
    for (const auto &s : p.sequences)
        if (std::ranges::any_of(s.actions, [&](const auto &x) { return x.id == a; }))
            return &s;
    return nullptr;
}
const sct::SctDocument *document(const SctAuthoringProject &p, SctScriptId id) {
    for (const auto &c : p.contents)
        if (p.effectiveScript(c.owner) == id && c.authoredState)
            return c.authoredState->document.get();
    return nullptr;
}
const sct::SctDocumentParameter *parameter(const sct::SctDocument &d, sct::SctParameterSite site) {
    auto index = sct::SctDocumentIndex::build(d);
    auto *i = index.find(d, site.instruction);
    if (!i)
        return nullptr;
    if (site.parameter.repeatedGroupOrdinal &&
        *site.parameter.repeatedGroupOrdinal >= i->repeatedParameterGroups.size())
        return nullptr;
    const auto &ps = site.parameter.repeatedGroupOrdinal
                         ? i->repeatedParameterGroups[*site.parameter.repeatedGroupOrdinal].parameters
                         : i->fixedParameters;
    auto it = std::ranges::find(ps, site.parameter.schemaIndex, &sct::SctDocumentParameter::schemaIndex);
    return it == ps.end() ? nullptr : &*it;
}
std::optional<SctScriptId> portScript(const SctAuthoringProject &p, SctPortId id) {
    auto *port = p.find(id);
    return port ? std::visit([&](auto owner) { return p.effectiveScript(SctContentOwner{owner}); }, port->owner)
                : std::nullopt;
}
std::string assetName(const SctAuthoringProject &p, SctScriptId id, bool stem) {
    auto *s = p.find(id);
    auto *b = s ? p.find(s->baseline) : nullptr;
    if (!b)
        return {};
    auto path = std::filesystem::path(b->source.locator.identityKey());
    return (stem ? path.stem() : path.filename()).string();
}
bool relevant(const SctExecutableRoute &r, std::optional<SctScriptId> s) {
    return !s || *s == r.sourceScript || *s == r.destinationScript;
}
bool selectedArmContains(const sct::SctDocument &doc, sct::SctInstructionId guard, sct::SctInstructionId response,
                         std::optional<std::uint32_t> ordinal) {
    auto structure = sct::SctStructuredControlFlowAnalysis::build(doc);
    for (const auto &section : structure.sections())
        for (const auto &region : section.regions)
            if (region.header.entryInstruction == guard)
                for (const auto &arm : region.arms) {
                    const bool selected = ordinal
                                              ? std::ranges::any_of(arm.caseLabels,
                                                                    [&](const auto &label) {
                                                                        return label.repeatedGroupOrdinal == *ordinal;
                                                                    })
                                              : arm.kind == sct::SctStructuredArmKind::Then;
                    if (selected)
                        for (const auto &id : arm.blocks)
                            for (const auto &block : section.blocks)
                                if (block.id == id &&
                                    std::ranges::find(block.instructions, response) != block.instructions.end())
                                    return true;
                }
    return false;
}
std::string currentFailure(const SctAuthoringProject &p, const SctAuthoringConnection &c) {
    const auto &r = *c.route;
    auto *e = p.find(r.arrival);
    auto *src = std::get_if<SctPortId>(&c.source);
    auto *dst = std::get_if<SctPortId>(&c.destination);
    if (!src || !dst || portScript(p, *src) != r.sourceScript || portScript(p, *dst) != r.destinationScript)
        return "Route endpoint ownership is missing or changed.";
    auto *a = p.find(r.sourceAction);
    auto *seq = sequence(p, r.sourceAction);
    if (!a || !seq || seq->script != r.sourceScript || p.find(*src)->action != r.sourceAction)
        return "Source action binding changed.";
    if (!e || e->script != r.destinationScript || !e->enabled || !e->binding ||
        e->binding->kind != SctEntrypointKind::Arrival || !e->binding->predicate || !e->binding->responseSite)
        return "Destination needs an enabled arrival entry with explicit condition and response.";
    auto *response = std::get_if<SctPortId>(&e->binding->response);
    if (!response || *response != *dst)
        return "Arrival response changed; explicitly reconnect the route.";
    if (r.kind != SctTransitionKind::LiteralLoad)
        return "This transition producer and arrival pair is not qualified for authoring.";
    if (r.sourceStem != assetName(p, r.sourceScript, true) ||
        r.targetFilename != assetName(p, r.destinationScript, false))
        return "Game asset binding changed.";
    const auto *sc = p.find(r.sourceScript);
    const auto *dc = p.find(r.destinationScript);
    if (sc->platform != dc->platform || sc->region != dc->region)
        return "Route endpoints have incompatible release profiles.";
    if (r.sourceOperand.schemaIndex != 0 || r.sourceOperand.repeatedGroupOrdinal)
        return "Ordinary load requires its fixed filename operand.";
    auto *d = document(p, r.sourceScript);
    if (!d)
        return "Source authored program is missing.";
    auto *i = sct::SctDocumentIndex::build(*d).find(*d, a->instruction);
    auto *v = parameter(*d, {a->instruction, r.sourceOperand});
    auto *ref = v ? std::get_if<sct::SctSupplementaryTextReference>(&v->value) : nullptr;
    if (!i || i->opcode != 43 || !ref)
        return "Source is no longer an ordinary literal load.";
    auto text = std::ranges::find(d->supplementaryText, ref->target, &sct::SctDocumentSupplementaryText::id);
    auto *plain = text == d->supplementaryText.end() ? nullptr : std::get_if<sct::SctPlainText>(&text->value);
    if (!plain || plain->utf8 != r.targetFilename)
        return "Source filename no longer matches its selected game asset.";
    if (r.observations.empty())
        return "Route observations are missing.";
    bool source = false, arrival = false, responseSeen = false;
    for (const auto &o : r.observations) {
        auto *os = sequence(p, o.action);
        auto *oa = p.find(o.action);
        auto *od = document(p, o.script);
        if (!os || !oa || !od || os->script != o.script)
            return "Observed route action was removed or reparented.";
        auto fingerprint = SctProgramStateCodec::instructionFingerprint(*od, oa->instruction);
        if (!fingerprint || fingerprint.value() != o.fingerprint)
            return "A route instruction was edited; inspect and explicitly reconnect it.";
        source |= o.role == SctRouteObservationRole::Source && o.action == r.sourceAction && o.script == r.sourceScript;
        arrival |= o.role == SctRouteObservationRole::Arrival &&
                   oa->instruction == e->binding->predicate->instruction && o.script == r.destinationScript;
        responseSeen |= o.role == SctRouteObservationRole::Response &&
                        oa->instruction == e->binding->responseSite->instruction && o.script == r.destinationScript;
    }
    if (!source || !arrival || !responseSeen)
        return "Required endpoint observations are missing.";
    auto location = SctSequenceAuthoring::arrivalLocation(r.sourceStem);
    if (!location)
        return location.diagnostics().front().message;
    auto *dd = document(p, r.destinationScript);
    if (!dd)
        return "Destination authored program is missing.";
    const auto &responseSite = *e->binding->responseSite;
    const bool directCase = r.arrivalCase && responseSite.instruction == e->binding->predicate->instruction &&
                            responseSite.parameter.schemaIndex == 3 &&
                            responseSite.parameter.repeatedGroupOrdinal == r.arrivalCase;
    if (!directCase &&
        !selectedArmContains(*dd, e->binding->predicate->instruction, responseSite.instruction, r.arrivalCase))
        return "Response is outside the selected arrival arm, or the boundary is not structurally recognized. Select "
               "an explicit supported block.";
    auto *cond = parameter(*dd, *e->binding->predicate);
    auto *expression = cond ? std::get_if<sct::SctCanonicalExpression>(&cond->value) : nullptr;
    if (!expression)
        return "Arrival condition is missing.";
    if (r.arrivalCase) {
        auto expected = SctExpressionLanguage::parse("Low16IntVar[15]");
        auto *value = parameter(*dd, {e->binding->predicate->instruction, {2, *r.arrivalCase}});
        auto *word = value ? std::get_if<sct::SctEncodedWordValue>(&value->value) : nullptr;
        if (sct::encodeSctCanonicalExpressionWords(*expression) !=
                sct::encodeSctCanonicalExpressionWords(*expected.expression) ||
            !word || word->value != location.value().encoded)
            return "Arrival case changed.";
    } else {
        auto expected = SctSequenceAuthoring::arrivalCondition(r.sourceStem);
        if (!expected ||
            SctExpressionLanguage::project(*expression).text != SctExpressionLanguage::project(expected.value()).text)
            return "Arrival comparison changed or is not a supported equality.";
    }
    if (r.discriminatorBit) {
        bool produced = false, required = false;
        const auto condition = SctExpressionLanguage::parse("BitVar[" + std::to_string(*r.discriminatorBit) +
                                                            "] == " + (r.discriminatorValue ? "1" : "0"));
        for (const auto &o : r.observations) {
            auto *action = p.find(o.action);
            auto *doc = document(p, o.script);
            if (!action || !doc)
                continue;
            auto ix = sct::SctDocumentIndex::build(*doc);
            auto *instruction = ix.find(*doc, action->instruction);
            if (!instruction)
                continue;
            auto *parameter0 = parameter(*doc, {instruction->id, {0, {}}});
            auto *expression0 = parameter0 ? std::get_if<sct::SctCanonicalExpression>(&parameter0->value) : nullptr;
            if (!expression0)
                continue;
            if (o.role == SctRouteObservationRole::Effect && o.script == r.sourceScript &&
                instruction->opcode == (r.discriminatorValue ? 17 : 18) &&
                SctExpressionLanguage::project(*expression0).text == std::to_string(*r.discriminatorBit)) {
                auto from = ix.instructionLocation(instruction->id), to = ix.instructionLocation(a->instruction);
                produced = from && to && from->sectionId == to->sectionId &&
                           from->instructionOrdinal + 1 == to->instructionOrdinal;
            }
            if (o.role == SctRouteObservationRole::Requirement && o.script == r.destinationScript &&
                instruction->opcode == 0 && condition.expression &&
                SctExpressionLanguage::project(*expression0).text ==
                    SctExpressionLanguage::project(*condition.expression).text)
                required = selectedArmContains(*doc, instruction->id, responseSite.instruction, {});
        }
        if (!produced || !required)
            return "A bit discriminator requires an explicitly selected set/clear immediately before the load and a "
                   "matching destination equality enclosing its response. No producer is synthesized.";
    }
    return {};
}
} // namespace
std::vector<SctTransitionSupport> SctRouteAuthoring::support() {
    const auto *schema = sct::findSctOpcodeSchema(43);
    const bool qualified = schema && schema->semantic.effect.kind == sct::SctOpcodeEffectKind::LoadScript;
    return {{SctTransitionKind::LiteralLoad, 43, qualified,
             "Literal filename load; engine origin encoded as script number * 10 + letter a-j."},
            {SctTransitionKind::Warp, {}, false, "Warp producer/arrival pairing is not qualified."},
            {SctTransitionKind::SavedAreaReturn, {}, false, "Saved-area and overworld return context is unresolved."},
            {SctTransitionKind::Battle, {}, false, "Battle return 10000 remains a tentative label."},
            {SctTransitionKind::ShipBattleExit, {}, false, "Ship battle return 40000 remains a tentative label."}};
}
std::vector<Diagnostic> SctRouteAuthoring::validate(const SctAuthoringProject &p, std::optional<SctScriptId> script,
                                                    bool output) {
    std::vector<Diagnostic> ds;
    std::set<std::pair<SctScriptId, SctActionId>> sources;
    std::map<std::pair<SctScriptId, std::string>, std::vector<const SctExecutableRoute *>> arrivals;
    for (const auto &c : p.connections)
        if (c.route) {
            const auto &r = *c.route;
            if (!relevant(r, script))
                continue;
            auto report = [&](std::string s) { ds.push_back(error("Route " + std::to_string(c.id.value) + ": " + s)); };
            if (!p.find(r.sourceScript) || !p.find(r.destinationScript) || r.sourceScript == r.destinationScript) {
                report("Cross-script endpoints are missing or identical.");
                continue;
            }
            if (static_cast<unsigned>(r.kind) > 4) {
                report("Invalid transition kind.");
                continue;
            }
            if (!r.sourceAction.valid() || !r.arrival.valid()) {
                report("Route identities must be nonzero, including retained draft evidence.");
                continue;
            }
            for (const auto &o : r.observations) {
                const bool sourceRole =
                    o.role == SctRouteObservationRole::Source || o.role == SctRouteObservationRole::Prerequisite ||
                    o.role == SctRouteObservationRole::Effect || o.role == SctRouteObservationRole::Continuation;
                if (static_cast<unsigned>(o.role) > 6 || !o.action.valid() ||
                    o.script != (sourceRole ? r.sourceScript : r.destinationScript) || o.fingerprint.size() != 64 ||
                    !std::ranges::all_of(o.fingerprint,
                                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                    report("Malformed route observation or ownership.");
            }
            if (r.unresolved) {
                if (r.unresolved->originalEvidence.empty() || r.unresolved->explanation.empty())
                    report("Unresolved intent requires original evidence and an explanation.");
                else
                    ds.push_back({output ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
                                  DiagnosticCode::UnresolvedSctAuthoringBinding,
                                  "Route " + std::to_string(c.id.value) + ": " + r.unresolved->explanation});
                continue;
            }
            auto failure = currentFailure(p, c);
            if (!failure.empty())
                report(failure);
            if (!sources.emplace(r.sourceScript, r.sourceAction).second)
                report("One source load action cannot own multiple routes.");
            auto &prior = arrivals[{r.destinationScript, r.sourceStem}];
            for (const auto *other : prior)
                if (std::pair{r.arrival, r.arrivalCase} != std::pair{other->arrival, other->arrivalCase}) {
                    const bool disjoint = r.discriminatorBit && other->discriminatorBit == r.discriminatorBit &&
                                          other->discriminatorValue != r.discriminatorValue;
                    if (!disjoint)
                        report("These sources have indistinguishable engine origin values. Select one shared arrival "
                               "or explicit disjoint bit producers and requirements.");
                }
            prior.push_back(&r);
        }
    return ds;
}
void SctRouteAuthoring::reconcile(SctAuthoringProject &p, SctScriptId script) {
    for (auto &c : p.connections)
        if (c.route && !c.route->unresolved && relevant(*c.route, script)) {
            auto why = currentFailure(p, c);
            if (!why.empty())
                c.route->unresolved = SctUnresolvedBinding{"Previously authored route " + std::to_string(c.id.value),
                                                           std::move(why), c.evidence};
        }
}
std::vector<SctScriptId> SctRouteAuthoring::closure(const SctAuthoringProject &p, std::span<const SctScriptId> seeds) {
    std::set<SctScriptId> ids(seeds.begin(), seeds.end());
    bool changed;
    do {
        changed = false;
        for (const auto &c : p.connections)
            if (c.route && (ids.contains(c.route->sourceScript) || ids.contains(c.route->destinationScript))) {
                changed |= ids.insert(c.route->sourceScript).second;
                changed |= ids.insert(c.route->destinationScript).second;
            }
    } while (changed);
    return {ids.begin(), ids.end()};
}
Result<SctAuthoringState> SctRouteAuthoring::draft(const SctAuthoringState &state, const SctRouteRequest &request,
                                                   SctTransitionKind kind, std::string evidence,
                                                   std::string explanation) {
    auto *source = state.project.find(request.source);
    auto *destination = state.project.find(request.response);
    auto *entry = state.project.find(request.arrival);
    auto src = portScript(state.project, request.source), dst = portScript(state.project, request.response);
    if (!source || !destination || !source->action || !src || !dst || !entry || entry->script != *dst || *src == *dst ||
        evidence.empty() || explanation.empty())
        return Result<SctAuthoringState>::failure(
            error("Draft route needs explicit script-owned endpoints, evidence, and an explanation."));
    auto next = state;
    if (request.replace && !next.project.find(*request.replace))
        return Result<SctAuthoringState>::failure(error("Draft reconnection is missing."));
    auto id =
        request.replace ? Result<SctConnectionId>::success(*request.replace) : next.project.allocate<SctConnectionId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    SctExecutableRoute route;
    route.kind = kind;
    route.sourceScript = *src;
    route.destinationScript = *dst;
    route.sourceAction = *source->action;
    route.arrival = request.arrival;
    route.arrivalCase = request.arrivalCase;
    route.sourceStem = assetName(state.project, *src, true);
    route.targetFilename = assetName(state.project, *dst, false);
    route.unresolved =
        SctUnresolvedBinding{std::move(evidence),
                             std::move(explanation),
                             {{SctEvidenceKind::UserAssertion,
                               "Explicit unresolved route intent; original execution instructions retained."}}};
    SctAuthoringConnection c{id.value(), request.source, request.response, route.unresolved->evidence,
                             std::move(route)};
    if (request.replace)
        *std::ranges::find(next.project.connections, *request.replace, &SctAuthoringConnection::id) = std::move(c);
    else
        next.project.connections.push_back(std::move(c));
    auto valid = SctAuthoringSession::validate(next);
    if (!valid)
        return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<SctAuthoringState> SctRouteAuthoring::remove(const SctAuthoringState &state, SctConnectionId id) {
    auto next = state;
    if (!std::erase_if(next.project.connections, [&](const auto &c) { return c.id == id; }))
        return Result<SctAuthoringState>::failure(error("Route is missing."));
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<SctAuthoringState> SctRouteAuthoring::connect(const SctAuthoringState &state, const SctRouteRequest &request) {
    auto fail = [](std::string s) { return Result<SctAuthoringState>::failure(error(std::move(s))); };
    auto *source = state.project.find(request.source);
    auto *response = state.project.find(request.response);
    auto *entry = state.project.find(request.arrival);
    if (!source || !response || !source->action || !response->action || !entry || !entry->enabled || !entry->binding ||
        entry->binding->kind != SctEntrypointKind::Arrival || !entry->binding->predicate ||
        !entry->binding->responseSite)
        return fail("Choose a source action port, enabled arrival condition/case and module response port.");
    const auto src = portScript(state.project, request.source), dst = portScript(state.project, request.response);
    if (!src || !dst || *src == *dst || entry->script != *dst || !std::holds_alternative<SctModuleId>(response->owner))
        return fail("Route needs different source and destination scripts and a module response.");
    if (request.insertLoadAfterSource) {
        auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, *src);
        if (!working)
            return Result<SctAuthoringState>::failure(working.diagnostics());
        auto d = std::make_shared<sct::SctDocument>(*working.value().document);
        auto *anchor = state.project.find(*source->action);
        if (!anchor)
            return fail("Insertion anchor is missing.");
        auto instruction = d->allocateInstructionId();
        auto text = d->allocateSupplementaryTextId();
        SctSemanticOperationBatch batch{
            {SctInsertSupplementaryTextAfterOperation{
                 {}, {text, sct::SctTextKind::PlainString, sct::SctPlainText{assetName(state.project, *dst, false)}}},
             SctInsertInstructionAfterOperation{
                 anchor->instruction, {instruction, 43, false, {}, {{0, sct::SctSupplementaryTextReference{text}}}}}}};
        auto applied = SctSemanticOperationService::applyInPlace(*d, batch);
        if (!applied.succeeded())
            return fail(applied.issues.front().message);
        working.value().document = d;
        auto changed =
            SctAuthoringMaterializer::replaceWorkingState(state.project, state.programs, *src, working.value());
        if (!changed)
            return Result<SctAuthoringState>::failure(changed.diagnostics());
        auto next = state;
        next.project = std::move(changed).takeValue();
        SctActionId action;
        SctSequenceId seq;
        for (const auto &sequence : next.project.sequences)
            if (sequence.script == *src)
                for (const auto &a : sequence.actions)
                    if (a.instruction == instruction) {
                        action = a.id;
                        seq = sequence.id;
                    }
        if (!action.valid())
            return fail("Insertion needs an authored sequence.");
        auto port = next.project.allocate<SctPortId>();
        if (!port)
            return Result<SctAuthoringState>::failure(port.diagnostics());
        next.project.ports.push_back(
            {port.value(), source->owner, "Load " + assetName(state.project, *dst, false), action, seq});
        auto request2 = request;
        request2.source = port.value();
        request2.insertLoadAfterSource = false;
        return connect(next, request2);
    }
    auto location = SctSequenceAuthoring::arrivalLocation(assetName(state.project, *src, true));
    if (!location)
        return fail(location.diagnostics().front().message);
    if (!support().front().authorable)
        return fail("SPICE load metadata is unavailable.");
    auto sw = SctAuthoringMaterializer::workingState(state.project, state.programs, *src),
         dw = SctAuthoringMaterializer::workingState(state.project, state.programs, *dst);
    if (!sw || !dw)
        return fail("Endpoint authored program is unavailable.");
    auto sd = std::make_shared<sct::SctDocument>(*sw.value().document),
         dd = std::make_shared<sct::SctDocument>(*dw.value().document);
    auto sourceAction = state.project.find(*source->action);
    auto responseAction = state.project.find(*response->action);
    if (!sourceAction || !responseAction)
        return fail("Endpoint action is missing.");
    auto *load = sct::SctDocumentIndex::build(*sd).find(*sd, sourceAction->instruction);
    auto *loadParameter = parameter(*sd, {sourceAction->instruction, {0, {}}});
    auto *oldText = loadParameter ? std::get_if<sct::SctSupplementaryTextReference>(&loadParameter->value) : nullptr;
    if (!load || load->opcode != 43 || !oldText)
        return fail("Select an existing ordinary load (43) with a literal footer filename. Insert it through sequence "
                    "authoring first if necessary.");
    auto old = std::ranges::find(sd->supplementaryText, oldText->target, &sct::SctDocumentSupplementaryText::id);
    if (old == sd->supplementaryText.end())
        return fail("Load filename reference is missing.");
    const auto filename = assetName(state.project, *dst, false);
    const auto *oldPlain = std::get_if<sct::SctPlainText>(&old->value);
    if (!oldPlain || oldPlain->utf8 != filename) {
        size_t uses = 0;
        for (const auto &section : sd->sections)
            if (const auto *body = std::get_if<sct::SctScriptSectionContent>(&section.content))
                for (const auto &instruction : body->instructions) {
                    auto count = [&](const auto &ps) {
                        for (const auto &p : ps)
                            if (const auto *r = std::get_if<sct::SctSupplementaryTextReference>(&p.value);
                                r && r->target == oldText->target)
                                ++uses;
                    };
                    count(instruction.fixedParameters);
                    for (const auto &group : instruction.repeatedParameterGroups)
                        count(group.parameters);
                }
        if (uses == 1)
            old->value = sct::SctPlainText{filename};
        else {
            const auto newId = sd->allocateSupplementaryTextId();
            sd->supplementaryText.push_back({newId, sct::SctTextKind::PlainString, sct::SctPlainText{filename}});
            const_cast<sct::SctDocumentParameter *>(loadParameter)->value = sct::SctSupplementaryTextReference{newId};
        }
    }
    auto *guard = sct::SctDocumentIndex::build(*dd).find(*dd, entry->binding->predicate->instruction);
    auto *cp = parameter(*dd, *entry->binding->predicate);
    auto *expression = cp ? std::get_if<sct::SctCanonicalExpression>(&cp->value) : nullptr;
    if (!guard || !expression)
        return fail("Arrival condition is missing.");
    if (request.arrivalCase) {
        if (guard->opcode != 3)
            return fail("A case ordinal requires an existing switch.");
        auto nextCase = SctSequenceAuthoring::selectArrivalCase(state, *dst, guard->id, *request.arrivalCase,
                                                                location.value().stem);
        if (!nextCase)
            return Result<SctAuthoringState>::failure(nextCase.diagnostics());
        auto *word = parameter(*dd, {guard->id, {2, *request.arrivalCase}});
        if (!word)
            return fail("Arrival case is missing.");
        const_cast<sct::SctDocumentParameter *>(word)->value = sct::SctEncodedWordValue{location.value().encoded};
    } else {
        if (guard->opcode != 0)
            return fail("Select an arrival comparison or an explicit switch case.");
        const auto *program = std::get_if<sct::SctTypedScptProgram>(&expression->body);
        if (!program || program->operations.size() != 3)
            return fail("Use an existing simple previous-location equality; keep other requirements in explicitly "
                        "selected surrounding blocks.");
        const auto *read = std::get_if<sct::SctScptValueOperation>(&program->operations[0]);
        const auto *literal = std::get_if<sct::SctScptValueOperation>(&program->operations[1]);
        const auto *op = std::get_if<sct::SctScptBinaryOperation>(&program->operations[2]);
        if (!read || read->kind != sct::SctScptValueKind::IntegerVariableLow16Comparison ||
            (read->encodingWord & 0xffffff) != 15 || !literal || !op ||
            sct::sctScptOperatorSymbol(op->encodingWord) != "==" ||
            (literal->kind != sct::SctScptValueKind::DecimalLiteral &&
             literal->kind != sct::SctScptValueKind::FloatLiteral))
            return fail("Arrival discriminator is not a supported previous-location equality.");
        auto replacement = SctSequenceAuthoring::arrivalCondition(location.value().stem).value();
        auto changed = *expression;
        std::get<sct::SctTypedScptProgram>(changed.body).operations[1] =
            std::get<sct::SctTypedScptProgram>(replacement.body).operations[1];
        const_cast<sct::SctDocumentParameter *>(cp)->value = changed;
    }
    auto *rp = parameter(*dd, *entry->binding->responseSite);
    if (!rp || !std::holds_alternative<sct::SctInstructionReference>(rp->value))
        return fail("Arrival response is not an instruction target operand.");
    const_cast<sct::SctDocumentParameter *>(rp)->value = sct::SctInstructionReference{responseAction->instruction};
    auto next = state;
    for (auto &c : next.project.contents)
        if (c.authoredState) {
            if (next.project.effectiveScript(c.owner) == src)
                c.authoredState->document = sd;
            if (next.project.effectiveScript(c.owner) == dst)
                c.authoredState->document = dd;
        }
    std::ranges::find(next.project.entrypoints, request.arrival, &SctAuthoringEntrypoint::id)->binding->response =
        request.response;
    auto id =
        request.replace ? Result<SctConnectionId>::success(*request.replace) : next.project.allocate<SctConnectionId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    if (request.replace && !next.project.find(*request.replace))
        return fail("Reconnection target is missing.");
    SctExecutableRoute route;
    route.sourceScript = *src;
    route.destinationScript = *dst;
    route.sourceAction = *source->action;
    route.arrival = request.arrival;
    route.arrivalCase = request.arrivalCase;
    route.sourceStem = location.value().stem;
    route.targetFilename = filename;
    route.discriminatorBit = request.discriminatorBit;
    route.discriminatorValue = request.discriminatorValue;
    auto observe = [&](SctActionId id, SctScriptId script, SctRouteObservationRole role) -> bool {
        auto *a = next.project.find(id);
        auto *seq = sequence(next.project, id);
        auto *d = document(next.project, script);
        if (!a || !seq || seq->script != script || !d)
            return false;
        auto fp = SctProgramStateCodec::instructionFingerprint(*d, a->instruction);
        if (!fp)
            return false;
        route.observations.push_back({script, id, role, fp.value()});
        return true;
    };
    if (!observe(*source->action, *src, SctRouteObservationRole::Source))
        return fail("Source observation failed.");
    for (const auto &seq : next.project.sequences)
        if (seq.script == *dst)
            for (const auto &a : seq.actions) {
                if (a.instruction == entry->binding->predicate->instruction)
                    observe(a.id, *dst, SctRouteObservationRole::Arrival);
                if (a.instruction == entry->binding->responseSite->instruction)
                    observe(a.id, *dst, SctRouteObservationRole::Response);
            }
    for (auto a : request.prerequisites)
        if (!observe(a, *src, SctRouteObservationRole::Prerequisite))
            return fail("Invalid source prerequisite.");
    for (auto a : request.requirements)
        if (!observe(a, *dst, SctRouteObservationRole::Requirement))
            return fail("Invalid destination requirement.");
    for (auto a : request.effects)
        if (!observe(a, *src, SctRouteObservationRole::Effect))
            return fail("Invalid explicit source effect.");
    if (request.continuation && !observe(*request.continuation, *src, SctRouteObservationRole::Continuation))
        return fail("Invalid continuation.");
    SctAuthoringConnection connection{
        id.value(),
        request.source,
        request.response,
        {{SctEvidenceKind::UserAssertion,
          "Explicit load and arrival response; other conditions and effects remain unchanged."}},
        std::move(route)};
    if (request.replace)
        *std::ranges::find(next.project.connections, *request.replace, &SctAuthoringConnection::id) =
            std::move(connection);
    else
        next.project.connections.push_back(std::move(connection));
    for (auto &other : next.project.connections)
        if (other.id != id.value() && other.route && !other.route->unresolved) {
            auto failure = currentFailure(next.project, other);
            if (!failure.empty())
                other.route->unresolved =
                    SctUnresolvedBinding{"Route before explicit endpoint reconnection", failure, other.evidence};
        }
    auto valid = SctAuthoringSession::validate(next);
    if (!valid)
        return Result<SctAuthoringState>::failure(valid.diagnostics());
    return Result<SctAuthoringState>::success(std::move(next));
}
} // namespace salsa::core
