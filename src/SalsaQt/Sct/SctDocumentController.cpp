#include "Sct/SctDocumentController.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QDebug>

#include <algorithm>
#include <cassert>
#include <ranges>
#include <type_traits>
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
    connect(&publicationWatcher_, &QFutureWatcherBase::finished,
        this, &SctDocumentController::finishPublication);
}

SctDocumentController::~SctDocumentController() {
    if (watcher_.isRunning()) {
        stopSource_.request_stop();
        watcher_.waitForFinished();
    }
    if (publicationWatcher_.isRunning()) {
        publicationStop_.request_stop();
        publicationWatcher_.waitForFinished();
    }
    for (auto& [key, state] : documents_) {
        if (state.materializationWatcher) {
            state.materializationStop.request_stop();
            state.materializationWatcher->waitForFinished();
        }
        if (state.checkpointWatcher) {
            state.checkpointStop.request_stop();
            state.checkpointWatcher->waitForFinished();
        }
    }
    for (auto& watcher : retiredMaterializations_)
        if (watcher) watcher->waitForFinished();
    for (auto& watcher : retiredCheckpoints_)
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
    auto workspace = workspace_;
    watcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), locator, token, workspace = std::move(workspace)]() {
            return core::SctPatchCheckpointService::load(
                project, workspace.get(), workspace.get(), locator, token);
        }));
    return true;
}

bool SctDocumentController::reloadDocument(
    core::LocalGameProject project, const core::AssetLocator& locator) {
    if (busy() || !contains(locator)) return false;
    begin(Operation::Reloading, locator);
    const auto token = stopSource_.get_token();
    auto workspace = workspace_;
    watcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), locator, token, workspace = std::move(workspace)]() {
            return core::SctPatchCheckpointService::load(
                project, workspace.get(), workspace.get(), locator, token);
        }));
    return true;
}

bool SctDocumentController::adoptRebasedDocument(
    const core::LocalGameProject& project, const core::AssetLocator& locator) {
    auto* state = findState(locator);
    if (state == nullptr) return true;
    if (busy() || state->materializationWatcher || state->checkpointWatcher
        || state->session->isDirty() || workspace_ == nullptr) return false;
    auto reopened = core::SctPatchCheckpointService::load(
        project, workspace_.get(), workspace_.get(), locator);
    if (!reopened.load.succeeded() || !reopened.baseline
        || !reopened.patchApplied || reopened.patchConflict) {
        failureDiagnostics_ = reopened.load.infrastructureDiagnostics;
        failurePipelineDiagnostics_ = reopened.load.document
            ? reopened.load.document->diagnostics
            : std::vector<core::SctPipelineDiagnostic>{};
        return false;
    }
    state->session = core::SctEditSession::createRebased(
        reopened.baseline, reopened.load.document,
        reopened.authoredArms, reopened.textRepairs);
    state->status = SourceStatus::Current;
    state->patchConflict = false;
    state->editBlocked = false;
    state->publicationDiagnostics.clear();
    state->lastPublication.reset();
    failureDiagnostics_.clear();
    failurePipelineDiagnostics_.clear();
    emit documentChanged(identity(locator), SctDocumentUpdate{
        SctDocumentUpdateKind::Replacement,
        state->session->currentSnapshot(), std::nullopt,
        state->session->semanticProjection()});
    emit editCompleted(identity(locator), true,
        tr("The rebased patch was adopted as one undoable document change."));
    return true;
}

