#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SalsaCore/Sct/SctDocumentMaterializer.h"
#include "SpiceSCT/SctParser.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

class SctPatchStore;
class SctBaselineStore;

template<typename T>
struct SctValueDelta final {
    std::optional<T> before{};
    std::optional<T> after{};
};

template<typename T>
struct SctOrderDelta final {
    std::vector<T> before{};
    std::vector<T> after{};
};

struct SctPatchedInstruction final {
    std::optional<spice::sct::SctDocumentInstruction> before{};
    std::optional<spice::sct::SctDocumentInstruction> after{};
};

struct SctPatchedScriptSection final {
    spice::sct::SctSectionId section;
    std::optional<SctValueDelta<std::string>> nameBytes{};
    std::optional<SctOrderDelta<spice::sct::SctInstructionId>> instructionOrder{};
    std::vector<SctPatchedInstruction> instructions{};
};

struct SctPatchedTextValue final {
    SctTextTarget target;
    spice::sct::SctTextValue beforeValue;
    spice::sct::SctTextValue afterValue;
};

using SctPatchedTextRepair = SctTextRepairRecord;

struct SctPatchedAllocatorState final {
    std::uint64_t nextSectionId = 1;
    std::uint64_t nextInstructionId = 1;
    std::uint64_t nextStringId = 1;
    std::uint64_t nextSupplementaryTextId = 1;
    std::uint64_t nextOpaqueAttachmentId = 1;
    auto operator<=>(const SctPatchedAllocatorState&) const = default;
};

struct SctPatchedTextRepairDelta final {
    SctTextTarget target;
    std::optional<SctTextRepairProvenance> before{};
    std::optional<SctTextRepairProvenance> after{};
};

struct SctPatchedUnboundReferenceDelta final {
    spice::sct::SctParameterSite site;
    std::optional<SctUnboundReferenceOrigin> before{};
    std::optional<SctUnboundReferenceOrigin> after{};
};

struct SctSemanticState final {
    std::shared_ptr<const spice::sct::SctDocument> document{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctPatchedTextRepair> textRepairs{};
    std::vector<SctUnboundReferenceOrigin> unboundReferences{};
    std::vector<SctVariableAlias> aliases{};
    std::vector<SctEntityAnnotation> annotations{};
    std::vector<SctSectionFolder> folders{};
};

// A canonical baseline-to-working delta. It is intentionally not the undo journal.
struct SalsaScriptPatch final {
    std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention{};
    std::optional<SctValueDelta<SctPatchedAllocatorState>> allocatorState{};
    std::optional<SctOrderDelta<spice::sct::SctSectionId>> sectionOrder{};
    std::vector<SctValueDelta<spice::sct::SctDocumentSection>> sections{};
    std::vector<SctPatchedScriptSection> scriptSections{};
    std::vector<SctPatchedTextValue> textValues{};
    std::optional<SctOrderDelta<spice::sct::SctSupplementaryTextId>> supplementaryTextOrder{};
    std::vector<SctValueDelta<spice::sct::SctDocumentSupplementaryText>> supplementaryText{};
    std::vector<SctValueDelta<SctAuthoredArm>> authoredArms{};
    std::vector<SctPatchedTextRepairDelta> textRepairs{};
    std::vector<SctPatchedUnboundReferenceDelta> unboundReferences{};
    std::vector<SctValueDelta<SctVariableAlias>> aliases{};
    std::vector<SctValueDelta<SctEntityAnnotation>> annotations{};
    std::vector<SctValueDelta<SctSectionFolder>> folders{};

    [[nodiscard]] bool empty() const noexcept;
};

struct SctPatchApplication final {
    std::shared_ptr<const spice::sct::SctDocument> document{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctPatchedTextRepair> textRepairs{};
    std::vector<SctUnboundReferenceOrigin> unboundReferences{};
    std::vector<SctVariableAlias> aliases{};
    std::vector<SctEntityAnnotation> annotations{};
    std::vector<SctSectionFolder> folders{};
};

class SalsaScriptPatchCodec final {
public:
    static constexpr std::string_view PayloadType = "jahorta.salsa.sct-script-patch";
    static constexpr std::uint32_t SchemaVersion = 6;
    static constexpr std::uint32_t LegacySchemaVersion = 5;

    [[nodiscard]] static Result<std::vector<std::byte>> serialize(
        const SalsaScriptPatch& patch);
    [[nodiscard]] static Result<SalsaScriptPatch> deserialize(
        std::span<const std::byte> bytes);
};

class SalsaScriptPatchService final {
public:
    [[nodiscard]] static Result<SalsaScriptPatch> diff(
        const SctSemanticState& baseline,
        const SctSemanticState& working,
        std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention);

    [[nodiscard]] static Result<SctSemanticState> apply(
        const SctSemanticState& baseline,
        const SalsaScriptPatch& patch);
};

struct SctPatchedLoadResult final {
    SctLoadResult load{};
    std::shared_ptr<const SctDocumentSnapshot> baseline{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctPatchedTextRepair> textRepairs{};
    std::vector<SctUnboundReferenceOrigin> unboundReferences{};
    std::vector<SctVariableAlias> aliases{};
    std::vector<SctEntityAnnotation> annotations{};
    std::vector<SctSectionFolder> folders{};
    bool patchApplied = false;
    bool patchConflict = false;
};

struct SctCheckpointRequest final {
    RevisionId revision{};
    std::shared_ptr<const void> historyStateToken{};
    std::shared_ptr<const SctDocumentSnapshot> baseline{};
    SctMaterializationRequest materialization{};
    std::vector<SctPatchedTextRepair> textRepairs{};
    std::vector<SctUnboundReferenceOrigin> unboundReferences{};
    std::vector<SctVariableAlias> aliases{};
    std::vector<SctEntityAnnotation> annotations{};
    std::vector<SctSectionFolder> folders{};
};

struct SctCheckpointResult final {
    RevisionId revision{};
    std::shared_ptr<const void> historyStateToken{};
    std::vector<Diagnostic> diagnostics{};
    bool saved = false;
    bool cancelled = false;
};

class SctPatchCheckpointService final {
public:
    [[nodiscard]] static SctPatchedLoadResult load(
        const GameProjectContext& project,
        const SctPatchStore* store,
        const SctBaselineStore* baselines,
        const AssetLocator& locator,
        std::stop_token stopToken = {},
        spice::sct::SctParseTraceObserver traceObserver = {});

    [[nodiscard]] static SctCheckpointResult checkpoint(
        const SctCheckpointRequest& request,
        const SctPatchStore& store,
        const SctBaselineStore& baselines,
        std::stop_token stopToken = {});
};

}  // namespace salsa::core
