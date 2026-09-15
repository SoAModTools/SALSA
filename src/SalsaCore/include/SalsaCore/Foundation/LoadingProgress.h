#pragma once
#include <cstdint>
#include <functional>
#include <string>
namespace salsa::core {
struct SctLoadingProgress final {
    std::string phase;
    std::uint64_t completed = 0, total = 0;
    std::string item;
    bool cancellable = true;
};
using SctLoadingObserver = std::function<void(const SctLoadingProgress&)>;
} // namespace salsa::core
