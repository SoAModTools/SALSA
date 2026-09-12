#pragma once
#include "SalsaCore/Authoring/SctAuthoringSession.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"

namespace salsa::core {

enum class SctSequenceActionKind { Action, Branch, Switch, Jump, Call, Return, Wait, Preserved };
struct SctSequenceActionView final {
    SctActionId id;
    spice::sct::SctDocumentInstruction instruction;
    SctSequenceActionKind kind = SctSequenceActionKind::Preserved;
    std::string label;
    // All explicit control targets, in source parameter order. No inferred join.
    std::vector<spice::sct::SctInstructionId> targets;
};
struct SctStateRead final {
    SctVariableKey storage;
    spice::sct::SctScptValueKind access;
    std::uint32_t operationOrdinal = 0;
};
struct SctArrivalLocation final {
    std::string stem;
    std::uint32_t encoded = 0;
};

class SctSequenceAuthoring final {
public:
    [[nodiscard]] static Result<SctAuthoringState> promoteSequence(const SctAuthoringState& state,
        SctScriptId script, spice::sct::SctSectionId section, std::string name);
    [[nodiscard]] static Result<std::vector<SctSequenceActionView>> actions(const SctAuthoringState& state, SctSequenceId sequence);
    [[nodiscard]] static Result<SctAuthoringState> setTiming(const SctAuthoringState& state,
        SctSequenceId sequence, SctActionId action, std::optional<spice::sct::SctCanonicalExpression> schedule, bool skipRefresh);
    [[nodiscard]] static Result<SctAuthoringState> namePredicate(const SctAuthoringState& state,
        SctScriptId script, std::vector<spice::sct::SctParameterSite> uses, std::string name);
    [[nodiscard]] static Result<spice::sct::SctCanonicalExpression> predicate(const SctAuthoringState& state, SctPredicateId predicate);
    [[nodiscard]] static Result<SctAuthoringState> editPredicate(const SctAuthoringState& state,
        SctPredicateId predicate, const spice::sct::SctCanonicalExpression& expression);
    [[nodiscard]] static std::vector<SctStateRead> stateReads(const spice::sct::SctCanonicalExpression& expression);
    [[nodiscard]] static Result<SctArrivalLocation> arrivalLocation(std::string stem);
    [[nodiscard]] static Result<spice::sct::SctCanonicalExpression> arrivalCondition(std::string stem);
    // Only replaces the literal in an existing simple low-16 arrival comparison;
    // compound predicates use the ordinary explicit expression editor.
    [[nodiscard]] static Result<SctAuthoringState> selectArrival(const SctAuthoringState& state, SctPredicateId predicate, std::string stem);
    [[nodiscard]] static Result<SctAuthoringState> selectArrivalCase(const SctAuthoringState& state,
        SctScriptId script, spice::sct::SctInstructionId controller, std::uint32_t caseOrdinal, std::string stem);
    [[nodiscard]] static std::string specialArrivalMeaning(std::uint32_t encoded);

    [[nodiscard]] static Result<void> validateProgram(const SctAuthoringProject& project,
        SctScriptId script, const spice::sct::SctDocument& document);
    [[nodiscard]] static Result<void> reconcileActions(SctAuthoringProject& project,
        SctScriptId script, const spice::sct::SctDocument& document);
};
} // namespace salsa::core
