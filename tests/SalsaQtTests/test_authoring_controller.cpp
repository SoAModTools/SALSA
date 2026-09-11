#include "Sct/SctDocumentController.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include "SpiceSCT/SctInstructionFactory.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <gtest/gtest.h>
#include <fstream>

namespace {
using namespace salsa;
namespace sct = spice::sct;
bool waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 15000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return predicate();
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("salsa-qt-s3-" + core::generateSctAuthoringUuid().value());
    std::optional<core::LocalGameProject> project;
    std::shared_ptr<const core::LocalSalsaWorkspace> workspace;
    core::AssetLocator a = core::AssetLocator::fromRelativePath("a.sct").value();
    core::AssetLocator b = core::AssetLocator::fromRelativePath("b.sct").value();
    Fixture() {
        std::filesystem::create_directories(root / "source");
        sct::SctDocument d;
        sct::SctDocumentInstruction instruction; instruction.id = d.allocateInstructionId(); instruction.opcode = 16;
        instruction.fixedParameters = {{0, sct::SctExpressionFactory::encodedDecimalLiteral(12)}};
        d.sections.push_back({d.allocateSectionId(), "MAIN", sct::SctScriptSectionContent{{instruction}}});
        auto output = sct::SctDocumentExporter::exportDocument(d, sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,
            sct::kSctShiftJisByte7FEncoding, sct::SctDocumentOutputByteOrder::BigEndian, sct::SctDocumentOutputWrapper::Raw,
            sct::SctOpaquePreservationPolicy::RequirePreservation, {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
        if (!output.success) throw std::runtime_error("Synthetic output failed");
        for (const auto& locator : {a, b}) {
            std::ofstream file(root / "source" / locator.path(), std::ios::binary);
            file.write(reinterpret_cast<const char*>(output.bytes.data()), output.bytes.size());
        }
        project = core::LocalGameProject::inspect({root / "source", core::GamePlatform::GameCube, core::GameRegion::Japan}).value();
        workspace = std::make_shared<const core::LocalSalsaWorkspace>(core::LocalSalsaWorkspace::openOrCreate(root / "workspace", project->dataset()).value());
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
sct::SctParameterSite site(const qt::SctDocumentController& controller, const core::AssetLocator& locator) {
    const auto state = controller.semanticState(locator);
    const auto& document = *state->document;
    return {std::get<sct::SctScriptSectionContent>(document.sections.front().content).instructions.front().id, {0, {}}};
}
std::optional<core::SctLiteralConstant> value(const qt::SctDocumentController& controller, const core::AssetLocator& locator) {
    const auto state = controller.semanticState(locator);
    const auto& document = *state->document;
    return core::SctLiteralConstant::fromExpression(std::get<sct::SctCanonicalExpression>(
        std::get<sct::SctScriptSectionContent>(document.sections.front().content).instructions.front().fixedParameters.front().value));
}
TEST(AuthoringControllerTest, OneHistoryAcrossTabsAndProjectSaveSurvivesClosingAllViews) {
    Fixture f;
    qt::SctDocumentController controller;
    controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    ASSERT_TRUE(controller.contains(f.a));
    ASSERT_TRUE(controller.openDocument(*f.project, f.b)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    ASSERT_TRUE(controller.contains(f.b));
    ASSERT_TRUE(controller.replaceParameterValue(f.a, site(controller, f.a), sct::SctExpressionFactory::encodedDecimalLiteral(21)));
    ASSERT_TRUE(controller.replaceParameterValue(f.b, site(controller, f.b), sct::SctExpressionFactory::encodedDecimalLiteral(22)));
    const auto baseline = core::SctLiteralConstant::fromExpression(sct::SctExpressionFactory::encodedDecimalLiteral(12));
    const auto changedA = value(controller, f.a), changedB = value(controller, f.b);
    const auto siteA = site(controller, f.a);
    ASSERT_TRUE(controller.undo(f.a)); // Last command was made in b.
    EXPECT_EQ(value(controller, f.a), changedA); EXPECT_EQ(value(controller, f.b), baseline);
    ASSERT_TRUE(controller.redo(f.a)); EXPECT_EQ(value(controller, f.b), changedB);
    bool published = false;
    QObject::connect(&controller, &qt::SctDocumentController::publicationCompleted, &controller,
        [&](const QString&, bool success, bool, const QString&, bool) { published = success; });
    const auto exportedRevision = controller.workingRevision(f.a);
    ASSERT_TRUE(controller.exportDocument(*f.project, f.a,
        {sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding,
            sct::SctDocumentOutputByteOrder::BigEndian, sct::SctDocumentOutputWrapper::Raw}, f.root / "export.sct", false, {}));
    ASSERT_TRUE(waitFor([&] { return published; }));
    ASSERT_TRUE(controller.lastPublication(f.a)); EXPECT_EQ(controller.lastPublication(f.a)->revision, exportedRevision);
    controller.closeAll(); EXPECT_TRUE(controller.projectDirty()); EXPECT_EQ(controller.dirtyLocators().size(), 2u);
    EXPECT_TRUE(controller.canUndo(f.a));
    ASSERT_TRUE(controller.undo(f.a)); ASSERT_TRUE(controller.redo(f.a));
    EXPECT_TRUE(controller.openLocators().empty());
    ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&] { return !controller.isSaving(f.a); }));
    EXPECT_FALSE(controller.projectDirty());
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace);
    ASSERT_TRUE(reopened.openDocument(*f.project, f.a)); ASSERT_TRUE(reopened.openDocument(*f.project, f.b));
    EXPECT_EQ(value(reopened, f.a), changedA); EXPECT_EQ(value(reopened, f.b), changedB);
    EXPECT_EQ(site(reopened, f.a), siteA);
}
TEST(AuthoringControllerTest, FailedEditDoesNotChangeProjectAndMetadataUsesProjectUndo) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    const auto revision = controller.workingRevision(f.a);
    EXPECT_FALSE(controller.replaceParameterValue(f.a, {sct::SctInstructionId(99999), {0, {}}}, sct::SctExpressionFactory::encodedDecimalLiteral(2)));
    EXPECT_EQ(controller.workingRevision(f.a), revision);
    core::SctWorkspaceAuthoringState metadata; metadata.opcodeColors.push_back({16, 0x123456});
    ASSERT_TRUE(controller.setWorkspaceAuthoring(metadata)); EXPECT_EQ(controller.workspaceAuthoring(), metadata);
    ASSERT_TRUE(controller.undo(f.a)); EXPECT_TRUE(controller.workspaceAuthoring().opcodeColors.empty());
}
TEST(AuthoringControllerTest, DiscardRestoresSavedProjectAndAdvancesRevision) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    const auto savedValue = value(controller, f.a);
    ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&] { return !controller.isSaving(f.a); }));
    ASSERT_TRUE(controller.replaceParameterValue(f.a, site(controller, f.a), sct::SctExpressionFactory::encodedDecimalLiteral(44)));
    const auto editedRevision = controller.workingRevision(f.a);
    ASSERT_TRUE(controller.discardProjectChanges());
    EXPECT_FALSE(controller.projectDirty()); EXPECT_EQ(value(controller, f.a), savedValue);
    EXPECT_GT(controller.workingRevision(f.a).value, editedRevision.value);
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace);
    ASSERT_TRUE(reopened.openDocument(*f.project, f.a)); EXPECT_EQ(value(reopened, f.a), savedValue);
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
