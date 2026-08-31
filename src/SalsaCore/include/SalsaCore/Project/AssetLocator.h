#pragma once

#include "SalsaCore/Foundation/Result.h"

#include <compare>
#include <filesystem>
#include <string>

namespace salsa::core {

class AssetLocator final {
public:
    [[nodiscard]] static Result<AssetLocator> fromRelativePath(
        const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::string& identityKey() const noexcept;

    [[nodiscard]] bool operator==(const AssetLocator& other) const noexcept;
    [[nodiscard]] std::strong_ordering operator<=>(const AssetLocator& other) const noexcept;

private:
    AssetLocator(std::filesystem::path path, std::string identityKey);

    std::filesystem::path path_{};
    std::string identityKey_{};
};

}  // namespace salsa::core
