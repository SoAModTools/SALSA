#pragma once
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"

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
struct SctInterpretationAnalysis final {
    SctAuthoringProjectId project;
    RevisionId revision;
    std::vector<SctStateAccess> accesses;
    std::vector<SctStateLink> dependencies;
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
    [[nodiscard]] static SctInterpretationAnalysis analyze(const SctAuthoringState &);
    [[nodiscard]] static Result<SctAuthoringState> record(const SctAuthoringState &, const SctInterpretationAnalysis &);
    [[nodiscard]] static Result<SctAuthoringState> decide(const SctAuthoringState &, SctExplanationId,
                                                          SctInterpretationDecision);
    [[nodiscard]] static Result<SctAuthoringState> setName(const SctAuthoringState &, SctMeaningTarget, std::string);
};
} // namespace salsa::core
