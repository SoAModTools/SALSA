#include "ProgramGrouping.h"
#include "ProgramInternal.h"
#include "ProgramTypedValues.h"
#include <algorithm>
#include <set>
#include <unordered_set>
namespace salsa::core::program {
namespace {
namespace sct = spice::sct;
using Occurrence = editor::FlowOccurrence;
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
void addIssue(FlowGrouping &out, SctActionId id, std::string text) {
    auto &message = out.issues[id];
    if (message.find(text) != std::string::npos)
        return;
    if (!message.empty())
        message += '\n';
    message += std::move(text);
}
std::shared_ptr<FlowGrouping> project(const sct::SctSectionStructure &section,
                                      const sct::SctScriptSectionContent &code, const IdMap &ids) {
    auto out = std::make_shared<FlowGrouping>();
    auto id = [&](sct::SctInstructionId native) { return ids.at(native.value()); };
    std::map<SctActionId, std::size_t> positions;
    std::vector<SctActionId> order;
    std::set<SctActionId> plainJumps;
    for (const auto &instruction : code.instructions) {
        auto action = id(instruction.id);
        positions.emplace(action, order.size());
        order.push_back(action);
        if (instruction.opcode == 10 && !instruction.skipRefresh && !instruction.scheduledExpression)
            plainJumps.insert(action);
    }
    std::map<sct::SctBasicBlockId, const sct::SctStructuredBasicBlock *> blockIndex;
    for (const auto &block : section.blocks)
        blockIndex.emplace(block.id, &block);
    auto members = [&](const std::vector<sct::SctBasicBlockId> &blocks) {
        std::vector<SctActionId> result;
        for (const auto &blockId : blocks) {
            auto block = blockIndex.find(blockId);
            require(block != blockIndex.end(), "SPICE grouping refers to a missing block");
            for (auto instruction : block->second->instructions)
                result.push_back(id(instruction));
        }
        std::ranges::sort(result, {}, [&](auto action) { return positions.at(action); });
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    };
    struct Arm {
        std::string label;
        std::vector<SctActionId> members;
    };
    struct Region {
        sct::SctStructuredRegionId native;
        std::optional<sct::SctStructuredRegionId> parent;
        SctActionId header;
        std::vector<SctActionId> members;
        std::vector<Arm> arms;
        std::set<SctActionId> hidden;
        ActionKind kind;
    };
    std::map<sct::SctStructuredRegionId, Region> regions;
    for (const auto &source : section.regions) {
        Region region{source.id, source.parent, id(source.id.headerInstruction), members(source.members)};
        region.kind = source.id.kind == sct::SctStructuredRegionKind::Switch ? ActionKind::Switch
                      : source.id.kind == sct::SctStructuredRegionKind::If ||
                              source.id.kind == sct::SctStructuredRegionKind::IfElse
                          ? ActionKind::Conditional
                          : ActionKind::Loop;
        if (source.join)
            out->joins[region.header] = id(source.join->entryInstruction);
        for (const auto &evidence : source.evidence) {
            using E = sct::SctStructureEvidenceKind;
            if (evidence.source &&
                (evidence.kind == E::PreTargetJump || evidence.kind == E::BackwardTerminatorJump ||
                 evidence.kind == E::CommonForwardExit)) {
                auto action = id(*evidence.source);
                if (plainJumps.contains(action))
                    region.hidden.insert(action);
            }
        }
        for (const auto &arm : source.arms) {
            std::string label;
            switch (arm.kind) {
            case sct::SctStructuredArmKind::Then:
                label = "If";
                break;
            case sct::SctStructuredArmKind::Else:
                label = "Else";
                break;
            case sct::SctStructuredArmKind::LoopBody:
                label = source.id.kind == sct::SctStructuredRegionKind::While ? "While" : "Loop";
                break;
            case sct::SctStructuredArmKind::SwitchCase:
                label = "Case ";
                for (const auto &value : arm.caseLabels) {
                    if (label.size() > 5)
                        label += ", ";
                    label += value.value ? std::to_string(*value.value) : "Default";
                }
                break;
            }
            auto body = members(arm.blocks);
            if (source.id.kind != sct::SctStructuredRegionKind::NaturalLoop)
                std::erase(body, region.header);
            region.arms.push_back({std::move(label), std::move(body)});
        }
        if (region.arms.empty())
            region.arms.push_back({region.kind == ActionKind::Loop ? "Loop" : "If", {}});
        // Retain every member not assigned to an arm, including unreachable code.
        // Only explicit scaffolding reported by SPICE can be hidden.
        for (auto action : region.members) {
            if (action == region.header)
                continue;
            if (std::ranges::none_of(region.arms, [&](const auto &arm) {
                    return std::ranges::find(arm.members, action) != arm.members.end();
                }))
                region.arms.front().members.push_back(action);
        }
        for (auto &arm : region.arms)
            std::ranges::sort(arm.members, {}, [&](auto action) { return positions.at(action); });
        regions.emplace(source.id, std::move(region));
    }
    std::map<std::pair<std::optional<sct::SctStructuredRegionId>, SctActionId>, const Region *> nestedIndex;
    for (const auto &[key, region] : regions)
        nestedIndex.emplace(std::make_pair(region.parent, region.header), &region);
    std::function<void(const std::vector<SctActionId> &, std::optional<sct::SctStructuredRegionId>,
                       std::optional<Occurrence>, std::vector<GroupedRow> &, unsigned)>
        scope;
    scope = [&](const auto &list, auto parent, auto occurrence, auto &rows, unsigned depth) {
        require(depth < 256, "Control-flow nesting exceeds the editor grouping depth");
        std::set<SctActionId> emitted;
        for (auto action : list) {
            if (emitted.contains(action))
                continue;
            const Region *nested = nullptr;
            if (auto found = nestedIndex.find({parent, action}); found != nestedIndex.end())
                nested = found->second;
            if (!nested) {
                if (parent && regions.at(*parent).hidden.contains(action)) {
                    if (occurrence)
                        out->locations.emplace_back(action, *occurrence);
                    continue;
                }
                rows.push_back({action});
                if (occurrence)
                    out->locations.emplace_back(action, *occurrence);
                continue;
            }
            const auto &region = *nested;
            auto groupParent = occurrence;
            const auto kind = static_cast<std::uint32_t>(region.native.kind);
            if (region.kind == ActionKind::Switch) {
                Occurrence container{region.header, 0, true, kind};
                rows.push_back({region.header, container, "Switch", region.kind});
                if (occurrence)
                    out->parents[container] = *occurrence;
                groupParent = container;
            }
            auto &groupRows = region.kind == ActionKind::Switch ? out->bodies[*groupParent] : rows;
            for (std::uint32_t n = 0; n < region.arms.size(); ++n) {
                Occurrence arm{region.header, n, false, kind};
                groupRows.push_back({region.header, arm, region.arms[n].label, region.kind});
                if (groupParent)
                    out->parents[arm] = *groupParent;
                scope(region.arms[n].members, region.native, arm, out->bodies[arm], depth + 1);
            }
            out->locations.emplace_back(region.header, region.kind == ActionKind::Switch
                                                           ? Occurrence{region.header, 0, true, kind}
                                                           : Occurrence{region.header, 0, false, kind});
            emitted.insert(region.members.begin(), region.members.end());
        }
    };
    scope(order, {}, {}, out->roots, 0);
    std::stable_sort(out->locations.begin(), out->locations.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    std::size_t kept = 0;
    for (auto item : out->locations) {
        if (kept && out->locations[kept - 1].first == item.first)
            out->locations[kept - 1] = item;
        else
            out->locations[kept++] = item;
    }
    out->locations.resize(kept);
    for (const auto &issue : section.issues) {
        auto target = issue.instruction ? id(*issue.instruction)
                      : order.empty()   ? SctActionId{}
                                        : order.front();
        if (target.valid()) {
            addIssue(*out, target, issueText(issue));
            if (auto *occurrence = out->location(target)) {
                const auto &rows = out->bodies.at(*occurrence);
                if (target != occurrence->header && std::ranges::none_of(rows, [&](const auto &row) {
                        return row.action == target && !row.occurrence;
                    }))
                    addIssue(*out, occurrence->header, issueText(issue));
            }
        }
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
} // namespace
FlowGroups importFlowGroups(const sct::SctDocument &document, const IdMap &ids,
                            const std::map<std::uint64_t, SctSequenceId> &sections) {
    FlowGroups result;
    auto fallback = [&](const sct::SctDocumentSection &section, std::string message) {
        auto group = std::make_shared<FlowGrouping>();
        for (const auto &instruction : std::get<sct::SctScriptSectionContent>(section.content).instructions)
            group->roots.push_back({ids.at(instruction.id.value())});
        if (!group->roots.empty())
            addIssue(*group, group->roots.front().action, "Control-flow analysis failed: " + message);
        return group;
    };
    try {
        const auto analysis = sct::SctStructuredControlFlowAnalysis::build(document);
        for (const auto &section : document.sections)
            if (auto at = sections.find(section.id.value()); at != sections.end()) {
                try {
                    auto *shape = analysis.findSection(section.id);
                    require(shape, "SPICE did not return the requested section");
                    result.emplace(
                        at->second,
                        project(*shape, std::get<sct::SctScriptSectionContent>(section.content), ids));
                } catch (const std::exception &e) {
                    result[at->second] = fallback(section, e.what());
                }
            }
    } catch (const std::exception &e) {
        for (const auto &section : document.sections)
            if (auto at = sections.find(section.id.value()); at != sections.end())
                result[at->second] = fallback(section, e.what());
    }
    return result;
}
std::shared_ptr<const FlowGrouping> analyzeFlowGroup(ProgramData &d, SctSequenceId sequence,
                                                     std::stop_token stop) {
    auto *seq = d.sequence(sequence);
    require(seq && seq->order, "Missing grouping sequence");
    auto fallback = std::make_shared<FlowGrouping>();
    for (auto id : *seq->order)
        fallback->roots.push_back({id});
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
