#include "SalsaCore/Authoring/SctAuthoringSession.h"
#include <set>
#include <map>

namespace salsa::core {
namespace {
Diagnostic error(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(message)};
}
std::string semanticKey(SctAuthoringProject project) {
    project.revision = {1};
    auto encoded = SctAuthoringCodec::encode(project);
    return encoded ? encoded.value() : std::string{};
}
std::vector<SctScriptId> scripts(const SctAuthoringProject& a, const SctAuthoringProject& b) {
    std::set<SctScriptId> ids;
    for (const auto& script : a.scripts) ids.insert(script.id);
    for (const auto& script : b.scripts) ids.insert(script.id);
    return {ids.begin(), ids.end()};
}
std::map<std::uint64_t, unsigned> identities(const SctAuthoringProject& project) {
    std::map<std::uint64_t, unsigned> result;
    const auto add = [&](const auto& values, unsigned kind) { for (const auto& value : values) result.emplace(value.id.value, kind); };
    add(project.baselines, 0); add(project.scripts, 1); add(project.modules, 2); add(project.entrypoints, 3);
    add(project.ports, 4); add(project.connections, 5); add(project.contents, 6);
    add(project.sequences, 7); add(project.predicates, 8);
    add(project.languageGroups,10); add(project.texts,11); add(project.logicalTexts,12);
    add(project.storylines,13); add(project.meanings,14); add(project.explanations,15);
    for (const auto& story : project.storylines) add(story.steps,16);
    for (const auto& sequence : project.sequences) add(sequence.actions, 9);
    return result;
}
}
Result<void> SctAuthoringSession::validate(const SctAuthoringState& state) {
    auto encoded = SctAuthoringCodec::encode(state.project);
    if (!encoded) return Result<void>::failure(encoded.diagnostics());
    for (const auto& [name, data] : state.retainedEvidence)
        if (!name.starts_with("legacy/")) return Result<void>::failure(error("Retained evidence cannot replace authored records."));
    std::set<std::string> locators;
    for (const auto& script : state.project.scripts) {
        if (!locators.insert(state.project.find(script.baseline)->source.locator.identityKey()).second)
            return Result<void>::failure(error("A source asset is already owned by another imported script."));
        auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script.id);
        if (!working) return Result<void>::failure(working.diagnostics());
    }
    // Baselines unused by a script must still have an exact backing binding.
    for (const auto& baseline : state.project.baselines) {
        if (std::ranges::count_if(state.programs, [&](const auto& p) {
                return p && p->projectId() == state.project.id && p->baseline().id == baseline.id;
            }) != 1) return Result<void>::failure(error("Missing or duplicate baseline backing."));
        const auto& actual = (*std::ranges::find_if(state.programs, [&](const auto& p) { return p && p->baseline().id == baseline.id; }))->baseline();
        if (actual.importedDocument != baseline.importedDocument || actual.source.locator != baseline.source.locator
            || actual.source.revision != baseline.source.revision || actual.source.byteSize != baseline.source.byteSize
            || actual.datasetFingerprint != baseline.datasetFingerprint || actual.recipe != baseline.recipe
            || actual.textConvention != baseline.textConvention)
            return Result<void>::failure(error("Immutable baseline descriptor differs from its backing."));
    }
    if (state.programs.size() != state.project.baselines.size())
        return Result<void>::failure(error("Unowned baseline backing is not part of the project."));
    return Result<void>::success(encoded.diagnostics());
}
SctAuthoringSession::SctAuthoringSession(SctAuthoringState state)
    : current_(std::make_shared<const SctAuthoringState>(std::move(state))),
      history_{{current_, {}}}, saved_(semanticKey(current_->project)), currentKey_(saved_) {}
