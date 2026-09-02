#pragma once

#include "SalsaCore/Persistence/PatchEnvelopeFileStore.h"
#include "SalsaCore/Persistence/SctScriptPatch.h"
#include "SalsaCore/Project/ProjectTypes.h"

#include <filesystem>
#include <optional>

namespace salsa::core {

struct SalsaWorkspaceDescriptor final {
    std::filesystem::path root;
    std::filesystem::path datasetRoot;
};

class SctPatchStore {
public:
    virtual ~SctPatchStore() = default;
    [[nodiscard]] virtual Result<std::optional<PatchEnvelope>> load(
        const AssetLocator& locator) const = 0;
    [[nodiscard]] virtual Result<void> checkpoint(
        const AssetLocator& locator, const PatchEnvelope& envelope) const = 0;
};

class LocalSalsaWorkspace final : public SctPatchStore {
public:
    [[nodiscard]] static Result<LocalSalsaWorkspace> openOrCreate(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& datasetRoot);

    [[nodiscard]] const SalsaWorkspaceDescriptor& descriptor() const noexcept;
    [[nodiscard]] std::filesystem::path patchPath(const AssetLocator& locator) const;
    [[nodiscard]] Result<std::optional<PatchEnvelope>> load(
        const AssetLocator& locator) const override;
    [[nodiscard]] Result<void> checkpoint(
        const AssetLocator& locator, const PatchEnvelope& envelope) const override;

private:
    explicit LocalSalsaWorkspace(SalsaWorkspaceDescriptor descriptor);
    SalsaWorkspaceDescriptor descriptor_;
    PatchEnvelopeFileStore files_{};
};

}  // namespace salsa::core
