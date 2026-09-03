#include "SalsaCore/Persistence/LocalSalsaWorkspace.h"
#include "SalsaCore/Persistence/WorkspaceSession.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace salsa::core {
namespace {

class WorkspaceTemporaryDirectory final {
public:
    WorkspaceTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-workspace-persistence-tests-"
                + std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~WorkspaceTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

[[nodiscard]] Sha256Digest testDigest(const unsigned char value) {
    std::array<std::byte, Sha256Digest::Size> bytes{};
    bytes.fill(static_cast<std::byte>(value));
    return Sha256Digest(bytes);
}

[[nodiscard]] DatasetContext testDataset(
    const std::filesystem::path& root, const unsigned char fingerprint = 1) {
    return {root, DatasetIdentity{GamePlatform::GameCube, GameRegion::NorthAmerica,
        DatasetFingerprint{testDigest(fingerprint)}}};
}

[[nodiscard]] AssetLocator testLocator(const wchar_t* path) {
    return AssetLocator::fromRelativePath(path).value();
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}

TEST(WorkspacePersistenceTest, CreatesSchemaTwoWithReservedComponents) {
    WorkspaceTemporaryDirectory temporary;
    const auto datasetRoot = temporary.path() / L"dataset";
    const auto workspaceRoot = temporary.path() / L"workspace";
    std::filesystem::create_directory(datasetRoot);

    auto opened = LocalSalsaWorkspace::openOrCreate(
        workspaceRoot, testDataset(datasetRoot));
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value().descriptor().dataset.root,
        std::filesystem::weakly_canonical(datasetRoot));
    EXPECT_EQ(opened.value().descriptor().workspaceId.size(), 36u);
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"patches"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"baselines"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"authoring"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"imports"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"import-state"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"history"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"receipts"));
    EXPECT_TRUE(std::filesystem::is_directory(workspaceRoot / L"transactions"));
    EXPECT_EQ(opened.value().sessionPath(), workspaceRoot / L"session.json");

    auto reopened = LocalSalsaWorkspace::openOrCreate(
        workspaceRoot, testDataset(datasetRoot));
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened.value().descriptor().workspaceId,
        opened.value().descriptor().workspaceId);
}

TEST(WorkspacePersistenceTest, RequiresAndPersistsExplicitDatasetReassociation) {
    WorkspaceTemporaryDirectory temporary;
    const auto first = temporary.path() / L"dataset-a";
    const auto second = temporary.path() / L"dataset-b";
    const auto workspace = temporary.path() / L"workspace";
    std::filesystem::create_directory(first);
    std::filesystem::create_directory(second);
    ASSERT_TRUE(LocalSalsaWorkspace::openOrCreate(workspace, testDataset(first, 1)));

    auto assessment = LocalSalsaWorkspace::assess(workspace, testDataset(second, 2));
    ASSERT_TRUE(assessment);
    EXPECT_TRUE(assessment.value().requiresReassociation());
    auto rejected = LocalSalsaWorkspace::openOrCreate(workspace, testDataset(second, 2));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.diagnostics().front().code,
        DiagnosticCode::WorkspaceDatasetReassociationRequired);

    auto accepted = LocalSalsaWorkspace::openOrCreate(workspace,
        testDataset(second, 2), WorkspaceDatasetAcceptance::UserConfirmedReassociation);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().descriptor().dataset.identity.fingerprint,
        testDataset(second, 2).identity.fingerprint);
    EXPECT_TRUE(LocalSalsaWorkspace::openOrCreate(workspace, testDataset(second, 2)));
}

TEST(WorkspacePersistenceTest, RebindsMovedSiblingLayoutOnlyWithExactFingerprint) {
    WorkspaceTemporaryDirectory temporary;
    const auto original = temporary.path() / L"bundle-original";
    const auto moved = temporary.path() / L"bundle-moved";
    std::filesystem::create_directory(original);
    std::filesystem::create_directory(original / L"dataset");
    ASSERT_TRUE(LocalSalsaWorkspace::openOrCreate(
        original / L"workspace", testDataset(original / L"dataset", 7)));
    std::filesystem::rename(original, moved);

    auto assessment = LocalSalsaWorkspace::assess(
        moved / L"workspace", testDataset(moved / L"dataset", 7));
    ASSERT_TRUE(assessment);
    EXPECT_EQ(assessment.value().disposition,
        WorkspaceOpenDisposition::RebindRelative);
    auto opened = LocalSalsaWorkspace::openOrCreate(
        moved / L"workspace", testDataset(moved / L"dataset", 7));
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value().descriptor().lastKnownDatasetRoot,
        std::filesystem::weakly_canonical(moved / L"dataset"));
}

TEST(WorkspacePersistenceTest, UpgradesSchemaOneAndRemovesRollbackAfterVerification) {
    WorkspaceTemporaryDirectory temporary;
    const auto dataset = temporary.path() / L"dataset";
    const auto workspace = temporary.path() / L"workspace";
    std::filesystem::create_directory(dataset);
    std::filesystem::create_directory(workspace);
    std::filesystem::create_directory(workspace / L"patches");
    writeText(workspace / L"project.json",
        "{\n  \"formatId\": \"jahorta.salsa.workspace\",\n"
        "  \"schemaVersion\": 1,\n  \"datasetRoot\": \""
        + dataset.generic_string() + "\"\n}\n");

    auto assessment = LocalSalsaWorkspace::assess(workspace, testDataset(dataset));
    ASSERT_TRUE(assessment);
    EXPECT_EQ(assessment.value().disposition,
        WorkspaceOpenDisposition::UpgradeSchemaOne);
    auto opened = LocalSalsaWorkspace::openOrCreate(workspace, testDataset(dataset));
    ASSERT_TRUE(opened);
    EXPECT_FALSE(std::filesystem::exists(
        workspace / L"project.json.schema1.rollback"));
    EXPECT_TRUE(std::filesystem::is_directory(workspace / L"transactions"));
}

