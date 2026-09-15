#pragma once
#include "SalsaCore/Project/ProjectTypes.h"
#include <memory>
namespace salsa::core {
// Owns the exact immutable bytes whose digest was computed at capture time.
class CapturedSource final {
public:
    static Result<CapturedSource> capture(AssetLocator, std::vector<std::byte>);
    const AssetDescriptor &descriptor() const { return snapshot_.descriptor; }
    std::span<const std::byte> bytes() const { return snapshot_.bytes; }

private:
    friend class LocalGameProject;
    explicit CapturedSource(SourceAssetSnapshot snapshot) : snapshot_(std::move(snapshot)) {}
    SourceAssetSnapshot snapshot_;
};
} // namespace salsa::core
