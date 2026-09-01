#pragma once

#include "SalsaCore/Project/ProjectTypes.h"

namespace salsa::core {

[[nodiscard]] AssetCatalogDelta diffAssetCatalogs(
    const AssetCatalogSnapshot& previous,
    const AssetCatalogSnapshot& current);

}  // namespace salsa::core
