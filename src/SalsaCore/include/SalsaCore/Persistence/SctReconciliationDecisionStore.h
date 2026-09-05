#pragma once

#include "SalsaCore/Sct/SctReconciliation.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace salsa::core {

class SctReconciliationDecisionCodec final {
public:
    static constexpr std::string_view FormatId =
        "jahorta.salsa.sct-reconciliation-decisions";
    static constexpr std::uint32_t SchemaVersion = 2;
    static constexpr std::uint32_t LegacySchemaVersion = 1;

    [[nodiscard]] static Result<std::string> serialize(
        const SctReconciliationDecisionArtifact& artifact);
    [[nodiscard]] static Result<SctReconciliationDecisionArtifact> deserialize(
        std::string_view json);
};

class SctReconciliationDecisionStore {
public:
    virtual ~SctReconciliationDecisionStore() = default;
    [[nodiscard]] virtual Result<std::optional<SctReconciliationDecisionArtifact>>
        loadReconciliationDecisions(std::string_view decisionScopeId,
            std::string_view targetScopeKey) const = 0;
    [[nodiscard]] virtual Result<void> checkpointReconciliationDecisions(
        const SctReconciliationDecisionArtifact& artifact) const = 0;
};

class DirectorySctReconciliationDecisionStore final
    : public SctReconciliationDecisionStore {
public:
    explicit DirectorySctReconciliationDecisionStore(std::filesystem::path root);

    [[nodiscard]] std::filesystem::path path(std::string_view decisionScopeId,
        std::string_view targetScopeKey) const;
    [[nodiscard]] Result<std::optional<SctReconciliationDecisionArtifact>>
        loadReconciliationDecisions(std::string_view decisionScopeId,
            std::string_view targetScopeKey) const override;
    [[nodiscard]] Result<void> checkpointReconciliationDecisions(
        const SctReconciliationDecisionArtifact& artifact) const override;

private:
    std::filesystem::path root_;
};

}  // namespace salsa::core
