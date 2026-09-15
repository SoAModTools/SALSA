#pragma once
#include "SalsaCore/Editor/EditorBackend.h"
#include <QObject>
#include <QPointer>
#include <memory>
class QWidget;
class QAction;
namespace salsa::qt {
class ExclusiveOperationCoordinator;
// Shared selection and revision notification channel; owns no authoring state.
class EditorSessionPresenter final : public QObject {
    Q_OBJECT
  public:
    EditorSessionPresenter(ExclusiveOperationCoordinator *, QWidget *operationHost);
    ~EditorSessionPresenter() override;
    void bind(std::shared_ptr<core::editor::Backend>, core::editor::Revision);
    void select(std::optional<core::editor::Subject>);
    void selectMany(std::vector<core::editor::Subject>, std::optional<core::editor::Subject>);
    const std::vector<core::editor::Subject> &selectionSet() const { return selectionSet_; }
    void accept(const core::editor::Change &);
    void replaceParameter(core::editor::ParameterKey, std::string);
    std::shared_ptr<core::editor::Backend> backend() const { return backend_; }
    core::editor::Revision revision() const { return revision_; }
    const std::optional<core::editor::Inspection> &inspection() const { return inspection_; }
    QAction *undoAction() const { return undo_; }
    QAction *redoAction() const { return redo_; }
    bool busy() const;
    const std::optional<core::editor::Subject> &selection() const { return selection_; }
    void execute(QString, core::editor::EditIntent, std::optional<core::editor::Revision> = {});
    void navigate(core::editor::Subject);
    QAction *backAction() const { return back_; }
    QAction *forwardAction() const { return forward_; }

  signals:
    void inspectionChanged();
    void sessionChanged();
    void navigationRequested(const salsa::core::editor::Subject &);
    void changed(const salsa::core::editor::Change &);
    void diagnostic(const QString &);
    void availabilityChanged();

  private:
    void history(bool);
    void submit(QString, std::function<core::Result<core::editor::Change>(std::stop_token)>);
    ExclusiveOperationCoordinator *operations_;
    QPointer<QWidget> host_;
    std::shared_ptr<core::editor::Backend> backend_;
    core::editor::Revision revision_, requestRevision_;
    bool reading_ = false;
    std::optional<core::editor::Subject> selection_;
    std::vector<core::editor::Subject> selectionSet_;
    std::optional<core::editor::Inspection> inspection_;
    std::stop_source readStop_;
    std::uint64_t selectionGeneration_ = 0, sessionGeneration_ = 0;
    QAction *undo_, *redo_, *back_, *forward_;
    std::vector<core::editor::Subject> navigation_;
    std::size_t navigationAt_ = 0;
    std::optional<core::editor::Subject> historyTarget_;
};
} // namespace salsa::qt
