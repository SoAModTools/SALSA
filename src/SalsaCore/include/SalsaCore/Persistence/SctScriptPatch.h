#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SalsaCore/Sct/SctDocumentMaterializer.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

class SctPatchStore;

struct SctPatchedScriptSection final {
    spice::sct::SctSectionId section;
    std::optional<std::string> nameBytes{};
    std::vector<spice::sct::SctInstructionId> instructionOrder{};
    std::vector<spice::sct::SctInstructionId> deletedInstructions{};
    std::vector<spice::sct::SctDocumentInstruction> upsertedInstructions{};
};

struct SctPatchedTextValue final {
    SctTextTarget target;
    spice::sct::SctTextValue value;
};

using SctPatchedTextRepair = SctTextRepairRecord;

struct SctPatchedAllocatorState final {
    std::uint64_t nextSectionId = 1;
    std::uint64_t nextInstructionId = 1;
    std::uint64_t nextStringId = 1;
    std::uint64_t nextFooterEntryId = 1;
    std::uint64_t nextOpaqueAttachmentId = 1;
    auto operator<=>(const SctPatchedAllocatorState&) const = default;
};

// A canonical baseline-to-working delta. It is intentionally not the undo journal.
struct SalsaScriptPatch final {
    std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention{};
    std::optional<SctPatchedAllocatorState> allocatorState{};
    std::vector<spice::sct::SctSectionId> sectionOrder{};
    std::vector<spice::sct::SctSectionId> deletedSections{};
    std::vector<spice::sct::SctDocumentSection> insertedSections{};
    std::vector<SctPatchedScriptSection> scriptSections{};
    std::vector<SctPatchedTextValue> textValues{};
    std::vector<spice::sct::SctFooterEntryId> footerOrder{};
    std::vector<spice::sct::SctFooterEntryId> deletedFooterEntries{};
    std::vector<spice::sct::SctDocumentFooterEntry> upsertedFooterEntries{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctPatchedTextRepair> textRepairs{};

    [[nodiscard]] bool empty() const noexcept;
};

struct SctPatchApplication final {
    std::shared_ptr<const spice::sct::SctDocument> document{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctPatchedTextRepair> textRepairs{};
};

class SalsaScriptPatchCodec final {
public:
    static constexpr std::string_view PayloadType = "jahorta.salsa.sct-script-patch";
    static constexpr std::uint32_t SchemaVersion = 2;

    [[nodiscard]] static Result<std::vector<std::byte>> serialize(
        const SalsaScriptPatch& patch);
    [[nodiscard]] static Result<SalsaScriptPatch> deserialize(
        std::span<const std::byte> bytes);
};

class SalsaScriptPatchService final {
public:
    [[nodiscard]] static Result<SalsaScriptPatch> diff(
        const spice::sct::SctDocument& baseline,
        const spice::sct::SctDocument& working,
        std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention,
        std::span<const SctAuthoredArm> authoredArms,
        std::span<const SctPatchedTextRepair> textRepairs);

    [[nodiscard]] static Result<SctPatchApplication> apply(
        const spice::sct::SctDocument& baseline,
        const SalsaScriptPatch& patch);
};

struct SctPatchedLoadResult final {
    SctLoadResult load{};
    std::shared_ptr<const SctDocumentSnapshot> baseline{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctPatchedTextRepair> textRepairs{};
    bool patchApplied = false;
    bool patchConflict = false;
};

struct SctCheckpointRequest final {
    RevisionId revision{};
    std::shared_ptr<const void> historyStateToken{};
    std::shared_ptr<const SctDocumentSnapshot> baseline{};
    SctMaterializationRequest materialization{};
    std::vector<SctPatchedTextRepair> textRepairs{};
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
        const AssetLocator& locator,
        std::stop_token stopToken = {});

    [[nodiscard]] static SctCheckpointResult checkpoint(
        const SctCheckpointRequest& request,
        const SctPatchStore& store,
        std::stop_token stopToken = {});
};

}  // namespace salsa::core
