#include "SalsaCore/Persistence/SctScriptPatch.h"

#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Foundation/Hashing.h"
#include "SpiceSCT/SctScptEncoding.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] Diagnostic patchError(std::string message,
    const DiagnosticCode code = DiagnosticCode::InvalidSctPatch) {
    return {DiagnosticSeverity::Error, code, std::move(message), std::nullopt};
}

void attachPath(std::vector<Diagnostic>& diagnostics,
    const std::filesystem::path& path) {
    for (auto& diagnostic : diagnostics)
        if (!diagnostic.path) diagnostic.path = path;
}

void requireObject(const Json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size())
        throw std::runtime_error("object has missing or unknown fields");
    for (const auto field : fields)
        if (!value.contains(field)) throw std::runtime_error("object field is missing");
}

template<typename Id>
[[nodiscard]] Id id(const Json& value) {
    if (!value.is_number_unsigned()) throw std::runtime_error("entity ID is not unsigned");
    const auto raw = value.get<std::uint64_t>();
    if (raw == 0) throw std::runtime_error("entity ID zero is invalid");
    return Id(raw);
}

template<typename Id>
[[nodiscard]] Json ids(std::span<const Id> values) {
    Json result = Json::array();
    for (const auto value : values) result.push_back(value.value());
    return result;
}

template<typename Id>
[[nodiscard]] std::vector<Id> parseIds(const Json& values) {
    if (!values.is_array()) throw std::runtime_error("entity IDs are not an array");
    std::vector<Id> result;
    std::unordered_set<Id> seen;
    for (const auto& value : values) {
        const auto parsed = id<Id>(value);
        if (!seen.insert(parsed).second)
            throw std::runtime_error("entity ID array contains a duplicate");
        result.push_back(parsed);
    }
    return result;
}

template<typename Enum>
[[nodiscard]] Enum checkedEnum(const Json& value, const std::uint32_t maximum,
    const std::string_view description) {
    if (!value.is_number_unsigned())
        throw std::runtime_error(std::string(description) + " is not unsigned");
    const auto raw = value.get<std::uint64_t>();
    if (raw > maximum)
        throw std::runtime_error(std::string(description) + " is outside its supported range");
    return static_cast<Enum>(raw);
}

[[nodiscard]] Json optionalId(const auto& value) {
    return value ? Json(value->value()) : Json(nullptr);
}

template<typename Id>
[[nodiscard]] std::optional<Id> parseOptionalId(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional<Id>{id<Id>(value)};
}

[[nodiscard]] Json bytes(const std::span<const std::uint8_t> values) {
    Json result = Json::array();
    for (const auto value : values) result.push_back(value);
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> parseBytes(const Json& values) {
    if (!values.is_array()) throw std::runtime_error("bytes are not an array");
    std::vector<std::uint8_t> result;
    for (const auto& value : values) {
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 255u)
            throw std::runtime_error("byte is outside its range");
        result.push_back(value.get<std::uint8_t>());
    }
    return result;
}

[[nodiscard]] Json byteString(const std::string_view value) {
    return bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

[[nodiscard]] std::string parseByteString(const Json& value) {
    const auto parsed = parseBytes(value);
    return std::string(reinterpret_cast<const char*>(parsed.data()), parsed.size());
}

[[nodiscard]] Json words(const std::span<const std::uint32_t> values) {
    Json result = Json::array();
    for (const auto value : values) result.push_back(value);
    return result;
}

[[nodiscard]] std::vector<std::uint32_t> parseWords(const Json& values) {
    if (!values.is_array()) throw std::runtime_error("words are not an array");
    std::vector<std::uint32_t> result;
    for (const auto& value : values) {
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffull)
            throw std::runtime_error("word is outside its range");
        result.push_back(value.get<std::uint32_t>());
    }
    return result;
}

[[nodiscard]] Json encodeExpressionOperation(const spice::sct::SctScptOperation& operation) {
    return std::visit([](const auto& typed) -> Json {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, spice::sct::SctScptValueOperation>) {
            return Json{{"kind", "value"},
                {"valueKind", static_cast<std::uint32_t>(typed.kind)},
                {"encodingWord", typed.encodingWord},
                {"payloadWords", words(typed.payloadWords)}};
        } else if constexpr (std::is_same_v<T, spice::sct::SctScptBinaryOperation>) {
            return Json{{"kind", "binary"},
                {"binaryKind", static_cast<std::uint32_t>(typed.kind)},
                {"encodingWord", typed.encodingWord}};
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctScptStackOverwritePreviousWithTopOperation>) {
            return Json{{"kind", "stackOverwrite"}, {"encodingWord", typed.encodingWord}};
        } else {
            return Json{{"kind", "inert"}, {"encodingWord", typed.encodingWord}};
        }
    }, operation);
}

[[nodiscard]] spice::sct::SctScptOperation parseExpressionOperation(const Json& value) {
    if (!value.is_object() || !value.contains("kind"))
        throw std::runtime_error("expression operation is invalid");
    const auto kind = value.at("kind").get<std::string>();
    spice::sct::SctScptOperation result;
    if (kind == "value") {
        requireObject(value, {"kind", "valueKind", "encodingWord", "payloadWords"});
        result = spice::sct::SctScptValueOperation{
            checkedEnum<spice::sct::SctScptValueKind>(value.at("valueKind"), 9u,
                "SCPT value kind"),
            value.at("encodingWord").get<std::uint32_t>(),
            parseWords(value.at("payloadWords"))};
    } else if (kind == "binary") {
        requireObject(value, {"kind", "binaryKind", "encodingWord"});
        result = spice::sct::SctScptBinaryOperation{
            checkedEnum<spice::sct::SctScptBinaryOperationKind>(
                value.at("binaryKind"), 1u, "SCPT binary kind"),
            value.at("encodingWord").get<std::uint32_t>()};
    } else if (kind == "stackOverwrite") {
        requireObject(value, {"kind", "encodingWord"});
        result = spice::sct::SctScptStackOverwritePreviousWithTopOperation{
            value.at("encodingWord").get<std::uint32_t>()};
    } else if (kind == "inert") {
        requireObject(value, {"kind", "encodingWord"});
        result = spice::sct::SctScptInertOperation{
            value.at("encodingWord").get<std::uint32_t>()};
    } else {
        throw std::runtime_error("expression operation kind is invalid");
    }
    if (!spice::sct::isSctScptOperationEncodingValid(result))
        throw std::runtime_error("expression operation encoding does not match its kind");
    return result;
}

[[nodiscard]] Json encodeExpression(const spice::sct::SctCanonicalExpression& value) {
    Json body;
    if (const auto* program = std::get_if<spice::sct::SctTypedScptProgram>(&value.body)) {
        Json operations = Json::array();
        for (const auto& operation : program->operations)
            operations.push_back(encodeExpressionOperation(operation));
        body = Json{{"kind", "program"}, {"operations", std::move(operations)}};
    } else {
        body = Json{{"kind", "opaque"},
            {"words", words(std::get<spice::sct::SctOpaqueExpression>(value.body).words)}};
    }
    return Json{{"termination", static_cast<std::uint32_t>(value.termination)},
        {"body", std::move(body)}};
}

[[nodiscard]] spice::sct::SctCanonicalExpression parseExpression(const Json& value) {
    requireObject(value, {"termination", "body"});
    spice::sct::SctCanonicalExpression result;
    result.termination = checkedEnum<spice::sct::SctExpressionTermination>(
        value.at("termination"), 1u, "expression termination");
    const auto& body = value.at("body");
    if (!body.is_object() || !body.contains("kind"))
        throw std::runtime_error("expression body invalid");
    const auto kind = body.at("kind").get<std::string>();
    if (kind == "program") {
        requireObject(body, {"kind", "operations"});
        if (!body.at("operations").is_array())
            throw std::runtime_error("expression operations are not an array");
        spice::sct::SctTypedScptProgram program;
        for (const auto& operation : body.at("operations"))
            program.operations.push_back(parseExpressionOperation(operation));
        result.body = std::move(program);
    } else if (kind == "opaque") {
        requireObject(body, {"kind", "words"});
        result.body = spice::sct::SctOpaqueExpression{parseWords(body.at("words"))};
    } else throw std::runtime_error("expression body kind invalid");
    return result;
}

[[nodiscard]] Json encodeExpectedTarget(
    const spice::sct::SctExpectedReferenceTarget& target) {
    return Json{{"storage", static_cast<std::uint32_t>(target.storage)},
        {"textKind", target.textKind ? Json(static_cast<std::uint32_t>(*target.textKind))
                                      : Json(nullptr)}};
}

[[nodiscard]] spice::sct::SctExpectedReferenceTarget parseExpectedTarget(
    const Json& value) {
    requireObject(value, {"storage", "textKind"});
    spice::sct::SctExpectedReferenceTarget result;
    result.storage = checkedEnum<spice::sct::SctReferenceTargetStorage>(
        value.at("storage"), 2u, "reference target storage");
    if (!value.at("textKind").is_null())
        result.textKind = checkedEnum<spice::sct::SctTextKind>(
            value.at("textKind"), 1u, "text kind");
    return result;
}

[[nodiscard]] Json encodeReferenceTarget(
    const spice::sct::SctDocumentReferenceTarget& target) {
    return std::visit([](const auto id) -> Json {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return Json{{"kind", "instruction"}, {"id", id.value()}};
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return Json{{"kind", "string"}, {"id", id.value()}};
        return Json{{"kind", "supplementary-text"}, {"id", id.value()}};
    }, target);
}

[[nodiscard]] spice::sct::SctDocumentReferenceTarget parseReferenceTarget(
    const Json& value) {
    requireObject(value, {"kind", "id"});
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "instruction") return id<spice::sct::SctInstructionId>(value.at("id"));
    if (kind == "string") return id<spice::sct::SctStringId>(value.at("id"));
    if (kind == "supplementary-text" || kind == "footer")
        return id<spice::sct::SctSupplementaryTextId>(value.at("id"));
    throw std::runtime_error("reference target kind invalid");
}

[[nodiscard]] Json encodeParameterSite(const spice::sct::SctParameterSite& site) {
    return Json{{"instruction", site.instruction.value()},
        {"schemaIndex", site.parameter.schemaIndex},
        {"repeatedGroupOrdinal", site.parameter.repeatedGroupOrdinal
            ? Json(*site.parameter.repeatedGroupOrdinal) : Json(nullptr)}};
}

[[nodiscard]] spice::sct::SctParameterSite parseParameterSite(const Json& value) {
    requireObject(value, {"instruction", "schemaIndex", "repeatedGroupOrdinal"});
    spice::sct::SctParameterSite result;
    result.instruction = id<spice::sct::SctInstructionId>(value.at("instruction"));
    result.parameter.schemaIndex = value.at("schemaIndex").get<std::uint32_t>();
    if (!value.at("repeatedGroupOrdinal").is_null())
        result.parameter.repeatedGroupOrdinal =
            value.at("repeatedGroupOrdinal").get<std::uint32_t>();
    return result;
}

[[nodiscard]] Json encodeUnboundOrigin(const SctUnboundReferenceOrigin& origin) {
    return Json{{"site", encodeParameterSite(origin.site)},
        {"sourceAssetIdentity", origin.sourceAssetIdentity},
        {"sourceTarget", encodeReferenceTarget(origin.sourceTarget)},
        {"sourceTargetNameBytes", origin.sourceTargetNameBytes
            ? byteString(*origin.sourceTargetNameBytes) : Json(nullptr)}};
}

[[nodiscard]] SctUnboundReferenceOrigin parseUnboundOrigin(const Json& value) {
    requireObject(value, {"site", "sourceAssetIdentity", "sourceTarget",
        "sourceTargetNameBytes"});
    SctUnboundReferenceOrigin result;
    result.site = parseParameterSite(value.at("site"));
    result.sourceAssetIdentity = value.at("sourceAssetIdentity").get<std::string>();
    result.sourceTarget = parseReferenceTarget(value.at("sourceTarget"));
    if (!value.at("sourceTargetNameBytes").is_null())
        result.sourceTargetNameBytes = parseByteString(value.at("sourceTargetNameBytes"));
    return result;
}

