#include "SalsaCore/Persistence/SctReconciliationDecisionStore.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] Diagnostic decisionError(std::string message,
    std::optional<std::filesystem::path> path = std::nullopt) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctReconciliation,
        std::move(message), std::move(path)};
}

[[nodiscard]] bool exactKeys(const Json& value,
    const std::initializer_list<std::string_view> keys) {
    return value.is_object() && value.size() == keys.size()
        && std::ranges::all_of(keys, [&](const auto key) {
            return value.contains(std::string(key));
        });
}

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

[[nodiscard]] std::optional<Sha256Digest> parseDigest(
    const std::string_view value) {
    if (value.size() != Sha256Digest::Size * 2u) return std::nullopt;
    std::array<std::byte, Sha256Digest::Size> bytes{};
    const auto nibble = [](const char item) -> std::optional<unsigned int> {
        if (item >= '0' && item <= '9') return item - '0';
        if (item >= 'a' && item <= 'f') return item - 'a' + 10u;
        if (item >= 'A' && item <= 'F') return item - 'A' + 10u;
        return std::nullopt;
    };
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = nibble(value[index * 2u]);
        const auto low = nibble(value[index * 2u + 1u]);
        if (!high || !low) return std::nullopt;
        bytes[index] = static_cast<std::byte>((*high << 4u) | *low);
    }
    return Sha256Digest(bytes);
}

[[nodiscard]] std::string kindName(const SctReconciliationDecisionKind kind) {
    switch (kind) {
    case SctReconciliationDecisionKind::PairAsset: return "pair-asset";
    case SctReconciliationDecisionKind::IncomingAssetAddition: return "add-asset";
    case SctReconciliationDecisionKind::BaselineAssetRemoval: return "remove-asset";
    case SctReconciliationDecisionKind::PairEntity: return "pair-entity";
    case SctReconciliationDecisionKind::IncomingEntityAddition: return "add-entity";
    case SctReconciliationDecisionKind::BaselineEntityRemoval: return "remove-entity";
    }
    return {};
}

[[nodiscard]] std::optional<SctReconciliationDecisionKind> parseKind(
    const std::string_view value) {
    if (value == "pair-asset") return SctReconciliationDecisionKind::PairAsset;
    if (value == "add-asset") return SctReconciliationDecisionKind::IncomingAssetAddition;
    if (value == "remove-asset") return SctReconciliationDecisionKind::BaselineAssetRemoval;
    if (value == "pair-entity") return SctReconciliationDecisionKind::PairEntity;
    if (value == "add-entity") return SctReconciliationDecisionKind::IncomingEntityAddition;
    if (value == "remove-entity") return SctReconciliationDecisionKind::BaselineEntityRemoval;
    return std::nullopt;
}

[[nodiscard]] Json encodeEntity(const SctReconciliationEntityId& entity) {
    return std::visit([](const auto id) -> Json {
        using T = std::decay_t<decltype(id)>;
        std::string kind;
        std::uint64_t value = 0;
        if constexpr (std::is_same_v<T, spice::sct::SctSectionId>) {
            kind = "section"; value = id.value();
        } else if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>) {
            kind = "instruction"; value = id.value();
        } else if constexpr (std::is_same_v<T, spice::sct::SctStringId>) {
            kind = "string"; value = id.value();
        } else if constexpr (std::is_same_v<T, spice::sct::SctSupplementaryTextId>) {
            kind = "supplementary-text"; value = id.value();
        } else if constexpr (std::is_same_v<T, spice::sct::SctOpaqueAttachmentId>) {
            kind = "opaque"; value = id.value();
        } else {
            kind = "authored-arm"; value = id.value;
        }
        return Json{{"kind", kind}, {"id", value}};
    }, entity);
}

[[nodiscard]] SctReconciliationEntityId parseEntity(const Json& value) {
    if (!exactKeys(value, {"kind", "id"}) || !value.at("kind").is_string()
        || !value.at("id").is_number_unsigned()
        || value.at("id").get<std::uint64_t>() == 0)
        throw std::runtime_error("a reconciliation entity reference is malformed");
    const auto kind = value.at("kind").get<std::string>();
    const auto id = value.at("id").get<std::uint64_t>();
    if (kind == "section") return spice::sct::SctSectionId{id};
    if (kind == "instruction") return spice::sct::SctInstructionId{id};
    if (kind == "string") return spice::sct::SctStringId{id};
    if (kind == "supplementary-text" || kind == "footer")
        return spice::sct::SctSupplementaryTextId{id};
    if (kind == "opaque") return spice::sct::SctOpaqueAttachmentId{id};
    if (kind == "authored-arm") return SctAuthoredArmId{id};
    throw std::runtime_error("a reconciliation entity kind is unsupported");
}

