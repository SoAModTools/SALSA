#pragma once
#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SalsaCore/Editor/AuthoringFeatures.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include <filesystem>
namespace salsa::core::editor {
struct Meaning {
    SctMeaningId id;
    SctScriptId script;
    SctVariableKey variable;
    std::optional<std::int64_t> value;
    std::string name;
    std::vector<SctAuthoringEvidence> evidence;
    bool storyState = false;
};
enum class InterpretationDecision : std::uint8_t { Accepted, Rejected, Superseded };
struct InterpretationRecord {
    Subject subject;
    std::uint64_t revision = 0;
    InterpretationDecision decision = InterpretationDecision::Accepted;
    std::string explanation;
    std::vector<SctAuthoringEvidence> evidence;
};
enum class SlotRole : std::uint8_t { Generic, Entry, Exit };
enum class SlotBoundary : std::uint8_t { Operand, Fallthrough, Terminal, SequenceEnd };
enum class EntryMethod : std::uint8_t {
    Unknown,
    InternalCall,
    Arrival,
    BattleReturn,
    Handler,
    Signal,
    NamedEntry
};
enum class RouteOutcome : std::uint8_t {
    Observe,
    LocalTransfer,
    ReturnToCaller,
    ResumeLoop,
    ScriptTransition,
    ScheduledCall
};
struct SlotItem {
    SctPortId id;
    SctPortOwner owner;
    std::string name;
    std::optional<SctActionId> action;
    std::optional<SctSequenceId> sequence;
    SlotRole role = SlotRole::Entry;
    SlotBoundary boundary = SlotBoundary::SequenceEnd;
    std::optional<spice::sct::SctParameterAddress> operand;
};
struct RouteItem {
    SctConnectionId id;
    std::optional<SctPortId> source, destination;
    EntryMethod method = EntryMethod::Unknown;
    RouteOutcome outcome = RouteOutcome::Observe;
    std::optional<SctEntrypointId> arrival;
    std::optional<program::ArmId> requirement;
    std::optional<SctActionId> implementation;
    std::vector<SctAuthoringEvidence> evidence;
};
struct StoryCatalog {
    Revision revision;
    std::vector<std::pair<SctScriptId, std::string>> scripts;
    std::vector<ModuleItem> modules;
    std::vector<Meaning> meanings;
    std::vector<SlotItem> endpoints;
    std::vector<RouteItem> routes;
    std::vector<SctModuleId> narrative;
};
struct StoryRequest {
    std::vector<SctScriptId> scripts;
    bool unclassifiedBits = false, allEffects = false;
};
struct StoryEffect {
    SctModuleId module;
    SctScriptId script;
    SctVariableKey variable;
    SctActionId action;
    std::string access, expression, meaning;
    bool read = false;
};
struct StoryLink {
    SctModuleId source;
    Subject destination;
    std::optional<SctVariableKey> variable;
    SctActionId writer, reader;
    std::string explanation;
};
struct StoryView {
    Revision revision;
    std::vector<ModuleItem> modules;
    std::vector<StoryEffect> effects;
    std::vector<StoryLink> links;
    std::vector<SctModuleId> narrative;
    std::vector<Diagnostic> diagnostics;
};
struct ConditionItem {
    Subject subject;
    SctActionId owner;
    std::string raw, friendly;
    std::optional<program::ArmId> route;
    std::vector<SctEntrypointId> entrypoints;
};
struct InteractionView {
    Revision revision;
    std::vector<EntryItem> entries;
    std::vector<ConditionItem> conditions;
    std::vector<InterpretationRecord> decisions;
};
struct ItemExplanation {
    Revision revision;
    std::vector<SctAuthoringEvidence> evidence;
    std::vector<InterpretationRecord> decisions;
};
struct RouteDetail {
    Revision revision;
    SctModuleId module;
    std::vector<std::pair<Subject, std::string>> rows;
    std::vector<SctAuthoringEvidence> evidence;
};
struct WriteMeaning {
    Meaning meaning;
};
struct RemoveMeaning {
    SctMeaningId id;
};
struct WriteNarrative {
    std::vector<SctModuleId> modules;
};
struct DecideInterpretation {
    InterpretationRecord record;
};
struct WriteSlot {
    SlotItem slot;
};
struct RemoveSlot {
    SctPortId id;
};
struct ConnectRoute {
    RouteItem route;
    std::optional<spice::sct::SctCanonicalExpression> schedule;
};
struct DetachRoute {
    SctConnectionId id;
}; // Forget the binding, retaining ordinary executable instructions.
struct WriteRouteRequirement {
    program::ArmId route;
    spice::sct::SctCanonicalExpression expression;
};
struct BindRouteRequirement {
    SctConnectionId connection;
    program::ArmId route;
};
struct RecoverDispatch {
    SctActionId header;
};
using StoryEdit =
    std::variant<BindRouteRequirement, WriteMeaning, RemoveMeaning, WriteNarrative, DecideInterpretation,
                 WriteSlot, RemoveSlot, ConnectRoute, DetachRoute, WriteRouteRequirement, RecoverDispatch>;
struct OutputItem {
    SctScriptId script;
    std::string filename, reason;
};
struct OutputReview {
    Revision revision;
    std::vector<SctScriptId> roots;
    std::vector<OutputItem> items;
    std::vector<Diagnostic> diagnostics;
};
struct OutputRequest {
    OutputReview review;
    spice::sct::SctDocumentExportOptions options;
    std::filesystem::path directory;
};
struct OutputResult {
    Revision revision;
    std::vector<std::filesystem::path> files;
};
} // namespace salsa::core::editor
