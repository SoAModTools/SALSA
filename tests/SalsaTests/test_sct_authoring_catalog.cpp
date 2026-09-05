#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>

namespace salsa::core {
namespace {

TEST(SctAuthoringCatalogTest, WorkspaceRoundTripsCanonicalAliasesAndColors) {
    SctWorkspaceAuthoringState source;
    source.projectAliases = {
        {{SctVariableKind::Byte, 7}, "DoorState"},
        {{SctVariableKind::Bit, 2}, "CutsceneSeen"}};
    source.opcodeColors = {{10, 0x123456}, {0, 0xabcdef}};

    const auto encoded = SctAuthoringCatalogCodec::serializeWorkspace(source);
    ASSERT_TRUE(encoded) << encoded.diagnostics().front().message;
    const auto decoded = SctAuthoringCatalogCodec::deserializeWorkspace(encoded.value());
    ASSERT_TRUE(decoded) << decoded.diagnostics().front().message;
    ASSERT_EQ(decoded.value().projectAliases.size(), 2u);
    EXPECT_EQ(decoded.value().projectAliases.front().variable.kind,
        SctVariableKind::Bit);
    ASSERT_EQ(decoded.value().opcodeColors.size(), 2u);
    EXPECT_EQ(decoded.value().opcodeColors.front().opcode, 0u);
}

TEST(SctAuthoringCatalogTest, DuplicateAliasNamesAreRejectedCaseInsensitively) {
    SctWorkspaceAuthoringState source;
    source.projectAliases = {{{SctVariableKind::Byte, 7}, "DoorState"},
        {{SctVariableKind::Bit, 2}, "doorstate"}};
    const auto encoded = SctAuthoringCatalogCodec::serializeWorkspace(source);
    EXPECT_FALSE(encoded);
}

TEST(SctAuthoringCatalogTest, PersonalOverlayResolvesAndSuppliesCreationDefaults) {
    SctPersonalCatalog catalog{{SctCatalogOpcodeOverride{
        .opcode = 10,
        .mnemonic = "WaitForFlag",
        .description = "Wait for a project flag.",
        .note = "Personal note",
        .parameters = {{0, "Flag", 0x11223344u}},
        .colorRgb = 0x224466u,
        .category = "Flow"}}};
    SctCatalogResolver::install(std::make_shared<const SctPersonalCatalog>(catalog));

    const auto resolved = SctCatalogResolver::resolve(10);
    EXPECT_EQ(resolved.mnemonic, "WaitForFlag");
    EXPECT_EQ(resolved.description, "Wait for a project flag.");
    EXPECT_EQ(resolved.parameterLabels.front(), "Flag");
    EXPECT_EQ(resolved.colorRgb, 0x224466u);

    spice::sct::SctInstructionFactoryRequest request{.opcode = 10};
    SctCatalogResolver::applyCreationDefaults(request);
    ASSERT_EQ(request.parameterOverrides.size(), 1u);
    EXPECT_EQ(request.parameterOverrides.front().address.schemaIndex, 0u);
    ASSERT_TRUE(std::holds_alternative<spice::sct::SctEncodedWordValue>(
        request.parameterOverrides.front().value));
    EXPECT_EQ(std::get<spice::sct::SctEncodedWordValue>(
        request.parameterOverrides.front().value).value, 0x11223344u);

    SctCatalogResolver::install(nullptr);
}

TEST(SctAuthoringCatalogTest, LegacyPreviewDefaultsConflictsToKeepCurrent) {
    const std::string legacy = R"({
      "10": {"Name":"Incoming", "Description":"Imported",
        "Parameters":{"0":{"Name":"Flag", "Default":7,
          "LockedSchemaFact":"ignored"}}}
    })";
    const SctPersonalCatalog current{{SctCatalogOpcodeOverride{
        .opcode = 10, .mnemonic = "Current"}}};
    const auto preview = SctLegacyCatalogImporter::preview(
        std::as_bytes(std::span{legacy.data(), legacy.size()}), current);
    ASSERT_TRUE(preview) << preview.diagnostics().front().message;
    ASSERT_EQ(preview.value().changes.size(), 3u);
    EXPECT_TRUE(preview.value().changes.front().conflict);
    ASSERT_EQ(preview.value().ignoredLockedFields.size(), 1u);

    std::vector<SctLegacyCatalogFieldChange> accepted;
    for (const auto& change : preview.value().changes)
        if (!change.conflict) accepted.push_back(change);
    const auto applied = SctLegacyCatalogImporter::apply(
        preview.value(), current, accepted);
    ASSERT_TRUE(applied) << applied.diagnostics().front().message;
    ASSERT_EQ(applied.value().opcodes.size(), 1u);
    EXPECT_EQ(applied.value().opcodes.front().mnemonic, "Current");
    EXPECT_EQ(applied.value().opcodes.front().description, "Imported");
    ASSERT_EQ(applied.value().opcodes.front().parameters.size(), 1u);
    EXPECT_EQ(applied.value().opcodes.front().parameters.front().label, "Flag");
}

} // namespace
} // namespace salsa::core
