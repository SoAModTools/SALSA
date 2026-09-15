#include "SalsaCore/Project/CapturedSource.h"
namespace salsa::core {
Result<CapturedSource> CapturedSource::capture(AssetLocator locator, std::vector<std::byte> bytes) {
    auto hash = sha256(bytes);
    if (!hash)
        return Result<CapturedSource>::failure(hash.diagnostics());
    AssetDescriptor descriptor{std::move(locator), bytes.size(), {std::move(hash).takeValue()}};
    return Result<CapturedSource>::success(CapturedSource{{std::move(descriptor), std::move(bytes)}});
}
} // namespace salsa::core
