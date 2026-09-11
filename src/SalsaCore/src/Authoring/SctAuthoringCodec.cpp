#include "SalsaCore/Authoring/SctAuthoringCodec.h"

#include <nlohmann/json.hpp>
#include <charconv>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace salsa::core {
namespace {
using Json = nlohmann::ordered_json;
struct UnsupportedSchema : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void keys(const Json& value, std::initializer_list<std::string_view> names) {
    require(value.is_object() && value.size() == names.size(), "Unexpected object fields.");
    for (auto name : names) require(value.contains(std::string(name)), "Missing object field.");
}
std::string string(const Json& value) {
    require(value.is_string(), "Expected a string."); return value.get<std::string>();
}
std::uint64_t decimal(const Json& value) {
    const auto text = string(value);
    require(!text.empty() && (text.size() == 1 || text.front() != '0'), "Expected canonical unsigned decimal string.");
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid unsigned decimal string.");
    return number;
}
template<class Id> Id id(const Json& value) {
    const auto number = decimal(value); require(number != 0, "Entity ID cannot be zero."); return Id{number};
}
template<class Id> std::string id(const Id& value) { return std::to_string(value.value); }
const Json& array(const Json& value) { require(value.is_array(), "Expected an array."); return value; }
bool boolean(const Json& value) { require(value.is_boolean(), "Expected a boolean."); return value.get<bool>(); }
template<class T, class Encode> Json sorted(const std::vector<T>& values, Encode encode) {
    std::vector<const T*> order;
    for (const auto& value : values) order.push_back(&value);
    std::ranges::sort(order, [](const T* a, const T* b) { return a->id < b->id; });
    auto result = Json::array();
    for (const auto* value : order) result.push_back(encode(*value));
    return result;
}
template<class Enum, std::size_t N> Json enumJson(std::optional<Enum> value, const std::array<std::string_view, N>& names) {
    return value ? Json(names.at(static_cast<std::size_t>(*value))) : Json(nullptr);
}
template<class Enum, std::size_t N> std::optional<Enum> readEnum(const Json& value, const std::array<std::string_view, N>& names) {
    if (value.is_null()) return std::nullopt;
    const auto text = string(value);
    for (std::size_t i = 0; i < N; ++i) if (text == names[i]) return static_cast<Enum>(i);
    throw std::runtime_error("Unknown enum value.");
}
constexpr std::array<std::string_view, 2> Platforms{"gamecube", "dreamcast"};
constexpr std::array<std::string_view, 3> Regions{"north-america", "europe", "japan"};
constexpr std::array<std::string_view, 3> Conventions{"windows-1252-byte-7f", "shift-jis-byte-7f", "shift-jis-8140"};
constexpr std::array<std::string_view, 3> EvidenceKinds{"source-observation", "inference", "user-assertion"};
Json evidenceJson(const std::vector<SctAuthoringEvidence>& values) {
    auto result = Json::array();
    for (const auto& value : values) result.push_back({{"kind", EvidenceKinds.at(static_cast<std::size_t>(value.kind))}, {"description", value.description}});
    return result;
}
std::vector<SctAuthoringEvidence> readEvidence(const Json& values) {
    std::vector<SctAuthoringEvidence> result;
    for (const auto& value : array(values)) {
        keys(value, {"kind", "description"});
        const auto kind = readEnum<SctEvidenceKind>(value.at("kind"), EvidenceKinds);
        require(kind.has_value(), "Evidence kind cannot be null.");
        result.push_back({*kind, string(value.at("description"))});
    }
    return result;
}
template<class Id> Json referenceJson(const SctAuthoringReference<Id>& ref) {
    if (const auto* target = std::get_if<Id>(&ref)) return {{"kind", "resolved"}, {"target", id(*target)}};
    const auto& unresolved = std::get<SctUnresolvedBinding>(ref);
    return {{"kind", "unresolved"}, {"originalEvidence", unresolved.originalEvidence},
        {"explanation", unresolved.explanation}, {"evidence", evidenceJson(unresolved.evidence)}};
}
template<class Id> SctAuthoringReference<Id> readReference(const Json& value) {
    const auto kind = string(value.at("kind"));
    if (kind == "resolved") { keys(value, {"kind", "target"}); return id<Id>(value.at("target")); }
    require(kind == "unresolved", "Unknown reference kind.");
    keys(value, {"kind", "originalEvidence", "explanation", "evidence"});
    return SctUnresolvedBinding{string(value.at("originalEvidence")), string(value.at("explanation")), readEvidence(value.at("evidence"))};
}
Json usesJson(const std::vector<SctAuthoringReference<SctContentId>>& uses) {
    auto result = Json::array(); for (const auto& use : uses) result.push_back(referenceJson(use)); return result;
}
std::vector<SctAuthoringReference<SctContentId>> readUses(const Json& values) {
    std::vector<SctAuthoringReference<SctContentId>> result;
    for (const auto& value : array(values)) result.push_back(readReference<SctContentId>(value)); return result;
}
constexpr std::array<std::string_view, 7> EntityKinds{"script", "module", "entrypoint", "port", "connection", "content", "baseline"};
Json entityJson(const SctAuthoringEntityId& value) {
    return {{"kind", EntityKinds.at(value.index())}, {"id", std::visit([](auto item) { return id(item); }, value)}};
}
SctAuthoringEntityId readEntity(const Json& value) {
    keys(value, {"kind", "id"}); const auto kind = string(value.at("kind")); const auto& number = value.at("id");
    if (kind == "script") return id<SctScriptId>(number);
    if (kind == "module") return id<SctModuleId>(number);
    if (kind == "entrypoint") return id<SctEntrypointId>(number);
    if (kind == "port") return id<SctPortId>(number);
    if (kind == "connection") return id<SctConnectionId>(number);
    if (kind == "content") return id<SctContentId>(number);
    if (kind == "baseline") return id<SctBaselineId>(number);
    throw std::runtime_error("Unknown authored entity kind.");
}
template<class Owner> Json ownerJson(const Owner& owner) {
    return std::visit([](auto value) { return entityJson(SctAuthoringEntityId{value}); }, owner);
}
template<class Owner> Owner readOwner(const Json& value) {
    return std::visit([](auto entity) -> Owner {
        if constexpr (std::is_constructible_v<Owner, decltype(entity)>) return entity;
        else throw std::runtime_error("Invalid owner kind.");
    }, readEntity(value));
}
constexpr std::array<std::string_view, 6> SourceKinds{"document", "section", "instruction", "string", "supplementary-text", "opaque-attachment"};
Json sourceEntityJson(const spice::sct::SctDocumentEntityId& entity) {
    return {{"kind", SourceKinds.at(entity.index())}, {"id", std::visit([](auto value) -> std::string {
        if constexpr (std::is_same_v<decltype(value), std::monostate>) throw std::runtime_error("Document is not a selectable entity.");
        else return std::to_string(value.value());
    }, entity)}};
}
spice::sct::SctDocumentEntityId readSourceEntity(const Json& value) {
    keys(value, {"kind", "id"}); const auto kind = string(value.at("kind")); const auto number = decimal(value.at("id"));
    require(number != 0, "Source entity ID cannot be zero.");
    if (kind == "section") return spice::sct::SctSectionId{number};
    if (kind == "instruction") return spice::sct::SctInstructionId{number};
    if (kind == "string") return spice::sct::SctStringId{number};
    if (kind == "supplementary-text") return spice::sct::SctSupplementaryTextId{number};
    if (kind == "opaque-attachment") return spice::sct::SctOpaqueAttachmentId{number};
    throw std::runtime_error("Unknown source entity kind.");
}
Json regionJson(const SctPreservedProgramRegion& value) {
    Json coverage{{"kind", "whole-document"}};
    if (const auto* selected = std::get_if<SctOrderedSourceSelection>(&value.coverage)) {
        coverage = {{"kind", "ordered-selection"}, {"entities", Json::array()}};
        for (const auto& entity : selected->entities) coverage["entities"].push_back(sourceEntityJson(entity));
    }
    return {{"baseline", id(value.baseline)}, {"importedDocument", value.importedDocument.value}, {"coverage", coverage}};
}
SctPreservedProgramRegion readRegion(const Json& value) {
    keys(value, {"baseline", "importedDocument", "coverage"});
    SctPreservedProgramRegion result{id<SctBaselineId>(value.at("baseline")), {string(value.at("importedDocument"))}, SctWholeDocument{}};
    const auto& coverage = value.at("coverage"); const auto kind = string(coverage.at("kind"));
    if (kind == "whole-document") keys(coverage, {"kind"});
    else {
        require(kind == "ordered-selection", "Unknown coverage kind."); keys(coverage, {"kind", "entities"});
        SctOrderedSourceSelection selection;
        for (const auto& entity : array(coverage.at("entities"))) selection.entities.push_back(readSourceEntity(entity));
        result.coverage = std::move(selection);
    }
    return result;
}
Sha256Digest digest(const Json& value) {
    const auto text = string(value); require(text.size() == 64, "Invalid SHA-256 length.");
    std::array<std::byte, 32> bytes{};
    const auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        throw std::runtime_error("Invalid SHA-256 digit.");
    };
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>((nibble(text[2*i]) << 4) | nibble(text[2*i+1]));
    return Sha256Digest{bytes};
}
Json legacyJson(const std::optional<SctLegacyOrigin>& value) {
    if (!value) return nullptr;
    return {{"capsuleId", value->capsuleId}, {"scriptKey", value->scriptKey}, {"scriptOrdinal", std::to_string(value->scriptOrdinal)}};
}
std::optional<SctLegacyOrigin> readLegacy(const Json& value) {
    if (value.is_null()) return std::nullopt;
    keys(value, {"capsuleId", "scriptKey", "scriptOrdinal"});
    return SctLegacyOrigin{string(value.at("capsuleId")), string(value.at("scriptKey")), decimal(value.at("scriptOrdinal"))};
}
Json parse(std::string_view text) {
    std::vector<std::set<std::string>> objects;
    return Json::parse(text, [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::key) require(objects.back().insert(parsed.get<std::string>()).second, "Duplicate JSON field.");
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
}
void envelope(const Json& root, std::string_view format, std::uint32_t version) {
    require(string(root.at("format")) == format, "Unexpected format identifier.");
    require(root.at("schemaVersion").is_number_unsigned(), "Schema version must be an unsigned integer.");
    if (root.at("schemaVersion").get<std::uint64_t>() != version) throw UnsupportedSchema("Unsupported authoring schema version.");
}
Diagnostic failure(const std::exception& exception) {
    return {DiagnosticSeverity::Error,
        dynamic_cast<const UnsupportedSchema*>(&exception) ? DiagnosticCode::UnsupportedPersistenceSchemaVersion : DiagnosticCode::MalformedPersistenceJson,
        std::string("Authoring codec: ") + exception.what()};
}
Json literalJson(const SctLiteralConstant& literal) {
    using spice::sct::SctScptValueKind;
    const auto kind = literal.operation.kind == SctScptValueKind::InlineValue ? "inline"
        : literal.operation.kind == SctScptValueKind::DecimalLiteral ? "decimal" : "float";
    return {{"kind", kind}, {"encodingWord", literal.operation.encodingWord}, {"payloadWords", literal.operation.payloadWords},
        {"termination", literal.termination == spice::sct::SctExpressionTermination::InlineValue ? "inline" : "stop"}};
}
Json overridesJson(const std::vector<SctPreservedLiteralOverride>& edits) {
    auto ordered = edits;
    std::ranges::sort(ordered, {}, &SctPreservedLiteralOverride::site);
    auto result = Json::array();
    for (const auto& edit : ordered) result.push_back({{"instruction", std::to_string(edit.site.instruction.value())},
        {"schemaIndex", edit.site.parameter.schemaIndex}, {"opcode", edit.opcode},
        {"baselineValue", literalJson(edit.baselineValue)}, {"value", literalJson(edit.value)}});
    return result;
}
std::uint32_t word(const Json& value, std::uint64_t maximum = UINT32_MAX) {
    require(value.is_number_unsigned() && value.get<std::uint64_t>() <= maximum, "Word value is out of range.");
    return value.get<std::uint32_t>();
}
SctLiteralConstant readLiteral(const Json& value) {
    using namespace spice::sct;
    keys(value, {"kind", "encodingWord", "payloadWords", "termination"});
    const auto kind = string(value.at("kind"));
    require(kind == "inline" || kind == "decimal" || kind == "float", "Unsupported literal kind.");
    const auto termination = string(value.at("termination"));
    require(termination == "inline" || termination == "stop", "Unsupported literal termination.");
    SctLiteralConstant result{{kind == "inline" ? SctScptValueKind::InlineValue
        : kind == "decimal" ? SctScptValueKind::DecimalLiteral : SctScptValueKind::FloatLiteral,
        word(value.at("encodingWord")), {}}, termination == "inline" ? SctExpressionTermination::InlineValue : SctExpressionTermination::StopCode};
    for (const auto& payload : array(value.at("payloadWords"))) result.operation.payloadWords.push_back(word(payload));
    require(result.valid(), "Invalid literal encoding.");
    return result;
}
std::vector<SctPreservedLiteralOverride> readOverrides(const Json& values) {
    std::vector<SctPreservedLiteralOverride> result;
    for (const auto& edit : array(values)) {
        keys(edit, {"instruction", "schemaIndex", "opcode", "baselineValue", "value"});
        const auto instruction = decimal(edit.at("instruction"));
        require(instruction != 0, "Override instruction ID cannot be zero.");
        result.push_back({{spice::sct::SctInstructionId{instruction}, {word(edit.at("schemaIndex")), {}}},
            static_cast<std::uint16_t>(word(edit.at("opcode"), UINT16_MAX)),
            readLiteral(edit.at("baselineValue")), readLiteral(edit.at("value"))});
    }
    return result;
}
} // namespace

