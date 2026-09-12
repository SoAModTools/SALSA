#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Authoring/SctModuleAuthoring.h"
#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "Sct/SctDocumentController.h"
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
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
    Fixture(bool opaquePrefix = false) {
        std::filesystem::create_directories(root / "source");
        sct::SctDocument d;
        sct::SctDocumentInstruction instruction; instruction.id = d.allocateInstructionId(); instruction.opcode = 16;
        instruction.fixedParameters = {{0, sct::SctExpressionFactory::encodedDecimalLiteral(12)}};
        d.sections.push_back({d.allocateSectionId(), "MAIN", sct::SctScriptSectionContent{{instruction}}});
        for (const auto name : {"init","loop"}) d.sections.push_back({d.allocateSectionId(),name,sct::SctScriptSectionContent{{{d.allocateInstructionId(),12}}}});
        auto output = sct::SctDocumentExporter::exportDocument(d, sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,
            sct::kSctShiftJisByte7FEncoding, sct::SctDocumentOutputByteOrder::BigEndian, sct::SctDocumentOutputWrapper::Raw,
            sct::SctOpaquePreservationPolicy::RequirePreservation, {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
        if (!output.success) throw std::runtime_error("Synthetic output failed");
        if (opaquePrefix) {
            const auto offset = output.layout->sections.front().payloadSpan.offset;
            output.bytes.insert(output.bytes.begin() + offset, 32, 0xff);
            for (const auto& section : output.layout->sections) for (unsigned byte=0;byte<4;++byte)
                output.bytes[section.indexRowSpan.offset+byte] = static_cast<std::uint8_t>((section.dataRelativeOffset+32u) >> (24-byte*8));
        }
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
    ASSERT_TRUE(reopened.replaceParameterValue(f.a, siteA, sct::SctExpressionFactory::encodedDecimalLiteral(23)));
    ASSERT_TRUE(reopened.undo(f.a)); EXPECT_EQ(value(reopened, f.a), changedA);
}
TEST(AuthoringControllerTest, ProjectPublicationUsesFreshSemanticLayoutAfterGrowth) {
    Fixture f(true); qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    const auto before = controller.authoringState(); ASSERT_TRUE(before);
    ASSERT_FALSE(before->programs.front()->document().opaqueAttachments.empty());
    const auto script = before->project.scripts.front().id;
    const auto section = before->programs.front()->document().sections.front().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(before->project.revision, "Name sequence", [&](const auto& state) {
        return core::SctSequenceAuthoring::promoteSequence(state, script, section, "Fresh sequence");
    }));
    const auto sequence = controller.authoringState()->project.sequences.front();
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision, "Add schedule", [&](const auto& state) {
        return core::SctSequenceAuthoring::setTiming(state, sequence.id, sequence.actions.front().id,
            sct::SctExpressionFactory::encodedDecimalLiteral(3), true);
    }));
    bool finished = false, succeeded = false;
    QObject::connect(&controller, &qt::SctDocumentController::publicationCompleted, &controller,
        [&](const QString&, bool success, bool, const QString&, bool) { finished = true; succeeded = success; });
    ASSERT_TRUE(controller.exportDocument(*f.project, f.a,
        {sct::SctPlatform::GameCube, sct::kSctShiftJisByte7FEncoding,
            sct::SctDocumentOutputByteOrder::BigEndian, sct::SctDocumentOutputWrapper::Raw}, f.root / "fresh.sct", false, {}));
    ASSERT_TRUE(waitFor([&] { return finished; })); ASSERT_TRUE(succeeded);
    const auto receipt = controller.lastPublication(f.a); ASSERT_TRUE(receipt);
    EXPECT_TRUE(receipt->preservation.attachments.empty());
    ASSERT_EQ(receipt->layout.sections.size(), 3u); EXPECT_EQ(receipt->layout.sections[0].dataRelativeOffset, 0u);
    EXPECT_EQ(receipt->revision, controller.authoringState()->project.revision);
    EXPECT_FALSE(controller.authoringState()->programs.front()->document().opaqueAttachments.empty());
    ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&] { return !controller.isSaving(f.a); }));
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace);
    ASSERT_TRUE(reopened.authoringState());
    auto current = core::SctAuthoringMaterializer::workingState(reopened.authoringState()->project, reopened.authoringState()->programs, script); ASSERT_TRUE(current);
    EXPECT_TRUE(current.value().document->opaqueAttachments.empty());
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
TEST(AuthoringControllerTest, SemanticSequenceCommandsShareHistoryAndRejectStaleDialogCapture) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    const auto before = controller.authoringState(); ASSERT_TRUE(before);
    const auto script = before->project.scripts.front().id;
    const auto section = before->programs.front()->document().sections.front().id;
    auto promoted = controller.executeAuthoringCommand(before->project.revision, "Name sequence", [&](const auto& state) {
        return core::SctSequenceAuthoring::promoteSequence(state, script, section, "Opening");
    }); ASSERT_TRUE(promoted);
    const auto sequence = controller.authoringState()->project.sequences.front();
    const auto edit = [&](const auto& state) { return core::SctSequenceAuthoring::setTiming(state, sequence.id,
        sequence.actions.front().id, *core::SctExpressionLanguage::parse("3").expression, true); };
    EXPECT_FALSE(controller.executeAuthoringCommand(before->project.revision, "Stale dialog", edit));
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision, "Schedule action", edit));
    auto views = core::SctSequenceAuthoring::actions(*controller.authoringState(), sequence.id); ASSERT_TRUE(views);
    EXPECT_TRUE(views.value().front().instruction.skipRefresh); ASSERT_TRUE(views.value().front().instruction.scheduledExpression);
    ASSERT_TRUE(controller.undo(f.a));
    EXPECT_FALSE(core::SctSequenceAuthoring::actions(*controller.authoringState(), sequence.id).value().front().instruction.scheduledExpression);
    ASSERT_TRUE(controller.redo(f.a)); controller.closeAll();
    ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&] { return !controller.isSaving(f.a); }));
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace);
    const auto restored = reopened.authoringState(); ASSERT_TRUE(restored);
    EXPECT_EQ(restored->project.sequences.front().actions.front().id, sequence.actions.front().id);
    EXPECT_TRUE(core::SctSequenceAuthoring::actions(*restored, sequence.id).value().front().instruction.skipRefresh);
}
TEST(AuthoringControllerTest, ModuleCommandsShareEditorHistoryAndSurviveContainerReopen) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    const auto before = controller.authoringState(); ASSERT_TRUE(before);
    const auto script = before->project.scripts.front().id; const auto section = before->programs.front()->document().sections.front().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(before->project.revision, "Extract", [&](const auto& state) {
        return core::SctModuleAuthoring::extract(state, script, {section}, "Opening");
    }));
    const auto module = controller.authoringState()->project.modules.front().id;
    const auto sequence = controller.authoringState()->project.sequences.front().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision, "Append", [&](const auto& state) {
        return core::SctModuleAuthoring::appendAction(state, module, sequence, 12);
    }));
    EXPECT_EQ(controller.authoringState()->project.find(sequence)->actions.size(), 2u);
    ASSERT_TRUE(controller.undo(f.a)); EXPECT_EQ(controller.authoringState()->project.find(sequence)->actions.size(), 1u);
    ASSERT_TRUE(controller.redo(f.a)); controller.closeAll();
    ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&] { return !controller.isSaving(f.a); }));
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace); ASSERT_TRUE(reopened.authoringState());
    EXPECT_EQ(reopened.authoringState()->project.modules.front().id, module);
    EXPECT_EQ(reopened.authoringState()->project.find(sequence)->actions.size(), 2u);
}

