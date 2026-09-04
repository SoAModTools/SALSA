#include "Sct/SctRebaseDialog.h"
#include "Application/MainWindow.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <ranges>

namespace salsa::qt {
namespace {

[[nodiscard]] QString firstDiagnostic(
    const std::vector<core::Diagnostic>& diagnostics,
    const QString& fallback) {
    return diagnostics.empty() ? fallback
        : QString::fromStdString(diagnostics.front().message);
}

[[nodiscard]] QString statusText(const core::SctMergePreviewStatus status) {
    switch (status) {
    case core::SctMergePreviewStatus::Ready: return QObject::tr("Ready");
    case core::SctMergePreviewStatus::Conflicted: return QObject::tr("Needs resolution");
    case core::SctMergePreviewStatus::Invalid: return QObject::tr("Invalid");
    case core::SctMergePreviewStatus::Stale: return QObject::tr("Stale");
    }
    return QObject::tr("Unavailable");
}

[[nodiscard]] std::optional<core::SctSemanticState> editCandidate(
    QWidget* parent, const core::AssetLocator& locator,
    std::shared_ptr<const core::SctDocumentSnapshot> provenanceSnapshot,
    const core::SctSemanticState& initial) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Edit Conflict Candidate"));
    dialog.resize(1150, 760);
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(QObject::tr(
        "This is an isolated instance of the normal SCT editor. Only edits within "
        "the selected conflict dependency group will be accepted."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* editor = new MainWindow(
        MainWindow::Mode::IsolatedDocumentEditor, &dialog);
    layout->addWidget(editor, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Use Edited Candidate"));
    layout->addWidget(buttons);
    std::optional<core::SctSemanticState> result;
    if (!editor->installSemanticCandidate(locator,
            std::move(provenanceSnapshot), initial)) return std::nullopt;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
        [&]() {
            result = editor->captureSemanticCandidate(locator);
            if (!result) {
                QMessageBox::warning(&dialog, QObject::tr("Candidate not ready"),
                    QObject::tr("Apply or discard detached editor drafts before using the candidate."));
                return;
            }
            dialog.accept();
        });
    QObject::connect(buttons, &QDialogButtonBox::rejected,
        &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? result : std::nullopt;
}

}  // namespace

SctRebaseDialog::SctRebaseDialog(core::LocalGameProject project,
    std::shared_ptr<const core::LocalSalsaWorkspace> workspace,
    QWidget* parent)
    : QDialog(parent), project_(std::move(project)), workspace_(std::move(workspace)) {
    setWindowTitle(tr("Rebase Stale Patches"));
    resize(900, 620);
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr(
        "Review each stale patch against the current source. Only checked, fully "
        "resolved assets are committed; all other patches remain unchanged."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Asset / change"), tr("State"), tr("Resolution")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(tree_, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(this);
    refreshButton_ = buttons->addButton(tr("Regenerate Preview"),
        QDialogButtonBox::ActionRole);
    commitButton_ = buttons->addButton(tr("Commit Selected"),
        QDialogButtonBox::AcceptRole);
    cancelButton_ = buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(refreshButton_, &QPushButton::clicked, this,
        &SctRebaseDialog::startDiscovery);
    connect(commitButton_, &QPushButton::clicked, this,
        &SctRebaseDialog::commitSelected);
    connect(cancelButton_, &QPushButton::clicked, this, &SctRebaseDialog::reject);
    connect(&discoveryWatcher_, &QFutureWatcherBase::finished,
        this, &SctRebaseDialog::finishDiscovery);
    connect(&preparationWatcher_, &QFutureWatcherBase::finished,
        this, &SctRebaseDialog::finishPreparation);
    connect(&commitWatcher_, &QFutureWatcherBase::finished,
        this, &SctRebaseDialog::finishCommit);
    connect(tree_, &QTreeWidget::itemChanged, this,
        [this](QTreeWidgetItem* item, const int column) {
            if (column != 0 || item->parent() != nullptr) return;
            const auto index = item->data(0, Qt::UserRole).toULongLong();
            if (index >= assets_.size()) return;
            assets_[index].selected = item->checkState(0) == Qt::Checked;
            syncButtons();
        });
    startDiscovery();
}

SctRebaseDialog::~SctRebaseDialog() {
    stopSource_.request_stop();
    discoveryWatcher_.waitForFinished();
    preparationWatcher_.waitForFinished();
    commitWatcher_.waitForFinished();
}

const std::vector<core::AssetLocator>& SctRebaseDialog::committedAssets() const noexcept {
    return committedAssets_;
}

void SctRebaseDialog::reject() {
    stopSource_.request_stop();
    QDialog::reject();
}

void SctRebaseDialog::startDiscovery() {
    if (discoveryWatcher_.isRunning() || preparationWatcher_.isRunning()
        || commitWatcher_.isRunning()) return;
    stopSource_ = std::stop_source{};
    assets_.clear();
    tree_->clear();
    status_->setText(tr("Scanning the workspace for stale patches..."));
    syncButtons();
    const auto project = project_;
    const auto workspace = workspace_;
    const auto token = stopSource_.get_token();
    discoveryWatcher_.setFuture(QtConcurrent::run([project, workspace, token]() {
        return core::SctWorkspaceRebaseService::discover(project, *workspace, token);
    }));
}

void SctRebaseDialog::finishDiscovery() {
    auto discovery = discoveryWatcher_.result();
    if (stopSource_.stop_requested()) return;
    if (!discovery.diagnostics.empty()) {
        status_->setText(firstDiagnostic(discovery.diagnostics,
            tr("Stale-patch discovery reported a problem.")));
    }
    if (discovery.candidates.empty()) {
        status_->setText(tr("No stale patches were found."));
        syncButtons();
        return;
    }
    status_->setText(tr("Building semantic rebase previews for %1 patch(es)...")
        .arg(discovery.candidates.size()));
    const auto project = project_;
    const auto workspace = workspace_;
    const auto token = stopSource_.get_token();
    preparationWatcher_.setFuture(QtConcurrent::run(
        [project, workspace, token,
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
                    } else {
                        prepared.diagnostics = plan.diagnostics();
                    }
                }
                result.push_back(std::move(prepared));
                if (token.stop_requested()) break;
            }
            return result;
        }));
}

void SctRebaseDialog::finishPreparation() {
    assets_ = preparationWatcher_.result();
    if (stopSource_.stop_requested()) return;
    rebuildTree();
    const auto ready = std::ranges::count_if(assets_, [](const auto& asset) {
        return asset.preview.merge.status == core::SctMergePreviewStatus::Ready;
    });
    status_->setText(tr("Prepared %1 patch(es); %2 can be committed now.")
        .arg(assets_.size()).arg(ready));
    syncButtons();
}

void SctRebaseDialog::rebuildTree() {
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
            const auto* changePlan = asset.preview.merge.changePlan
                ? &*asset.preview.merge.changePlan : nullptr;
            if (changePlan) {
                for (const auto& script : changePlan->scripts)
                    for (const auto& unit : script.units) {
                        auto* child = new QTreeWidgetItem(root);
                        child->setText(0, QString::fromStdString(unit.summary));
                        child->setText(1, tr("Semantic change"));
                        if (!unit.details.empty())
                            child->setToolTip(0, QString::fromStdString(unit.details.front()));
                    }
            } else {
                for (const auto& script : asset.plan->corePlan.mergePlan.localChanges.scripts)
                    for (const auto& unit : script.units) {
                        auto* child = new QTreeWidgetItem(root);
                        child->setText(0, tr("Local: %1")
                            .arg(QString::fromStdString(unit.summary)));
                        child->setText(1, tr("Authored intent"));
                    }
                for (const auto& script : asset.plan->corePlan.mergePlan.incomingChanges.scripts)
                    for (const auto& unit : script.units) {
                        auto* child = new QTreeWidgetItem(root);
                        child->setText(0, tr("New source: %1")
                            .arg(QString::fromStdString(unit.summary)));
                        child->setText(1, tr("Source change"));
                    }
            }
            for (const auto& conflict : asset.plan->corePlan.mergePlan.conflicts) {
                auto* child = new QTreeWidgetItem(root);
                child->setText(0, QString::fromStdString(conflict.summary));
                child->setText(1, tr("Conflict"));
                if (!conflict.details.empty())
                    child->setToolTip(0, QString::fromStdString(conflict.details.front()));
                auto* choices = new QComboBox(tree_);
                choices->addItem(tr("Choose..."), 0);
                choices->addItem(tr("Keep local intent"), 1);
                choices->addItem(tr("Accept new source"), 2);
                choices->addItem(tr("Drop local change"), 3);
                choices->addItem(tr("Edit semantic candidate..."), 4);
                const auto resolution = std::ranges::find(asset.resolutions,
                    conflict.id, &core::SctMergeResolution::conflictId);
                if (resolution != asset.resolutions.end()) {
                    choices->setCurrentIndex(resolution->kind
                        == core::SctMergeResolutionKind::KeepLocal ? 1
                        : resolution->kind == core::SctMergeResolutionKind::AcceptIncoming ? 2
                        : resolution->kind == core::SctMergeResolutionKind::DropLocal ? 3
                        : resolution->kind == core::SctMergeResolutionKind::UseEditedCandidate ? 4
                        : 0);
                }
                connect(choices, &QComboBox::currentIndexChanged, this,
                    [this, assetIndex, id = conflict.id](const int choice) {
                        QTimer::singleShot(0, this,
                            [this, assetIndex, id, choice]() {
                                setConflictResolution(assetIndex, id, choice);
                            });
                    });
                tree_->setItemWidget(child, 2, choices);
            }
        }
        root->setExpanded(true);
    }
}