[[nodiscard]] Json encodeVariableKey(const SctVariableKey& value) {
    return Json{{"kind", static_cast<std::uint32_t>(value.kind)}, {"index", value.index}};
}

[[nodiscard]] SctVariableKey parseVariableKey(const Json& value) {
    requireObject(value, {"kind", "index"});
    return {checkedEnum<SctVariableKind>(value.at("kind"), 3u, "variable kind"),
        value.at("index").get<std::uint32_t>()};
}

[[nodiscard]] Json encodeAlias(const SctVariableAlias& value) {
    return Json{{"variable", encodeVariableKey(value.variable)}, {"alias", value.alias}};
}

[[nodiscard]] SctVariableAlias parseAlias(const Json& value) {
    requireObject(value, {"variable", "alias"});
    SctVariableAlias result{parseVariableKey(value.at("variable")),
        value.at("alias").get<std::string>()};
    if (result.alias.empty()) throw std::runtime_error("variable alias is empty");
    return result;
}

[[nodiscard]] Json encodeAuthoringTarget(const SctAuthoringTarget& value) {
    return Json{{"kind", static_cast<std::uint32_t>(value.kind)}, {"id", value.id},
        {"variableKind", value.variableKind
            ? Json(static_cast<std::uint32_t>(*value.variableKind)) : Json(nullptr)}};
}

[[nodiscard]] SctAuthoringTarget parseAuthoringTarget(const Json& value) {
    requireObject(value, {"kind", "id", "variableKind"});
    SctAuthoringTarget result;
    result.kind = checkedEnum<SctAuthoringTargetKind>(value.at("kind"), 5u,
        "authoring target kind");
    result.id = value.at("id").get<std::uint64_t>();
    if (!value.at("variableKind").is_null())
        result.variableKind = checkedEnum<SctVariableKind>(
            value.at("variableKind"), 3u, "authoring variable kind");
    if (result.kind == SctAuthoringTargetKind::Document) {
        if (result.id != 0u || result.variableKind)
            throw std::runtime_error("document authoring target is malformed");
    } else if (result.kind != SctAuthoringTargetKind::Variable && result.id == 0u) {
        throw std::runtime_error("authoring target ID zero is invalid");
    }
    if (result.kind == SctAuthoringTargetKind::Variable
        && result.id > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("authoring variable index is invalid");
    if ((result.kind == SctAuthoringTargetKind::Variable) != result.variableKind.has_value())
        throw std::runtime_error("authoring variable target kind is malformed");
    return result;
}

[[nodiscard]] Json optionalText(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] std::optional<std::string> parseOptionalText(const Json& value) {
    return value.is_null() ? std::nullopt
        : std::optional<std::string>{value.get<std::string>()};
}

[[nodiscard]] Json encodeAnnotation(const SctEntityAnnotation& value) {
    return Json{{"target", encodeAuthoringTarget(value.target)},
        {"note", optionalText(value.note)}, {"bookmarkLabel", optionalText(value.bookmarkLabel)},
        {"colorRgb", value.colorRgb ? Json(*value.colorRgb) : Json(nullptr)}};
}

[[nodiscard]] SctEntityAnnotation parseAnnotation(const Json& value) {
    requireObject(value, {"target", "note", "bookmarkLabel", "colorRgb"});
    SctEntityAnnotation result;
    result.target = parseAuthoringTarget(value.at("target"));
    result.note = parseOptionalText(value.at("note"));
    result.bookmarkLabel = parseOptionalText(value.at("bookmarkLabel"));
    if (!value.at("colorRgb").is_null()) {
        const auto color = value.at("colorRgb").get<std::uint32_t>();
        if (color > 0xffffffu) throw std::runtime_error("annotation color is not RGB");
        result.colorRgb = color;
    }
    if (!result.note && !result.bookmarkLabel && !result.colorRgb)
        throw std::runtime_error("empty annotation records are not canonical");
    return result;
}

[[nodiscard]] Json encodeFolder(const SctSectionFolder& value) {
    return Json{{"id", value.id.value}, {"parent", value.parent
            ? Json(value.parent->value) : Json(nullptr)},
        {"name", value.name}, {"sections", ids(std::span{value.sections})},
        {"note", optionalText(value.note)}, {"bookmarkLabel", optionalText(value.bookmarkLabel)},
        {"colorRgb", value.colorRgb ? Json(*value.colorRgb) : Json(nullptr)}};
}

[[nodiscard]] SctSectionFolder parseFolder(const Json& value) {
    requireObject(value, {"id", "parent", "name", "sections", "note",
        "bookmarkLabel", "colorRgb"});
    SctSectionFolder result;
    result.id.value = value.at("id").get<std::uint64_t>();
    if (!result.id.valid()) throw std::runtime_error("section folder ID is invalid");
    if (!value.at("parent").is_null()) {
        const auto parent = value.at("parent").get<std::uint64_t>();
        if (parent == 0u) throw std::runtime_error("section folder parent ID is invalid");
        result.parent = SctSectionFolderId{parent};
    }
    result.name = value.at("name").get<std::string>();
    result.sections = parseIds<spice::sct::SctSectionId>(value.at("sections"));
    result.note = parseOptionalText(value.at("note"));
    result.bookmarkLabel = parseOptionalText(value.at("bookmarkLabel"));
    if (!value.at("colorRgb").is_null()) {
        const auto color = value.at("colorRgb").get<std::uint32_t>();
        if (color > 0xffffffu) throw std::runtime_error("folder color is not RGB");
        result.colorRgb = color;
    }
    if (result.name.empty() || result.sections.empty() || result.parent == result.id)
        throw std::runtime_error("section folder is not canonical");
    return result;
}

[[nodiscard]] Json encodeParameterValue(
    const spice::sct::SctDocumentParameterValue& value) {
    return std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, spice::sct::SctEncodedWordValue>)
            return Json{{"kind", "word"}, {"value", item.value}};
        else if constexpr (std::is_same_v<T, spice::sct::SctCanonicalExpression>)
            return Json{{"kind", "expression"}, {"value", encodeExpression(item)}};
        else if constexpr (std::is_same_v<T, spice::sct::SctTerminatedWordSequenceValue>)
            return Json{{"kind", "wordSequence"}, {"words", words(item.words)}};
        else if constexpr (std::is_same_v<T, spice::sct::SctInstructionReference>)
            return Json{{"kind", "instructionReference"}, {"target", item.target.value()}};
        else if constexpr (std::is_same_v<T, spice::sct::SctStringReference>)
            return Json{{"kind", "stringReference"}, {"target", item.target.value()}};
        else if constexpr (std::is_same_v<T, spice::sct::SctSupplementaryTextReference>)
            return Json{{"kind", "supplementaryTextReference"}, {"target", item.target.value()}};
        else if constexpr (std::is_same_v<T, spice::sct::SctUnresolvedReferenceValue>)
            return Json{{"kind", "unresolvedReference"},
                {"expected", encodeExpectedTarget(item.expectedTarget)},
                {"words", words(item.encodedWords)}};
        else
            return Json{{"kind", "opaque"}, {"words", words(item.words)}};
    }, value);
}

[[nodiscard]] spice::sct::SctDocumentParameterValue parseParameterValue(
    const Json& value) {
    if (!value.is_object() || !value.contains("kind")) throw std::runtime_error("parameter value invalid");
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "word") {
        requireObject(value, {"kind", "value"});
        return spice::sct::SctEncodedWordValue{value.at("value").get<std::uint32_t>()};
    }
    if (kind == "expression") {
        requireObject(value, {"kind", "value"});
        return parseExpression(value.at("value"));
    }
    if (kind == "wordSequence") {
        requireObject(value, {"kind", "words"});
        return spice::sct::SctTerminatedWordSequenceValue{parseWords(value.at("words"))};
    }
    if (kind == "instructionReference") {
        requireObject(value, {"kind", "target"});
        return spice::sct::SctInstructionReference{id<spice::sct::SctInstructionId>(value.at("target"))};
    }
    if (kind == "stringReference") {
        requireObject(value, {"kind", "target"});
        return spice::sct::SctStringReference{id<spice::sct::SctStringId>(value.at("target"))};
    }
    if (kind == "supplementaryTextReference" || kind == "footerReference") {
        requireObject(value, {"kind", "target"});
        return spice::sct::SctSupplementaryTextReference{id<spice::sct::SctSupplementaryTextId>(value.at("target"))};
    }
    if (kind == "unresolvedReference") {
        requireObject(value, {"kind", "expected", "words"});
        return spice::sct::SctUnresolvedReferenceValue{
            parseExpectedTarget(value.at("expected")), parseWords(value.at("words"))};
    }
    if (kind == "opaque") {
        requireObject(value, {"kind", "words"});
        return spice::sct::SctOpaqueParameterValue{parseWords(value.at("words"))};
    }
    throw std::runtime_error("parameter value kind invalid");
}

[[nodiscard]] Json encodeParameter(const spice::sct::SctDocumentParameter& parameter) {
    return Json{{"schemaIndex", parameter.schemaIndex},
        {"value", encodeParameterValue(parameter.value)}};
}

[[nodiscard]] spice::sct::SctDocumentParameter parseParameter(const Json& value) {
    requireObject(value, {"schemaIndex", "value"});
    return {value.at("schemaIndex").get<std::uint32_t>(),
        parseParameterValue(value.at("value"))};
}

[[nodiscard]] Json encodeInstruction(const spice::sct::SctDocumentInstruction& value) {
    Json fixed = Json::array();
    for (const auto& parameter : value.fixedParameters) fixed.push_back(encodeParameter(parameter));
    Json repeated = Json::array();
    for (const auto& group : value.repeatedParameterGroups) {
        Json parameters = Json::array();
        for (const auto& parameter : group.parameters) parameters.push_back(encodeParameter(parameter));
        repeated.push_back(Json{{"parameters", std::move(parameters)}});
    }
    return Json{{"id", value.id.value()}, {"opcode", value.opcode},
        {"skipRefresh", value.skipRefresh},
        {"scheduledExpression", value.scheduledExpression
            ? Json(encodeExpression(*value.scheduledExpression)) : Json(nullptr)},
        {"fixedParameters", std::move(fixed)}, {"repeatedGroups", std::move(repeated)}};
}

[[nodiscard]] spice::sct::SctDocumentInstruction parseInstruction(const Json& value) {
    requireObject(value, {"id", "opcode", "skipRefresh", "scheduledExpression",
        "fixedParameters", "repeatedGroups"});
    spice::sct::SctDocumentInstruction result;
    result.id = id<spice::sct::SctInstructionId>(value.at("id"));
    result.opcode = value.at("opcode").get<std::uint16_t>();
    result.skipRefresh = value.at("skipRefresh").get<bool>();
    if (!value.at("scheduledExpression").is_null())
        result.scheduledExpression = parseExpression(value.at("scheduledExpression"));
    for (const auto& parameter : value.at("fixedParameters"))
        result.fixedParameters.push_back(parseParameter(parameter));
    for (const auto& group : value.at("repeatedGroups")) {
        requireObject(group, {"parameters"});
        spice::sct::SctDocumentRepeatedParameterGroup parsed;
        for (const auto& parameter : group.at("parameters"))
            parsed.parameters.push_back(parseParameter(parameter));
        result.repeatedParameterGroups.push_back(std::move(parsed));
    }
    return result;
}

