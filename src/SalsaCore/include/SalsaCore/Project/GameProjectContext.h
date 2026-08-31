#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Project/ProjectTypes.h"

namespace salsa::core {

class AssetCatalog {
public:
    virtual ~AssetCatalog() = default;

    [[nodiscard]] virtual const AssetCatalogSnapshot& snapshot() const noexcept = 0;
    [[nodiscard]] virtual Result<SourceAssetSnapshot> loadAsset(
        const AssetLocator& locator) const = 0;
};

class GameProjectContext {
public:
    virtual ~GameProjectContext() = default;

    [[nodiscard]] virtual const DatasetContext& dataset() const noexcept = 0;
    [[nodiscard]] virtual const AssetCatalog& assets() const noexcept = 0;
};

}  // namespace salsa::core
