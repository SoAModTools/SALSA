#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Persistence/PatchEnvelope.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace salsa::core {

class PatchEnvelopeCodec final {
public:
    static constexpr std::string_view FormatId = "jahorta.salsa.patch-envelope";
    static constexpr std::string_view Owner = "SALSA";
    static constexpr std::uint32_t SchemaVersion = 1;

    [[nodiscard]] static Result<std::string> serialize(const PatchEnvelope& envelope);
    [[nodiscard]] static Result<PatchEnvelope> deserialize(std::string_view json);
};

}  // namespace salsa::core
