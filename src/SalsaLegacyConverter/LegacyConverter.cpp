#include "LegacyConverter.h"

#include <Windows.h>
#include <bcrypt.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "bcrypt.lib")

namespace salsa::legacy {
namespace {

using Json = nlohmann::ordered_json;
using namespace std::string_view_literals;

struct Diagnostic final { std::string code; std::string path; std::string message; };
struct Counts final {
    std::uint64_t sections = 0;
    std::uint64_t instructions = 0;
    std::uint64_t parameters = 0;
    std::uint64_t links = 0;
    std::uint64_t strings = 0;
};
struct Entry final {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
    bool canonical = true;
};

[[nodiscard]] std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output; output.reserve(bytes.size() * 2u);
    for (const auto item : bytes) {
        const auto value = std::to_integer<unsigned char>(item);
        output.push_back(digits[value >> 4u]); output.push_back(digits[value & 0x0fu]);
    }
    return output;
}

class Sha256 final {
public:
    Sha256() {
        DWORD written = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectSize_), sizeof(objectSize_), &written, 0) < 0)
            return;
        object_.resize(objectSize_);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), objectSize_, nullptr, 0, 0) < 0)
            hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    [[nodiscard]] bool add(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 1u << 20));
            if (!hash_ || BCryptHashData(hash_, reinterpret_cast<PUCHAR>(
                    const_cast<std::byte*>(bytes.data())), count, 0) < 0) return false;
            bytes = bytes.subspan(count);
        }
        return true;
    }
    [[nodiscard]] std::string finish() {
        std::array<std::byte, 32> digest{};
        if (!hash_ || BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(digest.data()),
                static_cast<ULONG>(digest.size()), 0) < 0) return {};
        BCryptDestroyHash(hash_); hash_ = nullptr;
        return hex(digest);
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    DWORD objectSize_ = 0;
    std::vector<UCHAR> object_{};
};

[[nodiscard]] std::string sha256(std::span<const std::byte> bytes) {
    Sha256 hash;
    return hash.add(bytes) ? hash.finish() : std::string{};
}

[[nodiscard]] std::string sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    Sha256 hash;
    std::array<std::byte, 64u << 10> buffer{};
    for (;;) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && !hash.add(std::span{buffer.data(), static_cast<std::size_t>(count)}))
            return {};
        if (input.eof()) break;
        if (!input) return {};
    }
    return hash.finish();
}

[[nodiscard]] bool writeFile(const std::filesystem::path& path,
    std::span<const std::byte> bytes) {
    std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.flush(); return output.good();
}

[[nodiscard]] bool compressFile(const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::error_code error;
    const auto size = std::filesystem::file_size(source, error);
    if (error || size > std::numeric_limits<std::size_t>::max()) return false;
    std::ifstream input(source, std::ios::binary);
    std::vector<unsigned char> uncompressed(static_cast<std::size_t>(size));
    if (!uncompressed.empty()) input.read(reinterpret_cast<char*>(uncompressed.data()),
        static_cast<std::streamsize>(uncompressed.size()));
    if (!input) return false;
    std::vector<unsigned char> compressed;
    if (lodepng::compress(compressed, uncompressed) != 0) return false;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return false;
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!compressed.empty()) output.write(reinterpret_cast<const char*>(compressed.data()),
        static_cast<std::streamsize>(compressed.size()));
    output.flush(); return output.good();
}

[[nodiscard]] std::vector<std::byte> jsonBytes(const Json& value) {
    auto text = value.dump(2, ' ', false, Json::error_handler_t::strict); text.push_back('\n');
    const auto data = std::as_bytes(std::span{text.data(), text.size()});
    return {data.begin(), data.end()};
}

[[nodiscard]] std::string filenameUtf8(const std::filesystem::path& path) {
    const auto name = path.filename().u8string();
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}

[[nodiscard]] const std::map<std::string, std::set<std::string>, std::less<>>& expectedFields() {
    static const std::map<std::string, std::set<std::string>, std::less<>> fields{
        {"SALSA.Project.project_container.SCTProject", {"scts", "file_name", "filepath", "global_variables", "version", "inst_id_colors"}},
        {"SALSA.Project.project_container.SCTScript", {"folded_sects", "name", "index", "header", "sects", "sect_tree", "sect_list", "inst_locations", "links", "footer", "strings", "string_groups", "string_locations", "string_garbage", "unused_sections", "errors", "error_sections", "variables", "section_num"}},
        {"SALSA.Project.project_container.SCTSection", {"name", "length", "absolute_offset", "insts", "inst_tree", "inst_list", "inst_errors", "errors", "strings", "insts_used", "garbage", "string", "jump_loops", "internal_sections_inst", "internal_sections_curs", "is_compound", "type"}},
        {"SALSA.Project.project_container.SCTInstruction", {"ID", "base_id", "absolute_offset", "skip_refresh", "delay_param", "errors", "links_out", "links_in", "params", "l_params", "condition", "synopsis", "ungrouped_position", "my_goto_uuids", "my_master_uuids", "label", "encode_inst"}},
        {"SALSA.Project.project_container.SCTParameter", {"ID", "type", "link", "errors", "analyze_log", "value", "formatted_value", "raw_bytes", "linked_string", "override", "arithmetic_value"}},
        {"SALSA.Project.project_container.SCTLink", {"type", "script", "origin", "origin_trace", "target", "target_trace", "ID"}},
    };
    return fields;
}

