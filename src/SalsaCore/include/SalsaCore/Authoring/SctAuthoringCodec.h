#pragma once
#include "SalsaCore/Authoring/SctAuthoringProject.h"

namespace salsa::core {

class SctAuthoringCodec final {
public:
    static constexpr std::string_view Format = "jahorta.salsa.authoring-project";
    static constexpr std::uint32_t SchemaVersion = 1;
    [[nodiscard]] static Result<std::string> encode(const SctAuthoringProject& project);
    [[nodiscard]] static Result<SctAuthoringProject> decode(std::string_view json);
};
class SctAuthoringPresentationCodec final {
public:
    static constexpr std::string_view Format = "jahorta.salsa.authoring-presentation";
    static constexpr std::uint32_t SchemaVersion = 1;
    [[nodiscard]] static Result<std::string> encode(
        const SctAuthoringPresentation& presentation, const SctAuthoringProject& project);
    [[nodiscard]] static Result<SctAuthoringPresentation> decode(
        std::string_view json, const SctAuthoringProject& project);
};

} // namespace salsa::core
