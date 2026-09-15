#pragma once
#include "SalsaCore/Foundation/Result.h"
#include <compare>
#include <cstdint>
#include <string_view>
namespace salsa::core {
template<class Tag> struct SctAuthoringId final {
    std::uint64_t value = 0;
    [[nodiscard]] bool valid() const noexcept { return value != 0; }
    auto operator<=>(const SctAuthoringId&) const = default;
};
using SctScriptId = SctAuthoringId<struct AuthoringScriptTag>;
using SctModuleId = SctAuthoringId<struct AuthoringModuleTag>;
using SctEntrypointId = SctAuthoringId<struct AuthoringEntrypointTag>;
using SctPortId = SctAuthoringId<struct AuthoringPortTag>;
using SctConnectionId = SctAuthoringId<struct AuthoringConnectionTag>;
using SctContentId = SctAuthoringId<struct AuthoringContentTag>;
using SctSequenceId = SctAuthoringId<struct AuthoringSequenceTag>;
using SctActionId = SctAuthoringId<struct AuthoringActionTag>;
using SctPredicateId = SctAuthoringId<struct AuthoringPredicateTag>;
using SctLanguageGroupId = SctAuthoringId<struct AuthoringLanguageGroupTag>;
using SctTextResourceId = SctAuthoringId<struct AuthoringTextResourceTag>;
using SctLogicalTextId = SctAuthoringId<struct AuthoringLogicalTextTag>;
using SctBaselineId = SctAuthoringId<struct AuthoringBaselineTag>;
using SctStoryStepId = SctAuthoringId<struct AuthoringStoryStepTag>;
using SctStorylineId = SctAuthoringId<struct AuthoringStorylineTag>;
using SctMeaningId = SctAuthoringId<struct AuthoringMeaningTag>;
using SctExplanationId = SctAuthoringId<struct AuthoringExplanationTag>;

[[nodiscard]] bool validSctAuthoringUuid(std::string_view value) noexcept;
[[nodiscard]] Result<std::string> generateSctAuthoringUuid();
template<class Tag> struct SctAuthoringUuid final {
    std::string value;
    [[nodiscard]] bool valid() const noexcept { return validSctAuthoringUuid(value); }
    auto operator<=>(const SctAuthoringUuid&) const = default;
};
using SctAuthoringProjectId = SctAuthoringUuid<struct AuthoringProjectTag>;
using SctImportedDocumentId = SctAuthoringUuid<struct ImportedDocumentTag>;
using SctRealizationId = SctAuthoringUuid<struct RealizationTag>;

}
