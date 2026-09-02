#pragma once

#include "SalsaCore/Sct/SctPresentation.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"

#include <QAbstractItemModel>

#include <memory>
#include <unordered_map>
#include <vector>

namespace salsa::qt {

class SctOutlineModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit SctOutlineModel(QObject* parent = nullptr);

    QModelIndex index(int row, int column,
        const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
        int role) const override;

    void resetFrom(const std::vector<core::SctOutlineItem>& outline,
        const spice::sct::SctDocument& document);
    [[nodiscard]] bool apply(const core::SctEditChangeSet& changes);
    [[nodiscard]] std::optional<core::SctNavigationTarget> target(
        const QModelIndex& index) const noexcept;
    [[nodiscard]] QModelIndex indexForTarget(
        core::SctNavigationTarget target) const;
    [[nodiscard]] const spice::sct::SctDocumentInstruction* instruction(
        spice::sct::SctInstructionId id) const noexcept;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId> previousInstruction(
        spice::sct::SctInstructionId id) const;
    [[nodiscard]] std::optional<spice::sct::SctInstructionId> nextInstruction(
        spice::sct::SctInstructionId id) const;

private:
    struct Node final {
        QString label{};
        QString secondary{};
        core::SctNavigationTarget target{};
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children{};
    };

    [[nodiscard]] static std::unique_ptr<Node> makeNode(
        const core::SctOutlineItem& item, Node* parent);
    void indexNode(Node& node);
    [[nodiscard]] QModelIndex indexForNode(const Node* node, int column = 0) const;
    [[nodiscard]] int rowOf(const Node* node) const;
    [[nodiscard]] Node* nodeFor(core::SctNavigationTarget target) const;
    [[nodiscard]] static QString key(core::SctNavigationTarget target);
    [[nodiscard]] static std::unique_ptr<Node> instructionNode(
        const spice::sct::SctDocumentInstruction& instruction, Node* parent);
    [[nodiscard]] bool applyOne(
        const core::SctInstructionStructuralChange& change);
    [[nodiscard]] bool applyOne(const core::SctSectionStructuralChange& change);
    [[nodiscard]] bool applyOne(const core::SctFooterEntryStructuralChange& change);
    [[nodiscard]] int insertionRow(
        Node& section, const core::SctInstructionPlacement& placement) const;
    [[nodiscard]] int sectionInsertionRow(const core::SctSectionPlacement& placement) const;
    [[nodiscard]] static std::unique_ptr<Node> sectionNode(
        const spice::sct::SctDocumentSection& section, Node* parent = nullptr);
    [[nodiscard]] static std::unique_ptr<Node> footerEntryNode(
        const spice::sct::SctDocumentFooterEntry& entry, Node* parent);
    void refreshSectionLabels();
    void refreshFooterLabels();

    std::vector<std::unique_ptr<Node>> roots_{};
    std::unordered_map<std::string, Node*> nodes_{};
    std::unordered_map<std::uint64_t, spice::sct::SctDocumentInstruction>
        instructionValues_{};
};

}  // namespace salsa::qt
