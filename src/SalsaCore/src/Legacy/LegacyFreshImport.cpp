#include "SalsaCore/Legacy/LegacyFreshImport.h"

#include "SalsaCore/Persistence/AtomicFile.h"

#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctDocumentWorkflow.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctParser.h"
#include "SpiceSCT/SctScptEncoding.h"
#include "SpiceSCT/SctSemanticComparer.h"

#include <Windows.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <unordered_map>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;
using namespace std::string_view_literals;

[[nodiscard]] Diagnostic importError(DiagnosticCode code, std::string message,
    std::optional<std::filesystem::path> path = std::nullopt) {
    return {DiagnosticSeverity::Error, code, std::move(message), std::move(path)};
}

void report(const FreshLegacyImportObserver& observer,
    FreshLegacyImportPhase phase, std::uint64_t completed, std::uint64_t total,
    std::string current = {}) {
    if (observer) observer({phase, completed, total, std::move(current)});
}

[[nodiscard]] std::wstring pathIdentity(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    auto normalized = absolute.lexically_normal().wstring();
    std::ranges::replace(normalized, L'/', L'\\');
    std::ranges::transform(normalized, normalized.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    while (normalized.size() > 3u && normalized.back() == L'\\') normalized.pop_back();
    return normalized;
}

[[nodiscard]] bool nestedPaths(const std::filesystem::path& lhs,
    const std::filesystem::path& rhs) {
    const auto left = pathIdentity(lhs);
    const auto right = pathIdentity(rhs);
    const auto nested = [](const std::wstring& parent, const std::wstring& child) {
        return child.size() > parent.size() && child.starts_with(parent)
            && child[parent.size()] == L'\\';
    };
    return left == right || nested(left, right) || nested(right, left);
}

[[nodiscard]] std::optional<std::filesystem::path> volumeRoot(
    std::filesystem::path path) {
    std::error_code error;
    path = std::filesystem::absolute(path, error);
    if (error) return std::nullopt;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        error.clear();
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    if (path.empty() || error) return std::nullopt;
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    if (!GetVolumePathNameW(path.c_str(), buffer.data(),
            static_cast<DWORD>(buffer.size()))) return std::nullopt;
    auto folded = std::filesystem::path(buffer.data()).lexically_normal().wstring();
    std::ranges::transform(folded, folded.begin(), towlower);
    return std::filesystem::path(folded);
}

[[nodiscard]] bool sameVolume(const std::filesystem::path& lhs,
    const std::filesystem::path& rhs) {
    const auto left = volumeRoot(lhs);
    const auto right = volumeRoot(rhs);
    return left && right && *left == *right;
}

[[nodiscard]] LegacyImportDestinationInspection inspectDestination(
    const std::filesystem::path& path) {
    LegacyImportDestinationInspection result{path};
    if (path.empty()) return result;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.state = GetLastError() == ERROR_FILE_NOT_FOUND
                || GetLastError() == ERROR_PATH_NOT_FOUND
            ? LegacyImportDestinationState::Missing
            : LegacyImportDestinationState::Unreadable;
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.state = LegacyImportDestinationState::ReparsePoint;
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.state = LegacyImportDestinationState::NotDirectory;
        return result;
    }
    std::error_code error;
    const auto empty = std::filesystem::is_empty(path, error);
    result.state = error ? LegacyImportDestinationState::Unreadable
        : empty ? LegacyImportDestinationState::EmptyDirectory
                : LegacyImportDestinationState::NonEmptyDirectory;
    return result;
}

[[nodiscard]] bool validStem(const std::string_view value) {
    if (value.empty() || value == "." || value == ".."
        || value.back() == ' ' || value.back() == '.') return false;
    if (value.find_first_of("<>:\"/\\|?*") != std::string_view::npos
        || std::ranges::any_of(value, [](unsigned char c) { return c < 32u; })) return false;
    auto folded = std::string(value);
    std::ranges::transform(folded, folded.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto dot = folded.find('.');
    const auto base = folded.substr(0, dot);
    static const std::set<std::string, std::less<>> reserved{
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
        "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return !reserved.contains(base);
}

[[nodiscard]] std::string foldedAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] Result<std::vector<std::uint8_t>> readFile(
    const std::filesystem::path& path, std::uint64_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum || size > std::numeric_limits<std::size_t>::max())
        return Result<std::vector<std::uint8_t>>::failure(importError(
            DiagnosticCode::LegacyCapsuleInvalid,
            "A legacy capsule shard exceeds its read limit or cannot be measured.", path));
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::ifstream::traits_type::eof())
        return Result<std::vector<std::uint8_t>>::failure(importError(
            DiagnosticCode::LegacyCapsuleInvalid,
            "A legacy capsule shard could not be read completely.", path));
    return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

[[nodiscard]] Result<Json> readScriptRecord(const std::filesystem::path& path,
    const LegacyCapsuleValidationLimits& limits, const std::uint64_t expectedSize,
    const std::string_view expectedSha256) {
    auto compressed = readFile(path, limits.maxRecordBytes);
    if (!compressed) return Result<Json>::failure(compressed.diagnostics());
    if (compressed.value().size() != expectedSize)
        return Result<Json>::failure(importError(DiagnosticCode::LegacyCapsuleIntegrityFailed,
            "A legacy capsule shard size changed after validation.", path));
    const auto digest = sha256(std::as_bytes(std::span{compressed.value()}));
    if (!digest || digest.value().toHex() != expectedSha256)
        return Result<Json>::failure(importError(DiagnosticCode::LegacyCapsuleIntegrityFailed,
            "A legacy capsule shard hash changed after validation.", path));
    LodePNGDecompressSettings settings = lodepng_default_decompress_settings;
    settings.max_output_size = static_cast<std::size_t>(std::min<std::uint64_t>(
        limits.maxRecordBytes, std::numeric_limits<std::size_t>::max()));
    std::vector<unsigned char> decoded;
    if (lodepng::decompress(decoded, compressed.value().data(),
            compressed.value().size(), settings) != 0
        || decoded.size() > limits.maxRecordBytes)
        return Result<Json>::failure(importError(DiagnosticCode::LegacyCapsuleInvalid,
            "A legacy capsule shard could not be decompressed within its limit.", path));
    try {
        return Result<Json>::success(Json::from_cbor(decoded.begin(), decoded.end(), true, true));
    } catch (const std::exception&) {
        return Result<Json>::failure(importError(DiagnosticCode::LegacyCapsuleInvalid,
            "A legacy capsule shard is malformed.", path));
    }
}

class LegacyGraphView final {
public:
    void addRoot(const Json& root) { scan(root); }

    [[nodiscard]] const Json* resolve(const Json& value) const {
        if (!value.is_array() || value.empty() || !value[0].is_number_unsigned()) return nullptr;
        if (value[0].get<std::uint64_t>() != 0u) return &value;
        if (value.size() != 2u || !value[1].is_number_unsigned()) return nullptr;
        const auto id = value[1].get<std::uint64_t>();
        return id < nodes_.size() ? nodes_[static_cast<std::size_t>(id)] : nullptr;
    }

    [[nodiscard]] std::uint64_t tag(const Json& value) const {
        const auto* node = resolve(value);
        return node == nullptr ? 255u : (*node)[0].get<std::uint64_t>();
    }

    [[nodiscard]] bool isNull(const Json& value) const { return tag(value) == 1u; }

    [[nodiscard]] std::optional<bool> boolean(const Json& value) const {
        const auto* node = resolve(value);
        if (node == nullptr || tag(*node) != 2u || node->size() != 2u
            || !(*node)[1].is_boolean()) return std::nullopt;
        return (*node)[1].get<bool>();
    }

