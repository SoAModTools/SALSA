#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramService.h"
#include "SalsaCore/Program/ProgramValues.h"
#include <algorithm>
namespace salsa::core::editor {
namespace ir = program;
namespace {
template <class T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.diagnostics().empty() ? "Editor request failed"
                                                              : result.diagnostics().front().message);
    return std::move(result).takeValue();
}
ir::ReadSelection selection(Subject subject) {
    ir::ReadSelection s;
    s.descend = false;
    s.referencedText = false;
    std::visit(
        [&](auto id) {
            using T = decltype(id);
            if constexpr (std::is_same_v<T, SctActionId>)
                s.actions.push_back(id);
            else if constexpr (std::is_same_v<T, ir::SectionId>)
                s.sections.push_back(id);
            else if constexpr (std::is_same_v<T, SctSequenceId>)
                s.sequences.push_back(id);
            else if constexpr (std::is_same_v<T, SctTextResourceId>)
                s.texts.push_back(id);
            else if constexpr (std::is_same_v<T, ir::ConditionId>)
                s.conditions.push_back(id);
        },
        subject);
    return s;
}
std::span<const ir::ValueId> children(const ir::ValueBuffer &b, ir::ValueId id) {
    const auto &v = b.values.at(id.value);
    return std::span(b.children).subspan(v.first, v.count);
}
} // namespace
Result<NavigationPath> ProgramBackend::locate(Revision r, Subject subject, ViewMode mode) {
    auto p = service_->locate(r, subject, mode == ViewMode::Semantic).get();
    if (!p)
        return Result<NavigationPath>::failure(p.diagnostics());
    return Result<NavigationPath>::success(
        {r, p.value().script, std::move(p.value().subjects), p.value().heldSequence});
}
Result<BrowsePage> ProgramBackend::browse(Revision r, BrowseRequest query, std::stop_token stop) {
    return service_->browse(r, query, stop).get();
}
std::vector<Opcode> ProgramBackend::opcodes() const {
    std::vector<Opcode> out;
    for (const auto &schema : spice::sct::sctOpcodeSchemas())
        if (schema.documentRole == spice::sct::SctOpcodeDocumentRole::Instruction)
            out.push_back({schema.opcode, std::string(schema.semantic.mnemonic)});
    return out;
}
Result<TextDraft> ProgramBackend::text(Revision r, SctTextResourceId id, std::stop_token stop) {
    auto read = service_->read(r, selection(id), stop).get();
    if (!read)
        return Result<TextDraft>::failure(read.diagnostics());
    const auto &s = *read.value();
    const auto &t = s.texts.at(0);
    auto value = ir::ProgramValues::text(s.payload, t.value);
    if (!value)
        return Result<TextDraft>::failure(value.diagnostics());
    return Result<TextDraft>::success(
        {r, t.id, t.script, t.sourceName, t.supplementary, t.kind, std::move(value).takeValue()});
}
Result<StructureDraft> ProgramBackend::structure(Revision r, SctActionId id, std::stop_token stop) {
    auto read = service_->read(r, selection(id), stop).get();
    if (!read)
        return Result<StructureDraft>::failure(read.diagnostics());
    const auto &s = *read.value();
    const auto &a = s.actions.at(0);
    StructureDraft out{r, id, a.kind == ir::ActionKind::Switch};
    for (const auto &arm : s.arms) {
        ArmDraft row;
        row.id = arm.id;
        row.includesDefault = arm.includesDefault;
        row.fallthrough = arm.caseExit == ir::CaseExit::Fallthrough;
        row.labels.assign(s.caseLabels.begin() + arm.firstLabel,
                          s.caseLabels.begin() + arm.firstLabel + arm.labelCount);
        if (arm.condition.valid()) {
            auto c = std::ranges::find(s.conditions, arm.condition, &ir::ConditionRegion::id);
            if (c != s.conditions.end()) {
                auto value = ir::ProgramValues::expression(s.payload, c->result);
                if (value)
                    row.condition = value.value();
            }
        }
        out.arms.push_back(std::move(row));
    }
    return Result<StructureDraft>::success(std::move(out));
}
Result<Change> ProgramBackend::execute(Revision r, EditIntent intent, std::stop_token stop) {
    if(auto *story=std::get_if<StoryEdit>(&intent)) {
        auto result=service_->edit({r,{program::SemanticOperation{std::move(*story)}}},stop).get();
        if(!result)return Result<Change>::failure(result.diagnostics());
        return Result<Change>::success(describe(result.value()));
    }
    if(auto *feature=std::get_if<FeatureEdit>(&intent)) return featureEdit(r,std::move(*feature),stop);
    if (std::holds_alternative<PasteSelection>(intent) || std::holds_alternative<MoveSelection>(intent) ||
        std::holds_alternative<DeleteSelection>(intent) || std::holds_alternative<WriteNote>(intent) ||
        std::holds_alternative<WriteOrganization>(intent) || std::holds_alternative<WriteOpcode>(intent) ||
        std::holds_alternative<NewSection>(intent) || std::holds_alternative<RenameCodeSection>(intent))
        return productivity(r, std::move(intent), stop);
    try {
        std::vector<ir::Primitive> edits;
        std::optional<Subject> focus;
        auto read = [&](Subject id) { return take(service_->read(r, selection(id), stop).get()); };
        auto insertion = [&](Subject anchor) {
            std::optional<ir::ArmId> selectedArm;
            if (auto id = std::get_if<ir::ArmId>(&anchor)) {
                selectedArm = *id;
                auto path = take(service_->locate(r, anchor).get());
                anchor = path.subjects.at(path.subjects.size() - 2);
            }
            auto s = read(anchor);
            SctSequenceId sequence;
            std::size_t position = 0;
            if (selectedArm) {
                auto arm = std::ranges::find(s->arms, *selectedArm, &ir::Arm::id);
                if (arm == s->arms.end())
                    throw std::runtime_error("Missing branch body");
                return std::pair(arm->body, std::size_t{0});
            }
            if (auto condition = std::get_if<ir::ConditionId>(&anchor)) {
                auto c = std::ranges::find(s->conditions, *condition, &ir::ConditionRegion::id);
                if (c == s->conditions.end())
                    throw std::runtime_error("Missing condition preparation");
                auto seq = std::ranges::find(s->sequences, c->preparation, &ir::Sequence::id);
                if (seq == s->sequences.end())
                    throw std::runtime_error("Missing preparation sequence");
                return std::pair(c->preparation, seq->order->size());
            }
            if (!s->actions.empty()) {
                sequence = s->actions.front().owner;
                auto owner = std::ranges::find(s->sequences, sequence, &ir::Sequence::id);
                if (owner == s->sequences.end())
                    throw std::runtime_error("Missing action owner");
                const auto &order = *owner->order;
                position = std::ranges::find(order, s->actions.front().id) - order.begin() + 1;
            } else if (!s->sections.empty())
                sequence = s->sections.front().body;
            else if (!s->sequences.empty())
                sequence = s->sequences.front().id;
            if (!sequence.valid())
                throw std::runtime_error("Select an instruction or executable body for insertion");
            return std::pair(sequence, position);
        };
        std::visit(
            [&](auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, SetExpression>) {
                    auto value = take(ir::ValueBuffer::expression(e.value));
                    if (auto *id = std::get_if<ir::ConditionId>(&e.subject))
                        edits.push_back(ir::ReplaceCondition{*id, std::move(value)});
                    else if (e.parameter && std::holds_alternative<SctActionId>(e.subject))
                        edits.push_back(ir::ReplaceParameter{std::get<SctActionId>(e.subject),
                                                             {e.parameter->index, e.parameter->group},
                                                             std::move(value)});
                    else
                        throw std::runtime_error("Select an expression parameter or condition");
                } else if constexpr (std::is_same_v<T, BindReference>) {
                    auto inspected = take(inspect(r, e.action, stop));
                    auto p = std::ranges::find(inspected.parameters, e.parameter, &Parameter::key);
                    if (p == inspected.parameters.end())
                        throw std::runtime_error("Missing reference operand");
                    ir::ValueBuffer b;
                    if (p->kind == ParameterKind::ActionTarget && std::holds_alternative<SctActionId>(e.target))
                        b.add(ir::ValueKind::ActionReference, std::get<SctActionId>(e.target).value);
                    else if ((p->kind == ParameterKind::IndexedText || p->kind == ParameterKind::FooterText) &&
                             std::holds_alternative<SctTextResourceId>(e.target)) {
                        auto t = take(text(r, std::get<SctTextResourceId>(e.target), stop));
                        if (t.footer != (p->kind == ParameterKind::FooterText))
                            throw std::runtime_error("Text storage does not match this operand");
                        b.add(t.footer ? ir::ValueKind::SupplementaryTextReference
                                       : ir::ValueKind::IndexedTextReference,
                              t.id.value);
                    } else
                        throw std::runtime_error("Target type does not match this operand");
                    edits.push_back(
                        ir::ReplaceParameter{e.action, {e.parameter.index, e.parameter.group}, std::move(b)});
                } else if constexpr (std::is_same_v<T, SetTiming>) {
                    auto s = read(e.action);
                    const auto &a = s->actions.at(0);
                    const auto &v = s->payload.values.at(a.value.value);
                    std::vector<ir::ValueBuffer> parts;
                    if (e.schedule)
                        parts.push_back(take(ir::ValueBuffer::expression(*e.schedule)));
                    auto list = children(s->payload, a.value);
                    for (std::size_t n = (v.detail & 2) ? 1 : 0; n < list.size(); ++n)
                        parts.push_back(take(ir::ProgramValues::slice(s->payload, list[n])));
                    edits.push_back(ir::ReplaceInstruction{
                        e.action, take(ir::ProgramValues::compose(
                                      v.kind, v.data, (e.skipRefresh ? 1u : 0u) | (e.schedule ? 2u : 0u), parts))});
                } else if constexpr (std::is_same_v<T, WriteStringParameter>) {
                    auto inspected = take(inspect(r, e.action, stop));
                    auto p = std::ranges::find(inspected.parameters, e.parameter, &Parameter::key);
                    if (p == inspected.parameters.end() || p->kind != ParameterKind::FooterText)
                        throw std::runtime_error("Select a string-valued parameter");
                    edits.push_back(ir::ReplaceParameter{e.action, {e.parameter.index, e.parameter.group},
                        take(ir::ProgramValues::text(e.value)), p->textKind});
                } else if constexpr (std::is_same_v<T, WriteText>) {
                    if (e.draft.revision != r)
                        throw std::runtime_error("Text draft is stale; keep the draft and reload before applying");
                    edits.push_back(ir::ReplaceText{e.draft.id, take(ir::ProgramValues::text(e.draft.value))});
                } else if constexpr (std::is_same_v<T, CreateText>) {
                    edits.push_back(
                        ir::CreateText{e.script, e.name, e.footer, e.kind, take(ir::ProgramValues::text(e.value))});
                } else if constexpr (std::is_same_v<T, DeleteItem>) {
                    if (auto *id = std::get_if<SctTextResourceId>(&e.subject))
                        edits.push_back(ir::RemoveText{*id});
                    else if (auto *id = std::get_if<SctActionId>(&e.subject))
                        edits.push_back(ir::SemanticOperation{ir::EraseAction{*id}});
                    else
                        throw std::runtime_error("Select text or an authored action to delete");
                } else if constexpr (std::is_same_v<T, InsertOpcode>) {
                    auto [sequence, position] = insertion(e.anchor);
                    spice::sct::SctInstructionFactoryRequest request{e.opcode};
                    auto catalog=take(organization(r,{},stop));
                    if(auto custom=std::ranges::find(catalog.opcodes,e.opcode,&OpcodeCustomization::opcode);custom!=catalog.opcodes.end())
                        for(auto [index,value]:custom->defaults)request.parameterOverrides.push_back({{index,{}},spice::sct::SctEncodedWordValue{value}});
                    edits.push_back(ir::InsertInstruction{sequence, position, take(ir::ProgramValues::instruction(request))});
                } else if constexpr (std::is_same_v<T, InsertStructure>) {
                    auto [sequence, position] = insertion(e.anchor);
                    ir::InsertConstruct c;
                    c.sequence = sequence;
                    c.position = position;
                    c.condition = take(ir::ValueBuffer::expression(e.condition));
                    if (e.kind == "If") {
                        c.kind = ir::ActionKind::Conditional;
                        c.arms.push_back({{}, c.condition});
                        c.arms.push_back({});
                    } else if (e.kind == "Switch") {
                        c.kind = ir::ActionKind::Switch;
                        ir::ArmEdit arm;
                        arm.labels = {0};
                        c.arms.push_back(arm);
                        arm.labels.clear();
                        arm.includesDefault = true;
                        c.arms.push_back(arm);
                    } else {
                        if (e.kind != "While" && e.kind != "Do While" && e.kind != "For")
                            throw std::runtime_error("Unknown construct kind");
                        c.kind = ir::ActionKind::Loop;
                        c.loopKind = e.kind == "For"        ? ir::LoopKind::For
                                     : e.kind == "Do While" ? ir::LoopKind::DoWhile
                                                            : ir::LoopKind::While;
                    }
                    edits.push_back(ir::SemanticOperation{std::move(c)});
                } else if constexpr (std::is_same_v<T, InsertTransfer>) {
                    auto [sequence, position] = insertion(e.anchor);
                    edits.push_back(ir::SemanticOperation{ir::InsertTransfer{
                        sequence, position, e.continueLoop ? ir::TransferKind::Continue : ir::TransferKind::Break,
                        e.target}});
                } else if constexpr (std::is_same_v<T, WriteArms>) {
                    if (e.draft.revision != r)
                        throw std::runtime_error("Branch draft is stale");
                    ir::ReplaceArms change;
                    change.action = e.draft.action;
                    for (auto &a : e.draft.arms) {
                        ir::ArmEdit arm;
                        arm.id = a.id;
                        if (a.condition)
                            arm.condition = take(ir::ValueBuffer::expression(*a.condition));
                        arm.labels = a.labels;
                        arm.includesDefault = a.includesDefault;
                        arm.exit = a.fallthrough ? ir::CaseExit::Fallthrough : ir::CaseExit::ExitSwitch;
                        change.arms.push_back(std::move(arm));
                    }
                    edits.push_back(ir::SemanticOperation{std::move(change)});
                } else if constexpr (std::is_same_v<T, PromoteFlow>)
                    edits.push_back(ir::SemanticOperation{ir::PromoteFlow{e.header, e.continuation}});
            },
            intent);
        auto changed = take(service_->edit({r, std::move(edits)}, stop).get());
        auto out = describe(changed);
        if (std::holds_alternative<CreateText>(intent) && !changed.texts.empty())
            out.suggestedSelection = changed.texts.front();
        return Result<Change>::success(std::move(out));
    } catch (const std::exception &e) {
        return Result<Change>::failure(
            {{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, e.what()}});
    }
}
} // namespace salsa::core::editor
