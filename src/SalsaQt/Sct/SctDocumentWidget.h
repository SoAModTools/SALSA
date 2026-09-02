#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctPresentation.h"
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

private:
    void rebuildOutline();
    void reconcileOutline(const std::vector<core::SctOutlineItem>& outline);
    void reconcileOutlineChildren(
        QTreeWidgetItem* parent,
        const std::vector<core::SctOutlineItem>& desired);
    void updateOutlineItem(
        QTreeWidgetItem& item,
        const core::SctOutlineItem& desired);
    void showTarget(core::SctNavigationTarget target);
    void updateSourceBanner(int sourceStatus);
    QTreeWidgetItem* addOutlineItem(QTreeWidgetItem* parent, const core::SctOutlineItem& item);
    QTreeWidgetItem* addPropertyItem(
        QTreeWidgetItem* parent,
        const core::SctPropertyItem& property);

    core::AssetLocator locator_;
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot_{};
    std::optional<spice::sct::SctDocumentIndex> index_{};
    std::optional<core::SctNavigationTarget> currentTarget_{};
    bool editingEnabled_ = false;
    QLabel* sourceBanner_ = nullptr;
    QLabel* conventionBanner_ = nullptr;
    QComboBox* conventionCombo_ = nullptr;
    QPushButton* applyConventionButton_ = nullptr;
    QPushButton* reloadButton_ = nullptr;
    QTreeWidget* outline_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QTreeWidget* properties_ = nullptr;
    QTextEdit* preview_ = nullptr;
    std::vector<std::pair<QTreeWidgetItem*, core::SctInspectionLocation>>
        propertyLocations_{};
};

}  // namespace salsa::qt