void SctRebaseDialog::setConflictResolution(const std::size_t assetIndex,
    const std::string& conflictId, const int choice) {
    if (assetIndex >= assets_.size() || !assets_[assetIndex].plan) return;
    auto& asset = assets_[assetIndex];
    std::optional<core::SctSemanticState> edited;
    if (choice == 4) {
        std::vector<core::SctMergeResolution> prefix;
        for (const auto& conflict : asset.plan->corePlan.mergePlan.conflicts) {
            if (conflict.id == conflictId) break;
            const auto existing = std::ranges::find(asset.resolutions,
                conflict.id, &core::SctMergeResolution::conflictId);
            if (existing != asset.resolutions.end()) prefix.push_back(*existing);
        }
        const auto before = core::SctPatchRebaseService::preview(
            asset.plan->corePlan, prefix, asset.candidate.contextToken);
        const auto* initial = before.merge.candidate
            ? &*before.merge.candidate
            : &asset.plan->corePlan.mergePlan.automaticCandidate;
        edited = editCandidate(this, asset.candidate.locator,
            asset.plan->newBaselineSnapshot, *initial);
        if (!edited) {
            rebuildTree();
            return;
        }
    }
    std::erase_if(asset.resolutions, [&conflictId](const auto& resolution) {
        return resolution.conflictId == conflictId;
    });
    if (choice != 0) {
        const auto kind = choice == 1 ? core::SctMergeResolutionKind::KeepLocal
            : choice == 2 ? core::SctMergeResolutionKind::AcceptIncoming
            : choice == 3 ? core::SctMergeResolutionKind::DropLocal
            : core::SctMergeResolutionKind::UseEditedCandidate;
        asset.resolutions.push_back({conflictId, kind, std::move(edited)});
    }
    asset.preview = core::SctPatchRebaseService::preview(asset.plan->corePlan,
        asset.resolutions, asset.candidate.contextToken);
    asset.selected = asset.preview.merge.status == core::SctMergePreviewStatus::Ready;
    rebuildTree();
    syncButtons();
}

