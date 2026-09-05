#pragma once

#include "SalsaCore/Sct/SctAuthoringCatalog.h"

#include <QDialog>

class QDialogButtonBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace salsa::qt {

class SctCatalogEditorDialog final : public QDialog {
public:
    explicit SctCatalogEditorDialog(
        core::SctPersonalCatalog catalog, QWidget* parent = nullptr);

    [[nodiscard]] const core::SctPersonalCatalog& catalog() const noexcept;

private:
    void populate();
    void resetSelected();
    void importLegacy();
    void accept() override;
    [[nodiscard]] bool collect(core::SctPersonalCatalog& result);

    core::SctPersonalCatalog catalog_{};
    QTreeWidget* tree_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace salsa::qt
