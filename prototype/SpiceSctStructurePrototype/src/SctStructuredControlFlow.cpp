#include "SpiceSctStructurePrototype/SctStructuredControlFlow.h"

#include "SpiceSCT/SctOpcodeMetadata.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <ranges>
#include <set>
#include <unordered_map>
#include <utility>

namespace salsa::spice_sct_prototype {
namespace {

using spice::sct::SctControlFlowEdge;
using spice::sct::SctControlFlowKind;
using spice::sct::SctDocument;
using spice::sct::SctDocumentAnalysis;
using spice::sct::SctDocumentInstruction;
using spice::sct::SctInstructionId;
using spice::sct::SctScriptSectionContent;
using spice::sct::SctSectionId;
using spice::sct::SctSemanticConfidence;

struct InstructionLocation final {
    SctSectionId section;
    std::size_t ordinal = 0;
    const SctDocumentInstruction* instruction = nullptr;
};

struct EffectiveEdge final {
    SctControlFlowEdge edge;
    bool importedOpaqueHint = false;
    std::optional<spice::sct::SctOpaqueAttachmentId> attachment{};
};

class BitSet final {
public:
    explicit BitSet(const std::size_t size = 0, const bool fill = false)
        : size_(size), words_((size + 63u) / 64u, fill ? ~std::uint64_t{} : 0u) {
        trim();
    }

    void set(const std::size_t value) { words_[value / 64u] |= std::uint64_t{1} << (value % 64u); }
    void reset(const std::size_t value) { words_[value / 64u] &= ~(std::uint64_t{1} << (value % 64u)); }
    [[nodiscard]] bool test(const std::size_t value) const {
        return value < size_ && (words_[value / 64u] & (std::uint64_t{1} << (value % 64u))) != 0u;
    }
    void intersect(const BitSet& other) {
        for (std::size_t i = 0; i < words_.size(); ++i) words_[i] &= other.words_[i];
    }
    [[nodiscard]] std::size_t count() const {
        return std::accumulate(words_.begin(), words_.end(), std::size_t{},
            [](const std::size_t total, const std::uint64_t value) {
                return total + static_cast<std::size_t>(std::popcount(value));
            });
    }
    [[nodiscard]] bool operator==(const BitSet&) const = default;

private:
    void trim() {
        if (!words_.empty() && size_ % 64u != 0u)
            words_.back() &= (std::uint64_t{1} << (size_ % 64u)) - 1u;
    }

