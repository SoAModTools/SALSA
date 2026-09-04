#pragma once

#include "SalsaCore/Foundation/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace salsa::core {

inline constexpr std::string_view LegacyCapsuleFormatId =
    "jahorta.salsa.legacy-capsule";
inline constexpr std::uint32_t LegacyCapsuleSchemaVersion = 2;
inline constexpr std::string_view LegacyConverterContractId =
    "official-final-v7-native-2";

enum class LegacyCapsuleStatus {
    Ready,
    ActionRequired,
};

enum class LegacyScriptStatus {
    Accepted,
    Failed,
};

struct LegacyScriptSummary final {
    std::uint32_t ordinal = 0;
    std::string key{};
    std::string storedName{};
    LegacyScriptStatus status = LegacyScriptStatus::Failed;
    std::uint64_t sectionCount = 0;
    std::uint64_t instructionCount = 0;
    std::uint64_t parameterCount = 0;
    std::uint64_t stringCount = 0;
    std::uint64_t diagnosticCount = 0;
    std::filesystem::path recordRelativePath{};
    std::uint64_t compressedSize = 0;
    std::string compressedSha256{};
};

struct LegacyCapsuleSource final {
    std::string filename{};
    std::uint64_t size = 0;
    std::string sha256{};
    std::uint32_t pickleProtocol = 0;
    std::uint32_t projectVersion = 0;
    bool originalRetained = false;
};

struct LegacyCapsuleSummary final {
    std::filesystem::path root{};
    std::string capsuleId{};
    std::string converterContractId{};
    LegacyCapsuleStatus status = LegacyCapsuleStatus::Ready;
    LegacyCapsuleSource source{};
    std::uint64_t normalizationCount = 0;
    std::vector<LegacyScriptSummary> scripts{};
};

struct LegacyCapsuleValidationLimits final {
    std::uint64_t maxFiles = 1'024;
    std::uint64_t maxCanonicalBytes = 4ull << 30;
    std::uint64_t maxRecordBytes = 256ull << 20;
    std::uint64_t maxNodesPerRecord = 16ull << 20;
};

class LegacyCapsuleReader final {
public:
    [[nodiscard]] static Result<LegacyCapsuleSummary> validate(
        const std::filesystem::path& root,
        const LegacyCapsuleValidationLimits& limits = {});
};

}  // namespace salsa::core