Result<std::unique_ptr<SctAuthoringSession>> SctAuthoringSession::open(SctAuthoringState state) {
    auto checked = validate(state);
    if (!checked) return Result<std::unique_ptr<SctAuthoringSession>>::failure(checked.diagnostics());
    return Result<std::unique_ptr<SctAuthoringSession>>::success(
        std::unique_ptr<SctAuthoringSession>(new SctAuthoringSession(std::move(state))), checked.diagnostics());
}
Result<SctAuthoringChange> SctAuthoringSession::execute(RevisionId expected, std::string description, const Command& command) {
    const auto before = current_->project.revision;
    if (expected != before || before.value == UINT64_MAX || !command)
        return Result<SctAuthoringChange>::failure(error("Project command revision is stale, exhausted, or absent."));
    try {
        auto candidate = command(*current_);
        if (!candidate) return Result<SctAuthoringChange>::failure(candidate.diagnostics());
        auto next = std::move(candidate).takeValue();
        if (next.project.id != current_->project.id || next.project.nextEntityId < current_->project.nextEntityId)
            return Result<SctAuthoringChange>::failure(error("A command cannot replace project identity or rewind its allocator."));
        for (const auto& module : current_->project.modules) {
            const auto* after = next.project.find(module.id);
            if (after && after->script != module.script) return Result<SctAuthoringChange>::failure(error("Module reparenting is not supported."));
        }
        for (const auto& entry : current_->project.entrypoints) {
            const auto* after = next.project.find(entry.id);
            if (after && after->script != entry.script) return Result<SctAuthoringChange>::failure(error("Entrypoint reparenting is not supported."));
            if (entry.binding && entry.binding->kind == SctEntrypointKind::Lifecycle
                && (!after || !after->enabled || after->name != entry.name || !after->binding
                    || after->binding->kind != SctEntrypointKind::Lifecycle || after->binding->sequence != entry.binding->sequence))
                return Result<SctAuthoringChange>::failure(error("Required lifecycle entry identities cannot be removed, renamed, or disabled."));
        }
        for (const auto& text : current_->project.texts) {
            const auto* after = next.project.find(text.id);
            if (after && (after->script != text.script || after->target != text.target))
                return Result<SctAuthoringChange>::failure(error("A text resource cannot be rebound to another script or physical resource."));
        }
        for (const auto& text : current_->project.logicalTexts) {
            const auto* after = next.project.find(text.id);
            if (after && after->script != text.script)
                return Result<SctAuthoringChange>::failure(error("Logical text reparenting is not supported."));
        }
        for (const auto& script : current_->project.scripts) if (next.project.find(script.id)) {
            auto prior = SctAuthoringMaterializer::workingState(current_->project,current_->programs,script.id);
            auto after = SctAuthoringMaterializer::workingState(next.project,next.programs,script.id);
            if (!prior || !after) return Result<SctAuthoringChange>::failure(error("Cannot validate lifecycle section identity."));
            for (const auto& section : prior.value().document->sections) if (section.nameBytes == "init" || section.nameBytes == "loop") {
                const auto found = std::ranges::find(after.value().document->sections,section.id,&spice::sct::SctDocumentSection::id);
                if (found == after.value().document->sections.end() || found->nameBytes != section.nameBytes
                    || !std::holds_alternative<spice::sct::SctScriptSectionContent>(found->content))
                    return Result<SctAuthoringChange>::failure(error("Required init/loop section identities cannot be removed or renamed."));
            }
        }
        for(const auto& story:current_->project.storylines) for(const auto& step:story.steps)
            for(const auto& afterStory:next.project.storylines) for(const auto& after:afterStory.steps)
                if(step.id==after.id && step.target!=after.target)
                    return Result<SctAuthoringChange>::failure(error("A story occurrence cannot silently change its target."));
        const auto priorIds = identities(current_->project);
        for (const auto& [id, kind] : identities(next.project)) {
            const auto found = priorIds.find(id);
            if ((found == priorIds.end() && id < current_->project.nextEntityId)
                || (found != priorIds.end() && found->second != kind))
                return Result<SctAuthoringChange>::failure(error("A command cannot reuse or change the type of an allocated identity."));
        }
        // Existing imports remain immutable even when a command supplies new backing objects.
        for (const auto& prior : current_->programs) {
            auto found = std::ranges::find_if(next.programs, [&](const auto& p) { return p && p->baseline().id == prior->baseline().id; });
            if (found != next.programs.end() && *found != prior)
                return Result<SctAuthoringChange>::failure(error("A command cannot replace an immutable imported baseline."));
        }
        const auto revision = std::max(before.value, next.project.revision.value);
        if (revision == UINT64_MAX) return Result<SctAuthoringChange>::failure(error("Project revision is exhausted."));
        next.project.revision = {revision + 1};
        auto checked = validate(next);
        if (!checked) return Result<SctAuthoringChange>::failure(checked.diagnostics());
        auto nextKey = semanticKey(next.project);
        if (nextKey == currentKey_)
            return Result<SctAuthoringChange>::success({before, before, {}, false});
        auto affected = scripts(current_->project, next.project);
        auto accepted = std::make_shared<const SctAuthoringState>(std::move(next));
        // Allocate the replacement history before publishing any state.
        auto history = history_;
        history.resize(cursor_ + 1);
        history.push_back({accepted, std::move(description)});
        history_.swap(history); ++cursor_; current_ = std::move(accepted); currentKey_ = std::move(nextKey);
        return Result<SctAuthoringChange>::success({before, current_->project.revision, std::move(affected), true}, checked.diagnostics());
    } catch (const std::exception& exception) {
        return Result<SctAuthoringChange>::failure(error(std::string("Project command failed: ") + exception.what()));
    }
}
Result<SctPreparedAuthoringChange> SctAuthoringSession::prepare(
    std::shared_ptr<const SctAuthoringState> captured, std::string description, const Command& command) {
    if (!captured) return Result<SctPreparedAuthoringChange>::failure(error("No captured authoring state."));
    SctAuthoringSession isolated(*captured);
    auto changed = isolated.execute(captured->project.revision, description, command);
    if (!changed) return Result<SctPreparedAuthoringChange>::failure(changed.diagnostics());
    SctPreparedAuthoringChange prepared;
    prepared.before_ = std::move(captured); prepared.after_ = isolated.capture();
    prepared.semanticKey_ = isolated.currentKey_;
    prepared.change_ = changed.value(); prepared.description_ = std::move(description);
    return Result<SctPreparedAuthoringChange>::success(std::move(prepared), changed.diagnostics());
}
Result<SctAuthoringChange> SctAuthoringSession::install(const SctPreparedAuthoringChange& prepared) {
    // Exact immutable capture also rejects ABA revisions and foreign sessions.
    if (prepared.before_ != current_ || !prepared.after_)
        return Result<SctAuthoringChange>::failure(error("Prepared authoring change is stale."));
    if (!prepared.change_.committed) return Result<SctAuthoringChange>::success(prepared.change_);
    auto history = history_; history.resize(cursor_ + 1);
    history.push_back({prepared.after_, prepared.description_});
    history_.swap(history); ++cursor_; current_ = prepared.after_; currentKey_ = prepared.semanticKey_;
    return Result<SctAuthoringChange>::success(prepared.change_);
}
Result<SctAuthoringChange> SctAuthoringSession::navigate(std::size_t cursor) {
    const auto before = current_->project.revision;
    if (before.value == UINT64_MAX) return Result<SctAuthoringChange>::failure(error("Project revision is exhausted."));
    auto next = *history_[cursor].state;
    next.project.revision = {before.value + 1};
    next.project.nextEntityId = std::max(next.project.nextEntityId, current_->project.nextEntityId);
    auto affected = scripts(current_->project, next.project);
    current_ = std::make_shared<const SctAuthoringState>(std::move(next)); cursor_ = cursor; currentKey_ = semanticKey(current_->project);
    return Result<SctAuthoringChange>::success({before, current_->project.revision, std::move(affected), true});
}
Result<SctAuthoringChange> SctAuthoringSession::undo() {
    if (!canUndo()) return Result<SctAuthoringChange>::failure(error("There is no project command to undo."));
    return navigate(cursor_ - 1);
}
Result<SctAuthoringChange> SctAuthoringSession::redo() {
    if (!canRedo()) return Result<SctAuthoringChange>::failure(error("There is no project command to redo."));
    return navigate(cursor_ + 1);
}
std::string SctAuthoringSession::undoDescription() const { return canUndo() ? history_[cursor_].description : std::string{}; }
std::string SctAuthoringSession::redoDescription() const { return canRedo() ? history_[cursor_ + 1].description : std::string{}; }
bool SctAuthoringSession::isDirty() const { return currentKey_ != saved_; }
void SctAuthoringSession::markSaved(const SctAuthoringState& captured) {
    if (captured.project.id == current_->project.id) saved_ = semanticKey(captured.project);
}
bool SctAuthoringSession::accepts(const SctAuthoringMaterializationResult& result) const {
    if (result.cancelled || result.project != current_->project.id || result.revision != current_->project.revision) return false;
    std::set<SctScriptId> seen;
    for (const auto& output : result.scripts) {
        if (!current_->project.find(output.script) || !seen.insert(output.script).second) return false;
        if (output.prepared && !output.prepared->realization.isFresh(
                {current_->project.id, current_->project.revision, output.script, output.prepared->realization.key.realization})) return false;
    }
    return !result.scripts.empty();
}
} // namespace salsa::core