[[nodiscard]] Json encodeText(const spice::sct::SctTextValue& value) {
    return std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, spice::sct::SctPlainText>)
            return Json{{"kind", "plain"}, {"utf8", item.utf8}};
        else if constexpr (std::is_same_v<T, spice::sct::SctOpaqueText>)
            return Json{{"kind", "opaque"}, {"bytes", bytes(item.bytes)}};
        else if constexpr (std::is_same_v<T, spice::sct::SctEmptyIndexedText>)
            return Json{{"kind", "empty"}};
        else {
            Json elements = Json::array();
            for (const auto& element : item.body.elements) {
                std::visit([&](const auto& body) {
                    using E = std::decay_t<decltype(body)>;
                    if constexpr (std::is_same_v<E, spice::sct::SctTextChunk>)
                        elements.push_back(Json{{"kind", "text"}, {"utf8", body.utf8}});
                    else {
                        Json argument;
                        std::visit([&](const auto& arg) {
                            using A = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<A, spice::sct::SctNoCommandArgument>)
                                argument = Json{{"kind", "none"}};
                            else if constexpr (std::is_same_v<A, spice::sct::SctDecimalCommandArgument>)
                                argument = Json{{"kind", "decimal"},
                                    {"value", arg.value ? Json(*arg.value) : Json(nullptr)}};
                            else
                                argument = Json{{"kind", "bytes"}, {"values", bytes(arg.values)}};
                        }, body.argument);
                        elements.push_back(Json{{"kind", "command"},
                            {"code", static_cast<std::uint32_t>(body.code)},
                            {"argument", std::move(argument)}});
                    }
                }, element);
            }
            return Json{{"kind", "message"},
                {"header", item.headerUtf8 ? Json(*item.headerUtf8) : Json(nullptr)},
                {"elements", std::move(elements)}};
        }
    }, value);
}

[[nodiscard]] spice::sct::SctTextValue parseText(const Json& value) {
    if (!value.is_object() || !value.contains("kind")) throw std::runtime_error("text value invalid");
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "plain") {
        requireObject(value, {"kind", "utf8"});
        return spice::sct::SctPlainText{value.at("utf8").get<std::string>()};
    }
    if (kind == "opaque") {
        requireObject(value, {"kind", "bytes"});
        return spice::sct::SctOpaqueText{parseBytes(value.at("bytes"))};
    }
    if (kind == "empty") {
        requireObject(value, {"kind"});
        return spice::sct::SctEmptyIndexedText{};
    }
    if (kind != "message") throw std::runtime_error("text value kind invalid");
    requireObject(value, {"kind", "header", "elements"});
    spice::sct::SctMessage message;
    if (!value.at("header").is_null()) message.headerUtf8 = value.at("header").get<std::string>();
    for (const auto& element : value.at("elements")) {
        if (!element.is_object() || !element.contains("kind")) throw std::runtime_error("message element invalid");
        const auto elementKind = element.at("kind").get<std::string>();
        if (elementKind == "text") {
            requireObject(element, {"kind", "utf8"});
            message.body.elements.push_back(spice::sct::SctTextChunk{
                element.at("utf8").get<std::string>()});
            continue;
        }
        if (elementKind != "command") throw std::runtime_error("message element kind invalid");
        requireObject(element, {"kind", "code", "argument"});
        spice::sct::SctInlineCommand command;
        command.code = checkedEnum<spice::sct::SctMessageCommandCode>(
            element.at("code"), 11u, "message command code");
        const auto& argument = element.at("argument");
        if (!argument.is_object() || !argument.contains("kind")) throw std::runtime_error("command argument invalid");
        const auto argumentKind = argument.at("kind").get<std::string>();
        if (argumentKind == "none") {
            requireObject(argument, {"kind"});
            command.argument = spice::sct::SctNoCommandArgument{};
        } else if (argumentKind == "decimal") {
            requireObject(argument, {"kind", "value"});
            spice::sct::SctDecimalCommandArgument parsed;
            if (!argument.at("value").is_null()) parsed.value = argument.at("value").get<std::uint32_t>();
            command.argument = parsed;
        } else if (argumentKind == "bytes") {
            requireObject(argument, {"kind", "values"});
            command.argument = spice::sct::SctByteListCommandArgument{
                parseBytes(argument.at("values"))};
        } else throw std::runtime_error("command argument kind invalid");
        message.body.elements.push_back(std::move(command));
    }
    return message;
}

[[nodiscard]] Json encodeSection(const spice::sct::SctDocumentSection& value) {
    Json content = std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, spice::sct::SctScriptSectionContent>) {
            Json instructions = Json::array();
            for (const auto& instruction : item.instructions)
                instructions.push_back(encodeInstruction(instruction));
            return Json{{"kind", "script"}, {"instructions", std::move(instructions)}};
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringSectionContent>) {
            return Json{{"kind", "string"}, {"stringId", item.string.id.value()},
                {"textKind", static_cast<std::uint32_t>(item.string.kind)},
                {"value", encodeText(item.string.value)},
                {"preambleWords", words(item.preambleWords)}};
        } else if constexpr (std::is_same_v<T,
                spice::sct::SctStringGroupMarkerSectionContent>)
            return Json{{"kind", "stringGroupMarker"},
                {"preambleWords", words(item.preambleWords)}};
        else return Json{{"kind", "opaque"}};
    }, value.content);
    return Json{{"id", value.id.value()}, {"nameBytes", byteString(value.nameBytes)},
        {"content", std::move(content)}};
}

[[nodiscard]] spice::sct::SctDocumentSection parseSection(const Json& value) {
    requireObject(value, {"id", "nameBytes", "content"});
    spice::sct::SctDocumentSection result;
    result.id = id<spice::sct::SctSectionId>(value.at("id"));
    result.nameBytes = parseByteString(value.at("nameBytes"));
    const auto& content = value.at("content");
    if (!content.is_object() || !content.contains("kind")) throw std::runtime_error("section content invalid");
    const auto kind = content.at("kind").get<std::string>();
    if (kind == "script") {
        requireObject(content, {"kind", "instructions"});
        spice::sct::SctScriptSectionContent script;
        for (const auto& instruction : content.at("instructions"))
            script.instructions.push_back(parseInstruction(instruction));
        result.content = std::move(script);
    } else if (kind == "string") {
        requireObject(content, {"kind", "stringId", "textKind", "value", "preambleWords"});
        spice::sct::SctStringSectionContent strings;
        strings.string.id = id<spice::sct::SctStringId>(content.at("stringId"));
        strings.string.kind = checkedEnum<spice::sct::SctTextKind>(
            content.at("textKind"), 1u, "indexed text kind");
        strings.string.value = parseText(content.at("value"));
        strings.preambleWords = parseWords(content.at("preambleWords"));
        result.content = std::move(strings);
    } else if (kind == "stringGroupMarker") {
        requireObject(content, {"kind", "preambleWords"});
        result.content = spice::sct::SctStringGroupMarkerSectionContent{
            parseWords(content.at("preambleWords"))};
    } else if (kind == "opaque") {
        requireObject(content, {"kind"});
        result.content = spice::sct::SctOpaqueSectionContent{};
    } else throw std::runtime_error("section content kind invalid");
    return result;
}

[[nodiscard]] Json encodeSupplementaryText(const spice::sct::SctDocumentSupplementaryText& value) {
    return Json{{"id", value.id.value()}, {"kind", static_cast<std::uint32_t>(value.kind)},
        {"value", encodeText(value.value)}};
}

[[nodiscard]] spice::sct::SctDocumentSupplementaryText parseSupplementaryText(const Json& value) {
    requireObject(value, {"id", "kind", "value"});
    return {id<spice::sct::SctSupplementaryTextId>(value.at("id")),
        checkedEnum<spice::sct::SctTextKind>(
            value.at("kind"), 1u, "supplementary text kind"),
        parseText(value.at("value"))};
}

[[nodiscard]] Json encodeTarget(const SctTextTarget& target) {
    return std::visit([](const auto value) -> Json {
        using T = std::decay_t<decltype(value)>;
        return Json{{"kind", std::is_same_v<T, spice::sct::SctStringId>
                ? "indexed" : "supplementary"}, {"id", value.value()}};
    }, target);
}

[[nodiscard]] SctTextTarget parseTarget(const Json& value) {
    requireObject(value, {"kind", "id"});
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "indexed") return id<spice::sct::SctStringId>(value.at("id"));
    if (kind == "supplementary" || kind == "footer")
        return id<spice::sct::SctSupplementaryTextId>(value.at("id"));
    throw std::runtime_error("text target kind invalid");
}

[[nodiscard]] Json encodeArm(const SctAuthoredArm& value) {
    return Json{{"id", value.id.value}, {"section", value.controller.section.value()},
        {"instruction", value.controller.instruction.value()},
        {"kind", static_cast<std::uint32_t>(value.kind)},
        {"caseValue", value.caseValue ? Json(*value.caseValue) : Json(nullptr)},
        {"expectedJoin", optionalId(value.expectedJoin)},
        {"members", ids(std::span<const spice::sct::SctInstructionId>{value.members})},
        {"managedScaffolding", ids(std::span<const spice::sct::SctInstructionId>{
            value.managedScaffolding})},
        {"realization", static_cast<std::uint32_t>(value.realization)}};
}

[[nodiscard]] SctAuthoredArm parseArm(const Json& value) {
    requireObject(value, {"id", "section", "instruction", "kind", "caseValue",
        "expectedJoin", "members", "managedScaffolding", "realization"});
    SctAuthoredArm result;
    result.id.value = value.at("id").get<std::uint64_t>();
    if (!result.id.valid()) throw std::runtime_error("authored arm ID invalid");
    result.controller.section = id<spice::sct::SctSectionId>(value.at("section"));
    result.controller.instruction = id<spice::sct::SctInstructionId>(value.at("instruction"));
    result.kind = checkedEnum<spice::sct::SctStructuredArmKind>(
        value.at("kind"), 3u, "authored arm kind");
    if (!value.at("caseValue").is_null()) result.caseValue = value.at("caseValue").get<std::int32_t>();
    result.expectedJoin = parseOptionalId<spice::sct::SctInstructionId>(value.at("expectedJoin"));
    result.members = parseIds<spice::sct::SctInstructionId>(value.at("members"));
    result.managedScaffolding = parseIds<spice::sct::SctInstructionId>(value.at("managedScaffolding"));
    result.realization = checkedEnum<SctAuthoredArmRealization>(
        value.at("realization"), 1u, "authored arm realization");
    return result;
}

[[nodiscard]] Json encodeRepair(const SctPatchedTextRepair& value) {
    return Json{{"target", encodeTarget(value.target)},
        {"characters", static_cast<std::uint32_t>(value.provenance.encoding.characters)},
        {"messageSpace", static_cast<std::uint32_t>(value.provenance.encoding.messageSpace)},
        {"knownConvention", value.provenance.knownConvention
            ? Json(static_cast<std::uint32_t>(*value.provenance.knownConvention)) : Json(nullptr)},
        {"sourceSha256", value.provenance.sourceSha256}};
}

[[nodiscard]] SctPatchedTextRepair parseRepair(const Json& value) {
    requireObject(value, {"target", "characters", "messageSpace", "knownConvention", "sourceSha256"});
    SctPatchedTextRepair result;
    result.target = parseTarget(value.at("target"));
    result.provenance.encoding.characters = checkedEnum<spice::sct::SctCharacterEncoding>(
        value.at("characters"), 1u, "repair character encoding");
    result.provenance.encoding.messageSpace = checkedEnum<spice::sct::SctMessageSpaceEncoding>(
        value.at("messageSpace"), 1u, "repair message-space encoding");
    if (!value.at("knownConvention").is_null())
        result.provenance.knownConvention = checkedEnum<spice::sct::SctKnownTextConvention>(
            value.at("knownConvention"), 2u, "known text convention");
    result.provenance.sourceSha256 = value.at("sourceSha256").get<std::string>();
    if (result.provenance.sourceSha256.size() != 64u
        || !std::ranges::all_of(result.provenance.sourceSha256, [](const char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        }))
        throw std::runtime_error("repair source SHA-256 is not canonical lowercase hexadecimal");
    if (result.provenance.knownConvention) {
        const auto knownEncoding = spice::sct::sctTextEncodingFor(
            *result.provenance.knownConvention);
        if (!knownEncoding || *knownEncoding != result.provenance.encoding)
            throw std::runtime_error("repair convention does not match its explicit encoding");
    }
    return result;
}

