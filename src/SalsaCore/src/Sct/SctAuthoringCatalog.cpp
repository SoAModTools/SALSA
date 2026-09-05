#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include "SalsaCore/Persistence/AtomicFile.h"
#include "SpiceSCT/SctOpcodeMetadata.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <ranges>
#include <set>

namespace salsa::core {
namespace {
using Json = nlohmann::ordered_json;
constexpr std::size_t MaximumBytes = 16u * 1024u * 1024u;
std::mutex catalogMutex;
std::shared_ptr<const SctPersonalCatalog> installedCatalog =
    std::make_shared<const SctPersonalCatalog>();

[[nodiscard]] Diagnostic error(std::string message,
    const std::filesystem::path& path = {}) {
    return {DiagnosticSeverity::Error, DiagnosticCode::PersistenceReadFailed,
        std::move(message), path.empty() ? std::nullopt : std::optional{path}};
}

void exact(const Json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size())
        throw std::runtime_error("object has missing or unknown fields");
    for (const auto field : fields)
        if (!value.contains(field)) throw std::runtime_error("object field is missing");
}

[[nodiscard]] std::optional<std::string> optionalText(const Json& value) {
    return value.is_null() ? std::nullopt
        : std::optional<std::string>{value.get<std::string>()};
}

[[nodiscard]] Json text(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] std::vector<std::byte> encoded(Json value) {
    auto content = value.dump(2);
    content.push_back('\n');
    const auto bytes = std::as_bytes(std::span{content.data(), content.size()});
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] Result<std::vector<std::byte>> read(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path))
        return Result<std::vector<std::byte>>::success({});
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Result<std::vector<std::byte>>::failure(error(
        "The authoring catalog could not be opened.", path));
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > MaximumBytes)
        return Result<std::vector<std::byte>>::failure(error(
            "The authoring catalog exceeds its 16 MiB limit.", path));
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!result.empty()) input.read(reinterpret_cast<char*>(result.data()), size);
    if (!input) return Result<std::vector<std::byte>>::failure(error(
        "The authoring catalog could not be read completely.", path));
    return Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] Json variable(const SctVariableAlias& alias) {
    return {{"kind", static_cast<unsigned>(alias.variable.kind)},
        {"index", alias.variable.index}, {"alias", alias.alias}};
}

[[nodiscard]] SctVariableAlias parseVariable(const Json& value) {
    exact(value, {"kind", "index", "alias"});
    const auto kind = value.at("kind").get<unsigned>();
    if (kind > 3u) throw std::runtime_error("variable kind is invalid");
    SctVariableAlias result{{static_cast<SctVariableKind>(kind),
        value.at("index").get<std::uint32_t>()}, value.at("alias").get<std::string>()};
    if (result.alias.empty()) throw std::runtime_error("variable alias is empty");
    return result;
}

[[nodiscard]] const SctCatalogOpcodeOverride* overrideFor(
    const SctPersonalCatalog& catalog, const std::uint16_t opcode) {
    const auto found = std::ranges::find(catalog.opcodes, opcode,
        &SctCatalogOpcodeOverride::opcode);
    return found == catalog.opcodes.end() ? nullptr : &*found;
}

[[nodiscard]] bool validParameter(const spice::sct::SctOpcodeSchema& schema,
    const std::uint32_t index) {
    const std::span parameters{schema.parameterCatalog.data(),
        static_cast<std::size_t>(schema.parameterCatalogCount)};
    return std::ranges::find(parameters, index,
        &spice::sct::SctOpcodeParameterSchema::schemaIndex) != parameters.end();
}

