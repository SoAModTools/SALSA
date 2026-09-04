#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctPresentation.h"
#include "SalsaCore/Persistence/WorkspaceSession.h"
#include "Sct/SctOutlineModel.h"
#include "Sct/SctParameterTableModel.h"
#include "Sct/SctStructuredOutlineModel.h"
#include "SpiceSCT/SctDocumentIndex.h"

#include <QWidget>
#include <QList>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTextEdit;
class QTabWidget;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

namespace salsa::qt {

class SctDocumentWidget final : public QWidget {
    Q_OBJECT

public:
    struct InstructionInsertionContext final {
        spice::sct::SctInstructionId anchor;
        bool allowReturn = false;
    };

    explicit SctDocumentWidget(core::AssetLocator locator, QWidget* parent = nullptr);

    using ParameterPresentationProvider = std::function<
        core::SctParameterTablePresentation(spice::sct::SctInstructionId)>;
    using ParameterCommitHandler = std::function<bool(
        const spice::sct::SctParameterSite&, std::string)>;
    void setParameterPresentationProvider(ParameterPresentationProvider provider);
    void setParameterCommitHandler(ParameterCommitHandler handler);

    [[nodiscard]] const core::AssetLocator& locator() const noexcept;
    void setSnapshot(
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        int sourceStatus);
    void installVerifiedSnapshot(
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        int sourceStatus);
    void applyTextOnlySnapshot(
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        int sourceStatus,
        const core::SctEditChangeSet& changes);
    [[nodiscard]] bool applyInstructionChanges(
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        int sourceStatus,
        const core::SctEditChangeSet& changes);
    void setSourceStatus(int sourceStatus);
    void selectTarget(core::SctNavigationTarget target, bool reveal = true);
    void selectTargets(std::span<const core::SctNavigationTarget> targets,
        bool reveal = true);
    [[nodiscard]] bool selectLocation(
        const core::SctInspectionLocation& location,
        bool reveal = true);
    void setEditingEnabled(bool enabled);
    void setSemanticProjection(
        std::shared_ptr<const core::SctSemanticEditorProjection> projection);
    void setStructuredDeveloperOptions(
        bool showBasicBlocks, bool showRejectedEvidence,
        bool showControlFlowInstructions = false);
    [[nodiscard]] std::optional<core::SctNavigationTarget> currentTarget() const noexcept;
    [[nodiscard]] core::SctDocumentView activeView() const noexcept;
    void setActiveView(core::SctDocumentView view);
    [[nodiscard]] bool containsTarget(core::SctNavigationTarget target) const;
    [[nodiscard]] QString targetLabel(core::SctNavigationTarget target) const;
    [[nodiscard]] std::optional<InstructionInsertionContext> insertionContext() const;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId> selectedInstruction() const;
    [[nodiscard]] std::vector<spice::sct::SctInstructionId>
        selectedInstructions() const;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId>
        rangeMoveAnchor(core::SctInstructionMoveDirection direction) const;
    [[nodiscard]] std::optional<core::SctMessageTarget> selectedMessageTarget() const;
    [[nodiscard]] std::optional<core::SctTextTarget> selectedTextTarget() const;
    [[nodiscard]] std::optional<spice::sct::SctSectionId> selectedSection() const;
    [[nodiscard]] std::vector<spice::sct::SctSectionId> selectedSections() const;
    [[nodiscard]] bool canEditSelectedMessage() const;
    [[nodiscard]] bool canDeleteSelected() const;
    [[nodiscard]] bool canMoveSelected(core::SctInstructionMoveDirection direction) const;

signals:
    void textConventionRequested(const QString& identityKey, int convention);
    void reloadRequested(const QString& identityKey);
    void becameActive(const QString& identityKey);
    void editContextChanged();
    void insertInstructionRequested(const QString& identityKey);
    void deleteInstructionRequested(const QString& identityKey);
    void moveInstructionRequested(const QString& identityKey, int direction);
    void moveInstructionRangeRequested(const QString& identityKey,
        const QList<qulonglong>& instructions, qulonglong anchor);
    void editMessageRequested(const QString& identityKey);
    void createScriptSectionRequested(const QString& identityKey);
    void createIndexedStringRequested(const QString& identityKey);
    void renameSectionRequested(const QString& identityKey);
    void deleteSectionRequested(const QString& identityKey);
    void moveSectionRequested(const QString& identityKey, int direction);
    void createFooterTextRequested(const QString& identityKey, int kind);
    void deleteTextRequested(const QString& identityKey);
    void addElseRequested(const QString& identityKey, qulonglong controller);
    void addCaseRequested(const QString& identityKey, qulonglong controller);
    void setCaseValueRequested(const QString& identityKey, qulonglong arm);
    void removeSemanticArmRequested(const QString& identityKey, qulonglong arm);
    void insertIntoSemanticArmRequested(const QString& identityKey, qulonglong arm);
    void insertIntoStructuredArmRequested(
        const QString& identityKey, qulonglong controller, int armKind);
    void returnSemanticArmToEmptyRequested(
        const QString& identityKey, qulonglong arm, qulonglong instruction);
    void parameterNavigationRequested(
        const QString& identityKey, int kind, qulonglong id);
    void advancedScptRequested(const QString& identityKey,
        qulonglong instruction, quint32 schemaIndex, int repeatedOrdinal);
    void changeParameterReferenceRequested(const QString& identityKey,
        qulonglong instruction, quint32 schemaIndex, int repeatedOrdinal);
    void replaceOpaqueParameterRequested(const QString& identityKey,
        qulonglong instruction, quint32 schemaIndex, int repeatedOrdinal,
        int editorKind);
    void addRepeatedGroupRequested(const QString& identityKey,
        qulonglong instruction, quint32 ordinal);
    void deleteRepeatedGroupRequested(const QString& identityKey,
        qulonglong instruction, quint32 ordinal);
    void moveRepeatedGroupRequested(const QString& identityKey,
        qulonglong instruction, quint32 ordinal, int direction);
    void navigationChanged(const QString& identityKey, int kind, qulonglong id);
    void activeViewChanged(const QString& identityKey, int view);

private:
    void rebuildOutline();
    void rebuildStructuredOutline(bool initialLoad);
    void markStructuredOutlinePending();
    void syncDocumentButtons();
    void showTarget(core::SctNavigationTarget target);
    void updateSourceBanner(int sourceStatus);
    void showParameterTable(spice::sct::SctInstructionId instruction,
        const core::SctEditChangeSet* changes = nullptr);
    QTreeWidgetItem* addPropertyItem(
        QTreeWidgetItem* parent,
        const core::SctPropertyItem& property);

