#pragma once
#include "SalsaCore/Program/NativeImport.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include <future>
#include <stop_token>

namespace salsa::core::program {
struct Revision {
    SctAuthoringProjectId project;
    std::uint64_t revision = 0, generation = 0;
    auto operator<=>(const Revision &) const = default;
};
struct ServiceStatus {
    Revision current;
    std::uint64_t historyState = 0;
    bool dirty = false;
};
struct ScriptSummary {
    SctScriptId id;
    std::string name;
    std::size_t sections = 0;
};
struct Catalog {
    ServiceStatus status;
    std::vector<ScriptSummary> scripts;
    std::vector<Module> modules;
};

// Detached, bottom-up payload input/output. Handles are local to this buffer.
// Instruction roots retain schedule/refresh and fixed/repeated parameter order.
struct ValueBuffer {
    std::vector<Value> values;
    std::vector<ValueId> children;
    std::string characters;
    ValueId root;
    ValueId add(ValueKind, std::uint64_t data = 0, std::uint32_t detail = 0, std::span<const ValueId> = {});
    ValueId text(ValueKind, std::string_view);
    static Result<ValueBuffer> expression(const spice::sct::SctCanonicalExpression &);
};
struct ReplaceParameter {
    SctActionId action;
    spice::sct::SctParameterAddress address;
    ValueBuffer value;
};
struct ReplaceInstruction {
    SctActionId action;
    ValueBuffer value;
};
struct ReplaceCondition {
    ConditionId condition;
    ValueBuffer value;
};
struct ReplaceText {
    SctTextResourceId text;
    ValueBuffer value;
};
struct InsertInstruction {
    SctSequenceId sequence;
    std::size_t position = 0;
    ValueBuffer value;
};
struct EraseInstruction {
    SctActionId action;
};
struct MoveInstruction {
    SctActionId action;
    SctSequenceId sequence;
    std::size_t position = 0;
};
struct RenameSection {
    SectionId section;
    std::string name;
};
struct ReorderSections {
    SctScriptId script;
    std::vector<SectionId> order;
};
struct AddSection {
    SctScriptId script;
    std::string name;
    std::size_t position = 0;
};
struct EraseEmptySection {
    SectionId section;
};
struct EditAnnotation {
    std::size_t index = 0;
    std::string alias;
    std::vector<SctAuthoringEvidence> evidence;
};
// New arms allocate identity; existing IDs retain their body and preparation.
struct ArmEdit {
    ArmId id;
    std::optional<ValueBuffer> condition;
    std::vector<std::int32_t> labels;
    bool includesDefault = false;
    CaseExit exit = CaseExit::ExitSwitch;
};
struct InsertConstruct {
    SctSequenceId sequence;
    std::size_t position = 0;
    ActionKind kind = ActionKind::Conditional;
    LoopKind loopKind = LoopKind::While;
    ValueBuffer condition;
    std::vector<ArmEdit> arms;
};
struct ReplaceArms {
    SctActionId action;
    std::vector<ArmEdit> arms;
};
struct InsertTransfer {
    SctSequenceId sequence;
    std::size_t position = 0;
    TransferKind kind;
    SctActionId target;
};
struct MoveAction {
    SctActionId action;
    SctSequenceId sequence;
    std::size_t position = 0;
};
struct EraseAction {
    SctActionId action;
};
struct BindLifecycle {
    SectionId section;
    std::string name;
};
struct ScriptSettings {
    SctScriptId script;
    std::string name;
    std::optional<SctAuthoredOutputProfile> output;
    std::vector<SctScriptContext::ResourceAssociation> resources;
};
struct RetargetTransfer {
    SctActionId action, target;
};
// Recognizes a bounded If/Else diamond or While back-edge in one sequence.
// Unsupported topology fails without altering its explicit instruction leaves.
struct PromoteFlow {
    SctActionId header, continuation;
};
using MetadataEntity = std::variant<Module, Entrypoint, SctAuthoringPort, SctLanguageGroup, SctAuthoringConnection,
                                    SctLogicalText, VariableMeaning>;
struct PutMetadata {
    MetadataEntity entity;
}; // zero ID allocates; nonzero updates
using MetadataId = std::variant<SctModuleId, SctEntrypointId, SctPortId, SctLanguageGroupId, SctConnectionId,
                                SctLogicalTextId, SctMeaningId>;
struct EraseMetadata {
    MetadataId id;
};
struct PutAnnotation {
    Annotation annotation;
}; // upsert by stable subject
struct AssignLanguages {
    SctTextResourceId text;
    std::vector<SctLanguageGroupId> languages;
};
using SemanticOperation =
    std::variant<InsertConstruct, ReplaceArms, InsertTransfer, MoveAction, EraseAction, RetargetTransfer, PromoteFlow,
                 PutMetadata, EraseMetadata, PutAnnotation, AssignLanguages, BindLifecycle, ScriptSettings>;
using Primitive = std::variant<ReplaceParameter, ReplaceInstruction, ReplaceCondition, ReplaceText, InsertInstruction,
                               EraseInstruction, MoveInstruction, RenameSection, ReorderSections, AddSection,
                               EraseEmptySection, EditAnnotation, SemanticOperation>;
struct EditRequest {
    Revision expected;
    std::vector<Primitive> operations;
};
struct ChangeReceipt {
    ServiceStatus status;
    std::vector<SctActionId> actions;
    std::vector<SctSequenceId> sequences;
    std::vector<SectionId> sections;
    std::vector<SctTextResourceId> texts, presentationTexts;
    std::vector<ConditionId> conditions;
    std::vector<ArmId> arms;
    std::vector<std::uint64_t> metadata;
    std::vector<SctModuleId> modules;
    bool presentationChanged = false;
    std::vector<Variable> oldVariables, newVariables;
    std::vector<SctActionId> oldTargets, newTargets;
    std::vector<std::size_t> annotations;
    std::uint64_t examinedActions = 0, copiedOrderItems = 0;
    std::vector<Diagnostic> diagnostics;
};
struct PreparedData;
class PreparedChange {
    friend struct ServiceOwner;
    std::shared_ptr<PreparedData> data_;