bool SctDocumentController::installTransientDocument(
    const core::AssetLocator& locator,
    std::shared_ptr<const core::SctDocumentSnapshot> provenanceSnapshot,
    const core::SctSemanticState& semanticState) {
    if (busy() || contains(locator) || !provenanceSnapshot
        || !provenanceSnapshot->provenance || !semanticState.document) return false;
    const auto validation = spice::sct::SctDocumentValidator::validateDocument(
        *semanticState.document);
    if (!validation.validDocument) return false;
    auto snapshot = std::make_shared<core::SctDocumentSnapshot>(
        core::SctDocumentSnapshot{provenanceSnapshot->provenance,
            semanticState.document,
            std::make_shared<const spice::sct::SctDocumentAnalysis>(
                spice::sct::SctDocumentAnalysis::build(*semanticState.document,
                    provenanceSnapshot->provenance->importEvidence
                        ? &*provenanceSnapshot->provenance->importEvidence : nullptr)),
            spice::sct::SctDocumentReadiness::StructurallyValid, {}});
    DocumentState state{locator,
        std::make_unique<core::SctEditSession>(snapshot, snapshot,
            semanticState.authoredArms, semanticState.textRepairs)};
    auto [found, inserted] = documents_.emplace(locator.identityKey(), std::move(state));
    if (!inserted) return false;
    emit documentChanged(identity(locator), SctDocumentUpdate{
        SctDocumentUpdateKind::Replacement,
        found->second.session->currentSnapshot(), std::nullopt,
        found->second.session->semanticProjection()});
    emit focusRequested(identity(locator));
    return true;
}

std::optional<core::SctSemanticState> SctDocumentController::semanticState(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto document = state->session->materializeRevision(
        state->session->workingRevision());
    if (!document) return std::nullopt;
    return core::SctSemanticState{*document,
        {state->session->structuredAuthoring().arms().begin(),
            state->session->structuredAuthoring().arms().end()},
        state->session->workingState().textRepairProvenances()};
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
        core::SctPatchedLoadResult result;
        result.load = core::SctDocumentLoader::materialize(
            inspection, convention, core::SctTextSelectionOrigin::UserSelected, token);
        result.baseline = result.load.document;
        return result;
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
    if (publicationLocator_ && *publicationLocator_ == locator)
        publicationStop_.request_stop();
    if (runningLocator_.has_value() && *runningLocator_ == locator) {
        stopSource_.request_stop();
        ++generation_;
    }
    if (auto found = documents_.find(locator.identityKey()); found != documents_.end()) {
        retireMaterialization(found->second);
        retireCheckpoint(found->second);
    }
    if (documents_.erase(locator.identityKey()) != 0)
        emit documentClosed(identity(locator));
}

void SctDocumentController::closeAll() {
    if (publicationWatcher_.isRunning()) publicationStop_.request_stop();
    if (busy()) {
        stopSource_.request_stop();
        ++generation_;
    }
    std::vector<QString> identities;
    identities.reserve(documents_.size());
    for (auto& [key, state] : documents_) {
        identities.push_back(QString::fromStdString(key));
        retireMaterialization(state);
        retireCheckpoint(state);
    }
    documents_.clear();
    for (const auto& key : identities) emit documentClosed(key);
}

void SctDocumentController::cancel() {
    if (busy()) stopSource_.request_stop();
    if (publicationWatcher_.isRunning()) publicationStop_.request_stop();
    for (auto& [key, state] : documents_)
        if (state.checkpointWatcher) state.checkpointStop.request_stop();
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

std::optional<spice::sct::SctTextValue> SctDocumentController::workingText(
    const core::AssetLocator& locator, const core::SctTextTarget& target) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto* value = state->session->workingState().textValue(target);
    return value == nullptr ? std::nullopt : std::optional{*value};
}

core::SctParameterTablePresentation SctDocumentController::parameterPresentation(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction) const {
    const auto* state = findState(locator);
    return state == nullptr ? core::SctParameterTablePresentation{}
        : core::SctParameterAuthoringService::project(
            state->session->workingState(), instruction);
}

std::vector<core::SctReferenceCandidate>
SctDocumentController::referenceCandidates(
    const core::AssetLocator& locator,
    const spice::sct::SctParameterSite& site) const {
    const auto* state = findState(locator);
    return state == nullptr ? std::vector<core::SctReferenceCandidate>{}
        : core::SctParameterAuthoringService::referenceCandidates(
            state->session->workingState(), site);
}

std::vector<core::SctReferenceCandidate>
SctDocumentController::draftReferenceCandidates(
    const core::AssetLocator& locator, const std::uint16_t opcode,
    const spice::sct::SctParameterAddress& address) const {
    const auto* state = findState(locator);
    return state == nullptr ? std::vector<core::SctReferenceCandidate>{}
        : core::SctParameterAuthoringService::referenceCandidates(
            state->session->workingState(), opcode, address);
}