    std::size_t size_ = 0;
    std::vector<std::uint64_t> words_{};
};

struct SectionGraph final {
    const SctDocument* document = nullptr;
    const spice::sct::SctDocumentSection* section = nullptr;
    const SctScriptSectionContent* script = nullptr;
    std::vector<EffectiveEdge> edges{};
    std::vector<SctBasicBlock> blocks{};
    std::unordered_map<std::uint64_t, std::size_t> instructionBlock{};
    std::unordered_map<std::uint64_t, std::size_t> instructionOrdinal{};
    std::vector<std::vector<std::size_t>> successors{};
    std::vector<std::vector<std::size_t>> predecessors{};
    std::vector<BitSet> dominators{};
    std::vector<BitSet> postDominators{};
    std::size_t syntheticExit = 0;
};

[[nodiscard]] int confidenceRank(const SctSemanticConfidence value) noexcept {
    switch (value) {
    case SctSemanticConfidence::Unknown: return 0;
    case SctSemanticConfidence::Heuristic: return 1;
    case SctSemanticConfidence::Partial: return 2;
    case SctSemanticConfidence::Known: return 3;
    }
    return 0;
}

[[nodiscard]] SctSemanticConfidence weakest(
    const SctSemanticConfidence left, const SctSemanticConfidence right) noexcept {
    return confidenceRank(left) <= confidenceRank(right) ? left : right;
}

[[nodiscard]] bool isLocalSuccessorKind(const SctControlFlowKind kind) noexcept {
    return kind != SctControlFlowKind::Call && kind != SctControlFlowKind::Return;
}

[[nodiscard]] bool isControlBoundary(const SctControlFlowKind kind) noexcept {
    return kind != SctControlFlowKind::Fallthrough;
}

[[nodiscard]] bool sameTarget(const SctControlFlowEdge& left, const SctControlFlowEdge& right) {
    return left.sourceInstruction == right.sourceInstruction && left.kind == right.kind
        && left.origin == right.origin && left.targetInstruction == right.targetInstruction;
}

[[nodiscard]] bool isOpaqueGap(
    const SctDocumentAnalysis& analysis, const SctControlFlowEdge& edge) {
    for (const auto attachment : edge.crossedOpaqueAttachments) {
        const auto* context = analysis.opaqueContext.find(attachment);
        if (context == nullptr) continue;
        if (std::ranges::any_of(context->interpretations, [](const auto& interpretation) {
                return interpretation.kind == spice::sct::SctOpaqueInterpretationKind::ControlFlowGap
                    || interpretation.kind == spice::sct::SctOpaqueInterpretationKind::SwitchDispatchGap;
            })) return true;
    }
    return false;
}

[[nodiscard]] std::vector<EffectiveEdge> effectiveEdges(
    const SctDocument& document, const SctDocumentAnalysis& analysis,
    const std::unordered_map<std::uint64_t, InstructionLocation>& locations,
    std::vector<SctStructureIssue>& globalIssues) {
    std::vector<EffectiveEdge> result;
    result.reserve(analysis.controlFlow.currentEdges().size()
        + analysis.controlFlow.importedEdges().size());
    for (const auto& edge : analysis.controlFlow.currentEdges()) result.push_back({edge});

    for (const auto& imported : analysis.controlFlow.importedEdges()) {
        if (!imported.targetInstruction || imported.crossedOpaqueAttachments.empty()
            || !isOpaqueGap(analysis, imported)) continue;
        const auto source = locations.find(imported.sourceInstruction.value());
        const auto target = locations.find(imported.targetInstruction->value());
        if (source == locations.end() || target == locations.end()) continue;
        const auto current = std::ranges::find_if(result, [&](const auto& candidate) {
            return candidate.edge.sourceInstruction == imported.sourceInstruction
                && candidate.edge.kind == imported.kind && candidate.edge.origin == imported.origin;
        });
        if (current != result.end()) {
            if (!sameTarget(current->edge, imported)) {
                globalIssues.push_back({SctStructureIssueKind::HistoricalEdgeConflict,
                    source->second.section, imported.sourceInstruction,
                    {*imported.targetInstruction}, {{SctStructureEvidenceKind::ImportedOpaqueControlFlowGap,
                        SctSemanticConfidence::Heuristic, imported.sourceInstruction,
                        imported.targetInstruction, imported.crossedOpaqueAttachments.front()}}});
            }
            continue;
        }
        result.push_back({imported, true, imported.crossedOpaqueAttachments.front()});
    }
    return result;
}

void addIssue(SctSectionStructure& output, const SctStructureIssueKind kind,
    const SctSectionId section, const std::optional<SctInstructionId> instruction,
    std::vector<SctInstructionId> related = {}, std::vector<SctStructureEvidence> evidence = {}) {
    output.issues.push_back({kind, section, instruction, std::move(related), std::move(evidence)});
}

[[nodiscard]] SectionGraph buildGraph(const SctDocument& document,
    const spice::sct::SctDocumentSection& section, const SctScriptSectionContent& script,
    const std::vector<EffectiveEdge>& allEdges,
    const std::unordered_map<std::uint64_t, InstructionLocation>& locations,
    SctSectionStructure& output,
    const std::vector<EffectiveEdge>& boundaryEdges = {}) {
    SectionGraph graph{&document, &section, &script};
    if (script.instructions.empty()) return graph;
    for (std::size_t i = 0; i < script.instructions.size(); ++i)
        graph.instructionOrdinal.emplace(script.instructions[i].id.value(), i);
    for (const auto& edge : allEdges) {
        const auto source = locations.find(edge.edge.sourceInstruction.value());
        if (source != locations.end() && source->second.section == section.id)
            graph.edges.push_back(edge);
    }

    std::vector<bool> leaders(script.instructions.size(), false);
    leaders.front() = true;
    std::vector<bool> hasOutgoing(script.instructions.size(), false);
    for (const auto& effective : graph.edges) {
        const auto source = graph.instructionOrdinal.find(effective.edge.sourceInstruction.value());
        if (source == graph.instructionOrdinal.end()) continue;
        hasOutgoing[source->second] = true;
        if (isControlBoundary(effective.edge.kind)) leaders[source->second] = true;
        if (isControlBoundary(effective.edge.kind) && source->second + 1u < leaders.size())
            leaders[source->second + 1u] = true;
        if (!effective.edge.targetInstruction) {
            if (effective.edge.kind != SctControlFlowKind::Return) {
                addIssue(output, SctStructureIssueKind::UnresolvedControlFlow, section.id,
                    effective.edge.sourceInstruction, {}, {{SctStructureEvidenceKind::CanonicalControlFlow,
                        effective.edge.confidence, effective.edge.sourceInstruction, std::nullopt, std::nullopt}});
            }
            continue;
        }
        const auto targetLocation = locations.find(effective.edge.targetInstruction->value());
        if (targetLocation == locations.end()) continue;
        if (targetLocation->second.section != section.id) {
            addIssue(output, SctStructureIssueKind::CrossSectionControlFlow, section.id,
                effective.edge.sourceInstruction, {*effective.edge.targetInstruction});
            continue;
        }
        leaders[targetLocation->second.ordinal] = true;
    }
    // Imported hints are evaluated one at a time, but every hinted endpoint is
    // still a stable block boundary so all per-hint graphs use compatible IDs.
    for (const auto& effective : boundaryEdges) {
        const auto sourceLocation = locations.find(
            effective.edge.sourceInstruction.value());
        if (sourceLocation == locations.end()
            || sourceLocation->second.section != section.id
            || !isControlBoundary(effective.edge.kind)) continue;
        const auto source = graph.instructionOrdinal.find(
            effective.edge.sourceInstruction.value());
        if (source == graph.instructionOrdinal.end()) continue;
        leaders[source->second] = true;
        if (source->second + 1u < leaders.size()) leaders[source->second + 1u] = true;
        if (!effective.edge.targetInstruction) continue;
        const auto targetLocation = locations.find(
            effective.edge.targetInstruction->value());
        if (targetLocation != locations.end()
            && targetLocation->second.section == section.id) {
            leaders[targetLocation->second.ordinal] = true;
        }
    }
    for (std::size_t i = 0; i + 1u < script.instructions.size(); ++i) {
        if (!hasOutgoing[i]) {
            leaders[i + 1u] = true;
            addIssue(output, SctStructureIssueKind::MissingControlFlow, section.id,
                script.instructions[i].id);
        }
    }

    std::size_t begin = 0;
    while (begin < script.instructions.size()) {
        std::size_t end = begin + 1u;
        while (end < script.instructions.size() && !leaders[end]) ++end;
        SctBasicBlock block;
        block.id = {section.id, script.instructions[begin].id};
        for (std::size_t i = begin; i < end; ++i) {
            block.instructions.push_back(script.instructions[i].id);
            graph.instructionBlock.emplace(script.instructions[i].id.value(), graph.blocks.size());
        }
        graph.blocks.push_back(std::move(block));
        begin = end;
    }

    graph.successors.resize(graph.blocks.size());
    graph.predecessors.resize(graph.blocks.size());
    for (const auto& effective : graph.edges) {
        const auto source = graph.instructionBlock.find(effective.edge.sourceInstruction.value());
        if (source == graph.instructionBlock.end()) continue;
        std::optional<SctBasicBlockId> targetBlock;
        if (effective.edge.targetInstruction) {
            const auto target = graph.instructionBlock.find(effective.edge.targetInstruction->value());
            if (target != graph.instructionBlock.end()) targetBlock = graph.blocks[target->second].id;
        }
        graph.blocks[source->second].successors.push_back({effective.edge.sourceInstruction,
            effective.edge.kind, effective.edge.confidence, effective.edge.origin, targetBlock,
            effective.edge.crossedOpaqueAttachments,
            effective.importedOpaqueHint});
        if (!targetBlock || !isLocalSuccessorKind(effective.edge.kind)) continue;
        const auto target = graph.instructionBlock.find(targetBlock->entryInstruction.value());
        if (target == graph.instructionBlock.end()) continue;
        if (std::ranges::find(graph.successors[source->second], target->second)
            == graph.successors[source->second].end()) {
            graph.successors[source->second].push_back(target->second);
            graph.predecessors[target->second].push_back(source->second);
        }
    }

    std::queue<std::size_t> pending;
    pending.push(0u);
    graph.blocks[0].reachable = true;
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        for (const auto next : graph.successors[current]) {
            if (graph.blocks[next].reachable) continue;
            graph.blocks[next].reachable = true;
            pending.push(next);
        }
    }

