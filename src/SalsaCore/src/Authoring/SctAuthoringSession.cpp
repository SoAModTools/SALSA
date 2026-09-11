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
    return result;
}
}
Result<void> SctAuthoringSession::validate(const SctAuthoringState& state) {
    auto encoded = SctAuthoringCodec::encode(state.project);
    if (!encoded) return Result<void>::failure(encoded.diagnostics());
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
      history_{{current_, {}}}, saved_(semanticKey(current_->project)) {}
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
        if (semanticKey(next.project) == semanticKey(current_->project))
            return Result<SctAuthoringChange>::success({before, before, {}, false});
        auto affected = scripts(current_->project, next.project);
        auto accepted = std::make_shared<const SctAuthoringState>(std::move(next));
        // Allocate the replacement history before publishing any state.
        auto history = history_;
        history.resize(cursor_ + 1);
        history.push_back({accepted, std::move(description)});
        history_.swap(history); ++cursor_; current_ = std::move(accepted);
        return Result<SctAuthoringChange>::success({before, current_->project.revision, std::move(affected), true}, checked.diagnostics());
    } catch (const std::exception& exception) {
        return Result<SctAuthoringChange>::failure(error(std::string("Project command failed: ") + exception.what()));
    }
}
Result<SctAuthoringChange> SctAuthoringSession::navigate(std::size_t cursor) {
    const auto before = current_->project.revision;
    if (before.value == UINT64_MAX) return Result<SctAuthoringChange>::failure(error("Project revision is exhausted."));
    auto next = *history_[cursor].state;
    next.project.revision = {before.value + 1};
    next.project.nextEntityId = std::max(next.project.nextEntityId, current_->project.nextEntityId);
    auto affected = scripts(current_->project, next.project);
    current_ = std::make_shared<const SctAuthoringState>(std::move(next)); cursor_ = cursor;
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
bool SctAuthoringSession::isDirty() const { return semanticKey(current_->project) != saved_; }
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
