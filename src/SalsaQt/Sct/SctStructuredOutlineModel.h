#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SalsaCore/Sct/SctStructuredAuthoring.h"

#include <QAbstractItemModel>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace salsa::qt {

class SctStructuredOutlineModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    struct EditContext final {
        std::optional<spice::sct::SctInstructionId> controller{};
        std::optional<core::SctAuthoredArmId> authoredArm{};
        std::optional<spice_sct_prototype::SctStructuredRegionKind> regionKind{};
        std::optional<spice_sct_prototype::SctStructuredArmKind> armKind{};
        bool verified = false;
        bool virtualArm = false;
        bool needsValue = false;
        bool canReturnToEmpty = false;
    };
    explicit SctStructuredOutlineModel(QObject* parent = nullptr);

    QModelIndex index(int row, int column,
        const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
        int role) const override;

    void resetFrom(std::shared_ptr<const core::SctDocumentSnapshot> snapshot,
        std::shared_ptr<const core::SctSemanticEditorProjection> projection);
    void setDeveloperOptions(bool showBasicBlocks, bool showRejectedEvidence,
        bool showControlFlowInstructions);
    [[nodiscard]] std::optional<core::SctNavigationTarget> target(
        const QModelIndex& index) const noexcept;
    [[nodiscard]] QModelIndex indexForTarget(
        core::SctNavigationTarget target) const;
    [[nodiscard]] std::optional<EditContext> editContext(
        const QModelIndex& index) const noexcept;

private:
    struct Node final {
        QString label{};
        QString secondary{};
        QString tooltip{};
        std::optional<core::SctNavigationTarget> target{};
        bool suggested = false;
        std::optional<EditContext> editContext{};
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children{};
    };

    void rebuild();
    void appendItem(Node& parent,
        const spice_sct_prototype::SctStructuredOutlineItem& item,
        std::optional<spice::sct::SctInstructionId> controller = std::nullopt,
        bool verifiedRegion = false);
    void appendAuthoredArms();
    void indexNode(Node& node);
    [[nodiscard]] QModelIndex indexForNode(const Node* node, int column = 0) const;
    [[nodiscard]] int rowOf(const Node* node) const;
    [[nodiscard]] static QString key(core::SctNavigationTarget target);

    std::shared_ptr<const core::SctDocumentSnapshot> snapshot_{};
    std::shared_ptr<const core::SctSemanticEditorProjection> projection_{};
    bool showBasicBlocks_ = false;
    bool showRejectedEvidence_ = false;
    bool showControlFlowInstructions_ = false;
    std::vector<std::unique_ptr<Node>> roots_{};
    std::unordered_map<std::string, Node*> targets_{};
    std::unordered_map<std::uint64_t, Node*> regionsByController_{};
    std::vector<spice::sct::SctInstructionId> hiddenControlFlow_{};
};

}  // namespace salsa::qt
