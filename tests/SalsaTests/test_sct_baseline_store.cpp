#include "SalsaCore/Persistence/SctBaselineStore.h"

#include "SalsaCore/Foundation/Hashing.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace salsa::core {
namespace {

class BaselineTemporaryDirectory final {
public:
    BaselineTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-baseline-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~BaselineTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

TEST(SctBaselineStoreTest, RetainsDeduplicatesAndVerifiesExactSourceBytes) {
    BaselineTemporaryDirectory temporary;
    DirectorySctBaselineStore store(temporary.path());
    const std::array bytes{std::byte{0x01}, std::byte{0x7f}, std::byte{0xff}};
    const SourceRevision revision{sha256(bytes).value()};
    EXPECT_FALSE(store.loadBaseline(revision).value().has_value());
    ASSERT_TRUE(store.retainBaseline(revision, bytes));
    ASSERT_TRUE(store.retainBaseline(revision, bytes));
    const auto loaded = store.loadBaseline(revision);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(*loaded.value(), (std::vector<std::byte>{bytes.begin(), bytes.end()}));
    EXPECT_EQ(std::ranges::distance(std::filesystem::directory_iterator(temporary.path()),
        std::filesystem::directory_iterator{}), 1);
}

TEST(SctBaselineStoreTest, RejectsWrongInputAndTamperedRetainedBytes) {
    BaselineTemporaryDirectory temporary;
    DirectorySctBaselineStore store(temporary.path());
    const std::array bytes{std::byte{0x01}, std::byte{0x02}};
    const SourceRevision revision{sha256(bytes).value()};
    const std::array wrong{std::byte{0x03}};
    EXPECT_FALSE(store.retainBaseline(revision, wrong));
    ASSERT_TRUE(store.retainBaseline(revision, bytes));
    std::ofstream output(store.path(revision), std::ios::binary | std::ios::trunc);
    output.put('\x04');
    output.close();
    const auto loaded = store.loadBaseline(revision);
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.diagnostics().front().code, DiagnosticCode::SctBaselineCorrupt);
}

}  // namespace
}  // namespace salsa::core
