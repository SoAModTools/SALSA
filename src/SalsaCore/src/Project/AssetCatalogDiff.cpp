#include "SalsaCore/Project/AssetCatalogDiff.h"

namespace salsa::core {

AssetCatalogDelta diffAssetCatalogs(
    const AssetCatalogSnapshot& previous,
    const AssetCatalogSnapshot& current) {
    AssetCatalogDelta delta{};
    std::size_t previousIndex = 0;
    std::size_t currentIndex = 0;

    while (previousIndex < previous.assets.size() && currentIndex < current.assets.size()) {
        const auto& previousAsset = previous.assets[previousIndex];
        const auto& currentAsset = current.assets[currentIndex];

        if (previousAsset.locator < currentAsset.locator) {
            delta.removed.push_back(previousAsset.locator);
            ++previousIndex;
            continue;
        }
        if (currentAsset.locator < previousAsset.locator) {
            delta.added.push_back(currentAsset.locator);
            ++currentIndex;
            continue;
        }

        if (previousAsset.byteSize != currentAsset.byteSize ||
            previousAsset.revision != currentAsset.revision) {
            delta.changed.push_back(currentAsset.locator);
        }
        ++previousIndex;
        ++currentIndex;
    }

    while (previousIndex < previous.assets.size()) {
        delta.removed.push_back(previous.assets[previousIndex].locator);
        ++previousIndex;
    }
    while (currentIndex < current.assets.size()) {
        delta.added.push_back(current.assets[currentIndex].locator);
        ++currentIndex;
    }

    return delta;
}

}  // namespace salsa::core
