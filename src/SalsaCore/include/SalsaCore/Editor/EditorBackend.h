#pragma once
#include "SalsaCore/Editor/EditorIdentity.h"
#include "SalsaCore/Foundation/Result.h"
#include <stop_token>
namespace salsa::core::editor {
enum class RowKind { Instruction, Construct };
struct OutlineRow {
    Subject subject;
    SctSequenceId sequence;
    std::string label;
    bool expandable = false;
    std::optional<RowKind> kind;
};
struct OutlineRequest { Subject parent; std::size_t offset = 0, limit = 128; };
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
enum class ParameterKind { Word, Expression, ReadOnly };
struct Parameter {
    ParameterKey key;
    std::string name, value;
    ParameterKind kind = ParameterKind::ReadOnly;
    std::string unavailableReason;
};
struct Inspection {
    Revision revision;
    Subject subject;
    std::string title, description;
    std::vector<Parameter> parameters;
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
};
// Blocking operations run on workers. Each result is qualified by the captured
// revision and owns selected values only, never a live document or storage view.
class Backend {
  public:
    virtual ~Backend() = default;
    virtual Result<OutlinePage> outline(Revision, OutlineRequest, std::stop_token = {}) = 0;
    virtual Result<Inspection> inspect(Revision, Subject, std::stop_token = {}) = 0;
    virtual Result<Change> replaceParameter(Revision, ReplaceParameter, std::stop_token = {}) = 0;
    virtual Result<Change> history(Revision, bool redo, std::stop_token = {}) = 0;
};
}