Result<std::string> SctAuthoringCodec::encode(const SctAuthoringProject& project) {
    auto diagnostics = project.validate();
    if (hasErrors(diagnostics)) return Result<std::string>::failure(std::move(diagnostics));
    try {
        Json root{{"format", Format}, {"schemaVersion", SchemaVersion}, {"projectId", project.id.value},
            {"revision", id(project.revision)}, {"nextEntityId", std::to_string(project.nextEntityId)}};
        auto metadata = SctAuthoringCatalogCodec::serializeWorkspace(project.workspaceAuthoring);
        if (!metadata) return Result<std::string>::failure(metadata.diagnostics());
        root["workspaceAuthoring"] = Json::parse(metadata.value());
        root["baselines"] = sorted(project.baselines, [](const auto& b) {
            const auto path = b.source.locator.path().generic_u8string();
            return Json{{"id", id(b.id)}, {"datasetFingerprint", b.datasetFingerprint.digest.toHex()},
                {"asset", std::string(path.begin(), path.end())}, {"sourceRevision", b.source.revision.digest.toHex()},
                {"byteSize", std::to_string(b.source.byteSize)}, {"importedDocument", b.importedDocument.value},
                {"textConvention", enumJson(b.textConvention, Conventions)}, {"importEvidence", evidenceJson(b.importEvidence)},
                {"recipe", {{"platform", enumJson(b.recipe.platform, Platforms)}, {"trustSelectedTextEncoding", b.recipe.trustSelectedTextEncoding}}}};
        });
        root["scripts"] = sorted(project.scripts, [](const auto& s) {
            return Json{{"id", id(s.id)}, {"name", s.name}, {"baseline", id(s.baseline)},
                {"platform", enumJson(s.platform, Platforms)}, {"region", enumJson(s.region, Regions)},
                {"legacyOrigin", legacyJson(s.legacyOrigin)}, {"contentUses", usesJson(s.contentUses)}};
        });
        const auto owned = [](const auto& m) { return Json{{"id", id(m.id)}, {"script", id(m.script)},
            {"name", m.name}, {"contentUses", usesJson(m.contentUses)}, {"evidence", evidenceJson(m.evidence)}}; };
        root["modules"] = sorted(project.modules, owned); root["entrypoints"] = sorted(project.entrypoints, owned);
        root["ports"] = sorted(project.ports, [](const auto& p) { return Json{{"id", id(p.id)}, {"owner", ownerJson(p.owner)}, {"name", p.name}}; });
        root["connections"] = sorted(project.connections, [](const auto& c) { return Json{{"id", id(c.id)},
            {"source", referenceJson(c.source)}, {"destination", referenceJson(c.destination)}, {"evidence", evidenceJson(c.evidence)}}; });
        root["contents"] = sorted(project.contents, [](const auto& c) {
            Json patch = nullptr;
            if (c.physicalPatch) {
                auto encoded = SalsaScriptPatchCodec::serialize(*c.physicalPatch);
                require(static_cast<bool>(encoded), "Invalid physical patch.");
                patch = Json::parse(encoded.value());
            }
            return Json{{"id", id(c.id)},
            {"owner", ownerJson(c.owner)}, {"region", regionJson(c.region)}, {"evidence", evidenceJson(c.evidence)},
            {"literalOverrides", overridesJson(c.literalOverrides)}, {"physicalPatch", patch}}; });
        return Result<std::string>::success(root.dump(2), std::move(diagnostics));
    } catch (const std::exception& ex) { return Result<std::string>::failure(failure(ex)); }
}

