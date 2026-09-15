#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Authoring/SctAuthoringIdentity.h"
#include "SalsaCore/Project/ProjectTypes.h"
#include "SalsaCore/Persistence/SctScriptPatch.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"
#include "SpiceSCT/SctDocument.h"
#include "SpiceSCT/SctHeaderContract.h"

#include "smores/script/Relationships.h"
#include <algorithm>
#include <limits>
#include <variant>

namespace salsa::core {

using SctAuthoringEntityId = std::variant<SctScriptId, SctModuleId, SctEntrypointId,
    SctPortId, SctConnectionId, SctContentId, SctBaselineId, SctSequenceId, SctActionId, SctPredicateId,
    SctLanguageGroupId, SctTextResourceId, SctLogicalTextId>;
using SctContentOwner = std::variant<SctScriptId, SctModuleId, SctEntrypointId>;
using SctPortOwner = std::variant<SctModuleId, SctEntrypointId>;

enum class SctEvidenceKind { SourceObservation, Inference, UserAssertion };
struct SctAuthoringEvidence final {
    SctEvidenceKind kind = SctEvidenceKind::SourceObservation;
    std::string description;
};
struct SctUnresolvedBinding final {
    std::string originalEvidence;
    std::string explanation;
    std::vector<SctAuthoringEvidence> evidence;
};
template<class Id> using SctAuthoringReference = std::variant<Id, SctUnresolvedBinding>;

struct SctLegacyOrigin final {
    std::string capsuleId;
    std::string scriptKey;
    std::uint64_t scriptOrdinal = 0;
};

struct SctAuthoringImportRecipe final {
    std::optional<GamePlatform> platform;
    bool trustSelectedTextEncoding = false;
    auto operator<=>(const SctAuthoringImportRecipe&) const = default;
};

// Describes an immutable external baseline, not mutable SCT bytes or an import receipt.
// Imported document identity scopes SPICE IDs; source hash alone does not identify them.
struct SctAuthoringBaseline final {
    SctBaselineId id;
    DatasetFingerprint datasetFingerprint;
    AssetDescriptor source;
    SctImportedDocumentId importedDocument;
    std::optional<spice::sct::SctKnownTextConvention> textConvention;
    std::vector<SctAuthoringEvidence> importEvidence;
    SctAuthoringImportRecipe recipe;
};
struct SctAuthoredOutputProfile final {
    bool bigEndian = true;
    bool compressed = false;
    spice::sct::SctHeaderValues header;
};
struct SctScriptContext final {
    SctScriptId id;
    std::string name;
    SctBaselineId baseline;
    std::optional<GamePlatform> platform;
    std::optional<GameRegion> region;
    std::optional<SctLegacyOrigin> legacyOrigin;
    std::vector<SctAuthoringReference<SctContentId>> contentUses;
    std::optional<SctAuthoredOutputProfile> outputProfile;
    struct ResourceAssociation final {
        smores::script::ResourceIdentity resource;
        smores::script::ResourceRole role = smores::script::ResourceRole::Unclassified;
    };
    std::vector<ResourceAssociation> resources;
};
struct SctAuthoringModule final {
    SctModuleId id;
    SctScriptId script;
    std::string name;
    std::vector<SctAuthoringReference<SctContentId>> contentUses;
    std::vector<SctAuthoringEvidence> evidence;
    // Exclusive ownership of these low-level blocks in the parent's authored
    // program pool. The script retains assembly order across module boundaries.
    std::vector<spice::sct::SctSectionId> sections;
};
enum class SctEntrypointKind { Lifecycle, Arrival, DirectHandler, SignalConsumer, NamedSection };
struct SctNamedActivation final {
    std::string externalName;
    SctActionId body;
};
struct SctLocalActivation final {
    SctActionId source;
    // Absent denotes the source instruction's physical fallthrough edge.
    std::optional<spice::sct::SctParameterAddress> operand;
    SctActionId start;
    SctActionId continuation;
    bool lifecycleEntry = false;
};
using SctEntrypointActivation = std::variant<SctUnresolvedBinding, SctNamedActivation, SctLocalActivation>;
struct SctEntrypointBinding final {
    SctEntrypointKind kind = SctEntrypointKind::Lifecycle;
    SctSequenceId sequence;
    // Existing call or branch operand; absent means the section's first action.
    std::optional<spice::sct::SctParameterSite> responseSite;
    std::optional<spice::sct::SctParameterSite> predicate;
    SctAuthoringReference<SctPortId> response = SctUnresolvedBinding{"Unselected response", "Select a module response.", {}};
    std::optional<std::variant<smores::script::Entry, SctUnresolvedBinding>> provider;
    smores::script::ResourceRole resourceRole = smores::script::ResourceRole::Unclassified;
    std::optional<std::uint32_t> signalBit;
};
struct SctAuthoringEntrypoint final {
    SctEntrypointId id;
    SctScriptId script;
    std::string name;
    std::vector<SctAuthoringReference<SctContentId>> contentUses;
    std::vector<SctAuthoringEvidence> evidence;
    std::optional<SctEntrypointBinding> binding;
    bool enabled = true;
    SctEntrypointActivation activation = SctUnresolvedBinding{"Imported entry", "Activation boundary not selected.", {}};
};
struct SctAuthoringPort final {
    SctPortId id;
    SctPortOwner owner;
    std::string name;
    std::optional<SctActionId> action;
    std::optional<SctSequenceId> sequence;
};
enum class SctTransitionKind { LiteralLoad, Warp, SavedAreaReturn, Battle, ShipBattleExit };
// Captured instruction observations detect later low-level edits. They never replay edits.
enum class SctRouteObservationRole { Source, Arrival, Response, Prerequisite, Requirement, Effect, Continuation };
struct SctRouteObservation final {
    SctScriptId script;
    SctActionId action;
    SctRouteObservationRole role = SctRouteObservationRole::Requirement;
    std::string fingerprint;
};
struct SctExecutableRoute final {
    SctTransitionKind kind = SctTransitionKind::LiteralLoad;
    SctScriptId sourceScript, destinationScript;
    SctActionId sourceAction;
    spice::sct::SctParameterAddress sourceOperand{0, {}};
    SctEntrypointId arrival;
    std::optional<std::uint32_t> arrivalCase;
    std::string sourceStem, targetFilename;
    std::optional<std::uint32_t> discriminatorBit;
    bool discriminatorValue = false;
    std::vector<SctRouteObservation> observations;
    std::optional<SctUnresolvedBinding> unresolved;
};
struct SctAuthoringConnection final {
    SctConnectionId id;
    SctAuthoringReference<SctPortId> source;
    SctAuthoringReference<SctPortId> destination;
    std::vector<SctAuthoringEvidence> evidence;
    std::optional<SctExecutableRoute> route;
};
struct SctWholeDocument final {};
struct SctOrderedSourceSelection final {
    std::vector<spice::sct::SctDocumentEntityId> entities;
};
struct SctPreservedProgramRegion final {
    SctBaselineId baseline;
    SctImportedDocumentId importedDocument;
    std::variant<SctWholeDocument, SctOrderedSourceSelection> coverage;
};
// A single SCPT literal, retaining its exact encoding and termination. Compound
// programs, variable reads, and opaque expressions cannot inhabit this contract.
struct SctLiteralConstant final {
    spice::sct::SctScptValueOperation operation;
    spice::sct::SctExpressionTermination termination = spice::sct::SctExpressionTermination::StopCode;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] spice::sct::SctCanonicalExpression expression() const;
    [[nodiscard]] static std::optional<SctLiteralConstant> fromExpression(const spice::sct::SctCanonicalExpression& expression);
    auto operator<=>(const SctLiteralConstant&) const = default;
};
struct SctPreservedLiteralOverride final {
    spice::sct::SctParameterSite site;
    std::uint16_t opcode = 0;
    SctLiteralConstant baselineValue;
    SctLiteralConstant value;
};
struct SctAuthoringContent final {
    SctContentId id;
    SctContentOwner owner;
    SctPreservedProgramRegion region;
    std::vector<SctAuthoringEvidence> evidence;
    std::vector<SctPreservedLiteralOverride> literalOverrides;
    // Canonical baseline-to-working state, including existing editor metadata.
    // Physical patches and literal overrides are mutually exclusive.
    std::optional<SalsaScriptPatch> physicalPatch;
    // Complete current content; imported bytes and physical patches are not its
    // reconstruction recipe. Low-level entity IDs are stable within this script.
    std::optional<SctSemanticState> authoredState;
};

