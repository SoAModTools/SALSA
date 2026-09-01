#include "SalsaCore/Project/LocalGameProject.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string_view>
#include <vector>

namespace salsa::core {
namespace {

class TemporaryDataset final {
public:
    TemporaryDataset() {
        static std::atomic<unsigned long> sequence = 0;
        root_ = std::filesystem::temp_directory_path() /
            (L"salsa-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }

    TemporaryDataset(const TemporaryDataset&) = delete;
    TemporaryDataset& operator=(const TemporaryDataset&) = delete;

    ~TemporaryDataset() {
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    void write(const std::filesystem::path& relative, const std::string_view contents) const {
        const auto target = root_ / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output);
    }

private:
    std::filesystem::path root_{};
};

[[nodiscard]] bool hasCode(
    const std::vector<Diagnostic>& diagnostics,
    const DiagnosticCode code) {
    return std::ranges::any_of(diagnostics, [code](const Diagnostic& diagnostic) {
        return diagnostic.code == code;
    });
}

TEST(LocalGameProjectTest, CatalogsOnlySctFilesRecursivelyInStableOrder) {
    TemporaryDataset dataset{};
    dataset.write(L"z.SCT", "z");
    dataset.write(L"会話/a001a.sct", "dialogue");
    dataset.write(L"ignored.txt", "not an asset");

    auto result = LocalGameProject::inspect({ dataset.root() });

    ASSERT_TRUE(result);
    const auto& project = result.value();
    ASSERT_EQ(project.snapshot().assets.size(), 2U);
    EXPECT_EQ(project.snapshot().assets[0].locator.path().generic_wstring(), L"z.SCT");
    EXPECT_EQ(project.snapshot().assets[1].locator.path().generic_wstring(), L"会話/a001a.sct");
    EXPECT_FALSE(project.dataset().identity.platform.has_value());
    EXPECT_FALSE(project.dataset().identity.region.has_value());
    EXPECT_EQ(project.dataset().identity.fingerprint, project.snapshot().fingerprint);
}

TEST(LocalGameProjectTest, RetainsExplicitOptionalMetadata) {
    TemporaryDataset dataset{};
    dataset.write(L"a.sct", "a");

    auto result = LocalGameProject::inspect({
        dataset.root(),
        GamePlatform::Dreamcast,
        GameRegion::Japan,
    });

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().dataset().identity.platform, GamePlatform::Dreamcast);
    EXPECT_EQ(result.value().dataset().identity.region, GameRegion::Japan);
}

TEST(LocalGameProjectTest, FingerprintIsIndependentOfAbsoluteRoot) {
    TemporaryDataset first{};
    TemporaryDataset second{};
    first.write(L"scripts/a.sct", "same");
    second.write(L"scripts/a.sct", "same");

    auto firstProject = LocalGameProject::inspect({ first.root() });
    auto secondProject = LocalGameProject::inspect({ second.root() });

    ASSERT_TRUE(firstProject);
    ASSERT_TRUE(secondProject);
    EXPECT_EQ(
        firstProject.value().snapshot().fingerprint,
        secondProject.value().snapshot().fingerprint);
}

TEST(LocalGameProjectTest, FingerprintTracksContentPathsAddsAndRemovals) {
    TemporaryDataset dataset{};
    dataset.write(L"a.sct", "one");
    auto initial = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(initial);

    dataset.write(L"a.sct", "two");
    auto contentChanged = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(contentChanged);
    EXPECT_NE(initial.value().snapshot().fingerprint, contentChanged.value().snapshot().fingerprint);

    std::filesystem::rename(dataset.root() / L"a.sct", dataset.root() / L"renamed.sct");
    auto pathChanged = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(pathChanged);
    EXPECT_NE(contentChanged.value().snapshot().fingerprint, pathChanged.value().snapshot().fingerprint);

    dataset.write(L"added.sct", "added");
    auto added = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(added);
    EXPECT_NE(pathChanged.value().snapshot().fingerprint, added.value().snapshot().fingerprint);

    std::filesystem::remove(dataset.root() / L"added.sct");
    auto removed = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(removed);
    EXPECT_EQ(pathChanged.value().snapshot().fingerprint, removed.value().snapshot().fingerprint);
}

TEST(LocalGameProjectTest, LoadsOnlyTheRevisionCapturedByTheSnapshot) {
    TemporaryDataset dataset{};
    dataset.write(L"a.sct", "original");
    auto inspected = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(inspected);
    auto project = std::move(inspected).takeValue();
    const auto locator = project.snapshot().assets.front().locator;

    auto loaded = project.loadAsset(locator);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.value().bytes.size(), 8U);
    EXPECT_EQ(loaded.value().descriptor.revision, project.snapshot().assets.front().revision);

    dataset.write(L"a.sct", "changed");
    auto stale = project.loadAsset(locator);
    ASSERT_FALSE(stale);
    EXPECT_TRUE(hasCode(stale.diagnostics(), DiagnosticCode::SourceChanged));

