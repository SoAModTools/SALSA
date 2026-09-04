#pragma once

#include "LegacyPickle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace salsa::legacy {

inline constexpr std::string_view CapsuleFormatId = "jahorta.salsa.legacy-capsule";
inline constexpr std::uint32_t CapsuleSchemaVersion = 2;
inline constexpr std::string_view ConverterContractId = "official-final-v7-native-2";

struct ConversionLimits final {
    PickleLimits pickle{};
    std::uint64_t maxScripts = 512;
    std::uint64_t maxSections = 1ull << 16;
    std::uint64_t maxInstructions = 1ull << 20;
    std::uint64_t maxParameters = 1ull << 22;
    std::uint64_t maxLinks = 1ull << 22;
    std::uint64_t maxStrings = 1ull << 18;
    std::uint64_t maxOutputBytes = 4ull << 30;
};

struct ConversionRequest final {
    std::filesystem::path input{};
    std::filesystem::path output{};
    bool retainOriginal = false;
    bool disableResourceLimits = false;
    // Zero selects Auto; explicit values are limited to one through four.
    std::uint32_t scriptWorkers = 0;
    ConversionLimits limits{};
};

struct ConversionOutcome final {
    enum class Status { Ready, ActionRequired, Rejected, Failed };
    Status status = Status::Failed;
    std::string message{};
    std::string capsuleId{};
    std::uint64_t readProjectMilliseconds = 0;
    std::uint64_t normalizeScriptsMilliseconds = 0;
    std::uint64_t analyzeScriptsMilliseconds = 0;
    std::uint64_t encodeScriptsMilliseconds = 0;
    std::uint64_t encodeCpuMilliseconds = 0;
    std::uint64_t compressOutputCpuMilliseconds = 0;
    std::uint64_t convertScriptsMilliseconds = 0;
    std::uint64_t finalizeMilliseconds = 0;
    std::uint64_t totalMilliseconds = 0;
    std::uint32_t requestedScriptWorkers = 0;
    std::uint32_t usedScriptWorkers = 0;
};

using ProgressCallback = std::function<void(std::string_view phase,
    std::uint64_t completed, std::uint64_t total, std::string_view current)>;

[[nodiscard]] inline std::uint32_t resolveScriptWorkerCount(
    const std::uint32_t requested, const std::uint32_t logicalProcessors,
    const std::size_t scriptCount) noexcept {
    if (scriptCount == 0) return 0;
    const auto available = std::max(1u, logicalProcessors);
    const auto selected = requested == 0 ? std::min(4u, available) : requested;
    return static_cast<std::uint32_t>(std::min<std::size_t>(selected, scriptCount));
}

[[nodiscard]] ConversionOutcome convertLegacyProject(
    const ConversionRequest& request,
    const ProgressCallback& progress = {});

}  // namespace salsa::legacy
