#include "Sct/SctRebaseDialog.h"

#include "Application/MainWindow.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <ranges>

namespace salsa::qt {
namespace {

QString firstDiagnostic(const std::vector<core::Diagnostic>& diagnostics,
    const QString& fallback) {
    return diagnostics.empty() ? fallback
        : QString::fromStdString(diagnostics.front().message);
}

QString statusText(const core::SctMergePreviewStatus status) {
    switch (status) {
    case core::SctMergePreviewStatus::Ready: return QObject::tr("Ready");
    case core::SctMergePreviewStatus::Conflicted: return QObject::tr("Needs resolution");
    case core::SctMergePreviewStatus::Invalid: return QObject::tr("Invalid");
    case core::SctMergePreviewStatus::Stale: return QObject::tr("Stale");
    }
    return QObject::tr("Unavailable");
}

}  // namespace

SctRebaseController::SctRebaseController(core::LocalGameProject project,
    std::shared_ptr<const core::LocalSalsaWorkspace> workspace,
    CommitHandler committed, QObject* parent)
    : ExclusiveOperationController(parent), project_(std::move(project)),
      workspace_(std::move(workspace)), committed_(std::move(committed)) {
    connect(&discoveryWatcher_, &QFutureWatcherBase::finished,
        this, &SctRebaseController::finishDiscovery);
    connect(&preparationWatcher_, &QFutureWatcherBase::finished,
        this, &SctRebaseController::finishPreparation);
    connect(&commitWatcher_, &QFutureWatcherBase::finished,
        this, &SctRebaseController::finishCommit);
}

SctRebaseController::~SctRebaseController() {
    stopSource_.request_stop();
    discoveryWatcher_.waitForFinished();
    preparationWatcher_.waitForFinished();
    commitWatcher_.waitForFinished();
}

QString SctRebaseController::title() const { return tr("Rebase Stale Patches"); }

core::ExclusiveOperationFlowDefinition SctRebaseController::flowDefinition() const {
    using Role = core::ExclusiveOperationPageRole;
    using Progress = core::ExclusiveOperationProgressVisibility;
    using Layout = core::ExclusiveOperationPageLayout;
    return {"processing",
        {{"processing", Role::Processing, Progress::Visible, Layout::Compact},
         {"review", Role::Review, Progress::Hidden, Layout::Standard},
         {"editor", Role::Review, Progress::Hidden, Layout::Expanded},
         {"commit", Role::Commit, Progress::Visible, Layout::Compact},
         {"summary", Role::Summary, Progress::Hidden, Layout::Compact}},
        {{"prepared", "processing", "prepared", "review"},
         {"none", "processing", "none", "summary"},
         {"failed", "processing", "failed", "summary"},
         {"cancel_processing", "processing", "cancelled", "summary"},
         {"refresh", "review", "refresh", "processing"},
         {"edit", "review", "edit", "editor"},
         {"commit", "review", "commit", "commit"},
         {"cancel_review", "review", "cancelled", "summary"},
         {"apply_edit", "editor", "apply", "review"},
         {"back_edit", "editor", "back", "review"},
         {"cancel_editor", "editor", "cancelled", "summary"},
         {"complete", "commit", "complete", "summary"},
         {"commit_failed", "commit", "failed", "summary"},
         {"stale", "commit", "stale", "processing"}}};
}

QWidget* SctRebaseController::createPage(const std::string_view id, QWidget* parent) {
    if (id == "processing") return createProcessingPage(parent);
    if (id == "review") return createReviewPage(parent);
    if (id == "editor") return createEditorPage(parent);
    if (id == "commit") return createCommitPage(parent);
    return createSummaryPage(parent);
}

std::optional<ExclusiveOperationAction> SctRebaseController::actionForEdge(
    const std::string_view edge) const {
    if (edge == "refresh") return ExclusiveOperationAction{
        tr("Regenerate Preview"), ExclusiveOperationActionRole::Secondary};
    if (edge == "commit") return ExclusiveOperationAction{
        tr("Commit Selected"), ExclusiveOperationActionRole::Primary};
    if (edge == "apply_edit") return ExclusiveOperationAction{
        tr("Use Edited Candidate"), ExclusiveOperationActionRole::Primary};
    if (edge == "back_edit") return ExclusiveOperationAction{
        tr("Discard Candidate Edits"), ExclusiveOperationActionRole::Secondary};
    return std::nullopt;
}

bool SctRebaseController::edgeEnabled(const std::string_view edge) const {
    if (edge == "commit") return std::ranges::any_of(assets_, [](const auto& asset) {
        return asset.selected && asset.plan
            && asset.preview.merge.status == core::SctMergePreviewStatus::Ready;
    });
    return true;
}

void SctRebaseController::handleEvent(const std::string_view event) {
    if (event == "refresh") {
        raiseEvent(event);
    } else if (event == "commit") {
        raiseEvent(event);
        commitSelected();
    } else if (event == "apply") {
        if (applySemanticEdit()) raiseEvent(event);
    } else {
        ExclusiveOperationController::handleEvent(event);
    }
}

void SctRebaseController::pageEntered(const std::string_view page) {
    if (page == "processing" && !discoveryWatcher_.isRunning()
        && !preparationWatcher_.isRunning()) startDiscovery();
    if (page == "editor") buildEditor();
}

void SctRebaseController::requestCancel() {
    if (discoveryWatcher_.isRunning() || preparationWatcher_.isRunning()) {
        stopSource_.request_stop();
        setCancellable(false);
        if (processingStatus_) processingStatus_->setText(tr("Cancelling rebase preparation…"));
        return;
    }
    summary_ = tr("Rebase was cancelled. No additional patches were changed.");
    raiseEvent("cancelled");
}

QWidget* SctRebaseController::createProcessingPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    processingStatus_ = new QLabel(tr("Scanning the workspace for stale patches…"), page);
    processingStatus_->setWordWrap(true);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(processingStatus_);
    layout->addStretch();
    return page;
}