[[nodiscard]] Json fieldDispositions() {
    return {
        {"SCTProject", {{"scts", "semantic"}, {"file_name", "run-local"}, {"filepath", "run-local-discard"}, {"global_variables", "authoring"}, {"version", "contract"}, {"inst_id_colors", "authoring"}}},
        {"SCTScript", {{"folded_sects", "authoring"}, {"name", "semantic"}, {"index", "preserved-raw"}, {"header", "preserved-raw"}, {"sects", "semantic"}, {"sect_tree", "authoring"}, {"sect_list", "semantic-order"}, {"inst_locations", "derived-recompute"}, {"links", "semantic"}, {"footer", "semantic"}, {"strings", "semantic"}, {"string_groups", "authoring"}, {"string_locations", "derived-recompute"}, {"string_garbage", "preserved-raw"}, {"unused_sections", "advisory"}, {"errors", "advisory"}, {"error_sections", "advisory"}, {"variables", "authoring"}, {"section_num", "derived-recompute"}}},
        {"SCTSection", {{"name", "semantic"}, {"length", "derived-recompute"}, {"absolute_offset", "derived-recompute"}, {"insts", "semantic"}, {"inst_tree", "authoring"}, {"inst_list", "semantic-order"}, {"inst_errors", "advisory"}, {"errors", "advisory"}, {"strings", "semantic"}, {"insts_used", "derived-recompute"}, {"garbage", "preserved-raw"}, {"string", "semantic"}, {"jump_loops", "derived-recompute"}, {"internal_sections_inst", "derived-recompute"}, {"internal_sections_curs", "derived-recompute"}, {"is_compound", "semantic"}, {"type", "semantic"}}},
        {"SCTInstruction", {{"ID", "identity"}, {"base_id", "semantic"}, {"absolute_offset", "derived-recompute"}, {"skip_refresh", "semantic"}, {"delay_param", "semantic"}, {"errors", "advisory"}, {"links_out", "semantic"}, {"links_in", "derived-recompute"}, {"params", "semantic"}, {"l_params", "semantic"}, {"condition", "derived-recompute"}, {"synopsis", "derived-recompute"}, {"ungrouped_position", "derived-recompute"}, {"my_goto_uuids", "authoring"}, {"my_master_uuids", "authoring"}, {"label", "authoring"}, {"encode_inst", "authoring"}}},
        {"SCTParameter", {{"ID", "identity"}, {"type", "semantic"}, {"link", "semantic"}, {"errors", "advisory"}, {"analyze_log", "advisory"}, {"value", "semantic"}, {"formatted_value", "derived-recompute"}, {"raw_bytes", "preserved-raw"}, {"linked_string", "semantic"}, {"override", "semantic"}, {"arithmetic_value", "derived-recompute"}}},
        {"SCTLink", {{"type", "semantic"}, {"script", "semantic"}, {"origin", "derived-recompute"}, {"origin_trace", "semantic"}, {"target", "derived-recompute"}, {"target_trace", "semantic"}, {"ID", "identity"}}},
    };
}

[[nodiscard]] bool retainedField(const std::string_view className,
    const std::string_view field) {
    const auto excluded = [&](std::initializer_list<std::string_view> names) {
        return std::ranges::find(names, field) != names.end();
    };
    if (className.ends_with(".SCTProject"))
        return !excluded({"scts", "file_name", "filepath"});
    if (className.ends_with(".SCTScript"))
        return !excluded({"inst_locations", "string_locations", "section_num"});
    if (className.ends_with(".SCTSection"))
        return !excluded({"length", "absolute_offset", "insts_used", "jump_loops",
            "internal_sections_inst", "internal_sections_curs"});
    if (className.ends_with(".SCTInstruction"))
        return !excluded({"absolute_offset", "links_in", "condition", "synopsis",
            "ungrouped_position"});
    if (className.ends_with(".SCTParameter"))
        return !excluded({"formatted_value", "arithmetic_value"});
    if (className.ends_with(".SCTLink")) return !excluded({"origin", "target"});
    return true;
}

[[nodiscard]] std::span<const std::string_view> retainedFieldOrder(
    const std::string_view className) {
    static constexpr std::array project{"global_variables"sv, "version"sv, "inst_id_colors"sv};
    static constexpr std::array script{"folded_sects"sv, "name"sv, "index"sv, "header"sv,
        "sects"sv, "sect_tree"sv, "sect_list"sv, "links"sv, "footer"sv, "strings"sv,
        "string_groups"sv, "string_garbage"sv, "unused_sections"sv, "errors"sv,
        "error_sections"sv, "variables"sv};
    static constexpr std::array section{"name"sv, "insts"sv, "inst_tree"sv, "inst_list"sv,
        "inst_errors"sv, "errors"sv, "strings"sv, "garbage"sv, "string"sv,
        "is_compound"sv, "type"sv};
    static constexpr std::array instruction{"ID"sv, "base_id"sv, "skip_refresh"sv,
        "delay_param"sv, "errors"sv, "links_out"sv, "params"sv, "l_params"sv,
        "my_goto_uuids"sv, "my_master_uuids"sv, "label"sv, "encode_inst"sv};
    static constexpr std::array parameter{"ID"sv, "type"sv, "link"sv, "errors"sv,
        "analyze_log"sv, "value"sv, "raw_bytes"sv, "linked_string"sv, "override"sv};
    static constexpr std::array link{"type"sv, "script"sv, "origin_trace"sv,
        "target_trace"sv, "ID"sv};
    if (className.ends_with(".SCTProject")) return project;
    if (className.ends_with(".SCTScript")) return script;
    if (className.ends_with(".SCTSection")) return section;
    if (className.ends_with(".SCTInstruction")) return instruction;
    if (className.ends_with(".SCTParameter")) return parameter;
    if (className.ends_with(".SCTLink")) return link;
    return {};
}

