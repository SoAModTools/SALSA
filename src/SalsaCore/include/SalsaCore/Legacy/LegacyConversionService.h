#pragma once

#include "SalsaCore/Legacy/LegacyCapsule.h"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

namespace salsa::core {

enum class LegacyConversionPhase {
    Staging,
    Hashing,
    Parsing,
    Script,
    Writing,
    Validating,
    Finalizing,
};

struct LegacyConversionProgress final {
    LegacyConversionPhase phase = LegacyConversionPhase::Staging;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::string current{};
};

using LegacyConversionObserver = std::function<void(const LegacyConversionProgress&)>;

struct LegacyConversionRequest final {
    std::filesystem::path source{};
    std::filesystem::path destination{};
    std::filesystem::path converterExecutable{};
    std::optional<std::filesystem::path> receiptDirectory{};
    bool trustedInputConfirmed = false;
    bool retainOriginal = false;
    bool disableResourceLimits = false;
    // Zero selects Auto; explicit values are limited to one through four.
    std::uint32_t scriptWorkers = 0;
};

// Returns the normal converter Job Object ceiling for an installed-memory size.
// A zero size represents failed discovery and selects the documented fallback.
[[nodiscard]] std::uint64_t legacyConverterMemoryLimit(
    std::uint64_t installedPhysicalBytes) noexcept;

enum class LegacyConversionStatus {
    Ready,
    ActionRequired,
    Rejected,
    Cancelled,
    Failed,
};

struct LegacyConversionResult final {
    LegacyConversionStatus status = LegacyConversionStatus::Failed;
    std::optional<LegacyCapsuleSummary> capsule{};
    std::vector<Diagnostic> diagnostics{};

    [[nodiscard]] bool finalized() const noexcept {
        return status == LegacyConversionStatus::Ready
            || status == LegacyConversionStatus::ActionRequired;
    }
};

class LegacyConversionService final {
public:
    [[nodiscard]] static LegacyConversionResult convert(
        const LegacyConversionRequest& request,
        std::stop_token stop = {},
        const LegacyConversionObserver& observer = {});
};

}  // namespace salsa::core
