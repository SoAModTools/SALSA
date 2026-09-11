#pragma once
#include "SalsaCore/Authoring/SctAuthoringMaterializer.h"
#include "SalsaCore/Authoring/SctAuthoringCodec.h"
#include <functional>

namespace salsa::core {

struct SctAuthoringState final {
    SctAuthoringProject project;
    SctImportedPrograms programs;
};
struct SctAuthoringChange final {
    RevisionId before;
    RevisionId after;
    std::vector<SctScriptId> affectedScripts;
    bool committed = false;
};

// The sole live authoring authority. Editors operate on disposable physical
// projections and submit complete, revision-checked transactions here.
class SctAuthoringSession final {
public:
    using Command = std::function<Result<SctAuthoringState>(const SctAuthoringState&)>;
    [[nodiscard]] static Result<std::unique_ptr<SctAuthoringSession>> open(SctAuthoringState state);
    [[nodiscard]] const SctAuthoringState& state() const noexcept { return *current_; }
    [[nodiscard]] std::shared_ptr<const SctAuthoringState> capture() const noexcept { return current_; }
    [[nodiscard]] Result<SctAuthoringChange> execute(RevisionId expected, std::string description, const Command& command);
    [[nodiscard]] Result<SctAuthoringChange> undo();
    [[nodiscard]] Result<SctAuthoringChange> redo();
    [[nodiscard]] bool canUndo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ + 1 < history_.size(); }
    [[nodiscard]] std::string undoDescription() const;
    [[nodiscard]] std::string redoDescription() const;
    [[nodiscard]] bool isDirty() const;
    void markSaved(const SctAuthoringState& captured);
    [[nodiscard]] bool accepts(const SctAuthoringMaterializationResult& result) const;
    [[nodiscard]] static Result<void> validate(const SctAuthoringState& state);
private:
    explicit SctAuthoringSession(SctAuthoringState state);
    [[nodiscard]] Result<SctAuthoringChange> navigate(std::size_t cursor);
    struct Entry { std::shared_ptr<const SctAuthoringState> state; std::string description; };
    std::shared_ptr<const SctAuthoringState> current_;
    std::vector<Entry> history_;
    std::size_t cursor_ = 0;
    std::string saved_;
};
} // namespace salsa::core
