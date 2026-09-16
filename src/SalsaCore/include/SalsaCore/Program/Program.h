#pragma once

#include "SalsaCore/Editor/EditorBackend.h"
#include "SalsaCore/Authoring/SctAuthoringProject.h"
#include <memory>
#include <span>

namespace salsa::core::program {

// Pool handles are internal addresses, never authored identity. They are valid
// for the lifetime of their immutable Program (and may change on file loading).
struct ValueId {
    std::uint32_t value = UINT32_MAX;
    bool valid() const noexcept { return value != UINT32_MAX; }
    auto operator<=>(const ValueId&) const = default;
};
enum class ValueKind : std::uint32_t {
    Instruction, Parameter, RepeatedGroup, Word, Expression, ValueOperation,
    BinaryOperation, StackOperation, InertOperation, Words, ActionReference,
    IndexedTextReference, SupplementaryTextReference, Unresolved, Unavailable,
    PlainText, Message, TextChunk, TextCommand, EmptyIndexedText, IndexedSection, StringGroup
};
// Exact typed SPICE semantics in flat tables. `data` and `detail` are defined by
// kind; see Docs/WholeProgramIrModel.md. No recursive heap-owned value tree.
struct Value {
    std::uint64_t data = 0;
    std::uint32_t first = 0, count = 0, detail = 0;
    ValueKind kind = ValueKind::Word;
};
enum class ActionKind : std::uint8_t { Instruction, Conditional, Loop, Switch, Transfer, Dispatch };
struct Action {
    SctActionId id;
    SctSequenceId owner;
    ActionKind kind = ActionKind::Instruction;
    ValueId value; // Instruction payload; absent for a conditional.
    std::uint32_t firstArm = 0, armCount = 0;
    std::uint64_t sourceInstruction = 0; // Qualified by owner's script baseline.
    std::uint32_t detail = UINT32_MAX; // Index into the kind-specific construct table.
};
enum class CaseExit : std::uint8_t { ExitSwitch, Fallthrough };
struct Arm {
    ArmId id;
    SctActionId owner;
    ConditionId condition; // Conditional arm check; absent for Else and switch arms.
    SctSequenceId body;
    std::uint32_t firstLabel = 0, labelCount = 0; // Shared ordered case-label pool.
    bool includesDefault = false; // Default may share this body with numeric labels.
    CaseExit caseExit = CaseExit::ExitSwitch;
};
struct ConditionRegion {
    ConditionId id;
    SctActionId owner;
    std::optional<ArmId> arm;
    SctSequenceId preparation;
    ValueId result;
    bool skipRefresh = true; // Timing of the lowered controller, independent of its origin.
};
enum class LoopKind : std::uint8_t { While, DoWhile, For };
struct Loop {
    SctActionId owner;
    LoopKind kind = LoopKind::While;
    ConditionId condition;
    SctSequenceId initialize, body, update; // initialize/update exist only for For.
};
struct Switch { SctActionId owner; ConditionId selector; };
enum class DispatchMode : std::uint8_t { Independent, FirstMatch };
// Pure comparisons captured at their original evaluation phase. Predicates own
// their values independently; these samples preserve read timing during lowering.
struct DispatchSample {
    ConditionId condition;
    std::uint32_t beforeRoute = 0;
};
struct DispatchRoute {
    ArmId arm;
    std::vector<ConditionId> samples;
};
struct Dispatch {
    SctActionId owner;
    DispatchMode mode = DispatchMode::Independent;
    std::vector<DispatchSample> samples;
    std::vector<DispatchRoute> routes;
};
// Construction input only; published labels are pooled, not a vector per arm.
struct Case {
    std::vector<std::int32_t> labels;
    bool includesDefault = false;
    CaseExit exit = CaseExit::ExitSwitch;
};
enum class TransferKind : std::uint8_t { Break, Continue };
struct Transfer { SctActionId owner, target; TransferKind kind = TransferKind::Break; bool skipRefresh = true; };
struct TransferDestination {
    SctActionId construct;
    SctSequenceId sequence; // Absent means after the target construct (Break).
};
using SequenceOwner = std::variant<ArmId, ConditionId, SctActionId>;
enum class SequenceRole : std::uint8_t { SectionBody, ArmBody, ConditionPreparation, LoopInitialize, LoopBody, LoopUpdate };
struct Sequence {
    SctSequenceId id;
    SctScriptId script;
    std::optional<SequenceOwner> owner; // Root sequences belong to sections.
    std::shared_ptr<const std::vector<SctActionId>> order;
    SequenceRole role = SequenceRole::SectionBody;
};
struct Section {
    SectionId id;
    SctScriptId script;
    std::string name;
    SctSequenceId body; // Absent for non-code sections.
    ValueId content;
    std::uint64_t sourceSection = 0;
};
struct Text {
    SctTextResourceId id;
    SctScriptId script;
    ValueId value;
    spice::sct::SctTextKind kind{};
    bool supplementary = false;
    std::uint64_t sourceText = 0;
    std::string sourceName, sourceGroup, correspondenceKey;
    std::vector<SctLanguageGroupId> languages;
    bool explicitAssignment = false;
};
struct Script {
    SctScriptContext context;
    std::vector<SectionId> sections;
    std::vector<SctTextResourceId> supplementaryText;
};
struct Site {
    SctActionId action;
    std::optional<spice::sct::SctParameterAddress> parameter; // Empty: schedule or structured condition.
    std::optional<ArmId> arm;
    std::uint32_t operation = 0;
    std::optional<ConditionId> condition;
};
struct Module {
    SctModuleId id;
    SctScriptId script;
    std::string name;
    std::vector<SctSequenceId> sequences;
    std::vector<SctAuthoringEvidence> evidence;
};
struct Entrypoint {
    SctEntrypointId id;
    SctScriptId script;
    std::string name;
    SctEntrypointKind kind = SctEntrypointKind::NamedSection;
    SctSequenceId body;
    bool enabled = true;
    SctEntrypointActivation activation = SctUnresolvedBinding{"", "Activation boundary not selected", {}};
    SctAuthoringReference<SctPortId> response = SctUnresolvedBinding{"", "Response not selected", {}};
    std::optional<Site> responseSite, predicate;
    std::optional<std::variant<smores::script::Entry, SctUnresolvedBinding>> provider;
    std::vector<SctAuthoringEvidence> evidence;
    bool required() const noexcept { return kind == SctEntrypointKind::Lifecycle && (name == "init" || name == "loop"); }
};
// Slots describe a cutscene interface without owning a second copy of its code.
using SlotRole = editor::SlotRole;
using SlotBoundary = editor::SlotBoundary;
struct Port {
    SctPortId id;
    SctPortOwner owner;
    std::string name;
    std::optional<SctActionId> action;
    std::optional<SctSequenceId> sequence;
    SlotRole role = SlotRole::Generic;
    SlotBoundary boundary = SlotBoundary::Operand;
    std::optional<spice::sct::SctParameterAddress> operand;
};

using EntryMethod = editor::EntryMethod;
using RouteOutcome = editor::RouteOutcome;
enum class RequirementMatch : std::uint8_t { True, False, Case, Default };
struct RouteRequirement {
    Site site;
    RequirementMatch match = RequirementMatch::True;
    std::vector<std::int32_t> labels;
};
// Requirements in one path are conjunctive; paths are alternatives. Expressions
// remain in the executable IR and are resolved from these stable source sites.
struct RoutePath {
    std::vector<RouteRequirement> requirements;
    std::vector<SctActionId> preparation, consumption;
};
struct RouteDefinition {
    EntryMethod method = EntryMethod::Unknown;
    RouteOutcome outcome = RouteOutcome::Observe;
    std::vector<RoutePath> paths;
    std::optional<SctEntrypointId> entrypoint, arrival;
    // Receipt of an explicit connection command, never a replay recipe.
    std::optional<SctActionId> implementation, boundTarget;
    std::optional<spice::sct::SctParameterAddress> targetOperand;
    std::string boundFilename;
    std::optional<std::uint32_t> implementationOpcode;
    std::optional<ArmId> dispatchArm; // Authoritative logical requirement/body in its dispatch region.
};
struct Connection {
    SctConnectionId id;
    SctAuthoringReference<SctPortId> source, destination;
    std::vector<SctAuthoringEvidence> evidence;
    std::optional<SctExecutableRoute> route;
    std::optional<RouteDefinition> cutscene;
};

struct Annotation {
    editor::Subject subject;
    std::string alias;
    std::vector<SctAuthoringEvidence> evidence;
    std::string note, bookmark;
    std::optional<std::uint32_t> color;
};
using VariableMeaning = editor::Meaning;
struct Metadata {
    bool storyEnabled = false;
    editor::LocalizationState localization;
    std::vector<editor::SectionFolder> folders;
    std::vector<editor::OpcodeCustomization> opcodes;
    std::vector<Module> modules;
    std::vector<Entrypoint> entrypoints;
    std::vector<Port> ports;
    std::vector<SctLanguageGroup> languages;
    std::vector<Connection> connections;
    std::vector<SctLogicalText> logicalTexts;
    std::vector<Annotation> annotations;
    std::vector<VariableMeaning> meanings;
    std::vector<SctModuleId> narrative;
    std::vector<editor::InterpretationRecord> interpretations;
};
enum class Bank : std::uint8_t { Bit, Byte, Integer, Float };
struct Variable {
    Bank bank = Bank::Bit;
    std::uint32_t index = 0;
    auto operator<=>(const Variable&) const = default;
};
enum class Access : std::uint8_t { Read, Set, Clear, Assign, Modify, ComputedWrite };
struct VariableUse {
    Site site;
    Access access = Access::Read;
    ValueId expression;
    std::optional<Variable> variable; // Missing for computed destinations.
    Bank destinationBank = Bank::Bit;
};
enum class TargetUseKind : std::uint8_t { Operand, Break, Continue };
struct TargetUse { Site site; SctActionId target; TargetUseKind kind = TargetUseKind::Operand; };
struct Statistics {
    std::uint64_t actions = 0, values = 0, actionPages = 0, valuePages = 0;
    std::uint64_t excludedSourceBytes = 0, unavailableItems = 0;
};
class ProgramBuilder;
class NativeNormalizer;
struct ProgramData;
class Program final {
public:
    ~Program();
    Program(const Program&) = delete;
    const SctAuthoringProjectId& id() const noexcept;
    std::uint64_t revision() const noexcept;
    std::uint64_t highWater() const noexcept;
    std::span<const Script> scripts() const noexcept;
    std::span<const SctAuthoringBaseline> baselines() const noexcept;
    std::span<const Section> sections() const noexcept;
    std::span<const Text> texts() const noexcept;
    const Metadata& metadata() const noexcept;
    const Action* find(SctActionId) const noexcept;
    const Sequence* find(SctSequenceId) const noexcept;
    const Section* find(SectionId) const noexcept;
    const Script* find(SctScriptId) const noexcept;
    const Text* find(SctTextResourceId) const noexcept;
    const Value* find(ValueId) const noexcept;
    const ConditionRegion* find(ConditionId) const noexcept;
    const Arm *find(ArmId) const noexcept;
    const Loop* loop(SctActionId) const noexcept;
    const Switch* switchRegion(SctActionId) const noexcept;
    const Dispatch *dispatch(SctActionId) const noexcept;
    const Transfer* transfer(SctActionId) const noexcept;
    std::optional<TransferDestination> transferDestination(SctActionId) const noexcept;
    std::span<const ValueId> children(ValueId) const noexcept;
    std::string_view text(ValueId) const noexcept;
    std::span<const Arm> arms(SctActionId) const noexcept;
    std::span<const std::int32_t> caseLabels(ArmId) const noexcept;
    std::span<const VariableUse> uses(Variable) const noexcept;
    std::span<const VariableUse> computedWrites() const noexcept;
    std::span<const TargetUse> incoming(SctActionId) const noexcept;
    // Physical sequence order is separate from target operands. This does not
    // assert that an instruction permits fallthrough; WP3 supplies CFG semantics.
    std::optional<SctActionId> nextInSequence(SctActionId) const noexcept;
    Statistics statistics() const noexcept;
private:
    friend class ProgramBuilder;
    friend struct ServiceOwner;
    friend struct NativeCollection;
    friend class ProgramPersistence;
    explicit Program(std::unique_ptr<ProgramData>);
    std::unique_ptr<ProgramData> data_;
};

// One unpublished build transaction, also the future binary-load boundary.
// Mutators fail the transaction on invalid input; finish then returns no Program.
// Published Programs never change. WP2 adds local revision publication/history.
class ProgramBuilder final {
public:
    explicit ProgramBuilder(SctAuthoringProjectId, std::uint64_t revision = 0, std::uint64_t highWater = 0);
    ~ProgramBuilder();
    ProgramBuilder(const ProgramBuilder&) = delete;
    // Bulk input for already-decoded project tables; still requires a baseline.
    Result<SctScriptId> importedScript(SctAuthoringBaseline, SctScriptContext);
    Result<SectionId> section(Section, SctSequenceId requestedBody = {});
    Result<SctTextResourceId> textEntity(Text);
    std::vector<SectionId> scriptSections(SctScriptId) const;
    SctSequenceId sectionBody(SectionId) const noexcept;
    Result<SctScriptId> appendImported(const spice::sct::SctDocument&, SctAuthoringBaseline, SctScriptContext);
    Result<ValueId> expression(const spice::sct::SctCanonicalExpression&);
    // Children must already exist. This bottom-up contract rules out value cycles.
    Result<ValueId> value(ValueKind, std::uint64_t data = 0, std::uint32_t detail = 0,
        std::span<const ValueId> children = {});
    Result<ValueId> text(ValueKind, std::string_view);
    Result<SctActionId> instruction(SctSequenceId, ValueId payload, SctActionId requested = {});
    // First arm has a condition; absent condition is allowed only on final Else.
    // Creates empty owned body sequences; use instruction/conditional to fill them.
    Result<SctActionId> conditional(SctSequenceId, std::span<const ValueId> conditions);
    std::vector<SctSequenceId> branchBodies(SctActionId) const;
    std::vector<ConditionId> branchConditions(SctActionId) const;
    SctSequenceId conditionPreparation(ConditionId) const noexcept;
    Result<SctActionId> loop(SctSequenceId, LoopKind, ValueId condition);
    std::optional<Loop> loop(SctActionId) const;
    Result<SctActionId> switchRegion(SctSequenceId, ValueId selector, std::span<const Case>);
    std::optional<Switch> switchRegion(SctActionId) const;
    // Targets must lexically enclose this action. Continue targets a loop body;
    // Break can target any enclosing loop or switch scope.
    Result<SctActionId> transfer(SctSequenceId, TransferKind, SctActionId target);
    // Cold authoring metadata, never an SctDocument or a physical patch.
    Metadata& metadata();
    template<class Id> Result<Id> allocate() {
        auto result = allocateIdentity();
        if (!result) return Result<Id>::failure(result.diagnostics());
        return Result<Id>::success(Id{result.value()});
    }
    Result<std::shared_ptr<const Program>> finish();
private:
    friend class NativeNormalizer;
    friend class LegacyProgramImport;
    Result<std::uint64_t> allocateIdentity();
    std::unique_ptr<ProgramData> data_;
    std::vector<Diagnostic> diagnostics_;
};
} // namespace salsa::core::program
