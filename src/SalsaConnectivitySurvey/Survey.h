#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>
namespace spice::sct {
class SctDocument;
}
namespace salsa::survey {
using Json = nlohmann::json;
inline constexpr int SchemaVersion = 2;
inline constexpr const char *AnalyzerVersion = "sct-mld-connectivity-2";
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
Json extractResource(std::vector<std::byte>, const Json &identity);
Json scriptResourceContext(const spice::sct::SctDocument &);
Json linkResource(const Json &resource, const Json &script, bool details = false);
void indexResources(Json &run, Json &index, const std::filesystem::path &output);
Json queryResources(const Json &run, const Json &index, const std::filesystem::path &output, const std::string &kind,
                    const std::string &value, const std::string &script);
std::string variableName(unsigned kind, std::uint32_t index);
} // namespace salsa::survey