    const auto blockCount = graph.blocks.size();
    graph.syntheticExit = blockCount;
    graph.dominators.assign(blockCount, BitSet(blockCount, true));
    for (std::size_t i = 0; i < blockCount; ++i) {
        if (!graph.blocks[i].reachable) {
            graph.dominators[i] = BitSet(blockCount);
            graph.dominators[i].set(i);
        }
    }
    graph.dominators[0] = BitSet(blockCount);
    graph.dominators[0].set(0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < blockCount; ++i) {
            if (!graph.blocks[i].reachable) continue;
            BitSet next(blockCount, true);
            bool found = false;
            for (const auto predecessor : graph.predecessors[i]) {
                if (!graph.blocks[predecessor].reachable) continue;
                if (!found) next = graph.dominators[predecessor];
                else next.intersect(graph.dominators[predecessor]);
                found = true;
            }
            if (!found) next = BitSet(blockCount);
            next.set(i);
            if (!(next == graph.dominators[i])) {
                graph.dominators[i] = std::move(next);
                changed = true;
            }
        }
    }

    const auto postSize = blockCount + 1u;
    graph.postDominators.assign(postSize, BitSet(postSize, true));
    graph.postDominators[graph.syntheticExit] = BitSet(postSize);
    graph.postDominators[graph.syntheticExit].set(graph.syntheticExit);
    for (std::size_t i = 0; i < blockCount; ++i) {
        if (!graph.blocks[i].reachable) {
            graph.postDominators[i] = BitSet(postSize);
            graph.postDominators[i].set(i);
            graph.postDominators[i].set(graph.syntheticExit);
        }
    }
    changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < blockCount; ++i) {
            if (!graph.blocks[i].reachable) continue;
            const auto& successors = graph.successors[i];
            BitSet next = successors.empty()
                ? graph.postDominators[graph.syntheticExit]
                : graph.postDominators[successors.front()];
            for (std::size_t j = 1; j < successors.size(); ++j)
                next.intersect(graph.postDominators[successors[j]]);
            next.set(i);
            if (!(next == graph.postDominators[i])) {
                graph.postDominators[i] = std::move(next);
                changed = true;
            }
        }
    }
    return graph;
}

[[nodiscard]] bool dominates(const SectionGraph& graph,
    const std::size_t dominator, const std::size_t block) {
    return block < graph.dominators.size() && graph.dominators[block].test(dominator);
}

