#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramService.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <charconv>
#include <sstream>
namespace salsa::core::editor {
namespace ir = program;
namespace {
template <class T> Result<T> failure(std::string message) {
    return Result<T>::failure(
        {{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)}});
}
std::span<const ir::ValueId> children(const ir::ValueBuffer &b, ir::ValueId id) {
    const auto &v = b.values.at(id.value);
    return std::span(b.children).subspan(v.first, v.count);
}
Parameter parameter(const ir::ValueBuffer &b, ir::ValueId id, ParameterKey key) {
    Parameter p;
    p.key = key;
    p.name = "Parameter " + std::to_string(key.index);
    if (key.group)
        p.name += " (group " + std::to_string(*key.group) + ")";
    const auto &v = b.values.at(id.value);
    if (v.kind == ir::ValueKind::Word) {
        p.kind = ParameterKind::Word;
        p.value = std::to_string(v.data);
    } else if (v.kind == ir::ValueKind::Expression) {
        auto expression = ir::ProgramValues::expression(b, id);
        if (expression) {
            p.kind = ParameterKind::Expression;
            p.expression = expression.value();
            p.value = SctExpressionLanguage::project(expression.value()).text;
        }
    } else if (v.kind == ir::ValueKind::ActionReference) {
        p.kind = ParameterKind::ActionTarget;
        p.target = SctActionId{v.data};
        p.value = "Action " + std::to_string(v.data);
    } else if (v.kind == ir::ValueKind::IndexedTextReference || v.kind == ir::ValueKind::SupplementaryTextReference) {
        p.kind = v.kind == ir::ValueKind::IndexedTextReference ? ParameterKind::IndexedText : ParameterKind::FooterText;
        p.target = SctTextResourceId{v.data};
        p.value = "Text " + std::to_string(v.data);
    } else if (v.kind == ir::ValueKind::Words) {
        p.kind = ParameterKind::Words;
        for (auto w : children(b, id))
            p.value += std::to_string(b.values[w.value].data) + " ";
    } else
        p.value = "Structured or unresolved value";
    if (p.kind == ParameterKind::ReadOnly)
        p.unavailableReason = "Use the existing selected-item editor for this parameter type.";
    return p;
}
} // namespace
ProgramBackend::ProgramBackend(std::shared_ptr<ir::ProgramService> service) : service_(std::move(service)) {}
Change ProgramBackend::describe(const ir::ChangeReceipt &c) {
    return {c.status,
            c.actions,
            c.sequences,
            c.sections,
            c.scriptLayouts,
            c.presentationChanged,
            !c.metadata.empty() || !c.sections.empty(),
            c.diagnostics,
            c.texts,
            {},
            c.projectionSequences,
            c.selected,
            c.conditions};
}
Result<OutlinePage> ProgramBackend::outline(Revision revision, OutlineRequest request, std::stop_token stop) {
    auto result =
        service_
            ->outline(revision,
                      {request.parent, request.offset, request.limit, request.mode == ViewMode::Semantic},
                      stop)
            .get();
    if (!result)
        return Result<OutlinePage>::failure(result.diagnostics());
    auto &p = result.value();
    OutlinePage out{p.status, p.parent, p.script, p.offset, p.total, {}, p.elapsedMicroseconds};
    out.rows.reserve(p.rows.size());
    for (auto &r : p.rows) {
        std::optional<RowKind> kind;
        if (r.kind)
            kind = *r.kind == ir::ActionKind::Instruction ? RowKind::Instruction : RowKind::Construct;
        out.rows.push_back({r.subject, r.sequence, std::move(r.label), r.expandable, kind,
                            std::move(r.groupingIssue), r.color, r.bookmarked, r.chain, r.condition,
                            r.preparation});
    }
    return Result<OutlinePage>::success(std::move(out));
}
Result<Inspection> ProgramBackend::inspect(Revision revision, Subject subject, std::stop_token stop) {
    ir::ReadSelection request;
    request.descend = false;
    request.referencedText = false;
    std::visit(
        [&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, SctActionId>)
                request.actions.push_back(id);
            else if constexpr (std::is_same_v<T, ir::SectionId>)
                request.sections.push_back(id);
            else if constexpr (std::is_same_v<T, ir::ConditionId>)
                request.conditions.push_back(id);
            else if constexpr (std::is_same_v<T, SctSequenceId>)
                request.sequences.push_back(id);
            else if constexpr (std::is_same_v<T, SctTextResourceId>)
                request.texts.push_back(id);
        },
        subject);
    if (std::holds_alternative<ir::ArmId>(subject)) {
        auto located = service_->locate(revision, subject).get();
        if (!located)
            return Result<Inspection>::failure(located.diagnostics());
        for (auto it = located.value().subjects.rbegin(); it != located.value().subjects.rend(); ++it)
            if (auto action = std::get_if<SctActionId>(&*it)) {
                request.actions.push_back(*action);
                break;
            }
    }
    // Other metadata is not an instruction inspection request. Do not issue an
    // empty read, whose meaning could expand as new engine features are added.
    if (request.actions.empty() && request.sections.empty() && request.conditions.empty() &&
        request.sequences.empty() && request.texts.empty()) {
        if (stop.stop_requested())
            return Result<Inspection>::failure(
                {{DiagnosticSeverity::Error, DiagnosticCode::Cancelled, "Inspection cancelled."}});
        auto located = service_->locate(revision, subject).get();
        if (!located)
            return Result<Inspection>::failure(located.diagnostics());
        return Result<Inspection>::success({revision,
                                            subject,
                                            "Selected item",
                                            "Select a body for insertion or open its authoring tool.",
                                            {},
                                            located.value().script});
    }
    auto read = service_->read(revision, std::move(request), stop).get();
    if (!read)
        return Result<Inspection>::failure(read.diagnostics());
    const auto &s = *read.value();
    Inspection out{revision, subject, "Selected item", {}, {}};
    for (const auto &section : s.sections) {
        out.title = section.name;
        out.script = section.script;
    }
    if (!s.sequences.empty())
        out.script = s.sequences.front().script;
    if (!s.texts.empty()) {
        out.text = s.texts.front().id;
        out.script = s.texts.front().script;
    }
    if (auto condition = std::get_if<ir::ConditionId>(&subject))
        out.conditionTarget = *condition;
    if (auto arm = std::get_if<ir::ArmId>(&subject)) {
        auto found = std::ranges::find(s.arms, *arm, &ir::Arm::id);
        if (found != s.arms.end()) {
            out.construct = found->owner;
            out.body = found->body;
            if (found->condition.valid())
                out.conditionTarget = found->condition;
        }
    }
    for (const auto &a : s.actions) {
        out.title = "Action " + std::to_string(a.id.value);
        if (a.kind != ir::ActionKind::Instruction) {
            out.structured = true;
            out.construct = a.id;
            out.editableBranches = a.kind == ir::ActionKind::Conditional || a.kind == ir::ActionKind::Switch;
            if (std::holds_alternative<SctActionId>(subject)) {
                if (a.kind == ir::ActionKind::Conditional) {
                    auto arm = std::ranges::find(s.arms, a.id, &ir::Arm::owner);
                    if (arm != s.arms.end()) {
                        if (arm->condition.valid())
                            out.conditionTarget = arm->condition;
                        out.body = arm->body;
                    }
                } else if (a.kind == ir::ActionKind::Loop) {
                    auto loop = std::ranges::find(s.loops, a.id, &ir::Loop::owner);
                    if (loop != s.loops.end()) {
                        out.conditionTarget = loop->condition;
                        out.body = loop->body;
                    }
                } else if (a.kind == ir::ActionKind::Switch) {
                    auto sw = std::ranges::find(s.switches, a.id, &ir::Switch::owner);
                    if (sw != s.switches.end())
                        out.conditionTarget = sw->selector;
                }
            }
            out.description = "Structured authoring content. Edit its condition from this header.";
            continue;
        }
        out.instruction = true;
        const auto &v = s.payload.values.at(a.value.value);
        out.skipRefresh = v.detail & 1;
        if (v.detail & 2) {
            auto schedule = ir::ProgramValues::expression(s.payload, children(s.payload, a.value).front());
            if (schedule)
                out.schedule = schedule.value();
        }
        auto grouping = service_->assessFlow(revision, a.id, stop).get();
        if (!grouping)
            return Result<Inspection>::failure(grouping.diagnostics());
        out.evidence = std::move(grouping.value().issue);
        const auto *schema = spice::sct::findSctOpcodeSchema(static_cast<std::uint16_t>(v.data));
        out.title +=
            " — " + (schema ? std::string(schema->semantic.mnemonic) : "Opcode " + std::to_string(v.data));
        auto custom =
            std::ranges::find(s.catalog, static_cast<std::uint16_t>(v.data), &OpcodeCustomization::opcode);
        if (custom != s.catalog.end() && !custom->name.empty())
            out.title = custom->name + " (action " + std::to_string(a.id.value) + ")";
        out.description = std::string("Refresh: ") + (v.detail & 1 ? "skipped" : "normal") +
                          "; scheduled: " + (v.detail & 2 ? "yes" : "no");
        if (custom != s.catalog.end() && !custom->description.empty())
            out.description = custom->description + "\n" + out.description;
        if (custom != s.catalog.end() && !custom->note.empty())
            out.description += "\n" + custom->note;
        auto add = [&](ir::ValueId p, std::optional<std::uint32_t> group) {
            const auto &value = s.payload.values.at(p.value);
            if (value.kind == ir::ValueKind::Parameter && value.count == 1) {
                auto row = parameter(s.payload, children(s.payload, p).front(),
                                     {static_cast<std::uint32_t>(value.data), group});
                if (schema && value.data < schema->parameterCatalogCount) {
                    const auto &definition = schema->parameterCatalog[value.data];
                    const auto role = schema->semantic.parameterRoles[value.data];
                    if (!role.empty())
                        row.name =
                            std::string(role) + (group ? " (group " + std::to_string(*group) + ")" : "");
                    if (row.kind == ParameterKind::ReadOnly) {
                        if (definition.referenceKind == spice::sct::SctOpcodeReferenceKind::Instruction)
                            row.kind = ParameterKind::ActionTarget;
                        else if (definition.referenceKind == spice::sct::SctOpcodeReferenceKind::Text) {
                            row.kind = ParameterKind::IndexedText;
                            for (unsigned n = 0; n < schema->textReferenceCount; ++n)
                                if (schema->textReferences[n].parameterIndex == value.data)
                                    row.kind = schema->textReferences[n].storage ==
                                                       spice::sct::SctTextStorage::Footer
                                                   ? ParameterKind::FooterText
                                                   : ParameterKind::IndexedText;
                        }
                    }
                }
                if (custom != s.catalog.end())
                    for (auto &[index, name] : custom->parameters)
                        if (index == row.key.index)
                            row.name = name;
                out.parameters.push_back(std::move(row));
            }
        };
        std::uint32_t group = 0;
        for (auto child : children(s.payload, a.value)) {
            if (s.payload.values.at(child.value).kind == ir::ValueKind::RepeatedGroup) {
                for (auto p : children(s.payload, child))
                    add(p, group);
                ++group;
            } else
                add(child, {});
        }
    }
    if (out.conditionTarget) {
        auto c = std::ranges::find(s.conditions, *out.conditionTarget, &ir::ConditionRegion::id);
        if (c != s.conditions.end()) {
            auto e = ir::ProgramValues::expression(s.payload, c->result);
            if (e) {
                out.description = SctExpressionLanguage::project(e.value()).text;
                out.condition = e.value();
            }
            out.construct = c->owner;
            if (std::holds_alternative<ir::ConditionId>(subject))
                out.body = c->preparation;
            out.description +=
                "\nBefore check instructions run in order before this expression is evaluated.";
        }
    }
    if (!s.texts.empty())
        out.description = "Use Edit selected text for formatted dialogue and choices.";
    return Result<Inspection>::success(std::move(out));
}
Result<Change> ProgramBackend::replaceParameter(Revision revision, ReplaceParameter edit, std::stop_token stop) {
    auto inspected = inspect(revision, edit.action, stop);
    if (!inspected)
        return Result<Change>::failure(inspected.diagnostics());
    auto &parameters = inspected.value().parameters;
    auto p = std::ranges::find(parameters, edit.parameter, &Parameter::key);
    if (p == parameters.end())
        return failure<Change>("The selected parameter no longer exists.");
    ir::ValueBuffer value;
    if (p->kind == ParameterKind::Word) {
        std::uint32_t word = 0;
        auto [end, error] = std::from_chars(edit.text.data(), edit.text.data() + edit.text.size(), word);
        if (error != std::errc{} || end != edit.text.data() + edit.text.size())
            return failure<Change>("Enter an unsigned 32-bit decimal word.");
        value.add(ir::ValueKind::Word, word);
    } else if (p->kind == ParameterKind::Expression) {
        auto parsed = SctExpressionLanguage::parse(edit.text);
        if (!parsed.succeeded())
            return failure<Change>("The parameter expression could not be parsed.");
        auto converted = ir::ValueBuffer::expression(*parsed.expression);
        if (!converted)
            return Result<Change>::failure(converted.diagnostics());
        value = std::move(converted).takeValue();
    } else if (p->kind == ParameterKind::Words) {
        std::istringstream input(edit.text);
        std::string token;
        std::vector<ir::ValueId> words;
        while (input >> token) {
            std::uint32_t word = 0;
            auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), word);
            if (error != std::errc{} || end != token.data() + token.size())
                return failure<Change>("Enter whitespace-separated 32-bit decimal words.");
            words.push_back(value.add(ir::ValueKind::Word, word));
        }
        value.add(ir::ValueKind::Words, 0, 0, words);
    } else
        return failure<Change>(p->unavailableReason);
    auto changed =
        service_
            ->edit(
                {revision,
                 {ir::ReplaceParameter{edit.action, {edit.parameter.index, edit.parameter.group}, std::move(value)}}},
                stop)
            .get();
    if (!changed)
        return Result<Change>::failure(changed.diagnostics());
    return Result<Change>::success(describe(changed.value()));
}
Result<Change> ProgramBackend::history(Revision revision, bool redo, std::stop_token stop) {
    auto changed = redo ? service_->redo(revision, stop).get() : service_->undo(revision, stop).get();
    if (!changed)
        return Result<Change>::failure(changed.diagnostics());
    auto result=describe(changed.value());
    if(!redo)result.selected.clear();
    return Result<Change>::success(std::move(result));
}
} // namespace salsa::core::editor
