#include "SalsaCore/Sct/SctFragment.h"

#include "SpiceSCT/SctDocumentBuilder.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace salsa::core;
using namespace spice::sct;

struct FragmentDocument final {
    std::shared_ptr<const SctDocument> document;
    SctSectionId scriptSection;
    SctSectionId stringSection;
    SctSectionId markerSection;
    std::vector<SctInstructionId> instructions;
    SctStringId string;
};

FragmentDocument makeFragmentDocument() {
    SctDocumentBuilder builder;
    const auto scriptSection = builder.allocateSectionId();
    const auto stringSection = builder.allocateSectionId();
    const auto markerSection = builder.allocateSectionId();
    const auto first = builder.allocateInstructionId();
    const auto second = builder.allocateInstructionId();
    const auto third = builder.allocateInstructionId();
    const auto fourth = builder.allocateInstructionId();
    const auto string = builder.allocateStringId();

    SctDocumentInstruction head{first, 9};
    SctDocumentInstruction internalReference{second, 10};
    internalReference.fixedParameters.push_back({0, SctInstructionReference{third}});
    SctDocumentInstruction externalReference{third, 11};
    externalReference.fixedParameters.push_back({0, SctStringReference{string}});
    SctDocumentInstruction tail{fourth, 12};
    builder.document().sections.push_back({scriptSection, "SCRIPT",
        SctScriptSectionContent{{head, internalReference, externalReference, tail}}});
    builder.document().sections.push_back({stringSection, "MS0000001",
        SctStringSectionContent{SctDocumentString{
            string, SctPlainText{"Referenced text"}, SctTextKind::PlainString}}});
    builder.document().sections.push_back({markerSection, "STRINGS",
        SctStringGroupMarkerSectionContent{}});
    return {std::make_shared<const SctDocument>(std::move(builder).finish()),
        scriptSection, stringSection, markerSection,
        {first, second, third, fourth}, string};
}

class FragmentTemporaryDirectory final {
public:
    FragmentTemporaryDirectory() {
        static std::atomic_uint64_t next{1};
        path_ = std::filesystem::temp_directory_path()
            / ("salsa-fragment-test-" + std::to_string(next.fetch_add(1)));
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ~FragmentTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

const SctInsertInstructionAfterOperation* insertion(
    const SctFragmentPastePlan& plan, const std::size_t ordinal) {
    if (ordinal >= plan.document.operations.size()) return nullptr;
    return std::get_if<SctInsertInstructionAfterOperation>(
        &plan.document.operations[ordinal]);
}

std::vector<std::byte> jsonBytes(const nlohmann::ordered_json& value) {
    const auto text = value.dump();
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}
}  // namespace

TEST(SctFragment, CapturesOnlyContiguousPhysicalRangesAndCompleteAuthoredArms) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const SctAuthoredArm arm{SctAuthoredArmId{1},
        {source.scriptSection, source.instructions[1]}, SctStructuredArmKind::Then,
        std::nullopt, source.instructions[3], {source.instructions[2]}, {},
        SctAuthoredArmRealization::Virtual};
    const SctStructuredAuthoringState authoring(std::span{&arm, 1u});

    const std::array noncontiguous{source.instructions[0], source.instructions[2]};
    EXPECT_FALSE(SctFragmentService::captureInstructions(
        state, authoring, "source", noncontiguous));

    const std::array slicedArm{source.instructions[1], source.instructions[2]};
    EXPECT_FALSE(SctFragmentService::captureInstructions(
        state, authoring, "source", slicedArm));

    const std::array completeArm{source.instructions[1], source.instructions[2],
        source.instructions[3]};
    const auto captured = SctFragmentService::captureInstructions(
        state, authoring, "source", completeArm);
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured.value().instructions.size(), 3u);
    EXPECT_EQ(captured.value().authoredArms.size(), 1u);
    ASSERT_EQ(captured.value().dependencies.size(), 1u);
    EXPECT_EQ(captured.value().dependencies.front().target,
        SctDocumentReferenceTarget{source.string});
    EXPECT_EQ(captured.value().dependencies.front().targetNameBytes,
        std::optional<std::string>{"MS0000001"});
}

