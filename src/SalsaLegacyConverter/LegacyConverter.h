#pragma once

#include "LegacyPickle.h"

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
    ConversionLimits limits{};
};

struct ConversionOutcome final {
    enum class Status { Ready, ActionRequired, Rejected, Failed };
    Status status = Status::Failed;
    std::string message{};
    std::string capsuleId{};
};

using ProgressCallback = std::function<void(std::string_view phase,
    std::uint64_t completed, std::uint64_t total, std::string_view current)>;

[[nodiscard]] ConversionOutcome convertLegacyProject(
    const ConversionRequest& request,
    const ProgressCallback& progress = {});

}  // namespace salsa::legacy
