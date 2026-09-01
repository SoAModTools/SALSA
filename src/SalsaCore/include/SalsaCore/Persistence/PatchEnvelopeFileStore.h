#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Persistence/PatchEnvelope.h"

#include <filesystem>
#include <optional>

namespace salsa::core {

class PatchEnvelopeFileStore final {
public:
    [[nodiscard]] Result<std::optional<PatchEnvelope>> load(
        const std::filesystem::path& path) const;

    [[nodiscard]] Result<void> checkpoint(
        const std::filesystem::path& path,
        const PatchEnvelope& envelope) const;
};

}  // namespace salsa::core