TEST(SctFragment, RoundTripsDeterministicallyAndRejectsUnsupportedSchemas) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[1], source.instructions[2]};
    const auto captured = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(captured);

    const auto first = SctFragmentCodec::serialize(captured.value());
    const auto second = SctFragmentCodec::serialize(captured.value());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());

    const auto decoded = SctFragmentCodec::deserialize(first.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().kind, SctFragmentKind::InstructionRange);
    EXPECT_EQ(decoded.value().sourceAssetIdentity, "scripts/source.sct");
    ASSERT_EQ(decoded.value().instructions.size(), 2u);
    ASSERT_EQ(decoded.value().dependencies.size(), 1u);
    EXPECT_EQ(decoded.value().instructions[0].fixedParameters[0].value.index(),
        SctDocumentParameterValue{SctInstructionReference{source.instructions[2]}}.index());

    auto unsupported = first.value();
    auto text = std::string(reinterpret_cast<const char*>(unsupported.data()),
        unsupported.size());
    const auto marker = text.find("\"schemaVersion\": 4");
    ASSERT_NE(marker, std::string::npos);
    text.replace(marker, std::string("\"schemaVersion\": 4").size(),
        "\"schemaVersion\": 9");
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    const auto rejected = SctFragmentCodec::deserialize(bytes);
    ASSERT_FALSE(rejected);
    ASSERT_FALSE(rejected.diagnostics().empty());
    EXPECT_EQ(rejected.diagnostics().front().code,
        DiagnosticCode::UnsupportedSctFragmentSchema);
}

TEST(SctFragment, MigratesSchemaTwoFooterDependencies) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[2]};
    const auto captured = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(captured);
    ASSERT_EQ(captured.value().dependencies.size(), 1u);
    auto fragment = captured.value();
    fragment.instructions.front().fixedParameters.front().value =
        SctSupplementaryTextReference{SctSupplementaryTextId{77u}};
    fragment.dependencies.front().target = SctSupplementaryTextId{77u};
    fragment.dependencies.front().expectedTarget =
        {SctReferenceTargetStorage::SupplementaryText, SctTextKind::PlainString};
    fragment.dependencies.front().targetNameBytes = "legacy footer";

    const auto current = SctFragmentCodec::serialize(fragment);
    ASSERT_TRUE(current);
    std::string legacy(reinterpret_cast<const char*>(current.value().data()),
        current.value().size());
    auto encoded = nlohmann::ordered_json::parse(legacy);
    encoded["schemaVersion"] = 2;
    encoded.erase("semanticUnits");
    legacy = encoded.dump();
    auto version = legacy.find("\"schemaVersion\":2");
    ASSERT_NE(version, std::string::npos);
    auto targetKind = legacy.find("\"supplementary-text\"");
    ASSERT_NE(targetKind, std::string::npos);
    legacy.replace(targetKind, std::string("\"supplementary-text\"").size(),
        "\"footer\"");

    const auto legacyBytes = std::as_bytes(std::span{legacy.data(), legacy.size()});
    const auto decoded = SctFragmentCodec::deserialize(legacyBytes);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.value().dependencies.size(), 1u);
    EXPECT_EQ(decoded.value().dependencies.front().target,
        SctDocumentReferenceTarget{SctSupplementaryTextId{77u}});
    EXPECT_EQ(decoded.value().dependencies.front().targetNameBytes,
        std::optional<std::string>{"legacy footer"});
    const auto migrated = SctFragmentCodec::serialize(decoded.value());
    ASSERT_TRUE(migrated);
    const std::string migratedText(
        reinterpret_cast<const char*>(migrated.value().data()), migrated.value().size());
    EXPECT_NE(migratedText.find("\"schemaVersion\": 4"), std::string::npos);
    EXPECT_NE(migratedText.find("\"supplementary-text\""), std::string::npos);
    EXPECT_EQ(migratedText.find("\"footer\""), std::string::npos);
}

TEST(SctFragment, ReadsSchemaThreeAndUpgradesItToSchemaFour) {
    const auto source = makeFragmentDocument();
    const SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[1]};
    const auto captured = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(captured);
    const auto current = SctFragmentCodec::serialize(captured.value());
    ASSERT_TRUE(current);
    auto encoded = nlohmann::ordered_json::parse(
        reinterpret_cast<const char*>(current.value().data()),
        reinterpret_cast<const char*>(current.value().data()) + current.value().size());
    encoded["schemaVersion"] = 3;
    encoded.erase("semanticUnits");

    const auto decoded = SctFragmentCodec::deserialize(jsonBytes(encoded));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().kind, SctFragmentKind::InstructionRange);
    const auto upgraded = SctFragmentCodec::serialize(decoded.value());
    ASSERT_TRUE(upgraded);
    const std::string upgradedText(
        reinterpret_cast<const char*>(upgraded.value().data()), upgraded.value().size());
    EXPECT_NE(upgradedText.find("\"schemaVersion\": 4"), std::string::npos);
    EXPECT_NE(upgradedText.find("\"semanticUnits\": []"), std::string::npos);
}

