#pragma once

#include "SalsaCore/Project/GameProjectContext.h"

#include <stop_token>

namespace salsa::core {

class LocalGameProject final : public GameProjectContext, public AssetCatalog {
public:
    [[nodiscard]] static Result<LocalGameProject> inspect(
        const LocalGameProjectOptions& options,
        std::stop_token stopToken = {});

    [[nodiscard]] Result<LocalGameProject> rescan(
        std::stop_token stopToken = {}) const;

    [[nodiscard]] const DatasetContext& dataset() const noexcept override;
    [[nodiscard]] const AssetCatalog& assets() const noexcept override;
    [[nodiscard]] const AssetCatalogSnapshot& snapshot() const noexcept override;
    [[nodiscard]] Result<SourceAssetSnapshot> loadAsset(
        const AssetLocator& locator) const override;

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
