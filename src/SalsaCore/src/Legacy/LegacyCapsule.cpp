#include "SalsaCore/Legacy/LegacyCapsule.h"

#include "SalsaCore/Foundation/Hashing.h"

#include <Windows.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string_view>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;
using namespace std::string_view_literals;

[[nodiscard]] Diagnostic capsuleError(std::string message,
    const std::filesystem::path& path,
    const DiagnosticCode code = DiagnosticCode::LegacyCapsuleInvalid) {
    return {DiagnosticSeverity::Error, code, std::move(message), path};
}

[[nodiscard]] bool hasExactKeys(const Json& object,
    const std::initializer_list<std::string_view> keys) {
    return object.is_object() && object.size() == keys.size()
        && std::ranges::all_of(keys, [&object](const auto key) {
            return object.contains(std::string(key));
        });
}

[[nodiscard]] bool validHexDigest(const std::string_view value) {
    return value.size() == 64u && std::ranges::all_of(value, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] bool validRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()
        || path.lexically_normal() != path) return false;
    for (const auto& part : path)
        if (part.empty() || part == L"." || part == L"..") return false;
    return true;
}

[[nodiscard]] std::filesystem::path fromUtf8(const std::string_view text) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

[[nodiscard]] Result<std::vector<std::byte>> readBounded(
    const std::filesystem::path& path, const std::uint64_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum || size > std::numeric_limits<std::size_t>::max())
        return Result<std::vector<std::byte>>::failure(capsuleError(
            error ? "A capsule file size could not be read."
                  : "A capsule file exceeds its validation limit.", path));
    std::ifstream input(path, std::ios::binary);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::ifstream::traits_type::eof())
        return Result<std::vector<std::byte>>::failure(capsuleError(
            "A capsule file could not be read completely.", path));
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] bool isRegularWithoutReparsePoint(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

[[nodiscard]] Result<Json> parseJsonFile(const std::filesystem::path& path,
    const std::uint64_t maximum) {
    auto bytes = readBounded(path, maximum);
    if (!bytes) return Result<Json>::failure(bytes.diagnostics());
    try {
        const auto* begin = reinterpret_cast<const char*>(bytes.value().data());
        return Result<Json>::success(Json::parse(begin, begin + bytes.value().size()));
    } catch (const std::exception&) {
        return Result<Json>::failure(capsuleError(
            "A capsule JSON file is malformed.", path));
    }
}

[[nodiscard]] Result<Json> parseCborFile(const std::filesystem::path& path,
    const std::uint64_t maximum) {
    auto bytes = readBounded(path, maximum);
    if (!bytes) return Result<Json>::failure(bytes.diagnostics());
    try {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
        if (path.extension() != L".zlib")
            return Result<Json>::success(Json::from_cbor(begin,
                begin + bytes.value().size(), true, true));
        LodePNGDecompressSettings settings = lodepng_default_decompress_settings;
        settings.max_output_size = static_cast<std::size_t>(std::min<std::uint64_t>(
            maximum, std::numeric_limits<std::size_t>::max()));
        std::vector<unsigned char> decoded;
        if (lodepng::decompress(decoded, begin, bytes.value().size(), settings) != 0
            || decoded.size() > maximum)
            return Result<Json>::failure(capsuleError(
                "A capsule CBOR record could not be decompressed within its limit.", path));
        return Result<Json>::success(Json::from_cbor(decoded.begin(),
            decoded.end(), true, true));
    } catch (const std::exception&) {
        return Result<Json>::failure(capsuleError(
            "A capsule CBOR record is malformed.", path));
    }
}

[[nodiscard]] Result<std::vector<unsigned char>> readCompressedRecord(
    const std::filesystem::path& path, const std::uint64_t maximum,
    const std::uint64_t declaredSize, const std::string_view expectedDigest) {
    auto bytes = readBounded(path, maximum);
    if (!bytes) return Result<std::vector<unsigned char>>::failure(bytes.diagnostics());
    if (bytes.value().size() != declaredSize)
        return Result<std::vector<unsigned char>>::failure(capsuleError(
            "A capsule entry size does not match its manifest.", path,
            DiagnosticCode::LegacyCapsuleIntegrityFailed));
    auto digest = sha256(bytes.value());
    if (!digest || digest.value().toHex() != expectedDigest)
        return Result<std::vector<unsigned char>>::failure(capsuleError(
            "A capsule entry hash does not match its manifest.", path,
            DiagnosticCode::LegacyCapsuleIntegrityFailed));
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
    LodePNGDecompressSettings settings = lodepng_default_decompress_settings;
    settings.max_output_size = static_cast<std::size_t>(std::min<std::uint64_t>(
        maximum, std::numeric_limits<std::size_t>::max()));
    std::vector<unsigned char> decoded;
    if (lodepng::decompress(decoded, begin, bytes.value().size(), settings) != 0
        || decoded.size() > maximum)
        return Result<std::vector<unsigned char>>::failure(capsuleError(
            "A capsule CBOR record could not be decompressed within its limit.", path));
    return Result<std::vector<unsigned char>>::success(std::move(decoded));
}

[[nodiscard]] Result<std::string> hashFileBounded(const std::filesystem::path& path,
    const std::uint64_t maximum, const std::uint64_t declaredSize) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != declaredSize || size > maximum)
        return Result<std::string>::failure(capsuleError(
            error ? "A capsule file size could not be read."
                  : size != declaredSize
                    ? "A capsule entry size does not match its manifest."
                    : "A capsule file exceeds its validation limit.", path,
            size != declaredSize ? DiagnosticCode::LegacyCapsuleIntegrityFailed
                                 : DiagnosticCode::LegacyCapsuleInvalid));
    auto created = Sha256Hasher::create();
    if (!created) return Result<std::string>::failure(created.diagnostics());
    auto hasher = std::move(created).takeValue();
    std::ifstream input(path, std::ios::binary);
    std::array<std::byte, 64u << 10> buffer{};
    std::uint64_t readTotal = 0;
    while (readTotal < size) {
        const auto remaining = size - readTotal;
        const auto requested = static_cast<std::streamsize>(std::min<std::uint64_t>(
            remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()), requested);
        const auto count = input.gcount();
        if (count <= 0) return Result<std::string>::failure(capsuleError(
            "A capsule file could not be read completely.", path));
        auto updated = hasher.update(std::span{buffer.data(), static_cast<std::size_t>(count)});
        if (!updated) return Result<std::string>::failure(updated.diagnostics());
        readTotal += static_cast<std::uint64_t>(count);
    }
    auto digest = hasher.finish();
    if (!digest) return Result<std::string>::failure(digest.diagnostics());
    return Result<std::string>::success(digest.value().toHex());
}

