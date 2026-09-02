#pragma once

#include "SalsaCore/Project/LocalGameProject.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace salsa::qt {

enum class SctDocumentUpdateKind {
    Replacement,
    SourceStatus,
    RevisionTransition,
    VerifiedMaterialization,
};

struct SctDocumentUpdate final {
    SctDocumentUpdateKind kind = SctDocumentUpdateKind::Replacement;
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot{};
    std::optional<core::SctRevisionTransition> transition{};
    std::shared_ptr<const core::SctSemanticEditorProjection> semanticProjection{};
};

class SctDocumentController final : public QObject {
    Q_OBJECT

public:
    enum class SourceStatus { Current, Changed, Missing };
    Q_ENUM(SourceStatus)

    explicit SctDocumentController(QObject* parent = nullptr);
    ~SctDocumentController() override;

    [[nodiscard]] bool openDocument(
        core::LocalGameProject project, const core::AssetLocator& locator);
    [[nodiscard]] bool reloadDocument(
        core::LocalGameProject project, const core::AssetLocator& locator);
    [[nodiscard]] bool selectTextConvention(
        const core::AssetLocator& locator,
        spice::sct::SctKnownTextConvention convention);
    [[nodiscard]] bool insertInstructionAfter(
        const core::AssetLocator& locator,
        spice::sct::SctInstructionId anchorInstruction,
        std::uint16_t opcode);
    [[nodiscard]] bool deleteInstruction(
        const core::AssetLocator& locator,
        spice::sct::SctInstructionId instruction);
    [[nodiscard]] bool moveInstruction(
        const core::AssetLocator& locator,
        spice::sct::SctInstructionId instruction,
        core::SctInstructionMoveDirection direction);
    [[nodiscard]] bool replaceMessage(
        const core::AssetLocator& locator,
        const core::SctMessageTarget& target,
        const core::SctMessageDraft& draft,
        core::SctMessageEditKind kind);
    [[nodiscard]] bool addVirtualElse(
        const core::AssetLocator& locator,
        spice::sct::SctInstructionId controller);
    [[nodiscard]] bool addVirtualCase(
        const core::AssetLocator& locator,
        spice::sct::SctInstructionId controller);
    [[nodiscard]] bool setVirtualCaseValue(
        const core::AssetLocator& locator,
        core::SctAuthoredArmId arm,
        std::optional<std::int32_t> value);
    [[nodiscard]] bool removeVirtualArm(
        const core::AssetLocator& locator, core::SctAuthoredArmId arm);
    [[nodiscard]] bool insertInstructionIntoAuthoredArm(
        const core::AssetLocator& locator,
        core::SctAuthoredArmId arm, std::uint16_t opcode);
    [[nodiscard]] bool deleteOnlyInstructionFromAuthoredArm(
        const core::AssetLocator& locator, core::SctAuthoredArmId arm,
        spice::sct::SctInstructionId instruction);
    [[nodiscard]] bool insertInstructionIntoStructuredArm(
        const core::AssetLocator& locator,
        spice::sct::SctInstructionId controller,
        spice_sct_prototype::SctStructuredArmKind arm,
        std::uint16_t opcode);
    [[nodiscard]] bool undo(const core::AssetLocator& locator);
    [[nodiscard]] bool redo(const core::AssetLocator& locator);
    void synchronizeCatalog(const core::AssetCatalogSnapshot& catalog);
    void closeDocument(const core::AssetLocator& locator);
    void closeAll();
    void cancel();
    void setEditTimingsEnabled(bool enabled) noexcept;
    void setStructureTimingsEnabled(bool enabled) noexcept;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool contains(const core::AssetLocator& locator) const;
    [[nodiscard]] std::shared_ptr<const core::SctDocumentSnapshot> snapshot(
        const core::AssetLocator& locator) const;
    [[nodiscard]] std::shared_ptr<const core::SctSemanticEditorProjection>
        semanticProjection(const core::AssetLocator& locator) const;
    [[nodiscard]] std::optional<spice::sct::SctMessage> workingMessage(
        const core::AssetLocator& locator,
        const core::SctMessageTarget& target) const;
    [[nodiscard]] SourceStatus sourceStatus(const core::AssetLocator& locator) const;
    [[nodiscard]] bool structurallyValid(const core::AssetLocator& locator) const;
    [[nodiscard]] bool isDirty(const core::AssetLocator& locator) const;
    [[nodiscard]] bool canUndo(const core::AssetLocator& locator) const;
    [[nodiscard]] bool canRedo(const core::AssetLocator& locator) const;
    [[nodiscard]] std::optional<std::string> undoDescription(
        const core::AssetLocator& locator) const;
    [[nodiscard]] std::optional<std::string> redoDescription(
        const core::AssetLocator& locator) const;
    [[nodiscard]] std::vector<core::AssetLocator> dirtyLocators() const;
    [[nodiscard]] std::vector<core::AssetLocator> openLocators() const;
    [[nodiscard]] const std::vector<core::Diagnostic>& failureDiagnostics() const noexcept;
    [[nodiscard]] const std::vector<core::SctPipelineDiagnostic>& failurePipelineDiagnostics() const noexcept;

signals:
    void busyChanged();
    void documentChanged(const QString& identityKey, const SctDocumentUpdate& update);
    void documentClosed(const QString& identityKey);
    void focusRequested(const QString& identityKey);
    void operationCompleted(
        const QString& identityKey, bool success, bool cancelled, const QString& message);
    void editCompleted(const QString& identityKey, bool success, const QString& message);
    void selectionRequested(const QString& identityKey, int kind, qulonglong id);

private:
    enum class Operation { None, Opening, Reloading, Reimporting };
    struct DocumentState {
        core::AssetLocator locator;
        std::unique_ptr<core::SctEditSession> session;
        SourceStatus status = SourceStatus::Current;
        std::unique_ptr<QFutureWatcher<core::SctMaterializationResult>> materializationWatcher{};
        std::stop_source materializationStop{};
        std::uint64_t requestedMaterializationGeneration = 0;
        std::uint64_t runningMaterializationGeneration = 0;
        core::RevisionId requestedMaterializationRevision{};
        bool editBlocked = false;
    };

    void begin(Operation operation, const core::AssetLocator& locator);
    void onFinished();
    [[nodiscard]] DocumentState* findState(const core::AssetLocator& locator);
    [[nodiscard]] const DocumentState* findState(const core::AssetLocator& locator) const;
    [[nodiscard]] bool applyEditResult(
        DocumentState& state,
        core::SctEditResult result,
        QString successMessage);
    void requestMaterialization(DocumentState& state);
    void startMaterialization(const std::string& identityKey, DocumentState& state);
    void finishMaterialization(const std::string& identityKey, std::uint64_t generation);
    void retireMaterialization(DocumentState& state);

    QFutureWatcher<core::SctLoadResult> watcher_{};
    std::unordered_map<std::string, DocumentState> documents_{};
    std::optional<core::AssetLocator> runningLocator_{};
    std::stop_source stopSource_{};
    Operation operation_ = Operation::None;
    std::uint64_t generation_ = 0;
    std::uint64_t runningGeneration_ = 0;
    std::uint64_t nextMaterializationGeneration_ = 0;
    std::vector<std::unique_ptr<QFutureWatcher<core::SctMaterializationResult>>>
        retiredMaterializations_{};
    std::vector<core::Diagnostic> failureDiagnostics_{};
    std::vector<core::SctPipelineDiagnostic> failurePipelineDiagnostics_{};
    bool editTimingsEnabled_ = false;
    bool structureTimingsEnabled_ = false;
};

}  // namespace salsa::qt
