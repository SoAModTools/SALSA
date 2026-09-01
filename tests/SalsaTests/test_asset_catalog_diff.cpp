#include "SalsaCore/Project/AssetCatalogDiff.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace salsa::core {
namespace {

[[nodiscard]] Sha256Digest digest(const unsigned char value) {
    std::array<std::byte, Sha256Digest::Size> bytes{};
    bytes.fill(static_cast<std::byte>(value));
    return Sha256Digest(bytes);
}

[[nodiscard]] AssetDescriptor asset(
    const std::filesystem::path& path,
    const std::uint64_t size,
    const unsigned char revision) {
    auto locator = AssetLocator::fromRelativePath(path);
    EXPECT_TRUE(locator);
    return AssetDescriptor{
        std::move(locator).takeValue(),
        size,
        SourceRevision{ digest(revision) },
    };
}

[[nodiscard]] AssetCatalogSnapshot catalog(std::vector<AssetDescriptor> assets) {
    std::ranges::sort(assets, {}, &AssetDescriptor::locator);
    return AssetCatalogSnapshot{
        std::move(assets),
        DatasetFingerprint{ digest(0xff) },
    };
}

TEST(AssetCatalogDiffTest, ReportsAddedRemovedAndChangedAssets) {
    const auto previous = catalog({
        asset(L"a.sct", 1, 1),
        asset(L"b.sct", 2, 2),
        asset(L"same.sct", 3, 3),
    });
    const auto current = catalog({
        asset(L"b.sct", 4, 4),
        asset(L"same.sct", 3, 3),
        asset(L"z.sct", 5, 5),
    });

    const auto delta = diffAssetCatalogs(previous, current);

    ASSERT_EQ(delta.added.size(), 1U);
    EXPECT_EQ(delta.added.front().path(), L"z.sct");
    ASSERT_EQ(delta.removed.size(), 1U);
    EXPECT_EQ(delta.removed.front().path(), L"a.sct");
    ASSERT_EQ(delta.changed.size(), 1U);
    EXPECT_EQ(delta.changed.front().path(), L"b.sct");
}

TEST(AssetCatalogDiffTest, TreatsAPathRenameAsRemoveAndAdd) {
    const auto previous = catalog({ asset(L"old.sct", 1, 1) });
    const auto current = catalog({ asset(L"renamed.sct", 1, 1) });

    const auto delta = diffAssetCatalogs(previous, current);

    ASSERT_EQ(delta.added.size(), 1U);
    ASSERT_EQ(delta.removed.size(), 1U);
    EXPECT_TRUE(delta.changed.empty());
}

TEST(AssetCatalogDiffTest, UsesCaseInsensitiveLocatorIdentity) {
    const auto previous = catalog({ asset(L"Folder/A.SCT", 1, 1) });
    const auto current = catalog({ asset(L"folder/a.sct", 1, 1) });

    EXPECT_TRUE(diffAssetCatalogs(previous, current).empty());
}

TEST(AssetCatalogDiffTest, ReportsNoChangesForIdenticalCatalogs) {
    const auto previous = catalog({ asset(L"a.sct", 1, 1) });

    EXPECT_TRUE(diffAssetCatalogs(previous, previous).empty());
}

}  // namespace
}  // namespace salsa::core
