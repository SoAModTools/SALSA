#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctInspectionLocation.h"

#include "SpiceSCT/SctDocumentIndex.h"

#include <optional>
#include <string>
#include <vector>

namespace salsa::core {

struct SctOutlineItem final {
    std::string label{};
    std::string secondary{};
    SctNavigationTarget target{};
    std::vector<SctOutlineItem> children{};
};

struct SctPropertyItem final {
    std::string name{};
    std::string value{};
    std::string notes{};
    std::vector<SctPropertyItem> children{};
    std::optional<SctInspectionLocation> location{};
};

struct SctTextPreviewRun final {
    std::string text{};
    bool bold = false;
    std::optional<std::uint32_t> rgb{};
};

struct SctEntityPresentation final {
    std::string title{};
    std::string subtitle{};
    std::vector<SctPropertyItem> properties{};
    std::vector<SctTextPreviewRun> preview{};
};

class SctPresentationService final {
public:
    [[nodiscard]] static std::vector<SctOutlineItem> outline(
        const SctDocumentSnapshot& snapshot);
    [[nodiscard]] static SctEntityPresentation describe(
        const SctDocumentSnapshot& snapshot,
        SctNavigationTarget target);
    [[nodiscard]] static SctEntityPresentation describe(
        const SctDocumentSnapshot& snapshot,
        SctNavigationTarget target,
        const spice::sct::SctDocumentIndex& index);
};

}  // namespace salsa::core
