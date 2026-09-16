#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include <memory>
#include <functional>
#include <mutex>
namespace salsa::core::program {
class ProgramService;
struct ChangeReceipt;
} // namespace salsa::core::program
namespace salsa::core::editor {
class ProgramBackend final : public Backend {
  public:
    explicit ProgramBackend(std::shared_ptr<program::ProgramService>);
    Result<OutlinePage> outline(Revision, OutlineRequest, std::stop_token = {}) override;
    Result<Inspection> inspect(Revision, Subject, std::stop_token = {}) override;
    Result<Change> replaceParameter(Revision, ReplaceParameter, std::stop_token = {}) override;
    Result<NavigationPath> locate(Revision, Subject, ViewMode = ViewMode::Instructions) override;
    Result<TextDraft> text(Revision, SctTextResourceId, std::stop_token = {}) override;
    Result<StructureDraft> structure(Revision, SctActionId, std::stop_token = {}) override;
    Result<BrowsePage> browse(Revision, BrowseRequest, std::stop_token = {}) override;
    std::vector<Opcode> opcodes() const override;
    Result<Change> execute(Revision, EditIntent, std::stop_token = {}) override;
    Result<std::vector<std::byte>> copy(Revision, CopySelection, std::stop_token = {}) override;
    Result<FragmentReview> reviewPaste(Revision, Subject, std::span<const std::byte>, std::stop_token = {}) override;
    Result<Organization> organization(Revision, SctScriptId, std::stop_token = {}) override;
    Result<Change> history(Revision, bool, std::stop_token = {}) override;
    using ResourceLoader = std::function<Result<smores::script::Resource>(std::string, smores::script::ResourceRole, std::stop_token)>;
    void setResourceLoader(ResourceLoader loader) { resourceLoader_=std::move(loader); }
    Result<std::optional<SctTextResourceId>> translation(Revision, SctTextResourceId, SctLanguageGroupId,
                                                         std::stop_token = {}) override;
    Result<LocalizationCatalog> localizations(Revision, bool = false, std::stop_token = {}) override;
    Result<FeatureInventory> features(Revision, SctScriptId, std::stop_token = {}) override;
    Result<ModuleDetail> module(Revision, SctModuleId, std::stop_token = {}) override;
    Result<std::vector<LanguageSuggestion>> suggestLanguages(Revision, SctScriptId, std::stop_token = {}) override;
    Result<std::vector<FeatureSite>> textUses(Revision, SctTextResourceId, std::stop_token = {}) override;
    Result<std::vector<SctLocalActivation>> activationSuggestions(Revision, SctScriptId, std::stop_token = {}) override;
    Result<SelectorPreview> previewSelectors(Revision, SctActionId, SctActionId, smores::script::KnownBytes, std::stop_token = {}) override;
    Result<smores::script::Resource> inspectResource(Revision, std::string, smores::script::ResourceRole, std::stop_token = {}) override;
    Result<StoryCatalog> storyCatalog(Revision, std::stop_token = {}) override;
    Result<StoryView> story(Revision, StoryRequest, std::stop_token = {}) override;
    Result<InteractionView> interaction(Revision, SctScriptId, std::stop_token = {}) override;
    Result<ItemExplanation> explanation(Revision, Subject, std::stop_token = {}) override;
    Result<RouteDetail> routes(Revision, SctModuleId, std::stop_token = {}) override;
    Result<OutputReview> reviewOutput(Revision, std::vector<SctScriptId>, std::stop_token = {}) override;
    Result<OutputResult> publishOutput(OutputRequest, std::stop_token = {}, const SctLoadingObserver & = {}) override;
    // Migration seam for tools not yet moved behind editor contracts.
    static Change describe(const program::ChangeReceipt &);

  private:
    ResourceLoader resourceLoader_;
    std::vector<std::pair<Revision,smores::script::Resource>> inspected_;
    std::mutex resourceMutex_;
    Result<Change> featureEdit(Revision, FeatureEdit, std::stop_token);
    Result<Change> productivity(Revision, EditIntent, std::stop_token);
    std::shared_ptr<program::ProgramService> service_;
};
} // namespace salsa::core::editor