TEST(SctFragment, RoundTripsSchemaFourSemanticHierarchyAndRejectsMalformedUnits) {
    const auto source = makeFragmentDocument();
    const SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[1], source.instructions[2]};
    const auto captured = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(captured);
    auto fragment = captured.value();
    fragment.kind = SctFragmentKind::SemanticUnits;
    SctSemanticFragmentUnit arm;
    arm.kind = SctSemanticNodeKind::Arm;
    arm.instructions = {source.instructions[1], source.instructions[2]};
    arm.armKind = SctStructuredArmKind::Then;
    arm.entryInstruction = source.instructions[1];
    SctSemanticFragmentUnit region;
    region.kind = SctSemanticNodeKind::Region;
    region.instructions = arm.instructions;
    region.regionKind = SctStructuredRegionKind::If;
    region.entryInstruction = source.instructions[1];
    region.continuationInstruction = source.instructions[2];
    region.children.push_back(arm);
    fragment.semanticUnits.push_back(region);

    const auto encoded = SctFragmentCodec::serialize(fragment);
    ASSERT_TRUE(encoded);
    const auto decoded = SctFragmentCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().kind, SctFragmentKind::SemanticUnits);
    EXPECT_EQ(decoded.value().semanticUnits, fragment.semanticUnits);

    auto malformed = nlohmann::ordered_json::parse(
        reinterpret_cast<const char*>(encoded.value().data()),
        reinterpret_cast<const char*>(encoded.value().data()) + encoded.value().size());
    malformed["semanticUnits"][0]["unknown"] = true;
    EXPECT_FALSE(SctFragmentCodec::deserialize(jsonBytes(malformed)));
    malformed.erase("semanticUnits");
    malformed["semanticUnits"] = nlohmann::ordered_json::array(
        {{{"kind", 255u}, {"instructions", nlohmann::ordered_json::array({
            source.instructions[1].value()})}, {"regionKind", nullptr},
            {"armKind", nullptr}, {"caseValue", nullptr},
            {"entryInstruction", nullptr}, {"continuationInstruction", nullptr},
            {"children", nlohmann::ordered_json::array()}}});
    EXPECT_FALSE(SctFragmentCodec::deserialize(jsonBytes(malformed)));
}

TEST(SctSnippetCodec, KeepsSchemaOneEnvelopeWithSchemaFourSemanticFragment) {
    const auto source = makeFragmentDocument();
    const SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[1]};
    auto fragment = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(fragment);
    fragment.value().kind = SctFragmentKind::SemanticUnits;
    SctSemanticFragmentUnit unit;
    unit.kind = SctSemanticNodeKind::Instruction;
    unit.instructions.assign(selected.begin(), selected.end());
    fragment.value().semanticUnits.push_back(std::move(unit));
    const auto encoded = SctSnippetCodec::serialize(
        {"Semantic", "Reusable", fragment.value()});
    ASSERT_TRUE(encoded);
    const std::string text(reinterpret_cast<const char*>(encoded.value().data()),
        encoded.value().size());
    EXPECT_NE(text.find("\"schemaVersion\": 1"), std::string::npos);
    const auto decoded = SctSnippetCodec::deserialize(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().fragment.semanticUnits,
        fragment.value().semanticUnits);
    const auto embedded = SctFragmentCodec::serialize(decoded.value().fragment);
    ASSERT_TRUE(embedded);
    const std::string embeddedText(
        reinterpret_cast<const char*>(embedded.value().data()), embedded.value().size());
    EXPECT_NE(embeddedText.find("\"schemaVersion\": 4"), std::string::npos);
}

TEST(SctFragment, PasteRemapsInternalReferencesAndPreservesSameDocumentDependencies) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[1], source.instructions[2]};
    const auto fragment = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(fragment);

    const auto plan = SctFragmentService::planPaste(state, authoring,
        "scripts/source.sct", fragment.value(),
        SctFragmentPasteDestination{source.instructions[0]});
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan.value().document.operations.size(), 2u);
    const auto* first = insertion(plan.value(), 0);
    const auto* second = insertion(plan.value(), 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const auto* internal = std::get_if<SctInstructionReference>(
        &first->instruction.fixedParameters[0].value);
    ASSERT_NE(internal, nullptr);
    EXPECT_EQ(internal->target, second->instruction.id);
    const auto* external = std::get_if<SctStringReference>(
        &second->instruction.fixedParameters[0].value);
    ASSERT_NE(external, nullptr);
    EXPECT_EQ(external->target, source.string);
    EXPECT_EQ(plan.value().unboundReferenceCount, 0u);
    EXPECT_TRUE(plan.value().authoring.unboundReferences.empty());
}

