#include "Sct/SctDocumentController.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <ranges>
#include <utility>

namespace salsa::qt {
namespace {

[[nodiscard]] QString identity(const core::AssetLocator& locator) {
    return QString::fromStdString(locator.identityKey());
}

[[nodiscard]] QString firstError(const core::SctLoadResult& result) {
    const auto found = std::ranges::find_if(result.infrastructureDiagnostics, [](const auto& diagnostic) {
        return diagnostic.severity == core::DiagnosticSeverity::Error;
    });
    return found == result.infrastructureDiagnostics.end()
        ? SctDocumentController::tr("The SCT document could not be opened.")
        : QString::fromStdString(found->message);
}

}  // namespace

SctDocumentController::SctDocumentController(QObject* parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcherBase::finished, this, &SctDocumentController::onFinished);
}

SctDocumentController::~SctDocumentController() {
    if (watcher_.isRunning()) {
        stopSource_.request_stop();
        watcher_.waitForFinished();
    }
}

bool SctDocumentController::openDocument(
    core::LocalGameProject project, const core::AssetLocator& locator) {
    if (busy()) return false;
    if (contains(locator)) {
        emit focusRequested(identity(locator));
        return true;
    }
    begin(Operation::Opening, locator);
    const auto token = stopSource_.get_token();
    watcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), locator, token]() {
            return core::SctDocumentLoader::load(project, locator, token);
        }));
    return true;
}

bool SctDocumentController::reloadDocument(
    core::LocalGameProject project, const core::AssetLocator& locator) {
    if (busy() || !contains(locator)) return false;
    begin(Operation::Reloading, locator);
    const auto token = stopSource_.get_token();
    watcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), locator, token]() {
            return core::SctDocumentLoader::load(project, locator, token);
        }));
    return true;
}

bool SctDocumentController::selectTextConvention(
    const core::AssetLocator& locator,
    const spice::sct::SctKnownTextConvention convention) {
    if (busy()) return false;
    const auto found = documents_.find(locator.identityKey());
    if (found == documents_.end() || !found->second.snapshot
        || !found->second.snapshot->inspection) return false;
    auto inspection = found->second.snapshot->inspection;
    begin(Operation::Reimporting, locator);
    const auto token = stopSource_.get_token();
    watcher_.setFuture(QtConcurrent::run([inspection = std::move(inspection), convention, token]() {
        return core::SctDocumentLoader::materialize(
            inspection, convention, core::SctTextSelectionOrigin::UserSelected, token);
    }));
    return true;
}

void SctDocumentController::synchronizeCatalog(const core::AssetCatalogSnapshot& catalog) {
    for (auto& [key, state] : documents_) {
        const auto found = std::ranges::find_if(catalog.assets, [&state](const auto& asset) {
            return asset.locator == state.locator;
        });
        const auto next = found == catalog.assets.end() ? SourceStatus::Missing
            : found->revision == state.snapshot->source.descriptor.revision
                ? SourceStatus::Current : SourceStatus::Changed;
        if (next != state.status) {
            state.status = next;
            emit documentChanged(QString::fromStdString(key));
        }
    }
}

void SctDocumentController::closeDocument(const core::AssetLocator& locator) {
    if (runningLocator_.has_value() && *runningLocator_ == locator) {
        stopSource_.request_stop();
        ++generation_;
    }
    if (documents_.erase(locator.identityKey()) != 0)
        emit documentClosed(identity(locator));
}

void SctDocumentController::closeAll() {
    if (busy()) {
        stopSource_.request_stop();
        ++generation_;
    }
    std::vector<QString> identities;
    identities.reserve(documents_.size());
    for (const auto& [key, state] : documents_) identities.push_back(QString::fromStdString(key));
    documents_.clear();
    for (const auto& key : identities) emit documentClosed(key);
}

void SctDocumentController::cancel() {
    if (busy()) stopSource_.request_stop();
}

bool SctDocumentController::busy() const noexcept { return operation_ != Operation::None; }

bool SctDocumentController::contains(const core::AssetLocator& locator) const {
    return documents_.contains(locator.identityKey());
}

std::shared_ptr<const core::SctDocumentSnapshot> SctDocumentController::snapshot(
    const core::AssetLocator& locator) const {
    const auto found = documents_.find(locator.identityKey());
    return found == documents_.end() ? nullptr : found->second.snapshot;
}

SctDocumentController::SourceStatus SctDocumentController::sourceStatus(
    const core::AssetLocator& locator) const {
    const auto found = documents_.find(locator.identityKey());
    return found == documents_.end() ? SourceStatus::Missing : found->second.status;
}

std::vector<core::AssetLocator> SctDocumentController::openLocators() const {
    std::vector<core::AssetLocator> result;
    result.reserve(documents_.size());
    for (const auto& [key, state] : documents_) result.push_back(state.locator);
    std::ranges::sort(result);
    return result;
}

const std::vector<core::Diagnostic>& SctDocumentController::failureDiagnostics() const noexcept {
    return failureDiagnostics_;
}

const std::vector<core::SctPipelineDiagnostic>&
SctDocumentController::failurePipelineDiagnostics() const noexcept {
    return failurePipelineDiagnostics_;
}

void SctDocumentController::begin(const Operation operation, const core::AssetLocator& locator) {
    failureDiagnostics_.clear();
    failurePipelineDiagnostics_.clear();
    operation_ = operation;
    runningLocator_ = locator;
    stopSource_ = std::stop_source{};
    runningGeneration_ = ++generation_;
    emit busyChanged();
}

void SctDocumentController::onFinished() {
    auto result = watcher_.result();
    const auto operation = operation_;
    const auto locator = runningLocator_;
    const auto generation = runningGeneration_;
    operation_ = Operation::None;
    runningLocator_.reset();
    emit busyChanged();
    if (!locator.has_value() || generation != generation_) return;

    const auto key = locator->identityKey();
    if (result.cancelled) {
        emit operationCompleted(QString::fromStdString(key), false, true, tr("SCT operation cancelled."));
        return;
    }
    if (!result.succeeded()) {
        failureDiagnostics_ = result.infrastructureDiagnostics;
        if (result.inspection) failurePipelineDiagnostics_ = result.inspection->diagnostics;
        emit operationCompleted(QString::fromStdString(key), false, false, firstError(result));
        return;
    }
    auto found = documents_.find(key);
    if (found == documents_.end()) {
        documents_.emplace(key, DocumentState{ *locator, std::move(result.document), SourceStatus::Current });
    } else {
        found->second.snapshot = std::move(result.document);
        found->second.status = SourceStatus::Current;
    }
    emit documentChanged(QString::fromStdString(key));
    emit focusRequested(QString::fromStdString(key));
    const auto message = operation == Operation::Reimporting
        ? tr("Text convention applied without rereading the source asset.")
        : tr("SCT document loaded.");
    emit operationCompleted(QString::fromStdString(key), true, false, message);
}

}  // namespace salsa::qt
