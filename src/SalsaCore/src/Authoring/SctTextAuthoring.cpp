#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <map>
#include <set>

namespace salsa::core {
namespace {
namespace sct = spice::sct;
Diagnostic error(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)};
}
struct TextRef {
    sct::SctTextValue *value;
    sct::SctTextKind kind;
    sct::SctTextStorage storage;
};
std::optional<TextRef> textAt(sct::SctDocument &d, const sct::SctTextEntityId &target) {
    if (const auto *id = std::get_if<sct::SctStringId>(&target)) {
        for (auto &section : d.sections)
            if (auto *s = std::get_if<sct::SctStringSectionContent>(&section.content); s && s->string.id == *id)
                return TextRef{&s->string.value, s->string.kind, sct::SctTextStorage::IndexedSection};
    } else
        for (auto &t : d.supplementaryText)
            if (t.id == std::get<sct::SctSupplementaryTextId>(target))
                return TextRef{&t.value, t.kind, sct::SctTextStorage::Footer};
    return {};
}
bool refers(const sct::SctDocumentParameter &p, const sct::SctTextEntityId &id) {
    if (const auto *ref = std::get_if<sct::SctStringReference>(&p.value))
        return id == sct::SctTextEntityId{ref->target};
    if (const auto *ref = std::get_if<sct::SctSupplementaryTextReference>(&p.value))
        return id == sct::SctTextEntityId{ref->target};
    return false;
}
void retarget(sct::SctDocumentParameter &p, const sct::SctTextEntityId &target) {
    std::visit(
        [&](auto id) {
            if constexpr (std::is_same_v<decltype(id), sct::SctStringId>)
                p.value = sct::SctStringReference{id};
            else
                p.value = sct::SctSupplementaryTextReference{id};
        },
        target);
}
sct::SctDocumentParameter *parameter(sct::SctDocument &doc, const sct::SctParameterSite &site) {
    for (auto &s : doc.sections)
        if (auto *b = std::get_if<sct::SctScriptSectionContent>(&s.content))
            for (auto &i : b->instructions)
                if (i.id == site.instruction) {
                    if (site.parameter.repeatedGroupOrdinal &&
                        *site.parameter.repeatedGroupOrdinal >= i.repeatedParameterGroups.size())
                        return nullptr;
                    auto &ps = site.parameter.repeatedGroupOrdinal
                                   ? i.repeatedParameterGroups[*site.parameter.repeatedGroupOrdinal].parameters
                                   : i.fixedParameters;
                    const auto p =
                        std::ranges::find(ps, site.parameter.schemaIndex, &sct::SctDocumentParameter::schemaIndex);
                    return p == ps.end() ? nullptr : &*p;
                }
    return nullptr;
}
Result<SctAuthoringState> checked(SctAuthoringState state) {
    auto valid = SctAuthoringSession::validate(state);
    return valid ? Result<SctAuthoringState>::success(std::move(state), valid.diagnostics())
                 : Result<SctAuthoringState>::failure(valid.diagnostics());
}
Result<SctAuthoringState> replace(SctAuthoringState next, SctScriptId script, SctSemanticState working) {
    auto changed = SctAuthoringMaterializer::replaceWorkingState(next.project, next.programs, script, working);
    if (!changed)
        return Result<SctAuthoringState>::failure(changed.diagnostics());
    next.project = std::move(changed).takeValue();
    return checked(std::move(next));
}
std::string groupLabel(const std::string &key) {
    if (key == "F")
        return "French";
    if (key == "S")
        return "Spanish";
    if (key == "G")
        return "German";
    return "Unmarked";
}
} // namespace

