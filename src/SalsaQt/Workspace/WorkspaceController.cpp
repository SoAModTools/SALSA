#include "Workspace/WorkspaceController.h"

#include "SalsaCore/Project/AssetCatalogDiff.h"

#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <ranges>
#include <utility>

namespace salsa::qt {
namespace {

[[nodiscard]] bool wasCancelled(const std::vector<core::Diagnostic>& diagnostics) {
    return std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == core::DiagnosticCode::Cancelled;
    });
}

[[nodiscard]] QString firstErrorMessage(const std::vector<core::Diagnostic>& diagnostics) {
    const auto error = std::ranges::find_if(diagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == core::DiagnosticSeverity::Error;
    });
    return error == diagnostics.end()
        ? WorkspaceController::tr("The operation failed.")
        : QString::fromStdString(error->message);
}

[[nodiscard]] int progressValue(const std::size_t value) {
    return value > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(value);
}

}  // namespace

WorkspaceController::WorkspaceController(QObject* parent)
    : QObject(parent) {
    connect(
        &watcher_,
        &QFutureWatcherBase::finished,
        this,
        &WorkspaceController::onOperationFinished);
}

WorkspaceController::~WorkspaceController() {
    if (watcher_.isRunning()) {
        stopSource_.request_stop();
        watcher_.waitForFinished();
    }
}

bool WorkspaceController::openDataset(const QString& rootPath) {
    if (rootPath.isEmpty() || busy()) {
        return false;
    }

    beginOperation(Operation::Opening);
    const auto path = std::filesystem::path(rootPath.toStdWString());
    const auto token = stopSource_.get_token();
    const auto generation = runningGeneration_;
    QPointer<WorkspaceController> guard(this);
    core::DatasetScanObserver observer = [guard, generation](const auto& progress) {
        if (guard.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, generation, progress]() {
                if (!guard.isNull()) {
                    guard->handleProgress(generation, progress);
                }
            },
            Qt::QueuedConnection);
    };
    watcher_.setFuture(QtConcurrent::run([path, token, observer = std::move(observer)]() {
        return core::LocalGameProject::inspect(
            core::LocalGameProjectOptions{ path }, token, observer);
    }));
    return true;
}

bool WorkspaceController::refresh() {
    if (!project_.has_value() || busy()) {
        return false;
    }

    const auto project = *project_;
    beginOperation(Operation::Refreshing);
    const auto token = stopSource_.get_token();
    const auto generation = runningGeneration_;
    QPointer<WorkspaceController> guard(this);
    core::DatasetScanObserver observer = [guard, generation](const auto& progress) {
        if (guard.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, generation, progress]() {
                if (!guard.isNull()) {
                    guard->handleProgress(generation, progress);
                }
            },
            Qt::QueuedConnection);
    };
    watcher_.setFuture(QtConcurrent::run([project, token, observer = std::move(observer)]() {
        return project.rescan(token, observer);
    }));
    return true;
}

void WorkspaceController::closeWorkspace() {
    if (busy()) {
        stopSource_.request_stop();
        ++generation_;
    }
    clearWorkspaceState();
    emit workspaceChanged();
    emit selectionChanged();
    emit diagnosticsChanged();
    emit operationStateChanged();
}

void WorkspaceController::cancel() {
    if (busy()) {
        stopSource_.request_stop();
    }
}

void WorkspaceController::selectAsset(std::optional<core::AssetLocator> locator) {
    if (locator.has_value() && project_.has_value()) {
        const auto& assets = project_->snapshot().assets;
        const auto found = std::ranges::lower_bound(assets, *locator, {}, &core::AssetDescriptor::locator);
        if (found == assets.end() || found->locator != *locator) {
            locator.reset();
        }
    } else if (!project_.has_value()) {
        locator.reset();
    }

    if (selectedLocator_ == locator) {
        return;
    }
    selectedLocator_ = std::move(locator);
    emit selectionChanged();
}

bool WorkspaceController::hasWorkspace() const noexcept {
    return project_.has_value();
}

bool WorkspaceController::busy() const noexcept {
    return operation_ != Operation::None;
}