std::optional<spice::sct::SctCanonicalExpression>
SctDocumentController::workingParameterExpression(
    const core::AssetLocator& locator,
    const spice::sct::SctParameterSite& site) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto* parameter = state->session->workingState().parameter(site);
    if (parameter == nullptr) return std::nullopt;
    const auto* expression = std::get_if<spice::sct::SctCanonicalExpression>(
        &parameter->value);
    return expression == nullptr ? std::nullopt : std::optional{*expression};
}

std::optional<spice::sct::SctDocumentInstruction>
SctDocumentController::workingInstruction(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction) const {
    const auto* state = findState(locator);
    if (state == nullptr) return std::nullopt;
    const auto* value = state->session->workingState().instruction(instruction);
    return value == nullptr ? std::nullopt : std::optional{*value};
}

std::shared_ptr<const core::SctDocumentSnapshot> SctDocumentController::snapshot(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state == nullptr ? nullptr : state->session->currentSnapshot();
}

std::vector<core::SctPipelineDiagnostic> SctDocumentController::currentDiagnostics(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    if (state == nullptr) return {};
    auto result = state->session->currentDiagnostics();
    result.insert(result.end(), state->publicationDiagnostics.begin(),
        state->publicationDiagnostics.end());
    return result;
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
    return state != nullptr && !state->editBlocked && state->session->structurallyValid();
}

bool SctDocumentController::isDirty(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->session->isDirty();
}

core::RevisionId SctDocumentController::workingRevision(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state == nullptr ? core::RevisionId{} : state->session->workingRevision();
}

bool SctDocumentController::isSaving(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->checkpointWatcher != nullptr;
}

bool SctDocumentController::isPublishing() const noexcept {
    return publicationWatcher_.isRunning();
}

std::optional<core::SctPublicationReceipt> SctDocumentController::lastPublication(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state == nullptr ? std::nullopt : state->lastPublication;
}

bool SctDocumentController::patchConflict(const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state != nullptr && state->patchConflict;
}

bool SctDocumentController::hasWorkspace() const noexcept {
    return workspace_ != nullptr;
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

core::SctInstructionAuthoringDraftResult
SctDocumentController::createInstructionDraft(
    const core::AssetLocator& locator, const std::uint16_t opcode) const {
    const auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return {};
    return state->session->createInstructionDraft(opcode);
}

bool SctDocumentController::createInstructionAfter(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId anchorInstruction,
    core::SctInstructionAuthoringDraft draft) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->createInstructionAfter(
            anchorInstruction, std::move(draft)), tr("Instruction inserted."));
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

bool SctDocumentController::replacePlainText(
    const core::AssetLocator& locator, const core::SctTextTarget& target,
    std::string utf8) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->replacePlainText(target, std::move(utf8));
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Plain text edited."));
}

bool SctDocumentController::replaceTextValue(
    const core::AssetLocator& locator, const core::SctTextTarget& target,
    spice::sct::SctTextValue value, std::string description,
    std::optional<core::SctTextRepairProvenance> repairProvenance) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->replaceTextValue(
        target, std::move(value), std::move(description), std::move(repairProvenance));
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Text repaired."));
}

bool SctDocumentController::editParameterText(
    const core::AssetLocator& locator, const spice::sct::SctParameterSite& site,
    std::string text) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->editParameterText(site, std::move(text));
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Parameter edited."));
}

bool SctDocumentController::replaceParameterValue(
    const core::AssetLocator& locator, const spice::sct::SctParameterSite& site,
    spice::sct::SctDocumentParameterValue value) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->replaceParameterValue(site, std::move(value));
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Parameter edited."));
}