QWidget* SctRebaseController::createReviewPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* explanation = new QLabel(tr(
        "Review each stale patch. Only checked, fully resolved assets are committed."), page);
    explanation->setWordWrap(true);
    tree_ = new QTreeWidget(page);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Asset / change"), tr("State"), tr("Resolution")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    reviewStatus_ = new QLabel(page);
    reviewStatus_->setWordWrap(true);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(explanation);
    layout->addWidget(tree_, 1);
    layout->addWidget(reviewStatus_);
    connect(tree_, &QTreeWidget::itemChanged, this,
        [this](QTreeWidgetItem* item, const int column) {
            if (column != 0 || item->parent()) return;
            const auto index = item->data(0, Qt::UserRole).toULongLong();
            if (index >= assets_.size()) return;
            assets_[index].selected = item->checkState(0) == Qt::Checked;
            emit presentationChanged();
        });
    rebuildTree();
    return page;
}

QWidget* SctRebaseController::createEditorPage(QWidget* parent) {
    editorPage_ = new QWidget(parent);
    editorLayout_ = new QVBoxLayout(editorPage_);
    return editorPage_;
}

QWidget* SctRebaseController::createCommitPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    commitStatus_ = new QLabel(
        tr("Committing selected rebases as one recoverable workspace transaction…"), page);
    commitStatus_->setWordWrap(true);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(commitStatus_);
    layout->addStretch();
    return page;
}

QWidget* SctRebaseController::createSummaryPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    summaryStatus_ = new QLabel(summary_, page);
    summaryStatus_->setWordWrap(true);
    if (!activityReported_) {
        activityReported_ = true;
        reportTerminalActivity(QStringLiteral("PatchRebase"), summary_);
    }
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(summaryStatus_);
    layout->addStretch();
    return page;
}