Result<SctAuthoringProject> SctAuthoringCodec::decode(std::string_view text) {
    try {
        const auto root = parse(text); envelope(root, Format, SchemaVersion);
        keys(root, {"format", "schemaVersion", "projectId", "revision", "nextEntityId", "baselines", "scripts", "modules", "entrypoints", "ports", "connections", "contents", "workspaceAuthoring"});
        SctAuthoringProject result;
        const auto metadataJson = root.at("workspaceAuthoring").dump();
        auto metadata = SctAuthoringCatalogCodec::deserializeWorkspace(std::as_bytes(std::span(metadataJson.data(), metadataJson.size())));
        if (!metadata) return Result<SctAuthoringProject>::failure(metadata.diagnostics());
        result.workspaceAuthoring = std::move(metadata).takeValue();
        result.id.value = string(root.at("projectId")); result.revision = id<RevisionId>(root.at("revision"));
        result.nextEntityId = decimal(root.at("nextEntityId"));
        for (const auto& b : array(root.at("baselines"))) {
            keys(b, {"id", "datasetFingerprint", "asset", "sourceRevision", "byteSize", "importedDocument", "textConvention", "importEvidence", "recipe"});
            keys(b.at("recipe"), {"platform", "trustSelectedTextEncoding"});
            const auto path = string(b.at("asset"));
            auto locator = AssetLocator::fromRelativePath(std::filesystem::path(std::u8string(path.begin(), path.end())));
            if (!locator) return Result<SctAuthoringProject>::failure(locator.diagnostics());
            const auto normalized = locator.value().path().generic_u8string();
            require(path == std::string(normalized.begin(), normalized.end()), "Asset locator must be normalized.");
            result.baselines.push_back({id<SctBaselineId>(b.at("id")), {digest(b.at("datasetFingerprint"))},
                {std::move(locator).takeValue(), decimal(b.at("byteSize")), {digest(b.at("sourceRevision"))}},
                {string(b.at("importedDocument"))}, readEnum<spice::sct::SctKnownTextConvention>(b.at("textConvention"), Conventions), readEvidence(b.at("importEvidence")),
                {readEnum<GamePlatform>(b.at("recipe").at("platform"), Platforms), boolean(b.at("recipe").at("trustSelectedTextEncoding"))}});
        }
        for (const auto& s : array(root.at("scripts"))) {
            keys(s, {"id", "name", "baseline", "platform", "region", "legacyOrigin", "contentUses"});
            result.scripts.push_back({id<SctScriptId>(s.at("id")), string(s.at("name")), id<SctBaselineId>(s.at("baseline")),
                readEnum<GamePlatform>(s.at("platform"), Platforms), readEnum<GameRegion>(s.at("region"), Regions),
                readLegacy(s.at("legacyOrigin")), readUses(s.at("contentUses"))});
        }
        const auto readOwned = [](const Json& values, auto& destination) {
            using Item = typename std::decay_t<decltype(destination)>::value_type;
            for (const auto& m : array(values)) {
                keys(m, {"id", "script", "name", "contentUses", "evidence"});
                destination.push_back(Item{id<decltype(Item::id)>(m.at("id")), id<SctScriptId>(m.at("script")),
                    string(m.at("name")), readUses(m.at("contentUses")), readEvidence(m.at("evidence"))});
            }
        };
        readOwned(root.at("modules"), result.modules); readOwned(root.at("entrypoints"), result.entrypoints);
        for (const auto& p : array(root.at("ports"))) {
            keys(p, {"id", "owner", "name"});
            result.ports.push_back({id<SctPortId>(p.at("id")), readOwner<SctPortOwner>(p.at("owner")), string(p.at("name"))});
        }
        for (const auto& c : array(root.at("connections"))) {
            keys(c, {"id", "source", "destination", "evidence"});
            result.connections.push_back({id<SctConnectionId>(c.at("id")), readReference<SctPortId>(c.at("source")),
                readReference<SctPortId>(c.at("destination")), readEvidence(c.at("evidence"))});
        }
        for (const auto& c : array(root.at("contents"))) {
            keys(c, {"id", "owner", "region", "evidence", "literalOverrides", "physicalPatch"});
            result.contents.push_back({id<SctContentId>(c.at("id")), readOwner<SctContentOwner>(c.at("owner")),
                readRegion(c.at("region")), readEvidence(c.at("evidence")), readOverrides(c.at("literalOverrides"))});
            if (!c.at("physicalPatch").is_null()) {
                const auto text = c.at("physicalPatch").dump();
                auto patch = SalsaScriptPatchCodec::deserialize(std::as_bytes(std::span(text.data(), text.size())));
                if (!patch) return Result<SctAuthoringProject>::failure(patch.diagnostics());
                result.contents.back().physicalPatch = std::move(patch).takeValue();
            }
        }
        auto diagnostics = result.validate();
        if (hasErrors(diagnostics)) return Result<SctAuthoringProject>::failure(std::move(diagnostics));
        return Result<SctAuthoringProject>::success(std::move(result), std::move(diagnostics));
    } catch (const std::exception& ex) { return Result<SctAuthoringProject>::failure(failure(ex)); }
}

