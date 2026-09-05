#include "SalsaCore/Legacy/LegacyMetadataPromotion.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"
#include "SalsaCore/Persistence/SctBaselineStore.h"
#include "SalsaCore/Sct/SctAuthoringCatalog.h"
#include "SalsaCore/Project/LocalGameProject.h"

#include <lodepng.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
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

[[nodiscard]] Diagnostic error(std::string message,
    const std::filesystem::path& path = {}) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidWorkspaceTransaction,
        std::move(message), path.empty() ? std::nullopt : std::optional{path}};
}

[[nodiscard]] Result<std::vector<std::byte>> readBytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Result<std::vector<std::byte>>::failure(error(
        "The legacy import-state ledger could not be opened.", path));
    const auto end = input.tellg();
    if (end < 0) return Result<std::vector<std::byte>>::failure(error(
        "The legacy import-state ledger has an invalid size.", path));
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!result.empty() && !input.read(reinterpret_cast<char*>(result.data()), end))
        return Result<std::vector<std::byte>>::failure(error(
            "The legacy import-state ledger could not be read completely.", path));
    return Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] Result<Json> readState(const std::filesystem::path& path,
    std::vector<std::byte>* encoded = nullptr) {
    auto bytes = readBytes(path);
    if (!bytes) return Result<Json>::failure(bytes.diagnostics());
    try {
        auto json = Json::parse(reinterpret_cast<const char*>(bytes.value().data()),
            reinterpret_cast<const char*>(bytes.value().data() + bytes.value().size()));
        if (!json.is_object()
            || json.value("formatId", "") != "jahorta.salsa.legacy-import-state"
            || json.value("schemaVersion", 0u) != 1u || !json.contains("metadata")
            || !json.at("metadata").is_array())
            return Result<Json>::failure(error(
                "The legacy import-state ledger is malformed or unsupported.", path));
        if (encoded != nullptr) *encoded = std::move(bytes).takeValue();
        return Result<Json>::success(std::move(json));
    } catch (const std::exception&) {
        return Result<Json>::failure(error(
            "The legacy import-state ledger is malformed.", path));
    }
}

[[nodiscard]] std::optional<LegacyMetadataDisposition> parseDisposition(
    const std::string_view value) {
    if (value == "applied") return LegacyMetadataDisposition::Applied;
    if (value == "pending") return LegacyMetadataDisposition::Pending;
    if (value == "recomputed") return LegacyMetadataDisposition::Recomputed;
    if (value == "contract") return LegacyMetadataDisposition::Contract;
    if (value == "dropped-by-user") return LegacyMetadataDisposition::DroppedByUser;
    if (value == "blocked") return LegacyMetadataDisposition::Blocked;
    if (value == "unsupported") return LegacyMetadataDisposition::Unsupported;
    if (value == "invalid") return LegacyMetadataDisposition::Invalid;
    return std::nullopt;
}

[[nodiscard]] Result<std::vector<LegacyMetadataPlanRecord>> recordsFrom(
    const Json& state, const std::filesystem::path& path) {
    std::vector<LegacyMetadataPlanRecord> records;
    std::set<std::string, std::less<>> ids;
    try {
        for (const auto& item : state.at("metadata")) {
            LegacyMetadataPlanRecord record;
            record.recordId = item.at("recordId").get<std::string>();
            const auto kind = item.at("kind").get<int>();
            if (kind < static_cast<int>(LegacyMetadataKind::ProjectVariableAliases)
                || kind > static_cast<int>(LegacyMetadataKind::RecomputedState))
                return Result<std::vector<LegacyMetadataPlanRecord>>::failure(error(
                    "A legacy metadata record kind is unsupported.", path));
            record.kind = static_cast<LegacyMetadataKind>(kind);
            if (!item.at("scriptOrdinal").is_null())
                record.scriptOrdinal = item.at("scriptOrdinal").get<std::uint32_t>();
            record.owner = item.at("owner").get<std::string>();
            record.field = item.at("field").get<std::string>();
            const auto disposition = parseDisposition(
                item.at("disposition").get<std::string>());
            if (record.recordId.empty() || !ids.insert(record.recordId).second || !disposition)
                return Result<std::vector<LegacyMetadataPlanRecord>>::failure(error(
                    "Legacy metadata record identities or dispositions are invalid.", path));
            record.disposition = *disposition;
            record.reason = item.at("reason").get<std::string>();
            records.push_back(std::move(record));
        }
    } catch (const std::exception&) {
        return Result<std::vector<LegacyMetadataPlanRecord>>::failure(error(
            "A legacy metadata record is malformed.", path));
    }
    return Result<std::vector<LegacyMetadataPlanRecord>>::success(std::move(records));
}

[[nodiscard]] std::string digestOf(const std::span<const std::byte> bytes) {
    const auto digest = sha256(bytes);
    return digest ? digest.value().toHex() : std::string{};
}