[[nodiscard]] Json optionalLocator(const std::optional<AssetLocator>& locator) {
    return locator ? Json(pathUtf8(locator->path())) : Json(nullptr);
}

[[nodiscard]] std::optional<AssetLocator> parseLocator(const Json& value) {
    if (value.is_null()) return std::nullopt;
    if (!value.is_string()) throw std::runtime_error("an asset locator is malformed");
    auto locator = AssetLocator::fromRelativePath(pathFromUtf8(value.get<std::string>()));
    if (!locator) throw std::runtime_error("an asset locator is invalid");
    return std::move(locator).takeValue();
}

[[nodiscard]] Json encodeDecision(const SctReconciliationDecision& decision) {
    return Json{{"id", decision.id}, {"kind", kindName(decision.kind)},
        {"baselineAsset", optionalLocator(decision.baselineAsset)},
        {"incomingAssetKey", decision.incomingAssetKey
            ? Json(*decision.incomingAssetKey) : Json(nullptr)},
        {"baselineEntity", decision.baselineEntity
            ? encodeEntity(*decision.baselineEntity) : Json(nullptr)},
        {"incomingEntity", decision.incomingEntity
            ? encodeEntity(*decision.incomingEntity) : Json(nullptr)}};
}

[[nodiscard]] SctReconciliationDecision parseDecision(const Json& value) {
    if (!exactKeys(value, {"id", "kind", "baselineAsset", "incomingAssetKey",
            "baselineEntity", "incomingEntity"})
        || !value.at("id").is_string() || value.at("id").get<std::string>().empty()
        || !value.at("kind").is_string())
        throw std::runtime_error("a reconciliation decision is malformed");
    const auto kind = parseKind(value.at("kind").get<std::string>());
    if (!kind) throw std::runtime_error("a reconciliation decision kind is unsupported");
    SctReconciliationDecision result;
    result.id = value.at("id").get<std::string>();
    result.kind = *kind;
    result.baselineAsset = parseLocator(value.at("baselineAsset"));
    if (!value.at("incomingAssetKey").is_null()) {
        if (!value.at("incomingAssetKey").is_string())
            throw std::runtime_error("an incoming asset key is malformed");
        result.incomingAssetKey = value.at("incomingAssetKey").get<std::string>();
    }
    if (!value.at("baselineEntity").is_null())
        result.baselineEntity = parseEntity(value.at("baselineEntity"));
    if (!value.at("incomingEntity").is_null())
        result.incomingEntity = parseEntity(value.at("incomingEntity"));
    return result;
}

void validateDecision(const SctReconciliationDecision& decision) {
    if (decision.id.empty()) throw std::runtime_error("a reconciliation decision ID is empty");
    const bool baseline = decision.baselineAsset.has_value();
    const bool incoming = decision.incomingAssetKey.has_value()
        && !decision.incomingAssetKey->empty();
    const bool hasBaselineEntity = decision.baselineEntity.has_value();
    const bool hasIncomingEntity = decision.incomingEntity.has_value();
    switch (decision.kind) {
    case SctReconciliationDecisionKind::PairAsset:
        if (!baseline || !incoming || hasBaselineEntity || hasIncomingEntity) break;
        return;
    case SctReconciliationDecisionKind::IncomingAssetAddition:
        if (!baseline && incoming && !hasBaselineEntity && !hasIncomingEntity) return;
        break;
    case SctReconciliationDecisionKind::BaselineAssetRemoval:
        if (baseline && !incoming && !hasBaselineEntity && !hasIncomingEntity) return;
        break;
    case SctReconciliationDecisionKind::PairEntity:
        if (baseline && incoming && hasBaselineEntity && hasIncomingEntity
            && decision.baselineEntity->index() == decision.incomingEntity->index()) return;
        break;
    case SctReconciliationDecisionKind::IncomingEntityAddition:
        if (baseline && incoming && !hasBaselineEntity && hasIncomingEntity) return;
        break;
    case SctReconciliationDecisionKind::BaselineEntityRemoval:
        if (baseline && incoming && hasBaselineEntity && !hasIncomingEntity) return;
        break;
    }
    throw std::runtime_error("a reconciliation decision has inconsistent targets");
}

[[nodiscard]] Result<std::string> readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<std::string>::failure(decisionError(
        "The reconciliation decision artifact could not be opened.", path));
    return Result<std::string>::success(std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
}

}  // namespace

