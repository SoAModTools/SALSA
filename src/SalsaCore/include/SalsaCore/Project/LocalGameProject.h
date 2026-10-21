#pragma once

#include "SalsaCore/Project/GameProjectContext.h"
#include "SalsaCore/Project/CapturedSource.h"

#include <stop_token>

namespace salsa::core {

// Optional per-file work runs while its immutable capture is resident. The scan
// retains only descriptors after the consumer returns, then finalizes identity.
struct DatasetScanPipeline {
    std::size_t workers = 0;
    std::uint64_t workingByteBudget = 256ull * 1024 * 1024;
    std::uint64_t sourceExpansionFactor = 1;
    std::function<Result<void>(std::size_t, const CapturedSource&, std::stop_token)> consume;
};

class LocalGameProject final : public GameProjectContext, public AssetCatalog {
public:
    // Restore recorded source context without reading the extraction. The
    // catalog is historical; rescan/loadAsset explicitly check current files.
    [[nodiscard]] static LocalGameProject fromRecorded(DatasetContext, std::vector<AssetDescriptor>);
    [[nodiscard]] static Result<LocalGameProject> inspect(
        const LocalGameProjectOptions& options,
        std::stop_token stopToken = {},
        const DatasetScanObserver& observer = {},
        const DatasetScanPipeline& pipeline = {});

    [[nodiscard]] Result<LocalGameProject> rescan(
        std::stop_token stopToken = {},
        const DatasetScanObserver& observer = {}) const;

    [[nodiscard]] const DatasetContext& dataset() const noexcept override;
    [[nodiscard]] const AssetCatalog& assets() const noexcept override;
    [[nodiscard]] const AssetCatalogSnapshot& snapshot() const noexcept override;
    [[nodiscard]] Result<SourceAssetSnapshot> loadAsset(
        const AssetLocator& locator) const override;

    // Explicit optional context, qualified at selection time, outside the SCT catalog identity.
    [[nodiscard]] Result<SourceAssetSnapshot> loadContextAsset(const AssetLocator&) const;

private:
    LocalGameProject(
        LocalGameProjectOptions options,
        DatasetContext dataset,
        AssetCatalogSnapshot catalog);

    LocalGameProjectOptions options_{};
    DatasetContext dataset_;
    AssetCatalogSnapshot catalog_;
};

}  // namespace salsa::core
