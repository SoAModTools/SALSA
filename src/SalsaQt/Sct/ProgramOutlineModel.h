#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include <QAbstractItemModel>
#include <functional>
#include <map>
#include <memory>

namespace salsa::qt {
class ProgramOutlineModel final : public QAbstractItemModel {
  public:
    explicit ProgramOutlineModel(QObject *parent = nullptr);
    void bind(std::shared_ptr<core::editor::Backend>, core::editor::Revision, core::SctScriptId);
    bool pending(const QModelIndex &index) const;
    void apply(const core::editor::Change &);
    const core::editor::OutlineRow *row(const QModelIndex &) const;
    QModelIndex find(const core::editor::Subject &) const;
    std::function<void()> ready;
    std::function<void(QString)> diagnostic;
    std::function<void(std::uint64_t, std::size_t)> measured;
    QModelIndex index(int, int, const QModelIndex & = {}) const override;
    QModelIndex parent(const QModelIndex &) const override;
    int rowCount(const QModelIndex & = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return 2; }
    QVariant data(const QModelIndex &, int = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int) const override;
    bool canFetchMore(const QModelIndex &) const override;
    void fetchMore(const QModelIndex &) override;
    bool hasChildren(const QModelIndex & = {}) const override;

  private:
    struct Node {
        core::editor::OutlineRow row;
        std::size_t parent = 0, ordinal = 0, total = 0;
        std::vector<std::size_t> children;
        bool fetched = false, loading = false, live = true;
        std::uint64_t request = 0;
    };
    std::vector<Node> nodes_;
    std::vector<std::size_t> free_;
    std::shared_ptr<core::editor::Backend> service_;
    core::editor::Revision revision_;
    std::uint64_t generation_ = 0, nextRequest_ = 0;
    std::size_t slot(const QModelIndex &) const;
    QModelIndex indexFor(std::size_t) const;
    void discard(std::size_t);
    void resetChildren(std::size_t);
};
} // namespace salsa::qt
