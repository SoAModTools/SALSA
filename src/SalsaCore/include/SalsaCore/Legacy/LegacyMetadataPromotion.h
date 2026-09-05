#pragma once

#include "SalsaCore/Legacy/LegacyFreshImport.h"
#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"

#include <memory>
#include <span>

namespace salsa::core {

struct LegacyMetadataPromotionAssessment final {
    bool eligible = false;
    bool conflict = false;
    std::string reason{};
};

class LegacyMetadataPromotionAdapter {
public:
    virtual ~LegacyMetadataPromotionAdapter() = default;
    [[nodiscard]] virtual LegacyMetadataKind kind() const noexcept = 0;
    [[nodiscard]] virtual LegacyMetadataPromotionAssessment assess(
        const LegacyMetadataPlanRecord& record,
        const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const = 0;
    [[nodiscard]] virtual Result<std::vector<WorkspaceArtifactMutation>> prepare(
        const LegacyMetadataPlanRecord& record,
        const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const = 0;
    // Records with the same nonempty key are prepared as one artifact update. The
    // default keeps third-party adapters record-local.
    [[nodiscard]] virtual std::string aggregationKey(
        const LegacyMetadataPlanRecord& record) const;
    [[nodiscard]] virtual Result<std::vector<WorkspaceArtifactMutation>> prepareSelected(
        std::span<const LegacyMetadataPlanRecord> records,
        const LocalSalsaWorkspace& workspace,
        const std::filesystem::path& capsuleRoot) const;
};

class LegacyMetadataPromotionRegistry final {
public:
    [[nodiscard]] Result<void> add(
        std::shared_ptr<const LegacyMetadataPromotionAdapter> adapter);
    [[nodiscard]] const LegacyMetadataPromotionAdapter* find(
        LegacyMetadataKind kind) const noexcept;

private:
    std::vector<std::shared_ptr<const LegacyMetadataPromotionAdapter>> adapters_{};
};

// Registers the production adapters owned by the currently implemented SALSA
// authoring features. Unsupported capsule metadata remains pending.
[[nodiscard]] Result<void> registerBuiltInLegacyMetadataPromotionAdapters(
    LegacyMetadataPromotionRegistry& registry);

struct LegacyMetadataPromotionItem final {
    LegacyMetadataPlanRecord record{};
    LegacyMetadataPromotionAssessment assessment{};
};

struct LegacyMetadataPromotionPreview final {
    std::string capsuleId{};
    std::string stateDigest{};
    std::vector<LegacyMetadataPromotionItem> items{};

    [[nodiscard]] bool hasEligible() const noexcept;
};

struct LegacyMetadataPromotionResult final {
    bool applied = false;
    std::vector<std::string> appliedRecordIds{};
    std::vector<Diagnostic> diagnostics{};
};

class LegacyMetadataPromotionService final {
public:
    [[nodiscard]] static Result<LegacyMetadataPromotionPreview> preview(
        const LocalSalsaWorkspace& workspace,
        std::string_view capsuleId,
        const LegacyMetadataPromotionRegistry& registry);
    [[nodiscard]] static LegacyMetadataPromotionResult promote(
        const LocalSalsaWorkspace& workspace,
        const LegacyMetadataPromotionPreview& preview,
        std::span<const std::string> selectedRecordIds,
        const LegacyMetadataPromotionRegistry& registry);
};

}  // namespace salsa::core