[[nodiscard]] Json encodeAllocatorState(const SctPatchedAllocatorState& value) {
    return Json{{"nextSectionId", value.nextSectionId},
        {"nextInstructionId", value.nextInstructionId},
        {"nextStringId", value.nextStringId},
        {"nextSupplementaryTextId", value.nextSupplementaryTextId},
        {"nextOpaqueAttachmentId", value.nextOpaqueAttachmentId}};
}

[[nodiscard]] SctPatchedAllocatorState parseAllocatorState(const Json& value) {
    if (!value.is_object()) throw std::runtime_error("allocator state is not an object");
    const bool legacy = value.contains("nextFooterEntryId");
    requireObject(value, {"nextSectionId", "nextInstructionId", "nextStringId",
        legacy ? "nextFooterEntryId" : "nextSupplementaryTextId",
        "nextOpaqueAttachmentId"});
    SctPatchedAllocatorState result{
        value.at("nextSectionId").get<std::uint64_t>(),
        value.at("nextInstructionId").get<std::uint64_t>(),
        value.at("nextStringId").get<std::uint64_t>(),
        value.at(legacy ? "nextFooterEntryId" : "nextSupplementaryTextId")
            .get<std::uint64_t>(),
        value.at("nextOpaqueAttachmentId").get<std::uint64_t>(),
    };
    if (result.nextSectionId == 0 || result.nextInstructionId == 0
        || result.nextStringId == 0 || result.nextSupplementaryTextId == 0
        || result.nextOpaqueAttachmentId == 0)
        throw std::runtime_error("allocator state contains an invalid zero value");
    return result;
}

[[nodiscard]] std::string targetKey(const SctTextTarget& target) {
    return std::visit([](const auto value) {
        using T = std::decay_t<decltype(value)>;
        return std::string(std::is_same_v<T, spice::sct::SctStringId> ? "S:" : "F:")
            + std::to_string(value.value());
    }, target);
}

[[nodiscard]] const spice::sct::SctTextValue* findText(
    const spice::sct::SctDocument& document, const SctTextTarget& target) {
    return std::visit([&](const auto id) -> const spice::sct::SctTextValue* {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            for (const auto& section : document.sections)
                if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(
                        &section.content); text && text->string.id == id)
                    return &text->string.value;
        } else {
            const auto found = std::ranges::find(document.supplementaryText, id,
                &spice::sct::SctDocumentSupplementaryText::id);
            if (found != document.supplementaryText.end()) return &found->value;
        }
        return nullptr;
    }, target);
}

void verifyTextRepairs(const spice::sct::SctDocument& baseline,
    const spice::sct::SctDocument& working,
    const std::span<const SctPatchedTextRepair> repairs) {
    std::unordered_set<std::string> targets;
    for (const auto& repair : repairs) {
        if (!targets.insert(targetKey(repair.target)).second)
            throw std::runtime_error("text repair provenance contains a duplicate target");
        const auto* source = findText(baseline, repair.target);
        const auto* current = findText(working, repair.target);
        if (source == nullptr || current == nullptr)
            throw std::runtime_error("text repair provenance targets a missing text entity");
        const auto* opaque = std::get_if<spice::sct::SctOpaqueText>(source);
        if (opaque == nullptr || std::holds_alternative<spice::sct::SctOpaqueText>(*current))
            throw std::runtime_error("text repair provenance does not describe an opaque-to-semantic repair");
        const auto bytes = std::as_bytes(std::span{opaque->bytes});
        const auto digest = sha256(bytes);
        if (!digest || digest.value().toHex() != repair.provenance.sourceSha256)
            throw std::runtime_error("text repair provenance does not match its source bytes");
    }
}

template<typename T, typename Id>
[[nodiscard]] const T* findById(const std::vector<T>& values, const Id target) {
    const auto found = std::ranges::find(values, target, &T::id);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] bool sameInstruction(const spice::sct::SctDocumentInstruction& left,
    const spice::sct::SctDocumentInstruction& right) {
    return encodeInstruction(left) == encodeInstruction(right);
}

[[nodiscard]] bool sameText(const spice::sct::SctTextValue& left,
    const spice::sct::SctTextValue& right) {
    return encodeText(left) == encodeText(right);
}

[[nodiscard]] bool sameOpaqueAttachment(
    const spice::sct::SctOpaqueAttachment& left,
    const spice::sct::SctOpaqueAttachment& right) {
    return left.id == right.id && left.bytes == right.bytes
        && left.anchor == right.anchor && left.placement == right.placement
        && left.fixedOffset == right.fixedOffset && left.alignment == right.alignment
        && left.relocation == right.relocation && left.reason == right.reason;
}

[[nodiscard]] SctPatchedAllocatorState allocatorState(
    const spice::sct::SctDocument& document) {
    return {document.nextSectionIdValue(), document.nextInstructionIdValue(),
        document.nextStringIdValue(), document.nextSupplementaryTextIdValue(),
        document.nextOpaqueAttachmentIdValue()};
}

template<typename Id>
[[nodiscard]] bool uniqueIds(std::span<const Id> values) {
    std::unordered_set<Id> seen;
    return std::ranges::all_of(values, [&](const Id value) {
        return static_cast<bool>(value) && seen.insert(value).second;
    });
}

template<typename T, typename Id>
[[nodiscard]] bool reorder(std::vector<T>& values, const std::vector<Id>& order) {
    if (order.empty()) return true;
    if (values.size() != order.size() || !uniqueIds<Id>(order)) return false;
    std::vector<T> next;
    next.reserve(values.size());
    for (const auto itemId : order) {
        const auto found = std::ranges::find(values, itemId, &T::id);
        if (found == values.end()) return false;
        next.push_back(std::move(*found));
    }
    values = std::move(next);
    return true;
}

void advanceAllocators(spice::sct::SctDocument& document) {
    std::uint64_t maxSection = 0, maxInstruction = 0, maxString = 0, maxSupplementaryText = 0,
        maxOpaque = 0;
    for (const auto& section : document.sections) {
        maxSection = std::max(maxSection, section.id.value());
        if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&section.content))
            for (const auto& instruction : script->instructions)
                maxInstruction = std::max(maxInstruction, instruction.id.value());
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(&section.content))
            maxString = std::max(maxString, text->string.id.value());
    }
    for (const auto& entry : document.supplementaryText)
        maxSupplementaryText = std::max(maxSupplementaryText, entry.id.value());
    for (const auto& attachment : document.opaqueAttachments)
        maxOpaque = std::max(maxOpaque, attachment.id.value());
    while (document.nextSectionIdValue() <= maxSection) (void)document.allocateSectionId();
    while (document.nextInstructionIdValue() <= maxInstruction) (void)document.allocateInstructionId();
    while (document.nextStringIdValue() <= maxString) (void)document.allocateStringId();
    while (document.nextSupplementaryTextIdValue() <= maxSupplementaryText) (void)document.allocateSupplementaryTextId();
    while (document.nextOpaqueAttachmentIdValue() <= maxOpaque)
        (void)document.allocateOpaqueAttachmentId();
}

void advanceAllocatorsTo(spice::sct::SctDocument& document,
    const SctPatchedAllocatorState& target) {
    if (document.nextSectionIdValue() > target.nextSectionId
        || document.nextInstructionIdValue() > target.nextInstructionId
        || document.nextStringIdValue() > target.nextStringId
        || document.nextSupplementaryTextIdValue() > target.nextSupplementaryTextId
        || document.nextOpaqueAttachmentIdValue() > target.nextOpaqueAttachmentId)
        throw std::runtime_error("patched allocator state moves an allocator backward");
    while (document.nextSectionIdValue() < target.nextSectionId)
        (void)document.allocateSectionId();
    while (document.nextInstructionIdValue() < target.nextInstructionId)
        (void)document.allocateInstructionId();
    while (document.nextStringIdValue() < target.nextStringId)
        (void)document.allocateStringId();
    while (document.nextSupplementaryTextIdValue() < target.nextSupplementaryTextId)
        (void)document.allocateSupplementaryTextId();
    while (document.nextOpaqueAttachmentIdValue() < target.nextOpaqueAttachmentId)
        (void)document.allocateOpaqueAttachmentId();
}

[[nodiscard]] bool sameSection(const spice::sct::SctDocumentSection& left,
    const spice::sct::SctDocumentSection& right) {
    return encodeSection(left) == encodeSection(right);
}

[[nodiscard]] bool sameSupplementaryText(const spice::sct::SctDocumentSupplementaryText& left,
    const spice::sct::SctDocumentSupplementaryText& right) {
    return encodeSupplementaryText(left) == encodeSupplementaryText(right);
}

[[nodiscard]] bool sameArm(const SctAuthoredArm& left,
    const SctAuthoredArm& right) {
    return encodeArm(left) == encodeArm(right);
}

[[nodiscard]] bool sameRepair(const SctPatchedTextRepair& left,
    const SctPatchedTextRepair& right) {
    return encodeRepair(left) == encodeRepair(right);
}

template<typename T>
void requireDelta(const SctValueDelta<T>& delta, const char* label) {
    if (!delta.before && !delta.after)
        throw std::runtime_error(std::string(label) + " delta has no before or after value");
}

template<typename T, typename Id>
[[nodiscard]] T* findMutableById(std::vector<T>& values, const Id target) {
    const auto found = std::ranges::find(values, target, &T::id);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] const SctPatchedTextRepair* findRepair(
    const std::vector<SctPatchedTextRepair>& values, const SctTextTarget& target) {
    const auto key = targetKey(target);
    const auto found = std::ranges::find_if(values, [&](const auto& value) {
        return targetKey(value.target) == key;
    });
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] std::string siteKey(const spice::sct::SctParameterSite& site) {
    return std::to_string(site.instruction.value()) + ":"
        + std::to_string(site.parameter.schemaIndex) + ":"
        + (site.parameter.repeatedGroupOrdinal
            ? std::to_string(*site.parameter.repeatedGroupOrdinal) : "-");
}

[[nodiscard]] const SctUnboundReferenceOrigin* findUnboundReference(
    const std::vector<SctUnboundReferenceOrigin>& values,
    const spice::sct::SctParameterSite& site) {
    const auto found = std::ranges::find(values, site,
        &SctUnboundReferenceOrigin::site);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] const SctVariableAlias* findAlias(
    const std::vector<SctVariableAlias>& values, const SctVariableKey key) {
    const auto found = std::ranges::find(values, key, &SctVariableAlias::variable);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] const SctEntityAnnotation* findAnnotation(
    const std::vector<SctEntityAnnotation>& values, const SctAuthoringTarget target) {
    const auto found = std::ranges::find(values, target, &SctEntityAnnotation::target);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] const SctSectionFolder* findFolder(
    const std::vector<SctSectionFolder>& values, const SctSectionFolderId id) {
    const auto found = std::ranges::find(values, id, &SctSectionFolder::id);
    return found == values.end() ? nullptr : &*found;
}