// Bindings address the current program descended from this immutable import.
struct SctProgramBinding final {
    SctBaselineId baseline;
    SctImportedDocumentId importedDocument;
};
struct SctSequenceAction final {
    SctActionId id;
    spice::sct::SctInstructionId instruction;
};
struct SctAuthoredSequence final {
    SctSequenceId id;
    SctScriptId script;
    std::string name;
    SctProgramBinding binding;
    spice::sct::SctSectionId section;
    std::vector<SctSequenceAction> actions;
};
// The exact program at all uses is the definition's expression authority.
// Binding unrelated uses requires explicit selection and exact agreement.
struct SctNamedPredicate final {
    SctPredicateId id;
    SctScriptId script;
    std::string name;
    SctProgramBinding binding;
    std::vector<spice::sct::SctParameterSite> uses;
};

struct SctLanguageGroup final {
    SctLanguageGroupId id;
    std::string name;
    // Recognition key survives user rename; empty for custom groups.
    std::string recognitionKey;
};
struct SctAuthoredText final {
    SctTextResourceId id;
    SctScriptId script;
    spice::sct::SctTextEntityId target;
    std::string sourceName, sourceGroup, correspondenceKey;
    std::vector<SctLanguageGroupId> languages;
    bool explicitAssignment = false;
    std::vector<SctAuthoringEvidence> evidence;
};
struct SctLogicalText final {
    SctLogicalTextId id;
    SctScriptId script;
    std::string name;
    std::vector<SctTextResourceId> variants;
    std::optional<SctModuleId> module;
};

