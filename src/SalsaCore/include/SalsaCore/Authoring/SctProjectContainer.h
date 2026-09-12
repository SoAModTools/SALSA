#pragma once
#include "SalsaCore/Foundation/Result.h"
#include <map>
#include <span>
#include <string>
#include <vector>

namespace salsa::core {
using SctProjectRecords = std::map<std::string, std::vector<std::byte>>;
// SALSAPRJ v1: little-endian framing, named zlib records, SHA-256 of each
// uncompressed record. Authoring schemas remain versioned JSON inside it.
class SctProjectContainer final {
public:
    static constexpr std::size_t MaximumRecordBytes = 256u << 20;
    static constexpr std::size_t MaximumContainerBytes = 1u << 30;
    [[nodiscard]] static Result<std::vector<std::byte>> encode(const SctProjectRecords& records);
    // Also the diagnostic inspection API: returns exact named, unpacked records.
    [[nodiscard]] static Result<SctProjectRecords> decode(std::span<const std::byte> bytes);
};
} // namespace salsa::core