    [[nodiscard]] std::optional<std::int64_t> integer(const Json& value) const {
        const auto* node = resolve(value);
        if (node == nullptr || tag(*node) != 3u || node->size() != 2u
            || !(*node)[1].is_string()) return std::nullopt;
        const auto& text = (*node)[1].get_ref<const std::string&>();
        std::int64_t parsed = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return error == std::errc{} && end == text.data() + text.size()
            ? std::optional<std::int64_t>{parsed} : std::nullopt;
    }

    [[nodiscard]] std::optional<double> floating(const Json& value) const {
        const auto* node = resolve(value);
        if (node == nullptr || tag(*node) != 4u || node->size() != 2u
            || !(*node)[1].is_number_unsigned()) return std::nullopt;
        return std::bit_cast<double>((*node)[1].get<std::uint64_t>());
    }

    [[nodiscard]] std::optional<std::string> string(const Json& value) const {
        const auto* node = resolve(value);
        if (node == nullptr || tag(*node) != 5u || node->size() != 2u
            || !(*node)[1].is_binary()) return std::nullopt;
        const auto& bytes = (*node)[1].get_binary();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> bytes(
        const Json& value) const {
        const auto* node = resolve(value);
        if (node == nullptr || tag(*node) != 6u || node->size() != 2u
            || !(*node)[1].is_binary()) return std::nullopt;
        const auto& bytes = (*node)[1].get_binary();
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

    [[nodiscard]] const Json* sequence(const Json& value) const {
        const auto* node = resolve(value);
        const auto kind = node == nullptr ? 255u : tag(*node);
        if (kind < 7u || kind > 10u || node->size() != 3u || !(*node)[2].is_array())
            return nullptr;
        return &(*node)[2];
    }

    [[nodiscard]] const Json* dictionary(const Json& value) const {
        const auto* node = resolve(value);
        return node != nullptr && tag(*node) == 11u && node->size() == 3u
            && (*node)[2].is_array() ? &(*node)[2] : nullptr;
    }

    [[nodiscard]] const Json* objectField(const Json& value,
        std::uint64_t classCode, std::size_t field) const {
        const auto* node = resolve(value);
        if (node == nullptr || tag(*node) != 12u || node->size() != 4u
            || !(*node)[2].is_number_unsigned()
            || (*node)[2].get<std::uint64_t>() != classCode
            || !(*node)[3].is_array() || field >= (*node)[3].size()) return nullptr;
        return &(*node)[3][field];
    }

    [[nodiscard]] const Json* findStringKey(const Json& dictionaryValue,
        std::string_view key) const {
        const auto* entries = dictionary(dictionaryValue);
        if (entries == nullptr) return nullptr;
        for (const auto& pair : *entries) {
            if (!pair.is_array() || pair.size() != 2u) continue;
            if (const auto candidate = string(pair[0]); candidate && *candidate == key)
                return &pair[1];
        }
        return nullptr;
    }

    [[nodiscard]] const Json* findIntegerKey(const Json& dictionaryValue,
        std::uint32_t key) const {
        const auto* entries = dictionary(dictionaryValue);
        if (entries == nullptr) return nullptr;
        for (const auto& pair : *entries) {
            if (!pair.is_array() || pair.size() != 2u) continue;
            if (const auto candidate = integer(pair[0]); candidate && *candidate == key)
                return &pair[1];
        }
        return nullptr;
    }

private:
    void scan(const Json& encoded) {
        if (!encoded.is_array() || encoded.empty() || !encoded[0].is_number_unsigned()) return;
        const auto tag = encoded[0].get<std::uint64_t>();
        if (tag < 7u || tag > 12u || encoded.size() < 3u
            || !encoded[1].is_number_unsigned()) return;
        const auto id = encoded[1].get<std::uint64_t>();
        if (nodes_.size() <= id) nodes_.resize(static_cast<std::size_t>(id + 1u));
        if (nodes_[static_cast<std::size_t>(id)] != nullptr) return;
        nodes_[static_cast<std::size_t>(id)] = &encoded;
        const auto& children = tag == 12u ? encoded[3] : encoded[2];
        if (!children.is_array()) return;
        if (tag == 11u) {
            for (const auto& pair : children) if (pair.is_array() && pair.size() == 2u) {
                scan(pair[0]); scan(pair[1]);
            }
        } else {
            for (const auto& child : children) scan(child);
        }
    }

    std::vector<const Json*> nodes_{nullptr};
};

[[nodiscard]] std::optional<std::vector<std::uint32_t>> wordsFromBytes(
    const std::vector<std::uint8_t>& bytes,
    const spice::sct::SctDocumentOutputByteOrder byteOrder =
        spice::sct::SctDocumentOutputByteOrder::BigEndian) {
    if (bytes.empty() || bytes.size() % 4u != 0u) return std::nullopt;
    std::vector<std::uint32_t> words;
    words.reserve(bytes.size() / 4u);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4u) {
        if (byteOrder == spice::sct::SctDocumentOutputByteOrder::BigEndian) {
            words.push_back((static_cast<std::uint32_t>(bytes[offset]) << 24u)
                | (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u)
                | (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u)
                | static_cast<std::uint32_t>(bytes[offset + 3u]));
        } else {
            words.push_back(static_cast<std::uint32_t>(bytes[offset])
                | (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u)
                | (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u)
                | (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u));
        }
    }
    return words;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeLegacyText(
    const std::string& utf8, const spice::sct::SctTextEncoding& encoding) {
    if (utf8.empty()) return std::vector<std::uint8_t>{0u};
    const auto wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLength <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), wide.data(), wideLength) != wideLength)
        return std::nullopt;
    const auto encode = [&](const unsigned codePage)
        -> std::optional<std::vector<std::uint8_t>> {
        BOOL usedDefault = FALSE;
        const auto byteLength = WideCharToMultiByte(codePage, 0,
            wide.data(), wideLength, nullptr, 0, nullptr, &usedDefault);
        if (byteLength <= 0 || usedDefault) return std::nullopt;
        std::vector<std::uint8_t> result(static_cast<std::size_t>(byteLength) + 1u);
        usedDefault = FALSE;
        if (WideCharToMultiByte(codePage, 0, wide.data(), wideLength,
                reinterpret_cast<char*>(result.data()), byteLength, nullptr, &usedDefault)
                != byteLength || usedDefault) return std::nullopt;
        result.back() = 0u;
        return result;
    };
    const auto preferred = encoding.characters == spice::sct::SctCharacterEncoding::ShiftJis
        ? 932u : 1252u;
    if (auto result = encode(preferred)) return result;
    // Final legacy SALSA selected its European encoding per string, and v7 did
    // not persist the original byte encoding. Preserve that mixed corpus
    // losslessly by trying the other supported legacy encoding only when the
    // selected preference cannot represent the retained Unicode text.
    return encode(preferred == 932u ? 1252u : 932u);
}

struct TranslationState final {
    const LegacyGraphView& graph;
    const Json& ir;
    const SctPublicationOptions& publication;
    spice::sct::SctDocument document{};
    std::unordered_map<std::string, spice::sct::SctSectionId> sectionIds{};
    std::unordered_map<std::string, spice::sct::SctInstructionId> instructionIds{};
    std::unordered_map<std::string, spice::sct::SctStringId> stringIds{};
    std::set<std::string, std::less<>> footerStringNames{};
    std::vector<std::string> reasons{};
};

struct TranslatedLegacyDocument final {
    spice::sct::SctDocument document{};
    std::vector<LegacyEntityMapping> mappings{};
};

