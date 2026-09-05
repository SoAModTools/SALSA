#include "SalsaCore/Sct/SctChangePlan.h"

#include "SpiceSCT/SctDocumentBuilder.h"
#include "SpiceSCT/SctDocumentValidator.h"
#include "SpiceSCT/SctInstructionFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>

namespace salsa::core {
namespace {
using namespace spice::sct;

[[nodiscard]] Sha256Digest digest(const std::string_view value) {
    return sha256(std::as_bytes(std::span{value.data(), value.size()})).value();
}

[[nodiscard]] AssetLocator locator(const std::string_view path) {
    return AssetLocator::fromRelativePath(std::filesystem::path(path)).value();
}

[[nodiscard]] SctDocument makeDocument() {
    SctDocumentBuilder builder;
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
    builder.document().supplementaryText.push_back({builder.allocateSupplementaryTextId(),
        SctTextKind::PlainString, SctPlainText{"before"}});
    return std::move(builder).finish();
}

[[nodiscard]] SctSemanticState state(const SctDocument& document,
    std::vector<SctAuthoredArm> arms = {},
    std::vector<SctPatchedTextRepair> repairs = {}) {
    return {std::make_shared<const SctDocument>(document),
        std::move(arms), std::move(repairs)};
}

[[nodiscard]] const SctChangeUnit* unit(const SctScriptChangePlan& script,
    const SctChangeEntityKind kind, const std::uint64_t id = 0) {
    const auto found = std::ranges::find_if(script.units, [&](const auto& value) {
        if (value.entityKind != kind) return false;
        return id == 0 || (value.target && value.target->id == id);
    });
    return found == script.units.end() ? nullptr : &*found;
}

[[nodiscard]] SctScriptComparisonInput comparison(const AssetLocator& script,
    const SctDocument& before, const SctDocument& after,
    std::vector<SctAuthoredArm> beforeArms = {},
    std::vector<SctAuthoredArm> afterArms = {},
    std::vector<SctPatchedTextRepair> beforeRepairs = {},
    std::vector<SctPatchedTextRepair> afterRepairs = {}) {
    return {script, SourceRevision{digest(script.identityKey())},
        state(before, std::move(beforeArms), std::move(beforeRepairs)),
        state(after, std::move(afterArms), std::move(afterRepairs)), std::nullopt};
}

TEST(SctChangePlanTest, BuildsDeterministicMultiScriptEntityPlanAndAppliesSelection) {
    const auto firstLocator = locator("scripts/first.sct");
    const auto secondLocator = locator("scripts/second.sct");
    const auto baseline = makeDocument();
    auto firstWorking = baseline;
    auto& firstScript = std::get<SctScriptSectionContent>(
        firstWorking.sections.front().content);
    firstScript.instructions[1].skipRefresh = true;
    std::get<SctPlainText>(firstWorking.supplementaryText.front().value).utf8 = "after";
    auto secondWorking = baseline;
    std::get<SctScriptSectionContent>(secondWorking.sections.front().content)
        .instructions[1].skipRefresh = true;

    const std::array inputs{
        comparison(secondLocator, baseline, secondWorking),
        comparison(firstLocator, baseline, firstWorking),
    };
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    ASSERT_EQ(built.value().scripts.size(), 2u);
    EXPECT_EQ(built.value().scripts.front().locator, firstLocator);
    const auto builtAgain = SctChangePlanService::build(inputs);
    ASSERT_TRUE(builtAgain);
    EXPECT_EQ(built.value().id, builtAgain.value().id);

    const auto& first = built.value().scripts.front();
    const auto changedInstruction = firstScript.instructions[1].id;
    const auto* instructionUnit = unit(first, SctChangeEntityKind::Instruction,
        changedInstruction.value());
    const auto* footerUnit = unit(first, SctChangeEntityKind::SupplementaryText,
        firstWorking.supplementaryText.front().id.value());
    ASSERT_NE(instructionUnit, nullptr);
    ASSERT_NE(footerUnit, nullptr);

    SctChangeSelection selection{built.value().id, {instructionUnit->id}, {}};
    const std::array current{
        SctCurrentScriptState{firstLocator, state(baseline)},
        SctCurrentScriptState{secondLocator, state(baseline)},
    };
    const auto applied = SctChangePlanService::apply(built.value(), current, selection);
    ASSERT_EQ(applied.scripts.size(), 2u);
    const auto& firstApplied = applied.scripts.front();
    ASSERT_EQ(firstApplied.status, SctChangeApplicationStatus::Applied);
    ASSERT_TRUE(firstApplied.state.has_value());
    const auto& appliedScript = std::get<SctScriptSectionContent>(
        firstApplied.state->document->sections.front().content);
    EXPECT_TRUE(appliedScript.instructions[1].skipRefresh);
    EXPECT_EQ(std::get<SctPlainText>(
        firstApplied.state->document->supplementaryText.front().value).utf8, "before");
}

TEST(SctChangePlanTest, FullSelectionRecreatesCanonicalPatch) {
    const auto scriptLocator = locator("scripts/full.sct");
    const auto baseline = makeDocument();
    auto working = baseline;
    auto& instructions = std::get<SctScriptSectionContent>(
        working.sections.front().content).instructions;
    instructions[1].skipRefresh = true;
    std::swap(instructions[1], instructions[2]);
    std::get<SctPlainText>(working.supplementaryText.front().value).utf8 = "changed";
    const std::array inputs{comparison(scriptLocator, baseline, working)};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    auto selection = SctChangePlanService::selectAll(built.value());
    const std::array current{SctCurrentScriptState{scriptLocator, state(baseline)}};
    const auto applied = SctChangePlanService::apply(built.value(), current, selection);
    ASSERT_EQ(applied.scripts.front().status, SctChangeApplicationStatus::Applied);
    ASSERT_TRUE(applied.scripts.front().state.has_value());
    const auto reproduced = SalsaScriptPatchService::diff(state(baseline),
        *applied.scripts.front().state, std::nullopt);
    ASSERT_TRUE(reproduced);
    const auto expectedBytes = SalsaScriptPatchCodec::serialize(
        built.value().scripts.front().completePatch);
    const auto reproducedBytes = SalsaScriptPatchCodec::serialize(reproduced.value());
    ASSERT_TRUE(expectedBytes);
    ASSERT_TRUE(reproducedBytes);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(expectedBytes.value().data()),
                  expectedBytes.value().size()),
        std::string(reinterpret_cast<const char*>(reproducedBytes.value().data()),
            reproducedBytes.value().size()));
}