[[nodiscard]] std::vector<std::byte> encode(Json value) {
    auto text = value.dump(2);
    text.push_back('\n');
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

[[nodiscard]] Result<Json> readCbor(const std::filesystem::path& path) {
    auto bytes = readBytes(path);
    if (!bytes) return Result<Json>::failure(bytes.diagnostics());
    try {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
        return Result<Json>::success(Json::from_cbor(
            begin, begin + bytes.value().size(), true, true));
    } catch (const std::exception&) {
        return Result<Json>::failure(error("A retained legacy CBOR record is malformed.", path));
    }
}

[[nodiscard]] Result<Json> readScriptCbor(const std::filesystem::path& path) {
    auto compressed = readBytes(path);
    if (!compressed) return Result<Json>::failure(compressed.diagnostics());
    LodePNGDecompressSettings settings = lodepng_default_decompress_settings;
    settings.max_output_size = 256u * 1024u * 1024u;
    std::vector<unsigned char> decoded;
    const auto* begin = reinterpret_cast<const unsigned char*>(compressed.value().data());
    if (lodepng::decompress(decoded, begin, compressed.value().size(), settings) != 0)
        return Result<Json>::failure(error(
            "A retained legacy script record could not be decompressed.", path));
    try {
        return Result<Json>::success(Json::from_cbor(
            decoded.begin(), decoded.end(), true, true));
    } catch (const std::exception&) {
        return Result<Json>::failure(error(
            "A retained legacy script record is malformed.", path));
    }
}

class PromotionGraphView final {
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
        return node && node->is_array() && !node->empty() && (*node)[0].is_number_unsigned()
            ? (*node)[0].get<std::uint64_t>() : 255u;
    }
    [[nodiscard]] std::optional<std::int64_t> integer(const Json& value) const {
        const auto* node = resolve(value);
        if (!node || tag(*node) != 3u || node->size() != 2u || !(*node)[1].is_string())
            return std::nullopt;
        const auto& source = (*node)[1].get_ref<const std::string&>();
        std::int64_t parsed = 0;
        const auto [end, issue] = std::from_chars(
            source.data(), source.data() + source.size(), parsed);
        return issue == std::errc{} && end == source.data() + source.size()
            ? std::optional{parsed} : std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> string(const Json& value) const {
        const auto* node = resolve(value);
        if (!node || tag(*node) != 5u || node->size() != 2u || !(*node)[1].is_binary())
            return std::nullopt;
        const auto& bytes = (*node)[1].get_binary();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    [[nodiscard]] const Json* sequence(const Json& value) const {
        const auto* node = resolve(value);
        const auto kind = node ? tag(*node) : 255u;
        return kind >= 7u && kind <= 10u && node->size() == 3u && (*node)[2].is_array()
            ? &(*node)[2] : nullptr;
    }
    [[nodiscard]] const Json* dictionary(const Json& value) const {
        const auto* node = resolve(value);
        return node && tag(*node) == 11u && node->size() == 3u && (*node)[2].is_array()
            ? &(*node)[2] : nullptr;
    }
    [[nodiscard]] const Json* findStringKey(
        const Json& dictionaryValue, const std::string_view key) const {
        const auto* entries = dictionary(dictionaryValue);
        if (!entries) return nullptr;
        for (const auto& pair : *entries) if (pair.is_array() && pair.size() == 2u) {
            if (const auto candidate = string(pair[0]); candidate && *candidate == key)
                return &pair[1];
        }
        return nullptr;
    }
private:
    void scan(const Json& encoded) {
        if (!encoded.is_array() || encoded.empty() || !encoded[0].is_number_unsigned()) return;
        const auto kind = encoded[0].get<std::uint64_t>();
        if (kind < 7u || kind > 12u || encoded.size() < 3u
            || !encoded[1].is_number_unsigned()) return;
        const auto id = encoded[1].get<std::uint64_t>();
        if (nodes_.size() <= id) nodes_.resize(static_cast<std::size_t>(id + 1u));
        if (nodes_[static_cast<std::size_t>(id)]) return;
        nodes_[static_cast<std::size_t>(id)] = &encoded;
        const auto& children = kind == 12u ? encoded[3] : encoded[2];
        if (!children.is_array()) return;
        if (kind == 11u) {
            for (const auto& pair : children) if (pair.is_array() && pair.size() == 2u) {
                scan(pair[0]); scan(pair[1]);
            }
        } else for (const auto& child : children) scan(child);
    }
    std::vector<const Json*> nodes_{nullptr};
};

[[nodiscard]] std::optional<SctVariableKind> variableKind(
    const std::string_view name) {
    if (name == "BitVar") return SctVariableKind::Bit;
    if (name == "ByteVar") return SctVariableKind::Byte;
    if (name == "IntVar") return SctVariableKind::Integer;
    if (name == "FloatVar") return SctVariableKind::Float;
    return std::nullopt;
}

[[nodiscard]] Result<std::vector<SctVariableAlias>> extractAliases(
    const PromotionGraphView& graph, const Json& root) {
    std::vector<SctVariableAlias> result;
    const auto* kinds = graph.dictionary(root);
    if (!kinds) return Result<std::vector<SctVariableAlias>>::failure(error(
        "Legacy variable aliases do not have the official v7 dictionary shape."));
    for (const auto& kindPair : *kinds) {
        if (!kindPair.is_array() || kindPair.size() != 2u) continue;
        const auto name = graph.string(kindPair[0]);
        const auto kind = name ? variableKind(*name) : std::nullopt;
        const auto* variables = graph.dictionary(kindPair[1]);
        if (!kind || !variables) return Result<std::vector<SctVariableAlias>>::failure(error(
            "Legacy variable aliases contain an unsupported variable collection."));
        for (const auto& pair : *variables) {
            if (!pair.is_array() || pair.size() != 2u) continue;
            const auto index = graph.integer(pair[0]);
            const auto* aliasValue = graph.findStringKey(pair[1], "alias");
            const auto alias = aliasValue ? graph.string(*aliasValue) : std::nullopt;
            if (!index || *index < 0
                || static_cast<std::uint64_t>(*index) > std::numeric_limits<std::uint32_t>::max()
                || !alias)
                return Result<std::vector<SctVariableAlias>>::failure(error(
                    "A legacy variable alias record is malformed."));
            if (!alias->empty()) result.push_back({{*kind,
                static_cast<std::uint32_t>(*index)}, *alias});
        }
    }
    std::ranges::sort(result, {}, &SctVariableAlias::variable);
    return Result<std::vector<SctVariableAlias>>::success(std::move(result));
}

[[nodiscard]] Result<std::vector<SctOpcodeColor>> extractColors(
    const PromotionGraphView& graph, const Json& root) {
    std::vector<SctOpcodeColor> result;
    const auto* colors = graph.dictionary(root);
    if (!colors) return Result<std::vector<SctOpcodeColor>>::failure(error(
        "Legacy opcode colors do not have the official v7 dictionary shape."));
    for (const auto& pair : *colors) {
        if (!pair.is_array() || pair.size() != 2u) continue;
        const auto opcode = graph.integer(pair[0]);
        auto text = graph.string(pair[1]);
        if (!opcode || *opcode < 0 || *opcode > std::numeric_limits<std::uint16_t>::max()
            || !text) return Result<std::vector<SctOpcodeColor>>::failure(error(
                "A legacy opcode color record is malformed."));
        if (text->empty()) continue;
        if (text->starts_with('#')) text->erase(text->begin());
        std::uint32_t color = 0;
        const auto [end, issue] = std::from_chars(
            text->data(), text->data() + text->size(), color, 16);
        if (text->size() != 6u || issue != std::errc{} || end != text->data() + text->size())
            return Result<std::vector<SctOpcodeColor>>::failure(error(
                "A legacy opcode color is not a #RRGGBB value."));
        result.push_back({static_cast<std::uint16_t>(*opcode), color});
    }
    std::ranges::sort(result, {}, &SctOpcodeColor::opcode);
    return Result<std::vector<SctOpcodeColor>>::success(std::move(result));
}

struct ScriptPromotionContext final {
    AssetLocator locator;
    std::unordered_map<std::string, spice::sct::SctSectionId> sections{};
    Json record{};
};

[[nodiscard]] Result<ScriptPromotionContext> scriptContext(
    const LegacyMetadataPlanRecord& record, const LocalSalsaWorkspace& workspace,
    const std::filesystem::path& capsuleRoot) {
    if (!record.scriptOrdinal) return Result<ScriptPromotionContext>::failure(error(
        "A script metadata record has no script ordinal."));
    const auto capsuleId = capsuleRoot.parent_path().filename().string();
    const auto statePath = workspace.componentPath(workspace.descriptor().components.importState
        / (capsuleId + ".json"));
    auto state = readState(statePath);
    if (!state) return Result<ScriptPromotionContext>::failure(state.diagnostics());
    std::optional<AssetLocator> locator;
    for (const auto& mapping : state.value().at("assetMappings")) {
        if (mapping.value("legacyOrdinal", std::numeric_limits<std::uint32_t>::max())
            != *record.scriptOrdinal) continue;
        auto parsed = AssetLocator::fromRelativePath(
            pathFromUtf8(mapping.value("asset", std::string{})));
        if (!parsed) return Result<ScriptPromotionContext>::failure(parsed.diagnostics());
        locator = std::move(parsed).takeValue();
        break;
    }
    if (!locator) return Result<ScriptPromotionContext>::failure(error(
        "The imported script has no current asset mapping.", statePath));
    std::unordered_map<std::string, spice::sct::SctSectionId> sections;
    for (const auto& mapping : state.value().at("entityMappings")) {
        if (mapping.value("legacyOrdinal", std::numeric_limits<std::uint32_t>::max())
                == *record.scriptOrdinal
            && mapping.value("kind", -1) == static_cast<int>(LegacyEntityKind::Section))
            sections.emplace(mapping.value("legacyIdentity", std::string{}),
                spice::sct::SctSectionId(mapping.value("currentId", 0ull)));
    }
    auto capsule = LegacyCapsuleReader::validate(capsuleRoot);
    if (!capsule) return Result<ScriptPromotionContext>::failure(capsule.diagnostics());
    const auto script = std::ranges::find(capsule.value().scripts, *record.scriptOrdinal,
        &LegacyScriptSummary::ordinal);
    if (script == capsule.value().scripts.end())
        return Result<ScriptPromotionContext>::failure(error(
            "The retained capsule has no matching script record.", capsuleRoot));
    auto scriptRecord = readScriptCbor(capsuleRoot / script->recordRelativePath);
    if (!scriptRecord) return Result<ScriptPromotionContext>::failure(
        scriptRecord.diagnostics());
    return Result<ScriptPromotionContext>::success({
        std::move(*locator), std::move(sections), std::move(scriptRecord).takeValue()});
}

struct LogicalFolder final {
    std::string name{};
    std::vector<spice::sct::SctSectionId> sections{};
    std::optional<std::size_t> parent{};
};

[[nodiscard]] Result<std::vector<LogicalFolder>> extractFolders(
    const PromotionGraphView& graph, const Json& root,
    const std::unordered_map<std::string, spice::sct::SctSectionId>& mappings) {
    std::vector<LogicalFolder> result;
    const auto collectNames = [&](const auto& self, const Json& sequence,
                                  std::vector<std::string>& names) -> bool {
        const auto* items = graph.sequence(sequence);
        if (!items) return false;
        for (const auto& item : *items) {
            if (const auto name = graph.string(item)) names.push_back(*name);
            else if (const auto* entries = graph.dictionary(item);
                    entries && entries->size() == 1u && (*entries)[0].is_array()
                    && (*entries)[0].size() == 2u) {
                auto group = graph.string((*entries)[0][0]);
                if (!group) return false;
                if (group->ends_with("|group")) group->resize(group->size() - 6u);
                names.push_back(*group);
                if (!self(self, (*entries)[0][1], names)) return false;
            } else return false;
        }
        return true;
    };
    const auto walk = [&](const auto& self, const Json& sequence,
                          const std::optional<std::size_t> parent) -> bool {
        const auto* items = graph.sequence(sequence);
        if (!items) return false;
        for (const auto& item : *items) {
            const auto* entries = graph.dictionary(item);
            if (!entries) continue;
            if (entries->size() != 1u || !(*entries)[0].is_array()
                || (*entries)[0].size() != 2u) return false;
            auto group = graph.string((*entries)[0][0]);
            if (!group) return false;
            if (group->ends_with("|group")) group->resize(group->size() - 6u);
            std::vector<std::string> names{*group};
            if (!collectNames(collectNames, (*entries)[0][1], names)) return false;
            LogicalFolder folder{*group, {}, parent};
            for (const auto& name : names) {
                const auto found = mappings.find(name);
                if (found == mappings.end()) return false;
                if (std::ranges::find(folder.sections, found->second) == folder.sections.end())
                    folder.sections.push_back(found->second);
            }
            const auto index = result.size();
            result.push_back(std::move(folder));
            if (!self(self, (*entries)[0][1], index)) return false;
        }
        return true;
    };
    if (graph.tag(root) == 1u)
        return Result<std::vector<LogicalFolder>>::success({});
    if (!walk(walk, root, std::nullopt))
        return Result<std::vector<LogicalFolder>>::failure(error(
            "Legacy section groups are malformed or reference an unmapped section."));
    return Result<std::vector<LogicalFolder>>::success(std::move(result));
}

[[nodiscard]] Result<std::vector<std::byte>> envelopeBytes(
    const PatchEnvelope& envelope) {
    auto text = PatchEnvelopeCodec::serialize(envelope);
    if (!text) return Result<std::vector<std::byte>>::failure(text.diagnostics());
    const auto bytes = std::as_bytes(std::span{text.value().data(), text.value().size()});
    return Result<std::vector<std::byte>>::success({bytes.begin(), bytes.end()});
}

[[nodiscard]] Result<WorkspaceArtifactMutation> replacementMutation(
    const LocalSalsaWorkspace& workspace, const std::filesystem::path& absolute,
    std::vector<std::byte> replacement) {
    const auto relative = absolute.lexically_normal().lexically_relative(
        workspace.descriptor().root.lexically_normal());
    if (relative.empty() || relative.is_absolute()
        || (!relative.empty() && *relative.begin() == L".."))
        return Result<WorkspaceArtifactMutation>::failure(error(
            "A promoted metadata artifact resolves outside the workspace.", absolute));
    std::error_code issue;
    const auto exists = std::filesystem::exists(absolute, issue);
    if (issue) return Result<WorkspaceArtifactMutation>::failure(error(
        "A promoted metadata artifact could not be inspected.", absolute));
    std::optional<std::string> digest;
    if (exists) {
        auto current = readBytes(absolute);
        if (!current) return Result<WorkspaceArtifactMutation>::failure(current.diagnostics());
        auto hashed = sha256(current.value());
        if (!hashed) return Result<WorkspaceArtifactMutation>::failure(hashed.diagnostics());
        digest = hashed.value().toHex();
    }
    return Result<WorkspaceArtifactMutation>::success({relative, exists,
        std::move(digest), std::move(replacement)});
}

struct ImportedProjectMetadata final {
    std::vector<SctVariableAlias> aliases{};
    std::vector<SctOpcodeColor> colors{};
};

[[nodiscard]] Result<ImportedProjectMetadata> importedProjectMetadata(
    const std::filesystem::path& capsuleRoot) {
    auto record = readCbor(capsuleRoot / L"project.cbor");
    if (!record) return Result<ImportedProjectMetadata>::failure(record.diagnostics());
    try {
        const auto& fields = record.value().at("fields");
        const auto& aliasRoot = fields.at("global_variables");
        const auto& colorRoot = fields.at("inst_id_colors");
        PromotionGraphView graph;
        graph.addRoot(aliasRoot); graph.addRoot(colorRoot);
        auto aliases = extractAliases(graph, aliasRoot);
        if (!aliases) return Result<ImportedProjectMetadata>::failure(aliases.diagnostics());
        auto colors = extractColors(graph, colorRoot);
        if (!colors) return Result<ImportedProjectMetadata>::failure(colors.diagnostics());
        return Result<ImportedProjectMetadata>::success({
            std::move(aliases).takeValue(), std::move(colors).takeValue()});
    } catch (const std::exception&) {
        return Result<ImportedProjectMetadata>::failure(error(
            "The retained legacy project metadata record is malformed.", capsuleRoot));
    }
}

struct ImportedScriptMetadata final {
    ScriptPromotionContext context;
    std::vector<SctVariableAlias> aliases{};
    std::vector<LogicalFolder> folders{};
};

[[nodiscard]] Result<ImportedScriptMetadata> importedScriptMetadata(
    const LegacyMetadataPlanRecord& record, const LocalSalsaWorkspace& workspace,
    const std::filesystem::path& capsuleRoot) {
    auto context = scriptContext(record, workspace, capsuleRoot);
    if (!context) return Result<ImportedScriptMetadata>::failure(context.diagnostics());
    try {
        const auto& sidecar = context.value().record.at("ir").at("sidecar");
        const auto& aliasRoot = sidecar.at("variables");
        const auto& treeRoot = sidecar.at("sect_tree");
        PromotionGraphView graph;
        graph.addRoot(aliasRoot); graph.addRoot(treeRoot);
        auto aliases = extractAliases(graph, aliasRoot);
        if (!aliases) return Result<ImportedScriptMetadata>::failure(aliases.diagnostics());
        auto folders = extractFolders(graph, treeRoot, context.value().sections);
        if (!folders) return Result<ImportedScriptMetadata>::failure(folders.diagnostics());
        return Result<ImportedScriptMetadata>::success({
            std::move(context).takeValue(), std::move(aliases).takeValue(),
            std::move(folders).takeValue()});
    } catch (const std::exception&) {
        return Result<ImportedScriptMetadata>::failure(error(
            "The retained legacy script metadata record is malformed.", capsuleRoot));
    }
}

[[nodiscard]] std::optional<std::string> mergeProjectMetadata(
    SctWorkspaceAuthoringState& state, const ImportedProjectMetadata& imported,
    const bool includeAliases, const bool includeColors) {
    if (includeAliases) for (const auto& alias : imported.aliases) {
        const auto found = std::ranges::find(state.projectAliases,
            alias.variable, &SctVariableAlias::variable);
        if (found != state.projectAliases.end()) {
            if (found->alias != alias.alias)
                return "A project variable already has a different alias.";
        } else {
            const auto folded = [](std::string text) {
                std::ranges::transform(text, text.begin(), [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                return text;
            };
            if (std::ranges::any_of(state.projectAliases, [&](const auto& existing) {
                    return folded(existing.alias) == folded(alias.alias);
                })) return "A legacy project alias duplicates a current alias name.";
            state.projectAliases.push_back(alias);
        }
    }
    if (includeColors) for (const auto color : imported.colors) {
        const auto found = std::ranges::find(state.opcodeColors,
            color.opcode, &SctOpcodeColor::opcode);
        if (found != state.opcodeColors.end()) {
            if (found->colorRgb != color.colorRgb)
                return "An opcode already has a different project color.";
        } else state.opcodeColors.push_back(color);
    }
    return std::nullopt;
}

[[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> projectMutations(
    const LocalSalsaWorkspace& workspace, const std::filesystem::path& capsuleRoot,
    const std::span<const LegacyMetadataPlanRecord> records) {
    const bool includeAliases = std::ranges::any_of(records, [](const auto& record) {
        return record.kind == LegacyMetadataKind::ProjectVariableAliases;
    });
    const bool includeColors = std::ranges::any_of(records, [](const auto& record) {
        return record.kind == LegacyMetadataKind::OpcodeColors;
    });
    if ((!includeAliases && !includeColors) || std::ranges::any_of(records,
            [](const auto& record) { return record.scriptOrdinal.has_value(); }))
        return Result<std::vector<WorkspaceArtifactMutation>>::failure(error(
            "The selected records are not a valid project metadata group."));
    auto imported = importedProjectMetadata(capsuleRoot);
    if (!imported) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        imported.diagnostics());
    const auto path = workspace.componentPath(
        workspace.descriptor().components.authoring / L"workspace.json");
    auto current = SctWorkspaceAuthoringStore(path).load();
    if (!current) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        current.diagnostics());
    auto state = std::move(current).takeValue();
    if (const auto conflict = mergeProjectMetadata(state, imported.value(),
            includeAliases, includeColors))
        return Result<std::vector<WorkspaceArtifactMutation>>::failure(error(*conflict, path));
    auto encoded = SctAuthoringCatalogCodec::serializeWorkspace(state);
    if (!encoded) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        encoded.diagnostics());
    auto mutation = replacementMutation(workspace, path, std::move(encoded).takeValue());
    if (!mutation) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        mutation.diagnostics());
    return Result<std::vector<WorkspaceArtifactMutation>>::success(
        {std::move(mutation).takeValue()});
}

[[nodiscard]] std::optional<std::string> mergeScriptMetadata(
    SctSemanticState& state, const ImportedScriptMetadata& imported,
    const bool includeAliases, const bool includeFolders) {
    if (includeAliases) for (const auto& alias : imported.aliases) {
        const auto found = std::ranges::find(state.aliases,
            alias.variable, &SctVariableAlias::variable);
        if (found != state.aliases.end()) {
            if (found->alias != alias.alias)
                return "A script variable already has a different alias.";
        } else state.aliases.push_back(alias);
    }
    if (!includeFolders) return std::nullopt;
    std::uint64_t nextId = 1;
    for (const auto& folder : state.folders)
        nextId = std::max(nextId, folder.id.value + 1u);
    const bool hadFolders = !state.folders.empty();
    std::vector<SctSectionFolderId> ids;
    ids.reserve(imported.folders.size());
    for (std::size_t index = 0; index < imported.folders.size(); ++index) {
        const auto& incoming = imported.folders[index];
        const auto expectedParent = incoming.parent
            ? std::optional{ids.at(*incoming.parent)} : std::nullopt;
        const auto found = std::ranges::find_if(state.folders, [&](const auto& existing) {
            return existing.name == incoming.name && existing.sections == incoming.sections;
        });
        if (found != state.folders.end()) {
            if (found->parent != expectedParent)
                return "A matching section folder has a different parent.";
            ids.push_back(found->id);
            continue;
        }
        if (hadFolders)
            return "Current section folders overlap a legacy folder plan that has not been applied.";
        SctSectionFolder folder{{nextId++}, expectedParent,
            incoming.name, incoming.sections};
        ids.push_back(folder.id);
        state.folders.push_back(std::move(folder));
    }
    return std::nullopt;
}

struct LoadedPromotionScript final {
    LocalGameProject project;
    SctPatchedLoadResult loaded{};
};

[[nodiscard]] Result<LoadedPromotionScript> loadPromotionScript(
    const LocalSalsaWorkspace& workspace, const AssetLocator& locator) {
    const auto& dataset = workspace.descriptor().dataset;
    auto project = LocalGameProject::inspect({dataset.root,
        dataset.identity.platform, dataset.identity.region});
    if (!project) return Result<LoadedPromotionScript>::failure(project.diagnostics());
    auto value = std::move(project).takeValue();
    auto loaded = SctPatchCheckpointService::load(value, &workspace, &workspace, locator);
    if (!loaded.load.succeeded() || loaded.patchConflict) {
        auto diagnostics = loaded.load.infrastructureDiagnostics;
        if (diagnostics.empty()) diagnostics.push_back(error(
            "The current script could not be loaded for metadata promotion.", locator.path()));
        return Result<LoadedPromotionScript>::failure(std::move(diagnostics));
    }
    return Result<LoadedPromotionScript>::success(
        {std::move(value), std::move(loaded)});
}

[[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> scriptMutations(
    const LegacyMetadataPlanRecord& record, const LocalSalsaWorkspace& workspace,
    const std::filesystem::path& capsuleRoot,
    const std::span<const LegacyMetadataPlanRecord> records) {
    const bool includeAliases = std::ranges::any_of(records, [](const auto& selected) {
        return selected.kind == LegacyMetadataKind::ScriptVariableAliases;
    });
    const bool includeFolders = std::ranges::any_of(records, [](const auto& selected) {
        return selected.kind == LegacyMetadataKind::SectionGroups;
    });
    if ((!includeAliases && !includeFolders) || !record.scriptOrdinal
        || std::ranges::any_of(records, [&](const auto& selected) {
            return selected.scriptOrdinal != record.scriptOrdinal;
        }))
        return Result<std::vector<WorkspaceArtifactMutation>>::failure(error(
            "The selected records are not a valid script metadata group."));
    auto imported = importedScriptMetadata(record, workspace, capsuleRoot);
    if (!imported) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        imported.diagnostics());
    auto loaded = loadPromotionScript(workspace, imported.value().context.locator);
    if (!loaded) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        loaded.diagnostics());
    auto& current = loaded.value().loaded;
    SctSemanticState working{current.load.document->document,
        current.authoredArms, current.textRepairs, current.unboundReferences,
        current.aliases, current.annotations, current.folders};
    if (const auto conflict = mergeScriptMetadata(working, imported.value(),
            includeAliases, includeFolders))
        return Result<std::vector<WorkspaceArtifactMutation>>::failure(error(
            *conflict, imported.value().context.locator.path()));
    const SctSemanticState baseline{current.baseline->document};
    auto patch = SalsaScriptPatchService::diff(baseline, working,
        current.baseline->provenance->textConvention);
    if (!patch) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        patch.diagnostics());
    auto payload = SalsaScriptPatchCodec::serialize(patch.value());
    if (!payload) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        payload.diagnostics());
    const auto& source = current.baseline->provenance->source();
    PatchEnvelope envelope{
        current.baseline->provenance->inspection->sourceDatasetFingerprint,
        {{source.descriptor.locator, source.descriptor.revision}}, {},
        {std::string(SalsaScriptPatchCodec::PayloadType),
            SalsaScriptPatchCodec::SchemaVersion, std::move(payload).takeValue()}};
    auto envelopeData = envelopeBytes(envelope);
    if (!envelopeData) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        envelopeData.diagnostics());
    auto patchMutation = replacementMutation(workspace,
        workspace.patchPath(imported.value().context.locator),
        std::move(envelopeData).takeValue());
    if (!patchMutation) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        patchMutation.diagnostics());
    const auto baselinePath = DirectorySctBaselineStore(workspace.componentPath(
        workspace.descriptor().components.baselines)).path(source.descriptor.revision);
    auto baselineMutation = replacementMutation(workspace, baselinePath, source.bytes);
    if (!baselineMutation) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
        baselineMutation.diagnostics());
    return Result<std::vector<WorkspaceArtifactMutation>>::success({
        std::move(patchMutation).takeValue(),
        std::move(baselineMutation).takeValue()});
}

