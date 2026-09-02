#include "SalsaCore/Sct/SctMessageAuthoring.h"

#include "SpiceSCT/SctTextBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace salsa::core;
using namespace spice::sct;

SctInlineCommand command(const SctMessageCommandCode code) {
    const auto built = SctTextBuilder::noArgumentCommand(code);
    EXPECT_TRUE(built.command.has_value());
    return *built.command;
}

SctInlineCommand decimalCommand(
    const SctMessageCommandCode code,
    const std::optional<std::uint32_t> value) {
    const auto built = SctTextBuilder::decimalCommand(code, value);
    EXPECT_TRUE(built.command.has_value());
    return *built.command;
}

SctInlineCommand colorCommand(const std::vector<std::uint8_t>& values) {
    return SctInlineCommand{SctMessageCommandCode::P,
        SctByteListCommandArgument{values}};
}

std::vector<SctMessageCommandCode> commandCodes(const SctMessage& message) {
    std::vector<SctMessageCommandCode> result;
    for (const auto& element : message.body.elements) {
        if (const auto* typed = std::get_if<SctInlineCommand>(&element))
            result.push_back(typed->code);
    }
    return result;
}

bool hasIssue(const SctMessageProjection& projection, const SctMessageProfileIssueCode code) {
    return std::ranges::any_of(projection.issues, [code](const auto& issue) {
        return issue.code == code;
    });
}
}  // namespace

TEST(SctMessageAuthoringProfile, MaterializesCanonicalOrderAndFormattingTransitions) {
    SctMessageDraft draft;
    draft.headerUtf8 = std::string{};
    draft.position = SctMessagePosition::Upper;
    draft.disableFastForward = true;
    draft.body = {
        {"Normal\n", {}},
        {"Large red", {true, SctMessageRgb{0x11, 0x22, 0x33}}},
        {"Large default", {true, std::nullopt}},
        {"Green", {false, SctMessageRgb{0x44, 0x55, 0x66}}},
    };
    draft.completion = SctMessageAutomatic{65535};
    draft.openDuration = 0;
    draft.closeDuration = 65535;

    const auto materialized = SctMessageAuthoringProfile::materialize(draft);
    ASSERT_TRUE(materialized.succeeded());
    EXPECT_EQ(materialized.message->headerUtf8, std::optional<std::string>{""});
    const std::vector expected{
        SctMessageCommandCode::U,
        SctMessageCommandCode::X,
        SctMessageCommandCode::B,
        SctMessageCommandCode::P,
        SctMessageCommandCode::R,
        SctMessageCommandCode::B,
        SctMessageCommandCode::R,
        SctMessageCommandCode::P,
        SctMessageCommandCode::A,
        SctMessageCommandCode::Wo,
        SctMessageCommandCode::Wc,
    };
    EXPECT_EQ(commandCodes(*materialized.message), expected);

    const auto projected = SctMessageAuthoringProfile::project(*materialized.message);
    ASSERT_TRUE(projected.supported());
    EXPECT_EQ(*projected.draft, draft);
}

TEST(SctMessageAuthoringProfile, PreservesExactUnicodeAndAbsentVersusEmptyHeaders) {
    const std::string exact = "ASCII ";
    const std::string unicode = exact + "\xE2\x80\x9C\xEF\xBC\x82\xE3\x80\x9C\xEF\xBD\x9E";
    SctMessageDraft absent;
    absent.body = {{unicode, {}}};
    absent.completion = SctMessageContinue{};
    const auto absentBuilt = SctMessageAuthoringProfile::materialize(absent);
    ASSERT_TRUE(absentBuilt.succeeded());
    EXPECT_FALSE(absentBuilt.message->headerUtf8.has_value());
    const auto absentProjected = SctMessageAuthoringProfile::project(*absentBuilt.message);
    ASSERT_TRUE(absentProjected.supported());
    EXPECT_EQ(absentProjected.draft->body.front().utf8, unicode);

    absent.headerUtf8 = std::string{};
    const auto emptyBuilt = SctMessageAuthoringProfile::materialize(absent);
    ASSERT_TRUE(emptyBuilt.succeeded());
    ASSERT_TRUE(emptyBuilt.message->headerUtf8.has_value());
    EXPECT_TRUE(emptyBuilt.message->headerUtf8->empty());
}