[[nodiscard]] Result<void> verifyEntry(const std::filesystem::path& root,
    const Json& entry, const LegacyCapsuleValidationLimits& limits,
    std::uint64_t& canonicalBytes, std::set<std::filesystem::path>& expected,
    const bool verifyContents) {
    if (!hasExactKeys(entry, {"path", "size", "sha256", "canonical"})
        || !entry.at("path").is_string() || !entry.at("size").is_number_unsigned()
        || !entry.at("sha256").is_string() || !entry.at("canonical").is_boolean())
        return Result<void>::failure(capsuleError(
            "A capsule entry descriptor is malformed.", root / L"capsule.json"));
    const auto relative = fromUtf8(entry.at("path").get<std::string>());
    const auto digest = entry.at("sha256").get<std::string>();
    const auto declaredSize = entry.at("size").get<std::uint64_t>();
    if (!validRelativePath(relative) || !validHexDigest(digest)
        || _wcsicmp(relative.c_str(), L"capsule.json") == 0)
        return Result<void>::failure(capsuleError(
            "A capsule entry path or digest is invalid.", root / L"capsule.json"));
    auto folded = relative;
    auto foldedText = folded.generic_wstring();
    std::ranges::transform(foldedText, foldedText.begin(), towlower);
    if (!expected.insert(std::filesystem::path(foldedText)).second)
        return Result<void>::failure(capsuleError(
            "Capsule entry paths collide case-insensitively.", root / relative));
    const auto absolute = root / relative;
    if (!isRegularWithoutReparsePoint(absolute)) return Result<void>::failure(capsuleError(
        "A capsule entry is missing, not regular, or is a reparse point.", absolute));
    if (verifyContents) {
        auto actual = hashFileBounded(absolute, limits.maxCanonicalBytes, declaredSize);
        if (!actual) return Result<void>::failure(actual.diagnostics());
        if (actual.value() != digest)
            return Result<void>::failure(capsuleError(
                "A capsule entry hash does not match its manifest.", absolute,
                DiagnosticCode::LegacyCapsuleIntegrityFailed));
    } else {
        std::error_code error;
        const auto size = std::filesystem::file_size(absolute, error);
        if (error || size != declaredSize)
            return Result<void>::failure(capsuleError(
                "A capsule entry size does not match its manifest.", absolute,
                DiagnosticCode::LegacyCapsuleIntegrityFailed));
    }
    if (entry.at("canonical").get<bool>()) {
        if (canonicalBytes > limits.maxCanonicalBytes - declaredSize)
            return Result<void>::failure(capsuleError(
                "The capsule exceeds the canonical data limit.", root));
        canonicalBytes += declaredSize;
    }
    return Result<void>::success();
}

[[nodiscard]] bool validScriptPath(const std::string_view path,
    const std::uint32_t ordinal) {
    std::ostringstream expected;
    expected << "scripts/" << std::setw(6) << std::setfill('0') << ordinal
        << ".cbor.zlib";
    return path == expected.str();
}