[[nodiscard]] std::uint64_t retainedClassCode(const std::string_view className) {
    if (className.ends_with(".SCTProject")) return 1;
    if (className.ends_with(".SCTScript")) return 2;
    if (className.ends_with(".SCTSection")) return 3;
    if (className.ends_with(".SCTInstruction")) return 4;
    if (className.ends_with(".SCTParameter")) return 5;
    if (className.ends_with(".SCTLink")) return 6;
    return 0;
}

[[nodiscard]] bool exactObject(const ValueStore& store, const ValueId value,
    const std::string_view className, const std::string& path,
    std::vector<Diagnostic>& diagnostics) {
    if (value == InvalidValueId || store.kind(value) != ValueKind::Object) {
        diagnostics.push_back({"invalid-object-type", path, "Legacy object has an unexpected type."});
        return false;
    }
    const auto object = store.object(value);
    if (object.className != className || !expectedFields().contains(object.className)) {
        diagnostics.push_back({"unexpected-class", path, "Legacy object class is not allowed."});
        return false;
    }
    std::set<std::string> actual;
    for (const auto& [name, _] : object.attributes) actual.insert(name);
    if (actual != expectedFields().at(object.className)) {
        diagnostics.push_back({"unexpected-object-shape", path,
            "Legacy object fields do not match the official final v7 shape."});
        return false;
    }
    return true;
}

void validateSelectedGraph(const ValueStore& store, const ValueId value,
    const std::string& path, std::vector<Diagnostic>& diagnostics,
    std::unordered_set<ValueId>& active, std::unordered_set<ValueId>& visited) {
    if (value == InvalidValueId || visited.contains(value)) return;
    if (!active.insert(value).second) {
        diagnostics.push_back({"cyclic-value", path, "Cyclic values are not valid migration IR."});
        return;
    }
    const auto kind = store.kind(value);
    if (kind == ValueKind::Object) {
        const auto object = store.object(value);
        (void)exactObject(store, value, object.className, path, diagnostics);
        for (const auto& [name, child] : object.attributes)
            if (retainedField(object.className, name))
                validateSelectedGraph(store, child, path + "." + name,
                    diagnostics, active, visited);
    } else if (kind == ValueKind::List || kind == ValueKind::Tuple
        || kind == ValueKind::Set || kind == ValueKind::FrozenSet) {
        std::size_t index = 0;
        for (const auto child : store.items(value))
            validateSelectedGraph(store, child, path + "[" + std::to_string(index++) + "]",
                diagnostics, active, visited);
    } else if (kind == ValueKind::Dictionary) {
        std::size_t index = 0;
        for (const auto& [key, child] : store.entries(value)) {
            validateSelectedGraph(store, key, path + ".keys[" + std::to_string(index) + "]",
                diagnostics, active, visited);
            validateSelectedGraph(store, child, path + ".values[" + std::to_string(index++) + "]",
                diagnostics, active, visited);
        }
    }
    active.erase(value); visited.insert(value);
}

[[nodiscard]] std::uint64_t dictionarySize(const ValueStore& store, const ValueId value) {
    return value != InvalidValueId && store.kind(value) == ValueKind::Dictionary
        ? store.entries(value).size() : 0;
}

[[nodiscard]] Counts countScript(const ValueStore& store, const ValueId script) {
    Counts counts{};
    const auto sections = attributeValue(store, script, "sects");
    if (sections == InvalidValueId || store.kind(sections) != ValueKind::Dictionary) return counts;
    const auto sectionEntries = store.entries(sections); counts.sections = sectionEntries.size();
    for (const auto& [_, section] : sectionEntries) {
        const auto instructions = attributeValue(store, section, "insts");
        counts.instructions += dictionarySize(store, instructions);
        if (instructions != InvalidValueId && store.kind(instructions) == ValueKind::Dictionary) {
            for (const auto& [__, instruction] : store.entries(instructions)) {
                counts.parameters += dictionarySize(store,
                    attributeValue(store, instruction, "params"));
                const auto loops = attributeValue(store, instruction, "l_params");
                if (loops != InvalidValueId && (store.kind(loops) == ValueKind::List
                    || store.kind(loops) == ValueKind::Tuple))
                    for (const auto loop : store.items(loops))
                        counts.parameters += dictionarySize(store, loop);
            }
        }
    }
    const auto links = attributeValue(store, script, "links");
    counts.links = links != InvalidValueId && (store.kind(links) == ValueKind::List
        || store.kind(links) == ValueKind::Tuple) ? store.items(links).size() : 0;
    counts.strings = dictionarySize(store, attributeValue(store, script, "strings"));
    return counts;
}

