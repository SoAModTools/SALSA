#pragma once
#include "SalsaCore/Program/NativeImport.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include <future>
#include "SalsaCore/Foundation/BackgroundConcurrency.h"
#include "SalsaCore/Editor/EditorBackend.h"
#include <stop_token>

namespace salsa::core::program {
using Revision = editor::Revision;
using ServiceStatus = editor::SessionStatus;
struct ScriptSummary {
    SctScriptId id;
    std::string name;
    std::size_t sections = 0;
    SctScriptContext context;
    std::optional<AssetLocator> locator;
    std::optional<spice::sct::SctKnownTextConvention> textConvention;
};
struct Catalog {
    ServiceStatus status;
    Statistics storage;
    std::uint64_t childReferences = 0, characterBytes = 0;
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
struct CreateText {
    SctScriptId script;
    std::string name;
    bool supplementary = false;
    spice::sct::SctTextKind kind = spice::sct::SctTextKind::SctString;
    ValueBuffer value;
};
struct RemoveText { SctTextResourceId text; };
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
using MetadataEntity = std::variant<Module, Entrypoint, Port, SctLanguageGroup, Connection,
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
// Recover a shared pure-guard region as independent route-owned predicates.
// Optional bindings associate existing connection records with selected bodies.
struct PromoteDispatch {
    SctActionId header;
    std::vector<std::pair<SctSequenceId, SctConnectionId>> bindings;
};
struct SetRouteRequirement {
    ArmId route;
    ValueBuffer value;
};
// Edits control flow and records the connection in one journal entry.
struct ConnectSlots {
    Connection connection;
    RouteOutcome outcome = RouteOutcome::LocalTransfer;
    std::optional<ValueBuffer> schedule;
};
using SemanticOperation =
    std::variant<InsertConstruct, ReplaceArms, InsertTransfer, MoveAction, EraseAction, RetargetTransfer,
                 PromoteFlow, PutMetadata, EraseMetadata, PutAnnotation, AssignLanguages, BindLifecycle,
                 ScriptSettings, ConnectSlots, PromoteDispatch, SetRouteRequirement>;
using Primitive = std::variant<ReplaceParameter, ReplaceInstruction, ReplaceCondition, ReplaceText, InsertInstruction,
                               EraseInstruction, MoveInstruction, RenameSection, ReorderSections, AddSection,
                               EraseEmptySection, EditAnnotation, CreateText, RemoveText, SemanticOperation>;
struct EditRequest {
    Revision expected;
    std::vector<Primitive> operations;
};
struct ChangeReceipt {
    ServiceStatus status;
    std::vector<SctActionId> actions;
    std::vector<SctSequenceId> sequences;
    std::vector<SectionId> sections;
    std::vector<SctScriptId> scriptLayouts;
    std::vector<SctSequenceId> projectionSequences;
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
    std::vector<SctTextResourceId> texts;
    bool descend = true;
    bool referencedText = true;
    std::vector<SectionId> sections;
    std::vector<ConditionId> conditions;
    std::vector<SctSequenceId> sequences;
};
using EditorSubject = decltype(Annotation::subject);
struct OutlineRequest {
    EditorSubject parent;
    std::size_t offset = 0, limit = 128;
    bool semantic = false;
    std::optional<editor::FlowOccurrence> occurrence;
};
struct OutlineRow {
    EditorSubject subject;
    SctSequenceId sequence;
    std::string label;
    bool expandable = false;
    std::optional<ActionKind> kind;
    std::optional<editor::FlowOccurrence> occurrence;
    std::string groupingIssue;
};
struct OutlinePage {
    ServiceStatus status;
    EditorSubject parent;
    SctScriptId script;
    std::size_t offset = 0, total = 0;
    std::vector<OutlineRow> rows;
    std::uint64_t elapsedMicroseconds = 0;
};
struct FlowAssessment {
    std::optional<SctActionId> promotableContinuation;
    std::string issue;
};
struct NavigationPath { ServiceStatus status; SctScriptId script; std::vector<EditorSubject> subjects; std::vector<std::optional<editor::FlowOccurrence>> occurrences; };
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
    std::vector<Dispatch> dispatches;
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
    std::optional<DispatchMode> dispatchMode; // Logical ordered route edge, not a raw CFG branch.
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
struct CutsceneRouteView {
    SctConnectionId connection;
    std::optional<SctPortId> slot;
    SctActionId source, target;
    std::optional<spice::sct::SctParameterAddress> operand;
    FlowRole role = FlowRole::Unknown;
    EntryMethod method = EntryMethod::Unknown;
    RouteOutcome outcome = RouteOutcome::Observe;
    std::vector<RoutePath> paths;
    std::vector<ValueId> requirementValues; // Path order, local to summary payload.
    std::optional<SctScriptId> destinationScript;
    ValueId schedule;
    bool inferred = true, unresolved = false, changed = false;
    std::vector<SctAuthoringEvidence> evidence;
};
struct CutsceneSummary {
    SctModuleId module;
    std::uint64_t build = 0;
    std::vector<Port> ports;
    std::vector<CutsceneRouteView> incoming, exits;
    std::vector<VariableUse> effects;
    ValueBuffer payload;
    // Includes helper dependencies, but helper returns are not cutscene exits.
    std::vector<SctActionId> actions, entryTargets;
    std::vector<SctSequenceId> sequences;
    std::vector<SectionId> sections;
    std::vector<SctTextResourceId> texts;
    std::vector<std::uint64_t> metadata;
    bool incomplete = false;
    std::vector<Diagnostic> diagnostics;
};
struct CutsceneQuery { ServiceStatus status; std::shared_ptr<const CutsceneSummary> summary; bool reused = false; };
struct InteractionQuery {
    ServiceStatus status;
    std::vector<Entrypoint> entrypoints;
    std::vector<Connection> connections;
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
    std::size_t workers = 0; // Automatic uses the shared background budget.
    std::uint64_t workingByteBudget = 256ull * 1024 * 1024;
    SctLoadingObserver progress;
    std::shared_ptr<BackgroundConcurrency> concurrency; // Null uses the application budget.
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
    std::future<Result<NavigationPath>> locate(Revision, EditorSubject, bool semantic = false);
    std::future<Result<OutlinePage>> outline(Revision, OutlineRequest, std::stop_token = {});
    std::future<Result<editor::BrowsePage>> browse(Revision, editor::BrowseRequest, std::stop_token = {});
    std::future<Result<FlowAssessment>> assessFlow(Revision, SctActionId, std::stop_token = {});
    std::future<Result<VariableQuery>> uses(Revision, Variable);
    std::future<Result<TargetQuery>> incoming(Revision, SctActionId);
    std::future<Result<SummaryQuery>> summarize(Revision, SctModuleId, std::stop_token = {});
    std::future<Result<CutsceneQuery>> cutscene(Revision, SctModuleId, std::stop_token = {});
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
