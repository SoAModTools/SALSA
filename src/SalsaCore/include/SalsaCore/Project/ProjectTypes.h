#pragma once

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Project/AssetLocator.h"

#include <cstdint>
#include <filesystem>
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