TEST(SctChangePlanTest, PreservesAllocatorOnlyPlanAndMergesConcurrentHighWater) {
    const auto scriptLocator = locator("scripts/allocator.sct");
    const auto baseline = makeDocument();
    auto proposed = baseline;
    (void)proposed.allocateSectionId();
    (void)proposed.allocateInstructionId();
    const std::array inputs{comparison(scriptLocator, baseline, proposed)};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    const auto* allocator = unit(built.value().scripts.front(),
        SctChangeEntityKind::AllocatorState);
    ASSERT_NE(allocator, nullptr);
    EXPECT_TRUE(allocator->selectable);

    auto currentDocument = baseline;
    (void)currentDocument.allocateInstructionId();
    (void)currentDocument.allocateInstructionId();
    (void)currentDocument.allocateSupplementaryTextId();
    const auto currentInstructionHighWater = currentDocument.nextInstructionIdValue();
    const auto currentFooterHighWater = currentDocument.nextSupplementaryTextIdValue();
    const std::array current{
        SctCurrentScriptState{scriptLocator, state(currentDocument)}};
    const auto applied = SctChangePlanService::apply(built.value(), current,
        SctChangePlanService::selectAll(built.value()));
    ASSERT_EQ(applied.scripts.front().status, SctChangeApplicationStatus::Applied);
    ASSERT_TRUE(applied.scripts.front().state.has_value());
    EXPECT_EQ(applied.scripts.front().state->document->nextSectionIdValue(),
        proposed.nextSectionIdValue());
    EXPECT_EQ(applied.scripts.front().state->document->nextInstructionIdValue(),
        currentInstructionHighWater);
    EXPECT_EQ(applied.scripts.front().state->document->nextSupplementaryTextIdValue(),
        currentFooterHighWater);
}

