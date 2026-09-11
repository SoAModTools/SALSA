#pragma once

#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Project/ProjectTypes.h"
#include "SpiceSCT/SctDocument.h"

#include <algorithm>
#include <limits>
#include <variant>

namespace salsa::core {

template<class Tag> struct SctAuthoringId final {
    std::uint64_t value = 0;
    [[nodiscard]] bool valid() const noexcept { return value != 0; }
    auto operator<=>(const SctAuthoringId&) const = default;
};
using SctScriptId = SctAuthoringId<struct AuthoringScriptTag>;
using SctModuleId = SctAuthoringId<struct AuthoringModuleTag>;
using SctEntrypointId = SctAuthoringId<struct AuthoringEntrypointTag>;
using SctPortId = SctAuthoringId<struct AuthoringPortTag>;
using SctConnectionId = SctAuthoringId<struct AuthoringConnectionTag>;
using SctContentId = SctAuthoringId<struct AuthoringContentTag>;
using SctBaselineId = SctAuthoringId<struct AuthoringBaselineTag>;

[[nodiscard]] bool validSctAuthoringUuid(std::string_view value) noexcept;
[[nodiscard]] Result<std::string> generateSctAuthoringUuid();
template<class Tag> struct SctAuthoringUuid final {
    std::string value;
    [[nodiscard]] bool valid() const noexcept { return validSctAuthoringUuid(value); }
    auto operator<=>(const SctAuthoringUuid&) const = default;
};
using SctAuthoringProjectId = SctAuthoringUuid<struct AuthoringProjectTag>;
using SctImportedDocumentId = SctAuthoringUuid<struct ImportedDocumentTag>;
using SctRealizationId = SctAuthoringUuid<struct RealizationTag>;

using SctAuthoringEntityId = std::variant<SctScriptId, SctModuleId, SctEntrypointId,
    SctPortId, SctConnectionId, SctContentId, SctBaselineId>;
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

// Describes an immutable external baseline, not mutable SCT bytes or an import receipt.
// Imported document identity scopes SPICE IDs; source hash alone does not identify them.
struct SctAuthoringBaseline final {
    SctBaselineId id;
    DatasetFingerprint datasetFingerprint;
    AssetDescriptor source;
    SctImportedDocumentId importedDocument;
    std::optional<spice::sct::SctKnownTextConvention> textConvention;
    std::vector<SctAuthoringEvidence> importEvidence;
};
struct SctScriptContext final {
    SctScriptId id;
    std::string name;
    SctBaselineId baseline;
    std::optional<GamePlatform> platform;
    std::optional<GameRegion> region;
    std::optional<SctLegacyOrigin> legacyOrigin;
    std::vector<SctAuthoringReference<SctContentId>> contentUses;
};
struct SctAuthoringModule final {
    SctModuleId id;
    SctScriptId script;
    std::string name;
    std::vector<SctAuthoringReference<SctContentId>> contentUses;
    std::vector<SctAuthoringEvidence> evidence;
};
struct SctAuthoringEntrypoint final {
    SctEntrypointId id;
    SctScriptId script;
    std::string name;
    std::vector<SctAuthoringReference<SctContentId>> contentUses;
    std::vector<SctAuthoringEvidence> evidence;
};
struct SctAuthoringPort final {
    SctPortId id;
    SctPortOwner owner;
    std::string name;
};
struct SctAuthoringConnection final {
    SctConnectionId id;
    SctAuthoringReference<SctPortId> source;
    SctAuthoringReference<SctPortId> destination;
    std::vector<SctAuthoringEvidence> evidence;
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
struct SctAuthoringContent final {
    SctContentId id;
    SctContentOwner owner;
    SctPreservedProgramRegion region;
    std::vector<SctAuthoringEvidence> evidence;
};

// Mutable construction value for S1. Validate before accepting/persisting it.
// Project commands and revision advancement become authoritative in S3.
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
    [[nodiscard]] std::vector<Diagnostic> validate(const SctAuthoringProject& semantic) const;
};

} // namespace salsa::core