void SctRebaseController::startDiscovery() {
    stopSource_ = std::stop_source{};
    setCancellable(true);
    setFinishing(false);
    assets_.clear();
    if (tree_) tree_->clear();
    reportProgress(tr("Discovering stale patches"), 0, 0,
        ExclusiveOperationProgressUnit::Assets);
    const auto project = project_;
    const auto workspace = workspace_;
    const auto token = stopSource_.get_token();
    discoveryWatcher_.setFuture(QtConcurrent::run([project, workspace, token] {
        return core::SctWorkspaceRebaseService::discover(project, *workspace, token);
    }));
}

void SctRebaseController::finishDiscovery() {
    auto discovery = discoveryWatcher_.result();
    if (stopSource_.stop_requested()) {
        summary_ = tr("Rebase preparation was cancelled. No patches were changed.");
        raiseEvent("cancelled");
        return;
    }
    if (discovery.candidates.empty()) {
        summary_ = discovery.diagnostics.empty() ? tr("No stale patches were found.")
            : firstDiagnostic(discovery.diagnostics, tr("Stale-patch discovery failed."));
        raiseEvent(discovery.diagnostics.empty() ? "none" : "failed");
        return;
    }
    reportProgress(tr("Preparing semantic rebase previews"), 0,
        discovery.candidates.size(), ExclusiveOperationProgressUnit::Assets);
    const auto project = project_;
    const auto workspace = workspace_;
    const auto token = stopSource_.get_token();
    preparationWatcher_.setFuture(QtConcurrent::run(
        [this, project, workspace, token,
            candidates = std::move(discovery.candidates)]() mutable {
            std::vector<PreparedAsset> result;
            result.reserve(candidates.size());
            for (auto& candidate : candidates) {
                PreparedAsset prepared{std::move(candidate)};
                if (!token.stop_requested() && !prepared.candidate.sourceMissing) {
                    auto plan = core::SctWorkspaceRebaseService::build(
                        project, *workspace, prepared.candidate, {}, token);
                    if (plan) {
                        prepared.plan = std::move(plan).takeValue();
                        prepared.preview = core::SctPatchRebaseService::preview(
                            prepared.plan->corePlan, {}, prepared.candidate.contextToken);
                        prepared.selected = prepared.preview.merge.status
                            == core::SctMergePreviewStatus::Ready;
                    } else prepared.diagnostics = plan.diagnostics();
                }
                result.push_back(std::move(prepared));
                const auto completed = result.size();
                QMetaObject::invokeMethod(this, [this, completed, total = candidates.size()] {
                    reportProgress(tr("Preparing semantic rebase previews"), completed,
                        total, ExclusiveOperationProgressUnit::Assets);
                }, Qt::QueuedConnection);
                if (token.stop_requested()) break;
            }
            return result;
        }));
}

void SctRebaseController::finishPreparation() {
    assets_ = preparationWatcher_.result();
    if (stopSource_.stop_requested()) {
        summary_ = tr("Rebase preparation was cancelled. No patches were changed.");
        raiseEvent("cancelled");
        return;
    }
    rebuildTree();
    const auto ready = std::ranges::count_if(assets_, [](const auto& asset) {
        return asset.preview.merge.status == core::SctMergePreviewStatus::Ready;
    });
    if (reviewStatus_) reviewStatus_->setText(tr("Prepared %1 patch(es); %2 are ready.")
        .arg(assets_.size()).arg(ready));
    raiseEvent("prepared");
}

