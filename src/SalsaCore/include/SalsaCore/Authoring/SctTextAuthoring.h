#pragma once
#include "SalsaCore/Authoring/SctSequenceAuthoring.h"
#include "SpiceSCT/SctTextCodec.h"

namespace salsa::core {
struct SctTextObservation final {
    spice::sct::SctTextEntityId target;
    std::string name, group, correspondenceKey;
    std::optional<std::string> languageKey;
    std::string explanation;
};
struct SctTextResourceView final {
    spice::sct::SctTextValue value;
    spice::sct::SctTextKind kind;
    spice::sct::SctTextStorage storage;
    std::vector<spice::sct::SctParameterSite> uses;
};
struct SctTextEdit final {
    SctTextResourceId text;
    spice::sct::SctTextValue value;
};
struct SctTextIncomingEdge final {
    SctActionId source;
    spice::sct::SctParameterAddress operand;
};
// Clone a selected instruction as the operation template. Parameters, result
// handling, scheduling and refresh are retained; only this text operand changes.
struct SctTextInsertion final {
    SctSequenceId sequence;
    SctActionId before, prototype;
    spice::sct::SctParameterAddress operand;
    SctTextResourceId text;
    // Physical insertion also redirects this explicitly selected reference,
    // including a switch case. All unselected references retain their targets.
    std::optional<SctTextIncomingEdge> incoming;
};
class SctTextAuthoring final {
  public:
    [[nodiscard]] static std::vector<SctTextObservation> observeNames(std::vector<SctTextObservation>);
    [[nodiscard]] static std::vector<SctTextObservation> observe(const spice::sct::SctDocument &);
    [[nodiscard]] static Result<SctAuthoringState> discover(const SctAuthoringState &, SctScriptId);
    [[nodiscard]] static Result<SctAuthoringState> createLanguage(const SctAuthoringState &, std::string name);
    [[nodiscard]] static Result<SctAuthoringState> renameLanguage(const SctAuthoringState &, SctLanguageGroupId,
                                                                  std::string name);
    [[nodiscard]] static Result<SctAuthoringState>
    assign(const SctAuthoringState &, const std::vector<SctTextResourceId> &, std::vector<SctLanguageGroupId>);
    [[nodiscard]] static Result<SctAuthoringState> link(const SctAuthoringState &, SctScriptId, std::string name,
                                                        std::vector<SctTextResourceId>,
                                                        std::optional<SctModuleId> = {});
    [[nodiscard]] static Result<SctTextResourceView> inspect(const SctAuthoringState &, SctTextResourceId);
    [[nodiscard]] static Result<SctAuthoringState> createText(const SctAuthoringState &, SctScriptId,
                                                              std::string indexedName, spice::sct::SctTextKind,
                                                              spice::sct::SctTextStorage, spice::sct::SctTextValue,
                                                              std::vector<SctLanguageGroupId>,
                                                              spice::sct::SctTextEncoding);
    [[nodiscard]] static Result<SctAuthoringState> edit(const SctAuthoringState &, const std::vector<SctTextEdit> &,
                                                        spice::sct::SctTextEncoding);
    [[nodiscard]] static Result<SctAuthoringState>
    insert(const SctAuthoringState &, const std::vector<SctTextInsertion> &, spice::sct::SctTextEncoding);
    [[nodiscard]] static Result<SctAuthoringState> split(const SctAuthoringState &, SctTextResourceId,
                                                         SctLanguageGroupId, std::string indexedName,
                                                         const std::vector<spice::sct::SctParameterSite> &selectedUses);
    [[nodiscard]] static Result<void> validateProgram(const SctAuthoringProject &, SctScriptId,
                                                      const spice::sct::SctDocument &);
};
} // namespace salsa::core