bool SctDocumentController::retargetParameter(
    const core::AssetLocator& locator, const spice::sct::SctParameterSite& site,
    const spice::sct::SctDocumentReferenceTarget& target) {
    auto value = std::visit([](const auto id)
            -> spice::sct::SctDocumentParameterValue {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, spice::sct::SctInstructionId>)
            return spice::sct::SctInstructionReference{id};
        else if constexpr (std::is_same_v<T, spice::sct::SctStringId>)
            return spice::sct::SctStringReference{id};
        else return spice::sct::SctFooterEntryReference{id};
    }, target);
    return replaceParameterValue(locator, site, std::move(value));
}

bool SctDocumentController::insertRepeatedGroup(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction, const std::uint32_t ordinal,
    spice::sct::SctDocumentRepeatedParameterGroup group) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->insertRepeatedGroup(
            instruction, ordinal, std::move(group)), tr("Repeated group added."));
}

bool SctDocumentController::deleteRepeatedGroup(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction, const std::uint32_t ordinal) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->deleteRepeatedGroup(
            instruction, ordinal), tr("Repeated group deleted."));
}

bool SctDocumentController::moveRepeatedGroup(
    const core::AssetLocator& locator,
    const spice::sct::SctInstructionId instruction, const std::uint32_t ordinal,
    const core::SctRepeatedGroupMoveDirection direction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->moveRepeatedGroup(
            instruction, ordinal, direction), tr("Repeated group moved."));
}

bool SctDocumentController::createScriptSection(
    const core::AssetLocator& locator, std::string name,
    const std::optional<spice::sct::SctSectionId> after,
    const bool includeReturn) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->createScriptSection(std::move(name), after, includeReturn),
        tr("Script section created."));
}

bool SctDocumentController::createIndexedString(
    const core::AssetLocator& locator, std::string name,
    const std::optional<spice::sct::SctSectionId> after) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->createIndexedString(std::move(name), after),
        tr("Indexed string created."));
}

bool SctDocumentController::renameSection(
    const core::AssetLocator& locator, const spice::sct::SctSectionId section,
    std::string name) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked) return false;
    auto result = state->session->renameSection(section, std::move(name));
    if (!result.committed && result.diagnostics.empty()) return true;
    return applyEditResult(*state, std::move(result), tr("Section renamed."));
}

bool SctDocumentController::deleteSection(
    const core::AssetLocator& locator, const spice::sct::SctSectionId section) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->deleteSection(section), tr("Section deleted."));
}

bool SctDocumentController::moveSection(
    const core::AssetLocator& locator, const spice::sct::SctSectionId section,
    const core::SctSectionMoveDirection direction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->moveSection(section, direction),
        direction == core::SctSectionMoveDirection::Up
            ? tr("Section moved up.") : tr("Section moved down."));
}

bool SctDocumentController::createFooterText(
    const core::AssetLocator& locator, const core::SctCreatedFooterTextKind kind,
    const std::optional<spice::sct::SctFooterEntryId> after) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->createFooterText(kind, after), tr("Footer text created."));
}

bool SctDocumentController::deleteTextEntity(
    const core::AssetLocator& locator, const core::SctTextTarget& target) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->deleteTextEntity(target), tr("Text entity deleted."));
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
    const spice::sct::SctStructuredArmKind arm,
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

bool SctDocumentController::saveDocument(const core::AssetLocator& locator) {
    auto* state = findState(locator);
    if (state == nullptr || workspace_ == nullptr || state->patchConflict
        || state->status != SourceStatus::Current
        || state->checkpointWatcher != nullptr) return false;
    const auto generation = ++nextCheckpointGeneration_;
    auto request = state->session->checkpointRequest(generation);
    if (!request.has_value()) return false;

    state->checkpointStop = std::stop_source{};
    const auto token = state->checkpointStop.get_token();
    state->checkpointGeneration = generation;
    state->checkpointWatcher =
        std::make_unique<QFutureWatcher<core::SctCheckpointResult>>();
    const auto identityKey = locator.identityKey();
    connect(state->checkpointWatcher.get(), &QFutureWatcherBase::finished,
        this, [this, identityKey, generation] {
            finishCheckpoint(identityKey, generation);
        });
    auto workspace = workspace_;
    state->checkpointWatcher->setFuture(QtConcurrent::run(
        [request = std::move(*request), token, workspace = std::move(workspace)] {
            return core::SctPatchCheckpointService::checkpoint(
                request, *workspace, *workspace, token);
        }));
    emit documentChanged(identity(locator), SctDocumentUpdate{
        SctDocumentUpdateKind::SourceStatus,
        state->session->currentSnapshot(), std::nullopt,
        state->session->semanticProjection()});
    return true;
}