[[nodiscard]] bool isNumberedScriptPath(const std::string_view path) {
    constexpr std::string_view prefix = "scripts/";
    constexpr std::string_view suffix = ".cbor.zlib";
    if (!path.starts_with(prefix) || !path.ends_with(suffix)
        || path.size() != prefix.size() + 6u + suffix.size()) return false;
    const auto digits = path.substr(prefix.size(), 6u);
    return std::ranges::all_of(digits, [](const char c) { return c >= '0' && c <= '9'; });
}

[[nodiscard]] Result<void> validateInertGraphs(const std::vector<const Json*>& roots,
    const std::filesystem::path& path, std::uint64_t maximumNodes);

[[nodiscard]] Result<Json> validateProjectRecord(const std::filesystem::path& root,
    const LegacyCapsuleValidationLimits& limits) {
    auto parsed = parseCborFile(root / L"project.cbor", limits.maxRecordBytes);
    if (!parsed) return parsed;
    const auto& record = parsed.value();
    if (!hasExactKeys(record, {"formatId", "schemaVersion", "fieldDispositions",
            "fields", "normalizations", "diagnostics"})
        || record.at("formatId") != "jahorta.salsa.legacy-project-record"
        || !record.at("schemaVersion").is_number_unsigned()
        || record.at("schemaVersion").get<std::uint64_t>() != 2
        || !record.at("fieldDispositions").is_object()
        || !hasExactKeys(record.at("fields"), {"global_variables", "version", "inst_id_colors"})
        || !record.at("normalizations").is_array()
        || !record.at("diagnostics").is_array())
        return Result<Json>::failure(capsuleError(
            "The capsule project record is malformed.", root / L"project.cbor"));
    std::vector<const Json*> fieldRoots;
    for (auto iterator = record.at("fields").begin(); iterator != record.at("fields").end(); ++iterator)
        fieldRoots.push_back(&iterator.value());
    auto fields = validateInertGraphs(fieldRoots, root / L"project.cbor",
        limits.maxNodesPerRecord);
    if (!fields) return Result<Json>::failure(fields.diagnostics());
    return parsed;
}

[[nodiscard]] bool allowedLegacyClass(const std::string_view name) {
    static constexpr std::array allowed{
        "SALSA.Project.project_container.SCTProject",
        "SALSA.Project.project_container.SCTScript",
        "SALSA.Project.project_container.SCTSection",
        "SALSA.Project.project_container.SCTInstruction",
        "SALSA.Project.project_container.SCTParameter",
        "SALSA.Project.project_container.SCTLink",
        "builtins.bytearray",
    };
    return std::ranges::find(allowed, name) != allowed.end();
}