TEST(AuthoringControllerTest, EntrypointConnectionsUseSharedHistoryAndContainer) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project, f.a)); ASSERT_TRUE(waitFor([&] { return !controller.busy(); }));
    const auto before = controller.authoringState(); const auto script = before->project.scripts.front().id;
    const auto section = before->programs.front()->document().sections.front().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(before->project.revision,"Prepare response",[&](const auto& state) -> core::Result<core::SctAuthoringState> {
        auto extracted = core::SctModuleAuthoring::extract(state,script,{section},"Response"); if (!extracted) return extracted;
        return core::SctModuleAuthoring::appendAction(extracted.value(),extracted.value().project.modules.front().id,extracted.value().project.sequences.front().id,12);
    }));
    const auto state = controller.authoringState(); namespace rel = smores::script;
    rel::ResourceIdentity identity{state->project.baselines.front().datasetFingerprint.digest.toHex(),"synthetic.mld","revision","document"};
    rel::Entry entry{identity,0,{1},99,4000,"goscript",{}};
    rel::Catalog catalog{{{identity,rel::ResourceRole::Base,{entry}}}};
    ASSERT_TRUE(controller.executeAuthoringCommand(state->project.revision,"Connect trigger",[&](const auto& current) {
        return core::SctEntrypointAuthoring::createDirectHandler(current,script,entry,rel::ResourceRole::Base,current.project.ports.front().id,catalog);
    }));
    const auto id = controller.authoringState()->project.entrypoints.front().id;
    ASSERT_TRUE(controller.undo(f.a)); EXPECT_TRUE(controller.authoringState()->project.entrypoints.empty());
    ASSERT_TRUE(controller.redo(f.a)); EXPECT_EQ(controller.authoringState()->project.entrypoints.front().id,id);
    controller.closeAll(); ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&] { return !controller.isSaving(f.a); }));
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace); ASSERT_TRUE(reopened.authoringState());
    const auto& restored = reopened.authoringState()->project; ASSERT_EQ(restored.entrypoints.size(),1); EXPECT_EQ(restored.entrypoints.front().id,id);
    EXPECT_EQ(std::get<rel::Entry>(*restored.entrypoints.front().binding->provider),entry);
}