void SctRebaseController::rebuildTree() {
    if (!tree_) return;
    QSignalBlocker blocker(tree_);
    tree_->clear();
    for (std::size_t assetIndex = 0; assetIndex < assets_.size(); ++assetIndex) {
        auto& asset = assets_[assetIndex];
        auto* root = new QTreeWidgetItem(tree_);
        root->setText(0, QString::fromStdWString(asset.candidate.locator.path().wstring()));
        root->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(assetIndex));
        root->setFlags(root->flags() | Qt::ItemIsUserCheckable);
        root->setCheckState(0, asset.selected ? Qt::Checked : Qt::Unchecked);
        if (asset.candidate.sourceMissing) {
            root->setText(1, tr("Source missing"));
            root->setFlags(root->flags() & ~Qt::ItemIsEnabled);
        } else if (!asset.plan) {
            root->setText(1, tr("Cannot prepare"));
            root->setToolTip(0, firstDiagnostic(asset.diagnostics,
                tr("The preview could not be prepared.")));
            root->setFlags(root->flags() & ~Qt::ItemIsEnabled);
        } else {
            root->setText(1, statusText(asset.preview.merge.status));
            if (asset.preview.merge.changePlan)
                for (const auto& script : asset.preview.merge.changePlan->scripts)
                    for (const auto& unit : script.units) {
                        auto* child = new QTreeWidgetItem(root);
                        child->setText(0, QString::fromStdString(unit.summary));
                        child->setText(1, tr("Semantic change"));
                    }
            for (const auto& conflict : asset.plan->corePlan.mergePlan.conflicts) {
                auto* child = new QTreeWidgetItem(root);
                child->setText(0, QString::fromStdString(conflict.summary));
                child->setText(1, tr("Conflict"));
                auto* choices = new QComboBox(tree_);
                choices->addItem(tr("Choose…"), 0);
                choices->addItem(tr("Keep local intent"), 1);
                choices->addItem(tr("Accept new source"), 2);
                choices->addItem(tr("Drop local change"), 3);
                choices->addItem(tr("Edit semantic candidate…"), 4);
                const auto resolution = std::ranges::find(asset.resolutions,
                    conflict.id, &core::SctMergeResolution::conflictId);
                if (resolution != asset.resolutions.end()) choices->setCurrentIndex(
                    resolution->kind == core::SctMergeResolutionKind::KeepLocal ? 1
                    : resolution->kind == core::SctMergeResolutionKind::AcceptIncoming ? 2
                    : resolution->kind == core::SctMergeResolutionKind::DropLocal ? 3 : 4);
                connect(choices, &QComboBox::currentIndexChanged, this,
                    [this, assetIndex, id = conflict.id](const int choice) {
                        if (choice == 0) {
                            std::erase_if(assets_[assetIndex].resolutions,
                                [&id](const auto& value) { return value.conflictId == id; });
                        } else if (choice == 4) {
                            beginSemanticEdit(assetIndex, id);
                            return;
                        } else setResolution(assetIndex, id,
                            choice == 1 ? core::SctMergeResolutionKind::KeepLocal
                            : choice == 2 ? core::SctMergeResolutionKind::AcceptIncoming
                                         : core::SctMergeResolutionKind::DropLocal);
                        if (assets_[assetIndex].plan) {
                            auto& item = assets_[assetIndex];
                            item.preview = core::SctPatchRebaseService::preview(
                                item.plan->corePlan, item.resolutions,
                                item.candidate.contextToken);
                            item.selected = item.preview.merge.status
                                == core::SctMergePreviewStatus::Ready;
                        }
                        rebuildTree();
                        emit presentationChanged();
                    });
                tree_->setItemWidget(child, 2, choices);
            }
        }
        root->setExpanded(true);
    }
    emit presentationChanged();
}

void SctRebaseController::beginSemanticEdit(
    const std::size_t assetIndex, std::string conflictId) {
    editingAsset_ = assetIndex;
    editingConflict_ = std::move(conflictId);
    raiseEvent("edit");
}

