#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view Base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] Diagnostic envelopeError(
    const DiagnosticCode code,
    std::string message) {
    return { DiagnosticSeverity::Error, code, std::move(message), std::nullopt };
}

[[nodiscard]] bool hasExactKeys(
    const Json& object,
    const std::initializer_list<std::string_view> keys) {
    if (!object.is_object() || object.size() != keys.size()) {
        return false;
    }
    return std::ranges::all_of(keys, [&object](const std::string_view key) {
        return object.contains(std::string(key));
    });
}

[[nodiscard]] std::string locatorText(const AssetLocator& locator) {
    const auto utf8 = locator.path().generic_u8string();
    return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string_view text) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()),
        text.size()));
}

[[nodiscard]] std::optional<unsigned int> hexNibble(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned int>(value - 'A' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] Result<Sha256Digest> parseDigest(
    const std::string_view text,
    const std::string_view fieldName) {
    if (text.size() != Sha256Digest::Size * 2) {
        return Result<Sha256Digest>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            std::string(fieldName) + " must be a 64-character SHA-256 value."));
    }

    std::array<std::byte, Sha256Digest::Size> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = hexNibble(text[index * 2]);
        const auto low = hexNibble(text[index * 2 + 1]);
        if (!high.has_value() || !low.has_value()) {
            return Result<Sha256Digest>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                std::string(fieldName) + " contains a non-hexadecimal character."));
        }
        bytes[index] = static_cast<std::byte>((*high << 4) | *low);
    }
    return Result<Sha256Digest>::success(Sha256Digest(bytes));
}

[[nodiscard]] std::string encodeBase64(const std::span<const std::byte> bytes) {
    std::string encoded{};
    encoded.reserve(((bytes.size() + 2) / 3) * 4);

    std::size_t offset = 0;
    while (offset + 3 <= bytes.size()) {
        const auto first = std::to_integer<unsigned int>(bytes[offset]);
        const auto second = std::to_integer<unsigned int>(bytes[offset + 1]);
        const auto third = std::to_integer<unsigned int>(bytes[offset + 2]);
        encoded.push_back(Base64Alphabet[(first >> 2) & 0x3f]);
        encoded.push_back(Base64Alphabet[((first & 0x03) << 4) | (second >> 4)]);
        encoded.push_back(Base64Alphabet[((second & 0x0f) << 2) | (third >> 6)]);
        encoded.push_back(Base64Alphabet[third & 0x3f]);
        offset += 3;
    }

    const auto remaining = bytes.size() - offset;
    if (remaining == 1) {
        const auto first = std::to_integer<unsigned int>(bytes[offset]);
        encoded.push_back(Base64Alphabet[(first >> 2) & 0x3f]);
        encoded.push_back(Base64Alphabet[(first & 0x03) << 4]);
        encoded += "==";
    } else if (remaining == 2) {
        const auto first = std::to_integer<unsigned int>(bytes[offset]);
        const auto second = std::to_integer<unsigned int>(bytes[offset + 1]);
        encoded.push_back(Base64Alphabet[(first >> 2) & 0x3f]);
        encoded.push_back(Base64Alphabet[((first & 0x03) << 4) | (second >> 4)]);
        encoded.push_back(Base64Alphabet[(second & 0x0f) << 2]);
        encoded.push_back('=');
    }
    return encoded;
}

[[nodiscard]] std::optional<unsigned int> base64Value(const char value) noexcept {
    const auto position = Base64Alphabet.find(value);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(position);
}

