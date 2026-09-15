#pragma once
#include "SalsaCore/Editor/EditorIdentity.h"
#include "SalsaCore/Foundation/Result.h"
#include "SpiceSCT/SctDocument.h"
#include <stop_token>
namespace salsa::core::editor {
enum class ViewMode { Instructions, Semantic };
struct FlowOccurrence {
    SctActionId header;
    bool alternate = false;
    auto operator<=>(const FlowOccurrence &) const = default;
};
enum class RowKind { Instruction, Construct };
struct OutlineRow {
    Subject subject;
    SctSequenceId sequence;
    std::string label;
    bool expandable = false;
    std::optional<RowKind> kind;
    std::optional<FlowOccurrence> occurrence;
};
struct OutlineRequest {
    Subject parent;
    std::size_t offset = 0, limit = 128;
    ViewMode mode = ViewMode::Instructions;
    std::optional<FlowOccurrence> occurrence;
};
struct OutlinePage {
    SessionStatus status;
    Subject parent;
    SctScriptId script;
    std::size_t offset = 0, total = 0;
    std::vector<OutlineRow> rows;
    std::uint64_t elapsedMicroseconds = 0;
};
struct ParameterKey {
    std::uint32_t index = 0;
    std::optional<std::uint32_t> group;
    auto operator<=>(const ParameterKey &) const = default;
};
enum class ParameterKind { Word, Expression, ReadOnly, ActionTarget, IndexedText, FooterText, Words };
struct Parameter {
    ParameterKey key;
    std::string name, value;
    ParameterKind kind = ParameterKind::ReadOnly;
    std::string unavailableReason;
    std::optional<Subject> target;
    std::optional<spice::sct::SctCanonicalExpression> expression;
};
struct Inspection {
    Revision revision;
    Subject subject;
    std::string title, description;
    std::vector<Parameter> parameters;
    SctScriptId script;
    std::optional<SctTextResourceId> text;
    std::optional<spice::sct::SctCanonicalExpression> condition, schedule;
    bool instruction = false, skipRefresh = false, structured = false;
    std::optional<SctActionId> promotableContinuation;
    std::string evidence;
};
struct ReplaceParameter {
    SctActionId action;
    ParameterKey parameter;
    // Parsed according to the existing parameter's domain type. No pool handles.
    std::string text;
};
struct Change {
    SessionStatus status;
    std::vector<SctActionId> actions;
    std::vector<SctSequenceId> sequences;
    std::vector<program::SectionId> sections;
    std::vector<SctScriptId> scriptLayouts;
    bool presentationChanged = false, catalogChanged = false;
    std::vector<Diagnostic> diagnostics;
    std::vector<SctTextResourceId> texts;
    std::optional<Subject> suggestedSelection;
    std::vector<SctSequenceId> projectionSequences;
};
struct NavigationPath {
    Revision revision;
    SctScriptId script;
    std::vector<Subject> subjects;
    std::vector<std::optional<FlowOccurrence>> occurrences;
};
struct TextDraft {
    Revision revision;
    SctTextResourceId id;
    SctScriptId script;
    std::string name;
    bool footer = false;
    spice::sct::SctTextKind kind = spice::sct::SctTextKind::SctString;
    spice::sct::SctTextValue value;
};
struct Opcode {
    std::uint16_t code;
    std::string name;
};
enum class BrowseKind { Opcodes, Variable, Incoming, Texts, Incomplete, Actions };
struct BrowseRequest {
    BrowseKind kind = BrowseKind::Opcodes;
    SctScriptId script;
    std::uint32_t number = 0, bank = 0;
    std::optional<Subject> target;
    std::size_t offset = 0, limit = 128;
};
struct BrowseRow {
    Subject subject;
    std::string label;
};
struct BrowsePage {
    Revision revision;
    std::vector<BrowseRow> rows;
    std::size_t total = 0;
};
struct SetExpression {
    Subject subject;
    std::optional<ParameterKey> parameter;
    spice::sct::SctCanonicalExpression value;
};
struct BindReference {
    SctActionId action;
    ParameterKey parameter;
    Subject target;
};
struct SetTiming {
    SctActionId action;
    bool skipRefresh = false;
    std::optional<spice::sct::SctCanonicalExpression> schedule;
};
struct WriteText {
    TextDraft draft;
};
struct CreateText {
    SctScriptId script;
    std::string name;
    bool footer = false;
    spice::sct::SctTextKind kind = spice::sct::SctTextKind::SctString;
    spice::sct::SctTextValue value;
};
struct DeleteItem {
    Subject subject;
};
struct InsertOpcode {
    Subject anchor;
    std::uint16_t opcode = 12;
};
struct InsertStructure {
    Subject anchor;
    std::string kind;
    spice::sct::SctCanonicalExpression condition;
};
struct InsertTransfer {
    Subject anchor;
    SctActionId target;
    bool continueLoop = false;
};
struct ArmDraft {
    program::ArmId id;
    std::optional<spice::sct::SctCanonicalExpression> condition;
    std::vector<std::int32_t> labels;
    bool includesDefault = false, fallthrough = false;
};
struct StructureDraft {
    Revision revision;
    SctActionId action;
    bool switchCases = false;
    std::vector<ArmDraft> arms;
};
struct WriteArms {
    StructureDraft draft;
};
struct PromoteFlow {
    SctActionId header, continuation;
};
using EditIntent = std::variant<SetExpression, BindReference, SetTiming, WriteText, CreateText, DeleteItem,
                                InsertOpcode, InsertStructure, InsertTransfer, WriteArms, PromoteFlow>;
// Blocking operations run on workers. Each result is qualified by the captured
// revision and owns selected values only, never a live document or storage view.
class Backend {
  public:
    virtual ~Backend() = default;
    virtual Result<OutlinePage> outline(Revision, OutlineRequest, std::stop_token = {}) = 0;
    virtual Result<Inspection> inspect(Revision, Subject, std::stop_token = {}) = 0;
    virtual Result<Change> replaceParameter(Revision, ReplaceParameter, std::stop_token = {}) = 0;
    virtual Result<NavigationPath> locate(Revision, Subject, ViewMode = ViewMode::Instructions) = 0;
    virtual Result<TextDraft> text(Revision, SctTextResourceId, std::stop_token = {}) = 0;
    virtual Result<StructureDraft> structure(Revision, SctActionId, std::stop_token = {}) = 0;
    virtual Result<BrowsePage> browse(Revision, BrowseRequest, std::stop_token = {}) = 0;
    virtual std::vector<Opcode> opcodes() const = 0;
    virtual Result<Change> execute(Revision, EditIntent, std::stop_token = {}) = 0;
    virtual Result<Change> history(Revision, bool redo, std::stop_token = {}) = 0;
};
} // namespace salsa::core::editor