void SctRebaseController::buildEditor() {
    if (!editorPage_ || !editingAsset_ || *editingAsset_ >= assets_.size()) return;
    if (editor_) {
        editorLayout_->removeWidget(editor_);
        delete editor_;
        editor_ = nullptr;
    }
    auto& asset = assets_[*editingAsset_];
    if (!asset.plan) return;
    std::vector<core::SctMergeResolution> prefix;
    for (const auto& conflict : asset.plan->corePlan.mergePlan.conflicts) {
        if (conflict.id == editingConflict_) break;
        const auto existing = std::ranges::find(asset.resolutions,
            conflict.id, &core::SctMergeResolution::conflictId);
        if (existing != asset.resolutions.end()) prefix.push_back(*existing);
    }
    const auto before = core::SctPatchRebaseService::preview(
        asset.plan->corePlan, prefix, asset.candidate.contextToken);
    const auto* initial = before.merge.candidate ? &*before.merge.candidate
        : &asset.plan->corePlan.mergePlan.automaticCandidate;
    editor_ = new MainWindow(MainWindow::Mode::IsolatedDocumentEditor, editorPage_);
    editorLayout_->addWidget(editor_, 1);
    if (!editor_->installSemanticCandidate(asset.candidate.locator,
            asset.plan->newBaselineSnapshot, *initial))
        reportDiagnostics(tr("The semantic conflict candidate could not be opened."));
}

bool SctRebaseController::applySemanticEdit() {
    if (!editor_ || !editingAsset_) return false;
    const auto edited = editor_->captureSemanticCandidate(
        assets_[*editingAsset_].candidate.locator);
    if (!edited) {
        reportDiagnostics(tr(
            "Apply or discard detached editor drafts before using the candidate."));
        return false;
    }
    setResolution(*editingAsset_, editingConflict_,
        core::SctMergeResolutionKind::UseEditedCandidate, *edited);
    auto& asset = assets_[*editingAsset_];
    asset.preview = core::SctPatchRebaseService::preview(asset.plan->corePlan,
        asset.resolutions, asset.candidate.contextToken);
    asset.selected = asset.preview.merge.status == core::SctMergePreviewStatus::Ready;
    rebuildTree();
    return true;
}

void SctRebaseController::setResolution(const std::size_t assetIndex,
    const std::string& conflictId, const core::SctMergeResolutionKind kind,
    std::optional<core::SctSemanticState> edited) {
    auto& resolutions = assets_[assetIndex].resolutions;
    std::erase_if(resolutions, [&conflictId](const auto& value) {
        return value.conflictId == conflictId;
    });
    resolutions.push_back({conflictId, kind, std::move(edited)});
}

void SctRebaseController::commitSelected() {
    std::vector<PreparedAsset> selected;
    for (const auto& asset : assets_)
        if (asset.selected && asset.plan
            && asset.preview.merge.status == core::SctMergePreviewStatus::Ready)
            selected.push_back(asset);
    if (selected.empty()) return;
    setCancellable(false);
    setFinishing(true);
    const auto project = project_;
    const auto workspace = workspace_;
    commitWatcher_.setFuture(QtConcurrent::run(
        [project, workspace, assets = std::move(selected)] {
            std::vector<core::SctWorkspaceRebaseCommitSelection> selections;
            selections.reserve(assets.size());
            for (const auto& asset : assets)
                selections.push_back({&*asset.plan, &asset.preview, asset.resolutions});
            return core::SctWorkspaceRebaseService::commitSelected(
                project, *workspace, selections);
        }));
}

void SctRebaseController::finishCommit() {
    auto result = commitWatcher_.result();
    setFinishing(false);
    if (!result.committed) {
        if (result.stale) {
            summary_ = tr("The preview became stale. A fresh preview is being generated.");
            raiseEvent("stale");
            return;
        }
        finishWith(firstDiagnostic(result.diagnostics,
            tr("The selected rebases could not be committed.")), "failed");
        return;
    }
    auto adoption = committed_ ? committed_(result.committedAssets) : QString{};
    finishWith(adoption.isEmpty()
        ? tr("Committed %1 rebased patch(es).").arg(result.committedAssets.size())
        : adoption, "complete");
}

void SctRebaseController::finishWith(QString text, const std::string_view event) {
    summary_ = std::move(text);
    if (summaryStatus_) summaryStatus_->setText(summary_);
    raiseEvent(event);
}

}  // namespace salsa::qt