[[nodiscard]] SctPersonalCatalog parseCatalogEntries(const Json& entries) {
    if (!entries.is_array()) throw std::runtime_error("catalog entries are not an array");
    SctPersonalCatalog result;
    std::set<std::uint16_t> opcodes;
    for (const auto& item : entries) {
        exact(item, {"opcode", "mnemonic", "description", "note", "parameters",
            "colorRgb", "category"});
        SctCatalogOpcodeOverride entry;
        entry.opcode = item.at("opcode").get<std::uint16_t>();
        const auto* schema = spice::sct::findSctOpcodeSchema(entry.opcode);
        if (!schema || !opcodes.insert(entry.opcode).second)
            throw std::runtime_error("catalog opcode is invalid or duplicated");
        entry.mnemonic = optionalText(item.at("mnemonic"));
        entry.description = optionalText(item.at("description"));
        entry.note = optionalText(item.at("note"));
        entry.category = optionalText(item.at("category"));
        if (!item.at("colorRgb").is_null()) {
            const auto color = item.at("colorRgb").get<std::uint32_t>();
            if (color > 0xffffffu) throw std::runtime_error("catalog color is not RGB");
            entry.colorRgb = color;
        }
        if (!item.at("parameters").is_array())
            throw std::runtime_error("catalog parameters are not an array");
        std::set<std::uint32_t> parameters;
        for (const auto& encodedParameter : item.at("parameters")) {
            exact(encodedParameter, {"schemaIndex", "label", "creationDefaultWord"});
            SctCatalogParameterOverride parameter;
            parameter.schemaIndex = encodedParameter.at("schemaIndex").get<std::uint32_t>();
            if (!parameters.insert(parameter.schemaIndex).second
                || !validParameter(*schema, parameter.schemaIndex))
                throw std::runtime_error("catalog parameter is invalid or duplicated");
            parameter.label = optionalText(encodedParameter.at("label"));
            if (!encodedParameter.at("creationDefaultWord").is_null())
                parameter.creationDefaultWord = encodedParameter.at(
                    "creationDefaultWord").get<std::uint32_t>();
            if (!parameter.label && !parameter.creationDefaultWord)
                throw std::runtime_error("empty catalog parameter override is not canonical");
            entry.parameters.push_back(std::move(parameter));
        }
        std::ranges::sort(entry.parameters, {}, &SctCatalogParameterOverride::schemaIndex);
        result.opcodes.push_back(std::move(entry));
    }
    std::ranges::sort(result.opcodes, {}, &SctCatalogOpcodeOverride::opcode);
    return result;
}

[[nodiscard]] Json encodeCatalogEntries(const SctPersonalCatalog& catalog) {
    Json entries = Json::array();
    for (const auto& entry : catalog.opcodes) {
        Json parameters = Json::array();
        for (const auto& parameter : entry.parameters)
            parameters.push_back({{"schemaIndex", parameter.schemaIndex},
                {"label", text(parameter.label)},
                {"creationDefaultWord", parameter.creationDefaultWord
                    ? Json(*parameter.creationDefaultWord) : Json(nullptr)}});
        entries.push_back({{"opcode", entry.opcode}, {"mnemonic", text(entry.mnemonic)},
            {"description", text(entry.description)}, {"note", text(entry.note)},
            {"parameters", std::move(parameters)},
            {"colorRgb", entry.colorRgb ? Json(*entry.colorRgb) : Json(nullptr)},
            {"category", text(entry.category)}});
    }
    return entries;
}
} // namespace

