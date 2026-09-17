#include "ProgramGrouping.h"
#include "ProgramInternal.h"
#include "ProgramTypedValues.h"
#include <algorithm>
#include <set>
namespace salsa::core::program {
namespace {
namespace sct = spice::sct;
using IdMap = std::unordered_map<std::uint64_t, SctActionId>;
std::string issueText(const sct::SctStructureIssue &issue) {
    using R = sct::SctStructuredRejectionReason;
    if (issue.rejectionReason)
        switch (*issue.rejectionReason) {
        case R::MissingTarget:
            return "A control-flow target is missing or unresolved.";
        case R::ExternalEntry:
            return "Another route enters the proposed block's interior.";
        case R::UnsupportedExitShape:
            return "SPICE cannot group this exit pattern.";
        case R::NonContiguousCandidate:
            return "The proposed block is not contiguous.";
        case R::OverlappingArms:
            return "The proposed branch bodies overlap.";
        case R::AmbiguousSwitchCases:
            return "Switch case boundaries are ambiguous.";
        case R::ClosedComponentWithoutExit:
            return "The proposed region has no identifiable exit.";
        case R::HistoricalConflict:
            return "Historical flow conflicts with the current instructions.";
        case R::UnsupportedHistoricalShape:
            return "Historical flow does not establish a current group.";
        }
    using K = sct::SctStructureIssueKind;
    switch (issue.kind) {
    case K::UnresolvedControlFlow:
        return "A control-flow target is unresolved.";
    case K::CrossSectionNonCallControlFlow:
        return "Control flow crosses a section boundary.";
    case K::MissingControlFlow:
        return "An instruction has no known control-flow interpretation.";
    case K::IrreducibleCycle:
        return "This cycle cannot be represented as a single structured loop.";
    case K::MultipleEntryRegion:
        return "The proposed region has multiple entry points.";
    case K::AmbiguousJoin:
        return "The continuation after these branches is ambiguous.";
    case K::OverlappingRegions:
        return "Proposed control-flow regions overlap.";
    case K::AmbiguousSwitchCases:
        return "Switch case boundaries are ambiguous.";
    case K::HistoricalEdgeConflict:
        return "Historical flow conflicts with current instructions.";
    default:
        return "SPICE could not group this control-flow region.";
    }
}
void addIssue(ControlFlowDiagnostics &out, SctActionId id, std::string text) {
    auto &message = out.issues[id];
    if (message.find(text) != std::string::npos)
        return;
    if (!message.empty())
        message += '\n';
    message += std::move(text);
}
std::shared_ptr<ControlFlowDiagnostics> project(const sct::SctSectionStructure &section,
                                                const sct::SctScriptSectionContent &code, const IdMap &ids) {
    auto out = std::make_shared<ControlFlowDiagnostics>();
    for (const auto &issue : section.issues) {
        // A resolved Jump may deliberately continue in another section. This
        // limits section-local grouping, but is not an instruction warning.
        if (issue.kind == sct::SctStructureIssueKind::CrossSectionNonCallControlFlow && issue.instruction) {
            auto instruction = std::ranges::find(code.instructions, *issue.instruction,
                                                 &sct::SctDocumentInstruction::id);
            if (instruction != code.instructions.end() && instruction->opcode == 10)
                continue;
        }
        auto native = issue.instruction           ? *issue.instruction
                      : code.instructions.empty() ? sct::SctInstructionId{}
                                                  : code.instructions.front().id;
        if (auto at = ids.find(native.value()); at != ids.end())
            addIssue(*out, at->second, issueText(issue));
    }
    return out;
}
struct SourceView {
    const ProgramData &d;
    const Value *find(ValueId id) const { return d.values.find(id.value); }
    std::span<const ValueId> children(ValueId id) const { return d.child(id); }
};
class Values : public TypedValues<SourceView> {
public:
    explicit Values(const SourceView &p) : TypedValues(p) {}
    sct::SctDocumentParameter parameter(ValueId at) const {
        const auto &row = get(at, ValueKind::Parameter);
        auto children = p.children(at);
        require(children.size() == 1, "Incomplete instruction operand");
        auto id = children.front();
        const auto *v = p.find(id);
        require(v, "Missing operand value");
        sct::SctDocumentParameter result;
        result.schemaIndex = static_cast<std::uint32_t>(row.data);
        switch (v->kind) {
        case ValueKind::Word:
            result.value = sct::SctEncodedWordValue{static_cast<std::uint32_t>(v->data)};
            break;
        case ValueKind::Words:
            result.value = sct::SctTerminatedWordSequenceValue{words(id)};
            break;
        case ValueKind::Expression:
            result.value = expression(id);
            break;
        case ValueKind::ActionReference:
            result.value = sct::SctInstructionReference{sct::SctInstructionId{v->data}};
            break;
        case ValueKind::IndexedTextReference:
            result.value = sct::SctStringReference{sct::SctStringId{v->data}};
            break;
        case ValueKind::SupplementaryTextReference:
            result.value = sct::SctSupplementaryTextReference{sct::SctSupplementaryTextId{v->data}};
            break;
        case ValueKind::Unresolved:
            result.value = sct::SctUnresolvedReferenceValue{
                {static_cast<sct::SctReferenceTargetStorage>(v->data),
                 v->detail ? std::optional(static_cast<sct::SctTextKind>(v->detail - 1)) : std::nullopt},
                words(id)};
            break;
        default:
            result.value = sct::SctOpaqueParameterValue{};
            break;
        }
        return result;
    }
    sct::SctDocumentInstruction instruction(const Action &action) const {
        require(action.kind == ActionKind::Instruction, "Authored constructs already own their structure");
        const auto &row = get(action.value, ValueKind::Instruction);
        sct::SctDocumentInstruction out{sct::SctInstructionId{action.id.value},
                                        static_cast<std::uint16_t>(row.data)};
        out.skipRefresh = row.detail & 1;
        auto children = p.children(action.value);
        if (row.detail & 2) {
            require(!children.empty(), "Missing scheduled expression");
            out.scheduledExpression = expression(children.front());
            children = children.subspan(1);
        }
        for (auto child : children) {
            if (p.find(child)->kind == ValueKind::Parameter)
                out.fixedParameters.push_back(parameter(child));
            else {
                get(child, ValueKind::RepeatedGroup);
                sct::SctDocumentRepeatedParameterGroup group;
                for (auto param : p.children(child))
                    group.parameters.push_back(parameter(param));
                out.repeatedParameterGroups.push_back(std::move(group));
            }
        }
        return out;
    }
};
std::span<const TargetUse> incomingUses(const ProgramData &d, SctActionId target) {
    if (auto found = d.editedIncoming.find(target); found != d.editedIncoming.end() && found->second)
        return *found->second;
    auto at = d.directory.find(target.value);
    if (at.kind != EntityKind::Action || at.index >= d.incomingRanges.size())
        return {};
    auto range = d.incomingRanges[at.index];
    return std::span(d.targetUses).subspan(range.first, range.count);
}
// A terminal true arm has no common postdominating instruction with the false
// path. Use SPICE's block/edge facts to select the small guard, not the rest of
// the section as a synthetic Else. This is only an import candidate: the result
// becomes the same editable Conditional used by ordinary If/Else authoring.
std::vector<sct::SctStructuredRegion> returnGuards(const sct::SctSectionStructure &shape,
                                                 const sct::SctScriptSectionContent &code,
                                                 const std::map<std::uint64_t, std::size_t> &position) {
    using Flow = sct::SctControlFlowKind;
    std::vector<sct::SctStructuredRegion> result;
    std::vector<std::size_t> returnPositions;
    for (const auto &block : shape.blocks)
        for (const auto &edge : block.transfers)
            if (edge.kind == Flow::Return) returnPositions.push_back(position.at(edge.sourceInstruction.value()));
    std::ranges::sort(returnPositions);
    for (const auto &header : shape.blocks) {
        if (!header.reachable || header.instructions.empty()) continue;
        const auto controller = header.instructions.back();
        const auto begin = position.at(controller.value());
        if (code.instructions[begin].scheduledExpression) continue;
        const sct::SctStructuredControlTransfer *yes = nullptr, *no = nullptr;
        for (const auto &edge : header.transfers) if (edge.sourceInstruction == controller) {
            if (edge.kind == Flow::BranchTrue) yes = &edge;
            if (edge.kind == Flow::BranchFalse) no = &edge;
        }
        if (!yes || !no || !yes->targetBlock || !no->targetBlock ||
            yes->confidence != sct::SctSemanticConfidence::Known ||
            no->confidence != sct::SctSemanticConfidence::Known) continue;
        const auto first = position.at(yes->targetBlock->entryInstruction.value());
        const auto end = position.at(no->targetBlock->entryInstruction.value());
        if (first != begin + 1 || end <= first) continue;
        const auto terminal = std::ranges::lower_bound(returnPositions, first);
        if (terminal == returnPositions.end() || *terminal >= end) continue;
        std::map<sct::SctBasicBlockId, const sct::SctStructuredBasicBlock *> body;
        std::set<sct::SctInstructionId> instructions;
        bool valid = true;
        for (const auto &block : shape.blocks) {
            auto at = position.at(block.id.entryInstruction.value());
            if (at < first || at >= end) continue;
            body.emplace(block.id, &block);
            for (auto id : block.instructions) {
                if (position.at(id.value()) >= end) valid = false;
                instructions.insert(id);
            }
        }
        if (!valid || instructions.size() != end - first) continue;
        for (const auto &entry : shape.entryPoints)
            if (body.contains(entry.block)) valid = false;
        // Interior calls/branches from outside the arm must keep their original
        // entry identity and prevent automatic ownership of the arm.
        for (const auto &block : shape.blocks) if (!body.contains(block.id))
            for (const auto &edge : block.transfers)
                if (edge.targetInstruction && instructions.contains(*edge.targetInstruction) &&
                    !(edge.sourceInstruction == controller && edge.kind == Flow::BranchTrue)) valid = false;
        if (!valid) continue;
        // Settle only blocks whose every local successor returns. Cycles, unknown
        // transfers, scheduled control flow and exits into the continuation are
        // deliberately left explicit; calls retain their normal local continuation.
        std::set<sct::SctBasicBlockId> returns;
        bool progress = true;
        while (progress) {
            progress = false;
            for (const auto &[id, block] : body) {
                if (returns.contains(id)) continue;
                bool settled = true, exits = false;
                for (const auto &edge : block->transfers) {
                    if (edge.kind == Flow::Call) continue;
                    if (edge.kind == Flow::Fallthrough && edge.targetBlock == id) continue;
                    exits = true;
                    if (edge.confidence != sct::SctSemanticConfidence::Known ||
                        code.instructions[position.at(edge.sourceInstruction.value())].scheduledExpression) {
                        settled = false; break;
                    }
                    if (edge.kind == Flow::Return) continue;
                    if ((edge.kind != Flow::Fallthrough && edge.kind != Flow::Jump &&
                         edge.kind != Flow::BranchTrue && edge.kind != Flow::BranchFalse) ||
                        !edge.targetBlock || !returns.contains(*edge.targetBlock)) settled = false;
                }
                if (settled && exits) { returns.insert(id); progress = true; }
            }
        }
        if (returns.size() != body.size() || body.empty()) continue;
        sct::SctStructuredRegion guard;
        guard.id = {shape.section, controller, sct::SctStructuredRegionKind::If};
        guard.header = header.id;
        guard.join = no->targetBlock; // False continuation, not a shared exit.
        guard.members.push_back(header.id);
        sct::SctStructuredArm arm;
        arm.kind = sct::SctStructuredArmKind::Then;
        arm.entry = yes->targetBlock;
        for (const auto &[id, block] : body) { guard.members.push_back(id); arm.blocks.push_back(id); }
        guard.arms.push_back(std::move(arm));
        result.push_back(std::move(guard));
    }
    return result;
}
} // namespace
ControlFlowIssues normalizeImportedGroups(ProgramData &d, const sct::SctDocument &document, const IdMap &ids,
                                          const std::map<std::uint64_t, SctSequenceId> &sections) {
    ControlFlowIssues result;
    std::optional<sct::SctStructuredControlFlowAnalysis> analyzed;
    try {
        analyzed = sct::SctStructuredControlFlowAnalysis::build(document);
    } catch (const std::exception &e) {
        for (const auto &section : document.sections)
            if (auto at = sections.find(section.id.value()); at != sections.end()) {
                auto issue = std::make_shared<ControlFlowDiagnostics>();
                const auto &code = std::get<sct::SctScriptSectionContent>(section.content);
                if (!code.instructions.empty())
                    addIssue(*issue, ids.at(code.instructions.front().id.value()),
                             std::string("Control-flow analysis failed; instructions remain explicit: ") +
                                 e.what());
                result.emplace(at->second, std::move(issue));
            }
        return result;
    }
    const auto &analysis = *analyzed;
    for (const auto &section : document.sections) {
        auto at = sections.find(section.id.value());
        if (at == sections.end())
            continue;
        const auto &code = std::get<sct::SctScriptSectionContent>(section.content);
        const auto *shape = analysis.findSection(section.id);
        if (!shape)
            continue;
        auto diagnostics = project(*shape, code, ids);
        std::map<sct::SctBasicBlockId, const sct::SctStructuredBasicBlock *> blocks;
        for (const auto &block : shape->blocks)
            blocks.emplace(block.id, &block);
        std::map<std::uint64_t, std::size_t> position;
        for (std::size_t i = 0; i < code.instructions.size(); ++i)
            position.emplace(code.instructions[i].id.value(), i);
        auto guards = returnGuards(*shape, code, position);
        // Children normalize before parents; physical IDs are used only during import.
        std::vector<const sct::SctStructuredRegion *> regions;
        for (const auto &region : shape->regions)
            if (!std::ranges::any_of(guards, [&](const auto &guard) {
                    return guard.id.headerInstruction == region.id.headerInstruction;
                })) regions.push_back(&region);
        for (const auto &guard : guards) regions.push_back(&guard);
        std::stable_sort(regions.begin(), regions.end(),
                         [](auto *a, auto *b) { return a->members.size() < b->members.size(); });
        auto action = [&](sct::SctInstructionId id) { return ids.at(id.value()); };
        auto edit = [&](SctActionId id) -> Action & {
            return *d.actions.find(d.directory.find(id.value).index);
        };
        auto order = [&](SctSequenceId id) -> std::vector<SctActionId> & {
            return d.pendingOrders[d.directory.find(id.value).index];
        };
        for (const auto *region : regions) {
            auto header = action(region->id.headerInstruction);
            if (d.action(header)->kind != ActionKind::Instruction)
                continue;
            bool applying = false;
            try {
                using K = sct::SctStructuredRegionKind;
                require(region->id.kind != K::NaturalLoop,
                        "This cycle has no supported structured loop condition");
                require(region->join.has_value(), "The structured continuation is not known");
                const auto &instruction =
                    code.instructions[position.at(region->id.headerInstruction.value())];
                require(!instruction.scheduledExpression,
                        "A scheduled controller remains an explicit instruction");
                require(!region->arms.empty(), "The proposed group has no identifiable bodies");
                ValueId expression;
                for (auto child : d.child(d.action(header)->value)) {
                    const auto *parameter = d.values.find(child.value);
                    if (parameter->kind == ValueKind::Parameter && parameter->data == 0 &&
                        parameter->count == 1)
                        expression = d.child(child).front();
                }
                require(expression.valid() && d.values.find(expression.value)->kind == ValueKind::Expression,
                        "The group controller has no editable expression");
                std::set<SctActionId> members;
                std::size_t begin = position.at(instruction.id.value()), end = begin;
                for (auto block : region->members)
                    for (auto id : blocks.at(block)->instructions) {
                        // Ordinary work before a branch stays outside its arms.
                        // Loop headers have separate repeated-preparation semantics.
                        if (region->id.kind != K::While && position.at(id.value()) < begin)
                            continue;
                        members.insert(action(id));
                        end = std::max(end, position.at(id.value()));
                    }
                for (auto i = begin; i <= end; ++i)
                    require(members.contains(action(code.instructions[i].id)),
                            "The proposed group does not cover a contiguous instruction range");
                require(position.at(region->join->entryInstruction.value()) == end + 1,
                        "The continuation is not immediately after the group");
                struct Body {
                    const sct::SctStructuredArm *arm;
                    std::size_t start;
                    std::vector<SctActionId> actions;
                };
                std::vector<Body> bodies;
                for (const auto &arm : region->arms) {
                    require(arm.entry.has_value(), "A proposed arm has no identifiable boundary");
                    auto start = position.at(arm.entry->entryInstruction.value());
                    bodies.push_back({&arm, start, {}});
                }
                std::stable_sort(bodies.begin(), bodies.end(),
                                 [](const auto &a, const auto &b) { return a.start < b.start; });
                require(bodies.front().start <= begin + 1, "An arm would omit the beginning of the group");
                auto owner = d.action(header)->owner;
                for (auto id : order(owner))
                    if (members.contains(id) && id != header) {
                        auto pos = position.at(d.action(id)->sourceInstruction);
                        auto selected = bodies.begin();
                        for (auto body = bodies.begin(); body != bodies.end() && body->start <= pos; ++body)
                            selected = body;
                        selected->actions.push_back(id);
                    }
                const bool loop = region->id.kind == K::While, sw = region->id.kind == K::Switch;
                require(!loop || bodies.size() == 1, "A loop requires one body");
                require(loop || sw || bodies.size() <= 2, "Unsupported conditional arm layout");
                if (sw) {
                    for (const auto &body : bodies) {
                        require(!body.arm->caseLabels.empty(), "A switch arm has no labels");
                        for (const auto &label : body.arm->caseLabels) {
                            require(label.value.has_value(), "A switch label is not known");
                        }
                    }
                }
                auto script = d.sequence(owner)->script;
                auto condition = [&](std::optional<ArmId> arm = {}) {
                    auto id = d.makeCondition(script, header, expression, arm);
                    d.conditions.back().skipRefresh = instruction.skipRefresh;
                    return id;
                };
                // There is one backing construct. No inferred body tree survives this step.
                applying = true;
                auto &root = edit(header);
                root.kind = loop ? ActionKind::Loop : sw ? ActionKind::Switch : ActionKind::Conditional;
                root.value = {};
                root.firstArm = count32(d.arms.size());
                root.armCount = loop ? 0 : count32(bodies.size());
                std::vector<SctSequenceId> destinations;
                if (loop) {
                    auto c = condition();
                    auto body = d.makeSequence(script, header, {}, SequenceRole::LoopBody);
                    root.detail = count32(d.loops.size());
                    d.loops.push_back({header, LoopKind::While, c, {}, body, {}});
                    destinations.push_back(body);
                } else {
                    if (sw) {
                        auto c = condition();
                        root.detail = count32(d.switches.size());
                        d.switches.push_back({header, c});
                    }
                    for (std::size_t n = 0; n < bodies.size(); ++n) {
                        Arm arm;
                        arm.id = ArmId{d.claim(0, EntityKind::Arm, count32(d.arms.size()))};
                        arm.owner = header;
                        arm.body = d.makeSequence(script, arm.id, {}, SequenceRole::ArmBody);
                        if (!sw && n == 0)
                            arm.condition = condition(arm.id);
                        if (sw) {
                            arm.firstLabel = count32(d.caseLabels.size());
                            for (const auto &label : bodies[n].arm->caseLabels)
                                if (label.value && *label.value != -1) {
                                    d.caseLabels.push_back(*label.value);
                                    ++arm.labelCount;
                                } else
                                    arm.includesDefault = true;
                            if (n + 1 < bodies.size())
                                for (const auto &evidence : region->evidence)
                                    if (evidence.kind == sct::SctStructureEvidenceKind::CaseFallthrough &&
                                        evidence.target &&
                                        position.at(evidence.target->value()) == bodies[n + 1].start)
                                        arm.caseExit = CaseExit::Fallthrough;
                        }
                        destinations.push_back(arm.body);
                        d.arms.push_back(arm);
                    }
                }
                for (std::size_t n = 0; n < bodies.size(); ++n) {
                    for (auto id : bodies[n].actions)
                        edit(id).owner = destinations[n];
                    order(destinations[n]) = std::move(bodies[n].actions);
                }
                std::erase_if(order(owner), [&](auto id) { return id != header && members.contains(id); });
                // Retain glue identities and refresh behavior as ordinary scoped transfers.
                // Nested break targets are already normalized and are left alone.
                auto join = action(region->join->entryInstruction);
                for (auto id : members) {
                    auto original = *d.action(id);
                    if (original.kind != ActionKind::Instruction)
                        continue;
                    const auto *value = d.values.find(original.value.value);
                    if (value->data != 10 || (value->detail & 2))
                        continue;
                    SctActionId target;
                    for (auto child : d.child(original.value)) {
                        auto *p = d.values.find(child.value);
                        if (p->kind == ValueKind::Parameter && p->data == 0 && p->count == 1) {
                            auto *v = d.values.find(d.child(child).front().value);
                            if (v->kind == ValueKind::ActionReference)
                                target = SctActionId{v->data};
                        }
                    }
                    if (target != join && !(loop && target == header))
                        continue;
                    auto &transfer = edit(id);
                    transfer.kind = ActionKind::Transfer;
                    transfer.value = {};
                    transfer.detail = count32(d.transfers.size());
                    d.transfers.push_back({id, header,
                                           target == header ? TransferKind::Continue : TransferKind::Break,
                                           bool(value->detail & 1)});
                }
                diagnostics->issues.erase(header);
            } catch (const std::exception &e) {
                if (applying) throw; // Do not accept a partially installed construct.
                addIssue(*diagnostics, header, std::string("Kept explicit: ") + e.what());
            }
        }
        result.emplace(at->second, std::move(diagnostics));
    }
    return result;
}
std::shared_ptr<const ControlFlowDiagnostics> analyzeFlowIssues(ProgramData &d, SctSequenceId sequence,
                                                                std::stop_token stop) {
    auto *seq = d.sequence(sequence);
    require(seq && seq->order, "Missing grouping sequence");
    auto fallback = std::make_shared<ControlFlowDiagnostics>();
    try {
        sct::SctDocument document;
        SourceView view{d};
        Values values(view);
        IdMap ids;
        std::map<std::uint64_t, sct::SctDocumentInstruction> outside;
        std::map<SctActionId, std::string> unavailable;
        auto converted = [&](SctActionId id) {
            auto *action = d.action(id);
            require(action, "Missing grouping action");
            ids.emplace(id.value, id);
            try {
                return values.instruction(*action);
            } catch (const std::exception &e) {
                if (action->kind == ActionKind::Instruction)
                    unavailable[id] = e.what();
                // An unknown instruction is a graph boundary, not invented executable content.
                return sct::SctDocumentInstruction{sct::SctInstructionId{id.value}, UINT16_MAX};
            }
        };
        sct::SctScriptSectionContent code;
        for (auto id : *seq->order) {
            require(!stop.stop_requested(), "Grouping cancelled");
            code.instructions.push_back(converted(id));
            for (const auto &use : incomingUses(d, id)) {
                auto *source = d.action(use.site.action);
                if (source && source->owner != sequence && !outside.contains(source->id.value)) {
                    if (source->kind == ActionKind::Instruction)
                        outside.emplace(source->id.value, converted(source->id));
                    else {
                        // A structured transfer is still an external entry into this sequence.
                        sct::SctDocumentInstruction edge{sct::SctInstructionId{source->id.value}, 10};
                        edge.fixedParameters.push_back(
                            {0, sct::SctInstructionReference{sct::SctInstructionId{id.value}}});
                        outside.emplace(source->id.value, std::move(edge));
                    }
                }
            }
        }
        // Include target identities outside the local sequence without materializing their bodies.
        auto targets = [&](const sct::SctDocumentInstruction &instruction, std::set<std::uint64_t> &found) {
            auto param = [&](const auto &p) {
                if (auto *ref = std::get_if<sct::SctInstructionReference>(&p.value))
                    found.insert(ref->target.value());
            };
            for (const auto &p : instruction.fixedParameters)
                param(p);
            for (const auto &g : instruction.repeatedParameterGroups)
                for (const auto &p : g.parameters)
                    param(p);
        };
        std::set<std::uint64_t> targetIds;
        for (const auto &instruction : code.instructions)
            targets(instruction, targetIds);
        for (const auto &[id, instruction] : outside)
            targets(instruction, targetIds);
        for (auto id : targetIds)
            if (auto *action = d.action(SctActionId{id});
                action && action->owner != sequence && !outside.contains(id))
                outside.emplace(id, sct::SctDocumentInstruction{sct::SctInstructionId{id}, 12});
        document.sections.push_back({sct::SctSectionId{1}, "sequence", std::move(code)});
        // Each external site gets its own section, avoiding invented local fallthrough.
        for (auto &[id, instruction] : outside) {
            ids.emplace(id, SctActionId{id});
            document.sections.push_back({sct::SctSectionId{document.sections.size() + 1}, "external",
                                         sct::SctScriptSectionContent{{std::move(instruction)}}});
        }
        const auto analysis = sct::SctStructuredControlFlowAnalysis::build(document);
        require(!stop.stop_requested(), "Grouping cancelled");
        auto *shape = analysis.findSection(sct::SctSectionId{1});
        require(shape, "SPICE did not return the requested section");
        auto result =
            project(*shape, std::get<sct::SctScriptSectionContent>(document.sections.front().content), ids);
        for (const auto &[id, text] : unavailable)
            addIssue(*result, id, "Grouping is incomplete: " + text);
        // Authored constructs have their own outline; unknown stub issues are not analysis failures.
        for (auto id : *seq->order)
            if (d.action(id)->kind != ActionKind::Instruction)
                result->issues.erase(id);
        return result;
    } catch (const std::exception &e) {
        if (stop.stop_requested())
            throw;
        if (!seq->order->empty())
            addIssue(*fallback, seq->order->front(),
                     std::string("Control-flow analysis failed: ") + e.what());
        return fallback;
    }
}
} // namespace salsa::core::program
