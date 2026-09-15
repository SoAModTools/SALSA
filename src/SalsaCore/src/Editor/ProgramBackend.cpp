#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramService.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <charconv>
namespace salsa::core::editor {
namespace ir = program;
namespace {
template<class T> Result<T> failure(std::string message) {
    return Result<T>::failure({{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject,
                               std::move(message)}});
}
std::span<const ir::ValueId> children(const ir::ValueBuffer &b, ir::ValueId id) {
    const auto &v = b.values.at(id.value);
    return std::span(b.children).subspan(v.first, v.count);
}
Parameter parameter(const ir::ValueBuffer &b, ir::ValueId id, ParameterKey key) {
    Parameter p; p.key = key;
    p.name = "Parameter " + std::to_string(key.index);
    if (key.group) p.name += " (group " + std::to_string(*key.group) + ")";
    const auto &v = b.values.at(id.value);
    if (v.kind == ir::ValueKind::Word) {
        p.kind = ParameterKind::Word; p.value = std::to_string(v.data);
    } else if (v.kind == ir::ValueKind::Expression) {
        auto expression = ir::ProgramValues::expression(b, id);
        if (expression) {
            p.kind = ParameterKind::Expression;
            p.value = SctExpressionLanguage::project(expression.value()).text;
        }
    } else if (v.kind == ir::ValueKind::ActionReference) p.value = "Action " + std::to_string(v.data);
    else if (v.kind == ir::ValueKind::IndexedTextReference || v.kind == ir::ValueKind::SupplementaryTextReference)
        p.value = "Text " + std::to_string(v.data);
    else p.value = "Structured or unresolved value";
    if (p.kind == ParameterKind::ReadOnly)
        p.unavailableReason = "Use the existing selected-item editor for this parameter type.";
    return p;
}
}
ProgramBackend::ProgramBackend(std::shared_ptr<ir::ProgramService> service) : service_(std::move(service)) {}
Change ProgramBackend::describe(const ir::ChangeReceipt &c) {
    return {c.status, c.actions, c.sequences, c.sections, c.scriptLayouts, c.presentationChanged,
            !c.metadata.empty() || !c.sections.empty(), c.diagnostics};
}
Result<OutlinePage> ProgramBackend::outline(Revision revision, OutlineRequest request, std::stop_token stop) {
    auto result = service_->outline(revision, {request.parent, request.offset, request.limit}, stop).get();
    if (!result) return Result<OutlinePage>::failure(result.diagnostics());
    auto &p = result.value();
    OutlinePage out{p.status, p.parent, p.script, p.offset, p.total, {}, p.elapsedMicroseconds};
    out.rows.reserve(p.rows.size());
    for (auto &r : p.rows) {
        std::optional<RowKind> kind;
        if (r.kind) kind = *r.kind == ir::ActionKind::Instruction ? RowKind::Instruction : RowKind::Construct;
        out.rows.push_back({r.subject, r.sequence, std::move(r.label), r.expandable, kind});
    }
    return Result<OutlinePage>::success(std::move(out));
}
Result<Inspection> ProgramBackend::inspect(Revision revision, Subject subject, std::stop_token stop) {
    ir::ReadSelection request; request.descend = false; request.referencedText = false;
    std::visit([&](auto id) {
        using T = decltype(id);
        if constexpr (std::is_same_v<T, SctActionId>) request.actions.push_back(id);
        else if constexpr (std::is_same_v<T, ir::SectionId>) request.sections.push_back(id);
        else if constexpr (std::is_same_v<T, ir::ConditionId>) request.conditions.push_back(id);
        else if constexpr (std::is_same_v<T, SctSequenceId>) request.sequences.push_back(id);
        else if constexpr (std::is_same_v<T, SctTextResourceId>) request.texts.push_back(id);
    }, subject);
    // Other metadata is not an instruction inspection request. Do not issue an
    // empty read, whose meaning could expand as new engine features are added.
    if (request.actions.empty() && request.sections.empty() && request.conditions.empty() &&
        request.sequences.empty() && request.texts.empty())
    {
        if (stop.stop_requested()) return Result<Inspection>::failure({{DiagnosticSeverity::Error,
            DiagnosticCode::Cancelled, "Inspection cancelled."}});
        auto located = service_->locate(revision, subject).get();
        if (!located) return Result<Inspection>::failure(located.diagnostics());
        return Result<Inspection>::success({revision, subject, "Selected item", "Use its authoring tool for details.", {}});
    }
    auto read = service_->read(revision, std::move(request), stop).get();
    if (!read) return Result<Inspection>::failure(read.diagnostics());
    const auto &s = *read.value();
    Inspection out{revision, subject, "Selected item", {}, {}};
    for (const auto &section : s.sections) out.title = section.name;
    for (const auto &c : s.conditions) {
        auto e = ir::ProgramValues::expression(s.payload, c.result);
        if (e) out.description = SctExpressionLanguage::project(e.value()).text;
    }
    for (const auto &a : s.actions) {
        out.title = "Action " + std::to_string(a.id.value);
        if (a.kind != ir::ActionKind::Instruction) {
            out.description = "Structured authoring content. Expand to inspect its bodies and conditions.";
            continue;
        }
        const auto &v = s.payload.values.at(a.value.value);
        const auto *schema = spice::sct::findSctOpcodeSchema(static_cast<std::uint16_t>(v.data));
        out.title += " — " + (schema ? std::string(schema->semantic.mnemonic) : "Opcode " + std::to_string(v.data));
        out.description = std::string("Refresh: ") + (v.detail & 1 ? "skipped" : "normal") +
            "; scheduled: " + (v.detail & 2 ? "yes" : "no");
        auto add = [&](ir::ValueId p, std::optional<std::uint32_t> group) {
            const auto &value = s.payload.values.at(p.value);
            if (value.kind == ir::ValueKind::Parameter && value.count == 1)
                out.parameters.push_back(parameter(s.payload, children(s.payload, p).front(),
                    {static_cast<std::uint32_t>(value.data), group}));
        };
        std::uint32_t group = 0;
        for (auto child : children(s.payload, a.value)) {
            if (s.payload.values.at(child.value).kind == ir::ValueKind::RepeatedGroup) {
                for (auto p : children(s.payload, child)) add(p, group);
                ++group;
            } else add(child, {});
        }
    }
    if (!s.texts.empty()) out.description = "Use Edit selected text for formatted dialogue and choices.";
    return Result<Inspection>::success(std::move(out));
}
Result<Change> ProgramBackend::replaceParameter(Revision revision, ReplaceParameter edit, std::stop_token stop) {
    auto inspected = inspect(revision, edit.action, stop);
    if (!inspected) return Result<Change>::failure(inspected.diagnostics());
    auto &parameters = inspected.value().parameters;
    auto p = std::ranges::find(parameters, edit.parameter, &Parameter::key);
    if (p == parameters.end()) return failure<Change>("The selected parameter no longer exists.");
    ir::ValueBuffer value;
    if (p->kind == ParameterKind::Word) {
        std::uint32_t word = 0;
        auto [end, error] = std::from_chars(edit.text.data(), edit.text.data() + edit.text.size(), word);
        if (error != std::errc{} || end != edit.text.data() + edit.text.size())
            return failure<Change>("Enter an unsigned 32-bit decimal word.");
        value.add(ir::ValueKind::Word, word);
    } else if (p->kind == ParameterKind::Expression) {
        auto parsed = SctExpressionLanguage::parse(edit.text);
        if (!parsed.succeeded()) return failure<Change>("The parameter expression could not be parsed.");
        auto converted = ir::ValueBuffer::expression(*parsed.expression);
        if (!converted) return Result<Change>::failure(converted.diagnostics());
        value = std::move(converted).takeValue();
    } else return failure<Change>(p->unavailableReason);
    auto changed = service_->edit({revision, {ir::ReplaceParameter{edit.action,
        {edit.parameter.index, edit.parameter.group}, std::move(value)}}}, stop).get();
    if (!changed) return Result<Change>::failure(changed.diagnostics());
    return Result<Change>::success(describe(changed.value()));
}
Result<Change> ProgramBackend::history(Revision revision, bool redo, std::stop_token stop) {
    auto changed = redo ? service_->redo(revision, stop).get() : service_->undo(revision, stop).get();
    if (!changed) return Result<Change>::failure(changed.diagnostics());
    return Result<Change>::success(describe(changed.value()));
}
}
