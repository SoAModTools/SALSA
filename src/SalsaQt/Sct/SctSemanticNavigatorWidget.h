#pragma once

#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctInspectionLocation.h"
#include "SalsaCore/Sct/SctSemanticUsageIndex.h"

#include "SpiceSCT/SctDocumentIndex.h"

#include <QWidget>

#include <array>
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
    [[nodiscard]] bool applyInstructionChanges(
        const core::AssetLocator& locator,
        const core::SctDocumentSnapshot& snapshot,
        const core::SctEditChangeSet& changes);
    void clear();

signals:
    void navigationRequested(
        const QString& identityKey,
        core::SctInspectionLocation location);
    void statusMessageRequested(const QString& message);

private:
    void activate(QTreeWidgetItem* item, int column);
    void registerNavigation(
        QTreeWidgetItem* item,
        int column,
        core::SctInspectionLocation location);
    void buildOpcodes(
        QTreeWidget& tree,
        const spice::sct::SctDocumentIndex& index,
        const core::SctSemanticUsageIndex& usage);
    void buildReferences(
        QTreeWidget& tree,
        const spice::sct::SctDocumentIndex& index,
        const core::SctSemanticUsageIndex& usage);
    void buildVariables(
        QTreeWidget& tree,
        const spice::sct::SctDocumentIndex& index,
        const core::SctSemanticUsageIndex& usage);
    void buildIncompleteEvidence(
        QTreeWidget& tree,
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentIndex& index,
        const core::SctSemanticUsageIndex& usage);
    void populateTrees(
        QTreeWidget& opcodes,
        QTreeWidget& references,
        QTreeWidget& variables,
        QTreeWidget& incomplete,
        const spice::sct::SctDocument& document);
    static void reconcileTree(QTreeWidget& current, QTreeWidget& desired);

    QString identityKey_{};
    QLabel* emptyLabel_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeWidget* opcodes_ = nullptr;
    QTreeWidget* references_ = nullptr;
    QTreeWidget* variables_ = nullptr;
    QTreeWidget* incomplete_ = nullptr;
};

}  // namespace salsa::qt