    core::AssetLocator locator_;
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot_{};
    const spice::sct::SctDocumentIndex* index_ = nullptr;
    bool outlineReconciliationPending_ = false;
    std::optional<core::SctNavigationTarget> currentTarget_{};
    bool editingEnabled_ = false;
    QLabel* sourceBanner_ = nullptr;
    QLabel* conventionBanner_ = nullptr;
    QComboBox* conventionCombo_ = nullptr;
    QPushButton* applyConventionButton_ = nullptr;
    QPushButton* reloadButton_ = nullptr;
    QTreeView* outline_ = nullptr;
    SctOutlineModel* outlineModel_ = nullptr;
    QTabWidget* outlineTabs_ = nullptr;
    QLabel* structuredBanner_ = nullptr;
    QTreeView* structuredOutline_ = nullptr;
    SctStructuredOutlineModel* structuredOutlineModel_ = nullptr;
    QToolButton* insertInstructionButton_ = nullptr;
    QToolButton* deleteInstructionButton_ = nullptr;
    QToolButton* moveInstructionUpButton_ = nullptr;
    QToolButton* moveInstructionDownButton_ = nullptr;
    bool structuredOutlinePending_ = false;
    bool showStructuredBasicBlocks_ = false;
    bool showRejectedStructureEvidence_ = false;
    bool showSemanticControlFlowInstructions_ = false;
    std::shared_ptr<const core::SctSemanticEditorProjection> semanticProjection_{};
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QTreeWidget* properties_ = nullptr;
    QTreeView* parameterTable_ = nullptr;
    SctParameterTableModel* parameterTableModel_ = nullptr;
    ParameterPresentationProvider parameterPresentationProvider_{};
    ParameterCommitHandler parameterCommitHandler_{};
    QTextEdit* preview_ = nullptr;
    std::vector<std::pair<QTreeWidgetItem*, core::SctInspectionLocation>>
        propertyLocations_{};
};

}  // namespace salsa::qt
