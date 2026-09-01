#pragma once

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Project/AssetLocator.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace salsa::core {

enum class GamePlatform {
    GameCube,
    Dreamcast,
};

enum class GameRegion {
    NorthAmerica,
    Europe,
    Japan,
};

enum class DatasetScanPhase {
    Discovering,
    Hashing,
};

struct DatasetScanProgress final {
    DatasetScanPhase phase = DatasetScanPhase::Discovering;
    std::size_t completed = 0;
    std::optional<std::size_t> total{};
    std::filesystem::path currentPath{};
};

using DatasetScanObserver = std::function<void(const DatasetScanProgress&)>;

struct SourceRevision final {
    Sha256Digest digest;
    auto operator<=>(const SourceRevision&) const = default;
};

struct DatasetFingerprint final {
    Sha256Digest digest;
    auto operator<=>(const DatasetFingerprint&) const = default;
};

struct DatasetIdentity final {
    std::optional<GamePlatform> platform{};
    std::optional<GameRegion> region{};
    DatasetFingerprint fingerprint;
};

struct DatasetContext final {
    std::filesystem::path root{};
    DatasetIdentity identity;
};

struct AssetDescriptor final {
    AssetLocator locator;
    std::uint64_t byteSize = 0;
    SourceRevision revision;
};

struct AssetCatalogSnapshot final {
    std::vector<AssetDescriptor> assets{};
    DatasetFingerprint fingerprint;
};

struct AssetCatalogDelta final {
    std::vector<AssetLocator> added{};
    std::vector<AssetLocator> removed{};
    std::vector<AssetLocator> changed{};

    [[nodiscard]] bool empty() const noexcept {
        return added.empty() && removed.empty() && changed.empty();
    }
};

struct SourceAssetSnapshot final {
    AssetDescriptor descriptor;
    std::vector<std::byte> bytes{};
};

struct LocalGameProjectOptions final {
    std::filesystem::path datasetRoot{};
    std::optional<GamePlatform> platform{};
    std::optional<GameRegion> region{};
};

}  // namespace salsa::core