[[nodiscard]] Result<std::vector<std::byte>> decodeBase64(const std::string_view text) {
    if (text.empty()) {
        return Result<std::vector<std::byte>>::success({});
    }
    if (text.size() % 4 != 0 || text.size() / 4 > std::numeric_limits<std::size_t>::max() / 3) {
        return Result<std::vector<std::byte>>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "The patch payload is not canonical padded base64."));
    }

    std::vector<std::byte> decoded{};
    decoded.reserve((text.size() / 4) * 3);
    for (std::size_t offset = 0; offset < text.size(); offset += 4) {
        const bool last = offset + 4 == text.size();
        const auto first = base64Value(text[offset]);
        const auto second = base64Value(text[offset + 1]);
        if (!first.has_value() || !second.has_value()) {
            return Result<std::vector<std::byte>>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch payload contains invalid base64 characters."));
        }

        const char thirdCharacter = text[offset + 2];
        const char fourthCharacter = text[offset + 3];
        if (thirdCharacter == '=') {
            if (!last || fourthCharacter != '=' || (*second & 0x0f) != 0) {
                return Result<std::vector<std::byte>>::failure(envelopeError(
                    DiagnosticCode::InvalidPatchEnvelope,
                    "The patch payload is not canonical padded base64."));
            }
            decoded.push_back(static_cast<std::byte>((*first << 2) | (*second >> 4)));
            continue;
        }

        const auto third = base64Value(thirdCharacter);
        if (!third.has_value()) {
            return Result<std::vector<std::byte>>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch payload contains invalid base64 characters."));
        }
        decoded.push_back(static_cast<std::byte>((*first << 2) | (*second >> 4)));
        decoded.push_back(static_cast<std::byte>((*second << 4) | (*third >> 2)));

        if (fourthCharacter == '=') {
            if (!last || (*third & 0x03) != 0) {
                return Result<std::vector<std::byte>>::failure(envelopeError(
                    DiagnosticCode::InvalidPatchEnvelope,
                    "The patch payload is not canonical padded base64."));
            }
            continue;
        }

        const auto fourth = base64Value(fourthCharacter);
        if (!fourth.has_value()) {
            return Result<std::vector<std::byte>>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch payload contains invalid base64 characters."));
        }
        decoded.push_back(static_cast<std::byte>((*third << 6) | *fourth));
    }
    return Result<std::vector<std::byte>>::success(std::move(decoded));
}

[[nodiscard]] Result<PatchEnvelope> canonicalize(PatchEnvelope envelope) {
    if (envelope.affectedAssets.empty()) {
        return Result<PatchEnvelope>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "A patch envelope must affect at least one asset."));
    }
    if (envelope.payload.type.empty()) {
        return Result<PatchEnvelope>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "The opaque patch payload type cannot be empty."));
    }
    if (envelope.payload.schemaVersion == 0) {
        return Result<PatchEnvelope>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "The opaque patch payload schema version must be greater than zero."));
    }

    std::ranges::sort(envelope.affectedAssets, {}, [](const PatchAssetExpectation& asset)
        -> const std::string& {
        return asset.locator.identityKey();
    });
    for (std::size_t index = 1; index < envelope.affectedAssets.size(); ++index) {
        if (envelope.affectedAssets[index - 1].locator == envelope.affectedAssets[index].locator) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "Affected asset locators must be unique without regard to case."));
        }
    }

    std::ranges::sort(envelope.dependencies, {}, [](const AssetLocator& locator)
        -> const std::string& {
        return locator.identityKey();
    });
    for (std::size_t index = 1; index < envelope.dependencies.size(); ++index) {
        if (envelope.dependencies[index - 1] == envelope.dependencies[index]) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "Dependency locators must be unique without regard to case."));
        }
    }

    for (const auto& affected : envelope.affectedAssets) {
        const auto dependency = std::ranges::lower_bound(
            envelope.dependencies,
            affected.locator.identityKey(),
            {},
            [](const AssetLocator& locator) -> const std::string& {
                return locator.identityKey();
            });
        if (dependency != envelope.dependencies.end() && *dependency == affected.locator) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "An affected asset cannot also be listed as a dependency."));
        }
    }
    return Result<PatchEnvelope>::success(std::move(envelope));
}

[[nodiscard]] Result<std::uint32_t> parsePositiveVersion(
    const Json& value,
    const std::string_view fieldName) {
    if (!value.is_number_unsigned()) {
        return Result<std::uint32_t>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            std::string(fieldName) + " must be an unsigned integer."));
    }
    const auto parsed = value.get<std::uint64_t>();
    if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::uint32_t>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            std::string(fieldName) + " must fit in a nonzero 32-bit unsigned integer."));
    }
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed));
}

}  // namespace

