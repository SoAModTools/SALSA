#include "Editor/SupportingSequenceEditor.h"
#include "Editor/EditorSessionPresenter.h"
#include "Editor/ScriptEditingActions.h"
#include "Editor/ScriptOutlineComponent.h"
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
namespace salsa::qt {
namespace ed = core::editor;
SupportingSequenceEditor::SupportingSequenceEditor(EditorSessionPresenter *session, QWidget *parent)
    : QWidget(parent), session_(session) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(tr("Supporting instructions"), this));
    phases_ = new QComboBox(this);
    layout->addWidget(phases_);
    timing_ = new QLabel(this);
    timing_->setWordWrap(true);
    layout->addWidget(timing_);
    outline_ = new ScriptOutlineComponent(session, this);
    layout->addWidget(outline_);
    editing_ = new ScriptEditingActions(session, this);
    outline_->editRequested = [this] {
        if (editing_->editAction()->isEnabled()) editing_->editAction()->trigger();
    };
    auto *toolbar = new QToolBar(this);
    layout->addWidget(toolbar);
    append_ = toolbar->addAction(tr("Insert at phase start…"));
    selectPhase_ = toolbar->addAction(tr("Select phase"));
    toolbar->addAction(editing_->insertAction());
    toolbar->addAction(editing_->structureAction());
    toolbar->addAction(editing_->editAction());
    toolbar->addAction(editing_->branchesAction());
    toolbar->addAction(editing_->timingAction());
    remove_ = toolbar->addAction(tr("Delete selection"));
    ownerAction_ = toolbar->addAction(tr("Return to construct"));
    connect(append_, &QAction::triggered, this, [this] {
        auto n = phases_->currentIndex();
        if (n >= 0) editing_->insertInto(sequences_.at(n).sequence);
    });
    connect(selectPhase_, &QAction::triggered, this, [this] {
        auto n = phases_->currentIndex();
        if (n >= 0) session_->select(sequences_.at(n).sequence);
    });
    connect(remove_, &QAction::triggered, this, [this] {
        auto items = outline_->selectedItems();
        if (!items.empty()) session_->execute(tr("Delete supporting instructions"), ed::DeleteSelection{items});
    });
    connect(ownerAction_, &QAction::triggered, this, [this] {
        if (owner_) session_->navigate(*owner_);
    });
    connect(phases_, &QComboBox::currentIndexChanged, this, [this](int n) { showPhase(n, true); });
    connect(session_, &EditorSessionPresenter::inspectionChanged, this, [this] { refresh(); });
    connect(session_, &EditorSessionPresenter::availabilityChanged, this, [this] {
        const bool ready = session_->backend() && !session_->busy();
        append_->setEnabled(ready); remove_->setEnabled(ready);
    });
    connect(session_, &EditorSessionPresenter::sessionChanged, this, [this] {
        outline_->cancelReveal();
        owner_.reset(); sequences_.clear(); hide();
    });
    hide();
}
void SupportingSequenceEditor::showPhase(int n, bool select) {
    if (n < 0 || n >= static_cast<int>(sequences_.size())) return;
    const auto &phase = sequences_[n];
    timing_->setText(QString::fromStdString(phase.timing));
    outline_->bindSequence(script_, phase.sequence);
    if (select) session_->select(phase.sequence);
}
void SupportingSequenceEditor::refresh() {
    const auto &i = session_->inspection();
    outline_->cancelReveal();
    if (!i) {
        // Selection inspection is asynchronous. Keep the current scope during
        // that request, so selecting its children never destroys their view.
        if (!session_->selection()) { owner_.reset(); sequences_.clear(); hide(); }
        return;
    }
    if (i->supportingSequences.empty()) { owner_.reset(); sequences_.clear(); hide(); return; }
    bool rebuild = owner_ != i->supportingOwner || sequences_.size() != i->supportingSequences.size();
    if (!rebuild)
        for (std::size_t n = 0; n < sequences_.size(); ++n)
            rebuild |= sequences_[n].sequence != i->supportingSequences[n].sequence;
    auto active = phases_->currentIndex();
    if (rebuild) active = 0;
    if (i->heldSequence)
        for (std::size_t n = 0; n < i->supportingSequences.size(); ++n)
            if (i->supportingSequences[n].sequence == *i->heldSequence) active = static_cast<int>(n);
    const auto previous = phases_->currentIndex();
    owner_ = i->supportingOwner;
    script_ = i->script;
    sequences_ = i->supportingSequences;
    {
        QSignalBlocker blocker(phases_);
        phases_->clear();
        for (const auto &phase : sequences_) phases_->addItem(QString::fromStdString(phase.label));
        phases_->setCurrentIndex(active);
    }
    show();
    if (rebuild || active != previous) showPhase(active, false);
    if (i->heldSequence) outline_->reveal(i->subject);
}
} // namespace salsa::qt
