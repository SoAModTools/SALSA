#pragma once

#include "SalsaCore/Foundation/Result.h"

#include <cstddef>
#include <filesystem>
#include <span>

namespace salsa::core {

[[nodiscard]] Result<void> replaceFileAtomically(
    const std::filesystem::path& destination,
    std::span<const std::byte> contents);

}  // namespace salsa::core
