#include "Editor/ScriptOutlineComponent.h"
#include "Editor/EditorSessionPresenter.h"
#include <QHeaderView>
namespace salsa::qt {
ScriptOutlineComponent::ScriptOutlineComponent(EditorSessionPresenter *session, QWidget *parent)
    : QTreeView(parent), session_(session), model_(new ProgramOutlineModel(this)) {
    setModel(model_); setUniformRowHeights(true);
    header()->setSectionResizeMode(0, QHeaderView::Stretch); setColumnHidden(1, true);
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (selectionChanged) selectionChanged();
    });
    connect(this, &QTreeView::doubleClicked, this, [this] { if (editRequested) editRequested(); });
    connect(session_, &EditorSessionPresenter::changed, model_, &ProgramOutlineModel::apply);
}
void ScriptOutlineComponent::bind(core::SctScriptId script) {
    model_->bind(session_->backend(), session_->revision(), script);
}
void ScriptOutlineComponent::activate() {
    auto *row = model_->row(currentIndex());
    session_->select(row ? std::optional(row->subject) : std::nullopt);
}
}