bool SctDocumentController::exportDocument(
    core::LocalGameProject project,
    const core::AssetLocator& locator,
    core::SctPublicationOptions options,
    std::filesystem::path destination,
    const bool allowSourceReplacement,
    core::SctPublicationObserver observer) {
    auto* state = findState(locator);
    if (state == nullptr || busy() || publicationWatcher_.isRunning()
        || state->editBlocked) return false;
    const auto snapshot = state->session->currentSnapshot();
    if (!snapshot || !snapshot->provenance || !snapshot->provenance->inspection)
        return false;
    const auto generation = ++nextPublicationGeneration_;
    auto captured = state->session->capturePublicationRevision(generation);
    if (!captured) return false;

    state->publicationDiagnostics.clear();
    failureDiagnostics_.clear();
    failurePipelineDiagnostics_.clear();
    core::SctPublicationRequest request{
        locator,
        snapshot->provenance->inspection->sourceDatasetFingerprint,
        snapshot->provenance->source().descriptor.revision,
        std::move(*captured),
        options,
        std::move(destination),
        allowSourceReplacement,
    };
    publicationStop_ = std::stop_source{};
    const auto token = publicationStop_.get_token();
    publicationLocator_ = locator;
    publicationWatcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), request = std::move(request), token,
            observer = std::move(observer)] {
            return core::SctPublicationService::publish(project, request, token, observer);
        }));
    return true;
}