void normalizeStrings(ValueStore& store, const ValueId script, const std::string& key,
    Json& normalizations) {
    const auto strings = attributeValue(store, script, "strings");
    if (strings == InvalidValueId || store.kind(strings) != ValueKind::Dictionary) return;
    for (const auto& [id, value] : store.entries(strings)) {
        if (store.kind(value) != ValueKind::String) continue;
        auto text = store.text(value);
        if (text.find("\\h") != std::string::npos) continue;
        text.insert(0, "\\h()"); store.replaceText(value, text);
        normalizations.push_back({{"code", "string-header-inserted"},
            {"script", key}, {"entity", valueString(store, id)}});
    }
}

bool removeStringFromSequence(ValueStore& store, const ValueId value,
    const std::string& target, std::unordered_set<ValueId>& active) {
    if (value == InvalidValueId || !active.insert(value).second) return false;
    bool changed = false; const auto kind = store.kind(value);
    if (kind == ValueKind::List || kind == ValueKind::Tuple) {
        auto items = store.items(value);
        const auto old = items.size();
        std::erase_if(items, [&](const ValueId item) {
            return store.kind(item) == ValueKind::String && store.text(item) == target;
        });
        changed = old != items.size();
        for (const auto item : items)
            changed = removeStringFromSequence(store, item, target, active) || changed;
        if (changed) store.replaceItems(value, items);
    } else if (kind == ValueKind::Dictionary) {
        for (const auto& [_, item] : store.entries(value))
            changed = removeStringFromSequence(store, item, target, active) || changed;
    }
    active.erase(value); return changed;
}

void normalizePlaceholders(ValueStore& store, const ValueId script,
    const std::string& scriptKey, Json& normalizations) {
    const auto sections = attributeValue(store, script, "sects");
    if (sections == InvalidValueId || store.kind(sections) != ValueKind::Dictionary) return;
    for (const auto& [sectionKey, section] : store.entries(sections)) {
        const auto instructions = attributeValue(store, section, "insts");
        if (instructions == InvalidValueId || store.kind(instructions) != ValueKind::Dictionary) continue;
        auto instructionEntries = store.entries(instructions);
        std::vector<std::string> placeholders;
        for (const auto& [id, instruction] : instructionEntries) {
            const auto base = attributeValue(store, instruction, "base_id");
            if (store.kind(instruction) == ValueKind::Object
                && store.object(instruction).className.ends_with(".SCTInstruction")
                && base != InvalidValueId && store.kind(base) == ValueKind::Null)
                placeholders.push_back(valueString(store, id));
        }
        for (const auto& id : placeholders) {
            std::erase_if(instructionEntries, [&](const auto& entry) {
                return valueString(store, entry.first) == id;
            });
            std::unordered_set<ValueId> active;
            (void)removeStringFromSequence(store,
                attributeValue(store, section, "inst_list"), id, active);
            active.clear();
            (void)removeStringFromSequence(store,
                attributeValue(store, section, "inst_tree"), id, active);
            normalizations.push_back({{"code", "placeholder-instruction-removed"},
                {"script", scriptKey},
                {"entity", valueString(store, sectionKey) + "/" + id}});
        }
        if (!placeholders.empty()) store.replaceEntries(instructions, instructionEntries);
    }
}

[[nodiscard]] bool withinCounts(const Counts& total, const ConversionLimits& limits) {
    return total.sections <= limits.maxSections && total.instructions <= limits.maxInstructions
        && total.parameters <= limits.maxParameters && total.links <= limits.maxLinks
        && total.strings <= limits.maxStrings;
}

class CborWriter final {
public:
    explicit CborWriter(const std::filesystem::path& path) {
        std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
        if (!error) output_.open(path, std::ios::binary | std::ios::trunc);
    }
    ~CborWriter() { output_.flush(); }
    [[nodiscard]] bool good() { output_.flush(); return output_.good(); }
    void array(const std::uint64_t size) { major(4, size); }
    void map(const std::uint64_t size) { major(5, size); }
    void unsignedValue(const std::uint64_t value) { major(0, value); }
    void signedValue(const std::int64_t value) {
        if (value >= 0) major(0, static_cast<std::uint64_t>(value));
        else major(1, static_cast<std::uint64_t>(-(value + 1)));
    }
    void boolean(const bool value) { byte(value ? 0xf5 : 0xf4); }
    void null() { byte(0xf6); }
    void text(const std::string_view value) {
        major(3, value.size()); output_.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    void bytes(const std::span<const std::byte> value) {
        major(2, value.size());
        if (!value.empty()) output_.write(reinterpret_cast<const char*>(value.data()),
            static_cast<std::streamsize>(value.size()));
    }
    void json(const Json& value) {
        if (value.is_null()) null();
        else if (value.is_boolean()) boolean(value.get<bool>());
        else if (value.is_number_unsigned()) unsignedValue(value.get<std::uint64_t>());
        else if (value.is_number_integer()) signedValue(value.get<std::int64_t>());
        else if (value.is_number_float()) {
            byte(0xfb); const auto bits = std::bit_cast<std::uint64_t>(value.get<double>());
            bigEndian(bits, 8);
        } else if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) {
            array(value.size()); for (const auto& item : value) json(item);
        } else if (value.is_object()) {
            map(value.size());
            for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
                text(iterator.key()); json(iterator.value());
            }
        } else throw std::runtime_error("unsupported JSON value in CBOR writer");
    }
