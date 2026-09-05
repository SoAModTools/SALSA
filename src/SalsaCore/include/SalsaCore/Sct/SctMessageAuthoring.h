#pragma once

#include "SpiceSCT/SctDocument.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace salsa::core {

struct SctMessageRgb final {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    auto operator<=>(const SctMessageRgb&) const = default;
};

struct SctMessageTextStyle final {
    bool doubleScale = false;
    std::optional<SctMessageRgb> color{};
    auto operator<=>(const SctMessageTextStyle&) const = default;
};

struct SctMessageTextRun final {
    std::string utf8{};
    SctMessageTextStyle style{};
    auto operator<=>(const SctMessageTextRun&) const = default;
};

enum class SctMessagePosition {
    Default,
    Lower,
    Upper,
};

struct SctMessageContinue final {
    auto operator<=>(const SctMessageContinue&) const = default;
};

struct SctMessageClose final {
    auto operator<=>(const SctMessageClose&) const = default;
};

struct SctMessageAutomatic final {
    std::uint16_t delay = 0;
    auto operator<=>(const SctMessageAutomatic&) const = default;
};

using SctMessageCompletion = std::variant<
    SctMessageContinue,
    SctMessageClose,
    SctMessageAutomatic>;

struct SctMessageDraft final {
    std::optional<std::string> headerUtf8{};
    std::vector<SctMessageTextRun> body{};
    SctMessagePosition position = SctMessagePosition::Default;
    bool disableFastForward = false;
    SctMessageCompletion completion = SctMessageContinue{};
    std::optional<std::uint16_t> openDuration{};
    std::optional<std::uint16_t> closeDuration{};
    auto operator<=>(const SctMessageDraft&) const = default;
};

enum class SctMessageProfileIssueCode {
    InvalidHeader,
    InvalidText,
    UnsupportedCommand,
    InvalidCommandArgument,
    MisplacedCommand,
    DuplicateCommand,
    ConflictingPosition,
    MissingCompletion,
    MultipleCompletions,
    TextAfterCompletion,
    NumericValueRequired,
    NumericValueOutOfRange,
    InvalidColor,
    TrailingFormatting,
};

struct SctMessageProfileIssue final {
    SctMessageProfileIssueCode code = SctMessageProfileIssueCode::InvalidText;
    std::optional<std::size_t> elementOrdinal{};
    std::string message{};
    auto operator<=>(const SctMessageProfileIssue&) const = default;
};

struct SctMessageProjection final {
    std::optional<SctMessageDraft> draft{};
    std::vector<SctMessageProfileIssue> issues{};
    [[nodiscard]] bool supported() const noexcept {
        return draft.has_value() && issues.empty();
    }
};

struct SctMessageMaterialization final {
    std::optional<spice::sct::SctMessage> message{};
    std::vector<SctMessageProfileIssue> issues{};
    [[nodiscard]] bool succeeded() const noexcept {
        return message.has_value() && issues.empty();
    }
};

class SctMessageAuthoringProfile final {
public:
    [[nodiscard]] static SctMessageProjection project(
        const spice::sct::SctMessage& message);
    [[nodiscard]] static SctMessageMaterialization materialize(
        const SctMessageDraft& draft);
};

using SctTextTarget = std::variant<
    spice::sct::SctStringId,
    spice::sct::SctSupplementaryTextId>;

using SctMessageTarget = SctTextTarget;

enum class SctMessageEditKind {
    Typing,
    Deletion,
    Paste,
    Replacement,
    Formatting,
    Header,
    Completion,
    Options,
};

}  // namespace salsa::core
