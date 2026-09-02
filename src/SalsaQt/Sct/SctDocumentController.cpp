#include "Sct/SctDocumentController.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cassert>
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
    if (found == documents_.end() || !found->second.session
        || !found->second.session->currentSnapshot()->inspection) return false;
    auto inspection = found->second.session->currentSnapshot()->inspection;
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
            : found->revision == state.session->currentSnapshot()->source.descriptor.revision
                ? SourceStatus::Current : SourceStatus::Changed;
        if (next != state.status) {
            state.status = next;
            emit documentChanged(QString::fromStdString(key),
                SctDocumentUpdate{SctDocumentUpdateKind::SourceStatus,
                    state.session->currentSnapshot(), std::nullopt});
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
    const auto* state = findState(locator);
    return state == nullptr ? nullptr : state->session->currentSnapshot();
}

SctDocumentController::SourceStatus SctDocumentController::sourceStatus(
    const core::AssetLocator& locator) const {
    const auto found = documents_.find(locator.identityKey());
    return found == documents_.end() ? SourceStatus::Missing : found->second.status;
}

bool SctDocumentController::structurallyValid(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->session->structurallyValid();
}

bool SctDocumentController::isDirty(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->session->isDirty();
}

bool SctDocumentController::canUndo(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->session->canUndo();
}

bool SctDocumentController::canRedo(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->session->canRedo();
}

std::optional<std::string> SctDocumentController::undoDescription(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto description = state->session->undoDescription();
    return description.has_value() ? std::optional<std::string>(*description) : std::nullopt;
}

std::optional<std::string> SctDocumentController::redoDescription(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto description = state->session->redoDescription();
    return description.has_value() ? std::optional<std::string>(*description) : std::nullopt;
}

std::vector<core::AssetLocator> SctDocumentController::dirtyLocators() const {
    std::vector<core::AssetLocator> result;
    for (const auto& [key, state] : documents_) {
        if (state.session->isDirty()) result.push_back(state.locator);
    }
    std::ranges::sort(result);
    return result;
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

bool SctDocumentController::insertInstructionAfter(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId anchorInstruction,
    const std::uint16_t opcode) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && applyEditResult(*state,
        state->session->insertInstructionAfter(anchorInstruction, opcode),
        tr("Instruction inserted."));
}

bool SctDocumentController::deleteInstruction(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && applyEditResult(*state,
        state->session->deleteInstruction(instruction),
        tr("Instruction deleted."));
}

bool SctDocumentController::moveInstruction(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction,
    const core::SctInstructionMoveDirection direction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && applyEditResult(*state,
        state->session->moveInstruction(instruction, direction),
        direction == core::SctInstructionMoveDirection::Up
            ? tr("Instruction moved up.") : tr("Instruction moved down."));
}

bool SctDocumentController::replaceMessage(
    const core::AssetLocator& locator,
    const core::SctMessageTarget& target,
    const core::SctMessageDraft& draft,
    const core::SctMessageEditKind kind) {
    auto* state = findState(locator);
    if (state == nullptr || busy()) return false;
    auto result = state->session->replaceMessage(target, draft, kind);
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Message edited."));
}

bool SctDocumentController::undo(const core::AssetLocator& locator) {
    auto* state = findState(locator);
    if (state == nullptr || busy()) return false;
    auto result = state->session->undo();
    return result.has_value() && applyEditResult(*state, std::move(*result), tr("Undo complete."));
}

bool SctDocumentController::redo(const core::AssetLocator& locator) {
    auto* state = findState(locator);
    if (state == nullptr || busy()) return false;
    auto result = state->session->redo();
    return result.has_value() && applyEditResult(*state, std::move(*result), tr("Redo complete."));
}

SctDocumentController::DocumentState* SctDocumentController::findState(
    const core::AssetLocator& locator) {
    const auto found = documents_.find(locator.identityKey());
    return found == documents_.end() ? nullptr : &found->second;
}

const SctDocumentController::DocumentState* SctDocumentController::findState(
    const core::AssetLocator& locator) const {
    const auto found = documents_.find(locator.identityKey());
    return found == documents_.end() ? nullptr : &found->second;
}

bool SctDocumentController::applyEditResult(
    DocumentState& state,
    core::SctEditResult result,
    QString successMessage) {
    failureDiagnostics_.clear();
    failurePipelineDiagnostics_ = result.diagnostics;
    const auto key = identity(state.locator);
    if (!result.committed) {
        const auto message = result.diagnostics.empty()
            ? tr("The edit could not be applied.")
            : QString::fromStdString(result.diagnostics.front().message);
        emit editCompleted(key, false, message);
        return false;
    }
    failurePipelineDiagnostics_.clear();
    assert(result.transition.has_value());
    emit documentChanged(key, SctDocumentUpdate{
        SctDocumentUpdateKind::RevisionTransition,
        result.snapshot,
        result.transition});
    if (result.suggestedSelection.has_value()) {
        emit selectionRequested(key, static_cast<int>(result.suggestedSelection->kind),
            static_cast<qulonglong>(result.suggestedSelection->id));
    }
    emit editCompleted(key, true, std::move(successMessage));
    return true;
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
        documents_.emplace(key, DocumentState{ *locator,
            std::make_unique<core::SctEditSession>(std::move(result.document)), SourceStatus::Current });
    } else {
        found->second.session = std::make_unique<core::SctEditSession>(std::move(result.document));
        found->second.status = SourceStatus::Current;
    }
    emit documentChanged(QString::fromStdString(key),
        SctDocumentUpdate{SctDocumentUpdateKind::Replacement,
            documents_.at(key).session->currentSnapshot(), std::nullopt});
    emit focusRequested(QString::fromStdString(key));
    const auto message = operation == Operation::Reimporting
        ? tr("Text convention applied without rereading the source asset.")
        : tr("SCT document loaded.");
    emit operationCompleted(QString::fromStdString(key), true, false, message);
}

}  // namespace salsa::qt