    auto rescanned = project.rescan();
    ASSERT_TRUE(rescanned);
    EXPECT_NE(project.snapshot().fingerprint, rescanned.value().snapshot().fingerprint);
    EXPECT_TRUE(rescanned.value().loadAsset(locator));
}

TEST(LocalGameProjectTest, RejectsMissingInvalidAndEmptyDatasetRoots) {
    auto missing = LocalGameProject::inspect({
        std::filesystem::temp_directory_path() / L"salsa-path-that-does-not-exist",
    });
    ASSERT_FALSE(missing);
    EXPECT_TRUE(hasCode(missing.diagnostics(), DiagnosticCode::InvalidDatasetRoot));

    TemporaryDataset dataset{};
    auto empty = LocalGameProject::inspect({ dataset.root() });
    ASSERT_FALSE(empty);
    EXPECT_TRUE(hasCode(empty.diagnostics(), DiagnosticCode::NoSctAssets));

    dataset.write(L"not-a-directory.txt", "file");
    auto file = LocalGameProject::inspect({ dataset.root() / L"not-a-directory.txt" });
    ASSERT_FALSE(file);
    EXPECT_TRUE(hasCode(file.diagnostics(), DiagnosticCode::InvalidDatasetRoot));
}

TEST(LocalGameProjectTest, HonorsPreRequestedCancellationWithoutPartialResults) {
    TemporaryDataset dataset{};
    dataset.write(L"a.sct", "a");
    std::stop_source source{};
    source.request_stop();

    auto result = LocalGameProject::inspect({ dataset.root() }, source.get_token());

    ASSERT_FALSE(result);
    EXPECT_FALSE(result.hasValue());
    EXPECT_TRUE(hasCode(result.diagnostics(), DiagnosticCode::Cancelled));
}

TEST(LocalGameProjectTest, ReportsDiscoveryAndHashingProgressWithoutChangingResults) {
    TemporaryDataset dataset{};
    dataset.write(L"nested/a.sct", "a");
    dataset.write(L"b.SCT", "bb");

    std::vector<DatasetScanProgress> progress{};
    auto observed = LocalGameProject::inspect(
        { dataset.root() },
        {},
        [&progress](const DatasetScanProgress& update) {
            progress.push_back(update);
        });
    auto unobserved = LocalGameProject::inspect({ dataset.root() });

    ASSERT_TRUE(observed);
    ASSERT_TRUE(unobserved);
    EXPECT_EQ(observed.value().snapshot().fingerprint, unobserved.value().snapshot().fingerprint);
    ASSERT_EQ(
        observed.value().snapshot().assets.size(),
        unobserved.value().snapshot().assets.size());
    for (std::size_t index = 0; index < observed.value().snapshot().assets.size(); ++index) {
        const auto& observedAsset = observed.value().snapshot().assets[index];
        const auto& unobservedAsset = unobserved.value().snapshot().assets[index];
        EXPECT_EQ(observedAsset.locator, unobservedAsset.locator);
        EXPECT_EQ(observedAsset.byteSize, unobservedAsset.byteSize);
        EXPECT_EQ(observedAsset.revision, unobservedAsset.revision);
    }

    EXPECT_EQ(std::ranges::count_if(progress, [](const auto& update) {
        return update.phase == DatasetScanPhase::Discovering;
    }), 2);
    EXPECT_EQ(std::ranges::count_if(progress, [](const auto& update) {
        return update.phase == DatasetScanPhase::Hashing;
    }), 2);

    const auto& final = progress.back();
    EXPECT_EQ(final.phase, DatasetScanPhase::Hashing);
    EXPECT_EQ(final.completed, 2U);
    ASSERT_TRUE(final.total.has_value());
    EXPECT_EQ(*final.total, 2U);
    EXPECT_FALSE(final.currentPath.empty());
}

TEST(LocalGameProjectTest, ObserverCanCancelWithoutReturningPartialResults) {
    TemporaryDataset dataset{};
    dataset.write(L"a.sct", "a");
    dataset.write(L"b.sct", "b");
    std::stop_source source{};

    auto result = LocalGameProject::inspect(
        { dataset.root() },
        source.get_token(),
        [&source](const DatasetScanProgress& update) {
            if (update.phase == DatasetScanPhase::Hashing && update.completed == 1U) {
                source.request_stop();
            }
        });

    ASSERT_FALSE(result);
    EXPECT_FALSE(result.hasValue());
    EXPECT_TRUE(hasCode(result.diagnostics(), DiagnosticCode::Cancelled));
}

TEST(LocalGameProjectTest, ReportsUnknownLocatorsWithoutTouchingTheFilesystem) {
    TemporaryDataset dataset{};
    dataset.write(L"a.sct", "a");
    auto inspected = LocalGameProject::inspect({ dataset.root() });
    ASSERT_TRUE(inspected);
    auto missing = AssetLocator::fromRelativePath(L"missing.sct");
    ASSERT_TRUE(missing);

    auto result = inspected.value().loadAsset(missing.value());

    ASSERT_FALSE(result);
    EXPECT_TRUE(hasCode(result.diagnostics(), DiagnosticCode::AssetNotFound));
}

}  // namespace
}  // namespace salsa::core
