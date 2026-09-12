#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>
namespace salsa::survey {
using Json = nlohmann::json;
inline constexpr int SchemaVersion = 1;
inline constexpr const char *AnalyzerVersion = "sct-connectivity-1";
std::string hash(std::span<const std::byte>);
std::string hash(const std::string &);
std::vector<std::byte> readBytes(const std::filesystem::path &);
void writeText(const std::filesystem::path &, const std::string &);
Json readJson(const std::filesystem::path &);
void writeFacts(const std::filesystem::path &, const Json &);
Json readFacts(const std::filesystem::path &);
Json extract(std::vector<std::byte> bytes, const Json &identity);
Json summarize(const Json &);
Json select(const Json &facts, const std::string &kind, const std::string &value);
std::string variableName(unsigned kind, std::uint32_t index);
} // namespace salsa::survey
