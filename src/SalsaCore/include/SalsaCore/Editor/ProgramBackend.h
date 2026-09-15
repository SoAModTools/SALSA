#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include <memory>
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
    Result<Change> history(Revision, bool, std::stop_token = {}) override;
    // Migration seam for tools not yet moved behind editor contracts.
    static Change describe(const program::ChangeReceipt &);

  private:
    std::shared_ptr<program::ProgramService> service_;
};
} // namespace salsa::core::editor
