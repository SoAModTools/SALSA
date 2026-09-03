#pragma once

#include "SalsaCore/Persistence/PatchEnvelopeFileStore.h"
#include "SalsaCore/Persistence/SctBaselineStore.h"
#include "SalsaCore/Persistence/SctReconciliationDecisionStore.h"
#include "SalsaCore/Persistence/SctScriptPatch.h"
#include "SalsaCore/Project/ProjectTypes.h"

#include <filesystem>
#include <optional>
#include <string>

namespace salsa::core {

struct SalsaWorkspaceComponents final {
    std::filesystem::path patches;
    std::filesystem::path baselines;
    std::filesystem::path authoring;
    std::filesystem::path imports;
    std::filesystem::path importState;
    std::filesystem::path history;
    std::filesystem::path receipts;
    std::filesystem::path transactions;
    std::filesystem::path session;
    bool operator==(const SalsaWorkspaceComponents&) const = default;
};

struct SalsaWorkspaceDescriptor final {
    std::string workspaceId;
    std::filesystem::path root;
    DatasetContext dataset;
    std::optional<std::filesystem::path> relativeDatasetRoot{};
    std::filesystem::path lastKnownDatasetRoot;
    SalsaWorkspaceComponents components;
};

enum class WorkspaceOpenDisposition {
    Create,
    OpenCompatible,
    RebindRelative,
    UpgradeSchemaOne,
    RecoverSchemaOne,
    RequiresReassociation,
};

struct WorkspaceOpenAssessment final {
    WorkspaceOpenDisposition disposition = WorkspaceOpenDisposition::Create;
    std::filesystem::path workspaceRoot;
    std::optional<std::filesystem::path> storedDatasetRoot{};
    std::string message{};

    [[nodiscard]] bool requiresReassociation() const noexcept {
        return disposition == WorkspaceOpenDisposition::RequiresReassociation;
    }
};

enum class WorkspaceDatasetAcceptance {
    ExactOnly,
    UserConfirmedReassociation,
};

class SctPatchStore {
public:
    virtual ~SctPatchStore() = default;
    [[nodiscard]] virtual Result<std::optional<PatchEnvelope>> load(
        const AssetLocator& locator) const = 0;
    [[nodiscard]] virtual Result<void> checkpoint(
        const AssetLocator& locator, const PatchEnvelope& envelope) const = 0;
};

class LocalSalsaWorkspace final : public SctPatchStore,
                                  public SctBaselineStore,
                                  public SctReconciliationDecisionStore {
public:
    static constexpr std::uint32_t SchemaVersion = 2;

    [[nodiscard]] static Result<WorkspaceOpenAssessment> assess(
        const std::filesystem::path& workspaceRoot,
        const DatasetContext& dataset);
    [[nodiscard]] static Result<LocalSalsaWorkspace> openOrCreate(
        const std::filesystem::path& workspaceRoot,
        const DatasetContext& dataset,
        WorkspaceDatasetAcceptance acceptance = WorkspaceDatasetAcceptance::ExactOnly);

    [[nodiscard]] const SalsaWorkspaceDescriptor& descriptor() const noexcept;
    [[nodiscard]] std::filesystem::path componentPath(
        const std::filesystem::path& relativePath) const;
    [[nodiscard]] std::filesystem::path sessionPath() const;
    [[nodiscard]] std::filesystem::path patchPath(const AssetLocator& locator) const;
    [[nodiscard]] Result<std::optional<PatchEnvelope>> load(
        const AssetLocator& locator) const override;
    [[nodiscard]] Result<void> checkpoint(
        const AssetLocator& locator, const PatchEnvelope& envelope) const override;
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> loadBaseline(
        const SourceRevision& revision) const override;
    [[nodiscard]] Result<void> retainBaseline(
        const SourceRevision& revision, std::span<const std::byte> bytes) const override;
    [[nodiscard]] Result<std::optional<SctReconciliationDecisionArtifact>>
        loadReconciliationDecisions(std::string_view decisionScopeId,
            std::string_view targetScopeKey) const override;
    [[nodiscard]] Result<void> checkpointReconciliationDecisions(
        const SctReconciliationDecisionArtifact& artifact) const override;

private:
    explicit LocalSalsaWorkspace(SalsaWorkspaceDescriptor descriptor);
    SalsaWorkspaceDescriptor descriptor_;
    PatchEnvelopeFileStore files_{};
};

}  // namespace salsa::core
