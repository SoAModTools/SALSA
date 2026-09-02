#include "Sct/SctDocumentController.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QDebug>

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
    for (auto& [key, state] : documents_) {
        if (state.materializationWatcher) {
            state.materializationStop.request_stop();
            state.materializationWatcher->waitForFinished();
        }
    }
    for (auto& watcher : retiredMaterializations_)
        if (watcher) watcher->waitForFinished();
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
        || !found->second.session->currentSnapshot()->provenance->inspection) return false;
    auto inspection = found->second.session->currentSnapshot()->provenance->inspection;
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
            : found->revision == state.session->currentSnapshot()->provenance->source().descriptor.revision
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
    if (auto found = documents_.find(locator.identityKey()); found != documents_.end())
        retireMaterialization(found->second);
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
    for (auto& [key, state] : documents_) {
        identities.push_back(QString::fromStdString(key));
        retireMaterialization(state);
    }
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

std::optional<spice::sct::SctMessage> SctDocumentController::workingMessage(
    const core::AssetLocator& locator,
    const core::SctMessageTarget& target) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto* message = state->session->workingState().message(target);
    return message == nullptr ? std::nullopt : std::optional{*message};
}

std::shared_ptr<const core::SctDocumentSnapshot> SctDocumentController::snapshot(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state == nullptr ? nullptr : state->session->currentSnapshot();
}

std::shared_ptr<const core::SctSemanticEditorProjection>
SctDocumentController::semanticProjection(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state == nullptr ? nullptr : state->session->semanticProjection();
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
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->insertInstructionAfter(anchorInstruction, opcode),
        tr("Instruction inserted."));
}

bool SctDocumentController::deleteInstruction(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->deleteInstruction(instruction),
        tr("Instruction deleted."));
}

bool SctDocumentController::moveInstruction(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction,
    const core::SctInstructionMoveDirection direction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
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
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->replaceMessage(target, draft, kind);
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Message edited."));
}

bool SctDocumentController::addVirtualElse(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId controller) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->addVirtualElse(controller), tr("Empty Else added."));
}

bool SctDocumentController::addVirtualCase(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId controller) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->addVirtualCase(controller), tr("Empty Switch case added."));
}

bool SctDocumentController::setVirtualCaseValue(
    const core::AssetLocator& locator, const core::SctAuthoredArmId arm,
    const std::optional<std::int32_t> value) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->setVirtualCaseValue(arm, value), tr("Switch case value changed."));
}

bool SctDocumentController::removeVirtualArm(
    const core::AssetLocator& locator, const core::SctAuthoredArmId arm) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->removeVirtualArm(arm), tr("Empty semantic arm removed."));
}

bool SctDocumentController::insertInstructionIntoAuthoredArm(
    const core::AssetLocator& locator, const core::SctAuthoredArmId arm,
    const std::uint16_t opcode) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->insertInstructionIntoAuthoredArm(arm, opcode),
        tr("Semantic arm realized."));
}

bool SctDocumentController::insertInstructionIntoStructuredArm(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId controller,
    const spice_sct_prototype::SctStructuredArmKind arm,
    const std::uint16_t opcode) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->insertInstructionIntoStructuredArm(controller, arm, opcode),
        tr("Instruction inserted into semantic arm."));
}

bool SctDocumentController::deleteOnlyInstructionFromAuthoredArm(
    const core::AssetLocator& locator, const core::SctAuthoredArmId arm,
    const spice::sct::SctInstructionId instruction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->deleteOnlyInstructionFromAuthoredArm(arm, instruction),
        tr("Semantic arm returned to empty."));
}

bool SctDocumentController::undo(const core::AssetLocator& locator) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->undo();
    return result.has_value() && applyEditResult(*state, std::move(*result), tr("Undo complete."));
}

bool SctDocumentController::redo(const core::AssetLocator& locator) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
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
    QElapsedTimer notificationTimer;
    notificationTimer.start();
    emit documentChanged(key, SctDocumentUpdate{
        SctDocumentUpdateKind::RevisionTransition,
        result.snapshot,
        result.transition,
        state.session->semanticProjection()});
    if (editTimingsEnabled_) {
        qInfo().noquote() << QStringLiteral(
            "SALSA edit timing %1: preflight=%2us journal=%3us model-notification=%4us")
            .arg(key).arg(result.preflightMicroseconds)
            .arg(result.journalMicroseconds)
            .arg(notificationTimer.nsecsElapsed() / 1000);
    }
    if (result.suggestedSelection.has_value()) {
        emit selectionRequested(key, static_cast<int>(result.suggestedSelection->kind),
            static_cast<qulonglong>(result.suggestedSelection->id));
    }
    emit editCompleted(key, true, std::move(successMessage));
    if (result.changes.documentChanged) requestMaterialization(state);
    return true;
}

void SctDocumentController::requestMaterialization(DocumentState& state) {
    state.requestedMaterializationGeneration = ++nextMaterializationGeneration_;
    state.requestedMaterializationRevision = state.session->workingRevision();
    if (!state.materializationWatcher)
        startMaterialization(state.locator.identityKey(), state);
}

