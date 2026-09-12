#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace salsa::survey {
struct ChildResult {
    std::uint32_t exitCode;
    std::string interruption;
    std::uint64_t peakWorkingSetBytes;
    double wallMs;
};
ChildResult runIsolated(const std::filesystem::path &exe, const std::vector<std::wstring> &arguments,
                        std::chrono::milliseconds timeout, const std::atomic_bool &stopped);
} // namespace salsa::survey