class ProjectMetadataAdapter final : public LegacyMetadataPromotionAdapter {
public:
    explicit ProjectMetadataAdapter(const LegacyMetadataKind kind) : kind_(kind) {}
    [[nodiscard]] LegacyMetadataKind kind() const noexcept override { return kind_; }
    [[nodiscard]] LegacyMetadataPromotionAssessment assess(
        const LegacyMetadataPlanRecord&, const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        const auto imported = importedProjectMetadata(capsuleRoot);
        if (!imported) return {false, false, imported.diagnostics().front().message};
        const auto path = workspace.componentPath(
            workspace.descriptor().components.authoring / L"workspace.json");
        auto state = SctWorkspaceAuthoringStore(path).load();
        if (!state) return {false, false, state.diagnostics().front().message};
        if (const auto conflict = mergeProjectMetadata(state.value(), imported.value(),
                kind_ == LegacyMetadataKind::ProjectVariableAliases,
                kind_ == LegacyMetadataKind::OpcodeColors))
            return {true, true, *conflict};
        return {true, false, kind_ == LegacyMetadataKind::ProjectVariableAliases
            ? "Ready to apply project aliases."
            : "Ready to apply project opcode colors."};
    }
    [[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> prepare(
        const LegacyMetadataPlanRecord& record, const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        return projectMutations(workspace, capsuleRoot, std::span{&record, 1u});
    }
    [[nodiscard]] std::string aggregationKey(
        const LegacyMetadataPlanRecord&) const override {
        return "built-in:project-authoring";
    }
    [[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> prepareSelected(
        const std::span<const LegacyMetadataPlanRecord> records,
        const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        return projectMutations(workspace, capsuleRoot, records);
    }
private:
    LegacyMetadataKind kind_;
};

class ScriptMetadataAdapter final : public LegacyMetadataPromotionAdapter {
public:
    explicit ScriptMetadataAdapter(const LegacyMetadataKind kind) : kind_(kind) {}
    [[nodiscard]] LegacyMetadataKind kind() const noexcept override { return kind_; }
    [[nodiscard]] LegacyMetadataPromotionAssessment assess(
        const LegacyMetadataPlanRecord& record, const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        auto imported = importedScriptMetadata(record, workspace, capsuleRoot);
        if (!imported) return {false, false, imported.diagnostics().front().message};
        auto loaded = loadPromotionScript(workspace, imported.value().context.locator);
        if (!loaded) return {false, false, loaded.diagnostics().front().message};
        auto& current = loaded.value().loaded;
        SctSemanticState working{current.load.document->document,
            current.authoredArms, current.textRepairs, current.unboundReferences,
            current.aliases, current.annotations, current.folders};
        if (const auto conflict = mergeScriptMetadata(working, imported.value(),
                kind_ == LegacyMetadataKind::ScriptVariableAliases,
                kind_ == LegacyMetadataKind::SectionGroups))
            return {true, true, *conflict};
        return {true, false, kind_ == LegacyMetadataKind::ScriptVariableAliases
            ? "Ready to apply script aliases."
            : "Ready to apply section folders."};
    }
    [[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> prepare(
        const LegacyMetadataPlanRecord& record, const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        return scriptMutations(record, workspace, capsuleRoot,
            std::span{&record, 1u});
    }
    [[nodiscard]] std::string aggregationKey(
        const LegacyMetadataPlanRecord& record) const override {
        return "built-in:script-authoring:"
            + std::to_string(record.scriptOrdinal.value_or(
                (std::numeric_limits<std::uint32_t>::max)()));
    }
    [[nodiscard]] Result<std::vector<WorkspaceArtifactMutation>> prepareSelected(
        const std::span<const LegacyMetadataPlanRecord> records,
        const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const override {
        if (records.empty()) return Result<std::vector<WorkspaceArtifactMutation>>::failure(
            error("A script metadata group must not be empty."));
        return scriptMutations(records.front(), workspace, capsuleRoot, records);
    }
private:
    LegacyMetadataKind kind_;
};

}  // namespace

std::string LegacyMetadataPromotionAdapter::aggregationKey(
    const LegacyMetadataPlanRecord& record) const {
    return record.recordId;
}

Result<std::vector<WorkspaceArtifactMutation>>
LegacyMetadataPromotionAdapter::prepareSelected(
    const std::span<const LegacyMetadataPlanRecord> records,
    const LocalSalsaWorkspace& workspace,
    const std::filesystem::path& capsuleRoot) const {
    if (records.size() != 1u)
        return Result<std::vector<WorkspaceArtifactMutation>>::failure(error(
            "This metadata adapter cannot combine multiple selected records."));
    return prepare(records.front(), workspace, capsuleRoot);
}

Result<void> registerBuiltInLegacyMetadataPromotionAdapters(
    LegacyMetadataPromotionRegistry& registry) {
    const std::array kinds{
        LegacyMetadataKind::ProjectVariableAliases,
        LegacyMetadataKind::OpcodeColors,
        LegacyMetadataKind::ScriptVariableAliases,
        LegacyMetadataKind::SectionGroups};
    for (const auto kind : kinds) {
        std::shared_ptr<const LegacyMetadataPromotionAdapter> adapter;
        if (kind == LegacyMetadataKind::ProjectVariableAliases
            || kind == LegacyMetadataKind::OpcodeColors)
            adapter = std::make_shared<ProjectMetadataAdapter>(kind);
        else adapter = std::make_shared<ScriptMetadataAdapter>(kind);
        auto added = registry.add(std::move(adapter));
        if (!added) return added;
    }
    return Result<void>::success();
}

Result<void> LegacyMetadataPromotionRegistry::add(
    std::shared_ptr<const LegacyMetadataPromotionAdapter> adapter) {
    if (!adapter || find(adapter->kind()) != nullptr)
        return Result<void>::failure(error(
            "A metadata promotion adapter is missing or duplicates an existing kind."));
    adapters_.push_back(std::move(adapter));
    return Result<void>::success();
}

const LegacyMetadataPromotionAdapter* LegacyMetadataPromotionRegistry::find(
    const LegacyMetadataKind kind) const noexcept {
    const auto found = std::ranges::find_if(adapters_, [kind](const auto& adapter) {
        return adapter->kind() == kind;
    });
    return found == adapters_.end() ? nullptr : found->get();
}

bool LegacyMetadataPromotionPreview::hasEligible() const noexcept {
    return std::ranges::any_of(items, [](const auto& item) {
        return item.assessment.eligible && !item.assessment.conflict;
    });
}

Result<LegacyMetadataPromotionPreview> LegacyMetadataPromotionService::preview(
    const LocalSalsaWorkspace& workspace, const std::string_view capsuleId,
    const LegacyMetadataPromotionRegistry& registry) {
    const auto path = workspace.componentPath(workspace.descriptor().components.importState
        / (std::string(capsuleId) + ".json"));
    std::vector<std::byte> encoded;
    auto state = readState(path, &encoded);
    if (!state) return Result<LegacyMetadataPromotionPreview>::failure(state.diagnostics());
    if (state.value().value("capsuleId", "") != capsuleId)
        return Result<LegacyMetadataPromotionPreview>::failure(error(
            "The requested capsule does not match the import-state ledger.", path));
    auto records = recordsFrom(state.value(), path);
    if (!records) return Result<LegacyMetadataPromotionPreview>::failure(records.diagnostics());
    LegacyMetadataPromotionPreview result;
    result.capsuleId = std::string(capsuleId);
    result.stateDigest = digestOf(encoded);
    const auto capsuleRoot = workspace.componentPath(
        workspace.descriptor().components.imports / std::string(capsuleId) / L"capsule");
    if (!std::filesystem::is_directory(capsuleRoot))
        return Result<LegacyMetadataPromotionPreview>::failure(error(
            "The retained legacy capsule is missing.", capsuleRoot));
    for (auto& record : records.value()) {
        LegacyMetadataPromotionAssessment assessment;
        if (record.disposition != LegacyMetadataDisposition::Pending
            && record.disposition != LegacyMetadataDisposition::DroppedByUser) {
            assessment.reason = "This record is not pending promotion.";
        } else if (const auto* adapter = registry.find(record.kind)) {
            assessment = adapter->assess(record, workspace, capsuleRoot);
        } else {
            assessment.reason = "The running SALSA version has no owning feature adapter.";
        }
        result.items.push_back({std::move(record), std::move(assessment)});
    }
    return Result<LegacyMetadataPromotionPreview>::success(std::move(result));
}

LegacyMetadataPromotionResult LegacyMetadataPromotionService::promote(
    const LocalSalsaWorkspace& workspace,
    const LegacyMetadataPromotionPreview& preview,
    const std::span<const std::string> selectedRecordIds,
    const LegacyMetadataPromotionRegistry& registry) {
    LegacyMetadataPromotionResult result;
    if (selectedRecordIds.empty()) {
        result.diagnostics.push_back(error("At least one eligible metadata record must be selected."));
        return result;
    }
    const auto stateRelative = workspace.descriptor().components.importState
        / (preview.capsuleId + ".json");
    const auto statePath = workspace.componentPath(stateRelative);
    std::vector<std::byte> currentBytes;
    auto state = readState(statePath, &currentBytes);
    if (!state) { result.diagnostics = state.diagnostics(); return result; }
    if (digestOf(currentBytes) != preview.stateDigest) {
        result.diagnostics.push_back(error(
            "The legacy import-state ledger changed after metadata preview.", statePath));
        return result;
    }
    std::set<std::string, std::less<>> selected;
    for (const auto& id : selectedRecordIds)
        if (!selected.insert(id).second) {
            result.diagnostics.push_back(error("Metadata promotion selections must be unique."));
            return result;
        }
    std::vector<WorkspaceArtifactMutation> mutations;
    std::map<std::filesystem::path, std::size_t> paths;
    const auto capsuleRoot = workspace.componentPath(
        workspace.descriptor().components.imports / preview.capsuleId / L"capsule");
    if (!std::filesystem::is_directory(capsuleRoot)) {
        result.diagnostics.push_back(error("The retained legacy capsule is missing.",
            capsuleRoot));
        return result;
    }
    struct PreparationGroup final {
        const LegacyMetadataPromotionAdapter* adapter = nullptr;
        std::vector<LegacyMetadataPlanRecord> records{};
    };
    std::map<std::string, PreparationGroup, std::less<>> groups;
    for (const auto& item : preview.items) {
        if (!selected.contains(item.record.recordId)) continue;
        if (!item.assessment.eligible || item.assessment.conflict) {
            result.diagnostics.push_back(error(
                "A selected metadata record is no longer eligible for promotion."));
            return result;
        }
        const auto* adapter = registry.find(item.record.kind);
        if (adapter == nullptr) {
            result.diagnostics.push_back(error(
                "A selected metadata record has no owning feature adapter."));
            return result;
        }
        const auto currentAssessment = adapter->assess(
            item.record, workspace, capsuleRoot);
        if (!currentAssessment.eligible || currentAssessment.conflict) {
            result.diagnostics.push_back(error(
                "A selected metadata record became stale or conflicted after preview."));
            return result;
        }
        const auto key = adapter->aggregationKey(item.record);
        if (key.empty()) {
            result.diagnostics.push_back(error(
                "A selected metadata adapter returned an empty aggregation key."));
            return result;
        }
        auto& group = groups[key];
        if (group.adapter == nullptr) group.adapter = adapter;
        group.records.push_back(item.record);
        result.appliedRecordIds.push_back(item.record.recordId);
    }
    if (result.appliedRecordIds.size() != selected.size()) {
        result.diagnostics.push_back(error(
            "At least one selected metadata record is absent from the preview."));
        result.appliedRecordIds.clear();
        return result;
    }
    for (const auto& [key, group] : groups) {
        (void)key;
        auto prepared = group.adapter->prepareSelected(
            group.records, workspace, capsuleRoot);
        if (!prepared) { result.diagnostics = prepared.diagnostics(); return result; }
        for (auto& mutation : prepared.value()) {
            const auto existing = paths.find(mutation.relativePath);
            if (existing != paths.end()) {
                const auto& prior = mutations[existing->second];
                if (prior.expectedPresent == mutation.expectedPresent
                    && prior.expectedSha256 == mutation.expectedSha256
                    && prior.replacement == mutation.replacement)
                    continue;
                result.diagnostics.push_back(error(
                    "Selected metadata adapters target the same workspace artifact."));
                return result;
            }
            paths.emplace(mutation.relativePath, mutations.size());
            mutations.push_back(std::move(mutation));
        }
    }
    for (auto& item : state.value().at("metadata")) {
        if (!selected.contains(item.value("recordId", ""))) continue;
        item["disposition"] = "applied";
        item["reason"] = "Applied by the owning typed SALSA feature adapter.";
        if (!item.contains("history") || !item.at("history").is_array())
            item["history"] = Json::array();
        item["history"].push_back({{"decision", "applied"},
            {"applicationVersion", applicationVersion()}});
    }
    auto replacement = encode(std::move(state).takeValue());
    const auto beforeDigest = sha256(currentBytes);
    if (!beforeDigest) { result.diagnostics = beforeDigest.diagnostics(); return result; }
    mutations.push_back({stateRelative, true, beforeDigest.value().toHex(),
        std::move(replacement)});

    std::string identity = preview.capsuleId;
    for (const auto& id : selected) { identity.push_back('\0'); identity += id; }
    const auto transactionDigest = sha256(std::as_bytes(std::span{
        identity.data(), identity.size()}));
    if (!transactionDigest) { result.diagnostics = transactionDigest.diagnostics(); return result; }
    const auto transactionId = "metadata-" + transactionDigest.value().toHex().substr(0, 32);
    WorkspaceTransactionRequest request{transactionId,
        workspace.descriptor().workspaceId, "legacy-metadata-promotion",
        preview.stateDigest, std::move(mutations)};
    auto prepared = WorkspaceTransactionService::prepare(workspace.descriptor().root,
        workspace.descriptor().components.transactions, request);
    if (!prepared.succeeded()) { result.diagnostics = prepared.diagnostics; return result; }
    auto committed = WorkspaceTransactionService::commitPrepared(workspace.descriptor().root,
        workspace.descriptor().components.transactions, transactionId);
    if (committed.status != WorkspaceTransactionStatus::Verified) {
        result.diagnostics = committed.diagnostics;
        return result;
    }
    result.applied = true;
    return result;
}

}  // namespace salsa::core