TEST(WorkspacePersistenceTest, RecoversRollbackOnlySchemaOneUpgradeState) {
    WorkspaceTemporaryDirectory temporary;
    const auto dataset = temporary.path() / L"dataset";
    const auto workspace = temporary.path() / L"workspace";
    std::filesystem::create_directory(dataset);
    std::filesystem::create_directory(workspace);
    const std::string schemaOne =
        "{\n  \"formatId\": \"jahorta.salsa.workspace\",\n"
        "  \"schemaVersion\": 1,\n  \"datasetRoot\": \""
        + dataset.generic_string() + "\"\n}\n";
    writeText(workspace / L"project.json.schema1.rollback", schemaOne);

    auto assessment = LocalSalsaWorkspace::assess(workspace, testDataset(dataset));
    ASSERT_TRUE(assessment);
    EXPECT_EQ(assessment.value().disposition,
        WorkspaceOpenDisposition::RecoverSchemaOne);
    auto opened = LocalSalsaWorkspace::openOrCreate(workspace, testDataset(dataset));
    ASSERT_TRUE(opened);
    EXPECT_TRUE(std::filesystem::exists(workspace / L"project.json"));
    EXPECT_FALSE(std::filesystem::exists(
        workspace / L"project.json.schema1.rollback"));
}

TEST(WorkspacePersistenceTest, RejectsFutureWorkspaceWithoutChangingIt) {
    WorkspaceTemporaryDirectory temporary;
    const auto dataset = temporary.path() / L"dataset";
    const auto workspace = temporary.path() / L"workspace";
    std::filesystem::create_directory(dataset);
    std::filesystem::create_directory(workspace);
    const std::string future =
        "{\"formatId\":\"jahorta.salsa.workspace\",\"schemaVersion\":999}";
    writeText(workspace / L"project.json", future);
    auto opened = LocalSalsaWorkspace::openOrCreate(workspace, testDataset(dataset));
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.diagnostics().front().code,
        DiagnosticCode::UnsupportedPersistenceSchemaVersion);
    std::ifstream stream(workspace / L"project.json", std::ios::binary);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(stream)), {}), future);
}

TEST(WorkspaceSessionTest, RoundTripsTypedCheckpointContext) {
    WorkspaceSessionState state;
    state.workspaceId = "12345678-1234-4234-9234-123456789abc";
    const auto first = testLocator(L"scripts/A.sct");
    const auto second = testLocator(L"scripts/B.sct");
    state.documents = {
        {first, SctDocumentView::Physical,
            SctNavigationTarget{SctNavigationKind::Instruction, 10}},
        {second, SctDocumentView::Semantic,
            SctNavigationTarget{SctNavigationKind::Section, 20}},
    };
    state.activeDocument = second;
    state.selectedProjectAsset = first;
    state.expandedProjectDirectories = {L"scripts", L"scripts/subdirectory"};
    state.navigation = {
        {},
        {first, SctNavigationTarget{SctNavigationKind::Instruction, 10}},
        {second, SctNavigationTarget{SctNavigationKind::Section, 20}},
    };
    state.navigationIndex = 1;

    auto encoded = WorkspaceSessionCodec::serialize(state);
    ASSERT_TRUE(encoded);
    auto decoded = WorkspaceSessionCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), state);
    EXPECT_EQ(WorkspaceSessionCodec::serialize(decoded.value()).value(), encoded.value());
}

TEST(WorkspaceSessionTest, RejectsInconsistentAndMismatchedState) {
    WorkspaceSessionState invalid;
    invalid.workspaceId = "12345678-1234-4234-9234-123456789abc";
    invalid.activeDocument = testLocator(L"missing.sct");
    EXPECT_FALSE(WorkspaceSessionCodec::serialize(invalid));

    WorkspaceTemporaryDirectory temporary;
    WorkspaceSessionState valid;
    valid.workspaceId = "12345678-1234-4234-9234-123456789abc";
    WorkspaceSessionStore store;
    ASSERT_TRUE(store.checkpoint(temporary.path() / L"session.json", valid));
    auto mismatched = store.load(temporary.path() / L"session.json",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    EXPECT_FALSE(mismatched);
    EXPECT_EQ(mismatched.diagnostics().front().code,
        DiagnosticCode::InvalidWorkspaceSession);
}

TEST(WorkspaceSessionTest, RejectsCorruptAndUnsupportedOptionalState) {
    EXPECT_FALSE(WorkspaceSessionCodec::deserialize("not-json"));
    const auto unsupported = WorkspaceSessionCodec::deserialize(
        "{\"formatId\":\"jahorta.salsa.session\",\"schemaVersion\":2,"
        "\"workspaceId\":\"12345678-1234-4234-9234-123456789abc\","
        "\"documents\":[],\"activeDocument\":null,"
        "\"selectedProjectAsset\":null,\"expandedProjectDirectories\":[],"
        "\"navigation\":[],\"navigationIndex\":0}");
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.diagnostics().front().code,
        DiagnosticCode::UnsupportedPersistenceSchemaVersion);
}

}  // namespace
}  // namespace salsa::core
