#pragma once

#include "SalsaCore/Sct/SctParameterAuthoring.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"

#include <QAbstractItemModel>
#include <QStyledItemDelegate>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace salsa::qt {

class SctParameterTableModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    using CommitHandler = std::function<std::optional<QString>(
        const core::SctParameterRowPresentation&, const QString&)>;

    explicit SctParameterTableModel(QObject* parent = nullptr);

    QModelIndex index(int row, int column,
        const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setCommitHandler(CommitHandler handler);
    void resetFrom(core::SctParameterTablePresentation presentation);
    bool apply(core::SctParameterTablePresentation presentation,
        const core::SctEditChangeSet& changes);
    [[nodiscard]] std::optional<QString> commit(
        const QModelIndex& index, const QString& text);
    [[nodiscard]] const core::SctParameterRowPresentation* parameter(
        const QModelIndex& index) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> groupOrdinal(
        const QModelIndex& index) const noexcept;
    [[nodiscard]] const core::SctParameterTablePresentation& presentation() const noexcept;

private:
    enum class NodeKind { Parameter, Group };
    struct Node final {
        NodeKind kind = NodeKind::Parameter;
        Node* parent = nullptr;
        core::SctParameterRowPresentation parameter{};
        std::uint32_t groupOrdinal = 0;
        std::vector<std::unique_ptr<Node>> children{};
    };

    [[nodiscard]] Node* nodeAt(int row, const QModelIndex& parent) const;
    [[nodiscard]] QModelIndex indexForNode(const Node* node, int column = 0) const;
    [[nodiscard]] int rowOf(const Node* node) const;
    [[nodiscard]] static std::unique_ptr<Node> makeParameter(
        core::SctParameterRowPresentation parameter, Node* parent = nullptr);
    [[nodiscard]] static std::unique_ptr<Node> makeGroup(
        core::SctRepeatedGroupPresentation group);
    [[nodiscard]] Node* findParameter(const spice::sct::SctParameterSite& site) const;
    void refreshFrom(core::SctParameterTablePresentation presentation);

    core::SctParameterTablePresentation presentation_{};
    std::vector<std::unique_ptr<Node>> fixed_{};
    std::vector<std::unique_ptr<Node>> groups_{};
    CommitHandler commitHandler_{};
};

class SctParameterItemDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit SctParameterItemDelegate(QObject* parent = nullptr);
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace salsa::qt