Result<std::string> SctReconciliationDecisionCodec::serialize(
    const SctReconciliationDecisionArtifact& source) {
    try {
        if (source.decisionScopeId.empty() || source.targetScopeKey.empty()
            || source.reconciliationContractVersion != SctDocumentReconciler::ContractVersion)
            throw std::runtime_error("the reconciliation decision header is invalid");
        auto artifact = source;
        std::ranges::sort(artifact.assets, {}, [](const auto& value) {
            return (value.baselineAsset ? value.baselineAsset->identityKey() : std::string())
                + "|" + value.incomingAssetKey.value_or("");
        });
        Json root{{"formatId", std::string(FormatId)}, {"schemaVersion", SchemaVersion},
            {"decisionScopeId", artifact.decisionScopeId},
            {"targetScopeKey", artifact.targetScopeKey},
            {"reconciliationContractVersion", artifact.reconciliationContractVersion},
            {"assets", Json::array()}};
        std::set<std::string> bindings;
        std::set<std::string> decisionIds;
        for (auto& asset : artifact.assets) {
            if (!asset.baselineAsset && !asset.incomingAssetKey)
                throw std::runtime_error("a decision binding has no asset");
            if (asset.baselineAsset.has_value() != asset.baselineRevision.has_value()
                || asset.incomingAssetKey.has_value()
                    != asset.incomingFingerprint.has_value()
                || (asset.incomingAssetKey && asset.incomingAssetKey->empty())
                || (asset.incomingFingerprint && asset.incomingFingerprint->empty()))
                throw std::runtime_error(
                    "a decision binding lacks exact invalidation evidence");
            const auto key = (asset.baselineAsset
                    ? asset.baselineAsset->identityKey() : std::string())
                + "|" + asset.incomingAssetKey.value_or("");
            if (!bindings.insert(key).second)
                throw std::runtime_error("a decision binding is duplicated");
            std::ranges::sort(asset.decisions, {}, &SctReconciliationDecision::id);
            Json decisions = Json::array();
            for (const auto& decision : asset.decisions) {
                validateDecision(decision);
                if (decision.baselineAsset != asset.baselineAsset
                    || decision.incomingAssetKey != asset.incomingAssetKey)
                    throw std::runtime_error(
                        "a decision does not belong to its asset binding");
                if (!decisionIds.insert(decision.id).second)
                    throw std::runtime_error("a reconciliation decision ID is duplicated");
                decisions.push_back(encodeDecision(decision));
            }
            root["assets"].push_back(Json{
                {"baselineAsset", optionalLocator(asset.baselineAsset)},
                {"baselineRevision", asset.baselineRevision
                    ? Json(asset.baselineRevision->digest.toHex()) : Json(nullptr)},
                {"incomingAssetKey", asset.incomingAssetKey
                    ? Json(*asset.incomingAssetKey) : Json(nullptr)},
                {"incomingFingerprint", asset.incomingFingerprint
                    ? Json(*asset.incomingFingerprint) : Json(nullptr)},
                {"decisions", std::move(decisions)}});
        }
        return Result<std::string>::success(root.dump(2) + '\n');
    } catch (const std::exception& error) {
        return Result<std::string>::failure(decisionError(
            std::string("The reconciliation decision artifact could not be serialized: ")
                + error.what()));
    }
}