Result<std::string> SctAuthoringPresentationCodec::encode(const SctAuthoringPresentation& view, const SctAuthoringProject& project) {
    auto diagnostics = view.validate(project);
    if (hasErrors(diagnostics)) return Result<std::string>::failure(std::move(diagnostics));
    try {
        Json root{{"format", Format}, {"schemaVersion", SchemaVersion}, {"projectId", view.project.value},
            {"placements", Json::array()}, {"selection", Json::array()}};
        auto placements = view.placements;
        std::ranges::sort(placements, {}, &SctGraphPlacement::entity);
        for (const auto& p : placements) root["placements"].push_back({{"entity", entityJson(p.entity)}, {"x", p.x}, {"y", p.y}, {"collapsed", p.collapsed}});
        auto selection = view.selection; std::ranges::sort(selection);
        for (const auto& s : selection) root["selection"].push_back(entityJson(s));
        return Result<std::string>::success(root.dump(2));
    } catch (const std::exception& ex) { return Result<std::string>::failure(failure(ex)); }
}
Result<SctAuthoringPresentation> SctAuthoringPresentationCodec::decode(std::string_view text, const SctAuthoringProject& project) {
    try {
        const auto root = parse(text); envelope(root, Format, SchemaVersion);
        keys(root, {"format", "schemaVersion", "projectId", "placements", "selection"});
        SctAuthoringPresentation result{{string(root.at("projectId"))}, {}, {}};
        for (const auto& p : array(root.at("placements"))) {
            keys(p, {"entity", "x", "y", "collapsed"});
            require(p.at("x").is_number() && p.at("y").is_number(), "Coordinates must be numbers.");
            result.placements.push_back({readEntity(p.at("entity")), p.at("x").get<double>(), p.at("y").get<double>(), boolean(p.at("collapsed"))});
        }
        for (const auto& s : array(root.at("selection"))) result.selection.push_back(readEntity(s));
        auto diagnostics = result.validate(project);
        if (hasErrors(diagnostics)) return Result<SctAuthoringPresentation>::failure(std::move(diagnostics));
        return Result<SctAuthoringPresentation>::success(std::move(result));
    } catch (const std::exception& ex) { return Result<SctAuthoringPresentation>::failure(failure(ex)); }
}

} // namespace salsa::core
