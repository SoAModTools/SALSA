#pragma once
#include "SalsaCore/Authoring/SctAuthoringProject.h"
#include "SalsaCore/Editor/EditorIdentity.h"
#include "SalsaCore/Editor/Localization.h"
namespace salsa::core::editor {
struct BodyItem {
    SctSequenceId id;
    std::string name;
    std::optional<program::SectionId> section;
};
struct ModuleItem {
    SctModuleId id;
    SctScriptId script;
    std::string name;
    std::vector<SctSequenceId> bodies;
};
struct EntryItem {
    SctEntrypointId id;
    SctScriptId script;
    std::string name;
    SctEntrypointKind kind = SctEntrypointKind::NamedSection;
    SctSequenceId body;
    bool enabled = true;
    SctEntrypointActivation activation = SctUnresolvedBinding{"", "Select activation boundary", {}};
    std::optional<SctPortId> response;
    std::optional<std::variant<smores::script::Entry, SctUnresolvedBinding>> provider;
    bool required() const {
        return kind == SctEntrypointKind::Lifecycle && (name == "init" || name == "loop");
    }
};
struct TextItem {
    SctTextResourceId id;
    std::string name, group;
    bool footer = false, explicitAssignment = false;
    std::vector<SctLanguageGroupId> languages;
};
struct DefinitionItem {
    Subject subject;
    std::string name;
    std::vector<Subject> uses;
};
struct FeatureInventory {
    Revision revision;
    SctScriptId script;
    std::vector<BodyItem> bodies;
    std::vector<ModuleItem> modules;
    std::vector<EntryItem> entries;
    std::vector<TextItem> texts;
    std::vector<SctLanguageGroup> languages;
    std::vector<SctLogicalText> logical;
    std::vector<SctScriptContext::ResourceAssociation> resources;
    std::vector<DefinitionItem> definitions;
};
struct FeatureSite {
    SctActionId action;
    spice::sct::SctParameterAddress operand;
    auto operator<=>(const FeatureSite &) const = default;
};
struct ModuleDetail {
    Revision revision;
    ModuleItem module;
    std::vector<Subject> callers, dependencies;
};
struct LanguageSuggestion {
    SctTextResourceId text;
    std::string key, correspondence, explanation;
};
struct SelectorRow {
    SctActionId action;
    std::uint32_t destination;
    spice::sct::SctCanonicalExpression expression;
    bool scheduled = false;
};
struct SelectorPreview {
    Revision revision;
    smores::script::KnownBytes values;
    std::vector<SelectorRow> assignments;
    std::vector<smores::script::ResourceSelection> tiles;
    smores::script::ResourceSelection overworldScript, overworldBase;
};
struct WriteModule {
    ModuleItem module;
    bool createEmptyBody = false;
};
struct RemoveModule {
    SctModuleId id;
};
struct WriteEntry {
    EntryItem entry;
    std::optional<FeatureSite> responseSite;
    std::optional<SctSequenceId> responseBody;
};
struct RemoveEntry {
    SctEntrypointId id;
};
struct RepairRoot {
    program::SectionId section;
    std::string name;
};
struct WriteLanguage {
    SctLanguageGroup group;
};
struct RemoveLanguage {
    SctLanguageGroupId id;
};
struct AssignTextGroups {
    std::vector<SctTextResourceId> texts;
    std::vector<SctLanguageGroupId> languages;
};
struct ApplyLanguageSuggestions {
    SctScriptId script;
    std::vector<SctTextResourceId> texts;
};
struct WriteLogicalText {
    SctLogicalText item;
};
struct RemoveLogicalText {
    SctLogicalTextId id;
};
struct SplitText {
    SctTextResourceId text;
    SctLanguageGroupId language;
    std::string name;
    std::vector<FeatureSite> uses;
};
struct BatchTextValue {
    SctTextResourceId text;
    spice::sct::SctTextValue value;
};
struct WriteTextBatch {
    std::vector<BatchTextValue> edits;
};
struct InsertTextUse {
    FeatureSite prototype;
    SctActionId before;
    SctTextResourceId text;
    std::optional<FeatureSite> incoming;
};
struct InsertTextUses {
    std::vector<InsertTextUse> edits;
};
struct SetResources {
    SctScriptId script;
    std::vector<SctScriptContext::ResourceAssociation> associations;
};
struct SelectResourceEntry {
    SctEntrypointId entry;
    smores::script::Resource snapshot;
    std::uint64_t tableIndex;
};
struct SelectorWrite {
    std::optional<SctActionId> replace;
    std::uint32_t destination;
    spice::sct::SctCanonicalExpression expression;
};
struct WriteSelectors {
    SctActionId before;
    std::vector<SelectorWrite> edits;
};
struct SetArrival {
    program::ConditionId condition;
    std::string stem;
    std::optional<std::uint32_t> encoded;
};
using FeatureEdit =
    std::variant<LocalizationChange, SetArrival, WriteModule, RemoveModule, WriteEntry, RemoveEntry,
                 RepairRoot, WriteLanguage, RemoveLanguage, AssignTextGroups, ApplyLanguageSuggestions,
                 WriteLogicalText, RemoveLogicalText, SplitText, WriteTextBatch, InsertTextUses, SetResources,
                 SelectResourceEntry, WriteSelectors>;
} // namespace salsa::core::editor