[[nodiscard]] Result<void> validateInertGraphs(const std::vector<const Json*>& roots,
    const std::filesystem::path& path, const std::uint64_t maximumNodes) {
    std::vector<const Json*> pending;
    for (auto iterator = roots.rbegin(); iterator != roots.rend(); ++iterator)
        pending.push_back(*iterator);
    std::uint64_t nextNodeId = 1;
    while (!pending.empty()) {
        const auto* node = pending.back(); pending.pop_back();
        if (!node->is_array() || node->empty() || !(*node)[0].is_number_unsigned())
            return Result<void>::failure(capsuleError(
                "A tagged legacy value is malformed.", path));
        const auto tag = (*node)[0].get<std::uint64_t>();
        if (tag == 0u) {
            if (node->size() != 2u || !(*node)[1].is_number_unsigned())
                return Result<void>::failure(capsuleError(
                    "A legacy graph reference is invalid.", path));
            const auto nodeId = (*node)[1].get<std::uint64_t>();
            if (nodeId == 0u || nodeId >= nextNodeId)
                return Result<void>::failure(capsuleError(
                    "A legacy graph reference is invalid.", path));
            continue;
        }
        const auto pushSequence = [&](const Json& sequence) -> bool {
            if (!sequence.is_array()) return false;
            for (auto iterator = sequence.rbegin(); iterator != sequence.rend(); ++iterator)
                pending.push_back(&*iterator);
            return true;
        };
        switch (tag) {
        case 1u:
            if (node->size() != 1u) goto malformed;
            break;
        case 2u:
            if (node->size() != 2u || !(*node)[1].is_boolean()) goto malformed;
            break;
        case 3u:
            if (node->size() != 2u || !(*node)[1].is_string()) goto malformed;
            break;
        case 4u:
            if (node->size() != 2u || !(*node)[1].is_number_unsigned()) goto malformed;
            break;
        case 5u: case 6u:
            if (node->size() != 2u || !(*node)[1].is_binary()) goto malformed;
            break;
        case 7u: case 8u: case 9u: case 10u:
            if (node->size() != 3u || !(*node)[1].is_number_unsigned()
                || (*node)[1].get<std::uint64_t>() != nextNodeId
                || nextNodeId++ > maximumNodes
                || !pushSequence((*node)[2])) goto malformed;
            break;
        case 11u:
            if (node->size() != 3u || !(*node)[1].is_number_unsigned()
                || (*node)[1].get<std::uint64_t>() != nextNodeId
                || nextNodeId++ > maximumNodes || !(*node)[2].is_array()) goto malformed;
            for (auto iterator = (*node)[2].rbegin(); iterator != (*node)[2].rend(); ++iterator) {
                if (!iterator->is_array() || iterator->size() != 2u) goto malformed;
                pending.push_back(&(*iterator)[1]); pending.push_back(&(*iterator)[0]);
            }
            break;
        case 12u: {
            if (node->size() != 4u || !(*node)[1].is_number_unsigned()
                || (*node)[1].get<std::uint64_t>() != nextNodeId
                || nextNodeId++ > maximumNodes || !(*node)[2].is_number_unsigned()
                || !(*node)[3].is_array()) goto malformed;
            static constexpr std::array<std::size_t, 6> fieldCounts{3, 16, 11, 12, 9, 5};
            const auto classCode = (*node)[2].get<std::uint64_t>();
            if (classCode == 0 || classCode > fieldCounts.size()
                || (*node)[3].size() != fieldCounts[classCode - 1u]) goto malformed;
            for (auto iterator = (*node)[3].rbegin(); iterator != (*node)[3].rend(); ++iterator)
                pending.push_back(&*iterator);
            break;
        }
        case 13u:
            if (node->size() != 2u || !(*node)[1].is_string()
                || !allowedLegacyClass((*node)[1].get_ref<const std::string&>())) goto malformed;
            break;
        default:
            goto malformed;
        }
        continue;
malformed:
        return Result<void>::failure(capsuleError(
            "A tagged legacy value has an invalid payload.", path));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validateInertGraph(const Json& root,
    const std::filesystem::path& path, const std::uint64_t maximumNodes) {
    return validateInertGraphs({&root}, path, maximumNodes);
}

class CborCursor final {
public:
    explicit CborCursor(const std::span<const unsigned char> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool array(std::uint64_t& size) { return head(4u, size); }
    [[nodiscard]] bool map(std::uint64_t& size) { return head(5u, size); }
    [[nodiscard]] bool unsignedValue(std::uint64_t& value) { return head(0u, value); }
    [[nodiscard]] bool boolean(bool& value) {
        if (position_ >= bytes_.size()) return false;
        if (bytes_[position_] == 0xf4u) { ++position_; value = false; return true; }
        if (bytes_[position_] == 0xf5u) { ++position_; value = true; return true; }
        return false;
    }
    [[nodiscard]] bool null() {
        if (position_ >= bytes_.size() || bytes_[position_] != 0xf6u) return false;
        ++position_; return true;
    }
    [[nodiscard]] bool nextIsNull() const {
        return position_ < bytes_.size() && bytes_[position_] == 0xf6u;
    }
    [[nodiscard]] bool text(std::string_view& value) {
        std::uint64_t size = 0;
        if (!head(3u, size) || size > bytes_.size() - position_) return false;
        value = {reinterpret_cast<const char*>(bytes_.data() + position_),
            static_cast<std::size_t>(size)};
        position_ += static_cast<std::size_t>(size); return true;
    }
    [[nodiscard]] bool expectText(const std::string_view expected) {
        std::string_view actual;
        return text(actual) && actual == expected;
    }
    [[nodiscard]] bool byteString() {
        std::uint64_t size = 0;
        if (!head(2u, size) || size > bytes_.size() - position_) return false;
        position_ += static_cast<std::size_t>(size); return true;
    }
    [[nodiscard]] bool skip(std::uint32_t depth, std::uint64_t& budget) {
        if (depth > 128u || budget == 0) return false;
        --budget;
        std::uint8_t major = 0, additional = 0;
        std::uint64_t value = 0;
        if (!head(major, additional, value)) return false;
        if (major == 0u || major == 1u) return true;
        if (major == 2u || major == 3u) {
            if (value > bytes_.size() - position_) return false;
            position_ += static_cast<std::size_t>(value); return true;
        }
        if (major == 4u || major == 5u) {
            if (major == 5u && value > std::numeric_limits<std::uint64_t>::max() / 2u)
                return false;
            const auto children = major == 5u ? value * 2u : value;
            if (children > budget) return false;
            for (std::uint64_t index = 0; index < children; ++index)
                if (!skip(depth + 1u, budget)) return false;
            return true;
        }
        return major == 7u && (additional == 20u || additional == 21u
            || additional == 22u || additional == 26u || additional == 27u);
    }
    [[nodiscard]] bool finished() const noexcept { return position_ == bytes_.size(); }

private:
    [[nodiscard]] bool head(const std::uint8_t expectedMajor, std::uint64_t& value) {
        std::uint8_t major = 0, additional = 0;
        return head(major, additional, value) && major == expectedMajor;
    }
    [[nodiscard]] bool head(std::uint8_t& major, std::uint8_t& additional,
        std::uint64_t& value) {
        if (position_ >= bytes_.size()) return false;
        const auto initial = bytes_[position_++];
        major = initial >> 5u; additional = initial & 0x1fu;
        if (additional < 24u) { value = additional; return true; }
        const auto count = additional == 24u ? 1u : additional == 25u ? 2u
            : additional == 26u ? 4u : additional == 27u ? 8u : 0u;
        if (count == 0u || count > bytes_.size() - position_) return false;
        value = 0;
        for (unsigned index = 0; index < count; ++index)
            value = (value << 8u) | bytes_[position_++];
        return true;
    }

    std::span<const unsigned char> bytes_{};
    std::size_t position_ = 0;
};

[[nodiscard]] bool validateTaggedValue(CborCursor& cursor,
    std::uint64_t& nextNodeId, const std::uint64_t maximumNodes,
    const std::uint32_t depth = 0) {
    if (depth > 1'024u) return false;
    std::uint64_t size = 0, tag = 0;
    if (!cursor.array(size) || size == 0u || !cursor.unsignedValue(tag)) return false;
    if (tag == 0u) {
        std::uint64_t id = 0;
        return size == 2u && cursor.unsignedValue(id) && id != 0u && id < nextNodeId;
    }
    const auto node = [&]() {
        std::uint64_t id = 0;
        if (!cursor.unsignedValue(id) || id != nextNodeId || nextNodeId > maximumNodes)
            return false;
        ++nextNodeId; return true;
    };
    switch (tag) {
    case 1u:
        return size == 1u;
    case 2u: {
        bool value = false; return size == 2u && cursor.boolean(value);
    }
    case 3u: {
        std::string_view value; return size == 2u && cursor.text(value);
    }
    case 4u: {
        std::uint64_t value = 0; return size == 2u && cursor.unsignedValue(value);
    }
    case 5u: case 6u:
        return size == 2u && cursor.byteString();
    case 7u: case 8u: case 9u: case 10u: {
        std::uint64_t count = 0;
        if (size != 3u || !node() || !cursor.array(count)) return false;
        for (std::uint64_t index = 0; index < count; ++index)
            if (!validateTaggedValue(cursor, nextNodeId, maximumNodes, depth + 1u))
                return false;
        return true;
    }
    case 11u: {
        std::uint64_t count = 0;
        if (size != 3u || !node() || !cursor.array(count)) return false;
        for (std::uint64_t index = 0; index < count; ++index) {
            std::uint64_t pairSize = 0;
            if (!cursor.array(pairSize) || pairSize != 2u
                || !validateTaggedValue(cursor, nextNodeId, maximumNodes, depth + 1u)
                || !validateTaggedValue(cursor, nextNodeId, maximumNodes, depth + 1u))
                return false;
        }
        return true;
    }
    case 12u: {
        static constexpr std::array<std::uint64_t, 6> fieldCounts{3, 16, 11, 12, 9, 5};
        std::uint64_t classCode = 0, count = 0;
        if (size != 4u || !node() || !cursor.unsignedValue(classCode)
            || classCode == 0u || classCode > fieldCounts.size()
            || !cursor.array(count) || count != fieldCounts[classCode - 1u]) return false;
        for (std::uint64_t index = 0; index < count; ++index)
            if (!validateTaggedValue(cursor, nextNodeId, maximumNodes, depth + 1u))
                return false;
        return true;
    }
    case 13u: {
        std::string_view name;
        return size == 2u && cursor.text(name) && allowedLegacyClass(name);
    }
    default:
        return false;
    }
}

[[nodiscard]] Result<void> validateScriptRecord(const std::filesystem::path& root,
    const Json& inventory, const LegacyCapsuleValidationLimits& limits,
    const std::uint64_t declaredSize, const std::string_view expectedDigest) {
    const auto relative = fromUtf8(inventory.at("path").get<std::string>());
    auto decoded = readCompressedRecord(root / relative, limits.maxRecordBytes,
        declaredSize, expectedDigest);
    if (!decoded) return Result<void>::failure(decoded.diagnostics());
    CborCursor cursor(std::span<const unsigned char>{decoded.value()});
    std::uint64_t size = 0, number = 0;
    std::string_view text;
    const auto malformed = [&]() {
        return Result<void>::failure(capsuleError(
            "A capsule script record is malformed.", root / relative));
    };
    if (!cursor.map(size) || size != 9u
        || !cursor.expectText("formatId") || !cursor.text(text)
        || text != "jahorta.salsa.legacy-script-record"
        || !cursor.expectText("schemaVersion") || !cursor.unsignedValue(number) || number != 2u
        || !cursor.expectText("ordinal") || !cursor.unsignedValue(number)
        || number != inventory.at("ordinal").get<std::uint64_t>()
        || !cursor.expectText("key") || !cursor.text(text)
        || text != inventory.at("key").get_ref<const std::string&>()
        || !cursor.expectText("storedName") || !cursor.text(text)
        || text != inventory.at("storedName").get_ref<const std::string&>()
        || !cursor.expectText("status") || !cursor.text(text)
        || text != inventory.at("status").get_ref<const std::string&>()
        || !cursor.expectText("counts") || !cursor.map(size) || size != 5u)
        return malformed();
    std::array<std::uint64_t, 5> counts{};
    static constexpr std::array countNames{
        "sections"sv, "instructions"sv, "parameters"sv, "links"sv, "strings"sv};
    for (std::size_t index = 0; index < countNames.size(); ++index)
        if (!cursor.expectText(countNames[index]) || !cursor.unsignedValue(counts[index]))
            return malformed();
    if (counts[0] != inventory.at("sections").get<std::uint64_t>()
        || counts[1] != inventory.at("instructions").get<std::uint64_t>()
        || counts[2] != inventory.at("parameters").get<std::uint64_t>()
        || counts[4] != inventory.at("strings").get<std::uint64_t>()
        || !cursor.expectText("diagnostics") || !cursor.array(size)
        || size != inventory.at("diagnostics").get<std::uint64_t>())
        return malformed();
    std::uint64_t diagnosticBudget = std::max<std::uint64_t>(1u,
        std::min<std::uint64_t>(limits.maxNodesPerRecord, 1u << 20));
    for (std::uint64_t index = 0; index < size; ++index)
        if (!cursor.skip(0, diagnosticBudget)) return malformed();
    if (!cursor.expectText("ir")) return malformed();
    const auto failed = inventory.at("status") == "failed";
    if (cursor.nextIsNull()) {
        if (!failed || !cursor.null() || !cursor.finished()) return malformed();
        return Result<void>::success();
    }
    if (!cursor.map(size) || size != 7u) return malformed();
    std::uint64_t nextNodeId = 1;
    for (const auto field : {"header"sv, "footer"sv, "string_groups"sv,
            "strings"sv, "sections"sv, "links"sv})
        if (!cursor.expectText(field)
            || !validateTaggedValue(cursor, nextNodeId, limits.maxNodesPerRecord))
            return malformed();
    if (!cursor.expectText("sidecar") || !cursor.map(size) || size != 9u) return malformed();
    for (const auto field : {"folded_sects"sv, "index"sv, "sect_tree"sv,
            "sect_list"sv, "string_garbage"sv, "unused_sections"sv,
            "errors"sv, "error_sections"sv, "variables"sv})
        if (!cursor.expectText(field)
            || !validateTaggedValue(cursor, nextNodeId, limits.maxNodesPerRecord))
            return malformed();
    return cursor.finished() ? Result<void>::success() : malformed();
}

}  // namespace

Result<LegacyCapsuleSummary> LegacyCapsuleReader::validate(
    const std::filesystem::path& root,
    const LegacyCapsuleValidationLimits& limits) {
    std::error_code error;
    const auto attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule root is not a regular directory.", root));
    auto manifestResult = parseJsonFile(root / L"capsule.json", limits.maxRecordBytes);
    if (!manifestResult) return Result<LegacyCapsuleSummary>::failure(
        manifestResult.diagnostics());
    const auto& manifest = manifestResult.value();
    if (!hasExactKeys(manifest, {"formatId", "schemaVersion", "converterContractId",
            "capsuleId", "status", "source", "normalizationCount", "scripts", "entries"})
        || !manifest.at("formatId").is_string()
        || manifest.at("formatId").get<std::string>() != LegacyCapsuleFormatId
        || !manifest.at("schemaVersion").is_number_unsigned())
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule header is invalid.", root / L"capsule.json"));
    if (manifest.at("schemaVersion").get<std::uint64_t>() != LegacyCapsuleSchemaVersion)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule schema is unsupported.", root / L"capsule.json",
            DiagnosticCode::LegacyCapsuleUnsupportedSchema));
    if (!manifest.at("converterContractId").is_string()
        || manifest.at("converterContractId").get<std::string>() != LegacyConverterContractId
        || !manifest.at("capsuleId").is_string()
        || !validHexDigest(manifest.at("capsuleId").get<std::string>())
        || !manifest.at("status").is_string()
        || !manifest.at("normalizationCount").is_number_unsigned()
        || !manifest.at("scripts").is_array() || !manifest.at("entries").is_array())
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule manifest is malformed.", root / L"capsule.json"));
    const auto statusText = manifest.at("status").get<std::string>();
    if (statusText != "ready" && statusText != "action-required")
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule status is invalid.", root / L"capsule.json"));
    const auto& source = manifest.at("source");
    if (!hasExactKeys(source, {"filename", "size", "sha256", "pickleProtocol",
            "projectVersion", "originalRetained"})
        || !source.at("filename").is_string() || !source.at("size").is_number_unsigned()
        || !source.at("sha256").is_string()
        || !validHexDigest(source.at("sha256").get<std::string>())
        || !source.at("pickleProtocol").is_number_unsigned()
        || !source.at("projectVersion").is_number_unsigned()
        || !source.at("originalRetained").is_boolean())
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule source provenance is malformed.", root / L"capsule.json"));

    if (manifest.at("entries").size() > limits.maxFiles)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The legacy capsule contains too many files.", root));
    std::set<std::filesystem::path> expected{};
    std::uint64_t canonicalBytes = 0;
    std::string identityMaterial(LegacyConverterContractId);
    identityMaterial.push_back('\0');
    identityMaterial += source.at("sha256").get<std::string>();
    identityMaterial.push_back('\0');
    std::string previousEntryPath{};
    std::map<std::string, bool, std::less<>> declaredPaths{};
    std::map<std::string, std::pair<std::uint64_t, std::string>, std::less<>>
        declaredIntegrity{};
    for (const auto& entry : manifest.at("entries")) {
        const auto deferredScript = entry.is_object() && entry.contains("path")
            && entry.at("path").is_string()
            && isNumberedScriptPath(entry.at("path").get_ref<const std::string&>());
        auto verified = verifyEntry(root, entry, limits, canonicalBytes, expected,
            !deferredScript);
        if (!verified) return Result<LegacyCapsuleSummary>::failure(verified.diagnostics());
        const auto entryPath = entry.at("path").get<std::string>();
        if (!previousEntryPath.empty() && entryPath <= previousEntryPath)
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "Capsule entries are not in canonical path order.", root / L"capsule.json"));
        previousEntryPath = entryPath;
        const auto canonical = entry.at("canonical").get<bool>();
        declaredPaths.emplace(entryPath, canonical);
        declaredIntegrity.emplace(entryPath, std::pair{
            entry.at("size").get<std::uint64_t>(),
            entry.at("sha256").get<std::string>()});
        if (entryPath == "project.cbor") {
            if (!canonical) return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "The capsule project record must be canonical.", root / L"capsule.json"));
        } else if (isNumberedScriptPath(entryPath)) {
            if (!canonical) return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "Capsule script records must be canonical.", root / L"capsule.json"));
        } else if (entryPath == "evidence/source.prj") {
            if (canonical) return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "Original project evidence must be noncanonical.", root / L"capsule.json"));
        } else {
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "The capsule contains an unknown entry role.", root / L"capsule.json"));
        }
        if (entry.at("canonical").get<bool>()) {
            identityMaterial += entryPath;
            identityMaterial.push_back('\0');
            identityMaterial += entry.at("sha256").get<std::string>();
            identityMaterial.push_back('\0');
            identityMaterial += std::to_string(entry.at("size").get<std::uint64_t>());
            identityMaterial.push_back('\n');
        }
    }
    const auto identityBytes = std::as_bytes(std::span{
        identityMaterial.data(), identityMaterial.size()});
    auto identity = sha256(identityBytes);
    if (!identity || identity.value().toHex() != manifest.at("capsuleId").get<std::string>())
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The canonical capsule identity does not match its contents.",
            root / L"capsule.json", DiagnosticCode::LegacyCapsuleIntegrityFailed));
    if (!declaredPaths.contains("project.cbor"))
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The capsule project record is missing.", root / L"capsule.json"));
    auto projectRecord = validateProjectRecord(root, limits);
    if (!projectRecord) return Result<LegacyCapsuleSummary>::failure(
        projectRecord.diagnostics());
    std::uint64_t actualFiles = 0;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto entryAttributes = GetFileAttributesW(iterator->path().c_str());
        if (entryAttributes == INVALID_FILE_ATTRIBUTES
            || (entryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
            || iterator->is_symlink(error) || error
            || (iterator->status(error).type() != std::filesystem::file_type::directory
                && iterator->status(error).type() != std::filesystem::file_type::regular))
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "The capsule contains a reparse point or unsupported entry.", iterator->path()));
        if (iterator->is_regular_file(error) && ++actualFiles > manifest.at("entries").size() + 1u)
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "The capsule contains unlisted files.", root));
    }
    if (error || actualFiles != manifest.at("entries").size() + 1u)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The capsule contains missing or unlisted files.", root));

    LegacyCapsuleSummary summary{};
    summary.root = root;
    summary.capsuleId = manifest.at("capsuleId").get<std::string>();
    summary.converterContractId = manifest.at("converterContractId").get<std::string>();
    summary.status = statusText == "ready"
        ? LegacyCapsuleStatus::Ready : LegacyCapsuleStatus::ActionRequired;
    summary.normalizationCount = manifest.at("normalizationCount").get<std::uint64_t>();
    summary.source = {source.at("filename").get<std::string>(),
        source.at("size").get<std::uint64_t>(), source.at("sha256").get<std::string>(),
        source.at("pickleProtocol").get<std::uint32_t>(),
        source.at("projectVersion").get<std::uint32_t>(),
        source.at("originalRetained").get<bool>()};
    std::uint32_t expectedOrdinal = 0;
    std::set<std::string> names{};
    bool actionRequired = !projectRecord.value().at("diagnostics").empty();
    for (const auto& script : manifest.at("scripts")) {
        if (!hasExactKeys(script, {"ordinal", "key", "storedName", "status",
                "sections", "instructions", "parameters", "strings", "diagnostics", "path"})
            || !script.at("ordinal").is_number_unsigned()
            || script.at("ordinal").get<std::uint64_t>() != expectedOrdinal
            || !script.at("key").is_string() || !script.at("storedName").is_string()
            || !script.at("status").is_string() || !script.at("path").is_string())
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "A capsule script inventory entry is malformed.", root / L"capsule.json"));
        auto folded = script.at("key").get<std::string>();
        std::ranges::transform(folded, folded.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const auto scriptStatus = script.at("status").get<std::string>();
        const auto duplicateName = !names.insert(folded).second;
        if ((scriptStatus != "accepted" && scriptStatus != "failed")
            || (duplicateName && scriptStatus != "failed"))
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "Accepted capsule script keys collide or a script status is invalid.", root / L"capsule.json"));
        if (!validScriptPath(script.at("path").get<std::string>(), expectedOrdinal))
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "A capsule script path does not match its ordinal.", root / L"capsule.json"));
        const auto relative = fromUtf8(script.at("path").get<std::string>());
        auto foldedPath = relative.generic_wstring();
        std::ranges::transform(foldedPath, foldedPath.begin(), towlower);
        if (!expected.contains(std::filesystem::path(foldedPath)))
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "A script inventory path is not a declared capsule entry.", root / relative));
        for (const auto key : {"sections", "instructions", "parameters", "strings", "diagnostics"})
            if (!script.at(key).is_number_unsigned())
                return Result<LegacyCapsuleSummary>::failure(capsuleError(
                    "A script inventory count is malformed.", root / L"capsule.json"));
        const auto integrity = declaredIntegrity.find(
            script.at("path").get_ref<const std::string&>());
        if (integrity == declaredIntegrity.end())
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "A script inventory path is not a declared capsule entry.", root / relative));
        auto scriptRecord = validateScriptRecord(root, script, limits,
            integrity->second.first, integrity->second.second);
        if (!scriptRecord) return Result<LegacyCapsuleSummary>::failure(
            scriptRecord.diagnostics());
        actionRequired = actionRequired || scriptStatus == "failed";
        summary.scripts.push_back({expectedOrdinal++, script.at("key").get<std::string>(),
            script.at("storedName").get<std::string>(),
            scriptStatus == "accepted" ? LegacyScriptStatus::Accepted : LegacyScriptStatus::Failed,
            script.at("sections").get<std::uint64_t>(),
            script.at("instructions").get<std::uint64_t>(),
            script.at("parameters").get<std::uint64_t>(),
            script.at("strings").get<std::uint64_t>(),
            script.at("diagnostics").get<std::uint64_t>(), relative,
            integrity->second.first, integrity->second.second});
    }
    const auto scriptEntryCount = std::ranges::count_if(declaredPaths, [](const auto& entry) {
        return isNumberedScriptPath(entry.first);
    });
    if (scriptEntryCount != summary.scripts.size()
        || summary.normalizationCount != projectRecord.value().at("normalizations").size()
        || (summary.status == LegacyCapsuleStatus::ActionRequired) != actionRequired)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The capsule summary does not match its project and script records.",
            root / L"capsule.json", DiagnosticCode::LegacyCapsuleIntegrityFailed));
    if (summary.source.pickleProtocol != 4u || summary.source.projectVersion != 7u)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The capsule does not describe an official v7 protocol-4 project.",
            root / L"capsule.json"));
    const auto hasEvidence = declaredPaths.contains("evidence/source.prj");
    if (summary.source.originalRetained != hasEvidence)
        return Result<LegacyCapsuleSummary>::failure(capsuleError(
            "The capsule source-evidence disposition is inconsistent.",
            root / L"capsule.json", DiagnosticCode::LegacyCapsuleIntegrityFailed));
    if (summary.source.originalRetained) {
        const auto evidence = root / L"evidence" / L"source.prj";
        if (!isRegularWithoutReparsePoint(evidence))
            return Result<LegacyCapsuleSummary>::failure(capsuleError(
                "Retained source evidence is missing.", evidence));
    }
    return Result<LegacyCapsuleSummary>::success(std::move(summary));
}

}  // namespace salsa::core
