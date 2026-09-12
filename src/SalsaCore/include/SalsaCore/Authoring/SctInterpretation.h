#pragma once
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
#include <functional>
#include <map>
#include <span>

namespace salsa::core {
enum class SctStateAccessKind {
    Read,
    Set,
    Clear,
    Toggle,
    Assign,
    UnknownWrite
};
struct SctStateAccess final {
    SctMeaningTarget target;
    SctVariableKey variable;
    SctStateAccessKind kind;
    std::string expression;
    bool condition = false;
};
struct SctStateLink final {
    SctMeaningTarget writer, reader;
    SctVariableKey variable;
};
// A write with an expression-addressed destination stays visible even when no
// concrete variable index can be assigned. It never receives an invented index.
struct SctUnresolvedStateWrite final {
    SctMeaningTarget target;
    SctStateAccessKind kind;
    std::string expression;
};
struct SctStateEffectScope final {
    SctMeaningTarget root;
    std::vector<SctMeaningTarget> sections;
    std::vector<std::size_t> writes; // indexes in analysis.accesses, in source order
    std::vector<std::size_t> unresolvedWrites;
};
struct SctStateIncidence final {
    std::vector<std::size_t> writers, readers;
};
class SctStateConnectivityIndex final {
public:
    std::map<SctVariableKey,SctStateIncidence> variables;
    [[nodiscard]] static SctStateConnectivityIndex build(std::span<const SctStateAccess>);
    [[nodiscard]] std::uint64_t pairCount() const;
    // Return true to stop. Pairs are generated only while the caller requests them.
    [[nodiscard]] bool anyLink(std::span<const SctStateAccess>, const std::function<bool(const SctStateLink&)>&,
        std::optional<SctVariableKey> variable = {}) const;
};
struct SctConnectivityRoot final {
    SctMeaningTarget target;
    std::vector<spice::sct::SctSectionId> sections;
};
struct SctDocumentConnectivityFacts final {
    std::vector<SctStateAccess> accesses;
    std::vector<SctUnresolvedStateWrite> unresolvedWrites;
    std::vector<SctStateEffectScope> effects;
    std::vector<spice::sct::SctControlFlowEdge> controlFlow;
};
struct SctInterpretationAnalysis final {
    SctAuthoringProjectId project;
    RevisionId revision;
    std::vector<SctStateAccess> accesses;
    SctStateConnectivityIndex connectivity;
    std::vector<SctUnresolvedStateWrite> unresolvedWrites;
    std::vector<SctStateEffectScope> effects;
    std::vector<SctExplanationRecord> suggestions;
    std::vector<Diagnostic> diagnostics;
};
class SctInterpretation final {
  public:
    [[nodiscard]] static std::vector<Diagnostic> validate(const SctAuthoringProject &);
    [[nodiscard]] static SctMeaningTarget sectionTarget(const SctAuthoringProject &, SctScriptId,
                                                        spice::sct::SctSectionId);
    [[nodiscard]] static SctMeaningTarget siteTarget(const SctAuthoringProject &, SctScriptId,
                                                     spice::sct::SctParameterSite);
    [[nodiscard]] static std::string describe(const SctAuthoringState &, const SctMeaningTarget &);
    [[nodiscard]] static std::string observe(const SctAuthoringState &, const SctMeaningTarget &);
    [[nodiscard]] static bool fresh(const SctAuthoringState &, const SctExplanationRecord &);
    [[nodiscard]] static std::string label(const SctAuthoringState &, const SctMeaningTarget &, std::string fallback);
    [[nodiscard]] static std::string condition(const SctAuthoringState &, SctScriptId,
                                               const spice::sct::SctCanonicalExpression &, bool friendly = true);
    [[nodiscard]] static Result<std::vector<spice::sct::SctInstructionId>> generatedInstructions(
        const SctAuthoringState &, const SctMeaningTarget &, const SctPreparedAuthoringOutput &,
        const SctRealizationKey &expected);
    // Pure document extraction: no project adoption, naming decisions or export.
    [[nodiscard]] static SctDocumentConnectivityFacts extract(const spice::sct::SctDocument&, SctScriptId,
        SctImportedDocumentId, std::span<const SctConnectivityRoot> roots = {});
    [[nodiscard]] static SctInterpretationAnalysis analyze(const SctAuthoringState &);
    [[nodiscard]] static Result<SctAuthoringState> record(const SctAuthoringState &, const SctInterpretationAnalysis &);
    [[nodiscard]] static Result<SctAuthoringState> decide(const SctAuthoringState &, SctExplanationId,
                                                          SctInterpretationDecision);
    [[nodiscard]] static Result<SctAuthoringState> setName(const SctAuthoringState &, SctMeaningTarget, std::string);
};
} // namespace salsa::core
