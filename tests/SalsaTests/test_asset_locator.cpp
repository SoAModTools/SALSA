#include "SalsaCore/Project/AssetLocator.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace salsa::core {
namespace {

TEST(AssetLocatorTest, NormalizesSeparatorsAndComparesCaseInsensitively) {
    auto first = AssetLocator::fromRelativePath(std::filesystem::path(L"Scripts") / L"A001A.SCT");
    auto second = AssetLocator::fromRelativePath(L"scripts/a001a.sct");

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(first.value().identityKey(), "scripts/a001a.sct");
    EXPECT_EQ(first.value().path().generic_wstring(), L"Scripts/A001A.SCT");
}

TEST(AssetLocatorTest, PreservesUnicodeInItsStableIdentity) {
    auto locator = AssetLocator::fromRelativePath(L"会話/場面.SCT");

    ASSERT_TRUE(locator);
    EXPECT_FALSE(locator.value().identityKey().empty());
    EXPECT_EQ(locator.value().path().generic_wstring(), L"会話/場面.SCT");
}

TEST(AssetLocatorTest, RejectsPathsThatCanEscapeTheDataset) {
    EXPECT_FALSE(AssetLocator::fromRelativePath({}));
    EXPECT_FALSE(AssetLocator::fromRelativePath(L"C:\\dataset\\a.sct"));
    EXPECT_FALSE(AssetLocator::fromRelativePath(L"../a.sct"));
    EXPECT_FALSE(AssetLocator::fromRelativePath(L"scripts/../a.sct"));
    EXPECT_FALSE(AssetLocator::fromRelativePath(L"./a.sct"));
}

}  // namespace
}  // namespace salsa::core