// Interpretation targets do not own execution content. Physical points are
// qualified by script and imported-document identity, never by an offset/name.
struct SctMeaningTarget final {
    SctScriptId script; // zero permits a project-wide variable definition
    std::optional<SctAuthoringEntityId> entity;
    std::optional<SctVariableKey> variable;
    std::optional<std::int64_t> value;
    std::optional<spice::sct::SctSectionId> section;
    std::optional<spice::sct::SctParameterSite> site;
    std::optional<smores::script::Entry> resource;
    SctImportedDocumentId document;
    auto operator<=>(const SctMeaningTarget&) const = default;
};
enum class SctInterpretationDecision { Suggested, Accepted, Rejected, Superseded };
struct SctExplanationRecord final {
    SctExplanationId id;
    std::string key, rule, ruleVersion, interpretation, reasoning, qualification;
    SctEvidenceKind origin = SctEvidenceKind::Inference;
    SctInterpretationDecision decision = SctInterpretationDecision::Suggested;
    RevisionId revision;
    std::vector<SctMeaningTarget> subjects;
    // Evidence is a captured observation, not a live execution reference.
    std::vector<SctMeaningTarget> evidence;
    std::vector<std::string> observations;
    std::optional<SctExplanationId> supersedes;
};
struct SctMeaning final {
    SctMeaningId id;
    SctMeaningTarget target;
    std::string name;
    std::optional<SctExplanationId> explanation;
};

struct SctStoryStep final {
    SctStoryStepId id;
    SctMeaningTarget target;
    std::string name;
    auto operator<=>(const SctStoryStep&) const = default;
};
struct SctStoryline final {
    SctStorylineId id;
    std::string name;
    std::vector<SctStoryStep> steps; // authored narrative occurrences, not execution
};
struct SctStoryPlacement final {
    SctMeaningTarget target;
    double x = 0, y = 0;
    std::optional<SctStoryStepId> occurrence;
};