void SctDocumentController::setWorkspace(
    std::shared_ptr<const core::LocalSalsaWorkspace> workspace) {
    workspace_ = std::move(workspace);
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
    state.publicationDiagnostics.clear();
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
        if (result.analysis) {
            for (const auto& section : result.analysis->structuredControlFlow.sections()) {
                blocks += section.blocks.size();
                regions += section.regions.size();
                issues += section.issues.size();
            }
        }
        qInfo().noquote() << QStringLiteral(
            "SALSA structure analysis %1 generation %2: aggregate-analysis=%3us sections=%4 blocks=%5 regions=%6 issues=%7")
            .arg(QString::fromStdString(identityKey)).arg(generation)
            .arg(result.timings.analysisMicroseconds)
            .arg(result.analysis
                ? result.analysis->structuredControlFlow.sections().size() : 0u)
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

void SctDocumentController::finishCheckpoint(
    const std::string& identityKey, const std::uint64_t generation) {
    const auto found = documents_.find(identityKey);
    if (found == documents_.end()) return;
    auto& state = found->second;
    if (!state.checkpointWatcher || state.checkpointGeneration != generation) return;
    auto* completedWatcher = state.checkpointWatcher.release();
    auto result = completedWatcher->result();
    completedWatcher->deleteLater();

    failureDiagnostics_ = result.diagnostics;
    if (result.saved) {
        (void)state.session->markPatchCheckpoint(
            result.revision, result.historyStateToken);
        failureDiagnostics_.clear();
    }
    emit documentChanged(QString::fromStdString(identityKey), SctDocumentUpdate{
        SctDocumentUpdateKind::SourceStatus,
        state.session->currentSnapshot(), std::nullopt,
        state.session->semanticProjection()});
    const auto message = result.saved
        ? (state.session->isDirty()
            ? tr("SCT patch checkpoint saved; newer edits remain unsaved.")
            : tr("SCT patch checkpoint saved."))
        : result.cancelled
            ? tr("SCT patch checkpoint cancelled.")
            : result.diagnostics.empty()
                ? tr("The SCT patch checkpoint could not be saved.")
                : QString::fromStdString(result.diagnostics.front().message);
    emit checkpointCompleted(QString::fromStdString(identityKey),
        result.saved, result.cancelled, message);
}

void SctDocumentController::finishPublication() {
    auto result = publicationWatcher_.result();
    const auto locator = publicationLocator_;
    publicationLocator_.reset();
    if (!locator) return;
    auto* state = findState(*locator);
    const auto identityKey = identity(*locator);
    bool replacedSource = false;
    bool newerEdits = false;
    if (state != nullptr) {
        state->publicationDiagnostics = result.diagnostics;
        if (result.receipt) {
            replacedSource = result.receipt->replacedSource;
            newerEdits = state->session->workingRevision() != result.receipt->revision;
            state->lastPublication = result.receipt;
            if (replacedSource) state->status = SourceStatus::Changed;
        }
        emit documentChanged(identityKey, SctDocumentUpdate{
            SctDocumentUpdateKind::SourceStatus,
            state->session->currentSnapshot(), std::nullopt,
            state->session->semanticProjection()});
    }
    failureDiagnostics_ = result.infrastructureDiagnostics;
    failurePipelineDiagnostics_.clear();
    const auto message = result.succeeded()
        ? (newerEdits
            ? tr("SCT revision %1 exported; newer edits remain unpublished.")
                .arg(result.receipt->revision.value)
            : tr("SCT revision %1 exported successfully.")
                .arg(result.receipt->revision.value))
        : result.cancelled
            ? tr("SCT export cancelled.")
            : !result.infrastructureDiagnostics.empty()
                ? QString::fromStdString(result.infrastructureDiagnostics.front().message)
                : !result.diagnostics.empty()
                    ? QString::fromStdString(result.diagnostics.front().message)
                    : tr("The SCT document could not be exported.");
    emit publicationCompleted(identityKey, result.succeeded(), result.cancelled,
        message, replacedSource);
}

void SctDocumentController::retireCheckpoint(DocumentState& state) {
    if (!state.checkpointWatcher) return;
    state.checkpointStop.request_stop();
    state.checkpointWatcher->disconnect(this);
    retiredCheckpoints_.push_back(std::move(state.checkpointWatcher));
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
    if (result.load.cancelled) {
        emit operationCompleted(QString::fromStdString(key), false, true, tr("SCT operation cancelled."));
        return;
    }
    if (!result.load.succeeded()) {
        failureDiagnostics_ = result.load.infrastructureDiagnostics;
        if (result.load.inspection)
            failurePipelineDiagnostics_ = result.load.inspection->diagnostics;
        emit operationCompleted(QString::fromStdString(key), false, false, firstError(result.load));
        return;
    }
    failureDiagnostics_ = result.load.infrastructureDiagnostics;
    auto found = documents_.find(key);
    auto makeSession = [&]() {
        if (result.patchApplied) {
            return std::make_unique<core::SctEditSession>(result.baseline,
                result.load.document, result.authoredArms, result.textRepairs);
        }
        return std::make_unique<core::SctEditSession>(result.load.document);
    };
    if (found == documents_.end()) {
        DocumentState state{*locator, makeSession()};
        state.status = SourceStatus::Current;
        state.patchConflict = result.patchConflict;
        state.editBlocked = result.patchConflict;
        documents_.emplace(key, std::move(state));
    } else {
        retireMaterialization(found->second);
        retireCheckpoint(found->second);
        found->second.session = makeSession();
        found->second.status = SourceStatus::Current;
        found->second.patchConflict = result.patchConflict;
        found->second.editBlocked = result.patchConflict;
    }
    emit documentChanged(QString::fromStdString(key),
        SctDocumentUpdate{SctDocumentUpdateKind::Replacement,
            documents_.at(key).session->currentSnapshot(), std::nullopt,
            documents_.at(key).session->semanticProjection()});
    emit focusRequested(QString::fromStdString(key));
    const auto message = result.patchConflict
        ? tr("The source baseline was opened read-only because its saved patch could not be applied.")
        : result.patchApplied
            ? tr("SCT document and saved patch loaded.")
        : operation == Operation::Reimporting
        ? tr("Text convention applied without rereading the source asset.")
        : tr("SCT document loaded.");
    emit operationCompleted(QString::fromStdString(key), true, false, message);
}

}  // namespace salsa::qt
