#include "SalsaCore/Authoring/SctAuthoringCodec.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <type_traits>

namespace salsa::core {
namespace {
using Json = nlohmann::ordered_json;
constexpr auto ProjectUuid = "e44499b5-dcf0-449a-9815-5bdf558eface";
constexpr auto DocumentA = "a44499b5-dcf0-449a-9815-5bdf558eface";
constexpr auto DocumentB = "b44499b5-dcf0-449a-9815-5bdf558eface";
constexpr auto RealizationUuid = "c44499b5-dcf0-449a-9815-5bdf558eface";
static_assert(!std::is_convertible_v<SctModuleId, SctScriptId>);
static_assert(!std::is_constructible_v<SctPortOwner, SctScriptId>);
static_assert(!std::is_convertible_v<SctAuthoringProjectId, SctImportedDocumentId>);

template<class Id> Id allocate(SctAuthoringProject& p) {
    auto result = p.allocate<Id>(); EXPECT_TRUE(result); return result.value();
}
SctAuthoringProject fixture() {
    SctAuthoringProject p; p.id.value = ProjectUuid;
    for (int index = 0; index < 2; ++index) {
        const auto baseline = allocate<SctBaselineId>(p);
        const SctImportedDocumentId document{index == 0 ? DocumentA : DocumentB};
        const auto source = AssetLocator::fromRelativePath(index == 0 ? L"field/synthetic-a.sct" : L"field/synthetic-b.sct").value();
        p.baselines.push_back({baseline, {Sha256Digest{{}}}, {source, 128, {Sha256Digest{{}}}}, document,
            spice::sct::SctKnownTextConvention::ShiftJisByte7F, {{SctEvidenceKind::SourceObservation, "Synthetic import evidence"}}});
        const auto script = allocate<SctScriptId>(p);
        p.scripts.push_back({script, index == 0 ? "First" : "Second", baseline, GamePlatform::GameCube, GameRegion::Europe, {}, {}});
        const auto module = allocate<SctModuleId>(p);
        p.modules.push_back({module, script, "Cutscene", {}, {{SctEvidenceKind::Inference, "Candidate cutscene"}}});
        const auto entry = allocate<SctEntrypointId>(p);
        p.entrypoints.push_back({entry, script, "Entry", {}, {}});
        const auto sourcePort = allocate<SctPortId>(p), destinationPort = allocate<SctPortId>(p);
        p.ports.push_back({sourcePort, entry, "Activate"}); p.ports.push_back({destinationPort, module, "Start"});
        p.connections.push_back({allocate<SctConnectionId>(p), sourcePort, destinationPort, {}});
        const auto content = allocate<SctContentId>(p);
        p.contents.push_back({content, module, {baseline, document, SctWholeDocument{}}, {}});
        p.modules.back().contentUses.push_back(content);
        p.entrypoints.back().contentUses.push_back(content);
    }
    return p;
}
std::string encoded(const SctAuthoringProject& p) {
    auto result = SctAuthoringCodec::encode(p);
    EXPECT_TRUE(result) << (result.diagnostics().empty() ? "" : result.diagnostics().front().message);
    return result ? result.value() : "";
}
TEST(SctAuthoringProjectTest, ConstructsUniqueProjectsAndImportedScriptOwnership) {
    const auto first = SctAuthoringProject::create(), second = SctAuthoringProject::create();
    ASSERT_TRUE(first); ASSERT_TRUE(second); EXPECT_TRUE(first.value().id.valid());
    EXPECT_NE(first.value().id, second.value().id);
    auto p = fixture(); EXPECT_FALSE(hasErrors(p.validate()));
    EXPECT_EQ(p.modules[0].name, p.modules[1].name); EXPECT_NE(p.modules[0].id, p.modules[1].id);
    EXPECT_EQ(p.effectiveScript(p.contents[0].owner), p.scripts[0].id);
    EXPECT_EQ(p.find(p.modules[1].id)->script, p.scripts[1].id);
    EXPECT_EQ(p.find(SctContentId{999}), nullptr);
    EXPECT_EQ(p.find(p.baselines[0].id), &p.baselines[0]);
}
TEST(SctAuthoringProjectTest, RenameAndRoundTripRetainIdsAllocatorAndLegacyOrigin) {
    auto p = fixture();
    p.scripts[0].legacyOrigin = SctLegacyOrigin{"capsule-reference", "legacy-script-key", 7};
    const auto moduleId = p.modules[0].id, otherId = p.modules[1].id;
    p.modules[0].name = "Renamed scene";
    const auto bytes = encoded(p); auto restored = SctAuthoringCodec::decode(bytes);
    ASSERT_TRUE(restored); EXPECT_EQ(encoded(restored.value()), bytes);
    EXPECT_EQ(restored.value().id, p.id); EXPECT_EQ(restored.value().revision, p.revision);
    EXPECT_EQ(restored.value().find(moduleId)->name, "Renamed scene");
    EXPECT_EQ(restored.value().find(otherId)->name, "Cutscene");
    ASSERT_TRUE(restored.value().scripts[0].legacyOrigin);
    EXPECT_EQ(restored.value().scripts[0].legacyOrigin->scriptKey, "legacy-script-key");
    EXPECT_EQ(restored.value().scripts[0].legacyOrigin->scriptOrdinal, 7);
    EXPECT_EQ(restored.value().scripts[0].legacyOrigin->capsuleId, "capsule-reference");
    EXPECT_EQ(allocate<SctModuleId>(restored.value()).value, p.nextEntityId);
}
TEST(SctAuthoringProjectTest, RejectsBrokenOwnershipAndBindingsBeforeEncoding) {
    const std::vector<std::pair<std::string, std::function<void(SctAuthoringProject&)>>> cases{
        {"missing baseline", [](auto& p) { p.scripts[0].baseline = {}; }},
        {"foreign baseline", [](auto& p) { p.contents[0].region.baseline = p.baselines[1].id; }},
        {"wrong document", [](auto& p) { p.contents[0].region.importedDocument = p.baselines[1].importedDocument; }},
        {"duplicate document", [](auto& p) { p.baselines[1].importedDocument = p.baselines[0].importedDocument; }},
        {"duplicate ID across types", [](auto& p) { p.modules[0].id.value = p.scripts[0].id.value; }},
        {"duplicate module", [](auto& p) { p.modules.push_back(p.modules[0]); }},
        {"zero ID", [](auto& p) { p.connections[0].id = {}; }},
        {"stale allocator", [](auto& p) { p.nextEntityId = 2; }},
        {"zero allocator", [](auto& p) { p.nextEntityId = 0; }},
        {"orphan module", [](auto& p) { p.modules[0].script = SctScriptId{999}; }},
        {"orphan entrypoint", [](auto& p) { p.entrypoints[0].script = {}; }},
        {"orphan port", [](auto& p) { p.ports[0].owner = SctEntrypointId{999}; }},
        {"orphan content", [](auto& p) { p.contents[0].owner = SctModuleId{999}; }},
        {"dangling endpoint", [](auto& p) { p.connections[0].destination = SctPortId{999}; }},
        {"dangling use", [](auto& p) { p.modules[0].contentUses.push_back(SctContentId{999}); }},
        {"cross-script use", [](auto& p) { p.modules[0].contentUses.push_back(p.contents[1].id); }},
        {"invalid evidence", [](auto& p) { p.modules[0].evidence[0].kind = static_cast<SctEvidenceKind>(99); }},
        {"empty unresolved", [](auto& p) { p.connections[0].destination = SctUnresolvedBinding{}; }},
        {"zero revision", [](auto& p) { p.revision = {}; }},
        {"invalid UUID", [](auto& p) { p.id.value = "not-a-uuid"; }},
        {"nil UUID", [](auto& p) { p.id.value = "00000000-0000-0000-0000-000000000000"; }},
        {"invalid profile", [](auto& p) { p.scripts[0].region = static_cast<GameRegion>(99); }},
        {"invalid convention", [](auto& p) { p.baselines[0].textConvention = static_cast<spice::sct::SctKnownTextConvention>(99); }},
    };
    for (const auto& [name, mutate] : cases) {
        SCOPED_TRACE(name); auto p = fixture(); mutate(p);
        EXPECT_TRUE(hasErrors(p.validate())); EXPECT_FALSE(SctAuthoringCodec::encode(p));
    }
}
TEST(SctAuthoringProjectTest, UnresolvedBindingsRoundTripAsWarningsWithEvidence) {
    auto p = fixture();
    const SctUnresolvedBinding unresolved{"raw-selector=42", "Target has not been identified",
        {{SctEvidenceKind::SourceObservation, "Observed raw selector"}, {SctEvidenceKind::UserAssertion, "User interpretation"}}};
    p.connections[0].destination = unresolved;
    p.modules[0].contentUses.push_back(unresolved);
    auto result = SctAuthoringCodec::decode(encoded(p)); ASSERT_TRUE(result);
    ASSERT_EQ(result.diagnostics().size(), 2);
    for (const auto& d : result.diagnostics()) {
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning); EXPECT_EQ(d.code, DiagnosticCode::UnresolvedSctAuthoringBinding);
    }
    const auto& binding = std::get<SctUnresolvedBinding>(result.value().connections[0].destination);
    EXPECT_EQ(binding.originalEvidence, unresolved.originalEvidence); EXPECT_EQ(binding.explanation, unresolved.explanation);
    ASSERT_EQ(binding.evidence.size(), 2); EXPECT_EQ(binding.evidence[1].kind, SctEvidenceKind::UserAssertion);
    EXPECT_EQ(result.value().modules[0].evidence[0].kind, SctEvidenceKind::Inference);
}
TEST(SctAuthoringProjectTest, PreservesOrderedPhysicalSelectionsAndOpaqueReferences) {
    auto p = fixture();
    const SctOrderedSourceSelection selection{{spice::sct::SctInstructionId{99}, spice::sct::SctInstructionId{2},
        spice::sct::SctOpaqueAttachmentId{7}, spice::sct::SctStringId{7}, spice::sct::SctSupplementaryTextId{8}, spice::sct::SctSectionId{9}}};
    p.contents[0].region.coverage = selection;
    const auto bytes = encoded(p); auto result = SctAuthoringCodec::decode(bytes); ASSERT_TRUE(result);
    const auto& region = result.value().contents[0].region;
    EXPECT_EQ(region.baseline, p.baselines[0].id); EXPECT_EQ(region.importedDocument, p.baselines[0].importedDocument);
    EXPECT_EQ(std::get<SctOrderedSourceSelection>(region.coverage).entities, selection.entities);
    EXPECT_TRUE(std::holds_alternative<SctWholeDocument>(result.value().contents[1].region.coverage));
    for (const SctOrderedSourceSelection bad : {SctOrderedSourceSelection{}, SctOrderedSourceSelection{{std::monostate{}}},
        SctOrderedSourceSelection{{spice::sct::SctInstructionId{0}}},
        SctOrderedSourceSelection{{spice::sct::SctInstructionId{2}, spice::sct::SctInstructionId{2}}}}) {
        p.contents[0].region.coverage = bad; EXPECT_TRUE(hasErrors(p.validate()));
    }
}
TEST(SctAuthoringProjectTest, UnorderedCollectionsCanonicalizeButContentUseOrderDoesNot) {
    auto p = fixture(); const auto before = encoded(p);
    std::ranges::reverse(p.scripts); std::ranges::reverse(p.baselines); std::ranges::reverse(p.modules);
    std::ranges::reverse(p.entrypoints); std::ranges::reverse(p.connections); std::ranges::reverse(p.ports); std::ranges::reverse(p.contents);
    EXPECT_EQ(encoded(p), before);
    p.modules[0].contentUses.push_back(SctUnresolvedBinding{"raw", "unresolved", {}});
    const auto ordered = encoded(p); std::ranges::reverse(p.modules[0].contentUses);
    EXPECT_NE(encoded(p), ordered);
}
TEST(SctAuthoringProjectTest, DecimalIdsAreExactAboveDoublePrecisionAndAllocatorDoesNotWrap) {
    auto p = fixture(); p.nextEntityId = 9007199254740993ULL;
    const auto module = allocate<SctModuleId>(p); p.modules.push_back({module, p.scripts[0].id, "Large ID", {}, {}});
    auto result = SctAuthoringCodec::decode(encoded(p)); ASSERT_TRUE(result);
    ASSERT_NE(result.value().find(module), nullptr); EXPECT_EQ(module.value, 9007199254740993ULL);
    p.nextEntityId = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(p.allocate<SctModuleId>()); EXPECT_EQ(p.nextEntityId, std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(SctAuthoringCodec::decode(encoded(p)));
}
TEST(SctAuthoringProjectTest, MalformedOrUnsupportedJsonNeverReturnsPartialProject) {
    const auto bytes = encoded(fixture());
    const std::vector<std::function<void(Json&)>> mutations{
        [](auto& j) { j["scripts"][0]["baseline"] = "999"; },
        [](auto& j) { j["scripts"][0]["baseline"] = "0"; },
        [](auto& j) { j["scripts"][0]["baseline"] = nullptr; },
        [](auto& j) { j["ports"][0]["owner"]["kind"] = "script"; },
        [](auto& j) { j["modules"][0]["id"] = 3; },
        [](auto& j) { j["nextEntityId"] = "18446744073709551616"; },
        [](auto& j) { j["nextEntityId"] = "-1"; },
        [](auto& j) { j["nextEntityId"] = "017"; },
        [](auto& j) { j["baselines"][0]["asset"] = "../escape.sct"; },
        [](auto& j) { j["baselines"][0]["asset"] = "C:/absolute.sct"; },
        [](auto& j) { j["baselines"][0]["sourceRevision"] = "bad-hash"; },
        [](auto& j) { j["modules"][0]["evidence"][0]["kind"] = nullptr; },
        [](auto& j) { j["format"] = "other"; },
        [](auto& j) { j["schemaVersion"] = 1; },
        [](auto& j) { j["schemaVersion"] = 1.0; },
        [](auto& j) { j["extra"] = true; },
        [](auto& j) { j.erase("contents"); },
        [](auto& j) { j["modules"] = Json::object(); },
    };
    for (std::size_t i = 0; i < mutations.size(); ++i) {
        SCOPED_TRACE(i); auto json = Json::parse(bytes); mutations[i](json);
        auto result = SctAuthoringCodec::decode(json.dump()); EXPECT_FALSE(result.hasValue()); EXPECT_TRUE(hasErrors(result.diagnostics()));
    }
    auto json = Json::parse(bytes); json["schemaVersion"] = 1;
    EXPECT_EQ(SctAuthoringCodec::decode(json.dump()).diagnostics()[0].code, DiagnosticCode::UnsupportedPersistenceSchemaVersion);
    EXPECT_FALSE(SctAuthoringCodec::decode("{"));
    EXPECT_FALSE(SctAuthoringCodec::decode("{\"format\":\"a\",\"format\":\"b\"}"));
}
TEST(SctAuthoringProjectTest, RealizationMappingsAreRevisionQualifiedAndAllowSharedLocations) {
    const auto p = fixture();
    SctRealizationKey key{p.id, p.revision, p.scripts[0].id, {RealizationUuid}};
    const spice::sct::SctDiagnosticLocation location{spice::sct::SctDocumentEntityId{spice::sct::SctInstructionId{12}}};
    const spice::sct::SctDiagnosticLocation parameter{spice::sct::SctParameterSite{spice::sct::SctInstructionId{12}, {3, {}}}};
    SctRealizationMap map{key, {{p.modules[0].id, {location, parameter}}, {p.contents[0].id, {location}}}};
    EXPECT_TRUE(map.isFresh(key)); EXPECT_EQ(map.records[0].locations.size(), 2);
    auto changed = key; changed.project.value = DocumentA; EXPECT_FALSE(map.isFresh(changed));
    changed = key; ++changed.revision.value; EXPECT_FALSE(map.isFresh(changed));
    changed = key; changed.script = p.scripts[1].id; EXPECT_FALSE(map.isFresh(changed));
    changed = key; changed.realization.value = DocumentB; EXPECT_FALSE(map.isFresh(changed));
    SctRealizationMap invalid; EXPECT_FALSE(invalid.isFresh(invalid.key));
}
TEST(SctAuthoringPresentationTest, LayoutAndSelectionRoundTripWithoutChangingSemanticState) {
    const auto p = fixture(); const auto semantic = encoded(p);
    SctAuthoringPresentation view{p.id, {{p.modules[0].id, 12, -5, false}, {p.scripts[0].id, 99, 1, true}}, {p.modules[0].id}};
    auto result = SctAuthoringPresentationCodec::encode(view, p); ASSERT_TRUE(result);
    auto decoded = SctAuthoringPresentationCodec::decode(result.value(), p); ASSERT_TRUE(decoded);
    EXPECT_EQ(SctAuthoringPresentationCodec::encode(decoded.value(), p).value(), result.value());
    view.placements[0].x = 1234; view.placements[0].collapsed = true; view.selection.clear();
    auto moved = SctAuthoringPresentationCodec::encode(view, p); ASSERT_TRUE(moved);
    EXPECT_NE(moved.value(), result.value()); EXPECT_EQ(encoded(p), semantic);
    EXPECT_FALSE(SctAuthoringCodec::decode(moved.value()));
    EXPECT_FALSE(SctAuthoringPresentationCodec::decode(semantic, p));
}
TEST(SctAuthoringPresentationTest, RejectsInvalidLayoutAndForeignProjects) {
    const auto p = fixture();
    SctAuthoringPresentation view{p.id, {{p.modules[0].id, 0, 0, false}}, {}};
    view.placements[0].x = std::numeric_limits<double>::infinity(); EXPECT_FALSE(SctAuthoringPresentationCodec::encode(view, p));
    view.placements[0].x = 0; view.placements.push_back(view.placements[0]); EXPECT_FALSE(SctAuthoringPresentationCodec::encode(view, p));
    view.placements.pop_back(); view.selection.push_back(SctModuleId{999}); EXPECT_FALSE(SctAuthoringPresentationCodec::encode(view, p));
    view.selection.clear(); view.project.value = DocumentA; EXPECT_FALSE(SctAuthoringPresentationCodec::encode(view, p));
}
} // namespace
} // namespace salsa::core