TEST(AuthoringControllerTest, LanguageTextCommandsShareHistoryPublicationAndCompressedPersistence) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project,f.a)); ASSERT_TRUE(waitFor([&]{return !controller.busy();}));
    const auto script=controller.authoringState()->project.scripts.front().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision,"Custom language",[](const auto& s){return core::SctTextAuthoring::createLanguage(s,"Mod language");}));
    const auto group=controller.authoringState()->project.languageGroups.back().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision,"Create text",[&](const auto& s){return core::SctTextAuthoring::createText(s,script,"newText",sct::SctTextKind::SctString,sct::SctTextStorage::IndexedSection,sct::SctMessage{{},{{sct::SctTextChunk{"Hello"}}}},{group},sct::kSctShiftJisByte7FEncoding);}));
    const auto text=controller.authoringState()->project.texts.back().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision,"Link text",[&](const auto& s){return core::SctTextAuthoring::link(s,script,"Greeting",{text});}));
    const auto logical=controller.authoringState()->project.logicalTexts.back().id;
    ASSERT_TRUE(controller.deleteTextEntity(f.a,controller.authoringState()->project.find(text)->target));
    EXPECT_EQ(controller.authoringState()->project.find(text),nullptr);
    EXPECT_TRUE(controller.authoringState()->project.find(logical)->variants.empty());
    ASSERT_TRUE(controller.undo(f.a)); ASSERT_NE(controller.authoringState()->project.find(text),nullptr);
    EXPECT_EQ(controller.authoringState()->project.find(logical)->variants,std::vector{text});
    ASSERT_TRUE(controller.redo(f.a)); EXPECT_EQ(controller.authoringState()->project.find(text),nullptr); ASSERT_TRUE(controller.undo(f.a));
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision,"Edit text",[&](const auto& s){return core::SctTextAuthoring::edit(s,{{text,sct::SctMessage{{},{{sct::SctTextChunk{"Changed"}}}}}},sct::kSctShiftJisByte7FEncoding);}));
    ASSERT_TRUE(controller.undo(f.a)); ASSERT_TRUE(controller.redo(f.a));
    const auto captured=controller.authoringState();
    const auto coreOutput=core::SctAuthoringMaterializer::materialize({std::make_shared<const core::SctAuthoringProject>(captured->project),captured->programs,{script}}); ASSERT_TRUE(coreOutput.succeeded());
    bool finished=false,success=false; QObject::connect(&controller,&qt::SctDocumentController::publicationCompleted,&controller,[&](const QString&,bool ok,bool,const QString&,bool){finished=true;success=ok;});
    ASSERT_TRUE(controller.exportDocument(*f.project,f.a,{sct::SctPlatform::GameCube,sct::kSctShiftJisByte7FEncoding,sct::SctDocumentOutputByteOrder::BigEndian,sct::SctDocumentOutputWrapper::Raw},f.root/"language.sct",false,{}));
    ASSERT_TRUE(waitFor([&]{return finished;})); ASSERT_TRUE(success); EXPECT_EQ(controller.lastPublication(f.a)->outputDigest,coreOutput.scripts[0].prepared->digest);
    controller.closeAll(); ASSERT_TRUE(controller.saveDocument(f.a)); ASSERT_TRUE(waitFor([&]{return !controller.isSaving(f.a);}));
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace); ASSERT_TRUE(reopened.authoringState());
    const auto restored=reopened.authoringState(); ASSERT_NE(restored->project.find(text),nullptr);
    EXPECT_EQ(restored->project.find(text)->languages,std::vector{group}); EXPECT_EQ(restored->project.find(group)->name,"Mod language");
    EXPECT_EQ(restored->project.find(text)->target,captured->project.find(text)->target);
    const auto output=core::SctAuthoringMaterializer::materialize({std::make_shared<const core::SctAuthoringProject>(restored->project),restored->programs,{script}});
    ASSERT_TRUE(output.succeeded()); EXPECT_EQ(output.scripts[0].prepared->digest,coreOutput.scripts[0].prepared->digest);
}