void SctRebaseDialog::commitSelected() {
    std::vector<PreparedAsset> selectedAssets;
    for (const auto& asset : assets_) {
        if (!asset.selected || !asset.plan
            || asset.preview.merge.status != core::SctMergePreviewStatus::Ready) continue;
        selectedAssets.push_back(asset);
    }
    if (selectedAssets.empty()) return;
    status_->setText(tr("Committing selected rebases as one workspace transaction..."));
    const auto project = project_;
    const auto workspace = workspace_;
    commitWatcher_.setFuture(QtConcurrent::run(
        [project, workspace, assets = std::move(selectedAssets)]() {
            std::vector<core::SctWorkspaceRebaseCommitSelection> selections;
            selections.reserve(assets.size());
            for (const auto& asset : assets)
                selections.push_back({&*asset.plan, &asset.preview, asset.resolutions});
            return core::SctWorkspaceRebaseService::commitSelected(
                project, *workspace, selections);
        }));
    syncButtons();
}

void SctRebaseDialog::finishCommit() {
    auto result = commitWatcher_.result();
    if (!result.committed) {
        status_->setText(firstDiagnostic(result.diagnostics, result.stale
            ? tr("The preview became stale. Regenerate it before committing.")
            : tr("The selected rebases could not be committed.")));
        syncButtons();
        return;
    }
    committedAssets_ = std::move(result.committedAssets);
    accept();
}

void SctRebaseDialog::syncButtons() {
    const bool busy = discoveryWatcher_.isRunning()
        || preparationWatcher_.isRunning() || commitWatcher_.isRunning();
    tree_->setEnabled(!busy);
    refreshButton_->setEnabled(!busy);
    cancelButton_->setEnabled(!commitWatcher_.isRunning());
    commitButton_->setEnabled(!busy && std::ranges::any_of(assets_, [](const auto& asset) {
        return asset.selected
            && asset.preview.merge.status == core::SctMergePreviewStatus::Ready;
    }));
}

}  // namespace salsa::qt