  public:
    PreparedChange() = default;
    const ChangeReceipt &preview() const;
};

struct ReadSelection {
    std::optional<SctScriptId> script;
    std::optional<SctModuleId> module;
    std::vector<SctActionId> actions;
};
// Owns only the selected closure. Safe across edits, maintenance and shutdown.
// Sequence orders may contain IDs outside an explicit action selection.
struct ReadScope {
    ServiceStatus status;
    std::vector<Script> scripts;
    std::vector<Action> actions;
    std::vector<Sequence> sequences;
    std::vector<Section> sections;
    std::vector<Text> texts;
    std::vector<Arm> arms;
    std::vector<std::int32_t> caseLabels;
    std::vector<ConditionRegion> conditions;
    std::vector<Loop> loops;
    std::vector<Switch> switches;
    std::vector<Transfer> transfers;
    ValueBuffer payload;
};
struct VariableQuery {
    ServiceStatus status;
    Variable variable;
    std::vector<VariableUse> uses;
    ValueBuffer payload;
};
struct TargetQuery {
    ServiceStatus status;
    SctActionId target;
    std::vector<TargetUse> uses;
};
enum class FlowRole {
    Fallthrough,
    Call,
    Jump,
    TrueBranch,
    FalseBranch,
    Case,
    Default,
    LoopBody,
    LoopExit,
    Break,
    Continue,
    Return,
    Transition,
    Unknown
};
struct RelationshipKey {
    SctScriptId script;
    SctActionId source;
    FlowRole role = FlowRole::Unknown;
    std::uint64_t arm = 0;
    std::uint32_t ordinal = 0;
    auto operator<=>(const RelationshipKey &) const = default;
};
struct FlowRelationship {
    RelationshipKey key;
    SctActionId target, scopeTarget;
    SctSequenceId sequence;
    std::optional<SctScriptId> scriptTarget;
    std::optional<spice::sct::SctParameterAddress> operand;
    ValueId predicate, schedule;
    ConditionId condition, targetCondition;
    SctSequenceId preparation;
    std::vector<std::int32_t> labels;
    bool skipRefresh = false;
    std::uint64_t sourceInstruction = 0;
};
struct ModuleSummary {
    SctModuleId module;
    std::uint64_t build = 0;
    std::vector<SctActionId> actions;
    std::vector<SctSequenceId> sequences;
    std::vector<SectionId> sections;
    std::vector<SctTextResourceId> texts;
    std::vector<Variable> variables;
    std::vector<VariableUse> effects;
    std::vector<FlowRelationship> flow;
    std::vector<Loop> loops;
    std::vector<Diagnostic> diagnostics;
    ValueBuffer payload;
    bool cyclic = false, unknown = false;
};
struct SummaryQuery {
    ServiceStatus status;
    std::shared_ptr<const ModuleSummary> summary;
    bool reused = false;
};
struct InteractionQuery {
    ServiceStatus status;
    std::vector<Entrypoint> entrypoints;
    std::vector<SctAuthoringConnection> connections;
    std::vector<Annotation> annotations;
    std::vector<VariableMeaning> meanings;
    std::vector<SctLanguageGroup> languages;
};
struct ExplanationQuery {
    ServiceStatus status;
    std::string label;
    std::vector<SctAuthoringEvidence> records;
};
struct Checkpoint {
    ServiceStatus status;
    std::shared_ptr<const Program> program;
};
struct Maintenance {
    std::uint64_t valuesBefore = 0, valuesAfter = 0;
};

// Reads occur only after a job obtains its working-memory reservation. Readers
// must honor stop where possible; byteSize is checked after reading. Identity is
// assigned by manifest ordinal, independently of worker completion order.
struct NativeLoadItem {
    SctAuthoringBaseline baseline;
    SctScriptContext context;
    spice::sct::SctDocumentImportOptions options;
    std::uint64_t workingBytes = 0;
    std::function<Result<std::vector<std::uint8_t>>(std::stop_token)> read;
};
struct NativeLoadRequest {
    Revision expected;
    SctAuthoringProjectId project;
    std::vector<NativeLoadItem> items;
    std::size_t workers = 2;
    std::uint64_t workingByteBudget = 256ull * 1024 * 1024;
    SctLoadingObserver progress;
};

// One serialized owner; no live model pointers escape. Callbacks run off the UI
// thread. The caller queues progress into its exclusive-operation widget.
// Destruction requests shutdown without joining parser threads on the caller.
struct ProgramOpenRequest;
struct ProgramOpenResult;
class ProgramService final {
  public:
    ProgramService();
    ~ProgramService();
    ProgramService(const ProgramService &) = delete;
    std::future<Result<ServiceStatus>> status();
    std::future<Result<Catalog>> catalog(Revision);
    // Consumes exclusive ownership, avoiding a full-program copy on adoption.
    std::future<Result<ServiceStatus>> adopt(Revision, std::shared_ptr<const Program>, std::stop_token = {});
    std::future<Result<ServiceStatus>> load(NativeLoadRequest, std::stop_token = {});
    std::future<Result<ProgramOpenResult>> openProject(ProgramOpenRequest, std::stop_token = {});
    std::future<Result<PreparedChange>> prepare(EditRequest, std::stop_token = {});
    std::future<Result<ChangeReceipt>> install(PreparedChange, std::stop_token = {});
    std::future<Result<ChangeReceipt>> edit(EditRequest, std::stop_token = {});
    std::future<Result<ChangeReceipt>> undo(Revision, std::stop_token = {});
    std::future<Result<ChangeReceipt>> redo(Revision, std::stop_token = {});
    std::future<Result<std::shared_ptr<const ReadScope>>> read(Revision, ReadSelection, std::stop_token = {});
    std::future<Result<VariableQuery>> uses(Revision, Variable);
    std::future<Result<TargetQuery>> incoming(Revision, SctActionId);
    std::future<Result<SummaryQuery>> summarize(Revision, SctModuleId, std::stop_token = {});
    std::future<Result<InteractionQuery>> interactions(Revision, SctScriptId);
    std::future<Result<ExplanationQuery>> explain(Revision, decltype(Annotation::subject));
    std::future<Result<Checkpoint>> checkpoint(Revision, std::stop_token = {});
    std::future<Result<ServiceStatus>> markSaved(Revision captured, std::uint64_t historyState);
    std::future<Result<Maintenance>> maintain(Revision, bool discardHistory = false);
    std::future<Result<bool>> shutdown();

  private:
    struct Control;
    std::shared_ptr<Control> control_;
};
} // namespace salsa::core::program
