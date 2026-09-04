#include "SalsaCore/Sct/SctMerge.h"
#include "SalsaCore/Sct/SctPatchRebase.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>

namespace salsa::core {
namespace {
using namespace spice::sct;

[[nodiscard]] Sha256Digest digest(const std::string_view value) {
    return sha256(std::as_bytes(std::span{value.data(), value.size()})).value();
}

[[nodiscard]] AssetLocator locator() {
    return AssetLocator::fromRelativePath("scripts/merge.sct").value();
}

[[nodiscard]] SctDocument document(const std::uint64_t seed = 0) {
    SctDocumentBuilder builder;
    for (std::uint64_t index = 0; index < seed; ++index) {
        (void)builder.allocateSectionId();
        (void)builder.allocateInstructionId();
        (void)builder.allocateFooterEntryId();
    }
    SctScriptSectionContent script;
    for (const std::uint16_t opcode : {9u, 125u, 12u}) {
        const auto draft = SctInstructionFactory::createDraft({opcode});
        EXPECT_TRUE(draft.draft.has_value());
        const auto built = SctInstructionFactory::materialize(
            builder.document(), *draft.draft);
        EXPECT_TRUE(built.instruction.has_value());
        script.instructions.push_back(*built.instruction);
    }
    builder.document().sections.push_back(
        {builder.allocateSectionId(), "SCRIPT", std::move(script)});
    builder.document().footerEntries.push_back({builder.allocateFooterEntryId(),
        SctTextKind::PlainString, SctPlainText{"base"}});
    return std::move(builder).finish();
}

[[nodiscard]] SctSemanticState state(const SctDocument& value) {
    return {std::make_shared<const SctDocument>(value), {}, {}};
}

[[nodiscard]] SctMergeRequest request(const SctDocument& base,
    const SctDocument& local, const SctDocument& incoming) {
    return {SctMergeMode::TrueThreeWay, locator(), SourceRevision{digest("base")},
        state(base), state(local), state(incoming), std::nullopt, "context-1"};
}

[[nodiscard]] SctDocumentInstruction& middle(SctDocument& value) {
    return std::get<SctScriptSectionContent>(value.sections.front().content)
        .instructions[1];
}

[[nodiscard]] const SctDocumentInstruction& middle(const SctDocument& value) {
    return std::get<SctScriptSectionContent>(value.sections.front().content)
        .instructions[1];
}

void replaceMiddleOpcode(SctDocument& value, const std::uint16_t opcode) {
    const auto originalId = middle(value).id;
    const auto draft = SctInstructionFactory::createDraft({opcode});
    ASSERT_TRUE(draft.draft.has_value());
    const auto built = SctInstructionFactory::materialize(value, *draft.draft);
    ASSERT_TRUE(built.instruction.has_value());
    middle(value) = *built.instruction;
    middle(value).id = originalId;
}

TEST(SctMergeTest, ComposesDisjointLocalAndIncomingChangesDeterministically) {
    const auto base = document();
    auto local = base;
    middle(local).skipRefresh = true;
    auto incoming = base;
    std::get<SctPlainText>(incoming.footerEntries.front().value).utf8 = "incoming";

    const auto built = SctMergePlanService::build(request(base, local, incoming));
    ASSERT_TRUE(built) << (built.diagnostics().empty()
        ? "no diagnostic" : built.diagnostics().front().message);
    EXPECT_TRUE(built.value().conflicts.empty());
    const auto preview = SctMergePlanService::preview(
        built.value(), {}, "context-1");
    ASSERT_EQ(preview.status, SctMergePreviewStatus::Ready);
    ASSERT_TRUE(preview.candidate.has_value());
    EXPECT_TRUE(middle(*preview.candidate->document).skipRefresh);
    EXPECT_EQ(std::get<SctPlainText>(
        preview.candidate->document->footerEntries.front().value).utf8, "incoming");

    const auto again = SctMergePlanService::build(request(base, local, incoming));
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value().id, built.value().id);
}

TEST(SctMergeTest, CombinesIdenticalIndependentOutcomesWithoutConflict) {
    const auto base = document();
    auto local = base;
    auto incoming = base;
    middle(local).skipRefresh = true;
    middle(incoming).skipRefresh = true;

    const auto built = SctMergePlanService::build(request(base, local, incoming));
    ASSERT_TRUE(built) << (built.diagnostics().empty()
        ? "no diagnostic" : built.diagnostics().front().message);
    EXPECT_TRUE(built.value().conflicts.empty());
    EXPECT_EQ(SctMergePlanService::preview(
        built.value(), {}, "context-1").status, SctMergePreviewStatus::Ready);
}

TEST(SctMergeTest, RequiresTypedResolutionForDivergentEntityChanges) {
    const auto base = document();
    auto local = base;
    auto incoming = base;
    middle(local).skipRefresh = true;
    replaceMiddleOpcode(incoming, 12u);

    const auto built = SctMergePlanService::build(request(base, local, incoming));
    ASSERT_TRUE(built);
    ASSERT_EQ(built.value().conflicts.size(), 1u);
    EXPECT_EQ(SctMergePlanService::preview(
        built.value(), {}, "context-1").status, SctMergePreviewStatus::Conflicted);

    const std::array keepLocal{SctMergeResolution{
        built.value().conflicts.front().id, SctMergeResolutionKind::KeepLocal}};
    const auto localPreview = SctMergePlanService::preview(
        built.value(), keepLocal, "context-1");
    ASSERT_EQ(localPreview.status, SctMergePreviewStatus::Ready);
    ASSERT_TRUE(localPreview.candidate);
    EXPECT_TRUE(middle(*localPreview.candidate->document).skipRefresh);
    EXPECT_EQ(middle(*localPreview.candidate->document).opcode, 125u);

    const std::array keepIncoming{SctMergeResolution{
        built.value().conflicts.front().id, SctMergeResolutionKind::AcceptIncoming}};
    const auto incomingPreview = SctMergePlanService::preview(
        built.value(), keepIncoming, "context-1");
    ASSERT_EQ(incomingPreview.status, SctMergePreviewStatus::Ready);
    ASSERT_TRUE(incomingPreview.candidate);
    EXPECT_FALSE(middle(*incomingPreview.candidate->document).skipRefresh);
    EXPECT_EQ(middle(*incomingPreview.candidate->document).opcode, 12u);
}

TEST(SctMergeTest, RejectsStaleContextAndEditedCandidateOutsideConflictGroup) {
    const auto base = document();
    auto local = base;
    auto incoming = base;
    middle(local).skipRefresh = true;
    replaceMiddleOpcode(incoming, 12u);
    const auto built = SctMergePlanService::build(request(base, local, incoming));
    ASSERT_TRUE(built);
    EXPECT_EQ(SctMergePlanService::preview(
        built.value(), {}, "different").status, SctMergePreviewStatus::Stale);

    auto escaped = incoming;
    std::get<SctPlainText>(escaped.footerEntries.front().value).utf8 = "escaped";
    const std::array edited{SctMergeResolution{built.value().conflicts.front().id,
        SctMergeResolutionKind::UseEditedCandidate, state(escaped)}};
    const auto rejected = SctMergePlanService::preview(
        built.value(), edited, "context-1");
    EXPECT_EQ(rejected.status, SctMergePreviewStatus::Invalid);
}

TEST(SctPatchRebaseTest, ReconcilesIndependentSourceLineagesAndVerifiesPatchRoundTrip) {
    const auto oldBaseline = document();
    auto local = oldBaseline;
    middle(local).skipRefresh = true;
    const auto oldPatch = SalsaScriptPatchService::diff(
        state(oldBaseline), state(local), std::nullopt);
    ASSERT_TRUE(oldPatch);

    auto newBaseline = document(10);
    std::get<SctPlainText>(newBaseline.footerEntries.front().value).utf8 = "new source";
    const auto built = SctPatchRebaseService::build({locator(),
        SourceRevision{digest("old-source")}, SourceRevision{digest("new-source")},
        state(oldBaseline), oldPatch.value(), state(newBaseline),
        "workspace-id", "context-2", {}});
    ASSERT_TRUE(built) << (built.diagnostics().empty()
        ? "no diagnostic" : built.diagnostics().front().message);
    const auto preview = SctPatchRebaseService::preview(
        built.value(), {}, "context-2");
    ASSERT_EQ(preview.merge.status, SctMergePreviewStatus::Ready);
    ASSERT_TRUE(preview.rebasedPatch.has_value());
    ASSERT_TRUE(preview.merge.candidate.has_value());
    EXPECT_TRUE(middle(*preview.merge.candidate->document).skipRefresh);
    EXPECT_EQ(std::get<SctPlainText>(
        preview.merge.candidate->document->footerEntries.front().value).utf8,
        "new source");
    EXPECT_FALSE(preview.serializedPatch.empty());
}

}  // namespace
}  // namespace salsa::core
