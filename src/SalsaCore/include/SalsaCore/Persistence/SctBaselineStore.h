#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Project/ProjectTypes.h"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace salsa::core {

class SctBaselineStore {
public:
    virtual ~SctBaselineStore() = default;
    [[nodiscard]] virtual Result<std::optional<std::vector<std::byte>>> loadBaseline(
        const SourceRevision& revision) const = 0;
    [[nodiscard]] virtual Result<void> retainBaseline(
        const SourceRevision& revision, std::span<const std::byte> bytes) const = 0;
};

class DirectorySctBaselineStore final : public SctBaselineStore {
public:
    explicit DirectorySctBaselineStore(std::filesystem::path root);

    [[nodiscard]] std::filesystem::path path(const SourceRevision& revision) const;
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> loadBaseline(
        const SourceRevision& revision) const override;
    [[nodiscard]] Result<void> retainBaseline(
        const SourceRevision& revision, std::span<const std::byte> bytes) const override;

private:
    std::filesystem::path root_;
};

}  // namespace salsa::core
