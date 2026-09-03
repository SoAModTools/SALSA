#pragma once

#include "SalsaCore/Sct/SctInspectionLocation.h"
#include "SpiceSCT/SctDocument.h"

#include <optional>
#include <string>
#include <vector>

namespace salsa::core {

class SctWorkingState;

enum class SctInlineParameterEditorKind {
    ReadOnly,
    EncodedUnsigned,
    EncodedSigned,
    EncodedHex,
    ConventionalScpt,
    TerminatedWords,
    Reference,
    PlainFooterText,
    AdvancedScpt,
};

struct SctParameterRowPresentation final {
    spice::sct::SctParameterSite site;
    std::string parameter;
    std::string value;
    std::string notes;
    SctInlineParameterEditorKind editor = SctInlineParameterEditorKind::ReadOnly;
    std::optional<SctInlineParameterEditorKind> replacementEditor{};
    std::optional<SctNavigationTarget> navigation{};
    std::optional<spice::sct::SctFooterEntryId> inlineFooterText{};
};

struct SctRepeatedGroupPresentation final {
    std::uint32_t ordinal = 0;
    std::vector<SctParameterRowPresentation> parameters;
};

struct SctParameterTablePresentation final {
    spice::sct::SctInstructionId instruction;
    std::vector<SctParameterRowPresentation> fixedParameters;
    std::vector<SctRepeatedGroupPresentation> repeatedGroups;
    bool supportsRepeatedGroups = false;
    std::uint32_t minimumRepeatedGroups = 0;
    bool repeatedGroupsManagedBySemanticEditor = false;
};

struct SctParameterParseResult final {
    std::optional<spice::sct::SctDocumentParameterValue> value{};
    std::string error{};
    std::vector<std::string> warnings{};

    [[nodiscard]] bool succeeded() const noexcept {
        return value.has_value() && error.empty();
    }
};

struct SctReferenceCandidate final {
    spice::sct::SctDocumentReferenceTarget target;
    std::string label;
};

class SctParameterAuthoringService final {
public:
    [[nodiscard]] static SctParameterTablePresentation project(
        const SctWorkingState& state,
        spice::sct::SctInstructionId instruction);
    [[nodiscard]] static SctParameterParseResult parseInline(
        const spice::sct::SctDocumentInstruction& instruction,
        const spice::sct::SctParameterSite& site,
        std::string text);
    [[nodiscard]] static SctParameterParseResult parseDraftValue(
        std::uint16_t opcode, const spice::sct::SctParameterAddress& address,
        const spice::sct::SctDocumentParameterValue& current,
        std::string text);
    [[nodiscard]] static bool equivalent(
        const spice::sct::SctDocumentParameterValue& left,
        const spice::sct::SctDocumentParameterValue& right);
    [[nodiscard]] static std::vector<SctReferenceCandidate> referenceCandidates(
        const SctWorkingState& state,
        const spice::sct::SctParameterSite& site);
    [[nodiscard]] static std::vector<SctReferenceCandidate> referenceCandidates(
        const SctWorkingState& state, std::uint16_t opcode,
        const spice::sct::SctParameterAddress& address);
};

} // namespace salsa::core