TEST(SctChangePlanTest, IsolatesExpectedBeforeConflictAndAppliesCleanEntity) {
    const auto scriptLocator = locator("scripts/conflict.sct");
    const auto baseline = makeDocument();
    auto planned = baseline;
    auto& plannedInstructions = std::get<SctScriptSectionContent>(
        planned.sections.front().content).instructions;
    plannedInstructions[1].skipRefresh = true;
    std::get<SctPlainText>(planned.supplementaryText.front().value).utf8 = "planned";
    const std::array inputs{comparison(scriptLocator, baseline, planned)};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    auto selection = SctChangePlanService::selectAll(built.value());

    auto currentDocument = baseline;
    std::get<SctScriptSectionContent>(currentDocument.sections.front().content)
        .instructions[1].skipRefresh = true;
    const std::array current{SctCurrentScriptState{scriptLocator, state(currentDocument)}};
    const auto applied = SctChangePlanService::apply(built.value(), current, selection);
    ASSERT_EQ(applied.scripts.front().status, SctChangeApplicationStatus::Applied);
    EXPECT_TRUE(std::ranges::any_of(applied.scripts.front().diagnostics,
        [](const auto& value) {
            return value.disposition == SctChangeDisposition::Conflict
                && value.code == "ExpectedBeforeMismatch";
        }));
    ASSERT_TRUE(applied.scripts.front().state.has_value());
    EXPECT_EQ(std::get<SctPlainText>(
        applied.scripts.front().state->document->supplementaryText.front().value).utf8,
        "planned");
    EXPECT_TRUE(std::get<SctScriptSectionContent>(
        applied.scripts.front().state->document->sections.front().content)
        .instructions[1].skipRefresh);
}

TEST(SctChangePlanTest, RequiresPerInstructionAcknowledgementForRepairState) {
    const auto scriptLocator = locator("scripts/repair.sct");
    auto baseline = makeDocument();
    auto& baselineInstructions = std::get<SctScriptSectionContent>(
        baseline.sections.front().content).instructions;
    const auto target = baselineInstructions[1].id;
    SctDocumentInstruction jump{baseline.allocateInstructionId(), 10u};
    jump.fixedParameters.push_back({0u, SctInstructionReference{target}});
    baselineInstructions.insert(baselineInstructions.begin() + 1, jump);
    ASSERT_TRUE(SctDocumentValidator::validateDocument(baseline).validDocument);
    auto working = baseline;
    auto& workingInstructions = std::get<SctScriptSectionContent>(
        working.sections.front().content).instructions;
    std::erase_if(workingInstructions, [&](const auto& value) { return value.id == target; });
    const std::array inputs{comparison(scriptLocator, baseline, working)};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    const auto* deletion = unit(built.value().scripts.front(),
        SctChangeEntityKind::Instruction, target.value());
    ASSERT_NE(deletion, nullptr);
    SctChangeSelection selection{built.value().id, {deletion->id}, {}};
    const std::array current{SctCurrentScriptState{scriptLocator, state(baseline)}};
    const auto blocked = SctChangePlanService::preview(built.value(), current, selection);
    EXPECT_EQ(blocked.scripts.front().status, SctChangeApplicationStatus::Blocked);
    selection.acknowledgedWarningIds.push_back(deletion->id);
    const auto accepted = SctChangePlanService::apply(built.value(), current, selection);
    EXPECT_EQ(accepted.scripts.front().status,
        SctChangeApplicationStatus::RepairRequired);
    ASSERT_TRUE(accepted.scripts.front().state.has_value());
    EXPECT_FALSE(SctDocumentValidator::validateDocument(
        *accepted.scripts.front().state->document).validDocument);
}

