#pragma once
#include "SalsaCore/Authoring/SctAuthoringProject.h"
#include "SalsaCore/Editor/EditorIdentity.h"
#include "SalsaCore/Foundation/LoadingProgress.h"
#include "SalsaCore/Foundation/Result.h"
#include <filesystem>
#include <stop_token>
namespace salsa::core::editor {
struct LocalizationLanguage {
    SctLanguageGroupId language;
    std::optional<std::uint32_t> selector;
    bool included = true;
};
struct LocalizationUse {
    SctActionId action;
    spice::sct::SctParameterAddress operand;
    SctLogicalTextId message;
};
// One canonical executable arm/body, with a lowering recipe rather than copies.
// A switch may also retain its authored empty Default arm.
struct LocalizedBlock {
    SctActionId action;
    SctSequenceId body;
    bool switchSelection = false, independent = false, skipRefresh = true;
    std::vector<SctLanguageGroupId> languages;
    std::string explanation;
    bool unconditional = false; // An originally unguarded message stays unguarded with one output language.
};
struct LocalizationState {
    std::vector<LocalizationLanguage> languages;
    std::vector<LocalizationUse> uses;
    std::vector<LocalizedBlock> blocks;
};
struct TranslationCell {
    SctLogicalTextId message;
    std::string language;
    std::optional<spice::sct::SctTextValue> value; // missing, not an empty message
    SctScriptId script;                            // Only used for a new indexed donor message.
    std::string messageName;
};
struct LocalizationMessage {
    SctLogicalTextId id;
    SctScriptId script;
    std::string name, scriptName;
    std::vector<TranslationCell> translations;
    std::vector<std::string> correspondenceKeys;
};
struct LocalizationCatalog {
    Revision revision;
    std::vector<SctLanguageGroup> languages;
    LocalizationState settings;
    std::vector<LocalizationMessage> messages;
    std::vector<std::pair<SctActionId, std::string>> candidates;
    std::vector<std::string> issues;
};
struct LocalizationChange {
    bool discover = false;
    std::vector<SctActionId> promote;
    std::vector<SctActionId> expand;
    std::optional<std::vector<LocalizationLanguage>> languages;
    std::vector<TranslationCell> translations;
};
struct IndexedTranslation {
    SctLogicalTextId message;
    SctScriptId script;
    std::string source, key, suggestedLanguage;
    spice::sct::SctTextValue value;
};
struct IndexedTranslationReview {
    std::vector<IndexedTranslation> rows;
    std::vector<std::string> issues;
};
class LocalizationSct final {
  public:
    static Result<IndexedTranslationReview> read(const std::vector<std::filesystem::path> &,
                                                 const LocalizationCatalog &, std::stop_token = {},
                                                 SctLoadingObserver = {});
};
class LocalizationCsv final {
  public:
    static Result<std::string> encode(const LocalizationCatalog &, const std::vector<std::string> &languages);
    static Result<std::vector<TranslationCell>> decode(std::string_view, const LocalizationCatalog &);
    static Result<std::string> text(const spice::sct::SctTextValue &);
    static Result<spice::sct::SctTextValue> text(std::string_view);
};
} // namespace salsa::core::editor