Result<std::vector<std::byte>> SctAuthoringCatalogCodec::serializeWorkspace(
    const SctWorkspaceAuthoringState& state) {
    try {
        auto canonical = state;
        std::ranges::sort(canonical.projectAliases, {}, &SctVariableAlias::variable);
        std::ranges::sort(canonical.opcodeColors, {}, &SctOpcodeColor::opcode);
        Json aliases = Json::array(), colors = Json::array();
        for (const auto& alias : canonical.projectAliases) aliases.push_back(variable(alias));
        for (const auto& color : canonical.opcodeColors) {
            if (!spice::sct::findSctOpcodeSchema(color.opcode) || color.colorRgb > 0xffffffu)
                throw std::runtime_error("workspace opcode color is invalid");
            colors.push_back({{"opcode", color.opcode}, {"colorRgb", color.colorRgb}});
        }
        auto bytes = encoded({
            {"formatId", "jahorta.salsa.workspace-authoring"},
            {"schemaVersion", WorkspaceSchemaVersion}, {"projectAliases", aliases},
            {"opcodeColors", colors}});
        auto validated = deserializeWorkspace(bytes);
        if (!validated) return Result<std::vector<std::byte>>::failure(
            validated.diagnostics());
        return Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch (const std::exception& exception) {
        return Result<std::vector<std::byte>>::failure(error(
            std::string("Workspace authoring metadata is invalid: ") + exception.what()));
    }
}

Result<SctWorkspaceAuthoringState> SctAuthoringCatalogCodec::deserializeWorkspace(
    const std::span<const std::byte> bytes) {
    try {
        const auto document = Json::parse(reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data() + bytes.size()));
        exact(document, {"formatId", "schemaVersion", "projectAliases", "opcodeColors"});
        if (document.at("formatId") != "jahorta.salsa.workspace-authoring"
            || document.at("schemaVersion") != WorkspaceSchemaVersion)
            throw std::runtime_error("workspace authoring schema is unsupported");
        if (!document.at("projectAliases").is_array()
            || !document.at("opcodeColors").is_array())
            throw std::runtime_error("workspace authoring collections are malformed");
        SctWorkspaceAuthoringState result;
        std::set<SctVariableKey> variables;
        std::set<std::string> names;
        for (const auto& item : document.at("projectAliases")) {
            auto alias = parseVariable(item);
            auto folded = alias.alias;
            std::ranges::transform(folded, folded.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (!variables.insert(alias.variable).second || !names.insert(folded).second)
                throw std::runtime_error("workspace aliases are duplicated");
            result.projectAliases.push_back(std::move(alias));
        }
        std::set<std::uint16_t> opcodes;
        for (const auto& item : document.at("opcodeColors")) {
            exact(item, {"opcode", "colorRgb"});
            SctOpcodeColor color{item.at("opcode").get<std::uint16_t>(),
                item.at("colorRgb").get<std::uint32_t>()};
            if (!spice::sct::findSctOpcodeSchema(color.opcode) || color.colorRgb > 0xffffffu
                || !opcodes.insert(color.opcode).second)
                throw std::runtime_error("workspace opcode colors are invalid or duplicated");
            result.opcodeColors.push_back(color);
        }
        std::ranges::sort(result.projectAliases, {}, &SctVariableAlias::variable);
        std::ranges::sort(result.opcodeColors, {}, &SctOpcodeColor::opcode);
        return Result<SctWorkspaceAuthoringState>::success(std::move(result));
    } catch (const std::exception& exception) {
        return Result<SctWorkspaceAuthoringState>::failure(error(
            std::string("Workspace authoring metadata is malformed: ") + exception.what()));
    }
}

Result<std::vector<std::byte>> SctAuthoringCatalogCodec::serializeCatalog(
    const SctPersonalCatalog& catalog) {
    try {
        // Re-parse the canonical representation to apply the same validation as load.
        auto bytes = encoded({{"formatId", "jahorta.salsa.personal-sct-catalog"},
            {"schemaVersion", PersonalCatalogSchemaVersion},
            {"opcodes", encodeCatalogEntries(catalog)}});
        auto validated = deserializeCatalog(bytes);
        if (!validated) return Result<std::vector<std::byte>>::failure(validated.diagnostics());
        return Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch (const std::exception& exception) {
        return Result<std::vector<std::byte>>::failure(error(
            std::string("Personal catalog is invalid: ") + exception.what()));
    }
}

Result<SctPersonalCatalog> SctAuthoringCatalogCodec::deserializeCatalog(
    const std::span<const std::byte> bytes) {
    try {
        const auto document = Json::parse(reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data() + bytes.size()));
        exact(document, {"formatId", "schemaVersion", "opcodes"});
        if (document.at("formatId") != "jahorta.salsa.personal-sct-catalog"
            || document.at("schemaVersion") != PersonalCatalogSchemaVersion)
            throw std::runtime_error("personal catalog schema is unsupported");
        return Result<SctPersonalCatalog>::success(parseCatalogEntries(document.at("opcodes")));
    } catch (const std::exception& exception) {
        return Result<SctPersonalCatalog>::failure(error(
            std::string("Personal catalog is malformed: ") + exception.what()));
    }
}

SctWorkspaceAuthoringStore::SctWorkspaceAuthoringStore(std::filesystem::path path)
    : path_(std::move(path)) {}
const std::filesystem::path& SctWorkspaceAuthoringStore::path() const noexcept { return path_; }
Result<SctWorkspaceAuthoringState> SctWorkspaceAuthoringStore::load() const {
    auto bytes = read(path_);
    if (!bytes) return Result<SctWorkspaceAuthoringState>::failure(bytes.diagnostics());
    if (bytes.value().empty()) return Result<SctWorkspaceAuthoringState>::success({});
    return SctAuthoringCatalogCodec::deserializeWorkspace(bytes.value());
}
Result<void> SctWorkspaceAuthoringStore::save(const SctWorkspaceAuthoringState& state) const {
    auto bytes = SctAuthoringCatalogCodec::serializeWorkspace(state);
    return bytes ? replaceFileAtomically(path_, bytes.value())
                 : Result<void>::failure(bytes.diagnostics());
}

SctPersonalCatalogStore::SctPersonalCatalogStore(std::filesystem::path path)
    : path_(std::move(path)) {}
const std::filesystem::path& SctPersonalCatalogStore::path() const noexcept { return path_; }
Result<SctPersonalCatalog> SctPersonalCatalogStore::load() const {
    auto bytes = read(path_);
    if (!bytes) return Result<SctPersonalCatalog>::failure(bytes.diagnostics());
    if (bytes.value().empty()) return Result<SctPersonalCatalog>::success({});
    return SctAuthoringCatalogCodec::deserializeCatalog(bytes.value());
}
Result<void> SctPersonalCatalogStore::save(const SctPersonalCatalog& catalog) const {
    auto bytes = SctAuthoringCatalogCodec::serializeCatalog(catalog);
    return bytes ? replaceFileAtomically(path_, bytes.value())
                 : Result<void>::failure(bytes.diagnostics());
}

void SctCatalogResolver::install(std::shared_ptr<const SctPersonalCatalog> catalog) {
    std::scoped_lock lock(catalogMutex);
    installedCatalog = catalog ? std::move(catalog)
        : std::make_shared<const SctPersonalCatalog>();
}
std::shared_ptr<const SctPersonalCatalog> SctCatalogResolver::catalog() {
    std::scoped_lock lock(catalogMutex);
    return installedCatalog;
}
SctResolvedCatalogEntry SctCatalogResolver::resolve(const std::uint16_t opcode) {
    SctResolvedCatalogEntry result;
    result.opcode = opcode;
    const auto* schema = spice::sct::findSctOpcodeSchema(opcode);
    if (!schema) return result;
    result.mnemonic = schema->semantic.mnemonic.empty()
        ? "Opcode" : std::string(schema->semantic.mnemonic);
    result.note = std::string(schema->semantic.notes);
    result.parameterLabels.resize(schema->parameterCatalogCount);
    for (std::size_t index = 0; index < schema->parameterCatalogCount; ++index)
        result.parameterLabels[index] = std::string(schema->parameterCatalog[index].role);
    const auto snapshot = catalog();
    if (const auto* overlay = overrideFor(*snapshot, opcode)) {
        if (overlay->mnemonic) result.mnemonic = *overlay->mnemonic;
        if (overlay->description) result.description = *overlay->description;
        if (overlay->note) result.note = *overlay->note;
        if (overlay->colorRgb) result.colorRgb = overlay->colorRgb;
        if (overlay->category) result.category = *overlay->category;
        result.parameterOverrides = overlay->parameters;
        for (const auto& parameter : overlay->parameters) {
            if (!parameter.label) continue;
            const std::span parameters{schema->parameterCatalog.data(),
                static_cast<std::size_t>(schema->parameterCatalogCount)};
            const auto found = std::ranges::find(parameters, parameter.schemaIndex,
                &spice::sct::SctOpcodeParameterSchema::schemaIndex);
            if (found != parameters.end())
                result.parameterLabels[static_cast<std::size_t>(
                    found - parameters.begin())] = *parameter.label;
        }
    }
    return result;
}
void SctCatalogResolver::applyCreationDefaults(
    spice::sct::SctInstructionFactoryRequest& request) {
    for (const auto& parameter : resolve(request.opcode).parameterOverrides)
        if (parameter.creationDefaultWord)
            request.parameterOverrides.push_back({
                spice::sct::SctParameterAddress{parameter.schemaIndex, std::nullopt},
                spice::sct::SctEncodedWordValue{*parameter.creationDefaultWord}});
}

Result<SctLegacyCatalogImportPreview> SctLegacyCatalogImporter::preview(
    const std::span<const std::byte> bytes, const SctPersonalCatalog& current) {
    try {
        const auto document = Json::parse(reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data() + bytes.size()));
        if (!document.is_object()) throw std::runtime_error("legacy catalog root is not an object");
        SctLegacyCatalogImportPreview result;
        for (const auto& [opcodeText, value] : document.items()) {
            const auto opcodeValue = std::stoul(opcodeText);
            if (opcodeValue > 0xffffu || !spice::sct::findSctOpcodeSchema(
                    static_cast<std::uint16_t>(opcodeValue)) || !value.is_object())
                throw std::runtime_error("legacy catalog opcode entry is invalid");
            SctCatalogOpcodeOverride entry;
            entry.opcode = static_cast<std::uint16_t>(opcodeValue);
            const auto* existing = overrideFor(current, entry.opcode);
            const auto takeText = [&](const char* legacy, const char* field,
                std::optional<std::string>& destination,
                const std::optional<std::string>* present) {
                if (!value.contains(legacy) || value.at(legacy).is_null()) return;
                destination = value.at(legacy).get<std::string>();
                result.changes.push_back({entry.opcode, field, *destination,
                    present && *present && **present != *destination});
            };
            takeText("Name", "mnemonic", entry.mnemonic,
                existing ? &existing->mnemonic : nullptr);
            takeText("Description", "description", entry.description,
                existing ? &existing->description : nullptr);
            takeText("Notes", "note", entry.note, existing ? &existing->note : nullptr);
            if (value.contains("Parameters") && value.at("Parameters").is_object()) {
                for (const auto& [indexText, parameterValue] : value.at("Parameters").items()) {
                    if (!parameterValue.is_object()) continue;
                    SctCatalogParameterOverride parameter;
                    parameter.schemaIndex = std::stoul(indexText);
                    if (parameterValue.contains("Name"))
                        parameter.label = parameterValue.at("Name").get<std::string>();
                    if (parameterValue.contains("Default")
                        && parameterValue.at("Default").is_number_integer())
                        parameter.creationDefaultWord = parameterValue.at("Default").get<std::uint32_t>();
                    if (parameter.label || parameter.creationDefaultWord) {
                        const SctCatalogParameterOverride* currentParameter = nullptr;
                        if (existing) {
                            const auto found = std::ranges::find(existing->parameters,
                                parameter.schemaIndex,
                                &SctCatalogParameterOverride::schemaIndex);
                            if (found != existing->parameters.end()) currentParameter = &*found;
                        }
                        const bool conflict = currentParameter != nullptr
                            && ((parameter.label && currentParameter->label
                                    && parameter.label != currentParameter->label)
                                || (parameter.creationDefaultWord
                                    && currentParameter->creationDefaultWord
                                    && parameter.creationDefaultWord
                                        != currentParameter->creationDefaultWord));
                        entry.parameters.push_back(parameter);
                        result.changes.push_back({entry.opcode,
                            "parameter." + indexText, parameter.label.value_or("default"),
                            conflict});
                    }
                    for (const auto& [field, ignored] : parameterValue.items())
                        if (field != "Name" && field != "Default")
                            result.ignoredLockedFields.push_back(opcodeText + ".Parameters."
                                + indexText + "." + field);
                }
            }
            for (const auto& [field, ignored] : value.items())
                if (field != "Name" && field != "Description" && field != "Notes"
                    && field != "Parameters")
                    result.ignoredLockedFields.push_back(opcodeText + "." + field);
            result.incoming.opcodes.push_back(std::move(entry));
        }
        auto canonical = SctAuthoringCatalogCodec::serializeCatalog(result.incoming);
        if (!canonical) return Result<SctLegacyCatalogImportPreview>::failure(
            canonical.diagnostics());
        return Result<SctLegacyCatalogImportPreview>::success(std::move(result));
    } catch (const std::exception& exception) {
        return Result<SctLegacyCatalogImportPreview>::failure(error(
            std::string("Legacy UserInstructions.json is malformed: ") + exception.what()));
    }
}