private:
    void byte(const std::uint8_t value) { output_.put(static_cast<char>(value)); }
    void bigEndian(const std::uint64_t value, const unsigned count) {
        for (auto shift = count * 8u; shift != 0; shift -= 8u)
            byte(static_cast<std::uint8_t>(value >> (shift - 8u)));
    }
    void major(const std::uint8_t type, const std::uint64_t value) {
        const auto prefix = static_cast<std::uint8_t>(type << 5u);
        if (value < 24u) byte(static_cast<std::uint8_t>(prefix | value));
        else if (value <= 0xffu) { byte(prefix | 24u); bigEndian(value, 1); }
        else if (value <= 0xffffu) { byte(prefix | 25u); bigEndian(value, 2); }
        else if (value <= 0xffffffffu) { byte(prefix | 26u); bigEndian(value, 4); }
        else { byte(prefix | 27u); bigEndian(value, 8); }
    }
    std::ofstream output_{};
};

class FilteredEncoder final {
public:
    FilteredEncoder(const ValueStore& store, CborWriter& writer)
        : store_(store), writer_(writer) {}
    void write(const ValueId value) { writeValue(value, 0); }
private:
    enum : std::uint8_t {
        Reference = 0, Null = 1, Boolean = 2, Integer = 3, Float64 = 4,
        String = 5, Bytes = 6, List = 7, Tuple = 8, Set = 9,
        FrozenSet = 10, Dictionary = 11, Object = 12, Global = 13, Invalid = 255,
    };
    void writeValue(const ValueId value, const std::uint32_t depth) {
        if (value == InvalidValueId) {
            writer_.array(2); writer_.unsignedValue(Invalid); writer_.unsignedValue(0); return;
        }
        if (depth > 1'024u) throw std::runtime_error("legacy value nesting is too deep");
        const auto kind = store_.kind(value);
        const auto compound = kind == ValueKind::List || kind == ValueKind::Tuple
            || kind == ValueKind::Set || kind == ValueKind::FrozenSet
            || kind == ValueKind::Dictionary || kind == ValueKind::Object;
        if (compound) if (const auto found = ids_.find(value); found != ids_.end()) {
            writer_.array(2); writer_.unsignedValue(Reference);
            writer_.unsignedValue(found->second); return;
        }
        const auto nodeId = compound ? nextId_++ : 0;
        if (compound) ids_[value] = nodeId;
        const auto prefix = [&](const std::uint64_t size, const std::uint8_t tag) {
            writer_.array(size); writer_.unsignedValue(tag);
            if (compound) writer_.unsignedValue(nodeId);
        };
        switch (kind) {
        case ValueKind::Null: prefix(1, Null); break;
        case ValueKind::Boolean:
            prefix(2, Boolean); writer_.boolean(store_.boolean(value)); break;
        case ValueKind::Integer: {
            prefix(2, Integer); writer_.text(store_.text(value)); break;
        }
        case ValueKind::Float:
            prefix(2, Float64); writer_.unsignedValue(store_.floatBits(value)); break;
        case ValueKind::String: {
            prefix(2, String); const auto text = store_.text(value);
            writer_.bytes(std::as_bytes(std::span{text.data(), text.size()})); break;
        }
        case ValueKind::Bytes: {
            prefix(2, Bytes); const auto bytes = store_.bytes(value); writer_.bytes(bytes); break;
        }
        case ValueKind::List: case ValueKind::Tuple: case ValueKind::Set:
        case ValueKind::FrozenSet: {
            const auto kind = store_.kind(value);
            const auto tag = kind == ValueKind::List ? List : kind == ValueKind::Tuple ? Tuple
                : kind == ValueKind::Set ? Set : FrozenSet;
            const auto items = store_.items(value); prefix(3, tag); writer_.array(items.size());
            for (const auto item : items) writeValue(item, depth + 1u);
            break;
        }
        case ValueKind::Dictionary: {
            const auto entries = store_.entries(value); prefix(3, Dictionary);
            writer_.array(entries.size());
            for (const auto& [key, child] : entries) {
                writer_.array(2); writeValue(key, depth + 1u); writeValue(child, depth + 1u);
            }
            break;
        }
        case ValueKind::Object: {
            const auto object = store_.object(value);
            const auto fields = retainedFieldOrder(object.className);
            prefix(4, Object); writer_.unsignedValue(retainedClassCode(object.className));
            writer_.array(fields.size());
            for (const auto name : fields) {
                const auto found = std::ranges::find(object.attributes, name,
                    &std::pair<std::string, ValueId>::first);
                writeValue(found == object.attributes.end()
                    ? InvalidValueId : found->second, depth + 1u);
            }
            break;
        }
        case ValueKind::Global:
            prefix(2, Global); writer_.text(store_.text(value)); break;
        }
    }
    const ValueStore& store_;
    CborWriter& writer_;
    std::uint64_t nextId_ = 1;
    std::unordered_map<ValueId, std::uint64_t> ids_{};
};

[[nodiscard]] Json diagnosticsJson(const std::vector<Diagnostic>& diagnostics) {
    Json result = Json::array();
    for (const auto& item : diagnostics)
        result.push_back({{"code", item.code}, {"path", item.path}, {"message", item.message}});
    return result;
}

void writeNamedValue(CborWriter& writer, FilteredEncoder& encoder,
    const ValueStore& store, const ValueId object, const std::string_view name) {
    writer.text(name); encoder.write(attributeValue(store, object, name));
}

[[nodiscard]] bool writeScriptRecord(const std::filesystem::path& path,
    const ValueStore& store, const ValueId script, const std::uint32_t ordinal,
    const std::string& key, const std::string& storedName, const bool accepted,
    const Counts& counts, const std::vector<Diagnostic>& diagnostics) {
    CborWriter writer(path); FilteredEncoder encoder(store, writer);
    writer.map(9);
    writer.text("formatId"); writer.text("jahorta.salsa.legacy-script-record");
    writer.text("schemaVersion"); writer.unsignedValue(2);
    writer.text("ordinal"); writer.unsignedValue(ordinal);
    writer.text("key"); writer.text(key);
    writer.text("storedName"); writer.text(storedName);
    writer.text("status"); writer.text(accepted ? "accepted" : "failed");
    writer.text("counts"); writer.map(5);
    writer.text("sections"); writer.unsignedValue(counts.sections);
    writer.text("instructions"); writer.unsignedValue(counts.instructions);
    writer.text("parameters"); writer.unsignedValue(counts.parameters);
    writer.text("links"); writer.unsignedValue(counts.links);
    writer.text("strings"); writer.unsignedValue(counts.strings);
    writer.text("diagnostics"); writer.json(diagnosticsJson(diagnostics));
    writer.text("ir");
    if (script == InvalidValueId || store.kind(script) != ValueKind::Object
        || !store.object(script).className.ends_with(".SCTScript")) {
        writer.null(); return writer.good();
    }
    // This order is the migration pipeline: framing, text, semantic sections,
    // then references and authoring/advisory sidecars.
    writer.map(7);
    writeNamedValue(writer, encoder, store, script, "header");
    writeNamedValue(writer, encoder, store, script, "footer");
    writeNamedValue(writer, encoder, store, script, "string_groups");
    writeNamedValue(writer, encoder, store, script, "strings");
    writer.text("sections"); encoder.write(attributeValue(store, script, "sects"));
    writeNamedValue(writer, encoder, store, script, "links");
    writer.text("sidecar"); writer.map(9);
    for (const auto field : {"folded_sects", "index", "sect_tree", "sect_list",
            "string_garbage", "unused_sections", "errors", "error_sections", "variables"})
        writeNamedValue(writer, encoder, store, script, field);
    return writer.good();
}

[[nodiscard]] bool writeProjectRecord(const std::filesystem::path& path,
    const ValueStore& store, const ValueId project, const Json& normalizations,
    const std::vector<Diagnostic>& diagnostics) {
    CborWriter writer(path); FilteredEncoder encoder(store, writer);
    writer.map(6);
    writer.text("formatId"); writer.text("jahorta.salsa.legacy-project-record");
    writer.text("schemaVersion"); writer.unsignedValue(2);
    writer.text("fieldDispositions"); writer.json(fieldDispositions());
    writer.text("fields"); writer.map(3);
    writeNamedValue(writer, encoder, store, project, "global_variables");
    writeNamedValue(writer, encoder, store, project, "version");
    writeNamedValue(writer, encoder, store, project, "inst_id_colors");
    writer.text("normalizations"); writer.json(normalizations);
    writer.text("diagnostics"); writer.json(diagnosticsJson(diagnostics));
    return writer.good();
}

[[nodiscard]] bool addExistingFile(const std::filesystem::path& root,
    const std::string& relative, const bool canonical, std::vector<Entry>& entries,
    std::uint64_t& outputBytes, const ConversionRequest& request) {
    const auto* begin = reinterpret_cast<const char8_t*>(relative.data());
    const auto path = root / std::filesystem::path(std::u8string(begin, begin + relative.size()));
    std::error_code error; const auto size = std::filesystem::file_size(path, error);
    if (error || (!request.disableResourceLimits
        && (size > request.limits.maxOutputBytes
            || outputBytes > request.limits.maxOutputBytes - size))) return false;
    const auto digest = sha256File(path); if (digest.empty()) return false;
    outputBytes += size; entries.push_back({relative, size, digest, canonical}); return true;
}

[[nodiscard]] std::string capsuleIdentity(const std::string& sourceHash,
    const std::vector<Entry>& entries) {
    std::string material(ConverterContractId); material.push_back('\0');
    material += sourceHash; material.push_back('\0');
    for (const auto& entry : entries) if (entry.canonical) {
        material += entry.path; material.push_back('\0'); material += entry.sha256;
        material.push_back('\0'); material += std::to_string(entry.size); material.push_back('\n');
    }
    return sha256(std::as_bytes(std::span{material.data(), material.size()}));
}

}  // namespace

