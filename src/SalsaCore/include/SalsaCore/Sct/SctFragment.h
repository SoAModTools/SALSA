#pragma once

#include "SalsaCore/Foundation/Result.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"
#include "SalsaCore/Sct/SctWorkingState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

enum class SctFragmentKind { InstructionRange, SectionRange, SemanticUnits };

struct SctSemanticFragmentUnit final {
    SctSemanticNodeKind kind = SctSemanticNodeKind::Instruction;
    std::vector<spice::sct::SctInstructionId> instructions{};
    std::optional<spice::sct::SctStructuredRegionKind> regionKind{};
    std::optional<spice::sct::SctStructuredArmKind> armKind{};
    std::optional<std::int32_t> caseValue{};
    std::optional<spice::sct::SctInstructionId> entryInstruction{};
    std::optional<spice::sct::SctInstructionId> continuationInstruction{};
    std::vector<SctSemanticFragmentUnit> children{};
    auto operator<=>(const SctSemanticFragmentUnit&) const = default;
};

struct SctFragmentDependency final {
    spice::sct::SctParameterSite sourceSite;
    spice::sct::SctDocumentReferenceTarget target;
    spice::sct::SctExpectedReferenceTarget expectedTarget;
    std::optional<std::string> targetNameBytes{};
    auto operator<=>(const SctFragmentDependency&) const = default;
};

struct SctSemanticFragment final {
    SctFragmentKind kind = SctFragmentKind::InstructionRange;
    std::string sourceAssetIdentity{};
    std::vector<spice::sct::SctDocumentInstruction> instructions{};
    std::vector<spice::sct::SctDocumentSection> sections{};
    std::vector<SctAuthoredArm> authoredArms{};
    std::vector<SctEntityAnnotation> annotations{};
    std::vector<SctSectionFolder> folders{};
    std::vector<SctFragmentDependency> dependencies{};
    std::vector<SctSemanticFragmentUnit> semanticUnits{};
};

struct SctFragmentPasteDestination final {
    std::optional<spice::sct::SctInstructionId> instructionAfter{};
    std::optional<spice::sct::SctSectionId> sectionAfter{};
    std::vector<std::string> sectionNames{};
};

struct SctFragmentPastePlan final {
    SctSemanticOperationBatch document{};
    SctStructuredAuthoringOperationBatch authoring{};
    std::vector<SctNavigationTarget> insertedSelection{};
    std::size_t unboundReferenceCount = 0;
};

class SctFragmentCodec final {
public:
    static constexpr std::string_view FormatId = "jahorta.salsa.sct-fragment";
    static constexpr std::uint32_t SchemaVersion = 4;
    static constexpr std::uint32_t PreviousSchemaVersion = 3;
    static constexpr std::uint32_t LegacySchemaVersion = 2;
    static constexpr std::size_t MaximumBytes = 64u * 1024u * 1024u;
    static constexpr std::string_view MimeType =
        "application/vnd.jahorta.salsa.sct-fragment+json";

    [[nodiscard]] static Result<std::vector<std::byte>> serialize(
        const SctSemanticFragment& fragment);
    [[nodiscard]] static Result<SctSemanticFragment> deserialize(
        std::span<const std::byte> bytes);
};

class SctFragmentService final {
public:
    [[nodiscard]] static Result<SctSemanticFragment> captureInstructions(
        const SctWorkingState& state,
        const SctStructuredAuthoringState& authoring,
        std::string sourceAssetIdentity,
        std::span<const spice::sct::SctInstructionId> instructions);
    [[nodiscard]] static Result<SctSemanticFragment> captureSections(
        const SctWorkingState& state,
        const SctStructuredAuthoringState& authoring,
        std::string sourceAssetIdentity,
        std::span<const spice::sct::SctSectionId> sections);
    [[nodiscard]] static std::vector<std::string> suggestSectionNames(
        const SctWorkingState& state, const SctSemanticFragment& fragment);
    [[nodiscard]] static Result<SctFragmentPastePlan> planPaste(
        const SctWorkingState& state,
        const SctStructuredAuthoringState& authoring,
        std::string_view destinationAssetIdentity,
        const SctSemanticFragment& fragment,
        const SctFragmentPasteDestination& destination);
};

struct SctSnippet final {
    std::string name{};
    std::string description{};
    SctSemanticFragment fragment{};
};

class SctSnippetCodec final {
public:
    static constexpr std::string_view FormatId = "jahorta.salsa.sct-snippet";
    static constexpr std::uint32_t SchemaVersion = 1;

    [[nodiscard]] static Result<std::vector<std::byte>> serialize(
        const SctSnippet& snippet);
    [[nodiscard]] static Result<SctSnippet> deserialize(
        std::span<const std::byte> bytes);
};

class SctSnippetStore final {
public:
    explicit SctSnippetStore(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] Result<std::vector<SctSnippet>> loadAll() const;
    [[nodiscard]] Result<void> save(const SctSnippet& snippet) const;
    [[nodiscard]] Result<void> remove(std::string_view name) const;

private:
    std::filesystem::path root_{};
};

}  // namespace salsa::core