[[nodiscard]] std::optional<std::string> fieldString(const LegacyGraphView& graph,
    const Json& object, std::uint64_t classCode, std::size_t field) {
    const auto* value = graph.objectField(object, classCode, field);
    return value == nullptr ? std::nullopt : graph.string(*value);
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> parameterWords(
    const LegacyGraphView& graph, const Json& parameter,
    const spice::sct::SctDocumentOutputByteOrder sourceByteOrder) {
    const auto* raw = graph.objectField(parameter, 5u, 6u);
    if (raw != nullptr) if (auto bytes = graph.bytes(*raw); bytes && !bytes->empty())
        return wordsFromBytes(*bytes);
    const auto* overrideValue = graph.objectField(parameter, 5u, 8u);
    if (overrideValue != nullptr) if (auto bytes = graph.bytes(*overrideValue); bytes && !bytes->empty())
        return wordsFromBytes(*bytes, sourceByteOrder);
    const auto* value = graph.objectField(parameter, 5u, 5u);
    if (value == nullptr) return std::nullopt;
    const auto* typeValue = graph.objectField(parameter, 5u, 1u);
    const auto type = typeValue == nullptr ? std::nullopt : graph.string(*typeValue);
    const auto scpt = type && type->find("scpt") != std::string::npos;
    const auto finishExpression = [scpt](std::vector<std::uint32_t> words) {
        if (scpt) words.push_back(0x0000001du);
        return words;
    };
    if (const auto integer = graph.integer(*value)) {
        if (!scpt) return std::vector<std::uint32_t>{static_cast<std::uint32_t>(*integer)};
        const auto narrowed = static_cast<float>(*integer);
        return std::vector<std::uint32_t>{0x04000000u,
            std::bit_cast<std::uint32_t>(narrowed), 0x0000001du};
    }
    if (const auto floating = graph.floating(*value)) {
        const auto narrowed = static_cast<float>(*floating);
        return std::vector<std::uint32_t>{0x04000000u, std::bit_cast<std::uint32_t>(narrowed),
            0x0000001du};
    }
    const auto text = graph.string(*value);
    if (!text) return std::nullopt;
    static const std::map<std::string, std::uint32_t, std::less<>> secondary{
        {"Gold", 0x50000000u}, {"Reputation", 0x50000001u},
        {"Vyse.curHP", 0x50000002u}, {"Aika.curHP", 0x50000003u},
        {"Fina.curHP", 0x50000004u}, {"Drachma.curHP", 0x50000005u},
        {"Enrique.curHP", 0x50000006u}, {"Gilder.curHP", 0x50000007u},
        {"Vyse.lvl", 0x5000004au}};
    if (const auto found = secondary.find(*text); found != secondary.end())
        return finishExpression({found->second});
    if (text->starts_with("decimal: ")) {
        const auto body = std::string_view(*text).substr(9u);
        const auto plus = body.find('+');
        const auto slash = body.find('/');
        std::uint32_t whole = 0, fraction = 0;
        if (plus != std::string_view::npos && slash != std::string_view::npos
            && plus < slash) {
            const auto wholeText = body.substr(0, plus);
            const auto fractionText = body.substr(plus + 1u, slash - plus - 1u);
            const auto [wholeEnd, wholeError] = std::from_chars(
                wholeText.data(), wholeText.data() + wholeText.size(), whole);
            const auto [fractionEnd, fractionError] = std::from_chars(
                fractionText.data(), fractionText.data() + fractionText.size(), fraction);
            if (wholeError == std::errc{} && wholeEnd == wholeText.data() + wholeText.size()
                && fractionError == std::errc{}
                && fractionEnd == fractionText.data() + fractionText.size()
                && whole <= 0xffffu && fraction <= 0xffu)
                return finishExpression({0x08000000u | (whole << 8u) | fraction});
        }
    }
    if (text->starts_with("float: ")) {
        const auto body = std::string_view(*text).substr(7u);
        float parsed = 0.0f;
        const auto [end, error] = std::from_chars(
            body.data(), body.data() + body.size(), parsed);
        if (error == std::errc{} && end == body.data() + body.size())
            return finishExpression({0x04000000u, std::bit_cast<std::uint32_t>(parsed)});
    }
    static constexpr std::array prefixes{
        std::pair{"IntVar: "sv, 0x50000000u}, std::pair{"FloatVar: "sv, 0x40000000u},
        std::pair{"BitVar: "sv, 0x20000000u}, std::pair{"ByteVar: "sv, 0x10000000u}};
    for (const auto& [prefix, encodedPrefix] : prefixes) if (text->starts_with(prefix)) {
        std::uint32_t index = 0;
        const auto digits = std::string_view(*text).substr(prefix.size());
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (error == std::errc{} && end == digits.data() + digits.size()
            && index <= 0x00ffffffu)
            return finishExpression({encodedPrefix | index});
    }
    if (text->starts_with("0x")) {
        std::uint32_t word = 0;
        const auto digits = std::string_view(*text).substr(2u);
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), word, 16);
        if (error == std::errc{} && end == digits.data() + digits.size())
            return std::vector<std::uint32_t>{word};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> linkTargetInstruction(
    const LegacyGraphView& graph, const Json& parameter) {
    const auto* link = graph.objectField(parameter, 5u, 2u);
    if (link == nullptr || graph.isNull(*link)) return std::nullopt;
    const auto* trace = graph.objectField(*link, 6u, 3u);
    const auto* items = trace == nullptr ? nullptr : graph.sequence(*trace);
    return items != nullptr && items->size() >= 2u ? graph.string((*items)[1]) : std::nullopt;
}

[[nodiscard]] std::optional<spice::sct::SctDocumentParameter> translateParameter(
    TranslationState& state, const Json& parameter, const spice::sct::SctOpcodeSchema& schema,
    std::uint32_t schemaIndex) {
    using namespace spice::sct;
    const auto* parameterSchema = sctOpcodeParameterSchema(schema, schemaIndex);
    if (parameterSchema == nullptr) {
        state.reasons.push_back("Opcode parameter " + std::to_string(schemaIndex)
            + " has no current SpiceSCT schema entry.");
        return std::nullopt;
    }
    SctDocumentParameter result;
    result.schemaIndex = schemaIndex;
    if (parameterSchema->referenceKind == SctOpcodeReferenceKind::Instruction) {
        const auto target = linkTargetInstruction(state.graph, parameter);
        const auto found = target ? state.instructionIds.find(*target) : state.instructionIds.end();
        if (!target || found == state.instructionIds.end()) {
            state.reasons.push_back("An instruction reference could not be resolved from its legacy trace.");
            return std::nullopt;
        }
        result.value = SctInstructionReference{found->second};
        return result;
    }
    if (const auto textRule = sctOpcodeTextReference(schema, schemaIndex)) {
        const auto* linked = state.graph.objectField(parameter, 5u, 7u);
        const auto text = linked == nullptr ? std::nullopt : state.graph.string(*linked);
        if (!text) {
            state.reasons.push_back("A text reference has no legacy linked-string value.");
            return std::nullopt;
        }
        if (textRule->storage == SctTextStorage::IndexedSection) {
            const auto found = state.stringIds.find(*text);
            if (found == state.stringIds.end()) {
                state.reasons.push_back("An indexed-string reference could not be resolved by legacy name.");
                return std::nullopt;
            }
            result.value = SctStringReference{found->second};
            return result;
        }
        auto footerText = *text;
        if (state.footerStringNames.contains(*text)) {
            const auto* stored = state.graph.findStringKey(state.ir.at("strings"), *text);
            const auto resolved = stored == nullptr ? std::nullopt : state.graph.string(*stored);
            if (!resolved) {
                state.reasons.push_back("A footer string identity has no retained text value.");
                return std::nullopt;
            }
            footerText = *resolved;
        }
        const auto encoded = encodeLegacyText(footerText, state.publication.textEncoding);
        if (!encoded) {
            state.reasons.push_back("A footer string is not encodable under the selected publication profile.");
            return std::nullopt;
        }
        SctDocumentSupplementaryText footer;
        footer.id = state.document.allocateSupplementaryTextId();
        footer.kind = textRule->kind;
        footer.value = SctOpaqueText{*encoded};
        state.document.supplementaryText.push_back(std::move(footer));
        result.value = SctSupplementaryTextReference{state.document.supplementaryText.back().id};
        return result;
    }
    const auto words = parameterWords(state.graph, parameter, state.publication.byteOrder);
    if (!words) {
        const auto* typeValue = state.graph.objectField(parameter, 5u, 1u);
        const auto type = typeValue == nullptr ? std::nullopt : state.graph.string(*typeValue);
        const auto* semanticValue = state.graph.objectField(parameter, 5u, 5u);
        state.reasons.push_back("Opcode " + std::to_string(schema.opcode) + " parameter "
            + std::to_string(schemaIndex) + " (legacy type "
            + (type ? *type : "unknown") + ") has no complete normalized raw-word representation; value tag is "
            + std::to_string(semanticValue == nullptr ? 255u : state.graph.tag(*semanticValue)) + ".");
        return std::nullopt;
    }
    if (parameterSchema->encoding == SctOpcodeParameterEncoding::ScptExpression) {
        auto expressionWords = *words;
        const auto scan = scanSctScptWords(expressionWords);
        if (!scan.complete || scan.wordCount != expressionWords.size()) {
            state.reasons.push_back("Opcode " + std::to_string(schema.opcode) + " parameter "
                + std::to_string(schemaIndex) + " has "
                + std::to_string(expressionWords.size()) + " retained SCPT words, but the first "
                + std::to_string(scan.wordCount) + " words do not form exactly one expression"
                + (expressionWords.empty() ? std::string{"."}
                    : "; first word is " + std::to_string(expressionWords.front())
                        + " and last word is " + std::to_string(expressionWords.back()) + "."));
            return std::nullopt;
        }
        const auto terminated = !expressionWords.empty() && expressionWords.back() == 0x0000001du;
        result.value = SctCanonicalExpression{SctOpaqueExpression{std::move(expressionWords)},
            terminated ? SctExpressionTermination::StopCode
                       : SctExpressionTermination::InlineValue};
    } else if (parameterSchema->encoding == SctOpcodeParameterEncoding::RawWordsUntilSentinel) {
        result.value = SctTerminatedWordSequenceValue{*words};
    } else if (words->size() == 1u) {
        result.value = SctEncodedWordValue{words->front()};
    } else {
        result.value = SctOpaqueParameterValue{*words};
    }
    return result;
}

[[nodiscard]] std::optional<spice::sct::SctDocumentInstruction> translateInstruction(
    TranslationState& state, const Json& legacy) {
    using namespace spice::sct;
    const auto id = fieldString(state.graph, legacy, 4u, 0u);
    const auto* baseValue = state.graph.objectField(legacy, 4u, 1u);
    const auto base = baseValue == nullptr ? std::nullopt : state.graph.integer(*baseValue);
    const auto* encodeValue = state.graph.objectField(legacy, 4u, 11u);
    const auto encode = encodeValue == nullptr ? std::nullopt : state.graph.boolean(*encodeValue);
    if (!id || !base || *base < 0 || *base > std::numeric_limits<std::uint16_t>::max()
        || !encode) {
        state.reasons.push_back("A legacy instruction identity or opcode is malformed.");
        return std::nullopt;
    }
    if (!*encode) return std::nullopt;
    const auto* schema = findSctOpcodeSchema(static_cast<std::uint16_t>(*base));
    if (schema == nullptr) {
        state.reasons.push_back("Opcode " + std::to_string(*base)
            + " is not represented by the current SpiceSCT schema.");
        return std::nullopt;
    }
    SctDocumentInstruction result;
    const auto foundId = state.instructionIds.find(*id);
    if (foundId == state.instructionIds.end()) {
        state.reasons.push_back("A legacy instruction lost its planned canonical identity.");
        return std::nullopt;
    }
    result.id = foundId->second;
    result.opcode = static_cast<std::uint16_t>(*base);
    if (const auto* skip = state.graph.objectField(legacy, 4u, 2u))
        result.skipRefresh = state.graph.boolean(*skip).value_or(false);
    if (const auto* delay = state.graph.objectField(legacy, 4u, 3u);
        delay != nullptr && !state.graph.isNull(*delay)) {
        const auto words = parameterWords(
            state.graph, *delay, state.publication.byteOrder);
        if (!words) {
            state.reasons.push_back("A scheduled instruction has no complete delay expression words.");
            return std::nullopt;
        }
        auto expressionWords = *words;
        const auto terminated = !expressionWords.empty() && expressionWords.back() == 0x0000001du;
        result.scheduledExpression = SctCanonicalExpression{
            SctOpaqueExpression{std::move(expressionWords)},
            terminated ? SctExpressionTermination::StopCode
                       : SctExpressionTermination::InlineValue};
    }
    const auto* parameters = state.graph.objectField(legacy, 4u, 6u);
    const auto repeated = sctOpcodeRepeatedGroup(*schema);
    for (std::uint32_t index = 0; index < schema->parameters.paramCount; ++index) {
        if (repeated && index == repeated->iterationCountParameter) continue;
        if (repeated && index >= repeated->firstParameter && index <= repeated->lastParameter)
            continue;
        const auto* parameter = parameters == nullptr ? nullptr
            : state.graph.findIntegerKey(*parameters, index);
        if (parameter == nullptr) {
            state.reasons.push_back("Opcode " + std::to_string(*base) + " is missing fixed parameter "
                + std::to_string(index) + ".");
            return std::nullopt;
        }
        auto converted = translateParameter(state, *parameter, *schema, index);
        if (!converted) return std::nullopt;
        result.fixedParameters.push_back(std::move(*converted));
    }
    if (repeated) {
        const auto* loopsValue = state.graph.objectField(legacy, 4u, 7u);
        const auto* loops = loopsValue == nullptr ? nullptr : state.graph.sequence(*loopsValue);
        if (loops == nullptr) {
            state.reasons.push_back("A repeated-parameter opcode has malformed legacy loop groups.");
            return std::nullopt;
        }
        for (const auto& loop : *loops) {
            SctDocumentRepeatedParameterGroup group;
            for (std::uint32_t index = repeated->firstParameter;
                 index <= repeated->lastParameter; ++index) {
                const auto* parameter = state.graph.findIntegerKey(loop, index);
                if (parameter == nullptr) {
                    state.reasons.push_back("A legacy repeated-parameter group is incomplete.");
                    return std::nullopt;
                }
                auto converted = translateParameter(state, *parameter, *schema, index);
                if (!converted) return std::nullopt;
                group.parameters.push_back(std::move(*converted));
            }
            result.repeatedParameterGroups.push_back(std::move(group));
        }
    }
    return result;
}

[[nodiscard]] std::optional<TranslatedLegacyDocument> translateDocument(
    const Json& record, const SctPublicationOptions& publication,
    std::vector<std::string>& reasons) {
    using namespace spice::sct;
    if (!record.contains("ir") || !record.at("ir").is_object()) {
        reasons.push_back("The accepted script has no migration IR.");
        return std::nullopt;
    }
    const auto& ir = record.at("ir");
    static constexpr std::array roots{"header", "footer", "string_groups", "strings",
        "sections", "links"};
    LegacyGraphView graph;
    for (const auto* name : roots) graph.addRoot(ir.at(name));
    for (const auto* name : {"folded_sects", "index", "sect_tree", "sect_list",
            "string_garbage", "unused_sections", "errors", "error_sections", "variables"})
        graph.addRoot(ir.at("sidecar").at(name));
    TranslationState state{graph, ir, publication};
    const auto& sectionsValue = ir.at("sections");
    const auto* sectionEntries = graph.dictionary(sectionsValue);
    if (sectionEntries == nullptr) {
        if (graph.isNull(sectionsValue)) return TranslatedLegacyDocument{
            std::move(state.document), {}};
        reasons.push_back("The legacy script section dictionary is malformed.");
        return std::nullopt;
    }
    std::map<std::string, const Json*, std::less<>> sections;
    for (const auto& pair : *sectionEntries) if (pair.is_array() && pair.size() == 2u) {
        if (auto name = graph.string(pair[0])) sections.emplace(std::move(*name), &pair[1]);
    }
    std::vector<std::string> order;
    const auto& orderValue = ir.at("sidecar").at("sect_list");
    if (const auto* items = graph.sequence(orderValue)) {
        for (const auto& item : *items) if (auto name = graph.string(item)) order.push_back(*name);
    } else if (!graph.isNull(orderValue)) {
        reasons.push_back("The legacy script section order is malformed.");
        return std::nullopt;
    }
    if (order.empty()) for (const auto& [name, _] : sections) order.push_back(name);
    const auto& groupsValue = ir.at("string_groups");
    const auto* groups = graph.dictionary(groupsValue);
    const auto isGroup = [&](const std::string& name) {
        return groups != nullptr && graph.findStringKey(groupsValue, name) != nullptr;
    };
    if (groups != nullptr) {
        const auto* footerGroup = graph.findStringKey(groupsValue, "_Footer_");
        const auto* footerItems = footerGroup == nullptr ? nullptr : graph.sequence(*footerGroup);
        if (footerItems != nullptr) for (const auto& item : *footerItems)
            if (const auto name = graph.string(item)) state.footerStringNames.insert(*name);
    }

    if (const auto* strings = graph.dictionary(ir.at("strings"))) {
        for (const auto& pair : *strings) if (pair.is_array() && pair.size() == 2u) {
            if (const auto name = graph.string(pair[0]))
                if (!state.footerStringNames.contains(*name))
                    state.stringIds.emplace(*name, state.document.allocateStringId());
        }
    } else if (!graph.isNull(ir.at("strings"))) {
        reasons.push_back("The legacy indexed-string dictionary is malformed.");
        return std::nullopt;
    }

    for (const auto& name : order) {
        const auto found = sections.find(name);
        if (found == sections.end()) {
            state.reasons.push_back("The legacy section order names an absent section: " + name + ".");
            continue;
        }
        const auto type = fieldString(graph, *found->second, 3u, 10u);
        if (!type) {
            state.reasons.push_back("Legacy section " + name + " has no recognized type.");
            continue;
        }
        const auto* instructionsValue = graph.objectField(*found->second, 3u, 1u);
        const auto* instructionEntries = instructionsValue == nullptr
            ? nullptr : graph.dictionary(*instructionsValue);
        if (instructionEntries == nullptr) continue;
        for (const auto& pair : *instructionEntries) if (pair.is_array() && pair.size() == 2u) {
            const auto id = graph.string(pair[0]);
            const auto* encodeValue = graph.objectField(pair[1], 4u, 11u);
            if (id && (encodeValue == nullptr || graph.boolean(*encodeValue).value_or(true)))
                state.instructionIds.emplace(*id, state.document.allocateInstructionId());
        }
    }

    std::set<std::string, std::less<>> placedStrings;
    const auto appendStringSection = [&](const std::string& stringName) {
        const auto id = state.stringIds.find(stringName);
        const auto* value = graph.findStringKey(ir.at("strings"), stringName);
        const auto text = value == nullptr ? std::nullopt : graph.string(*value);
        if (id == state.stringIds.end() || !text) {
            state.reasons.push_back("Indexed string " + stringName
                + " has no semantic text value or planned identity.");
            return;
        }
        const auto encoded = encodeLegacyText(*text, publication.textEncoding);
        if (!encoded) {
            state.reasons.push_back("Indexed string " + stringName
                + " is not encodable under the selected publication profile.");
            return;
        }
        SctDocumentString string;
        string.id = id->second;
        string.kind = SctTextKind::SctString;
        string.value = SctOpaqueText{*encoded};
        SctDocumentSection stringSection;
        stringSection.id = state.document.allocateSectionId();
        state.sectionIds.emplace(stringName, stringSection.id);
        stringSection.nameBytes = stringName;
        stringSection.content = SctStringSectionContent{std::move(string)};
        state.document.sections.push_back(std::move(stringSection));
        placedStrings.insert(stringName);
    };

    for (const auto& name : order) {
        const auto found = sections.find(name);
        if (found == sections.end()) continue;
        const auto type = fieldString(graph, *found->second, 3u, 10u);
        if (!type) continue;
        if (*type == "String") {
            appendStringSection(name);
            continue;
        }
        SctDocumentSection section;
        section.id = state.document.allocateSectionId();
        state.sectionIds.emplace(name, section.id);
        section.nameBytes = name;
        if (*type == "Label") {
            section.content = SctStringGroupMarkerSectionContent{};
        } else if (*type == "Script" || type->empty()) {
            SctScriptSectionContent content;
            const auto* instructionsValue = graph.objectField(*found->second, 3u, 1u);
            const auto* instructionOrderValue = graph.objectField(*found->second, 3u, 3u);
            const auto* instructionOrder = instructionOrderValue == nullptr
                ? nullptr : graph.sequence(*instructionOrderValue);
            if (instructionsValue == nullptr || instructionOrder == nullptr) {
                state.reasons.push_back("Script section " + name
                    + " has malformed instruction storage or order.");
                continue;
            }
            for (const auto& idValue : *instructionOrder) {
                const auto id = graph.string(idValue);
                const auto* instruction = id ? graph.findStringKey(*instructionsValue, *id) : nullptr;
                if (instruction == nullptr) {
                    state.reasons.push_back("Script section " + name
                        + " references an absent instruction.");
                    continue;
                }
                const auto* encodeValue = graph.objectField(*instruction, 4u, 11u);
                if (encodeValue != nullptr && !graph.boolean(*encodeValue).value_or(true)) continue;
                auto converted = translateInstruction(state, *instruction);
                if (converted) content.instructions.push_back(std::move(*converted));
            }
            section.content = std::move(content);
        } else {
            state.reasons.push_back("Legacy section " + name + " has unsupported type " + *type + ".");
            continue;
        }
        state.document.sections.push_back(std::move(section));
        if (*type == "Label" && isGroup(name)) {
            const auto* groupValue = graph.findStringKey(groupsValue, name);
            const auto* groupItems = groupValue == nullptr ? nullptr : graph.sequence(*groupValue);
            if (groupItems == nullptr) {
                state.reasons.push_back("Legacy string group " + name + " is malformed.");
            } else {
                for (const auto& item : *groupItems) {
                    if (const auto stringName = graph.string(item)) appendStringSection(*stringName);
                    else state.reasons.push_back("Legacy string group " + name
                        + " contains a malformed string identity.");
                }
            }
        }
    }
    for (const auto& [name, _] : state.stringIds)
        if (!placedStrings.contains(name))
            state.reasons.push_back("Indexed string " + name
                + " is not assigned to an ordered legacy string group.");
    reasons.insert(reasons.end(), state.reasons.begin(), state.reasons.end());
    if (!state.reasons.empty()) return std::nullopt;
    std::vector<LegacyEntityMapping> mappings;
    mappings.reserve(state.sectionIds.size() + state.instructionIds.size()
        + state.stringIds.size());
    for (const auto& [identity, id] : state.sectionIds)
        mappings.push_back({LegacyEntityKind::Section, identity, id.value()});
    for (const auto& [identity, id] : state.instructionIds)
        mappings.push_back({LegacyEntityKind::Instruction, identity, id.value()});
    for (const auto& [identity, id] : state.stringIds)
        mappings.push_back({LegacyEntityKind::String, identity, id.value()});
    std::ranges::sort(mappings, [](const auto& left, const auto& right) {
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.legacyIdentity < right.legacyIdentity;
    });
    return TranslatedLegacyDocument{std::move(state.document), std::move(mappings)};
}

[[nodiscard]] std::string metadataRecordId(const FreshLegacyImportPlan& plan,
    const std::optional<std::uint32_t> ordinal, const std::string_view field) {
    std::string material = plan.capsuleId;
    material.push_back('\0');
    material += ordinal ? std::to_string(*ordinal) : "project";
    material.push_back('\0');
    material += field;
    const auto digest = sha256(std::as_bytes(std::span{material.data(), material.size()}));
    return digest ? digest.value().toHex() : std::string{};
}

void addMetadataPlan(FreshLegacyImportPlan& plan, const FreshLegacyImportRequest& request,
    const std::uint32_t scriptCount,
    const std::span<const LegacyProjectDiagnostic> projectDiagnostics) {
    const auto append = [&](const std::optional<std::uint32_t> ordinal,
        const LegacyMetadataKind kind, const std::string_view owner,
        const std::string_view field, LegacyMetadataDisposition disposition,
        std::string reason) {
        const auto recordId = metadataRecordId(plan, ordinal, field);
        const auto decision = std::ranges::find(request.metadataDecisions, recordId,
            &LegacyMetadataDecision::recordId);
        if (decision != request.metadataDecisions.end()
            && decision->action == LegacyMetadataDecisionAction::Drop
            && disposition != LegacyMetadataDisposition::Contract
            && disposition != LegacyMetadataDisposition::Recomputed) {
            disposition = LegacyMetadataDisposition::DroppedByUser;
            reason = "Explicitly discarded for this import; the immutable capsule remains unchanged.";
        }
        plan.metadata.push_back({recordId, kind, ordinal, std::string(owner),
            std::string(field), disposition, std::move(reason)});
    };
    const std::string pendingReason =
        "Retained in the immutable capsule for a typed future promotion adapter.";
    const auto projectDisposition = [&](const std::string_view code) {
        const auto diagnostic = std::ranges::find(projectDiagnostics, code,
            &LegacyProjectDiagnostic::code);
        return diagnostic == projectDiagnostics.end()
            ? std::pair{LegacyMetadataDisposition::Pending, pendingReason}
            : std::pair{LegacyMetadataDisposition::Invalid, diagnostic->message};
    };
    append(std::nullopt, LegacyMetadataKind::RecomputedState, "SCTProject", "version",
        LegacyMetadataDisposition::Contract,
        "Validated as the official final version-7 contract.");
    const auto globals = projectDisposition("invalid-global-variables");
    append(std::nullopt, LegacyMetadataKind::ProjectVariableAliases, "SCTProject",
        "global_variables", globals.first, globals.second);
    const auto colors = projectDisposition("invalid-instruction-colors");
    append(std::nullopt, LegacyMetadataKind::OpcodeColors, "SCTProject",
        "inst_id_colors", colors.first, colors.second);
    static constexpr std::array pendingFields{"folded_sects", "sect_tree", "string_groups",
        "string_garbage", "unused_sections", "errors", "error_sections", "variables",
        "instruction_labels", "instruction_grouping", "suppressed_instructions"};
    static constexpr std::array recomputedFields{"index", "sect_list", "instruction_locations",
        "string_locations", "section_offsets", "instruction_offsets"};
    for (std::uint32_t ordinal = 0; ordinal < scriptCount; ++ordinal) {
        for (const auto* field : pendingFields) {
            LegacyMetadataKind kind = LegacyMetadataKind::PreservedEvidence;
            if (std::string_view(field) == "folded_sects") kind = LegacyMetadataKind::FoldedSections;
            else if (std::string_view(field) == "sect_tree") kind = LegacyMetadataKind::SectionGroups;
            else if (std::string_view(field) == "string_groups") kind = LegacyMetadataKind::StringGroups;
            else if (std::string_view(field) == "variables") kind = LegacyMetadataKind::ScriptVariableAliases;
            else if (std::string_view(field) == "instruction_labels") kind = LegacyMetadataKind::InstructionLabels;
            else if (std::string_view(field) == "instruction_grouping") kind = LegacyMetadataKind::InstructionGroups;
            else if (std::string_view(field) == "suppressed_instructions") kind = LegacyMetadataKind::SuppressedInstructions;
            else if (std::string_view(field) == "errors" || std::string_view(field) == "error_sections"
                || std::string_view(field) == "unused_sections") kind = LegacyMetadataKind::AdvisoryDiagnostics;
            append(ordinal, kind, "SCTScript", field,
                LegacyMetadataDisposition::Pending, pendingReason);
        }
        for (const auto* field : recomputedFields)
            append(ordinal, LegacyMetadataKind::RecomputedState, "SCTScript", field,
                LegacyMetadataDisposition::Recomputed,
                "Recomputed by SpiceSCT layout and current SALSA indexing.");
    }
}

[[nodiscard]] std::string regionName(const LegacyImportRegion region) {
    switch (region) {
    case LegacyImportRegion::NorthAmerica: return "north-america";
    case LegacyImportRegion::Europe: return "europe";
    case LegacyImportRegion::Japan: return "japan";
    case LegacyImportRegion::Unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string scopeName(LegacyImportTargetScope scope) {
    switch (scope) {
    case LegacyImportTargetScope::GameCube: return "gamecube";
    case LegacyImportTargetScope::DreamcastDisc1: return "dreamcast-disc-1";
    case LegacyImportTargetScope::DreamcastDisc2: return "dreamcast-disc-2";
    case LegacyImportTargetScope::UnknownCustom: return "unknown-custom";
    }
    return "unknown-custom";
}

void appendIdentityField(std::string& material, const std::string_view value) {
    material += std::to_string(value.size());
    material.push_back(':');
    material.append(value);
}

[[nodiscard]] std::string profileIdentity(const SctPublicationOptions& options) {
    return std::to_string(static_cast<int>(options.platform)) + ":"
        + std::to_string(static_cast<int>(options.textEncoding.characters)) + ":"
        + std::to_string(static_cast<int>(options.textEncoding.messageSpace)) + ":"
        + std::to_string(static_cast<int>(options.byteOrder)) + ":"
        + std::to_string(static_cast<int>(options.wrapper));
}

}  // namespace

bool FreshLegacyImportPlan::ready() const noexcept {
    return !planId.empty() && sourceDestination.fresh() && workspaceDestination.fresh()
        && std::ranges::any_of(scripts, [](const FreshLegacyScriptPlan& script) {
            return script.status == FreshLegacyScriptPlanStatus::Ready;
        })
        && std::ranges::none_of(scripts, [](const FreshLegacyScriptPlan& script) {
            return script.status == FreshLegacyScriptPlanStatus::Blocked;
        })
        && std::ranges::none_of(metadata, [](const LegacyMetadataPlanRecord& record) {
            return record.disposition == LegacyMetadataDisposition::Blocked
                || record.disposition == LegacyMetadataDisposition::Unsupported
                || record.disposition == LegacyMetadataDisposition::Invalid;
        });
}

static Result<FreshLegacyImportPlan> buildFreshLegacyImportPlan(
    const FreshLegacyImportRequest& request,
    const LegacyCapsuleValidationLimits& limits,
    const std::stop_token stopToken,
    const FreshLegacyImportObserver& observer,
    const std::filesystem::path* stagedSourceDirectory) {
    report(observer, FreshLegacyImportPhase::ValidatingCapsule, 0, 1);
    if (request.capsuleRoot.empty() || request.sourceDirectory.empty()
        || request.workspaceDirectory.empty())
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "Fresh legacy import requires a capsule, source directory, and workspace directory."));
    if (request.targetScope == LegacyImportTargetScope::UnknownCustom
        && request.customTargetName.empty())
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "An unknown/custom target scope requires a durable user-facing name."));
    const auto expectedPlatform = request.targetScope == LegacyImportTargetScope::GameCube
        ? std::optional{spice::sct::SctPlatform::GameCube}
        : request.targetScope == LegacyImportTargetScope::UnknownCustom
            ? std::nullopt : std::optional{spice::sct::SctPlatform::Dreamcast};
    if (expectedPlatform && request.publication.platform != *expectedPlatform)
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "The publication platform contradicts the selected target scope."));
    if (nestedPaths(request.sourceDirectory, request.workspaceDirectory))
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "Fresh source and workspace destinations must be distinct and non-overlapping."));
    if (!sameVolume(request.sourceDirectory, request.workspaceDirectory))
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "Fresh source and workspace destinations must share one filesystem volume."));
    if (stopToken.stop_requested())
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::Cancelled, "Fresh legacy import planning was cancelled."));
    auto capsule = LegacyCapsuleReader::validate(request.capsuleRoot, limits);
    if (!capsule) return Result<FreshLegacyImportPlan>::failure(capsule.diagnostics());
    report(observer, FreshLegacyImportPhase::ValidatingCapsule, 1, 1);

    std::map<std::uint32_t, const FreshLegacyScriptDecision*> decisions;
    for (const auto& decision : request.scriptDecisions) {
        if (decision.ordinal >= capsule.value().scripts.size()
            || !decisions.emplace(decision.ordinal, &decision).second)
            return Result<FreshLegacyImportPlan>::failure(importError(
                DiagnosticCode::LegacyImportInvalidRequest,
                "Script decisions must have unique ordinals present in the capsule."));
        if (decision.publicationOverride) {
            if (expectedPlatform && decision.publicationOverride->platform != *expectedPlatform)
                return Result<FreshLegacyImportPlan>::failure(importError(
                    DiagnosticCode::LegacyImportInvalidRequest,
                    "A script publication override contradicts the selected target scope."));
        }
    }

    report(observer, FreshLegacyImportPhase::InspectingDestinations, 0, 2);
    FreshLegacyImportPlan plan;
    plan.capsuleId = capsule.value().capsuleId;
    plan.converterContractId = capsule.value().converterContractId;
    plan.targetScope = request.targetScope;
    plan.region = request.region;
    plan.customTargetName = request.customTargetName;
    plan.sourceDestination = inspectDestination(request.sourceDirectory);
    report(observer, FreshLegacyImportPhase::InspectingDestinations, 1, 2);
    plan.workspaceDestination = inspectDestination(request.workspaceDirectory);
    report(observer, FreshLegacyImportPhase::InspectingDestinations, 2, 2);
    if (!plan.sourceDestination.fresh() || !plan.workspaceDestination.fresh())
        return Result<FreshLegacyImportPlan>::failure(importError(
            DiagnosticCode::LegacyImportDestinationNotFresh,
            "Fresh legacy import destinations must be missing or completely empty."));

    plan.scripts.reserve(capsule.value().scripts.size());
    std::set<std::string, std::less<>> stems;
    report(observer, FreshLegacyImportPhase::ConvertingScripts, 0,
        capsule.value().scripts.size());
    for (const auto& summary : capsule.value().scripts) {
        if (stopToken.stop_requested())
            return Result<FreshLegacyImportPlan>::failure(importError(
                DiagnosticCode::Cancelled, "Fresh legacy import planning was cancelled."));
        FreshLegacyScriptPlan script;
        script.ordinal = summary.ordinal;
        script.legacyKey = summary.key;
        script.storedName = summary.storedName;
        const auto decision = decisions.find(summary.ordinal);
        const auto* selected = decision == decisions.end() ? nullptr : decision->second;
        script.publication = selected && selected->publicationOverride
            ? *selected->publicationOverride : request.publication;
        script.outputStem = selected && selected->remappedStem
            ? *selected->remappedStem : summary.key;
        script.outputRelativePath = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(script.outputStem.data()), script.outputStem.size()))
            .replace_extension(L".sct");
        if (selected && !selected->include) {
            script.status = FreshLegacyScriptPlanStatus::Excluded;
            script.reasons.push_back("Explicitly excluded from this fresh import plan.");
        } else if (summary.status != LegacyScriptStatus::Accepted) {
            script.reasons.push_back("The capsule marks this script as requiring action; exclude it to plan the remainder.");
        } else if (summary.key != summary.storedName) {
            script.reasons.push_back("The legacy dictionary key and stored script name disagree.");
        } else if (!validStem(script.outputStem)) {
            script.reasons.push_back("The proposed game-facing script stem is not a valid destination filename.");
        } else if (!stems.insert(foldedAscii(script.outputStem)).second) {
            script.reasons.push_back("The proposed script stem collides case-insensitively within this target scope.");
        } else {
            auto record = readScriptRecord(request.capsuleRoot / summary.recordRelativePath,
                limits, summary.compressedSize, summary.compressedSha256);
            if (!record) {
                script.reasons.push_back(record.diagnostics().front().message);
            } else {
                auto document = translateDocument(record.value(), script.publication, script.reasons);
                if (document) {
                    const spice::sct::SctDocumentExportOptions options{
                        script.publication.platform, script.publication.textEncoding,
                        script.publication.byteOrder, script.publication.wrapper};
                    auto encoded = spice::sct::SctDocumentExporter::exportDocument(
                        document->document, options);
                    if (!encoded.success) {
                        for (const auto& diagnostic : encoded.diagnostics)
                            if (diagnostic.severity == spice::sct::SctDiagnosticSeverity::Error)
                                script.reasons.push_back(diagnostic.message);
                    } else {
                        spice::sct::SctParser parser;
                        const auto parsed = parser.parse(encoded.bytes, script.outputStem + ".sct");
                        spice::sct::SctDocumentImportOptions importOptions;
                        importOptions.declaredSourcePlatform = script.publication.platform;
                        importOptions.sourceTextEncoding = script.publication.textEncoding;
                        const auto imported = spice::sct::SctDocumentWorkflow::importForEditing(
                            parsed, importOptions);
                        if (!parsed.parseOk || !imported.import.document
                            || imported.readiness < spice::sct::SctDocumentReadiness::StructurallyValid) {
                            script.reasons.push_back("The generated SCT did not reparse as a structurally valid canonical document.");
                        } else {
                            const auto evidence = imported.import.context.bind(
                                imported.import.context.revisionProvenance());
                            const auto reencoded = spice::sct::SctDocumentExporter::exportDocument(
                                *imported.import.document, options,
                                evidence ? &*evidence : nullptr);
                            const auto reparsed = reencoded.success
                                ? parser.parse(reencoded.bytes, script.outputStem + ".sct")
                                : spice::sct::SctParseResult{};
                            auto comparison = reparsed.parseOk
                                ? spice::sct::SctSemanticComparer{}.compare(parsed, reparsed)
                                : spice::sct::SctSemanticCompareResult{false,
                                    {"The canonical re-encoding did not parse."}};
                            if (!reparsed.parseOk) {
                                for (const auto& diagnostic : reencoded.diagnostics) {
                                    if (diagnostic.severity
                                        == spice::sct::SctDiagnosticSeverity::Error)
                                        comparison.differences.push_back(
                                            "Canonical re-encoding: " + diagnostic.message);
                                }
                                for (const auto& diagnostic : reparsed.diagnostics) {
                                    comparison.differences.push_back("Reparse byte "
                                        + std::to_string(diagnostic.offset)
                                        + (diagnostic.section.empty() ? std::string{}
                                            : " in section " + diagnostic.section)
                                        + ": " + diagnostic.message);
                                }
                            }
                            if (!comparison.equivalent) {
                                script.reasons.push_back("The generated SCT failed canonical reparse semantic equivalence.");
                                script.reasons.insert(script.reasons.end(), comparison.differences.begin(),
                                    comparison.differences.end());
                            } else {
                                const auto digest = sha256(std::as_bytes(std::span{
                                    encoded.bytes.data(), encoded.bytes.size()}));
                                if (!digest) {
                                    script.reasons.push_back("The generated SCT could not be hashed.");
                                } else {
                                    bool staged = true;
                                    if (stagedSourceDirectory != nullptr) {
                                        report(observer, FreshLegacyImportPhase::StagingArtifacts,
                                            plan.scripts.size(), capsule.value().scripts.size(),
                                            summary.key);
                                        const auto destination = *stagedSourceDirectory
                                            / script.outputRelativePath;
                                        const auto written = replaceFileAtomically(destination,
                                            std::as_bytes(std::span{encoded.bytes}));
                                        if (!written) {
                                            staged = false;
                                            script.reasons.push_back(
                                                written.diagnostics().front().message);
                                        }
                                    }
                                    if (staged) {
                                        script.status = FreshLegacyScriptPlanStatus::Ready;
                                        script.outputDigest = digest.value();
                                        script.outputSize = encoded.outputSize;
                                        script.decodedPayloadSize = encoded.decodedPayloadSize;
                                        script.reparseEquivalent = true;
                                        script.entityMappings = std::move(document->mappings);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        plan.scripts.push_back(std::move(script));
        report(observer, FreshLegacyImportPhase::ConvertingScripts,
            plan.scripts.size(), capsule.value().scripts.size(), summary.key);
    }

    addMetadataPlan(plan, request, static_cast<std::uint32_t>(plan.scripts.size()),
        capsule.value().projectDiagnostics);
    std::set<std::string, std::less<>> metadataIds;
    for (const auto& decision : request.metadataDecisions) {
        if (!metadataIds.insert(decision.recordId).second
            || std::ranges::none_of(plan.metadata, [&](const auto& record) {
                return record.recordId == decision.recordId
                    && record.disposition == LegacyMetadataDisposition::DroppedByUser;
            }))
            return Result<FreshLegacyImportPlan>::failure(importError(
                DiagnosticCode::LegacyImportInvalidRequest,
                "Metadata decisions must uniquely identify droppable capsule records."));
    }
    report(observer, FreshLegacyImportPhase::FinalizingPlan, 0, 1);
    std::string identity;
    appendIdentityField(identity, plan.capsuleId);
    appendIdentityField(identity, scopeName(plan.targetScope));
    appendIdentityField(identity, regionName(plan.region));
    appendIdentityField(identity, plan.customTargetName);
    const auto sourceIdentity = pathIdentity(plan.sourceDestination.path);
    const auto workspaceIdentity = pathIdentity(plan.workspaceDestination.path);
    appendIdentityField(identity, {reinterpret_cast<const char*>(sourceIdentity.data()),
        sourceIdentity.size() * sizeof(wchar_t)});
    appendIdentityField(identity, {reinterpret_cast<const char*>(workspaceIdentity.data()),
        workspaceIdentity.size() * sizeof(wchar_t)});
    for (const auto& script : plan.scripts) {
        appendIdentityField(identity, std::to_string(script.ordinal));
        appendIdentityField(identity, std::to_string(static_cast<int>(script.status)));
        appendIdentityField(identity, script.outputStem);
        appendIdentityField(identity, profileIdentity(script.publication));
        appendIdentityField(identity, script.outputDigest
            ? script.outputDigest->toHex() : std::string{});
        for (const auto& mapping : script.entityMappings) {
            appendIdentityField(identity, std::to_string(static_cast<int>(mapping.kind)));
            appendIdentityField(identity, mapping.legacyIdentity);
            appendIdentityField(identity, std::to_string(mapping.currentId));
        }
        for (const auto& reason : script.reasons) appendIdentityField(identity, reason);
    }
    for (const auto& metadata : plan.metadata) {
        appendIdentityField(identity, metadata.scriptOrdinal
            ? std::to_string(*metadata.scriptOrdinal) : "project");
        appendIdentityField(identity, metadata.owner);
        appendIdentityField(identity, metadata.field);
        appendIdentityField(identity,
            std::to_string(static_cast<int>(metadata.disposition)));
        appendIdentityField(identity, metadata.reason);
    }
    const auto digest = sha256(std::as_bytes(std::span{identity.data(), identity.size()}));
    if (!digest) return Result<FreshLegacyImportPlan>::failure(digest.diagnostics());
    plan.planId = digest.value().toHex();
    report(observer, FreshLegacyImportPhase::FinalizingPlan, 1, 1);
    return Result<FreshLegacyImportPlan>::success(std::move(plan));
}

Result<FreshLegacyImportPlan> LegacyFreshImportPlanner::plan(
    const FreshLegacyImportRequest& request,
    const LegacyCapsuleValidationLimits& limits,
    const std::stop_token stopToken,
    const FreshLegacyImportObserver& observer) {
    return buildFreshLegacyImportPlan(request, limits, stopToken, observer, nullptr);
}

Result<FreshLegacyImportPreparation> LegacyFreshImportPreparer::prepare(
    const FreshLegacyImportRequest& request,
    const std::filesystem::path& stagedSourceDirectory,
    const LegacyCapsuleValidationLimits& limits,
    const std::stop_token stopToken,
    const FreshLegacyImportObserver& observer) {
    if (stagedSourceDirectory.empty()
        || nestedPaths(stagedSourceDirectory, request.sourceDirectory)
        || nestedPaths(stagedSourceDirectory, request.workspaceDirectory)
        || nestedPaths(stagedSourceDirectory, request.capsuleRoot))
        return Result<FreshLegacyImportPreparation>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "The private staging directory must be distinct from all import inputs and destinations.",
            stagedSourceDirectory));
    if (!sameVolume(stagedSourceDirectory, request.sourceDirectory))
        return Result<FreshLegacyImportPreparation>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "The private staging directory must share the destination filesystem volume.",
            stagedSourceDirectory));
    std::error_code error;
    if (std::filesystem::exists(stagedSourceDirectory, error) || error)
        return Result<FreshLegacyImportPreparation>::failure(importError(
            DiagnosticCode::LegacyImportInvalidRequest,
            "The private staging directory must not already exist.", stagedSourceDirectory));
    if (!std::filesystem::create_directories(stagedSourceDirectory, error) || error)
        return Result<FreshLegacyImportPreparation>::failure(importError(
            DiagnosticCode::PersistenceWriteFailed,
            "The private import staging directory could not be created.",
            stagedSourceDirectory));
    auto planned = buildFreshLegacyImportPlan(
        request, limits, stopToken, observer, &stagedSourceDirectory);
    if (!planned) {
        std::filesystem::remove_all(stagedSourceDirectory, error);
        return Result<FreshLegacyImportPreparation>::failure(planned.diagnostics());
    }
    return Result<FreshLegacyImportPreparation>::success(
        {request, std::move(planned).takeValue(), stagedSourceDirectory});
}

}  // namespace salsa::core
