#pragma once

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/History/RevisionHistory.h"
#include "SalsaCore/Project/GameProjectContext.h"
#include "SalsaCore/Sct/SctDocumentMaterializer.h"
#include "SpiceSCT/SctDocumentExporter.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace salsa::core {

enum class SctPublicationPhase {
    Preflight,
    Materializing,
    Encoding,
    Hashing,
    Installing,
};

struct SctPublicationProgress final {
    SctPublicationPhase phase = SctPublicationPhase::Preflight;
    std::uint64_t completed = 0;
    std::uint64_t total = 1;
};

using SctPublicationObserver = std::function<void(const SctPublicationProgress&)>;

struct SctPublicationOptions final {
    spice::sct::SctPlatform platform = spice::sct::SctPlatform::GameCube;
    spice::sct::SctTextEncoding textEncoding{};
    spice::sct::SctDocumentOutputByteOrder byteOrder =
        spice::sct::SctDocumentOutputByteOrder::BigEndian;
    spice::sct::SctDocumentOutputWrapper wrapper =
        spice::sct::SctDocumentOutputWrapper::Raw;
    auto operator<=>(const SctPublicationOptions&) const = default;
};

struct SctPublicationRevision final {
    RevisionId revision{};
    std::shared_ptr<const void> historyStateToken{};
    std::shared_ptr<const SctDocumentProvenance> provenance{};
    std::shared_ptr<const SctDocumentSnapshot> verifiedSnapshot{};
    std::optional<SctMaterializationRequest> materialization{};
};

struct SctPublicationRequest final {
    AssetLocator locator;
    DatasetFingerprint sourceDataset;
    SourceRevision expectedSourceRevision;
    SctPublicationRevision capturedRevision;
    SctPublicationOptions options;
    std::filesystem::path destination;
    bool allowSourceReplacement = false;
};

struct SctPublicationReceipt final {
    AssetLocator locator;
    RevisionId revision{};
    DatasetFingerprint sourceDataset;
    SourceRevision sourceRevision;
    std::filesystem::path destination;
    SctPublicationOptions options;
    Sha256Digest outputDigest;
    std::uint64_t outputSize = 0;
    std::uint32_t decodedPayloadSize = 0;
    spice::sct::SctDocumentLayout layout;
    spice::sct::SctPreservationReport preservation;
    bool replacedSource = false;
};

struct SctPublicationResult final {
    std::optional<SctPublicationReceipt> receipt{};
    std::vector<SctPipelineDiagnostic> diagnostics{};
    std::vector<Diagnostic> infrastructureDiagnostics{};
    std::optional<SctMaterializationResult> materialization{};
    bool cancelled = false;

    [[nodiscard]] bool succeeded() const noexcept {
        return receipt.has_value() && !cancelled
            && !hasErrors(infrastructureDiagnostics);
    }
};

struct SctPublicationDefaults final {
    std::optional<spice::sct::SctPlatform> platform{};
    std::optional<spice::sct::SctTextEncoding> textEncoding{};
    std::optional<spice::sct::SctDocumentOutputByteOrder> byteOrder{};
    std::optional<spice::sct::SctDocumentOutputWrapper> wrapper{};
};

class SctPublicationService final {
public:
    [[nodiscard]] static SctPublicationDefaults defaultsFor(
        const DatasetContext& dataset,
        const SctDocumentSnapshot& snapshot) noexcept;

    [[nodiscard]] static SctPublicationResult publish(
        const GameProjectContext& project,
        const SctPublicationRequest& request,
        std::stop_token stopToken = {},
        const SctPublicationObserver& observer = {});
};

}  // namespace salsa::core
