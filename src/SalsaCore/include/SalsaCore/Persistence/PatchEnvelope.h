#pragma once

#include "SalsaCore/Project/ProjectTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace salsa::core {

struct PatchAssetExpectation final {
    AssetLocator locator;
    SourceRevision expectedRevision;
};

struct OpaquePatchPayload final {
    std::string type{};
    std::uint32_t schemaVersion = 0;
    std::vector<std::byte> bytes{};
};

struct PatchEnvelope final {
    DatasetFingerprint sourceDatasetFingerprint;
    std::vector<PatchAssetExpectation> affectedAssets{};
    std::vector<AssetLocator> dependencies{};
    OpaquePatchPayload payload;
};

}  // namespace salsa::core