void validateAuthoringState(const spice::sct::SctDocument& document,
    const std::vector<SctVariableAlias>& aliases,
    const std::vector<SctEntityAnnotation>& annotations,
    const std::vector<SctSectionFolder>& folders) {
    std::set<SctVariableKey> aliasKeys;
    std::set<std::string, std::less<>> aliasNames;
    for (const auto& alias : aliases) {
        auto name = alias.alias;
        std::ranges::transform(name, name.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (alias.alias.empty() || !aliasKeys.insert(alias.variable).second
            || !aliasNames.insert(std::move(name)).second)
            throw std::runtime_error("variable aliases are empty or duplicated");
    }
    const auto targetExists = [&](const SctAuthoringTarget& target) {
        switch (target.kind) {
        case SctAuthoringTargetKind::Document:
            return target.id == 0u && !target.variableKind;
        case SctAuthoringTargetKind::Section:
            return !target.variableKind && findById(document.sections,
                spice::sct::SctSectionId(target.id)) != nullptr;
        case SctAuthoringTargetKind::Instruction:
            if (target.variableKind) return false;
            for (const auto& section : document.sections)
                if (const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(
                        &section.content);
                    script && findById(script->instructions,
                        spice::sct::SctInstructionId(target.id))) return true;
            return false;
        case SctAuthoringTargetKind::String:
            if (target.variableKind) return false;
            for (const auto& section : document.sections)
                if (const auto* string = std::get_if<spice::sct::SctStringSectionContent>(
                        &section.content);
                    string && string->string.id.value() == target.id) return true;
            return false;
        case SctAuthoringTargetKind::SupplementaryText:
            return !target.variableKind && findById(document.supplementaryText,
                spice::sct::SctSupplementaryTextId(target.id)) != nullptr;
        case SctAuthoringTargetKind::Variable:
            return target.variableKind
                && static_cast<std::uint8_t>(*target.variableKind)
                    <= static_cast<std::uint8_t>(SctVariableKind::Float)
                && target.id <= std::numeric_limits<std::uint32_t>::max();
        }
        return false;
    };
    std::set<SctAuthoringTarget> annotationTargets;
    for (const auto& annotation : annotations)
        if (!targetExists(annotation.target)
            || !annotationTargets.insert(annotation.target).second)
            throw std::runtime_error("entity annotations target absent or duplicated entities");

    std::set<SctSectionFolderId> folderIds;
    std::map<std::optional<SctSectionFolderId>, std::set<std::string, std::less<>>>
        siblingNames;
    std::unordered_map<std::uint64_t, const SctSectionFolder*> byId;
    std::unordered_map<std::uint64_t, std::size_t> positions;
    for (std::size_t index = 0; index < document.sections.size(); ++index)
        positions.emplace(document.sections[index].id.value(), index);
    for (const auto& folder : folders) {
        auto name = folder.name;
        std::ranges::transform(name, name.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (!folder.id.valid() || folder.name.empty() || folder.sections.empty()
            || !folderIds.insert(folder.id).second
            || !siblingNames[folder.parent].insert(std::move(name)).second)
            throw std::runtime_error("section folders are empty or duplicated");
        std::set<spice::sct::SctSectionId> unique;
        std::vector<std::size_t> ordered;
        for (const auto section : folder.sections) {
            const auto position = positions.find(section.value());
            if (position == positions.end() || !unique.insert(section).second)
                throw std::runtime_error("a section folder references an absent or duplicate section");
            ordered.push_back(position->second);
        }
        std::ranges::sort(ordered);
        if (ordered.back() - ordered.front() + 1u != ordered.size())
            throw std::runtime_error("a section folder is not physically contiguous");
        byId.emplace(folder.id.value, &folder);
    }
    for (const auto& folder : folders) {
        std::set<std::uint64_t> ancestry{folder.id.value};
        auto parent = folder.parent;
        while (parent) {
            const auto found = byId.find(parent->value);
            if (found == byId.end() || !ancestry.insert(parent->value).second)
                throw std::runtime_error("a section folder parent is missing or cyclic");
            if (!std::ranges::all_of(folder.sections, [&](const auto section) {
                    return std::ranges::find(found->second->sections, section)
                        != found->second->sections.end();
                })) throw std::runtime_error("a child folder escapes its parent range");
            parent = found->second->parent;
        }
    }
    for (std::size_t left = 0; left < folders.size(); ++left)
        for (std::size_t right = left + 1u; right < folders.size(); ++right) {
            if (folders[left].parent != folders[right].parent) continue;
            if (std::ranges::any_of(folders[left].sections, [&](const auto section) {
                    return std::ranges::find(folders[right].sections, section)
                        != folders[right].sections.end();
                })) throw std::runtime_error("sibling section folders overlap");
        }
}

}  // namespace

bool SalsaScriptPatch::empty() const noexcept {
    return !allocatorState.has_value()
        && !sectionOrder.has_value() && sections.empty()
        && scriptSections.empty() && textValues.empty() && !supplementaryTextOrder.has_value()
        && supplementaryText.empty()
        && authoredArms.empty() && textRepairs.empty() && unboundReferences.empty()
        && aliases.empty() && annotations.empty() && folders.empty();
}

Result<std::vector<std::byte>> SalsaScriptPatchCodec::serialize(
    const SalsaScriptPatch& patch) {
    try {
        Json document = Json::object();
        document["formatId"] = PayloadType;
        document["schemaVersion"] = SchemaVersion;
        document["sourceTextConvention"] = patch.sourceTextConvention
            ? Json(static_cast<std::uint32_t>(*patch.sourceTextConvention)) : Json(nullptr);
        document["allocatorState"] = patch.allocatorState
            ? Json{{"before", encodeAllocatorState(*patch.allocatorState->before)},
                {"after", encodeAllocatorState(*patch.allocatorState->after)}} : Json(nullptr);
        document["sectionOrder"] = patch.sectionOrder
            ? Json{{"before", ids(std::span{patch.sectionOrder->before})},
                {"after", ids(std::span{patch.sectionOrder->after})}} : Json(nullptr);
        document["sections"] = Json::array();
        for (const auto& section : patch.sections)
            document["sections"].push_back(Json{
                {"before", section.before ? encodeSection(*section.before) : Json(nullptr)},
                {"after", section.after ? encodeSection(*section.after) : Json(nullptr)}});
        document["scriptSections"] = Json::array();
        for (const auto& section : patch.scriptSections) {
            Json instructions = Json::array();
            for (const auto& instruction : section.instructions)
                instructions.push_back(Json{
                    {"before", instruction.before
                        ? encodeInstruction(*instruction.before) : Json(nullptr)},
                    {"after", instruction.after
                        ? encodeInstruction(*instruction.after) : Json(nullptr)}});
            document["scriptSections"].push_back(Json{{"section", section.section.value()},
                {"nameBytes", section.nameBytes
                    ? Json{{"before", byteString(*section.nameBytes->before)},
                        {"after", byteString(*section.nameBytes->after)}} : Json(nullptr)},
                {"instructionOrder", section.instructionOrder
                    ? Json{{"before", ids(std::span{section.instructionOrder->before})},
                        {"after", ids(std::span{section.instructionOrder->after})}} : Json(nullptr)},
                {"instructions", std::move(instructions)}});
        }
        document["textValues"] = Json::array();
        for (const auto& value : patch.textValues)
            document["textValues"].push_back(Json{{"target", encodeTarget(value.target)},
                {"before", encodeText(value.beforeValue)},
                {"after", encodeText(value.afterValue)}});
        document["supplementaryTextOrder"] = patch.supplementaryTextOrder
            ? Json{{"before", ids(std::span{patch.supplementaryTextOrder->before})},
                {"after", ids(std::span{patch.supplementaryTextOrder->after})}} : Json(nullptr);
        document["supplementaryText"] = Json::array();
        for (const auto& entry : patch.supplementaryText)
            document["supplementaryText"].push_back(Json{
                {"before", entry.before ? encodeSupplementaryText(*entry.before) : Json(nullptr)},
                {"after", entry.after ? encodeSupplementaryText(*entry.after) : Json(nullptr)}});
        document["authoredArms"] = Json::array();
        for (const auto& arm : patch.authoredArms)
            document["authoredArms"].push_back(Json{
                {"before", arm.before ? encodeArm(*arm.before) : Json(nullptr)},
                {"after", arm.after ? encodeArm(*arm.after) : Json(nullptr)}});
        document["textRepairs"] = Json::array();
        for (const auto& repair : patch.textRepairs) {
            const auto before = repair.before
                ? encodeRepair(SctPatchedTextRepair{repair.target, *repair.before}) : Json(nullptr);
            const auto after = repair.after
                ? encodeRepair(SctPatchedTextRepair{repair.target, *repair.after}) : Json(nullptr);
            document["textRepairs"].push_back(Json{{"target", encodeTarget(repair.target)},
                {"before", before}, {"after", after}});
        }
        document["unboundReferences"] = Json::array();
        for (const auto& reference : patch.unboundReferences)
            document["unboundReferences"].push_back(Json{
                {"site", encodeParameterSite(reference.site)},
                {"before", reference.before
                    ? encodeUnboundOrigin(*reference.before) : Json(nullptr)},
                {"after", reference.after
                    ? encodeUnboundOrigin(*reference.after) : Json(nullptr)}});
        document["aliases"] = Json::array();
        for (const auto& alias : patch.aliases)
            document["aliases"].push_back(Json{
                {"before", alias.before ? encodeAlias(*alias.before) : Json(nullptr)},
                {"after", alias.after ? encodeAlias(*alias.after) : Json(nullptr)}});
        document["annotations"] = Json::array();
        for (const auto& annotation : patch.annotations)
            document["annotations"].push_back(Json{
                {"before", annotation.before ? encodeAnnotation(*annotation.before) : Json(nullptr)},
                {"after", annotation.after ? encodeAnnotation(*annotation.after) : Json(nullptr)}});
        document["folders"] = Json::array();
        for (const auto& folder : patch.folders)
            document["folders"].push_back(Json{
                {"before", folder.before ? encodeFolder(*folder.before) : Json(nullptr)},
                {"after", folder.after ? encodeFolder(*folder.after) : Json(nullptr)}});
        auto text = document.dump(2);
        text.push_back('\n');
        const auto raw = std::as_bytes(std::span{text.data(), text.size()});
        return Result<std::vector<std::byte>>::success({raw.begin(), raw.end()});
    } catch (const std::exception& error) {
        return Result<std::vector<std::byte>>::failure(patchError(
            std::string("The SCT patch could not be serialized: ") + error.what()));
    }
}

Result<SalsaScriptPatch> SalsaScriptPatchCodec::deserialize(
    const std::span<const std::byte> bytesValue) {
    try {
        const std::string_view text(reinterpret_cast<const char*>(bytesValue.data()), bytesValue.size());
        const auto document = Json::parse(text);
        if (!document.is_object() || !document.contains("formatId")
            || !document.contains("schemaVersion"))
            throw std::runtime_error("patch header is malformed");
        if (document.at("formatId").get<std::string>() != PayloadType)
            throw std::runtime_error("patch format ID does not match SALSA SCT patches");
        if (!document.at("schemaVersion").is_number_unsigned())
            throw std::runtime_error("patch schema version is malformed");
        const auto schemaVersion = document.at("schemaVersion").get<std::uint32_t>();
        if (schemaVersion != SchemaVersion && schemaVersion != LegacySchemaVersion) {
            return Result<SalsaScriptPatch>::failure(patchError(
                "The SCT patch schema version is unsupported.",
                DiagnosticCode::UnsupportedSctPatchSchema));
        }
        const bool legacy = schemaVersion == LegacySchemaVersion;
        const std::string supplementaryTextOrderKey =
            legacy ? "footerOrder" : "supplementaryTextOrder";
        const std::string supplementaryTextKey =
            legacy ? "footerEntries" : "supplementaryText";
        requireObject(document, {"formatId", "schemaVersion", "sourceTextConvention",
            "allocatorState", "sectionOrder", "sections", "scriptSections",
            "textValues", supplementaryTextOrderKey, supplementaryTextKey,
            "authoredArms", "textRepairs", "unboundReferences", "aliases",
            "annotations", "folders"});
        SalsaScriptPatch patch;
        if (!document.at("sourceTextConvention").is_null())
            patch.sourceTextConvention = checkedEnum<spice::sct::SctKnownTextConvention>(
                document.at("sourceTextConvention"), 2u, "source text convention");
        if (!document.at("allocatorState").is_null()) {
            const auto& value = document.at("allocatorState");
            requireObject(value, {"before", "after"});
            patch.allocatorState = SctValueDelta<SctPatchedAllocatorState>{
                parseAllocatorState(value.at("before")), parseAllocatorState(value.at("after"))};
        }
        if (!document.at("sectionOrder").is_null()) {
            const auto& value = document.at("sectionOrder");
            requireObject(value, {"before", "after"});
            patch.sectionOrder = SctOrderDelta<spice::sct::SctSectionId>{
                parseIds<spice::sct::SctSectionId>(value.at("before")),
                parseIds<spice::sct::SctSectionId>(value.at("after"))};
        }
        for (const auto& section : document.at("sections")) {
            requireObject(section, {"before", "after"});
            SctValueDelta<spice::sct::SctDocumentSection> delta;
            if (!section.at("before").is_null()) delta.before = parseSection(section.at("before"));
            if (!section.at("after").is_null()) delta.after = parseSection(section.at("after"));
            requireDelta(delta, "section");
            patch.sections.push_back(std::move(delta));
        }
        for (const auto& section : document.at("scriptSections")) {
            requireObject(section, {"section", "nameBytes", "instructionOrder", "instructions"});
            SctPatchedScriptSection parsed;
            parsed.section = id<spice::sct::SctSectionId>(section.at("section"));
            if (!section.at("nameBytes").is_null()) {
                const auto& name = section.at("nameBytes");
                requireObject(name, {"before", "after"});
                parsed.nameBytes = SctValueDelta<std::string>{
                    parseByteString(name.at("before")), parseByteString(name.at("after"))};
            }
            if (!section.at("instructionOrder").is_null()) {
                const auto& order = section.at("instructionOrder");
                requireObject(order, {"before", "after"});
                parsed.instructionOrder = SctOrderDelta<spice::sct::SctInstructionId>{
                    parseIds<spice::sct::SctInstructionId>(order.at("before")),
                    parseIds<spice::sct::SctInstructionId>(order.at("after"))};
            }
            for (const auto& instruction : section.at("instructions")) {
                requireObject(instruction, {"before", "after"});
                SctPatchedInstruction delta;
                if (!instruction.at("before").is_null())
                    delta.before = parseInstruction(instruction.at("before"));
                if (!instruction.at("after").is_null())
                    delta.after = parseInstruction(instruction.at("after"));
                if (!delta.before && !delta.after)
                    throw std::runtime_error("instruction delta has no before or after value");
                parsed.instructions.push_back(std::move(delta));
            }
            patch.scriptSections.push_back(std::move(parsed));
        }
        for (const auto& value : document.at("textValues")) {
            requireObject(value, {"target", "before", "after"});
            patch.textValues.push_back({parseTarget(value.at("target")),
                parseText(value.at("before")), parseText(value.at("after"))});
        }
        if (!document.at(supplementaryTextOrderKey).is_null()) {
            const auto& value = document.at(supplementaryTextOrderKey);
            requireObject(value, {"before", "after"});
            patch.supplementaryTextOrder = SctOrderDelta<spice::sct::SctSupplementaryTextId>{
                parseIds<spice::sct::SctSupplementaryTextId>(value.at("before")),
                parseIds<spice::sct::SctSupplementaryTextId>(value.at("after"))};
        }
        for (const auto& entry : document.at(supplementaryTextKey)) {
            requireObject(entry, {"before", "after"});
            SctValueDelta<spice::sct::SctDocumentSupplementaryText> delta;
            if (!entry.at("before").is_null()) delta.before = parseSupplementaryText(entry.at("before"));
            if (!entry.at("after").is_null()) delta.after = parseSupplementaryText(entry.at("after"));
            requireDelta(delta, "supplementary text");
            patch.supplementaryText.push_back(std::move(delta));
        }
        for (const auto& arm : document.at("authoredArms")) {
            requireObject(arm, {"before", "after"});
            SctValueDelta<SctAuthoredArm> delta;
            if (!arm.at("before").is_null()) delta.before = parseArm(arm.at("before"));
            if (!arm.at("after").is_null()) delta.after = parseArm(arm.at("after"));
            requireDelta(delta, "authored arm");
            patch.authoredArms.push_back(std::move(delta));
        }
        for (const auto& repair : document.at("textRepairs")) {
            requireObject(repair, {"target", "before", "after"});
            SctPatchedTextRepairDelta delta{parseTarget(repair.at("target"))};
            if (!repair.at("before").is_null()) {
                auto parsed = parseRepair(repair.at("before"));
                if (targetKey(parsed.target) != targetKey(delta.target))
                    throw std::runtime_error("repair before target does not match its delta");
                delta.before = std::move(parsed.provenance);
            }
            if (!repair.at("after").is_null()) {
                auto parsed = parseRepair(repair.at("after"));
                if (targetKey(parsed.target) != targetKey(delta.target))
                    throw std::runtime_error("repair after target does not match its delta");
                delta.after = std::move(parsed.provenance);
            }
            if (!delta.before && !delta.after)
                throw std::runtime_error("text repair delta has no before or after value");
            patch.textRepairs.push_back(std::move(delta));
        }
        for (const auto& reference : document.at("unboundReferences")) {
            requireObject(reference, {"site", "before", "after"});
            SctPatchedUnboundReferenceDelta delta;
            delta.site = parseParameterSite(reference.at("site"));
            if (!reference.at("before").is_null())
                delta.before = parseUnboundOrigin(reference.at("before"));
            if (!reference.at("after").is_null())
                delta.after = parseUnboundOrigin(reference.at("after"));
            if ((!delta.before && !delta.after)
                || (delta.before && delta.before->site != delta.site)
                || (delta.after && delta.after->site != delta.site))
                throw std::runtime_error("unbound reference delta is invalid");
            patch.unboundReferences.push_back(std::move(delta));
        }
        for (const auto& item : document.at("aliases")) {
            requireObject(item, {"before", "after"});
            SctValueDelta<SctVariableAlias> delta;
            if (!item.at("before").is_null()) delta.before = parseAlias(item.at("before"));
            if (!item.at("after").is_null()) delta.after = parseAlias(item.at("after"));
            requireDelta(delta, "variable alias");
            if (delta.before && delta.after && delta.before->variable != delta.after->variable)
                throw std::runtime_error("variable alias identity changed");
            patch.aliases.push_back(std::move(delta));
        }
        for (const auto& item : document.at("annotations")) {
            requireObject(item, {"before", "after"});
            SctValueDelta<SctEntityAnnotation> delta;
            if (!item.at("before").is_null()) delta.before = parseAnnotation(item.at("before"));
            if (!item.at("after").is_null()) delta.after = parseAnnotation(item.at("after"));
            requireDelta(delta, "entity annotation");
            if (delta.before && delta.after && delta.before->target != delta.after->target)
                throw std::runtime_error("entity annotation identity changed");
            patch.annotations.push_back(std::move(delta));
        }
        for (const auto& item : document.at("folders")) {
            requireObject(item, {"before", "after"});
            SctValueDelta<SctSectionFolder> delta;
            if (!item.at("before").is_null()) delta.before = parseFolder(item.at("before"));
            if (!item.at("after").is_null()) delta.after = parseFolder(item.at("after"));
            requireDelta(delta, "section folder");
            if (delta.before && delta.after && delta.before->id != delta.after->id)
                throw std::runtime_error("section folder identity changed");
            patch.folders.push_back(std::move(delta));
        }
        return Result<SalsaScriptPatch>::success(std::move(patch));
    } catch (const std::exception& error) {
        return Result<SalsaScriptPatch>::failure(patchError(
            std::string("The SCT patch is malformed: ") + error.what()));
    }
}

Result<SalsaScriptPatch> SalsaScriptPatchService::diff(
    const SctSemanticState& baseline,
    const SctSemanticState& working,
    const std::optional<spice::sct::SctKnownTextConvention> sourceTextConvention) {
    if (!baseline.document || !working.document)
        return Result<SalsaScriptPatch>::failure(patchError(
            "A semantic diff requires complete before and after documents."));
    const auto& baselineDocument = *baseline.document;
    const auto& workingDocument = *working.document;
    SalsaScriptPatch patch;
    patch.sourceTextConvention = sourceTextConvention;
    const auto baselineAllocators = allocatorState(baselineDocument);
    const auto workingAllocators = allocatorState(workingDocument);
    if (baselineAllocators != workingAllocators)
        patch.allocatorState = SctValueDelta<SctPatchedAllocatorState>{
            baselineAllocators, workingAllocators};
    std::vector<spice::sct::SctSectionId> baselineOrder, workingOrder;
    for (const auto& section : baselineDocument.sections) baselineOrder.push_back(section.id);
    for (const auto& section : workingDocument.sections) workingOrder.push_back(section.id);
    if (baselineOrder != workingOrder)
        patch.sectionOrder = SctOrderDelta<spice::sct::SctSectionId>{baselineOrder, workingOrder};

    for (const auto& source : baselineDocument.sections) {
        const auto* target = findById(workingDocument.sections, source.id);
        if (target == nullptr) {
            patch.sections.push_back({source, std::nullopt});
            continue;
        }
        if (source.content.index() != target->content.index())
            return Result<SalsaScriptPatch>::failure(patchError(
                "A source section changed physical kind without receiving a new ID."));
        SctPatchedScriptSection sectionPatch;
        sectionPatch.section = source.id;
        if (source.nameBytes != target->nameBytes)
            sectionPatch.nameBytes = SctValueDelta<std::string>{
                source.nameBytes, target->nameBytes};
        const auto* sourceScript = std::get_if<spice::sct::SctScriptSectionContent>(&source.content);
        const auto* targetScript = std::get_if<spice::sct::SctScriptSectionContent>(&target->content);
        if (sourceScript && targetScript) {
            std::vector<spice::sct::SctInstructionId> sourceOrder, targetOrder;
            for (const auto& item : sourceScript->instructions) sourceOrder.push_back(item.id);
            for (const auto& item : targetScript->instructions) targetOrder.push_back(item.id);
            if (sourceOrder != targetOrder)
                sectionPatch.instructionOrder = SctOrderDelta<spice::sct::SctInstructionId>{
                    sourceOrder, targetOrder};
            for (const auto& item : sourceScript->instructions)
                if (!findById(targetScript->instructions, item.id))
                    sectionPatch.instructions.push_back({item, std::nullopt});
            for (const auto& item : targetScript->instructions) {
                const auto* old = findById(sourceScript->instructions, item.id);
                if (old == nullptr) sectionPatch.instructions.push_back({std::nullopt, item});
                else if (!sameInstruction(*old, item))
                    sectionPatch.instructions.push_back({*old, item});
            }
        } else if (const auto* sourceText = std::get_if<spice::sct::SctStringSectionContent>(&source.content)) {
            const auto& targetText = std::get<spice::sct::SctStringSectionContent>(target->content);
            if (sourceText->preambleWords != targetText.preambleWords
                || sourceText->string.id != targetText.string.id
                || sourceText->string.kind != targetText.string.kind)
                return Result<SalsaScriptPatch>::failure(patchError(
                    "Indexed-string structure changed outside the supported patch contract."));
            if (!sameText(sourceText->string.value, targetText.string.value))
                patch.textValues.push_back({sourceText->string.id,
                    sourceText->string.value, targetText.string.value});
        }
        if (sectionPatch.nameBytes || sectionPatch.instructionOrder
            || !sectionPatch.instructions.empty())
            patch.scriptSections.push_back(std::move(sectionPatch));
    }
    for (const auto& section : workingDocument.sections)
        if (!findById(baselineDocument.sections, section.id))
            patch.sections.push_back({std::nullopt, section});

    std::vector<spice::sct::SctSupplementaryTextId> baselineSupplementaryTextOrder, workingSupplementaryTextOrder;
    for (const auto& entry : baselineDocument.supplementaryText) baselineSupplementaryTextOrder.push_back(entry.id);
    for (const auto& entry : workingDocument.supplementaryText) workingSupplementaryTextOrder.push_back(entry.id);
    if (baselineSupplementaryTextOrder != workingSupplementaryTextOrder)
        patch.supplementaryTextOrder = SctOrderDelta<spice::sct::SctSupplementaryTextId>{
            baselineSupplementaryTextOrder, workingSupplementaryTextOrder};
    for (const auto& entry : baselineDocument.supplementaryText) {
        const auto* target = findById(workingDocument.supplementaryText, entry.id);
        if (!target) patch.supplementaryText.push_back({entry, std::nullopt});
        else if (!sameSupplementaryText(entry, *target)) patch.supplementaryText.push_back({entry, *target});
    }
    for (const auto& entry : workingDocument.supplementaryText)
        if (!findById(baselineDocument.supplementaryText, entry.id))
            patch.supplementaryText.push_back({std::nullopt, entry});

    if (baselineDocument.opaqueAttachments.size() != workingDocument.opaqueAttachments.size()
        || !std::ranges::equal(baselineDocument.opaqueAttachments,
            workingDocument.opaqueAttachments,
            sameOpaqueAttachment))
        return Result<SalsaScriptPatch>::failure(patchError(
            "Opaque attachments changed outside the supported patch contract."));

    for (const auto& arm : baseline.authoredArms) {
        const auto* target = findById(working.authoredArms, arm.id);
        if (!target) patch.authoredArms.push_back({arm, std::nullopt});
        else if (!sameArm(arm, *target)) patch.authoredArms.push_back({arm, *target});
    }
    for (const auto& arm : working.authoredArms)
        if (!findById(baseline.authoredArms, arm.id))
            patch.authoredArms.push_back({std::nullopt, arm});
    for (const auto& repair : baseline.textRepairs) {
        const auto* target = findRepair(working.textRepairs, repair.target);
        if (!target) patch.textRepairs.push_back({repair.target, repair.provenance, std::nullopt});
        else if (!sameRepair(repair, *target))
            patch.textRepairs.push_back({repair.target, repair.provenance, target->provenance});
    }
    for (const auto& repair : working.textRepairs)
        if (!findRepair(baseline.textRepairs, repair.target))
            patch.textRepairs.push_back({repair.target, std::nullopt, repair.provenance});
    for (const auto& reference : baseline.unboundReferences) {
        const auto* target = findUnboundReference(
            working.unboundReferences, reference.site);
        if (!target)
            patch.unboundReferences.push_back(
                {reference.site, reference, std::nullopt});
        else if (*target != reference)
            patch.unboundReferences.push_back(
                {reference.site, reference, *target});
    }
    for (const auto& reference : working.unboundReferences)
        if (!findUnboundReference(baseline.unboundReferences, reference.site))
            patch.unboundReferences.push_back(
                {reference.site, std::nullopt, reference});
    for (const auto& alias : baseline.aliases) {
        const auto* target = findAlias(working.aliases, alias.variable);
        if (!target) patch.aliases.push_back({alias, std::nullopt});
        else if (*target != alias) patch.aliases.push_back({alias, *target});
    }
    for (const auto& alias : working.aliases)
        if (!findAlias(baseline.aliases, alias.variable))
            patch.aliases.push_back({std::nullopt, alias});
    for (const auto& annotation : baseline.annotations) {
        const auto* target = findAnnotation(working.annotations, annotation.target);
        if (!target) patch.annotations.push_back({annotation, std::nullopt});
        else if (*target != annotation) patch.annotations.push_back({annotation, *target});
    }
    for (const auto& annotation : working.annotations)
        if (!findAnnotation(baseline.annotations, annotation.target))
            patch.annotations.push_back({std::nullopt, annotation});
    for (const auto& folder : baseline.folders) {
        const auto* target = findFolder(working.folders, folder.id);
        if (!target) patch.folders.push_back({folder, std::nullopt});
        else if (*target != folder) patch.folders.push_back({folder, *target});
    }
    for (const auto& folder : working.folders)
        if (!findFolder(baseline.folders, folder.id))
            patch.folders.push_back({std::nullopt, folder});

    std::ranges::sort(patch.sections, {}, [](const auto& value) {
        return (value.before ? value.before->id : value.after->id).value();
    });
    std::ranges::sort(patch.scriptSections, {}, [](const auto& value) { return value.section.value(); });
    for (auto& section : patch.scriptSections)
        std::ranges::sort(section.instructions, {}, [](const auto& value) {
            return (value.before ? value.before->id : value.after->id).value();
        });
    std::ranges::sort(patch.textValues, {}, [](const auto& value) { return targetKey(value.target); });
    std::ranges::sort(patch.supplementaryText, {}, [](const auto& value) {
        return (value.before ? value.before->id : value.after->id).value();
    });
    std::ranges::sort(patch.authoredArms, {}, [](const auto& value) {
        return (value.before ? value.before->id : value.after->id).value;
    });
    std::ranges::sort(patch.textRepairs, {}, [](const auto& value) { return targetKey(value.target); });
    std::ranges::sort(patch.unboundReferences, {}, [](const auto& value) {
        return siteKey(value.site);
    });
    std::ranges::sort(patch.aliases, {}, [](const auto& value) {
        return (value.before ? value.before->variable : value.after->variable);
    });
    std::ranges::sort(patch.annotations, {}, [](const auto& value) {
        return (value.before ? value.before->target : value.after->target);
    });
    std::ranges::sort(patch.folders, {}, [](const auto& value) {
        return (value.before ? value.before->id : value.after->id).value;
    });
    return Result<SalsaScriptPatch>::success(std::move(patch));
}

Result<SctSemanticState> SalsaScriptPatchService::apply(
    const SctSemanticState& baseline, const SalsaScriptPatch& patch) {
    try {
        if (!baseline.document) throw std::runtime_error("the baseline document is missing");
        const auto& source = *baseline.document;
        if (patch.allocatorState && allocatorState(source) != *patch.allocatorState->before)
            throw std::runtime_error("allocator expected-before state does not match");
        if (patch.sectionOrder) {
            std::vector<spice::sct::SctSectionId> order;
            for (const auto& section : source.sections) order.push_back(section.id);
            if (order != patch.sectionOrder->before)
                throw std::runtime_error("section order expected-before state does not match");
        }
        for (const auto& change : patch.sections) {
            requireDelta(change, "section");
            const auto id = change.before ? change.before->id : change.after->id;
            const auto* current = findById(source.sections, id);
            if (change.before) {
                if (!current || !sameSection(*current, *change.before))
                    throw std::runtime_error("section expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted section already exists");
        }
        for (const auto& change : patch.scriptSections) {
            const auto found = std::ranges::find(source.sections, change.section,
                &spice::sct::SctDocumentSection::id);
            if (found == source.sections.end()) throw std::runtime_error("patched section is missing");
            const auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&found->content);
            if (!script) throw std::runtime_error("patched instruction section is not a script");
            if (change.nameBytes && found->nameBytes != *change.nameBytes->before)
                throw std::runtime_error("section name expected-before state does not match");
            if (change.instructionOrder) {
                std::vector<spice::sct::SctInstructionId> order;
                for (const auto& instruction : script->instructions) order.push_back(instruction.id);
                if (order != change.instructionOrder->before)
                    throw std::runtime_error("instruction order expected-before state does not match");
            }
            for (const auto& instruction : change.instructions) {
                const auto id = instruction.before ? instruction.before->id : instruction.after->id;
                const auto* current = findById(script->instructions, id);
                if (instruction.before) {
                    if (!current || !sameInstruction(*current, *instruction.before))
                        throw std::runtime_error("instruction expected-before state does not match");
                } else if (current) throw std::runtime_error("inserted instruction already exists");
            }
        }
        for (const auto& change : patch.textValues) {
            const auto* current = findText(source, change.target);
            if (!current || !sameText(*current, change.beforeValue))
                throw std::runtime_error("text expected-before state does not match");
        }
        if (patch.supplementaryTextOrder) {
            std::vector<spice::sct::SctSupplementaryTextId> order;
            for (const auto& entry : source.supplementaryText) order.push_back(entry.id);
            if (order != patch.supplementaryTextOrder->before)
                throw std::runtime_error(
                    "supplementary-text order expected-before state does not match");
        }
        for (const auto& change : patch.supplementaryText) {
            requireDelta(change, "supplementary text");
            const auto id = change.before ? change.before->id : change.after->id;
            const auto* current = findById(source.supplementaryText, id);
            if (change.before) {
                if (!current || !sameSupplementaryText(*current, *change.before))
                    throw std::runtime_error("supplementary text expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted supplementary text already exists");
        }
        for (const auto& change : patch.authoredArms) {
            requireDelta(change, "authored arm");
            const auto id = change.before ? change.before->id : change.after->id;
            const auto* current = findById(baseline.authoredArms, id);
            if (change.before) {
                if (!current || !sameArm(*current, *change.before))
                    throw std::runtime_error("authored arm expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted authored arm already exists");
        }
        for (const auto& change : patch.textRepairs) {
            const auto* current = findRepair(baseline.textRepairs, change.target);
            if (change.before) {
                if (!current || current->provenance != *change.before)
                    throw std::runtime_error("text repair expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted text repair already exists");
        }
        for (const auto& change : patch.unboundReferences) {
            const auto* current = findUnboundReference(
                baseline.unboundReferences, change.site);
            if (change.before) {
                if (!current || *current != *change.before)
                    throw std::runtime_error(
                        "unbound reference expected-before state does not match");
            } else if (current) {
                throw std::runtime_error("inserted unbound reference already exists");
            }
        }
        for (const auto& change : patch.aliases) {
            requireDelta(change, "variable alias");
            const auto key = change.before ? change.before->variable : change.after->variable;
            const auto* current = findAlias(baseline.aliases, key);
            if (change.before) {
                if (!current || *current != *change.before)
                    throw std::runtime_error("variable alias expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted variable alias already exists");
        }
        for (const auto& change : patch.annotations) {
            requireDelta(change, "entity annotation");
            const auto key = change.before ? change.before->target : change.after->target;
            const auto* current = findAnnotation(baseline.annotations, key);
            if (change.before) {
                if (!current || *current != *change.before)
                    throw std::runtime_error("annotation expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted annotation already exists");
        }
        for (const auto& change : patch.folders) {
            requireDelta(change, "section folder");
            const auto key = change.before ? change.before->id : change.after->id;
            const auto* current = findFolder(baseline.folders, key);
            if (change.before) {
                if (!current || *current != *change.before)
                    throw std::runtime_error("section folder expected-before state does not match");
            } else if (current) throw std::runtime_error("inserted section folder already exists");
        }

        auto document = std::make_shared<spice::sct::SctDocument>(source);
        for (const auto& change : patch.sections) {
            const auto id = change.before ? change.before->id : change.after->id;
            auto* existing = findMutableById(document->sections, id);
            if (change.before && change.after) *existing = *change.after;
            else if (change.before)
                std::erase_if(document->sections, [&](const auto& value) { return value.id == id; });
            else document->sections.push_back(*change.after);
        }
        for (const auto& change : patch.scriptSections) {
            auto* found = findMutableById(document->sections, change.section);
            if (!found) throw std::runtime_error("patched section was removed");
            if (change.nameBytes) found->nameBytes = *change.nameBytes->after;
            auto* script = std::get_if<spice::sct::SctScriptSectionContent>(&found->content);
            for (const auto& instruction : change.instructions) {
                const auto id = instruction.before ? instruction.before->id : instruction.after->id;
                auto* existing = findMutableById(script->instructions, id);
                if (instruction.before && instruction.after) *existing = *instruction.after;
                else if (instruction.before)
                    std::erase_if(script->instructions,
                        [&](const auto& value) { return value.id == id; });
                else script->instructions.push_back(*instruction.after);
            }
            if (change.instructionOrder
                && !reorder(script->instructions, change.instructionOrder->after))
                throw std::runtime_error("instruction order does not match patched entities");
        }
        for (const auto& change : patch.textValues) {
            const bool replaced = std::visit([&](const auto target) {
                using T = std::decay_t<decltype(target)>;
                if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
                    for (auto& section : document->sections)
                        if (auto* text = std::get_if<spice::sct::SctStringSectionContent>(&section.content);
                            text && text->string.id == target) {
                            text->string.value = change.afterValue;
                            return true;
                        }
                } else {
                    auto* entry = findMutableById(document->supplementaryText, target);
                    if (entry) { entry->value = change.afterValue; return true; }
                }
                return false;
            }, change.target);
            if (!replaced) throw std::runtime_error("patched text target is missing");
        }
        for (const auto& change : patch.supplementaryText) {
            const auto id = change.before ? change.before->id : change.after->id;
            auto* existing = findMutableById(document->supplementaryText, id);
            if (change.before && change.after) *existing = *change.after;
            else if (change.before)
                std::erase_if(document->supplementaryText,
                    [&](const auto& value) { return value.id == id; });
            else document->supplementaryText.push_back(*change.after);
        }
        if (patch.sectionOrder && !reorder(document->sections, patch.sectionOrder->after))
            throw std::runtime_error("section order does not match patched entities");
        if (patch.supplementaryTextOrder && !reorder(document->supplementaryText, patch.supplementaryTextOrder->after))
            throw std::runtime_error(
                "supplementary-text order does not match patched entities");
        advanceAllocators(*document);
        if (patch.allocatorState) advanceAllocatorsTo(*document, *patch.allocatorState->after);

        auto authoredArms = baseline.authoredArms;
        for (const auto& change : patch.authoredArms) {
            const auto id = change.before ? change.before->id : change.after->id;
            std::erase_if(authoredArms, [&](const auto& value) { return value.id == id; });
            if (change.after) authoredArms.push_back(*change.after);
        }
        std::ranges::sort(authoredArms, {}, [](const auto& value) { return value.id.value; });
        auto repairs = baseline.textRepairs;
        for (const auto& change : patch.textRepairs) {
            const auto key = targetKey(change.target);
            std::erase_if(repairs, [&](const auto& value) { return targetKey(value.target) == key; });
            if (change.after) repairs.push_back({change.target, *change.after});
        }
        std::ranges::sort(repairs, {}, [](const auto& value) { return targetKey(value.target); });
        auto unboundReferences = baseline.unboundReferences;
        for (const auto& change : patch.unboundReferences) {
            std::erase_if(unboundReferences, [&](const auto& value) {
                return value.site == change.site;
            });
            if (change.after) unboundReferences.push_back(*change.after);
        }
        std::ranges::sort(unboundReferences, {}, &SctUnboundReferenceOrigin::site);
        auto aliases = baseline.aliases;
        for (const auto& change : patch.aliases) {
            const auto key = change.before ? change.before->variable : change.after->variable;
            std::erase_if(aliases, [&](const auto& value) { return value.variable == key; });
            if (change.after) aliases.push_back(*change.after);
        }
        std::ranges::sort(aliases, {}, &SctVariableAlias::variable);
        auto annotations = baseline.annotations;
        for (const auto& change : patch.annotations) {
            const auto key = change.before ? change.before->target : change.after->target;
            std::erase_if(annotations, [&](const auto& value) { return value.target == key; });
            if (change.after) annotations.push_back(*change.after);
        }
        std::ranges::sort(annotations, {}, &SctEntityAnnotation::target);
        auto folders = baseline.folders;
        for (const auto& change : patch.folders) {
            const auto key = change.before ? change.before->id : change.after->id;
            std::erase_if(folders, [&](const auto& value) { return value.id == key; });
            if (change.after) folders.push_back(*change.after);
        }
        std::ranges::sort(folders, {}, [](const auto& value) { return value.id.value; });
        validateAuthoringState(*document, aliases, annotations, folders);
        verifyTextRepairs(*baseline.document, *document, repairs);
        return Result<SctSemanticState>::success(SctSemanticState{
            std::move(document), std::move(authoredArms), std::move(repairs),
            std::move(unboundReferences), std::move(aliases), std::move(annotations),
            std::move(folders)});
    } catch (const std::exception& error) {
        return Result<SctSemanticState>::failure(patchError(
            std::string("The SCT patch could not be applied: ") + error.what(),
            DiagnosticCode::SctPatchApplyFailed));
    }
}

SctPatchedLoadResult SctPatchCheckpointService::load(
    const GameProjectContext& project, const SctPatchStore* store,
    const SctBaselineStore* baselines, const AssetLocator& locator,
    const std::stop_token stopToken,
    spice::sct::SctParseTraceObserver traceObserver) {
    SctPatchedLoadResult result;
    result.load = SctDocumentLoader::load(
        project, locator, stopToken, std::move(traceObserver));
    result.baseline = result.load.document;
    if (!result.load.succeeded() || store == nullptr || stopToken.stop_requested())
        return result;

    const auto stored = store->load(locator);
    if (!stored) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.insert(
            result.load.infrastructureDiagnostics.end(), stored.diagnostics().begin(),
            stored.diagnostics().end());
        return result;
    }
    if (!stored.value().has_value()) return result;
    const auto& envelope = *stored.value();
    const auto sourceRevision = result.baseline->provenance->source().descriptor.revision;
    if (envelope.affectedAssets.size() != 1u
        || envelope.affectedAssets.front().locator != locator
        || envelope.affectedAssets.front().expectedRevision != sourceRevision) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.push_back(patchError(
            "The saved SCT patch targets a different source asset revision.",
            DiagnosticCode::SctPatchSourceMismatch));
        return result;
    }
    if (envelope.payload.type != SalsaScriptPatchCodec::PayloadType
        || (envelope.payload.schemaVersion != SalsaScriptPatchCodec::SchemaVersion
            && envelope.payload.schemaVersion
                != SalsaScriptPatchCodec::LegacySchemaVersion)) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.push_back(patchError(
            "The saved SCT patch payload type or schema is unsupported.",
            DiagnosticCode::UnsupportedSctPatchSchema));
        return result;
    }
    if (baselines == nullptr) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.push_back(patchError(
            "The saved SCT patch has no retained baseline store.",
            DiagnosticCode::SctBaselineMissing));
        return result;
    }
    auto retained = baselines->loadBaseline(sourceRevision);
    if (!retained) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.insert(
            result.load.infrastructureDiagnostics.end(), retained.diagnostics().begin(),
            retained.diagnostics().end());
        return result;
    }
    if (!retained.value()) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.push_back(patchError(
            "The exact source baseline retained for this SCT patch is missing.",
            DiagnosticCode::SctBaselineMissing));
        return result;
    }
    if (envelope.sourceDatasetFingerprint
        != result.baseline->provenance->inspection->sourceDatasetFingerprint) {
        result.load.infrastructureDiagnostics.push_back({DiagnosticSeverity::Warning,
            DiagnosticCode::SctPatchSourceMismatch,
            "The dataset has changed, but this asset still matches its saved patch expectation.",
            locator.path()});
    }
    auto decoded = SalsaScriptPatchCodec::deserialize(envelope.payload.bytes);
    if (!decoded) {
        result.patchConflict = true;
        auto diagnostics = decoded.diagnostics();
        attachPath(diagnostics, locator.path());
        result.load.infrastructureDiagnostics.insert(
            result.load.infrastructureDiagnostics.end(), diagnostics.begin(),
            diagnostics.end());
        return result;
    }
    auto patch = std::move(decoded).takeValue();
    if (patch.sourceTextConvention
        != result.baseline->provenance->textConvention) {
        result.load = SctDocumentLoader::materialize(
            result.load.inspection, patch.sourceTextConvention,
            SctTextSelectionOrigin::UserSelected, stopToken);
        result.baseline = result.load.document;
        if (!result.load.succeeded()) {
            result.patchConflict = true;
            return result;
        }
    }
    auto application = SalsaScriptPatchService::apply(SctSemanticState{
        result.baseline->document, {}, {}}, patch);
    if (!application) {
        result.patchConflict = true;
        auto diagnostics = application.diagnostics();
        attachPath(diagnostics, locator.path());
        result.load.infrastructureDiagnostics.insert(
            result.load.infrastructureDiagnostics.end(), diagnostics.begin(),
            diagnostics.end());
        return result;
    }
    auto applied = std::move(application).takeValue();
    SctMaterializationRequest request;
    request.baseRevision = RevisionId{1};
    request.targetRevision = RevisionId{1};
    request.baseDocument = applied.document;
    request.locator = locator;
    request.importEvidence = result.baseline->provenance->importEvidence;
    request.expectedStructuredArms = applied.authoredArms;
    auto materialized = SctDocumentMaterializer::materialize(request, stopToken);
    if (!materialized.succeeded()) {
        result.patchConflict = true;
        result.load.infrastructureDiagnostics.push_back(patchError(
            "The saved SCT patch did not produce a valid editable document.",
            DiagnosticCode::SctPatchVerificationFailed));
        return result;
    }
    auto snapshot = std::make_shared<SctDocumentSnapshot>();
    snapshot->provenance = result.baseline->provenance;
    snapshot->document = materialized.document;
    snapshot->analysis = materialized.analysis;
    snapshot->readiness = spice::sct::SctDocumentReadiness::StructurallyValid;
    snapshot->diagnostics = result.baseline->provenance->baselineDiagnostics;
    snapshot->diagnostics.insert(snapshot->diagnostics.end(),
        materialized.diagnostics.begin(), materialized.diagnostics.end());
    result.load.document = std::move(snapshot);
    result.authoredArms = std::move(applied.authoredArms);
    result.textRepairs = std::move(applied.textRepairs);
    result.unboundReferences = std::move(applied.unboundReferences);
    result.aliases = std::move(applied.aliases);
    result.annotations = std::move(applied.annotations);
    result.folders = std::move(applied.folders);
    result.patchApplied = true;
    return result;
}