// Construction/serialization value. SctAuthoringSession owns accepted live
// revisions; views submit commands rather than maintaining another authority.
struct SctAuthoringProject final {
    SctAuthoringProjectId id;
    RevisionId revision{1};
    std::uint64_t nextEntityId = 1;
    std::vector<SctAuthoringBaseline> baselines;
    std::vector<SctScriptContext> scripts;
    std::vector<SctAuthoringModule> modules;
    std::vector<SctAuthoringEntrypoint> entrypoints;
    std::vector<SctAuthoringPort> ports;
    std::vector<SctAuthoringConnection> connections;
    std::vector<SctAuthoringContent> contents;
    SctWorkspaceAuthoringState workspaceAuthoring;
    std::vector<SctAuthoredSequence> sequences;
    std::vector<SctNamedPredicate> predicates;
    std::vector<SctLanguageGroup> languageGroups;
    std::vector<SctAuthoredText> texts;
    std::vector<SctLogicalText> logicalTexts;
    std::vector<SctStoryline> storylines;
    std::vector<SctMeaning> meanings;
    std::vector<SctExplanationRecord> explanations;

    [[nodiscard]] static Result<SctAuthoringProject> create();
    template<class Tag> [[nodiscard]] Result<SctAuthoringId<Tag>> allocateId() {
        if (nextEntityId == 0 || nextEntityId == std::numeric_limits<std::uint64_t>::max())
            return Result<SctAuthoringId<Tag>>::failure({DiagnosticSeverity::Error,
                DiagnosticCode::InvalidSctAuthoringProject, "Authoring entity ID allocator is exhausted or invalid."});
        return Result<SctAuthoringId<Tag>>::success({nextEntityId++});
    }
    template<class Id> [[nodiscard]] Result<Id> allocate() {
        return allocateId<typename IdTag<Id>::type>();
    }
    [[nodiscard]] const SctAuthoringBaseline* find(SctBaselineId id) const noexcept;
    [[nodiscard]] const SctScriptContext* find(SctScriptId id) const noexcept;
    [[nodiscard]] const SctAuthoringModule* find(SctModuleId id) const noexcept;
    [[nodiscard]] const SctAuthoringEntrypoint* find(SctEntrypointId id) const noexcept;
    [[nodiscard]] const SctAuthoringPort* find(SctPortId id) const noexcept;
    [[nodiscard]] const SctAuthoringConnection* find(SctConnectionId id) const noexcept;
    [[nodiscard]] const SctAuthoringContent* find(SctContentId id) const noexcept;
    [[nodiscard]] const SctAuthoredSequence* find(SctSequenceId id) const noexcept;
    [[nodiscard]] const SctSequenceAction* find(SctActionId id) const noexcept;
    [[nodiscard]] const SctNamedPredicate* find(SctPredicateId id) const noexcept;
    [[nodiscard]] const SctLanguageGroup* find(SctLanguageGroupId id) const noexcept;
    [[nodiscard]] const SctAuthoredText* find(SctTextResourceId id) const noexcept;
    [[nodiscard]] const SctLogicalText* find(SctLogicalTextId id) const noexcept;
    [[nodiscard]] std::optional<SctScriptId> effectiveScript(const SctContentOwner& owner) const noexcept;
    [[nodiscard]] std::vector<Diagnostic> validate() const;
private:
    template<class Id> struct IdTag;
    template<class Tag> struct IdTag<SctAuthoringId<Tag>> { using type = Tag; };
};

struct SctRealizationKey final {
    SctAuthoringProjectId project;
    RevisionId revision;
    SctScriptId script;
    SctRealizationId realization;
    auto operator<=>(const SctRealizationKey&) const = default;
};
struct SctRealizationRecord final {
    SctAuthoringEntityId entity;
    std::vector<spice::sct::SctDiagnosticLocation> locations;
};
struct SctRealizationMap final {
    SctRealizationKey key;
    std::vector<SctRealizationRecord> records;
    [[nodiscard]] bool isFresh(const SctRealizationKey& expected) const noexcept;
};

// Deliberately separate from authored execution state and realization records.
struct SctGraphPlacement final {
    SctAuthoringEntityId entity;
    double x = 0;
    double y = 0;
    bool collapsed = false;
};
struct SctAuthoringPresentation final {
    SctAuthoringProjectId project;
    std::vector<SctGraphPlacement> placements;
    std::vector<SctAuthoringEntityId> selection;
    std::vector<SctStoryPlacement> storyPlacements;
    double storyZoom = 1;
    [[nodiscard]] std::vector<Diagnostic> validate(const SctAuthoringProject& semantic) const;
};

} // namespace salsa::core
