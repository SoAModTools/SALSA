#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramService.h"
#include <algorithm>
namespace salsa::core::editor {
namespace ir = program;
template <class T> Result<T> featureFailure(std::string text) {
    return Result<T>::failure(
        {{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(text)}});
}
Result<FeatureInventory> ProgramBackend::features(Revision r, SctScriptId script, std::stop_token stop) {
    return service_->features(r, script, stop).get();
}
Result<std::vector<LanguageSuggestion>> ProgramBackend::suggestLanguages(Revision r, SctScriptId script,
                                                                         std::stop_token stop) {
    return service_->suggestLanguages(r, script, stop).get();
}
Result<std::vector<FeatureSite>> ProgramBackend::textUses(Revision r, SctTextResourceId id,
                                                          std::stop_token stop) {
    return service_->textUses(r, id, stop).get();
}
Result<SelectorPreview> ProgramBackend::previewSelectors(Revision r, SctActionId first, SctActionId last,
                                                         smores::script::KnownBytes values,
                                                         std::stop_token stop) {
    return service_->previewSelectors(r, first, last, std::move(values), stop).get();
}
Result<ModuleDetail> ProgramBackend::module(Revision r, SctModuleId id, std::stop_token stop) {
    return service_->featureModule(r, id, stop).get();
}
Result<std::vector<SctLocalActivation>> ProgramBackend::activationSuggestions(Revision r, SctScriptId script,
                                                                              std::stop_token stop) {
    return service_->activationSuggestions(r, script, stop).get();
}
Result<smores::script::Resource> ProgramBackend::inspectResource(Revision r, std::string asset,
                                                                 smores::script::ResourceRole role,
                                                                 std::stop_token stop) {
    if (!resourceLoader_)
        return featureFailure<smores::script::Resource>(
            "No dataset is attached; optional resource inspection is unavailable");
    auto result = resourceLoader_(std::move(asset), role, stop);
    if (result) {
        std::lock_guard lock(resourceMutex_);
        std::erase_if(inspected_, [&](const auto &x) {
            return x.first != r || x.second.identity.asset == result.value().identity.asset;
        });
        inspected_.push_back({r, result.value()});
    }
    return result;
}
Result<Change> ProgramBackend::featureEdit(Revision r, FeatureEdit edit, std::stop_token stop) {
    if (auto *selection = std::get_if<SelectResourceEntry>(&edit)) {
        bool fresh = false;
        {
            std::lock_guard lock(resourceMutex_);
            fresh = std::ranges::any_of(inspected_, [&](const auto &x) {
                return x.first == r && x.second.identity == selection->snapshot.identity &&
                       x.second.entries == selection->snapshot.entries;
            });
        }
        if (!fresh)
            return featureFailure<Change>(
                "Inspect the resource at the current revision before selecting an entry");
        auto current = resourceLoader_(selection->snapshot.identity.asset, selection->snapshot.role, stop);
        if (!current)
            return Result<Change>::failure(current.diagnostics());
        if (current.value().identity != selection->snapshot.identity ||
            current.value().entries != selection->snapshot.entries)
            return featureFailure<Change>("Resource changed; inspect and select its entry again");
    }
    auto result = service_->edit({r, {ir::SemanticOperation{std::move(edit)}}}, stop).get();
    if (!result)
        return Result<Change>::failure(result.diagnostics());
    return Result<Change>::success(describe(result.value()));
}
} // namespace salsa::core::editor
