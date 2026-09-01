#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctPresentation.h"

#include <QWidget>

#include <memory>
#include <optional>

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
    explicit SctDocumentWidget(core::AssetLocator locator, QWidget* parent = nullptr);

    [[nodiscard]] const core::AssetLocator& locator() const noexcept;
    void setSnapshot(
        std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        int sourceStatus);
    void selectTarget(core::SctNavigationTarget target);

signals:
    void textConventionRequested(const QString& identityKey, int convention);
    void reloadRequested(const QString& identityKey);
    void becameActive(const QString& identityKey);

private:
    void rebuildOutline();
    void showTarget(core::SctNavigationTarget target);
    void updateSourceBanner(int sourceStatus);
    QTreeWidgetItem* addOutlineItem(QTreeWidgetItem* parent, const core::SctOutlineItem& item);

    core::AssetLocator locator_;
    std::shared_ptr<const core::SctDocumentSnapshot> snapshot_{};
    std::optional<core::SctNavigationTarget> currentTarget_{};
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
};

}  // namespace salsa::qt