Result<SctReconciliationDecisionArtifact> SctReconciliationDecisionCodec::deserialize(
    const std::string_view text) {
    try {
        const auto root = Json::parse(text);
        if (!exactKeys(root, {"formatId", "schemaVersion", "decisionScopeId",
                "targetScopeKey", "reconciliationContractVersion", "assets"})
            || !root.at("formatId").is_string()
            || root.at("formatId").get<std::string>() != FormatId
            || !root.at("schemaVersion").is_number_unsigned()
            || (root.at("schemaVersion").get<std::uint32_t>() != SchemaVersion
                && root.at("schemaVersion").get<std::uint32_t>() != LegacySchemaVersion)
            || !root.at("decisionScopeId").is_string()
            || !root.at("targetScopeKey").is_string()
            || !root.at("reconciliationContractVersion").is_number_unsigned()
            || !root.at("assets").is_array())
            throw std::runtime_error("the reconciliation decision header is invalid");
        SctReconciliationDecisionArtifact artifact;
        artifact.decisionScopeId = root.at("decisionScopeId").get<std::string>();
        artifact.targetScopeKey = root.at("targetScopeKey").get<std::string>();
        artifact.reconciliationContractVersion =
            root.at("reconciliationContractVersion").get<std::uint32_t>();
        if (artifact.decisionScopeId.empty() || artifact.targetScopeKey.empty()
            || artifact.reconciliationContractVersion
                != SctDocumentReconciler::ContractVersion)
            throw std::runtime_error("the reconciliation decision context is unsupported");
        for (const auto& value : root.at("assets")) {
            if (!exactKeys(value, {"baselineAsset", "baselineRevision",
                    "incomingAssetKey", "incomingFingerprint", "decisions"})
                || !value.at("decisions").is_array())
                throw std::runtime_error("a reconciliation decision binding is malformed");
            SctReconciliationDecisionBinding binding;
            binding.baselineAsset = parseLocator(value.at("baselineAsset"));
            if (!value.at("baselineRevision").is_null()) {
                if (!value.at("baselineRevision").is_string())
                    throw std::runtime_error("a baseline revision is malformed");
                auto digest = parseDigest(
                    value.at("baselineRevision").get<std::string>());
                if (!digest) throw std::runtime_error("a baseline revision is invalid");
                binding.baselineRevision = SourceRevision{*digest};
            }
            if (!value.at("incomingAssetKey").is_null()) {
                if (!value.at("incomingAssetKey").is_string())
                    throw std::runtime_error("an incoming asset key is malformed");
                binding.incomingAssetKey =
                    value.at("incomingAssetKey").get<std::string>();
            }
            if (!value.at("incomingFingerprint").is_null()) {
                if (!value.at("incomingFingerprint").is_string())
                    throw std::runtime_error("an incoming fingerprint is malformed");
                binding.incomingFingerprint =
                    value.at("incomingFingerprint").get<std::string>();
            }
            for (const auto& decision : value.at("decisions")) {
                auto parsed = parseDecision(decision);
                validateDecision(parsed);
                binding.decisions.push_back(std::move(parsed));
            }
            artifact.assets.push_back(std::move(binding));
        }
        auto canonical = serialize(artifact);
        if (!canonical) return Result<SctReconciliationDecisionArtifact>::failure(
            canonical.diagnostics());
        return Result<SctReconciliationDecisionArtifact>::success(std::move(artifact));
    } catch (const std::exception& error) {
        return Result<SctReconciliationDecisionArtifact>::failure(decisionError(
            std::string("The reconciliation decision artifact is malformed: ")
                + error.what()));
    }
}

DirectorySctReconciliationDecisionStore::DirectorySctReconciliationDecisionStore(
    std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path DirectorySctReconciliationDecisionStore::path(
    const std::string_view decisionScopeId,
    const std::string_view targetScopeKey) const {
    const std::string identity = std::string(decisionScopeId) + '|'
        + std::string(targetScopeKey);
    const auto bytes = std::as_bytes(std::span{identity.data(), identity.size()});
    const auto digest = sha256(bytes);
    return root_ / ((digest ? digest.value().toHex() : std::string("invalid"))
        + ".salsa-reconciliation.json");
}

Result<std::optional<SctReconciliationDecisionArtifact>>
DirectorySctReconciliationDecisionStore::loadReconciliationDecisions(
    const std::string_view decisionScopeId,
    const std::string_view targetScopeKey) const {
    const auto sourcePath = path(decisionScopeId, targetScopeKey);
    std::error_code error;
    if (!std::filesystem::exists(sourcePath, error)) {
        if (error) return Result<std::optional<SctReconciliationDecisionArtifact>>::failure(
            decisionError("The reconciliation decision path could not be inspected.",
                sourcePath));
        return Result<std::optional<SctReconciliationDecisionArtifact>>::success(
            std::nullopt);
    }
    auto text = readText(sourcePath);
    if (!text) return Result<std::optional<SctReconciliationDecisionArtifact>>::failure(
        text.diagnostics());
    auto decoded = SctReconciliationDecisionCodec::deserialize(text.value());
    if (!decoded) return Result<std::optional<SctReconciliationDecisionArtifact>>::failure(
        decoded.diagnostics());
    if (decoded.value().decisionScopeId != decisionScopeId
        || decoded.value().targetScopeKey != targetScopeKey)
        return Result<std::optional<SctReconciliationDecisionArtifact>>::failure(
            decisionError("The reconciliation decision artifact has the wrong context.",
                sourcePath));
    return Result<std::optional<SctReconciliationDecisionArtifact>>::success(
        std::optional<SctReconciliationDecisionArtifact>{
            std::move(decoded).takeValue()});
}

Result<void> DirectorySctReconciliationDecisionStore::checkpointReconciliationDecisions(
    const SctReconciliationDecisionArtifact& artifact) const {
    auto encoded = SctReconciliationDecisionCodec::serialize(artifact);
    if (!encoded) return Result<void>::failure(encoded.diagnostics());
    std::error_code directoryError;
    std::filesystem::create_directories(root_, directoryError);
    if (directoryError) return Result<void>::failure(decisionError(
        "The reconciliation decision directory could not be created.", root_));
    const auto bytes = std::as_bytes(std::span{
        encoded.value().data(), encoded.value().size()});
    return replaceFileAtomically(path(artifact.decisionScopeId,
        artifact.targetScopeKey), bytes);
}

}  // namespace salsa::core