std::vector<SctTextObservation> SctTextAuthoring::observe(const sct::SctDocument &doc) {
    std::vector<SctTextObservation> observations;
    std::string group;
    for (const auto &section : doc.sections) {
        if (std::holds_alternative<sct::SctStringGroupMarkerSectionContent>(section.content))
            group = section.nameBytes;
        else if (const auto *text = std::get_if<sct::SctStringSectionContent>(&section.content))
            observations.push_back(
                {text->string.id, section.nameBytes, group, {}, {}, "No corroborated language-name pattern."});
        else
            group.clear();
    }
    std::vector<std::set<std::pair<std::string, std::string>>> proposals(observations.size());
    for (std::size_t i = 0; i < observations.size(); ++i) {
        auto &o = observations[i];
        const auto &name = o.name;
        std::string normalized, baseGroup = o.group, key;
        if (o.group == "palyoko" && name.size() == 9 &&
            std::string_view("sfg").find(name[7]) != std::string_view::npos) {
            normalized = name.substr(0, 7) + name.substr(8);
            key = std::string(1, static_cast<char>(name[7] - 'a' + 'A'));
        } else if (name.size() >= 3 && (name[2] < '0' || name[2] > '9' || name.size() > 9) &&
                   std::string_view("FSG").find(name[1]) != std::string_view::npos) {
            key = name.substr(1, 1);
            normalized = name.substr(0, 1) + name.substr(2);
            if (!baseGroup.empty() && std::string_view("FSGfsg").find(baseGroup.back()) != std::string_view::npos) {
                const auto marker =
                    baseGroup.back() >= 'a' ? static_cast<char>(baseGroup.back() - 'a' + 'A') : baseGroup.back();
                if (marker != name[1]) {
                    o.explanation = "String and group language markers conflict.";
                    continue;
                }
                baseGroup.pop_back();
            } else if (!std::ranges::any_of(observations, [&](const auto &other) {
                           return other.name == normalized && other.group == baseGroup;
                       }))
                continue;
        } else
            continue;
        const auto correspondence = baseGroup + "/" + normalized;
        proposals[i].emplace(key, correspondence);
        for (std::size_t b = 0; b < observations.size(); ++b) {
            const auto& base = observations[b];
            const bool palyokoBase = o.group == "palyoko" && base.name.size() == 9 && name.size() == 9 &&
                std::string_view("sfg").find(base.name[7]) == std::string_view::npos &&
                base.name.substr(0, 7) + base.name.substr(8) == normalized;
            if (base.group == baseGroup && (base.name == normalized || palyokoBase))
                proposals[b].emplace("base", correspondence);
        }
    }
    for (std::size_t i = 0; i < observations.size(); ++i) {
        auto &o = observations[i];
        if (proposals[i].size() > 1)
            o.explanation = "Conflicting language candidates.";
        else if (proposals[i].size() == 1) {
            o.languageKey = proposals[i].begin()->first;
            o.correspondenceKey = proposals[i].begin()->second;
            o.explanation =
                *o.languageKey == "base"
                    ? "Unmarked counterpart of a legacy language-name family; no English language assumption."
                    : "Legacy indexed-name pattern proposes " + groupLabel(*o.languageKey) +
                          "; assignment remains editable.";
        }
    }
    // Several resources with the same inferred language/key are ambiguous.
    std::map<std::pair<std::string, std::string>, std::size_t> counts;
    for (const auto &o : observations)
        if (o.languageKey)
            ++counts[{o.correspondenceKey, *o.languageKey}];
    for (auto &o : observations)
        if (o.languageKey && counts[{o.correspondenceKey, *o.languageKey}] > 1) {
            o.languageKey.reset();
            o.explanation = "Multiple strings match the same proposed language and correspondence key.";
        }
    for (const auto &text : doc.supplementaryText)
        observations.push_back({text.id,
                                "Footer " + std::to_string(text.id.value()),
                                {},
                                {},
                                {},
                                "Footer text has no indexed-name language evidence."});
    return observations;
}
Result<SctAuthoringState> SctTextAuthoring::discover(const SctAuthoringState &state, SctScriptId script) {
    auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!working)
        return Result<SctAuthoringState>::failure(working.diagnostics());
    auto next = state;
    for (const auto &o : observe(*working.value().document)) {
        auto found = std::ranges::find_if(next.project.texts,
                                          [&](const auto &t) { return t.script == script && t.target == o.target; });
        if (found == next.project.texts.end()) {
            auto id = next.project.allocate<SctTextResourceId>();
            if (!id)
                return Result<SctAuthoringState>::failure(id.diagnostics());
            next.project.texts.push_back({id.value(), script, o.target, o.name, o.group});
            found = next.project.texts.end() - 1;
        }
        if (found->explicitAssignment)
            continue;
        found->correspondenceKey = o.correspondenceKey;
        found->languages.clear();
        found->evidence = {{SctEvidenceKind::SourceObservation,
                            "Original indexed name: " + found->sourceName + "; group: " + found->sourceGroup},
                           {SctEvidenceKind::Inference, o.explanation}};
        if (o.languageKey) {
            const auto recognition = "legacy:" + *o.languageKey;
            auto g = std::ranges::find(next.project.languageGroups, recognition, &SctLanguageGroup::recognitionKey);
            if (g == next.project.languageGroups.end()) {
                auto id = next.project.allocate<SctLanguageGroupId>();
                if (!id)
                    return Result<SctAuthoringState>::failure(id.diagnostics());
                next.project.languageGroups.push_back({id.value(), groupLabel(*o.languageKey), recognition});
                g = next.project.languageGroups.end() - 1;
            }
            found->languages.push_back(g->id);
        }
    }
    return checked(std::move(next));
}
Result<SctAuthoringState> SctTextAuthoring::createLanguage(const SctAuthoringState &state, std::string name) {
    auto next = state;
    auto id = next.project.allocate<SctLanguageGroupId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    next.project.languageGroups.push_back({id.value(), std::move(name), {}});
    return checked(std::move(next));
}
Result<SctAuthoringState> SctTextAuthoring::renameLanguage(const SctAuthoringState &state, SctLanguageGroupId id,
                                                           std::string name) {
    auto next = state;
    auto found = std::ranges::find(next.project.languageGroups, id, &SctLanguageGroup::id);
    if (found == next.project.languageGroups.end())
        return Result<SctAuthoringState>::failure(error("Language group is missing."));
    found->name = std::move(name);
    return checked(std::move(next));
}
Result<SctAuthoringState> SctTextAuthoring::assign(const SctAuthoringState &state,
                                                   const std::vector<SctTextResourceId> &ids,
                                                   std::vector<SctLanguageGroupId> groups) {
    auto next = state;
    std::set<SctTextResourceId> seen;
    for (auto id : ids) {
        auto text = std::ranges::find(next.project.texts, id, &SctAuthoredText::id);
        if (text == next.project.texts.end() || !seen.insert(id).second)
            return Result<SctAuthoringState>::failure(error("Missing or duplicate selected string."));
        text->languages = groups;
        text->explicitAssignment = true;
        text->evidence.push_back(
            {SctEvidenceKind::UserAssertion, "Explicit language-group assignment; execution references unchanged."});
    }
    return checked(std::move(next));
}
Result<SctAuthoringState> SctTextAuthoring::link(const SctAuthoringState &state, SctScriptId script, std::string name,
                                                 std::vector<SctTextResourceId> variants,
                                                 std::optional<SctModuleId> module) {
    auto next = state;
    auto id = next.project.allocate<SctLogicalTextId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    next.project.logicalTexts.push_back({id.value(), script, std::move(name), std::move(variants), module});
    return checked(std::move(next));
}
Result<SctTextResourceView> SctTextAuthoring::inspect(const SctAuthoringState &state, SctTextResourceId id) {
    const auto *t = state.project.find(id);
    if (!t)
        return Result<SctTextResourceView>::failure(error("String is missing."));
    auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, t->script);
    if (!w)
        return Result<SctTextResourceView>::failure(w.diagnostics());
    auto doc = *w.value().document;
    auto text = textAt(doc, t->target);
    if (!text)
        return Result<SctTextResourceView>::failure(error("String target is missing."));
    SctTextResourceView view{*text->value, text->kind, text->storage, {}};
    for (const auto &section : doc.sections)
        if (const auto *body = std::get_if<sct::SctScriptSectionContent>(&section.content))
            for (const auto &i : body->instructions) {
                const auto add = [&](const auto &ps, std::optional<std::uint32_t> group) {
                    for (const auto &p : ps)
                        if (refers(p, t->target))
                            view.uses.push_back({i.id, {p.schemaIndex, group}});
                };
                add(i.fixedParameters, {});
                for (std::uint32_t g = 0; g < i.repeatedParameterGroups.size(); ++g)
                    add(i.repeatedParameterGroups[g].parameters, g);
            }
    return Result<SctTextResourceView>::success(std::move(view));
}
Result<SctAuthoringState> SctTextAuthoring::edit(const SctAuthoringState &state, const std::vector<SctTextEdit> &edits,
                                                 sct::SctTextEncoding encoding) {
    auto next = state;
    std::map<SctScriptId, SctSemanticState> work;
    std::map<SctScriptId, std::shared_ptr<sct::SctDocument>> docs;
    std::set<SctTextResourceId> ids;
    for (const auto &edit : edits) {
        const auto *t = state.project.find(edit.text);
        if (!t || !ids.insert(edit.text).second)
            return Result<SctAuthoringState>::failure(error("Select each shared text resource only once."));
        if (!work.contains(t->script)) {
            auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, t->script);
            if (!w)
                return Result<SctAuthoringState>::failure(w.diagnostics());
            work[t->script] = w.value();
            docs[t->script] = std::make_shared<sct::SctDocument>(*w.value().document);
        }
        auto value = textAt(*docs[t->script], t->target);
        if (!value)
            return Result<SctAuthoringState>::failure(error("Text target is missing."));
        if (std::holds_alternative<sct::SctOpaqueText>(edit.value))
            return Result<SctAuthoringState>::failure(
                error("Author a semantic text value; opaque bytes are not language content."));
        const auto encoded = sct::encodeSctTextRecord(edit.value, value->kind, value->storage, encoding);
        if (!encoded.bytes)
            return Result<SctAuthoringState>::failure(error("Text encoding failed: " + encoded.error));
        *value->value = edit.value;
    }
    for (auto &[script, w] : work) {
        w.document = docs[script];
        auto changed = replace(std::move(next), script, w);
        if (!changed)
            return changed;
        next = std::move(changed).takeValue();
    }
    return checked(std::move(next));
}
Result<SctAuthoringState> SctTextAuthoring::createText(const SctAuthoringState &state, SctScriptId script,
                                                       std::string name, sct::SctTextKind kind,
                                                       sct::SctTextStorage storage, sct::SctTextValue value,
                                                       std::vector<SctLanguageGroupId> languages,
                                                       sct::SctTextEncoding encoding) {
    if (std::holds_alternative<sct::SctOpaqueText>(value) ||
        !sct::encodeSctTextRecord(value, kind, storage, encoding).bytes)
        return Result<SctAuthoringState>::failure(
            error("New text must be a semantic value encodable for the selected output convention."));
    auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!w)
        return Result<SctAuthoringState>::failure(w.diagnostics());
    auto doc = std::make_shared<sct::SctDocument>(*w.value().document);
    sct::SctTextEntityId target;
    if (storage == sct::SctTextStorage::IndexedSection) {
        if (name.empty() || name.size() > 16 ||
            std::ranges::any_of(name, [](unsigned char c) { return c < 33 || c > 126; }) ||
            std::ranges::any_of(doc->sections, [&](const auto &s) { return s.nameBytes == name; }))
            return Result<SctAuthoringState>::failure(
                error("Choose a unique printable indexed section name of 1–16 bytes."));
        if (!doc->sections.empty())
            if (const auto *code = std::get_if<sct::SctScriptSectionContent>(&doc->sections.back().content);
                code && (code->instructions.empty() ||
                         (code->instructions.back().opcode != 10 && code->instructions.back().opcode != 12 &&
                          code->instructions.back().opcode != 3)))
                return Result<SctAuthoringState>::failure(
                    error("Define the trailing script continuation before appending text sections."));
        const auto id = doc->allocateStringId();
        target = id;
        doc->sections.push_back(
            {doc->allocateSectionId(), name, sct::SctStringSectionContent{{id, std::move(value), kind}}});
    } else {
        const auto id = doc->allocateSupplementaryTextId();
        target = id;
        doc->supplementaryText.push_back({id, kind, std::move(value)});
    }
    auto next = state;
    auto id = next.project.allocate<SctTextResourceId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    next.project.texts.push_back({id.value(),
                                  script,
                                  target,
                                  std::move(name),
                                  {},
                                  {},
                                  std::move(languages),
                                  true,
                                  {{SctEvidenceKind::UserAssertion, "New authored text resource."}}});
    for (auto &content : next.project.contents)
        if (next.project.effectiveScript(content.owner) == script && content.authoredState)
            content.authoredState->document = doc;
    w.value().document = doc;
    return replace(std::move(next), script, w.value());
}
Result<SctAuthoringState> SctTextAuthoring::insert(const SctAuthoringState &state,
                                                   const std::vector<SctTextInsertion> &insertions,
                                                   sct::SctTextEncoding encoding) {
    if (insertions.empty())
        return checked(state);
    const auto *first = state.project.find(insertions.front().sequence);
    if (!first)
        return Result<SctAuthoringState>::failure(error("Select a sequence."));
    const auto script = first->script;
    auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, script);
    if (!w)
        return Result<SctAuthoringState>::failure(w.diagnostics());
    auto doc = std::make_shared<sct::SctDocument>(*w.value().document);
    const auto original = sct::SctDocumentIndex::build(*w.value().document);
    for (const auto &insertion : insertions)
        if (insertion.incoming && std::ranges::count(insertions, insertion.before, &SctTextInsertion::before) > 1)
            return Result<SctAuthoringState>::failure(
                error("Insert one incoming-route operation per anchor in a batch; separate shared routes explicitly."));
    for (const auto &insertion : insertions) {
        const auto *seq = state.project.find(insertion.sequence);
        const auto *t = state.project.find(insertion.text);
        const auto *anchor = state.project.find(insertion.before);
        const auto *source = state.project.find(insertion.prototype);
        if (!seq || seq->script != script || !t || t->script != script || !anchor || !source ||
            std::ranges::find(seq->actions, insertion.before, &SctSequenceAction::id) == seq->actions.end())
            return Result<SctAuthoringState>::failure(
                error("Insertion resources and anchor must belong to the selected script/sequence."));
        const bool sourceOwned = std::ranges::any_of(state.project.sequences, [&](const auto &s) {
            return s.script == script &&
                   std::ranges::find(s.actions, insertion.prototype, &SctSequenceAction::id) != s.actions.end();
        });
        const auto *instruction = sourceOwned ? original.find(*w.value().document, source->instruction) : nullptr;
        if (!instruction)
            return Result<SctAuthoringState>::failure(
                error("Template action is missing or belongs to another script."));
        auto prototype = *instruction;
        auto &ps = insertion.operand.repeatedGroupOrdinal &&
                           *insertion.operand.repeatedGroupOrdinal < prototype.repeatedParameterGroups.size()
                       ? prototype.repeatedParameterGroups[*insertion.operand.repeatedGroupOrdinal].parameters
                       : prototype.fixedParameters;
        if (insertion.operand.repeatedGroupOrdinal &&
            *insertion.operand.repeatedGroupOrdinal >= prototype.repeatedParameterGroups.size())
            return Result<SctAuthoringState>::failure(error("Template text operand is missing."));
        const auto p = std::ranges::find(ps, insertion.operand.schemaIndex, &sct::SctDocumentParameter::schemaIndex);
        if (p == ps.end() || (std::holds_alternative<sct::SctStringId>(t->target)
                                  ? !std::holds_alternative<sct::SctStringReference>(p->value)
                                  : !std::holds_alternative<sct::SctSupplementaryTextReference>(p->value)))
            return Result<SctAuthoringState>::failure(
                error("Select a matching indexed/footer text operand; operation kinds are not interchangeable."));
        auto text = textAt(*doc, t->target);
        if (!text || !sct::encodeSctTextRecord(*text->value, text->kind, text->storage, encoding).bytes)
            return Result<SctAuthoringState>::failure(error("Insertion text cannot be encoded."));
        retarget(*p, t->target);
        prototype.id = doc->allocateInstructionId();
        if (insertion.incoming) {
            const auto &incoming = *insertion.incoming;
            const auto *from = state.project.find(incoming.source);
            const bool owned = std::ranges::any_of(state.project.sequences, [&](const auto &s) {
                return s.script == script &&
                       std::ranges::find(s.actions, incoming.source, &SctSequenceAction::id) != s.actions.end();
            });
            auto *edge = from && owned ? parameter(*doc, {from->instruction, incoming.operand}) : nullptr;
            const auto *reference = edge ? std::get_if<sct::SctInstructionReference>(&edge->value) : nullptr;
            if (!reference || reference->target != anchor->instruction)
                return Result<SctAuthoringState>::failure(
                    error("Selected incoming operand does not target the insertion anchor."));
            edge->value = sct::SctInstructionReference{prototype.id};
        }
        for (auto &section : doc->sections)
            if (section.id == seq->section) {
                auto &body = std::get<sct::SctScriptSectionContent>(section.content).instructions;
                const auto at = std::ranges::find(body, anchor->instruction, &sct::SctDocumentInstruction::id);
                if (at == body.end())
                    return Result<SctAuthoringState>::failure(error("Insertion anchor is missing."));
                body.insert(at, std::move(prototype));
                break;
            }
    }
    w.value().document = doc;
    return replace(state, script, w.value());
}
Result<SctAuthoringState> SctTextAuthoring::split(const SctAuthoringState &state, SctTextResourceId id,
                                                  SctLanguageGroupId language, std::string name,
                                                  const std::vector<sct::SctParameterSite> &uses) {
    const auto *original = state.project.find(id);
    if (!original || original->languages.size() < 2 ||
        std::ranges::find(original->languages, language) == original->languages.end())
        return Result<SctAuthoringState>::failure(error("Choose one language of a shared text resource."));
    auto w = SctAuthoringMaterializer::workingState(state.project, state.programs, original->script);
    if (!w)
        return Result<SctAuthoringState>::failure(w.diagnostics());
    auto doc = std::make_shared<sct::SctDocument>(*w.value().document);
    auto target = original->target;
    if (const auto *source = std::get_if<sct::SctStringId>(&target)) {
        if (name.empty() || name.size() > 16 ||
            std::ranges::any_of(name, [](unsigned char c) { return c < 33 || c > 126; }) ||
            std::ranges::any_of(doc->sections, [&](const auto &s) { return s.nameBytes == name; }))
            return Result<SctAuthoringState>::failure(
                error("Choose a unique printable indexed section name of 1–16 bytes."));
        auto found = std::ranges::find_if(doc->sections, [&](const auto &s) {
            const auto *v = std::get_if<sct::SctStringSectionContent>(&s.content);
            return v && v->string.id == *source;
        });
        if (found == doc->sections.end())
            return Result<SctAuthoringState>::failure(error("Shared text is missing."));
        auto clone = *found;
        clone.id = doc->allocateSectionId();
        clone.nameBytes = name;
        auto &s = std::get<sct::SctStringSectionContent>(clone.content);
        s.string.id = doc->allocateStringId();
        target = s.string.id;
        doc->sections.insert(found + 1, std::move(clone));
    } else {
        const auto found = std::ranges::find(doc->supplementaryText, std::get<sct::SctSupplementaryTextId>(target),
                                             &sct::SctDocumentSupplementaryText::id);
        if (found == doc->supplementaryText.end())
            return Result<SctAuthoringState>::failure(error("Shared footer text is missing."));
        auto clone = *found;
        clone.id = doc->allocateSupplementaryTextId();
        target = clone.id;
        doc->supplementaryText.push_back(std::move(clone));
    }
    std::set<sct::SctParameterSite> unique;
    for (const auto &site : uses) {
        auto *p = parameter(*doc, site);
        if (!p || !refers(*p, original->target) || !unique.insert(site).second)
            return Result<SctAuthoringState>::failure(
                error("Split may retarget only explicitly selected uses of the shared resource."));
        retarget(*p, target);
    }
    auto next = state;
    auto text = *original;
    auto newId = next.project.allocate<SctTextResourceId>();
    if (!newId)
        return Result<SctAuthoringState>::failure(newId.diagnostics());
    text.id = newId.value();
    text.target = target;
    text.languages = {language};
    text.explicitAssignment = true;
    text.evidence.push_back({SctEvidenceKind::UserAssertion,
                             "Split shared text for one language; only selected execution uses retargeted."});
    auto &prior = *std::ranges::find(next.project.texts, id, &SctAuthoredText::id);
    std::erase(prior.languages, language);
    prior.explicitAssignment = true;
    next.project.texts.push_back(std::move(text));
    for (auto &logical : next.project.logicalTexts)
        if (std::ranges::find(logical.variants, id) != logical.variants.end())
            logical.variants.push_back(newId.value());
    // Install both the document and its new metadata before structural validation.
    for (auto &content : next.project.contents)
        if (next.project.effectiveScript(content.owner) == original->script && content.authoredState)
            content.authoredState->document = doc;
    w.value().document = doc;
    return replace(std::move(next), original->script, w.value());
}
Result<void> SctTextAuthoring::validateProgram(const SctAuthoringProject &p, SctScriptId script,
                                               const sct::SctDocument &d) {
    std::set<sct::SctTextEntityId> available;
    for (const auto &section : d.sections)
        if (const auto *s = std::get_if<sct::SctStringSectionContent>(&section.content))
            available.insert(s->string.id);
    for (const auto &t : d.supplementaryText)
        available.insert(t.id);
    for (const auto &t : p.texts)
        if (t.script == script && !available.contains(t.target))
            return Result<void>::failure(error("Authored text resource has a dangling physical target."));
    return Result<void>::success();
}
} // namespace salsa::core