TEST(AuthoringControllerTest, DisabledNamedEntryUsesSameLoweringAsCoreWithoutMldContext) {
    Fixture f; qt::SctDocumentController controller; controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project,f.a)); ASSERT_TRUE(waitFor([&]{return !controller.busy();}));
    const auto before=controller.authoringState(); const auto script=before->project.scripts.front().id;
    const auto section=before->programs.front()->document().sections.front().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(before->project.revision,"Declare entry",[&](const auto& s){return core::SctEntrypointAuthoring::declareNamed(s,script,section);}));
    const auto id=controller.authoringState()->project.entrypoints.back().id;
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision,"Disable entry",[&](const auto& s){return core::SctEntrypointAuthoring::setEnabled(s,id,false);}));
    smores::script::Resource absent{{"optional","missing/fiel32z.mld","revision","document"},smores::script::ResourceRole::StreamedTile,{}};
    ASSERT_TRUE(controller.executeAuthoringCommand(controller.authoringState()->project.revision,"Optional missing MLD",[&](const auto& s){return core::SctEntrypointAuthoring::associateResource(s,script,absent);}));
    ASSERT_TRUE(waitFor([&]{return !controller.busy();}));
    const auto captured=controller.authoringState();
    const auto coreOutput=core::SctAuthoringMaterializer::materialize({std::make_shared<const core::SctAuthoringProject>(captured->project),captured->programs,{script}}); ASSERT_TRUE(coreOutput.succeeded());
    bool finished=false,success=false; QObject::connect(&controller,&qt::SctDocumentController::publicationCompleted,&controller,[&](const QString&,bool ok,bool,const QString&,bool){finished=true;success=ok;});
    ASSERT_TRUE(controller.exportDocument(*f.project,f.a,{sct::SctPlatform::GameCube,sct::kSctShiftJisByte7FEncoding,sct::SctDocumentOutputByteOrder::BigEndian,sct::SctDocumentOutputWrapper::Raw},f.root/"disabled.sct",false,{}));
    ASSERT_TRUE(waitFor([&]{return finished;})); ASSERT_TRUE(success);
    EXPECT_EQ(controller.lastPublication(f.a)->outputDigest,coreOutput.scripts[0].prepared->digest);
    const auto published=controller.lastPublication(f.a);
    const auto& generated=published->activationRealization;
    ASSERT_TRUE(generated); EXPECT_EQ(generated->key.project,captured->project.id);
    EXPECT_EQ(generated->key.revision,captured->project.revision);
    ASSERT_EQ(generated->records.size(),1); EXPECT_EQ(generated->records.front().entity,core::SctAuthoringEntityId{id});
    EXPECT_GT(generated->records.front().locations.size(),2);
    ASSERT_TRUE(controller.undo(f.a)); ASSERT_TRUE(controller.undo(f.a)); EXPECT_TRUE(controller.authoringState()->project.find(id)->enabled);
    ASSERT_TRUE(controller.redo(f.a)); EXPECT_FALSE(controller.authoringState()->project.find(id)->enabled);
    controller.closeAll(); ASSERT_TRUE(controller.saveDocument(f.a));
    ASSERT_TRUE(waitFor([&]{return !controller.isSaving(f.a);}));
    qt::SctDocumentController reopened; reopened.setWorkspace(f.workspace); ASSERT_TRUE(reopened.authoringState());
    const auto* restored=reopened.authoringState()->project.find(id); ASSERT_NE(restored,nullptr);
    EXPECT_FALSE(restored->enabled); EXPECT_TRUE(std::holds_alternative<core::SctNamedActivation>(restored->activation));
}

}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(AuthoringControllerTest, InterpretationMetadataPersistsThroughControllerHistory) {
    Fixture f; qt::SctDocumentController controller;controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project,f.a));ASSERT_TRUE(waitFor([&]{return !controller.busy();}));
    auto state=controller.authoringState();ASSERT_TRUE(state);const auto script=state->project.scripts.front().id;
    const auto target=core::SctInterpretation::siteTarget(state->project,script,site(controller,f.a));
    ASSERT_TRUE(controller.executeAuthoringCommand(state->project.revision,"Name interaction",[&](const auto& s){return core::SctInterpretation::setName(s,target,"Chest preparation");}));
    const auto id=controller.authoringState()->project.meanings.front().id;
    EXPECT_EQ(value(controller,f.a),core::SctLiteralConstant::fromExpression(sct::SctExpressionFactory::encodedDecimalLiteral(12)));
    ASSERT_TRUE(controller.undo(f.a));EXPECT_TRUE(controller.authoringState()->project.meanings.empty());ASSERT_TRUE(controller.redo(f.a));
    ASSERT_TRUE(controller.saveDocument(f.a));ASSERT_TRUE(waitFor([&]{return !controller.isSaving(f.a);}));
    qt::SctDocumentController reopened;reopened.setWorkspace(f.workspace);ASSERT_TRUE(reopened.openDocument(*f.project,f.a));ASSERT_TRUE(waitFor([&]{return !reopened.busy();}));
    ASSERT_EQ(reopened.authoringState()->project.meanings.size(),1u);EXPECT_EQ(reopened.authoringState()->project.meanings.front().id,id);
    EXPECT_EQ(core::SctInterpretation::label(*reopened.authoringState(),target,"raw"),"Chest preparation");
}