Result<std::string> PatchEnvelopeCodec::serialize(const PatchEnvelope& envelope) {
    auto canonicalResult = canonicalize(envelope);
    if (!canonicalResult) {
        return Result<std::string>::failure(canonicalResult.diagnostics());
    }
    const auto& canonical = canonicalResult.value();

    const auto payloadDigest = sha256(canonical.payload.bytes);
    if (!payloadDigest) {
        return Result<std::string>::failure(payloadDigest.diagnostics());
    }

    try {
        Json header = Json::object();
        header["format"] = std::string(FormatId);
        header["owner"] = std::string(Owner);
        header["schemaVersion"] = SchemaVersion;

        Json affectedAssets = Json::array();
        for (const auto& asset : canonical.affectedAssets) {
            Json item = Json::object();
            item["locator"] = locatorText(asset.locator);
            item["expectedSourceRevision"] = asset.expectedRevision.digest.toHex();
            affectedAssets.push_back(std::move(item));
        }

        Json dependencies = Json::array();
        for (const auto& locator : canonical.dependencies) {
            dependencies.push_back(locatorText(locator));
        }

        Json payload = Json::object();
        payload["type"] = canonical.payload.type;
        payload["schemaVersion"] = canonical.payload.schemaVersion;
        payload["encoding"] = "base64";
        payload["sha256"] = payloadDigest.value().toHex();
        payload["data"] = encodeBase64(canonical.payload.bytes);

        Json document = Json::object();
        document["header"] = std::move(header);
        document["sourceDatasetFingerprint"] = canonical.sourceDatasetFingerprint.digest.toHex();
        document["affectedAssets"] = std::move(affectedAssets);
        document["dependencies"] = std::move(dependencies);
        document["payload"] = std::move(payload);

        auto serialized = document.dump(2, ' ', false, Json::error_handler_t::strict);
        serialized.push_back('\n');
        return Result<std::string>::success(std::move(serialized));
    } catch (const nlohmann::json::exception& exception) {
        return Result<std::string>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "The patch envelope could not be represented as UTF-8 JSON: " +
                std::string(exception.what())));
    }
}