void SctDocumentController::startMaterialization(
    const std::string& identityKey, DocumentState& state) {
    const auto request = state.session->materializationRequest(
        state.requestedMaterializationGeneration);
    if (!request.has_value()) return;
    state.materializationStop = std::stop_source{};
    const auto token = state.materializationStop.get_token();
    const auto generation = request->generation;
    state.runningMaterializationGeneration = generation;
    state.materializationWatcher =
        std::make_unique<QFutureWatcher<core::SctMaterializationResult>>();
    connect(state.materializationWatcher.get(), &QFutureWatcherBase::finished,
        this, [this, identityKey, generation] {
            finishMaterialization(identityKey, generation);
        });
    state.materializationWatcher->setFuture(QtConcurrent::run(
        [request = *request, token] {
            return core::SctDocumentMaterializer::materialize(request, token);
        }));
}

void SctDocumentController::finishMaterialization(
    const std::string& identityKey, const std::uint64_t generation) {
    const auto found = documents_.find(identityKey);
    if (found == documents_.end()) return;
    auto& state = found->second;
    if (!state.materializationWatcher
        || state.runningMaterializationGeneration != generation) return;
    auto* completedWatcher = state.materializationWatcher.release();
    auto result = completedWatcher->result();
    completedWatcher->deleteLater();
    if (editTimingsEnabled_) {
        qInfo().noquote() << QStringLiteral(
            "SALSA materialization timing %1 generation %2: replay=%3us validation=%4us analysis=%5us")
            .arg(QString::fromStdString(identityKey)).arg(generation)
            .arg(result.timings.replayMicroseconds)
            .arg(result.timings.validationMicroseconds)
            .arg(result.timings.analysisMicroseconds);
    }
    if (structureTimingsEnabled_) {
        std::size_t blocks = 0;
        std::size_t regions = 0;
        std::size_t issues = 0;
        if (result.structuredControlFlow) {
            for (const auto& section : result.structuredControlFlow->sections()) {
                blocks += section.blocks.size();
                regions += section.regions.size();
                issues += section.issues.size();
            }
        }
        qInfo().noquote() << QStringLiteral(
            "SALSA structure analysis %1 generation %2: time=%3us sections=%4 blocks=%5 regions=%6 issues=%7")
            .arg(QString::fromStdString(identityKey)).arg(generation)
            .arg(result.timings.structureAnalysisMicroseconds)
            .arg(result.structuredControlFlow
                ? result.structuredControlFlow->sections().size() : 0u)
            .arg(blocks).arg(regions).arg(issues);
    }

    const bool currentTarget = result.targetRevision == state.session->workingRevision();
    if (!result.cancelled && result.succeeded()) {
        if (state.session->installVerifiedMaterialization(result) && currentTarget) {
            emit documentChanged(QString::fromStdString(identityKey), SctDocumentUpdate{
                SctDocumentUpdateKind::VerifiedMaterialization,
                state.session->verifiedSnapshot(), std::nullopt,
                state.session->semanticProjection()});
        }
    } else if (!result.cancelled && currentTarget) {
        state.editBlocked = true;
        failurePipelineDiagnostics_ = result.diagnostics;
        if (failurePipelineDiagnostics_.empty()) {
            core::SctPipelineDiagnostic diagnostic;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.stage = core::SctPipelineStage::Validation;
            diagnostic.code = "BackgroundVerificationFailed";
            diagnostic.message = result.operationIssues.empty()
                ? tr("Background SCT verification rejected the edit.").toStdString()
                : result.operationIssues.front().message;
            diagnostic.locator = state.locator;
            failurePipelineDiagnostics_.push_back(std::move(diagnostic));
        }
        auto rollback = state.session->rejectToVerifiedRevision(
            result.baseRevision, failurePipelineDiagnostics_);
        if (rollback.has_value() && rollback->transition.has_value()) {
            emit documentChanged(QString::fromStdString(identityKey), SctDocumentUpdate{
                SctDocumentUpdateKind::RevisionTransition,
                rollback->snapshot, rollback->transition,
                state.session->semanticProjection()});
            state.requestedMaterializationRevision = rollback->revision;
            state.editBlocked = false;
            emit editCompleted(QString::fromStdString(identityKey), false,
                tr("Background verification rejected the edit and restored the last verified revision."));
        } else {
            emit editCompleted(QString::fromStdString(identityKey), false,
                tr("Background verification rejected the current revision; editing is paused."));
        }
    }

    if (state.requestedMaterializationGeneration != generation
        || state.requestedMaterializationRevision != result.targetRevision) {
        startMaterialization(identityKey, state);
    }
}

void SctDocumentController::retireMaterialization(DocumentState& state) {
    if (!state.materializationWatcher) return;
    state.materializationStop.request_stop();
    state.materializationWatcher->disconnect(this);
    retiredMaterializations_.push_back(std::move(state.materializationWatcher));
}

void SctDocumentController::setEditTimingsEnabled(const bool enabled) noexcept {
    editTimingsEnabled_ = enabled;
}

void SctDocumentController::setStructureTimingsEnabled(const bool enabled) noexcept {
    structureTimingsEnabled_ = enabled;
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
            documents_.at(key).session->currentSnapshot(), std::nullopt,
            documents_.at(key).session->semanticProjection()});
    emit focusRequested(QString::fromStdString(key));
    const auto message = operation == Operation::Reimporting
        ? tr("Text convention applied without rereading the source asset.")
        : tr("SCT document loaded.");
    emit operationCompleted(QString::fromStdString(key), true, false, message);
}

}  // namespace salsa::qt