SctCheckpointResult SctPatchCheckpointService::checkpoint(
    const SctCheckpointRequest& request, const SctPatchStore& store,
    const SctBaselineStore& baselines, const std::stop_token stopToken) {
    SctCheckpointResult result;
    result.revision = request.revision;
    result.historyStateToken = request.historyStateToken;
    if (!request.revision.valid() || !request.baseline
        || !request.baseline->document || stopToken.stop_requested()) {
        result.cancelled = stopToken.stop_requested();
        if (!result.cancelled) result.diagnostics.push_back(patchError(
            "The checkpoint request does not contain a valid source baseline."));
        return result;
    }
    auto materialized = SctDocumentMaterializer::materialize(
        request.materialization, stopToken);
    if (materialized.cancelled || stopToken.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    if (!materialized.succeeded()) {
        result.diagnostics.push_back(patchError(
            "The requested revision could not be verified for checkpointing.",
            DiagnosticCode::SctPatchVerificationFailed));
        return result;
    }
    const SctSemanticState baselineState{request.baseline->document, {}, {}, {}};
    const SctSemanticState workingState{materialized.document,
        request.materialization.expectedStructuredArms, request.textRepairs,
        request.unboundReferences, request.aliases, request.annotations, request.folders};
    auto patch = SalsaScriptPatchService::diff(baselineState, workingState,
        request.baseline->provenance->textConvention);
    if (!patch) {
        result.diagnostics = patch.diagnostics();
        return result;
    }
    auto payload = SalsaScriptPatchCodec::serialize(patch.value());
    if (!payload) {
        result.diagnostics = payload.diagnostics();
        return result;
    }
    auto decoded = SalsaScriptPatchCodec::deserialize(payload.value());
    if (!decoded) {
        result.diagnostics = decoded.diagnostics();
        return result;
    }
    auto reapplied = SalsaScriptPatchService::apply(baselineState, decoded.value());
    if (!reapplied) {
        result.diagnostics = reapplied.diagnostics();
        return result;
    }
    auto comparison = SalsaScriptPatchService::diff(baselineState,
        reapplied.value(), request.baseline->provenance->textConvention);
    if (!comparison) {
        result.diagnostics = comparison.diagnostics();
        return result;
    }
    auto comparisonBytes = SalsaScriptPatchCodec::serialize(comparison.value());
    if (!comparisonBytes || comparisonBytes.value() != payload.value()) {
        result.diagnostics.push_back(patchError(
            "The SCT patch failed deterministic reproduction verification.",
            DiagnosticCode::SctPatchVerificationFailed));
        return result;
    }
    const auto& source = request.baseline->provenance->source();
    PatchEnvelope envelope{
        request.baseline->provenance->inspection->sourceDatasetFingerprint,
        {{source.descriptor.locator, source.descriptor.revision}}, {},
        {std::string(SalsaScriptPatchCodec::PayloadType),
            SalsaScriptPatchCodec::SchemaVersion, std::move(payload).takeValue()}};
    auto retained = baselines.retainBaseline(source.descriptor.revision, source.bytes);
    if (!retained) {
        result.diagnostics = retained.diagnostics();
        return result;
    }
    auto saved = store.checkpoint(source.descriptor.locator, envelope);
    if (!saved) {
        result.diagnostics = saved.diagnostics();
        return result;
    }
    result.saved = true;
    return result;
}

}  // namespace salsa::core