Result<PatchEnvelope> PatchEnvelopeCodec::deserialize(const std::string_view json) {
    try {
        const Json document = Json::parse(json.begin(), json.end());
        if (!hasExactKeys(document, {
                "header",
                "sourceDatasetFingerprint",
                "affectedAssets",
                "dependencies",
                "payload",
            })) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch envelope has missing or unknown top-level fields."));
        }

        const auto& header = document.at("header");
        if (!hasExactKeys(header, { "format", "owner", "schemaVersion" }) ||
            !header.at("format").is_string() ||
            !header.at("owner").is_string()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch envelope header is invalid."));
        }
        if (header.at("format").get_ref<const std::string&>() != FormatId ||
            header.at("owner").get_ref<const std::string&>() != Owner) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch envelope format or owner is not recognized."));
        }
        if (!header.at("schemaVersion").is_number_unsigned()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The patch envelope schema version must be an unsigned integer."));
        }
        const auto envelopeVersion = header.at("schemaVersion").get<std::uint64_t>();
        if (envelopeVersion != SchemaVersion) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::UnsupportedPersistenceSchemaVersion,
                "The patch envelope schema version is not supported."));
        }

        if (!document.at("sourceDatasetFingerprint").is_string()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The source dataset fingerprint must be a string."));
        }
        auto datasetDigest = parseDigest(
            document.at("sourceDatasetFingerprint").get_ref<const std::string&>(),
            "sourceDatasetFingerprint");
        if (!datasetDigest) {
            return Result<PatchEnvelope>::failure(datasetDigest.diagnostics());
        }

        const auto& affectedJson = document.at("affectedAssets");
        if (!affectedJson.is_array() || affectedJson.empty()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "affectedAssets must be a nonempty array."));
        }
        std::vector<PatchAssetExpectation> affectedAssets{};
        affectedAssets.reserve(affectedJson.size());
        for (const auto& item : affectedJson) {
            if (!hasExactKeys(item, { "locator", "expectedSourceRevision" }) ||
                !item.at("locator").is_string() ||
                !item.at("expectedSourceRevision").is_string()) {
                return Result<PatchEnvelope>::failure(envelopeError(
                    DiagnosticCode::InvalidPatchEnvelope,
                    "An affected asset entry is invalid."));
            }

            const auto& locatorString = item.at("locator").get_ref<const std::string&>();
            auto locator = AssetLocator::fromRelativePath(pathFromUtf8(locatorString));
            if (!locator) {
                return Result<PatchEnvelope>::failure(envelopeError(
                    DiagnosticCode::InvalidPatchEnvelope,
                    "An affected asset locator is invalid."));
            }
            auto revisionDigest = parseDigest(
                item.at("expectedSourceRevision").get_ref<const std::string&>(),
                "expectedSourceRevision");
            if (!revisionDigest) {
                return Result<PatchEnvelope>::failure(revisionDigest.diagnostics());
            }
            affectedAssets.push_back({
                std::move(locator).takeValue(),
                SourceRevision{ std::move(revisionDigest).takeValue() },
            });
        }

        const auto& dependenciesJson = document.at("dependencies");
        if (!dependenciesJson.is_array()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "dependencies must be an array."));
        }
        std::vector<AssetLocator> dependencies{};
        dependencies.reserve(dependenciesJson.size());
        for (const auto& item : dependenciesJson) {
            if (!item.is_string()) {
                return Result<PatchEnvelope>::failure(envelopeError(
                    DiagnosticCode::InvalidPatchEnvelope,
                    "Every dependency locator must be a string."));
            }
            auto locator = AssetLocator::fromRelativePath(
                pathFromUtf8(item.get_ref<const std::string&>()));
            if (!locator) {
                return Result<PatchEnvelope>::failure(envelopeError(
                    DiagnosticCode::InvalidPatchEnvelope,
                    "A dependency locator is invalid."));
            }
            dependencies.push_back(std::move(locator).takeValue());
        }

        const auto& payloadJson = document.at("payload");
        if (!hasExactKeys(payloadJson, {
                "type",
                "schemaVersion",
                "encoding",
                "sha256",
                "data",
            }) ||
            !payloadJson.at("type").is_string() ||
            !payloadJson.at("encoding").is_string() ||
            !payloadJson.at("sha256").is_string() ||
            !payloadJson.at("data").is_string()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The opaque patch payload descriptor is invalid."));
        }
        const auto& payloadType = payloadJson.at("type").get_ref<const std::string&>();
        if (payloadType.empty() || payloadJson.at("encoding").get_ref<const std::string&>() != "base64") {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::InvalidPatchEnvelope,
                "The opaque patch payload type or encoding is invalid."));
        }
        auto payloadVersion = parsePositiveVersion(
            payloadJson.at("schemaVersion"),
            "payload.schemaVersion");
        if (!payloadVersion) {
            return Result<PatchEnvelope>::failure(payloadVersion.diagnostics());
        }
        auto declaredPayloadDigest = parseDigest(
            payloadJson.at("sha256").get_ref<const std::string&>(),
            "payload.sha256");
        if (!declaredPayloadDigest) {
            return Result<PatchEnvelope>::failure(declaredPayloadDigest.diagnostics());
        }
        auto payloadBytes = decodeBase64(payloadJson.at("data").get_ref<const std::string&>());
        if (!payloadBytes) {
            return Result<PatchEnvelope>::failure(payloadBytes.diagnostics());
        }
        const auto actualPayloadDigest = sha256(payloadBytes.value());
        if (!actualPayloadDigest) {
            return Result<PatchEnvelope>::failure(actualPayloadDigest.diagnostics());
        }
        if (actualPayloadDigest.value() != declaredPayloadDigest.value()) {
            return Result<PatchEnvelope>::failure(envelopeError(
                DiagnosticCode::PatchPayloadCorrupt,
                "The opaque patch payload does not match its SHA-256 digest."));
        }

        PatchEnvelope envelope{
            DatasetFingerprint{ std::move(datasetDigest).takeValue() },
            std::move(affectedAssets),
            std::move(dependencies),
            OpaquePatchPayload{
                payloadType,
                std::move(payloadVersion).takeValue(),
                std::move(payloadBytes).takeValue(),
            },
        };
        return canonicalize(std::move(envelope));
    } catch (const nlohmann::json::parse_error& exception) {
        return Result<PatchEnvelope>::failure(envelopeError(
            DiagnosticCode::MalformedPersistenceJson,
            "The patch envelope is not valid JSON: " + std::string(exception.what())));
    } catch (const nlohmann::json::exception& exception) {
        return Result<PatchEnvelope>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "The patch envelope JSON is invalid: " + std::string(exception.what())));
    } catch (const std::exception& exception) {
        return Result<PatchEnvelope>::failure(envelopeError(
            DiagnosticCode::InvalidPatchEnvelope,
            "The patch envelope could not be decoded: " + std::string(exception.what())));
    }
}

}  // namespace salsa::core