TEST(SctChangePlanTest, PreservesOpaqueDifferenceAfterExplicitAcknowledgement) {
    const auto scriptLocator = locator("scripts/opaque.sct");
    auto baseline = makeDocument();
    auto& instructions = std::get<SctScriptSectionContent>(
        baseline.sections.front().content).instructions;
    baseline.opaqueAttachments.push_back({baseline.allocateOpaqueAttachmentId(),
        {0xaa}, instructions[1].id, SctOpaquePlacement::After, std::nullopt, 1,
        SctOpaqueRelocationSupport::Relocatable, SctOpaqueReason::Gap});
    auto working = baseline;
    working.opaqueAttachments.front().bytes = {0xbb};
    std::get<SctScriptSectionContent>(working.sections.front().content)
        .instructions[1].skipRefresh = true;
    const std::array inputs{comparison(scriptLocator, baseline, working)};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    const auto& script = built.value().scripts.front();
    const auto* opaque = unit(script, SctChangeEntityKind::OpaquePreservation);
    const auto* instruction = unit(script, SctChangeEntityKind::Instruction,
        instructions[1].id.value());
    ASSERT_NE(opaque, nullptr);
    ASSERT_NE(instruction, nullptr);
    EXPECT_FALSE(opaque->selectable);
    EXPECT_TRUE(opaque->acknowledgementRequired);
    SctChangeSelection selection{built.value().id, {instruction->id}, {}};
    const std::array current{SctCurrentScriptState{scriptLocator, state(baseline)}};
    EXPECT_EQ(SctChangePlanService::apply(built.value(), current, selection)
        .scripts.front().status, SctChangeApplicationStatus::Blocked);
    selection.acknowledgedWarningIds.push_back(opaque->id);
    selection.selectedUnitIds.push_back(opaque->id);
    const auto unselectable = SctChangePlanService::apply(
        built.value(), current, selection);
    EXPECT_EQ(unselectable.scripts.front().status,
        SctChangeApplicationStatus::Blocked);
    EXPECT_TRUE(std::ranges::any_of(unselectable.scripts.front().diagnostics,
        [](const auto& value) { return value.code == "UnselectableChangeUnit"; }));
    selection.selectedUnitIds.pop_back();
    const auto accepted = SctChangePlanService::apply(built.value(), current, selection);
    ASSERT_EQ(accepted.scripts.front().status, SctChangeApplicationStatus::Applied);
    ASSERT_TRUE(accepted.scripts.front().state.has_value());
    EXPECT_EQ(accepted.scripts.front().state->document->opaqueAttachments.front().bytes,
        (std::vector<std::uint8_t>{0xaa}));
}

TEST(SctChangePlanTest, RejectsStaleAndUnknownSelectionsAndFormatsDiagnostics) {
    const auto scriptLocator = locator("scripts/stale.sct");
    const auto baseline = makeDocument();
    auto working = baseline;
    std::get<SctScriptSectionContent>(working.sections.front().content)
        .instructions.front().skipRefresh = true;
    const std::array inputs{comparison(scriptLocator, baseline, working)};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    const std::array current{SctCurrentScriptState{scriptLocator, state(baseline)}};

    auto selection = SctChangePlanService::selectAll(built.value());
    selection.planId = "stale";
    const auto stale = SctChangePlanService::preview(built.value(), current, selection);
    ASSERT_EQ(stale.scripts.front().status, SctChangeApplicationStatus::Blocked);
    ASSERT_FALSE(stale.scripts.front().diagnostics.empty());
    EXPECT_EQ(stale.scripts.front().diagnostics.front().code, "StalePlan");
    EXPECT_NE(SctChangeDiagnosticFormatter::format(
        stale.scripts.front().diagnostics.front()).find("[conflict] StalePlan:"),
        std::string::npos);

    selection.planId = built.value().id;
    selection.selectedUnitIds.push_back("unknown-unit");
    const auto unknown = SctChangePlanService::preview(built.value(), current, selection);
    ASSERT_EQ(unknown.scripts.front().status, SctChangeApplicationStatus::Blocked);
    ASSERT_FALSE(unknown.scripts.front().diagnostics.empty());
    EXPECT_EQ(unknown.scripts.front().diagnostics.front().code,
        "UnknownChangeUnit");
}