Result<SctPersonalCatalog> SctLegacyCatalogImporter::apply(
    const SctLegacyCatalogImportPreview& preview,
    const SctPersonalCatalog& current,
    const std::span<const SctLegacyCatalogFieldChange> selected) {
    auto result = current;
    const auto isSelected = [&](const std::uint16_t opcode,
                                const std::string_view field) {
        return std::ranges::any_of(selected, [&](const auto& change) {
            return change.opcode == opcode && change.field == field;
        });
    };
    for (const auto& incoming : preview.incoming.opcodes) {
        auto found = std::ranges::find(result.opcodes, incoming.opcode,
            &SctCatalogOpcodeOverride::opcode);
        if (found == result.opcodes.end()) {
            result.opcodes.push_back({.opcode = incoming.opcode});
            found = std::prev(result.opcodes.end());
        }
        if (incoming.mnemonic && isSelected(incoming.opcode, "mnemonic"))
            found->mnemonic = incoming.mnemonic;
        if (incoming.description && isSelected(incoming.opcode, "description"))
            found->description = incoming.description;
        if (incoming.note && isSelected(incoming.opcode, "note"))
            found->note = incoming.note;
        for (const auto& parameter : incoming.parameters) {
            const auto field = "parameter." + std::to_string(parameter.schemaIndex);
            if (!isSelected(incoming.opcode, field)) continue;
            auto destination = std::ranges::find(found->parameters,
                parameter.schemaIndex, &SctCatalogParameterOverride::schemaIndex);
            if (destination == found->parameters.end())
                found->parameters.push_back(parameter);
            else
                *destination = parameter;
        }
        std::ranges::sort(found->parameters, {},
            &SctCatalogParameterOverride::schemaIndex);
        if (!found->mnemonic && !found->description && !found->note
            && found->parameters.empty() && !found->colorRgb && !found->category)
            result.opcodes.erase(found);
    }
    std::ranges::sort(result.opcodes, {}, &SctCatalogOpcodeOverride::opcode);
    auto encodedResult = SctAuthoringCatalogCodec::serializeCatalog(result);
    if (!encodedResult) return Result<SctPersonalCatalog>::failure(
        encodedResult.diagnostics());
    return Result<SctPersonalCatalog>::success(std::move(result));
}

} // namespace salsa::core
