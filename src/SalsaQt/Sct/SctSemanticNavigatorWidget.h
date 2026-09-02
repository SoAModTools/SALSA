#pragma once

#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctInspectionLocation.h"

#include <QWidget>

#include <array>
#include <memory>
#include <vector>

class QLabel;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

Q_DECLARE_METATYPE(salsa::core::SctInspectionLocation)

namespace salsa::qt {

class SctSemanticNavigatorWidget final : public QWidget {
    Q_OBJECT

public:
    explicit SctSemanticNavigatorWidget(QWidget* parent = nullptr);

    void setDocument(
        const core::AssetLocator& locator,
        const core::SctDocumentSnapshot& snapshot);
    void clear();

signals:
    void navigationRequested(
        const QString& identityKey,
        core::SctInspectionLocation location);
    void statusMessageRequested(const QString& message);

private:
    struct ItemNavigation final {
        QTreeWidgetItem* item = nullptr;
        int column = 0;
        core::SctInspectionLocation location;
    };

    void activate(QTreeWidgetItem* item, int column);
    void registerNavigation(
        QTreeWidgetItem* item,
        int column,
        core::SctInspectionLocation location);
    void buildOpcodes(
        const spice::sct::SctDocument& document,
        const core::SctSemanticUsageIndex& usage);
    void buildReferences(
        const spice::sct::SctDocument& document,
        const core::SctSemanticUsageIndex& usage);
    void buildVariables(
        const spice::sct::SctDocument& document,
        const core::SctSemanticUsageIndex& usage);
    void buildIncompleteEvidence(
        const spice::sct::SctDocument& document,
        const core::SctSemanticUsageIndex& usage);

    QString identityKey_{};
    QLabel* emptyLabel_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeWidget* opcodes_ = nullptr;
    QTreeWidget* references_ = nullptr;
    QTreeWidget* variables_ = nullptr;
    QTreeWidget* incomplete_ = nullptr;
    std::vector<ItemNavigation> navigation_{};
};

}  // namespace salsa::qt