TEST(SctFragment, CrossDocumentPasteCreatesUnresolvedValuesWithDurableOrigin) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[2]};
    const auto fragment = SctFragmentService::captureInstructions(
        state, authoring, "scripts/source.sct", selected);
    ASSERT_TRUE(fragment);

    const auto plan = SctFragmentService::planPaste(state, authoring,
        "scripts/destination.sct", fragment.value(),
        SctFragmentPasteDestination{source.instructions[0]});
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan.value().unboundReferenceCount, 1u);
    ASSERT_EQ(plan.value().authoring.unboundReferences.size(), 1u);
    const auto* inserted = insertion(plan.value(), 0);
    ASSERT_NE(inserted, nullptr);
    EXPECT_NE(std::get_if<SctUnresolvedReferenceValue>(
        &inserted->instruction.fixedParameters[0].value), nullptr);
    const auto& origin = *plan.value().authoring.unboundReferences.front().after;
    EXPECT_EQ(origin.site.instruction, inserted->instruction.id);
    EXPECT_EQ(origin.sourceAssetIdentity, "scripts/source.sct");
    EXPECT_EQ(origin.sourceTarget, SctDocumentReferenceTarget{source.string});
    EXPECT_EQ(origin.sourceTargetNameBytes,
        std::optional<std::string>{"MS0000001"});

    auto documentApplied = state.apply(plan.value().document);
    ASSERT_TRUE(documentApplied.succeeded());
    auto authoringCopy = authoring;
    auto provenanceApplied = authoringCopy.apply(plan.value().authoring);
    ASSERT_TRUE(provenanceApplied.succeeded());
    ASSERT_EQ(authoringCopy.unboundReferences().size(), 1u);
    EXPECT_EQ(authoringCopy.unboundReferences().front(), origin);
}

TEST(SctFragment, CapturesSupportedSectionKindsAndSuggestsDeterministicNames) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const std::array selected{source.stringSection, source.markerSection};
    const std::array annotations{SctEntityAnnotation{
        {SctAuthoringTargetKind::String, source.string.value()}, "Translator note",
        "Review", 0x123456u}};
    const std::array folders{SctSectionFolder{{1}, std::nullopt, "Text",
        {source.stringSection, source.markerSection}, "Folder note", "Folder mark",
        0x654321u}};
    const std::array aliases{SctVariableAlias{
        {salsa::core::SctVariableKind::Byte, 3}, "NotCopied"}};
    const SctStructuredAuthoringState authoring({}, {}, aliases, annotations, folders);
    const auto fragment = SctFragmentService::captureSections(
        state, authoring, "source", selected);
    ASSERT_TRUE(fragment);
    ASSERT_EQ(fragment.value().sections.size(), 2u);
    ASSERT_EQ(fragment.value().annotations.size(), 1u);
    EXPECT_EQ(fragment.value().annotations.front().note, "Translator note");
    EXPECT_FALSE(fragment.value().annotations.front().bookmarkLabel.has_value());
    ASSERT_EQ(fragment.value().folders.size(), 1u);
    EXPECT_EQ(fragment.value().folders.front().note, "Folder note");
    EXPECT_FALSE(fragment.value().folders.front().bookmarkLabel.has_value());

    const auto names = SctFragmentService::suggestSectionNames(state, fragment.value());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "MS0000001_COPY");
    EXPECT_EQ(names[1], "STRINGS_COPY");
    const auto repeated = SctFragmentService::suggestSectionNames(state,
        SctSemanticFragment{SctFragmentKind::SectionRange, {}, {},
            {fragment.value().sections[0], fragment.value().sections[0]}});
    ASSERT_EQ(repeated.size(), 2u);
    EXPECT_EQ(repeated[0], "MS0000001_COPY");
    EXPECT_EQ(repeated[1], "MS0000001_COPY2");
}

TEST(SctSnippetStore, PersistsListsReplacesAndRemovesVersionedSnippets) {
    const auto source = makeFragmentDocument();
    SctWorkingState state(source.document);
    const SctStructuredAuthoringState authoring;
    const std::array selected{source.instructions[1]};
    const auto fragment = SctFragmentService::captureInstructions(
        state, authoring, "source", selected);
    ASSERT_TRUE(fragment);

    FragmentTemporaryDirectory temporary;
    const SctSnippetStore store(temporary.path());
    EXPECT_TRUE(store.save({"Reusable branch", "First description", fragment.value()}));
    EXPECT_TRUE(store.save({"Reusable branch", "Updated description", fragment.value()}));
    EXPECT_TRUE(store.save({"Another", {}, fragment.value()}));
    const auto loaded = store.loadAll();
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded.value().size(), 2u);
    EXPECT_EQ(loaded.value()[0].name, "Another");
    EXPECT_EQ(loaded.value()[1].name, "Reusable branch");
    EXPECT_EQ(loaded.value()[1].description, "Updated description");
    EXPECT_TRUE(store.remove("Reusable branch"));
    const auto remaining = store.loadAll();
    ASSERT_TRUE(remaining);
    ASSERT_EQ(remaining.value().size(), 1u);
    EXPECT_EQ(remaining.value().front().name, "Another");
}
