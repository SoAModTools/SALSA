#include "SalsaCore/Persistence/SctWorkspaceRebase.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cstring>
#include <fstream>

namespace salsa::core {
namespace {
using namespace spice::sct;

class RebaseTemporaryDirectory final {
public:
    RebaseTemporaryDirectory() {
        static std::atomic<unsigned long> sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / (L"salsa-rebase-tests-" + std::to_wstring(GetCurrentProcessId())
                + L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_ / L"dataset/scripts");
        std::filesystem::create_directories(path_ / L"workspace");
    }
    ~RebaseTemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_{};
};

[[nodiscard]] SctDocument sourceDocument(const std::string& sectionName = "SCRIPT") {
    SctDocumentBuilder builder;
    SctScriptSectionContent script;
    for (const std::uint16_t opcode : {9u, 12u}) {
        const auto draft = SctInstructionFactory::createDraft({opcode});
        EXPECT_TRUE(draft.draft.has_value());
        const auto instruction = SctInstructionFactory::materialize(
            builder.document(), *draft.draft);
        EXPECT_TRUE(instruction.instruction.has_value());
        script.instructions.push_back(*instruction.instruction);
    }
    builder.document().sections.push_back(
        {builder.allocateSectionId(), sectionName, std::move(script)});
    return std::move(builder).finish();
}

[[nodiscard]] std::vector<std::byte> encode(const SctDocument& document) {
    const SctDocumentExportOptions options{SctPlatform::GameCube,
        kSctShiftJisByte7FEncoding, SctDocumentOutputByteOrder::BigEndian,
        SctDocumentOutputWrapper::Raw};
    const auto exported = SctDocumentExporter::exportDocument(document, options);
    EXPECT_TRUE(exported.success);
    std::vector<std::byte> result(exported.bytes.size());
    std::memcpy(result.data(), exported.bytes.data(), exported.bytes.size());
    return result;
}

void write(const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

TEST(SctWorkspaceRebaseTest, DiscoversRebasesCommitsAndReopensAStalePatch) {
    RebaseTemporaryDirectory temporary;
    const auto datasetRoot = temporary.path() / L"dataset";
    const auto sourcePath = datasetRoot / L"scripts/test.sct";
    const auto asset = AssetLocator::fromRelativePath("scripts/test.sct").value();
    write(sourcePath, encode(sourceDocument()));
    auto oldProject = LocalGameProject::inspect({datasetRoot}).value();
    auto workspace = LocalSalsaWorkspace::openOrCreate(
        temporary.path() / L"workspace", oldProject.dataset()).value();

    const auto loaded = SctPatchCheckpointService::load(
        oldProject, nullptr, nullptr, asset);
    ASSERT_TRUE(loaded.load.succeeded());
    SctEditSession session(loaded.load.document);
    const auto& oldInstructions = std::get<SctScriptSectionContent>(
        loaded.load.document->document->sections.front().content).instructions;
    ASSERT_TRUE(session.insertInstructionAfter(oldInstructions.front().id, 125u).committed);
    const auto checkpointRequest = session.checkpointRequest(1u);
    ASSERT_TRUE(checkpointRequest.has_value());
    const auto checkpoint = SctPatchCheckpointService::checkpoint(
        *checkpointRequest, workspace, workspace);
    ASSERT_TRUE(checkpoint.saved);
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledDiscovery = SctWorkspaceRebaseService::discover(
        oldProject, workspace, cancelled.get_token());
    ASSERT_EQ(cancelledDiscovery.diagnostics.size(), 1u);
    EXPECT_EQ(cancelledDiscovery.diagnostics.front().code, DiagnosticCode::Cancelled);

    write(sourcePath, encode(sourceDocument("RENAMED")));
    auto newProject = oldProject.rescan().value();
    const auto discovery = SctWorkspaceRebaseService::discover(
        newProject, workspace);
    ASSERT_TRUE(discovery.diagnostics.empty());
    ASSERT_EQ(discovery.candidates.size(), 1u);
    const auto plan = SctWorkspaceRebaseService::build(
        newProject, workspace, discovery.candidates.front());
    ASSERT_TRUE(plan) << (plan.diagnostics().empty()
        ? "no diagnostic" : plan.diagnostics().front().message);
    const auto stalePreview = SctPatchRebaseService::preview(
        plan.value().corePlan, {}, "stale-context");
    EXPECT_EQ(stalePreview.merge.status, SctMergePreviewStatus::Stale);
    const auto preview = SctPatchRebaseService::preview(
        plan.value().corePlan, {}, discovery.candidates.front().contextToken);
    ASSERT_EQ(preview.merge.status, SctMergePreviewStatus::Ready);
    const auto committed = SctWorkspaceRebaseService::commit(
        newProject, workspace, plan.value(), preview, {});
    ASSERT_TRUE(committed.committed);

    const auto reopened = SctPatchCheckpointService::load(
        newProject, &workspace, &workspace, asset);
    ASSERT_TRUE(reopened.patchApplied);
    ASSERT_FALSE(reopened.patchConflict);
    ASSERT_TRUE(reopened.load.document);
    EXPECT_EQ(reopened.load.document->document->sections.front().nameBytes, "RENAMED");
    EXPECT_EQ(std::get<SctScriptSectionContent>(
        reopened.load.document->document->sections.front().content)
        .instructions.size(), 3u);
}

TEST(SctWorkspaceRebaseTest, CommitsSelectedCleanAssetsInOneTransaction) {
    RebaseTemporaryDirectory temporary;
    const auto datasetRoot = temporary.path() / L"dataset";
    const std::array assets{
        AssetLocator::fromRelativePath("scripts/first.sct").value(),
        AssetLocator::fromRelativePath("scripts/second.sct").value()};
    write(datasetRoot / assets[0].path(), encode(sourceDocument("FIRST")));
    write(datasetRoot / assets[1].path(), encode(sourceDocument("SECOND")));
    auto oldProject = LocalGameProject::inspect({datasetRoot}).value();
    auto workspace = LocalSalsaWorkspace::openOrCreate(
        temporary.path() / L"workspace", oldProject.dataset()).value();

    for (const auto& asset : assets) {
        const auto loaded = SctPatchCheckpointService::load(
            oldProject, nullptr, nullptr, asset);
        ASSERT_TRUE(loaded.load.succeeded());
        SctEditSession session(loaded.load.document);
        const auto& instructions = std::get<SctScriptSectionContent>(
            loaded.load.document->document->sections.front().content).instructions;
        ASSERT_TRUE(session.insertInstructionAfter(
            instructions.front().id, 125u).committed);
        const auto request = session.checkpointRequest(1u);
        ASSERT_TRUE(request.has_value());
        ASSERT_TRUE(SctPatchCheckpointService::checkpoint(
            *request, workspace, workspace).saved);
    }

    write(datasetRoot / assets[0].path(), encode(sourceDocument("FIRST_NEW")));
    write(datasetRoot / assets[1].path(), encode(sourceDocument("SECOND_NEW")));
    auto newProject = oldProject.rescan().value();
    const auto discovery = SctWorkspaceRebaseService::discover(newProject, workspace);
    ASSERT_EQ(discovery.candidates.size(), 2u);
    std::vector<SctWorkspaceRebasePlan> plans;
    std::vector<SctPatchRebasePreview> previews;
    for (const auto& candidate : discovery.candidates) {
        auto plan = SctWorkspaceRebaseService::build(
            newProject, workspace, candidate);
        ASSERT_TRUE(plan);
        plans.push_back(std::move(plan).takeValue());
        previews.push_back(SctPatchRebaseService::preview(
            plans.back().corePlan, {}, candidate.contextToken));
        ASSERT_EQ(previews.back().merge.status, SctMergePreviewStatus::Ready);
    }
    const std::array selections{
        SctWorkspaceRebaseCommitSelection{&plans[0], &previews[0], {}},
        SctWorkspaceRebaseCommitSelection{&plans[1], &previews[1], {}}};
    const auto committed = SctWorkspaceRebaseService::commitSelected(
        newProject, workspace, selections);
    ASSERT_TRUE(committed.committed);
    EXPECT_EQ(committed.committedAssets.size(), 2u);

    for (const auto& asset : assets) {
        const auto reopened = SctPatchCheckpointService::load(
            newProject, &workspace, &workspace, asset);
        ASSERT_TRUE(reopened.patchApplied);
        ASSERT_FALSE(reopened.patchConflict);
        EXPECT_EQ(std::get<SctScriptSectionContent>(
            reopened.load.document->document->sections.front().content)
            .instructions.size(), 3u);
    }
}

}  // namespace
}  // namespace salsa::core