TEST(AuthoringControllerTest, StoryLayoutIsIndependentAndConcurrentSaveRetainsNewerLayoutDirtyState) {
    Fixture f;qt::SctDocumentController controller;controller.setWorkspace(f.workspace);
    ASSERT_TRUE(controller.openDocument(*f.project,f.a));ASSERT_TRUE(waitFor([&]{return !controller.busy();}));
    ASSERT_TRUE(controller.saveDocument(f.a));ASSERT_TRUE(waitFor([&]{return !controller.isSaving(f.a);}));
    const auto state=controller.authoringState();const auto encoded=core::SctAuthoringCodec::encode(state->project).value();
    auto presentation=controller.authoringPresentation();
    auto target=core::SctInterpretation::siteTarget(state->project,state->project.scripts.front().id,site(controller,f.a));
    presentation.storyPlacements.push_back({target,12,34});
    ASSERT_TRUE(controller.setAuthoringPresentation(presentation));EXPECT_TRUE(controller.projectDirty());EXPECT_TRUE(controller.isDirty(f.a));
    EXPECT_EQ(core::SctAuthoringCodec::encode(controller.authoringState()->project).value(),encoded);
    ASSERT_TRUE(controller.saveDocument(f.a));presentation.storyPlacements.front().x=99;
    ASSERT_TRUE(controller.setAuthoringPresentation(presentation));ASSERT_TRUE(waitFor([&]{return !controller.isSaving(f.a);}));
    EXPECT_TRUE(controller.projectDirty());
    qt::SctDocumentController snapshot;snapshot.setWorkspace(f.workspace);EXPECT_EQ(snapshot.authoringPresentation().storyPlacements.front().x,12);
    ASSERT_TRUE(controller.saveDocument(f.a));ASSERT_TRUE(waitFor([&]{return !controller.isSaving(f.a);}));EXPECT_FALSE(controller.projectDirty());
    qt::SctDocumentController reopened;reopened.setWorkspace(f.workspace);EXPECT_EQ(reopened.authoringPresentation().storyPlacements.front().x,99);
    EXPECT_EQ(core::SctAuthoringCodec::encode(reopened.authoringState()->project).value(),encoded);
}