TEST(SctChangePlanTest, CouplesTextRepairProvenanceToSemanticText) {
    const auto scriptLocator = locator("scripts/text.sct");
    auto baseline = makeDocument();
    const std::vector<std::uint8_t> opaqueBytes{'o', 'p', 'a', 'q', 'u', 'e'};
    const auto textId = baseline.supplementaryText.front().id;
    baseline.supplementaryText.front().value = SctOpaqueText{opaqueBytes};
    auto working = baseline;
    working.supplementaryText.front().value = SctPlainText{"repaired"};
    const auto opaqueDigest = sha256(std::as_bytes(std::span{opaqueBytes})).value();
    const SctPatchedTextRepair repair{textId,
        {kSctShiftJisByte7FEncoding, SctKnownTextConvention::ShiftJisByte7F,
            opaqueDigest.toHex()}};
    const std::array inputs{comparison(scriptLocator, baseline, working,
        {}, {}, {}, {repair})};
    const auto built = SctChangePlanService::build(inputs);
    ASSERT_TRUE(built);
    const auto& script = built.value().scripts.front();
    const auto* text = unit(script, SctChangeEntityKind::SupplementaryText, textId.value());
    const auto* metadata = unit(script, SctChangeEntityKind::TextRepair);
    ASSERT_NE(text, nullptr);
    ASSERT_NE(metadata, nullptr);
    EXPECT_TRUE(std::ranges::find(text->coupledUnitIds, metadata->id)
        != text->coupledUnitIds.end());
    SctChangeSelection selection{built.value().id, {text->id}, {}};
    const std::array current{SctCurrentScriptState{scriptLocator, state(baseline)}};
    const auto applied = SctChangePlanService::apply(built.value(), current, selection);
    ASSERT_EQ(applied.scripts.front().status, SctChangeApplicationStatus::Applied);
    ASSERT_TRUE(applied.scripts.front().state.has_value());
    EXPECT_EQ(applied.scripts.front().state->textRepairs,
        (std::vector<SctPatchedTextRepair>{repair}));
}

TEST(SctChangePlanTest, SelectsAndAppliesUnboundReferenceProvenance) {
    const auto asset = locator("scripts/unbound.sct");
    const auto baseline = makeDocument();
    auto before = state(baseline);
    auto after = state(baseline);
    const auto instruction = std::get<SctScriptSectionContent>(
        baseline.sections.front().content).instructions[1].id;
    const SctParameterSite site{instruction, {0u, std::nullopt}};
    after.unboundReferences.push_back({site, "scripts/source.sct",
        SctStringId{27u}, std::string{"MS0000027"}});
    const std::array input{SctScriptComparisonInput{asset,
        SourceRevision{digest("unbound")}, before, after, std::nullopt}};
    const auto built = SctChangePlanService::build(input);
    ASSERT_TRUE(built);
    ASSERT_EQ(built.value().scripts.size(), 1u);
    const auto* metadata = unit(built.value().scripts.front(),
        SctChangeEntityKind::UnboundReference, instruction.value());
    ASSERT_NE(metadata, nullptr);

    const SctChangeSelection selection{built.value().id, {metadata->id}, {}};
    const std::array current{SctCurrentScriptState{asset, before}};
    const auto applied = SctChangePlanService::apply(
        built.value(), current, selection);
    ASSERT_EQ(applied.scripts.size(), 1u);
    ASSERT_TRUE(applied.scripts.front().state.has_value());
    ASSERT_EQ(applied.scripts.front().state->unboundReferences.size(), 1u);
    EXPECT_EQ(applied.scripts.front().state->unboundReferences.front(),
        after.unboundReferences.front());
}

}  // namespace
}  // namespace salsa::core