[[nodiscard]] std::optional<std::size_t> immediatePostDominator(
    const SectionGraph& graph, const std::size_t block) {
    if (block >= graph.postDominators.size()) return std::nullopt;
    std::optional<std::size_t> result;
    std::size_t bestCount = 0;
    for (std::size_t candidate = 0; candidate < graph.postDominators.size(); ++candidate) {
        if (candidate == block || !graph.postDominators[block].test(candidate)) continue;
        const auto count = graph.postDominators[candidate].count();
        if (!result || count > bestCount) {
            result = candidate;
            bestCount = count;
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t> nearestPostDominatorOutside(
    const SectionGraph& graph, const std::size_t block,
    const std::set<std::size_t>& excluded) {
    auto candidate = immediatePostDominator(graph, block);
    std::set<std::size_t> visited;
    while (candidate && *candidate != graph.syntheticExit
        && excluded.contains(*candidate)) {
        if (!visited.insert(*candidate).second) return std::nullopt;
        candidate = immediatePostDominator(graph, *candidate);
    }
    return candidate;
}

[[nodiscard]] std::set<std::size_t> reachableUntil(
    const SectionGraph& graph, const std::size_t start,
    const std::optional<std::size_t> stop) {
    std::set<std::size_t> result;
    if (stop && start == *stop) return result;
    std::queue<std::size_t> pending;
    pending.push(start);
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        if (stop && current == *stop) continue;
        if (!result.insert(current).second) continue;
        for (const auto next : graph.successors[current]) pending.push(next);
    }
    return result;
}

[[nodiscard]] std::vector<SctBasicBlockId> blockIds(
    const SectionGraph& graph, const std::set<std::size_t>& indexes) {
    std::vector<SctBasicBlockId> result;
    for (const auto index : indexes) result.push_back(graph.blocks[index].id);
    return result;
}

[[nodiscard]] bool contiguous(const std::set<std::size_t>& indexes) {
    return indexes.empty() || (*indexes.rbegin() - *indexes.begin() + 1u) == indexes.size();
}

[[nodiscard]] bool hasDefensibleBoundaries(const SectionGraph& graph,
    const std::set<std::size_t>& members, const std::size_t header,
    const std::optional<std::size_t> join) {
    for (const auto block : members) {
        if (block != header) {
            for (const auto predecessor : graph.predecessors[block]) {
                if (!members.contains(predecessor)) return false;
            }
        }
        for (const auto& edge : graph.blocks[block].successors) {
            if (edge.kind == SctControlFlowKind::Return) continue;
            if (!edge.target) return false;
            const auto target = graph.instructionBlock.find(edge.target->entryInstruction.value());
            if (target == graph.instructionBlock.end()) return false;
            if (members.contains(target->second) || (join && target->second == *join)) continue;
            return false;
        }
    }
    return true;
}

[[nodiscard]] SctSemanticConfidence regionConfidence(
    const SectionGraph& graph, const std::set<std::size_t>& members,
    bool& imported) {
    auto confidence = SctSemanticConfidence::Known;
    bool found = false;
    for (const auto block : members) {
        for (const auto& edge : graph.blocks[block].successors) {
            confidence = weakest(confidence, edge.confidence);
            imported = imported || edge.importedOpaqueHint;
            found = true;
        }
    }
    if (!found) confidence = SctSemanticConfidence::Unknown;
    if (imported) confidence = SctSemanticConfidence::Heuristic;
    return confidence;
}

[[nodiscard]] std::vector<SctStructureEvidence> canonicalEvidence(
    const SectionGraph& graph, const std::set<std::size_t>& members) {
    std::vector<SctStructureEvidence> result;
    for (const auto block : members) {
        for (const auto& edge : graph.blocks[block].successors) {
            result.push_back({edge.importedOpaqueHint
                    ? SctStructureEvidenceKind::ImportedOpaqueControlFlowGap
                    : SctStructureEvidenceKind::CanonicalControlFlow,
                edge.importedOpaqueHint ? SctSemanticConfidence::Heuristic : edge.confidence,
                edge.sourceInstruction,
                edge.target ? std::optional{edge.target->entryInstruction} : std::nullopt,
                edge.crossedOpaqueAttachments.empty()
                    ? std::nullopt : std::optional{edge.crossedOpaqueAttachments.front()}});
        }
    }
    return result;
}

[[nodiscard]] const SctDocumentInstruction* instruction(
    const SectionGraph& graph, const SctInstructionId id) {
    const auto found = graph.instructionOrdinal.find(id.value());
    return found == graph.instructionOrdinal.end()
        ? nullptr : &graph.script->instructions[found->second];
}

[[nodiscard]] std::optional<std::size_t> targetBlock(
    const SectionGraph& graph, const SctBasicBlockSuccessor& edge) {
    if (!edge.target) return std::nullopt;
    const auto found = graph.instructionBlock.find(edge.target->entryInstruction.value());
    return found == graph.instructionBlock.end() ? std::nullopt : std::optional{found->second};
}

[[nodiscard]] std::vector<const SctBasicBlockSuccessor*> edgesOf(
    const SectionGraph& graph, const std::size_t block, const SctControlFlowKind kind) {
    std::vector<const SctBasicBlockSuccessor*> result;
    for (const auto& edge : graph.blocks[block].successors)
        if (edge.kind == kind) result.push_back(&edge);
    return result;
}

void detectLoops(const SectionGraph& graph, SctSectionStructure& output) {
    // Characterize irreducible SCCs before looking for natural back edges. A
    // cyclic component with multiple graph entry nodes cannot be represented
    // as a single-entry structured loop and must stay flat.
    std::vector<int> discovery(graph.blocks.size(), -1);
    std::vector<int> lowLink(graph.blocks.size(), -1);
    std::vector<std::size_t> stack;
    std::vector<bool> onStack(graph.blocks.size(), false);
    int nextDiscovery = 0;
    std::function<void(std::size_t)> visit = [&](const std::size_t node) {
        discovery[node] = lowLink[node] = nextDiscovery++;
        stack.push_back(node);
        onStack[node] = true;
        for (const auto successor : graph.successors[node]) {
            if (!graph.blocks[successor].reachable) continue;
            if (discovery[successor] < 0) {
                visit(successor);
                lowLink[node] = std::min(lowLink[node], lowLink[successor]);
            } else if (onStack[successor]) {
                lowLink[node] = std::min(lowLink[node], discovery[successor]);
            }
        }
        if (lowLink[node] != discovery[node]) return;
        std::set<std::size_t> component;
        while (!stack.empty()) {
            const auto member = stack.back();
            stack.pop_back();
            onStack[member] = false;
            component.insert(member);
            if (member == node) break;
        }
        const bool selfCycle = component.size() == 1u
            && std::ranges::find(graph.successors[*component.begin()], *component.begin())
                != graph.successors[*component.begin()].end();
        if (component.size() < 2u && !selfCycle) return;
        std::set<std::size_t> entries;
        for (const auto member : component) {
            if (member == 0u || std::ranges::any_of(graph.predecessors[member],
                    [&](const auto predecessor) { return !component.contains(predecessor); })) {
                entries.insert(member);
            }
        }
        if (entries.size() > 1u) {
            addIssue(output, SctStructureIssueKind::IrreducibleCycle,
                graph.section->id, graph.blocks[*entries.begin()].id.entryInstruction,
                [&] {
                    std::vector<SctInstructionId> related;
                    for (const auto entry : entries)
                        related.push_back(graph.blocks[entry].id.entryInstruction);
                    return related;
                }());
        }
    };
    for (std::size_t node = 0; node < graph.blocks.size(); ++node)
        if (graph.blocks[node].reachable && discovery[node] < 0) visit(node);

    std::map<std::size_t, std::set<std::size_t>> loops;
    for (std::size_t source = 0; source < graph.blocks.size(); ++source) {
        if (!graph.blocks[source].reachable) continue;
        for (const auto target : graph.successors[source]) {
            if (!dominates(graph, target, source)) continue;
            auto& members = loops[target];
            members.insert(target);
            members.insert(source);
            std::vector<std::size_t> pending{source};
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                for (const auto predecessor : graph.predecessors[current]) {
                    if (members.insert(predecessor).second && predecessor != target)
                        pending.push_back(predecessor);
                }
            }
        }
    }

    for (const auto& [header, members] : loops) {
        if (!contiguous(members)) {
            addIssue(output, SctStructureIssueKind::IrreducibleCycle, graph.section->id,
                graph.blocks[header].id.entryInstruction);
            continue;
        }
        bool singleEntry = true;
        for (const auto member : members) {
            if (member == header) continue;
            if (std::ranges::any_of(graph.predecessors[member], [&](const auto predecessor) {
                    return !members.contains(predecessor);
                })) {
                singleEntry = false;
                break;
            }
        }
        if (!singleEntry) {
            addIssue(output, SctStructureIssueKind::MultipleEntryRegion, graph.section->id,
                graph.blocks[header].id.entryInstruction);
            continue;
        }
        const auto controller = graph.blocks[header].instructions.back();
        const auto trueEdges = edgesOf(graph, header, SctControlFlowKind::BranchTrue);
        const auto falseEdges = edgesOf(graph, header, SctControlFlowKind::BranchFalse);
        const bool whileShape = trueEdges.size() == 1u && falseEdges.size() == 1u
            && targetBlock(graph, *trueEdges.front()).has_value()
            && targetBlock(graph, *falseEdges.front()).has_value()
            && (members.contains(*targetBlock(graph, *trueEdges.front()))
                != members.contains(*targetBlock(graph, *falseEdges.front())));
        std::set<std::size_t> body = members;
        body.erase(header);
        std::optional<std::size_t> exit;
        if (whileShape) {
            const auto trueTarget = *targetBlock(graph, *trueEdges.front());
            const auto falseTarget = *targetBlock(graph, *falseEdges.front());
            exit = members.contains(trueTarget) ? falseTarget : trueTarget;
        }
        bool imported = false;
        auto evidence = canonicalEvidence(graph, members);
        for (const auto member : members) {
            for (const auto& edge : graph.blocks[member].successors) {
                const auto target = targetBlock(graph, edge);
                if (edge.kind == SctControlFlowKind::Jump && target
                    && *target == header && member >= header) {
                    evidence.push_back({SctStructureEvidenceKind::BackwardTerminatorJump,
                        edge.importedOpaqueHint ? SctSemanticConfidence::Heuristic : edge.confidence,
                        edge.sourceInstruction, graph.blocks[header].id.entryInstruction,
                        edge.crossedOpaqueAttachments.empty() ? std::nullopt
                            : std::optional{edge.crossedOpaqueAttachments.front()}});
                }
            }
        }
        SctStructuredRegion region;
        region.id = {graph.section->id, controller,
            whileShape ? SctStructuredRegionKind::While : SctStructuredRegionKind::NaturalLoop};
        region.header = graph.blocks[header].id;
        if (exit) region.join = graph.blocks[*exit].id;
        region.members = blockIds(graph, members);
        region.arms.push_back({SctStructuredArmKind::LoopBody,
            body.empty() ? std::nullopt : std::optional{graph.blocks[*body.begin()].id},
            blockIds(graph, body), {}});
        region.confidence = regionConfidence(graph, members, imported);
        region.strength = imported ? SctStructureClaimStrength::EvidenceLimited
                                   : SctStructureClaimStrength::Verified;
        region.evidence = std::move(evidence);
        output.regions.push_back(std::move(region));
    }
}

[[nodiscard]] bool isLoopHeader(const SctSectionStructure& output, const SctBasicBlockId& block) {
    return std::ranges::any_of(output.regions, [&](const auto& region) {
        return region.header == block && (region.id.kind == SctStructuredRegionKind::While
            || region.id.kind == SctStructuredRegionKind::NaturalLoop);
    });
}

void detectConditionals(const SectionGraph& graph, SctSectionStructure& output) {
    for (std::size_t header = 0; header < graph.blocks.size(); ++header) {
        if (!graph.blocks[header].reachable) continue;
        const auto trueEdges = edgesOf(graph, header, SctControlFlowKind::BranchTrue);
        const auto falseEdges = edgesOf(graph, header, SctControlFlowKind::BranchFalse);
        if (trueEdges.size() != 1u || falseEdges.size() != 1u
            || isLoopHeader(output, graph.blocks[header].id)) continue;
        const auto trueTarget = targetBlock(graph, *trueEdges.front());
        const auto falseTarget = targetBlock(graph, *falseEdges.front());
        if (!trueTarget || !falseTarget) continue;
        const auto post = immediatePostDominator(graph, header);
        if (!post) {
            addIssue(output, SctStructureIssueKind::AmbiguousJoin, graph.section->id,
                graph.blocks[header].instructions.back());
            continue;
        }
        const auto join = *post == graph.syntheticExit ? std::nullopt : std::optional{*post};
        auto thenBlocks = reachableUntil(graph, *trueTarget, join);
        auto elseBlocks = reachableUntil(graph, *falseTarget, join);
        std::vector<std::size_t> overlap;
        std::ranges::set_intersection(thenBlocks, elseBlocks, std::back_inserter(overlap));
        if (!overlap.empty()) {
            addIssue(output, SctStructureIssueKind::RejectedLegacyCandidate,
                graph.section->id, graph.blocks[header].instructions.back());
            continue;
        }
        std::set<std::size_t> members{header};
        members.insert(thenBlocks.begin(), thenBlocks.end());
        members.insert(elseBlocks.begin(), elseBlocks.end());
        if (!contiguous(members) || !hasDefensibleBoundaries(graph, members, header, join)) {
            addIssue(output, SctStructureIssueKind::RejectedLegacyCandidate,
                graph.section->id, graph.blocks[header].instructions.back());
            continue;
        }

        auto evidence = canonicalEvidence(graph, members);
        evidence.push_back({SctStructureEvidenceKind::ConditionalFalseTarget,
            falseEdges.front()->confidence, falseEdges.front()->sourceInstruction,
            falseEdges.front()->target ? std::optional{falseEdges.front()->target->entryInstruction}
                                       : std::nullopt, std::nullopt});
        const auto falseOrdinal = graph.instructionOrdinal.find(
            graph.blocks[*falseTarget].id.entryInstruction.value());
        if (falseOrdinal != graph.instructionOrdinal.end() && falseOrdinal->second > 0u) {
            const auto& prior = graph.script->instructions[falseOrdinal->second - 1u];
            if (prior.opcode == 10u) {
                evidence.push_back({SctStructureEvidenceKind::PreTargetJump,
                    SctSemanticConfidence::Known, prior.id, std::nullopt, std::nullopt});
            }
        }
        bool imported = false;
        SctStructuredRegion region;
        const bool hasElse = !elseBlocks.empty() && (!join || *falseTarget != *join);
        const auto controller = graph.blocks[header].instructions.back();
        region.id = {graph.section->id, controller,
            hasElse ? SctStructuredRegionKind::IfElse : SctStructuredRegionKind::If};
        region.header = graph.blocks[header].id;
        if (join) region.join = graph.blocks[*join].id;
        region.members = blockIds(graph, members);
        region.arms.push_back({SctStructuredArmKind::Then,
            thenBlocks.empty() ? std::nullopt : std::optional{graph.blocks[*thenBlocks.begin()].id},
            blockIds(graph, thenBlocks), {}});
        if (hasElse) {
            region.arms.push_back({SctStructuredArmKind::Else,
                elseBlocks.empty() ? std::nullopt : std::optional{graph.blocks[*elseBlocks.begin()].id},
                blockIds(graph, elseBlocks), {}});
        }
        region.confidence = regionConfidence(graph, members, imported);
        region.strength = imported ? SctStructureClaimStrength::EvidenceLimited
                                   : SctStructureClaimStrength::Verified;
        region.evidence = std::move(evidence);
        output.regions.push_back(std::move(region));
    }
}

[[nodiscard]] std::optional<std::int32_t> caseValue(
    const SctDocumentInstruction& instruction, const std::uint32_t group) {
    if (group >= instruction.repeatedParameterGroups.size()) return std::nullopt;
    const auto& parameters = instruction.repeatedParameterGroups[group].parameters;
    const auto found = std::ranges::find(parameters, 2u,
        &spice::sct::SctDocumentParameter::schemaIndex);
    if (found == parameters.end()) return std::nullopt;
    const auto* encoded = std::get_if<spice::sct::SctEncodedWordValue>(&found->value);
    return encoded == nullptr ? std::nullopt
        : std::optional{static_cast<std::int32_t>(encoded->value)};
}

void detectSwitches(const SectionGraph& graph, SctSectionStructure& output) {
    for (std::size_t header = 0; header < graph.blocks.size(); ++header) {
        if (!graph.blocks[header].reachable) continue;
        const auto caseEdges = edgesOf(graph, header, SctControlFlowKind::SwitchCase);
        if (caseEdges.empty()) continue;
        std::map<std::size_t, std::vector<const SctBasicBlockSuccessor*>> byTarget;
        bool incomplete = false;
        for (const auto* edge : caseEdges) {
            const auto target = targetBlock(graph, *edge);
            if (!target) incomplete = true;
            else byTarget[*target].push_back(edge);
        }
        if (incomplete || byTarget.empty()) {
            addIssue(output, SctStructureIssueKind::AmbiguousSwitchCases,
                graph.section->id, graph.blocks[header].instructions.back());
            continue;
        }
        std::set<std::size_t> caseEntries;
        for (const auto& [entry, edges] : byTarget) caseEntries.insert(entry);
        // A later case can postdominate an earlier fallthrough case without
        // being the switch join. Walk outward until the candidate is no longer
        // itself a case entry.
        const auto post = nearestPostDominatorOutside(graph, header, caseEntries);
        const auto join = post && *post != graph.syntheticExit
            ? std::optional{*post} : std::nullopt;
        std::set<std::size_t> members{header};
        std::vector<SctStructuredArm> arms;
        std::vector<SctStructureEvidence> evidence;
        bool valid = true;
        for (auto current = byTarget.begin(); current != byTarget.end(); ++current) {
            const auto next = std::next(current);
            const auto end = next != byTarget.end() ? next->first
                : join.value_or(graph.blocks.size());
            if (current->first >= end) {
                valid = false;
                break;
            }
            std::set<std::size_t> armBlocks;
            for (auto block = current->first; block < end; ++block) {
                if (!dominates(graph, header, block)
                    || !dominates(graph, current->first, block)) {
                    valid = false;
                    break;
                }
                armBlocks.insert(block);
                members.insert(block);
            }
            if (!valid) break;
            SctStructuredArm arm;
            arm.kind = SctStructuredArmKind::SwitchCase;
            arm.entry = graph.blocks[current->first].id;
            arm.blocks = blockIds(graph, armBlocks);
            for (const auto* edge : current->second) {
                const auto ordinal = edge->origin && edge->origin->parameter.repeatedGroupOrdinal
                    ? *edge->origin->parameter.repeatedGroupOrdinal : 0u;
                const auto* selector = instruction(graph, edge->sourceInstruction);
                arm.caseLabels.push_back({ordinal,
                    selector == nullptr ? std::nullopt : caseValue(*selector, ordinal),
                    spice::sct::SctParameterSite{edge->sourceInstruction, {2u, ordinal}}});
                evidence.push_back({current->second.size() > 1u
                        ? SctStructureEvidenceKind::SharedCaseTarget
                        : SctStructureEvidenceKind::PhysicalCaseBoundary,
                    edge->confidence, edge->sourceInstruction,
                    edge->target ? std::optional{edge->target->entryInstruction} : std::nullopt,
                    std::nullopt});
            }
            if (next != byTarget.end()) {
                const auto last = *armBlocks.rbegin();
                const bool fallsThrough = std::ranges::any_of(graph.blocks[last].successors,
                    [&](const auto& edge) {
                        return edge.kind == SctControlFlowKind::Fallthrough
                            && targetBlock(graph, edge) == std::optional{next->first};
                    });
                if (fallsThrough) {
                    evidence.push_back({SctStructureEvidenceKind::CaseFallthrough,
                        SctSemanticConfidence::Known,
                        graph.blocks[last].instructions.back(),
                        graph.blocks[next->first].id.entryInstruction, std::nullopt});
                }
            }
            arms.push_back(std::move(arm));
        }
        // Cross-arm flow is only structured switch fallthrough when both the
        // physical boundary and the canonical graph agree. An explicit jump
        // into another case is retained as flat code instead of being
        // over-grouped by the legacy physical-range heuristic.
        if (valid) {
            std::map<std::size_t, std::size_t> armForBlock;
            for (std::size_t armIndex = 0; armIndex < arms.size(); ++armIndex) {
                for (const auto& block : arms[armIndex].blocks) {
                    armForBlock.emplace(
                        graph.instructionBlock.at(block.entryInstruction.value()), armIndex);
                }
            }
            for (std::size_t armIndex = 0; armIndex < arms.size() && valid; ++armIndex) {
                const auto last = graph.instructionBlock.at(
                    arms[armIndex].blocks.back().entryInstruction.value());
                for (const auto& block : arms[armIndex].blocks) {
                    const auto blockIndex = graph.instructionBlock.at(
                        block.entryInstruction.value());
                    for (const auto& edge : graph.blocks[blockIndex].successors) {
                        const auto target = targetBlock(graph, edge);
                        if (!target || !armForBlock.contains(*target)
                            || armForBlock.at(*target) == armIndex) continue;
                        const bool nextCaseFallthrough = armIndex + 1u < arms.size()
                            && armForBlock.at(*target) == armIndex + 1u
                            && blockIndex == last
                            && arms[armIndex + 1u].entry
                            && *target == graph.instructionBlock.at(
                                arms[armIndex + 1u].entry->entryInstruction.value())
                            && edge.kind == SctControlFlowKind::Fallthrough;
                        if (!nextCaseFallthrough) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) break;
                }
            }
        }
        if (!valid || !contiguous(members)
            || !hasDefensibleBoundaries(graph, members, header, join)) {
            addIssue(output, SctStructureIssueKind::AmbiguousSwitchCases,
                graph.section->id, graph.blocks[header].instructions.back(), {}, std::move(evidence));
            continue;
        }
        for (const auto& arm : arms) {
            if (arm.blocks.empty()) continue;
            const auto last = graph.instructionBlock.at(arm.blocks.back().entryInstruction.value());
            for (const auto& edge : graph.blocks[last].successors) {
                if (edge.kind == SctControlFlowKind::Jump && join
                    && targetBlock(graph, edge) == join) {
                    evidence.push_back({SctStructureEvidenceKind::CommonForwardExit,
                        edge.confidence, edge.sourceInstruction,
                        graph.blocks[*join].id.entryInstruction, std::nullopt});
                }
            }
        }
        auto canonical = canonicalEvidence(graph, members);
        evidence.insert(evidence.end(), canonical.begin(), canonical.end());
        bool imported = false;
        SctStructuredRegion region;
        const auto controller = graph.blocks[header].instructions.back();
        region.id = {graph.section->id, controller, SctStructuredRegionKind::Switch};
        region.header = graph.blocks[header].id;
        if (join) region.join = graph.blocks[*join].id;
        region.members = blockIds(graph, members);
        region.arms = std::move(arms);
        region.confidence = regionConfidence(graph, members, imported);
        region.strength = imported ? SctStructureClaimStrength::EvidenceLimited
                                   : SctStructureClaimStrength::Verified;
        region.evidence = std::move(evidence);
        output.regions.push_back(std::move(region));
    }
}

[[nodiscard]] std::set<SctBasicBlockId> memberSet(const SctStructuredRegion& region) {
    return {region.members.begin(), region.members.end()};
}

void resolveNesting(SctSectionStructure& output) {
    std::vector<bool> rejected(output.regions.size(), false);
    std::vector<std::set<SctBasicBlockId>> members;
    members.reserve(output.regions.size());
    for (const auto& region : output.regions) members.push_back(memberSet(region));
    for (std::size_t left = 0; left < output.regions.size(); ++left) {
        for (std::size_t right = left + 1u; right < output.regions.size(); ++right) {
            std::vector<SctBasicBlockId> overlap;
            std::ranges::set_intersection(members[left], members[right], std::back_inserter(overlap));
            if (overlap.empty()) continue;
            const bool leftContains = std::ranges::includes(members[left], members[right]);
            const bool rightContains = std::ranges::includes(members[right], members[left]);
            if (!leftContains && !rightContains) {
                rejected[left] = rejected[right] = true;
                addIssue(output, SctStructureIssueKind::OverlappingRegions, output.section,
                    output.regions[left].id.headerInstruction,
                    {output.regions[right].id.headerInstruction});
            }
        }
    }
    std::vector<SctStructuredRegion> retained;
    for (std::size_t i = 0; i < output.regions.size(); ++i)
        if (!rejected[i]) retained.push_back(std::move(output.regions[i]));
    output.regions = std::move(retained);

    for (auto& child : output.regions) {
        const auto childMembers = memberSet(child);
        const SctStructuredRegion* parent = nullptr;
        std::size_t parentSize = std::numeric_limits<std::size_t>::max();
        for (const auto& candidate : output.regions) {
            if (candidate.id == child.id || candidate.members.size() <= child.members.size()) continue;
            const auto candidateMembers = memberSet(candidate);
            if (std::ranges::includes(candidateMembers, childMembers)
                && candidate.members.size() < parentSize) {
                parent = &candidate;
                parentSize = candidate.members.size();
            }
        }
        if (parent != nullptr) child.parent = parent->id;
    }
    std::ranges::sort(output.regions, [](const auto& left, const auto& right) {
        if (left.header.entryInstruction != right.header.entryInstruction)
            return left.header.entryInstruction < right.header.entryInstruction;
        return left.members.size() > right.members.size();
    });
}

[[nodiscard]] std::vector<std::size_t> indexesFor(
    const SectionGraph& graph, const std::vector<SctBasicBlockId>& blocks) {
    std::vector<std::size_t> result;
    for (const auto& block : blocks) {
        const auto found = graph.instructionBlock.find(block.entryInstruction.value());
        if (found != graph.instructionBlock.end()) result.push_back(found->second);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] SctStructuredOutlineItem blockItem(
    const SctBasicBlock& block) {
    SctStructuredOutlineItem item;
    item.kind = SctStructuredOutlineItemKind::BasicBlock;
    item.block = block.id;
    for (const auto instruction : block.instructions) {
        SctStructuredOutlineItem child;
        child.kind = SctStructuredOutlineItemKind::Instruction;
        child.instruction = instruction;
        child.block = block.id;
        item.children.push_back(std::move(child));
    }
    return item;
}

void buildOutline(const SectionGraph& graph, SctSectionStructure& output) {
    std::function<std::vector<SctStructuredOutlineItem>(
        const std::vector<std::size_t>&, const std::optional<SctStructuredRegionId>&)> render;
    render = [&](const std::vector<std::size_t>& allowed,
        const std::optional<SctStructuredRegionId>& parent) {
        std::vector<SctStructuredOutlineItem> result;
        std::set<std::size_t> allowedSet(allowed.begin(), allowed.end());
        for (std::size_t cursor = 0; cursor < allowed.size();) {
            const auto blockIndex = allowed[cursor];
            const auto region = std::ranges::find_if(output.regions, [&](const auto& candidate) {
                if (candidate.parent != parent) return false;
                const auto indexes = indexesFor(graph, candidate.members);
                return !indexes.empty() && indexes.front() == blockIndex
                    && std::ranges::all_of(indexes, [&](const auto value) { return allowedSet.contains(value); });
            });
            if (region == output.regions.end()) {
                result.push_back(blockItem(graph.blocks[blockIndex]));
                ++cursor;
                continue;
            }
            SctStructuredOutlineItem regionItem;
            regionItem.kind = SctStructuredOutlineItemKind::Region;
            regionItem.instruction = region->id.headerInstruction;
            regionItem.block = region->header;
            regionItem.region = region->id;
            regionItem.strength = region->strength;
            regionItem.confidence = region->confidence;
            regionItem.evidence = region->evidence;
            for (const auto& arm : region->arms) {
                SctStructuredOutlineItem armItem;
                armItem.kind = SctStructuredOutlineItemKind::Arm;
                armItem.arm = arm.kind;
                armItem.caseLabels = arm.caseLabels;
                const auto armIndexes = indexesFor(graph, arm.blocks);
                armItem.children = render(armIndexes, region->id);
                regionItem.children.push_back(std::move(armItem));
            }
            result.push_back(std::move(regionItem));
            const auto regionIndexes = indexesFor(graph, region->members);
            const auto last = regionIndexes.empty() ? blockIndex : regionIndexes.back();
            while (cursor < allowed.size() && allowed[cursor] <= last) ++cursor;
        }
        return result;
    };

    std::vector<std::size_t> all(graph.blocks.size());
    std::iota(all.begin(), all.end(), 0u);
    output.outline = render(all, std::nullopt);
    for (const auto& issue : output.issues) {
        SctStructuredOutlineItem item;
        item.kind = SctStructuredOutlineItemKind::Issue;
        item.instruction = issue.instruction;
        item.issue = issue.kind;
        item.evidence = issue.evidence;
        output.outline.push_back(std::move(item));
    }
}

}  // namespace

SctStructuredControlFlowAnalysis SctStructuredControlFlowAnalysis::build(
    const SctDocument& document, const SctDocumentAnalysis& analysis) {
    SctStructuredControlFlowAnalysis result;
    std::unordered_map<std::uint64_t, InstructionLocation> locations;
    for (const auto& section : document.sections) {
        const auto* script = std::get_if<SctScriptSectionContent>(&section.content);
        if (script == nullptr) continue;
        for (std::size_t ordinal = 0; ordinal < script->instructions.size(); ++ordinal) {
            locations.emplace(script->instructions[ordinal].id.value(),
                InstructionLocation{section.id, ordinal, &script->instructions[ordinal]});
        }
    }
    std::vector<SctStructureIssue> globalIssues;
    const auto edges = effectiveEdges(document, analysis, locations, globalIssues);
    std::vector<EffectiveEdge> currentEdges;
    std::vector<EffectiveEdge> importedHints;
    for (const auto& edge : edges) {
        (edge.importedOpaqueHint ? importedHints : currentEdges).push_back(edge);
    }
    for (const auto& section : document.sections) {
        const auto* script = std::get_if<SctScriptSectionContent>(&section.content);
        if (script == nullptr) continue;
        SctSectionStructure output;
        output.section = section.id;
        for (const auto& issue : globalIssues)
            if (issue.section == section.id) output.issues.push_back(issue);
        auto graph = buildGraph(document, section, *script, currentEdges,
            locations, output, importedHints);
        if (!graph.blocks.empty()) {
            detectLoops(graph, output);
            detectConditionals(graph, output);
            detectSwitches(graph, output);

            for (const auto& hint : importedHints) {
                const auto source = locations.find(hint.edge.sourceInstruction.value());
                if (source == locations.end() || source->second.section != section.id) continue;
                auto augmentedEdges = currentEdges;
                augmentedEdges.push_back(hint);
                SctSectionStructure candidateOutput;
                candidateOutput.section = section.id;
                auto candidateGraph = buildGraph(document, section, *script,
                    augmentedEdges, locations, candidateOutput, importedHints);
                detectLoops(candidateGraph, candidateOutput);
                detectConditionals(candidateGraph, candidateOutput);
                detectSwitches(candidateGraph, candidateOutput);
                resolveNesting(candidateOutput);
                bool recognized = false;
                for (auto& region : candidateOutput.regions) {
                    const bool usesHint = std::ranges::any_of(region.evidence,
                        [&](const auto& evidence) {
                            return evidence.kind
                                    == SctStructureEvidenceKind::ImportedOpaqueControlFlowGap
                                && evidence.source == std::optional{hint.edge.sourceInstruction}
                                && evidence.target == hint.edge.targetInstruction
                                && evidence.opaqueAttachment == hint.attachment;
                        });
                    if (!usesHint || region.strength
                            != SctStructureClaimStrength::EvidenceLimited) continue;
                    recognized = true;
                    if (!std::ranges::any_of(output.regions, [&](const auto& existing) {
                            return existing.id == region.id;
                        })) {
                        output.regions.push_back(std::move(region));
                    }
                }
                if (!recognized) {
                    addIssue(output, SctStructureIssueKind::RejectedLegacyCandidate,
                        section.id, hint.edge.sourceInstruction,
                        hint.edge.targetInstruction
                            ? std::vector<SctInstructionId>{*hint.edge.targetInstruction}
                            : std::vector<SctInstructionId>{},
                        {{SctStructureEvidenceKind::ImportedOpaqueControlFlowGap,
                            SctSemanticConfidence::Heuristic,
                            hint.edge.sourceInstruction, hint.edge.targetInstruction,
                            hint.attachment}});
                }
            }
            resolveNesting(output);
            buildOutline(graph, output);
        }
        output.blocks = std::move(graph.blocks);
        result.sections_.push_back(std::move(output));
    }
    return result;
}

const SctSectionStructure* SctStructuredControlFlowAnalysis::findSection(
    const SctSectionId section) const noexcept {
    const auto found = std::ranges::find(sections_, section, &SctSectionStructure::section);
    return found == sections_.end() ? nullptr : &*found;
}

const SctBasicBlock* SctStructuredControlFlowAnalysis::blockContaining(
    const SctInstructionId instructionId) const noexcept {
    for (const auto& section : sections_) {
        for (const auto& block : section.blocks) {
            if (std::ranges::find(block.instructions, instructionId) != block.instructions.end())
                return &block;
        }
    }
    return nullptr;
}

const SctStructuredRegion* SctStructuredControlFlowAnalysis::findRegion(
    const SctStructuredRegionId& regionId) const noexcept {
    const auto* section = findSection(regionId.section);
    if (section == nullptr) return nullptr;
    const auto found = std::ranges::find(section->regions, regionId, &SctStructuredRegion::id);
    return found == section->regions.end() ? nullptr : &*found;
}

std::vector<SctStructuredRegion> SctStructuredControlFlowAnalysis::regionsContaining(
    const SctInstructionId instructionId) const {
    std::vector<SctStructuredRegion> result;
    const auto* block = blockContaining(instructionId);
    if (block == nullptr) return result;
    const auto* section = findSection(block->id.section);
    if (section == nullptr) return result;
    for (const auto& region : section->regions) {
        if (std::ranges::find(region.members, block->id) != region.members.end())
            result.push_back(region);
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return left.members.size() < right.members.size();
    });
    return result;
}

}  // namespace salsa::spice_sct_prototype
