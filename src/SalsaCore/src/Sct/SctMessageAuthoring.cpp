#include "SalsaCore/Sct/SctMessageAuthoring.h"

#include "SpiceSCT/SctTextBuilder.h"

#include <type_traits>
#include <utility>

namespace salsa::core {
namespace {

using Code = spice::sct::SctMessageCommandCode;
using Command = spice::sct::SctInlineCommand;
using Element = spice::sct::SctFormattedTextElement;

void issue(
    std::vector<SctMessageProfileIssue>& issues,
    const SctMessageProfileIssueCode code,
    const std::optional<std::size_t> ordinal,
    std::string message) {
    issues.push_back({code, ordinal, std::move(message)});
}

[[nodiscard]] bool noArgument(const Command& command) {
    return std::holds_alternative<spice::sct::SctNoCommandArgument>(command.argument);
}

[[nodiscard]] std::optional<std::uint32_t> decimalArgument(const Command& command) {
    const auto* argument = std::get_if<spice::sct::SctDecimalCommandArgument>(&command.argument);
    return argument == nullptr ? std::nullopt : argument->value;
}

void appendRun(
    SctMessageDraft& draft,
    const std::string& text,
    const SctMessageTextStyle& style) {
    if (text.empty()) return;
    if (!draft.body.empty() && draft.body.back().style == style) {
        draft.body.back().utf8 += text;
    } else {
        draft.body.push_back({text, style});
    }
}

void appendCommand(std::vector<Element>& elements, const spice::sct::SctInlineCommandBuildResult& result) {
    if (result.command.has_value()) elements.push_back(*result.command);
}

[[nodiscard]] std::string buildIssueMessage(
    const std::vector<spice::sct::SctDocumentDiagnostic>& diagnostics) {
    return diagnostics.empty() ? "The message could not be materialized."
                               : diagnostics.front().message;
}

}  // namespace

SctMessageProjection SctMessageAuthoringProfile::project(
    const spice::sct::SctMessage& message) {
    SctMessageProjection result;
    SctMessageDraft draft;
    draft.headerUtf8 = message.headerUtf8;
    if (draft.headerUtf8.has_value()
        && (!spice::sct::SctTextBuilder::isValidUtf8(*draft.headerUtf8)
            || draft.headerUtf8->find('\0') != std::string::npos
            || draft.headerUtf8->find('\r') != std::string::npos
            || draft.headerUtf8->find('\n') != std::string::npos)) {
        issue(result.issues, SctMessageProfileIssueCode::InvalidHeader, std::nullopt,
            "The message header is not valid single-line UTF-8.");
    }

    enum class Phase { Prefix, Body, Trailing };
    Phase phase = Phase::Prefix;
    SctMessageTextStyle style;
    bool sawTextForCurrentFormatting = false;
    bool sawFormatting = false;
    bool sawPosition = false;
    bool sawFastForward = false;
    bool sawCompletion = false;
    bool sawOpenDuration = false;
    bool sawCloseDuration = false;

    for (std::size_t ordinal = 0; ordinal < message.body.elements.size(); ++ordinal) {
        const auto& element = message.body.elements[ordinal];
        if (const auto* text = std::get_if<spice::sct::SctTextChunk>(&element)) {
            if (phase == Phase::Trailing) {
                issue(result.issues, SctMessageProfileIssueCode::TextAfterCompletion, ordinal,
                    "Displayed text occurs after the message completion behavior.");
                continue;
            }
            if (text->utf8.empty() || !spice::sct::SctTextBuilder::isValidUtf8(text->utf8)
                || text->utf8.find('\0') != std::string::npos
                || text->utf8.find('\r') != std::string::npos) {
                issue(result.issues, SctMessageProfileIssueCode::InvalidText, ordinal,
                    "The message body contains invalid or noncanonical UTF-8 text.");
                continue;
            }
            phase = Phase::Body;
            appendRun(draft, text->utf8, style);
            sawTextForCurrentFormatting = true;
            continue;
        }

        const auto& command = std::get<Command>(element);
        const auto commandOrdinal = std::optional<std::size_t>{ordinal};
        switch (command.code) {
        case Code::D:
        case Code::U:
            if (!noArgument(command)) {
                issue(result.issues, SctMessageProfileIssueCode::InvalidCommandArgument,
                    commandOrdinal, "The message-position command has an invalid argument.");
            }
            if (phase != Phase::Prefix || sawFastForward) {
                issue(result.issues, SctMessageProfileIssueCode::MisplacedCommand,
                    commandOrdinal, "Message position must appear before fast-forward and body content.");
            } else if (sawPosition) {
                issue(result.issues, SctMessageProfileIssueCode::ConflictingPosition,
                    commandOrdinal, "The message contains repeated or conflicting position commands.");
            } else {
                sawPosition = true;
                draft.position = command.code == Code::D
                    ? SctMessagePosition::Lower : SctMessagePosition::Upper;
            }
            break;
        case Code::X:
            if (!noArgument(command)) {
                issue(result.issues, SctMessageProfileIssueCode::InvalidCommandArgument,
                    commandOrdinal, "The fast-forward command has an invalid argument.");
            }
            if (phase != Phase::Prefix) {
                issue(result.issues, SctMessageProfileIssueCode::MisplacedCommand,
                    commandOrdinal, "The fast-forward option must appear before body content.");
            } else if (sawFastForward) {
                issue(result.issues, SctMessageProfileIssueCode::DuplicateCommand,
                    commandOrdinal, "The fast-forward option is repeated.");
            } else {
                sawFastForward = true;
                draft.disableFastForward = true;
            }
            break;
        case Code::B:
            sawFormatting = true;
            if (!noArgument(command)) {
                issue(result.issues, SctMessageProfileIssueCode::InvalidCommandArgument,
                    commandOrdinal, "The double-scale command has an invalid argument.");
            }
            if (phase == Phase::Trailing) {
                issue(result.issues, SctMessageProfileIssueCode::MisplacedCommand,
                    commandOrdinal, "Formatting cannot occur after message completion.");
            } else {
                phase = Phase::Body;
                style.doubleScale = !style.doubleScale;
                sawTextForCurrentFormatting = false;
            }
            break;
        case Code::P: {
            sawFormatting = true;
            const auto* argument = std::get_if<spice::sct::SctByteListCommandArgument>(
                &command.argument);
            if (phase == Phase::Trailing) {
                issue(result.issues, SctMessageProfileIssueCode::MisplacedCommand,
                    commandOrdinal, "Formatting cannot occur after message completion.");
            } else if (argument == nullptr || argument->values.size() != 3u) {
                issue(result.issues, SctMessageProfileIssueCode::InvalidColor,
                    commandOrdinal, "The initial editor supports only three-component RGB color commands.");
            } else {
                phase = Phase::Body;
                style.color = SctMessageRgb{
                    argument->values[0], argument->values[1], argument->values[2]};
                sawTextForCurrentFormatting = false;
            }
            break;
        }
        case Code::R:
            sawFormatting = true;
            if (!noArgument(command)) {
                issue(result.issues, SctMessageProfileIssueCode::InvalidCommandArgument,
                    commandOrdinal, "The formatting-reset command has an invalid argument.");
            }
            if (phase == Phase::Trailing) {
                issue(result.issues, SctMessageProfileIssueCode::MisplacedCommand,
                    commandOrdinal, "Formatting cannot occur after message completion.");
            } else {
                phase = Phase::Body;
                style = {};
                sawTextForCurrentFormatting = false;
            }
            break;
        case Code::A:
        case Code::C:
        case Code::E:
            if (sawCompletion) {
                issue(result.issues, SctMessageProfileIssueCode::MultipleCompletions,
                    commandOrdinal, "The message contains more than one completion behavior.");
                break;
            }
            if (command.code == Code::A) {
                const auto value = decimalArgument(command);
                if (!value.has_value()) {
                    issue(result.issues, SctMessageProfileIssueCode::NumericValueRequired,
                        commandOrdinal, "Automatic continuation requires a delay.");
                    break;
                }
                if (*value > 0xffffu) {
                    issue(result.issues, SctMessageProfileIssueCode::NumericValueOutOfRange,
                        commandOrdinal, "Automatic continuation delay must be between 0 and 65535.");
                    break;
                }
                draft.completion = SctMessageAutomatic{static_cast<std::uint16_t>(*value)};
            } else {
                if (!noArgument(command)) {
                    issue(result.issues, SctMessageProfileIssueCode::InvalidCommandArgument,
                        commandOrdinal, "The manual completion command has an invalid argument.");
                }
                draft.completion = command.code == Code::C
                    ? SctMessageCompletion{SctMessageClose{}}
                    : SctMessageCompletion{SctMessageContinue{}};
            }
            sawCompletion = true;
            phase = Phase::Trailing;
            break;
        case Code::Wo:
        case Code::Wc: {
            if (!sawCompletion || phase != Phase::Trailing) {
                issue(result.issues, SctMessageProfileIssueCode::MisplacedCommand,
                    commandOrdinal, "Dialogue duration fields must follow message completion.");
                break;
            }
            const auto value = decimalArgument(command);
            if (!value.has_value()) {
                issue(result.issues, SctMessageProfileIssueCode::NumericValueRequired,
                    commandOrdinal, "Dialogue duration requires a numeric value.");
                break;
            }
            if (*value > 0xffffu) {
                issue(result.issues, SctMessageProfileIssueCode::NumericValueOutOfRange,
                    commandOrdinal, "Dialogue duration must be between 0 and 65535.");
                break;
            }
            if (command.code == Code::Wo) {
                if (sawOpenDuration || sawCloseDuration) {
                    issue(result.issues, sawOpenDuration
                            ? SctMessageProfileIssueCode::DuplicateCommand
                            : SctMessageProfileIssueCode::MisplacedCommand,
                        commandOrdinal, sawOpenDuration
                            ? "The open duration is repeated."
                            : "Open duration must precede close duration.");
                } else {
                    sawOpenDuration = true;
                    draft.openDuration = static_cast<std::uint16_t>(*value);
                }
            } else if (sawCloseDuration) {
                issue(result.issues, SctMessageProfileIssueCode::DuplicateCommand,
                    commandOrdinal, "The close duration is repeated.");
            } else {
                sawCloseDuration = true;
                draft.closeDuration = static_cast<std::uint16_t>(*value);
            }
            break;
        }
        case Code::S:
            issue(result.issues, SctMessageProfileIssueCode::UnsupportedCommand,
                commandOrdinal, "Reveal-interval commands are not supported by the initial editor.");
            break;
        }
    }

    if (!sawCompletion) {
        issue(result.issues, SctMessageProfileIssueCode::MissingCompletion, std::nullopt,
            "The message requires exactly one completion behavior.");
    }
    if (sawFormatting && !sawTextForCurrentFormatting) {
        issue(result.issues, SctMessageProfileIssueCode::TrailingFormatting, std::nullopt,
            "A formatting command has no following displayed text.");
    }
    if (result.issues.empty()) result.draft = std::move(draft);
    return result;
}

SctMessageMaterialization SctMessageAuthoringProfile::materialize(
    const SctMessageDraft& draft) {
    SctMessageMaterialization result;
    std::vector<Element> elements;
    if (draft.position == SctMessagePosition::Lower) {
        appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::D));
    } else if (draft.position == SctMessagePosition::Upper) {
        appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::U));
    }
    if (draft.disableFastForward)
        appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::X));

    SctMessageTextStyle current;
    for (std::size_t ordinal = 0; ordinal < draft.body.size(); ++ordinal) {
        const auto& run = draft.body[ordinal];
        if (run.utf8.empty() || !spice::sct::SctTextBuilder::isValidUtf8(run.utf8)
            || run.utf8.find('\0') != std::string::npos
            || run.utf8.find('\r') != std::string::npos) {
            issue(result.issues, SctMessageProfileIssueCode::InvalidText, ordinal,
                "Message runs must contain nonempty, zero-free UTF-8 using LF line endings.");
            continue;
        }

        const bool needsReset = (current.doubleScale && !run.style.doubleScale)
            || (current.color.has_value() && !run.style.color.has_value());
        if (needsReset) {
            appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::R));
            current = {};
        }
        if (current.doubleScale != run.style.doubleScale) {
            appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::B));
            current.doubleScale = run.style.doubleScale;
        }
        if (current.color != run.style.color && run.style.color.has_value()) {
            appendCommand(elements, spice::sct::SctTextBuilder::colorCommand(
                run.style.color->red, run.style.color->green, run.style.color->blue));
            current.color = run.style.color;
        }
        elements.push_back(spice::sct::SctTextChunk{run.utf8});
    }

    std::visit([&](const auto& completion) {
        using T = std::decay_t<decltype(completion)>;
        if constexpr (std::is_same_v<T, SctMessageContinue>) {
            appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::E));
        } else if constexpr (std::is_same_v<T, SctMessageClose>) {
            appendCommand(elements, spice::sct::SctTextBuilder::noArgumentCommand(Code::C));
        } else {
            appendCommand(elements, spice::sct::SctTextBuilder::decimalCommand(
                Code::A, completion.delay));
        }
    }, draft.completion);
    if (draft.openDuration.has_value()) {
        appendCommand(elements, spice::sct::SctTextBuilder::decimalCommand(
            Code::Wo, *draft.openDuration));
    }
    if (draft.closeDuration.has_value()) {
        appendCommand(elements, spice::sct::SctTextBuilder::decimalCommand(
            Code::Wc, *draft.closeDuration));
    }

    if (!result.issues.empty()) return result;
    auto built = spice::sct::SctTextBuilder::message(draft.headerUtf8, std::move(elements));
    if (!built.message.has_value()) {
        issue(result.issues, SctMessageProfileIssueCode::InvalidText, std::nullopt,
            buildIssueMessage(built.diagnostics));
        return result;
    }
    result.message = std::move(built.message);
    return result;
}

}  // namespace salsa::core