ConversionOutcome convertLegacyProject(const ConversionRequest& request,
    const ProgressCallback& progress) {
    const auto report = [&](const std::string_view phase, const std::uint64_t completed,
                            const std::uint64_t total, const std::string_view current = {}) {
        if (progress) progress(phase, completed, total, current);
    };
    std::error_code error;
    if (request.input.empty() || request.output.empty()
        || !std::filesystem::is_regular_file(request.input, error) || error
        || std::filesystem::exists(request.output, error))
        return {ConversionOutcome::Status::Rejected,
            "Input must be a regular file and output must not exist.", {}};
    const auto sourceSize = std::filesystem::file_size(request.input, error);
    if (error) return {ConversionOutcome::Status::Failed,
        "The project size could not be read.", {}};
    report("hash", 0, sourceSize, filenameUtf8(request.input));
    const auto sourceHash = sha256File(request.input);
    if (sourceHash.empty()) return {ConversionOutcome::Status::Failed,
        "The project could not be hashed.", {}};
    report("hash", sourceSize, sourceSize, filenameUtf8(request.input));

    const auto spoolPath = request.output.parent_path()
        / (request.output.filename().wstring() + L".pickle-spool-"
            + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    report("parse", 0, sourceSize, filenameUtf8(request.input));
    auto pickle = readLegacyPickle(request.input, spoolPath, request.limits.pickle,
        request.disableResourceLimits, [&](const std::uint64_t completed,
            const std::uint64_t total) { report("parse", completed, total); });
    if (!pickle) return {ConversionOutcome::Status::Rejected, pickle.error, {}};
    report("parse", sourceSize, sourceSize);
    auto& store = *pickle.store;
    std::vector<Diagnostic> projectDiagnostics;
    if (store.kind(pickle.root) != ValueKind::Object
        || store.object(pickle.root).className != "SALSA.Project.project_container.SCTProject")
        return {ConversionOutcome::Status::Rejected,
            "The pickle root is not a legacy SALSA project.", {}};
    const auto version = attributeValue(store, pickle.root, "version");
    if (version == InvalidValueId || store.kind(version) != ValueKind::Integer
        || store.text(version) != "7")
        return {ConversionOutcome::Status::Rejected,
            version != InvalidValueId && store.kind(version) == ValueKind::Integer
                && store.text(version) < "7"
                ? "Only final legacy project version 7 is supported. Open and resave this project in final legacy SALSA."
                : "The project is not an official final legacy version 7 project.", {}};
    if (!exactObject(store, pickle.root, "SALSA.Project.project_container.SCTProject",
            "project", projectDiagnostics))
        return {ConversionOutcome::Status::Rejected, projectDiagnostics.front().message, {}};
    const auto scripts = attributeValue(store, pickle.root, "scts");
    if (scripts == InvalidValueId || store.kind(scripts) != ValueKind::Dictionary)
        return {ConversionOutcome::Status::Rejected,
            "The v7 project script inventory is malformed.", {}};
    const auto scriptEntries = store.entries(scripts);
    if (!request.disableResourceLimits && scriptEntries.size() > request.limits.maxScripts)
        return {ConversionOutcome::Status::Rejected,
            "The project exceeds the script resource limit.", {}};

    std::filesystem::create_directories(request.output, error);
    if (error) return {ConversionOutcome::Status::Failed,
        "The capsule staging directory could not be created.", {}};
    bool ownsOutput = true;
    const auto fail = [&](std::string message) {
        if (ownsOutput) { std::error_code cleanup; std::filesystem::remove_all(request.output, cleanup); }
        return ConversionOutcome{ConversionOutcome::Status::Failed, std::move(message), {}};
    };

    Json normalizations = Json::array(), scriptInventory = Json::array();
    std::vector<Entry> entries; std::uint64_t outputBytes = 0; Counts totals{};
    std::set<std::string> foldedNames; bool actionRequired = false;
    const auto globals = attributeValue(store, pickle.root, "global_variables");
    const auto colors = attributeValue(store, pickle.root, "inst_id_colors");
    if (globals == InvalidValueId || store.kind(globals) != ValueKind::Dictionary)
        projectDiagnostics.push_back({"invalid-global-variables", "project.global_variables",
            "Global variable aliases do not have the official v7 dictionary shape."});
    if (colors == InvalidValueId || store.kind(colors) != ValueKind::Dictionary)
        projectDiagnostics.push_back({"invalid-instruction-colors", "project.inst_id_colors",
            "Instruction colors do not have the official v7 dictionary shape."});
    actionRequired = !projectDiagnostics.empty();

    std::uint32_t ordinal = 0;
    for (const auto& [keyValue, script] : scriptEntries) {
        const auto key = valueString(store, keyValue);
        report("script", ordinal, scriptEntries.size(), key);
        std::vector<Diagnostic> diagnostics;
        if (store.kind(keyValue) != ValueKind::String
            || !exactObject(store, script, "SALSA.Project.project_container.SCTScript",
                "project.scts[" + key + "]", diagnostics)) {
            if (diagnostics.empty()) diagnostics.push_back(
                {"invalid-script", key, "Script entry is malformed."});
        } else {
            std::unordered_set<ValueId> active, visited;
            validateSelectedGraph(store, script, "project.scts[" + key + "]",
                diagnostics, active, visited);
            const auto stored = attributeValue(store, script, "name");
            if (stored == InvalidValueId || store.kind(stored) != ValueKind::String
                || store.text(stored) != key)
                diagnostics.push_back({"script-key-name-mismatch", key,
                    "The dictionary key does not exactly match the stored script name."});
            auto folded = key; std::ranges::transform(folded, folded.begin(),
                [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!foldedNames.insert(folded).second)
                diagnostics.push_back({"duplicate-script-stem", key,
                    "The project contains case-insensitively colliding script stems."});
        }
        normalizePlaceholders(store, script, key, normalizations);
        normalizeStrings(store, script, key, normalizations);
        const auto counts = countScript(store, script);
        totals.sections += counts.sections; totals.instructions += counts.instructions;
        totals.parameters += counts.parameters; totals.links += counts.links;
        totals.strings += counts.strings;
        if (!request.disableResourceLimits && !withinCounts(totals, request.limits))
            diagnostics.push_back({"resource-limit", key,
                "The project exceeds a structural resource limit."});
        const bool accepted = diagnostics.empty(); actionRequired = actionRequired || !accepted;
        std::ostringstream relative; relative << "scripts/" << std::setw(6)
            << std::setfill('0') << ordinal << ".cbor.zlib";
        const auto relativeText = relative.str();
        const auto* begin = reinterpret_cast<const char8_t*>(relativeText.data());
        const auto recordPath = request.output
            / std::filesystem::path(std::u8string(begin, begin + relativeText.size()));
        const auto rawPartPath = recordPath.wstring() + L".uncompressed.part";
        const auto compressedPartPath = recordPath.wstring() + L".part";
        if (!writeScriptRecord(rawPartPath, store, script, ordinal, key,
                valueString(store, attributeValue(store, script, "name")), accepted,
                counts, diagnostics)) return fail("A script capsule record could not be written.");
        if (!compressFile(rawPartPath, compressedPartPath))
            return fail("A script capsule record could not be compressed.");
        std::filesystem::remove(rawPartPath, error);
        if (error) return fail("A temporary script record could not be removed.");
        std::filesystem::rename(compressedPartPath, recordPath, error);
        if (error || !addExistingFile(request.output, relativeText, true,
                entries, outputBytes, request))
            return fail("A script capsule record could not be finalized.");
        scriptInventory.push_back({{"ordinal", ordinal}, {"key", key},
            {"storedName", valueString(store, attributeValue(store, script, "name"))},
            {"status", accepted ? "accepted" : "failed"}, {"sections", counts.sections},
            {"instructions", counts.instructions}, {"parameters", counts.parameters},
            {"strings", counts.strings}, {"diagnostics", diagnostics.size()},
            {"path", relativeText}});
        ++ordinal;
    }

    const auto projectPart = request.output / L"project.cbor.part";
    if (!writeProjectRecord(projectPart, store, pickle.root, normalizations,
            projectDiagnostics)) return fail("The project capsule record could not be written.");
    std::filesystem::rename(projectPart, request.output / L"project.cbor", error);
    if (error || !addExistingFile(request.output, "project.cbor", true,
            entries, outputBytes, request))
        return fail("The project capsule record could not be finalized.");
    std::ranges::sort(entries, {}, &Entry::path);
    if (request.retainOriginal) {
        std::filesystem::create_directories(request.output / L"evidence", error);
        if (error || !std::filesystem::copy_file(request.input,
                request.output / L"evidence" / L"source.prj",
                std::filesystem::copy_options::none, error)
            || !addExistingFile(request.output, "evidence/source.prj", false,
                entries, outputBytes, request))
            return fail("Original project evidence could not be retained.");
    }
    std::ranges::sort(entries, {}, &Entry::path);
    const auto capsuleId = capsuleIdentity(sourceHash, entries);
    Json entryJson = Json::array();
    for (const auto& entry : entries) entryJson.push_back({{"path", entry.path},
        {"size", entry.size}, {"sha256", entry.sha256}, {"canonical", entry.canonical}});
    Json manifest{{"formatId", CapsuleFormatId}, {"schemaVersion", CapsuleSchemaVersion},
        {"converterContractId", ConverterContractId}, {"capsuleId", capsuleId},
        {"status", actionRequired ? "action-required" : "ready"},
        {"source", {{"filename", filenameUtf8(request.input)}, {"size", sourceSize},
            {"sha256", sourceHash}, {"pickleProtocol", pickle.protocol},
            {"projectVersion", 7}, {"originalRetained", request.retainOriginal}}},
        {"normalizationCount", normalizations.size()}, {"scripts", std::move(scriptInventory)},
        {"entries", std::move(entryJson)}};
    // Release the spool before publishing the accepted manifest.
    pickle.store.reset();
    report("finalize", 0, 1, {});
    if (!writeFile(request.output / L"capsule.json", jsonBytes(manifest)))
        return fail("The capsule manifest could not be written.");
    ownsOutput = false; report("finalize", 1, 1, {});
    return {actionRequired ? ConversionOutcome::Status::ActionRequired
                           : ConversionOutcome::Status::Ready,
        actionRequired ? "Capsule created with script decisions required."
                       : "Capsule created.", capsuleId};
}

}  // namespace salsa::legacy