WorkspaceController::Operation WorkspaceController::operation() const noexcept {
    return operation_;
}

const core::DatasetContext* WorkspaceController::dataset() const noexcept {
    return project_.has_value() ? &project_->dataset() : nullptr;
}

const core::AssetCatalogSnapshot* WorkspaceController::catalog() const noexcept {
    return project_.has_value() ? &project_->snapshot() : nullptr;
}

std::optional<core::AssetDescriptor> WorkspaceController::selectedAsset() const {
    if (!project_.has_value() || !selectedLocator_.has_value()) {
        return std::nullopt;
    }
    const auto& assets = project_->snapshot().assets;
    const auto found = std::ranges::lower_bound(
        assets, *selectedLocator_, {}, &core::AssetDescriptor::locator);
    return found != assets.end() && found->locator == *selectedLocator_
        ? std::optional<core::AssetDescriptor>(*found)
        : std::nullopt;
}

const std::optional<core::AssetLocator>& WorkspaceController::selectedLocator() const noexcept {
    return selectedLocator_;
}

const std::vector<core::Diagnostic>& WorkspaceController::diagnostics() const noexcept {
    return diagnostics_;
}

std::optional<core::LocalGameProject> WorkspaceController::projectSnapshot() const {
    return project_;
}

void WorkspaceController::beginOperation(const Operation operation) {
    operation_ = operation;
    stopSource_ = std::stop_source{};
    runningGeneration_ = ++generation_;
    emit operationStateChanged();
}

void WorkspaceController::handleProgress(
    const std::uint64_t generation,
    const core::DatasetScanProgress& progress) {
    if (generation != generation_ || !busy()) {
        return;
    }
    emit progressChanged(
        progress.total.has_value(),
        progressValue(progress.completed),
        progressValue(progress.total.value_or(0)),
        QString::fromStdWString(progress.currentPath.wstring()));
}

void WorkspaceController::onOperationFinished() {
    const auto completedOperation = operation_;
    const auto generation = runningGeneration_;
    auto result = watcher_.result();
    operation_ = Operation::None;
    emit operationStateChanged();

    if (generation != generation_) {
        emit operationCompleted(
            completedOperation, false, true, tr("Workspace closed."));
        return;
    }

    const bool cancelled = wasCancelled(result.diagnostics());
    if (cancelled) {
        emit operationCompleted(
            completedOperation, false, true, tr("Operation cancelled."));
        return;
    }

    if (!result) {
        diagnostics_ = result.diagnostics();
        emit diagnosticsChanged();
        emit operationCompleted(
            completedOperation, false, false, firstErrorMessage(diagnostics_));
        return;
    }

    auto nextProject = std::move(result).takeValue();
    QString message{};
    if (completedOperation == Operation::Refreshing && project_.has_value()) {
        const auto delta = core::diffAssetCatalogs(project_->snapshot(), nextProject.snapshot());
        message = tr("Refresh complete: %1 added, %2 removed, %3 changed.")
            .arg(delta.added.size())
            .arg(delta.removed.size())
            .arg(delta.changed.size());
    } else {
        message = tr("Opened %1 SCT assets.").arg(nextProject.snapshot().assets.size());
    }

    diagnostics_ = result.diagnostics();
    project_ = std::move(nextProject);
    if (selectedLocator_.has_value()) {
        const auto& assets = project_->snapshot().assets;
        const auto found = std::ranges::lower_bound(
            assets, *selectedLocator_, {}, &core::AssetDescriptor::locator);
        if (found == assets.end() || found->locator != *selectedLocator_) {
            selectedLocator_.reset();
        }
    }
    if (completedOperation == Operation::Opening) {
        selectedLocator_.reset();
    }

    emit workspaceChanged();
    emit selectionChanged();
    emit diagnosticsChanged();
    if (completedOperation == Operation::Opening) {
        emit datasetOpened(QString::fromStdWString(project_->dataset().root.wstring()));
    }
    emit operationCompleted(completedOperation, true, false, message);
}

void WorkspaceController::clearWorkspaceState() {
    project_.reset();
    selectedLocator_.reset();
    diagnostics_.clear();
}

}  // namespace salsa::qt
