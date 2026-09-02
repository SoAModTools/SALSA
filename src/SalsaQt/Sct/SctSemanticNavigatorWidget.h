#pragma once

#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctEditSession.h"
#include "SalsaCore/Sct/SctInspectionLocation.h"
#include "SpiceSCT/SctDocumentAnalysis.h"

#include <QWidget>

#include <array>
#include <cstdint>
#include <unordered_map>
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
    void installVerifiedDocument(
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
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentIndex& index,
        const spice::sct::SctSemanticUsageIndex& usage);
    void buildReferences(
        QTreeWidget& tree,
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentIndex& index,
        const spice::sct::SctSemanticUsageIndex& usage);
    void buildVariables(
        QTreeWidget& tree,
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentIndex& index,
        const spice::sct::SctSemanticUsageIndex& usage);
    void buildIncompleteEvidence(
        QTreeWidget& tree,
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentIndex& index,
        const spice::sct::SctSemanticUsageIndex& usage);
    void populateTrees(
        QTreeWidget& opcodes,
        QTreeWidget& references,
        QTreeWidget& variables,
        QTreeWidget& incomplete,
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentAnalysis& analysis);
    void resetIncrementalState(
        const spice::sct::SctDocument& document,
        const spice::sct::SctDocumentIndex& index);
    void removeContribution(
        const core::SctInstructionStructuralChange& change);
    void addContribution(
        const core::SctInstructionStructuralChange& change);
    void applyInstructionOrder(
        const core::SctInstructionStructuralChange& change);
    void reorderInstructionOccurrences(spice::sct::SctInstructionId instruction);
    [[nodiscard]] bool physicallyBefore(
        spice::sct::SctInstructionId left,
        spice::sct::SctInstructionId right) const;
    [[nodiscard]] QString incrementalInstructionName(
        spice::sct::SctInstructionId instruction) const;
    [[nodiscard]] QString incrementalInstructionContext(
        spice::sct::SctInstructionId instruction) const;
    static void reconcileTree(QTreeWidget& current, QTreeWidget& desired);

    struct InstructionPresentation final {
        std::uint16_t opcode = 0;
        std::uint64_t section = 0;
        QString name{};
        QString context{};
    };

    QString identityKey_{};
    QLabel* emptyLabel_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeWidget* opcodes_ = nullptr;
    QTreeWidget* references_ = nullptr;
    QTreeWidget* variables_ = nullptr;
    QTreeWidget* incomplete_ = nullptr;
    std::vector<std::uint64_t> sectionOrder_{};
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> instructionOrder_{};
    std::unordered_map<std::uint64_t, InstructionPresentation> instructionPresentation_{};
    bool reconciliationPending_ = false;
};

}  // namespace salsa::qt
