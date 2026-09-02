#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctPresentation.h"
#include "Sct/SctOutlineModel.h"
#include "Sct/SctStructuredOutlineModel.h"
#include "SpiceSCT/SctDocumentIndex.h"

#include <QWidget>

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTextEdit;
class QTabWidget;
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
    [[nodiscard]] std::optional<InstructionInsertionContext> insertionContext() const;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId> selectedInstruction() const;
    [[nodiscard]] std::optional<core::SctMessageTarget> selectedMessageTarget() const;
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
    void editMessageRequested(const QString& identityKey);
    void addElseRequested(const QString& identityKey, qulonglong controller);
    void addCaseRequested(const QString& identityKey, qulonglong controller);
    void setCaseValueRequested(const QString& identityKey, qulonglong arm);
    void removeSemanticArmRequested(const QString& identityKey, qulonglong arm);
    void insertIntoSemanticArmRequested(const QString& identityKey, qulonglong arm);
    void insertIntoStructuredArmRequested(
        const QString& identityKey, qulonglong controller, int armKind);
    void returnSemanticArmToEmptyRequested(
        const QString& identityKey, qulonglong arm, qulonglong instruction);

private:
    void rebuildOutline();
    void rebuildStructuredOutline(bool initialLoad);
    void markStructuredOutlinePending();
    void showTarget(core::SctNavigationTarget target);
    void updateSourceBanner(int sourceStatus);
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
    bool structuredOutlinePending_ = false;
    bool showStructuredBasicBlocks_ = false;
    bool showRejectedStructureEvidence_ = false;
    bool showSemanticControlFlowInstructions_ = false;
    std::shared_ptr<const core::SctSemanticEditorProjection> semanticProjection_{};
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QTreeWidget* properties_ = nullptr;
    QTextEdit* preview_ = nullptr;
    std::vector<std::pair<QTreeWidgetItem*, core::SctInspectionLocation>>
        propertyLocations_{};
};

}  // namespace salsa::qt
