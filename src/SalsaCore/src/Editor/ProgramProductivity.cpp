#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramPersistence.h"
#include <algorithm>
#include <set>
namespace salsa::core::editor {
namespace ir = program;
namespace {
template <class T> T take(Result<T> r) {
    if (!r)
        throw std::runtime_error(r.diagnostics().empty() ? "Editor operation failed"
                                                         : r.diagnostics().front().message);
    return std::move(r).takeValue();
}
} // namespace
Result<std::vector<std::byte>> ProgramBackend::copy(Revision r, CopySelection selection,
                                                    std::stop_token stop) {
    auto f = service_->copy(r, std::move(selection), stop).get();
    if (!f)
        return Result<std::vector<std::byte>>::failure(f.diagnostics());
    return ir::ProgramPersistence::encodeFragment(*f.value());
}
Result<FragmentReview> ProgramBackend::reviewPaste(Revision r, Subject anchor,
                                                   std::span<const std::byte> bytes, std::stop_token stop) {
    auto f = ir::ProgramPersistence::decodeFragment(bytes);
    if (!f)
        return Result<FragmentReview>::failure(f.diagnostics());
    return service_->reviewPaste(r, anchor, f.value(), stop).get();
}
Result<Organization> ProgramBackend::organization(Revision r, SctScriptId script, std::stop_token stop) {
    return service_->organization(r, script, stop).get();
}
Result<Change> ProgramBackend::productivity(Revision r, EditIntent intent, std::stop_token stop) {
    try {
        std::vector<ir::Primitive> edits;
        if (auto *e = std::get_if<PasteSelection>(&intent)) {
            auto f = take(ir::ProgramPersistence::decodeFragment(e->bytes));
            edits.push_back(
                ir::SemanticOperation{ir::PasteFragment{std::move(f), e->anchor, std::move(e->bindings)}});
        } else if (auto *e = std::get_if<NewSection>(&intent)) {
            auto org = take(organization(r, e->script, stop));
            edits.push_back(ir::AddSection{e->script, e->name, org.sections.size()});
        } else if (auto *e = std::get_if<RenameCodeSection>(&intent))
            edits.push_back(ir::RenameSection{e->section, e->name});
        else if (auto *e = std::get_if<WriteOrganization>(&intent))
            edits.push_back(ir::SemanticOperation{ir::Organize{*e}});
        else if (auto *e = std::get_if<WriteOpcode>(&intent))
            edits.push_back(ir::SemanticOperation{ir::CustomizeOpcode{e->opcode}});
        else if (auto *e = std::get_if<WriteNote>(&intent)) {
            edits.push_back(ir::SemanticOperation{ir::SetNote{e->note}});
        } else {
            auto items = std::holds_alternative<DeleteSelection>(intent)
                             ? std::get<DeleteSelection>(intent).items
                             : std::get<MoveSelection>(intent).items;
            auto fragment = take(service_->copy(r, {items}, stop).get());
            if (auto *move = std::get_if<MoveSelection>(&intent)) {
                if (!fragment->content.sections.empty()) {
                    auto model = take(organization(r, fragment->script, stop));
                    std::vector<ir::SectionId> selected, order;
                    for (auto &s : fragment->content.sections)
                        selected.push_back(s.id);
                    auto *anchor = std::get_if<ir::SectionId>(&move->anchor);
                    if (!anchor || std::ranges::find(selected, *anchor) != selected.end())
                        throw std::runtime_error("Choose an unselected section in the same script");
                    for (auto &s : model.sections)
                        order.push_back(std::get<ir::SectionId>(s.subject));
                    for (auto id : selected)
                        std::erase(order, id);
                    auto at = std::ranges::find(order, *anchor);
                    if (at == order.end())
                        throw std::runtime_error("Section destination is in another script");
                    order.insert(at + 1, selected.begin(), selected.end());
                    edits.push_back(
                        ir::SemanticOperation{ir::Organize{{fragment->script, order, model.folders}}});
                } else {
                    auto path = take(service_->locate(r, move->anchor).get());
                    if (path.script != fragment->script)
                        throw std::runtime_error(
                            "Move stays within the parent script; use copy/paste across scripts");
                    SctSequenceId destination;
                    std::size_t position = 0;
                    ir::ReadSelection read;
                    read.descend = false;
                    read.referencedText = false;
                    if (auto id = std::get_if<SctActionId>(&move->anchor))
                        read.actions.push_back(*id);
                    else if (auto id = std::get_if<SctSequenceId>(&move->anchor))
                        read.sequences.push_back(*id);
                    else if (auto id = std::get_if<ir::SectionId>(&move->anchor))
                        read.sections.push_back(*id);
                    else if (auto id = std::get_if<ir::ConditionId>(&move->anchor))
                        read.conditions.push_back(*id);
                    else if (auto id = std::get_if<ir::ArmId>(&move->anchor)) {
                        read.actions.push_back(
                            std::get<SctActionId>(path.subjects.at(path.subjects.size() - 2)));
                    } else
                        throw std::runtime_error("Select a destination body or action");
                    auto scope = take(service_->read(r, read, stop).get());
                    if (auto id = std::get_if<SctActionId>(&move->anchor)) {
                        if (std::ranges::find(fragment->roots, *id) != fragment->roots.end())
                            throw std::runtime_error("Destination is selected");
                        auto a = std::ranges::find(scope->actions, *id, &ir::Action::id);
                        destination = a->owner;
                        auto seq = std::ranges::find(scope->sequences, destination, &ir::Sequence::id);
                        position = std::ranges::find(*seq->order, *id) - seq->order->begin() + 1;
                    } else if (auto id = std::get_if<ir::ArmId>(&move->anchor)) {
                        auto arm = std::ranges::find(scope->arms, *id, &ir::Arm::id);
                        if (arm == scope->arms.end())
                            throw std::runtime_error("Missing arm");
                        destination = arm->body;
                    } else if (!scope->sections.empty())
                        destination = scope->sections.front().body;
                    else if (!scope->conditions.empty())
                        destination = scope->conditions.front().preparation;
                    else
                        destination = scope->sequences.front().id;
                    edits.push_back(
                        ir::SemanticOperation{ir::MoveActions{fragment->roots, destination, position}});
                }
            } else {
                for (auto id : fragment->roots)
                    edits.push_back(ir::SemanticOperation{ir::EraseAction{id}});
                for (auto &section : fragment->content.sections) {
                    auto seq =
                        std::ranges::find(fragment->content.sequences, section.body, &ir::Sequence::id);
                    for (auto id : *seq->order)
                        edits.push_back(ir::SemanticOperation{ir::EraseAction{id}});
                    edits.push_back(ir::EraseEmptySection{section.id});
                }
            }
        }
        auto result = take(service_->edit({r, std::move(edits)}, stop).get());
        auto change = describe(result);
        if(auto *move=std::get_if<MoveSelection>(&intent);move && change.selected.empty())change.selected=move->items;
        return Result<Change>::success(std::move(change));
    } catch (const std::exception &e) {
        return Result<Change>::failure(
            {{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, e.what()}});
    }
}
} // namespace salsa::core::editor