TEST(SctMessageAuthoringProfile, SupportsAllCompletionModesAndEmptyBodies) {
    for (const auto& completion : std::array<SctMessageCompletion, 3>{
             SctMessageContinue{}, SctMessageClose{}, SctMessageAutomatic{0}}) {
        SctMessageDraft draft;
        draft.completion = completion;
        const auto built = SctMessageAuthoringProfile::materialize(draft);
        ASSERT_TRUE(built.succeeded());
        const auto projected = SctMessageAuthoringProfile::project(*built.message);
        ASSERT_TRUE(projected.supported());
        EXPECT_EQ(projected.draft->completion, completion);
        EXPECT_TRUE(projected.draft->body.empty());
    }
}

TEST(SctMessageAuthoringProfile, RejectsUnsupportedAndNoncanonicalImportedMessages) {
    const std::vector<SctMessage> messages{
        {std::nullopt, SctFormattedText{{SctTextChunk{"text"}}}},
        {std::nullopt, SctFormattedText{{command(SctMessageCommandCode::E), SctTextChunk{"late"}}}},
        {std::nullopt, SctFormattedText{{command(SctMessageCommandCode::E), command(SctMessageCommandCode::C)}}},
        {std::nullopt, SctFormattedText{{SctTextChunk{"text"}, command(SctMessageCommandCode::D), command(SctMessageCommandCode::E)}}},
        {std::nullopt, SctFormattedText{{decimalCommand(SctMessageCommandCode::S, 5), command(SctMessageCommandCode::E)}}},
        {std::nullopt, SctFormattedText{{colorCommand({1, 2}), SctTextChunk{"text"}, command(SctMessageCommandCode::E)}}},
        {std::nullopt, SctFormattedText{{decimalCommand(SctMessageCommandCode::A, std::nullopt)}}},
        {std::nullopt, SctFormattedText{{decimalCommand(SctMessageCommandCode::A, 65536)}}},
        {std::nullopt, SctFormattedText{{command(SctMessageCommandCode::B), command(SctMessageCommandCode::E)}}},
    };
    for (const auto& message : messages) {
        const auto projection = SctMessageAuthoringProfile::project(message);
        EXPECT_FALSE(projection.supported());
        EXPECT_FALSE(projection.issues.empty());
    }
    EXPECT_TRUE(hasIssue(SctMessageAuthoringProfile::project(messages[0]),
        SctMessageProfileIssueCode::MissingCompletion));
    EXPECT_TRUE(hasIssue(SctMessageAuthoringProfile::project(messages[1]),
        SctMessageProfileIssueCode::TextAfterCompletion));
    EXPECT_TRUE(hasIssue(SctMessageAuthoringProfile::project(messages[2]),
        SctMessageProfileIssueCode::MultipleCompletions));
    EXPECT_TRUE(hasIssue(SctMessageAuthoringProfile::project(messages[4]),
        SctMessageProfileIssueCode::UnsupportedCommand));
    EXPECT_TRUE(hasIssue(SctMessageAuthoringProfile::project(messages[5]),
        SctMessageProfileIssueCode::InvalidColor));
    EXPECT_TRUE(hasIssue(SctMessageAuthoringProfile::project(messages[8]),
        SctMessageProfileIssueCode::TrailingFormatting));
}

TEST(SctMessageAuthoringProfile, RejectsInvalidDraftTextWithoutPartialMaterialization) {
    SctMessageDraft emptyRun;
    emptyRun.body.push_back({"", {}});
    EXPECT_FALSE(SctMessageAuthoringProfile::materialize(emptyRun).succeeded());

    SctMessageDraft carriageReturn;
    carriageReturn.body.push_back({"a\rb", {}});
    EXPECT_FALSE(SctMessageAuthoringProfile::materialize(carriageReturn).succeeded());

    SctMessageDraft invalidHeader;
    invalidHeader.headerUtf8 = "a\nb";
    EXPECT_FALSE(SctMessageAuthoringProfile::materialize(invalidHeader).succeeded());
}
