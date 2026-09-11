#include "Sct/SctDocumentController.h"
#include "Application/SalsaLogging.h"

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

[[nodiscard]] spice::sct::SctParseTraceObserver parserTraceObserver() {
    if (!salsaSctParserLog().isInfoEnabled()) return {};
    return [](const spice::sct::SctParseTraceEvent& event) {
        using enum spice::sct::SctParseTracePhase;
        const auto message = [phase = event.phase] {
            switch (phase) {
            case Starting: return SctDocumentController::tr("Starting SCT parse.");
            case Compression: return SctDocumentController::tr("Inspecting SCT compression.");
            case SectionIndex: return SctDocumentController::tr("Reading the SCT section index.");
            case InstructionTraversal: return SctDocumentController::tr("Walking SCT instructions.");
            case Complete: return SctDocumentController::tr("SCT parse complete.");
            }
            return SctDocumentController::tr("SCT parser phase changed.");
        }();
        qCInfo(salsaSctParserLog).noquote() << message;
    };
}

}  // namespace

SctDocumentController::SctDocumentController(QObject* parent) : QObject(parent) {
    auto project = core::SctAuthoringProject::create();
    if (project) {
        auto session = core::SctAuthoringSession::open({project.value(), {}});
        if (session) authoring_ = std::move(session).takeValue();
    }
    if (authoring_) authoringPresentation_.project = authoring_->state().project.id;
    connect(&authoringSaveWatcher_, &QFutureWatcherBase::finished, this, [this] {
        if (!savingAuthoring_ || !authoringSaveWatcher_.isFinished()) return;
        const auto saved = authoringSaveWatcher_.result();
        const auto locator = savingAuthoringLocator_;
        if (saved && authoring_ && savingAuthoring_
            && authoring_->state().project.id == savingAuthoring_->project.id) {
            authoringCheckpoint_ = saved.value();
            authoring_->markSaved(*savingAuthoring_);
        }
        failureDiagnostics_ = saved.diagnostics();
        savingAuthoring_.reset(); savingAuthoringLocator_.reset();
        for (const auto& [key, state] : documents_) emit documentChanged(QString::fromStdString(key),
            SctDocumentUpdate{SctDocumentUpdateKind::SourceStatus, state.session->currentSnapshot(), {}, state.session->semanticProjection()});
        if (locator) emit checkpointCompleted(identity(*locator), static_cast<bool>(saved), false,
            saved ? tr("Authoring project saved.") : QString::fromStdString(saved.diagnostics().front().message));
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, &SctDocumentController::onFinished);
    connect(&publicationWatcher_, &QFutureWatcherBase::finished,
        this, &SctDocumentController::finishPublication);
}

SctDocumentController::~SctDocumentController() {
    if (authoringSaveWatcher_.isRunning()) authoringSaveWatcher_.waitForFinished();
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
    if (busy() || authoringLoadFailed_) return false;
    if (contains(locator)) {
        emit focusRequested(identity(locator));
        return true;
    }
    if (authoringScript(locator)) {
        restoreAuthoringProjection(locator.identityKey());
        synchronizeCatalog(project.assets().snapshot());
        emit focusRequested(identity(locator));
        emit operationCompleted(identity(locator), true, false, tr("Authoring script loaded."));
        return true;
    }
    begin(Operation::Opening, locator);
    const auto token = stopSource_.get_token();
    auto workspace = workspace_;
    watcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), locator, token, workspace = std::move(workspace)]() {
            return core::SctPatchCheckpointService::load(
                project, workspace.get(), workspace.get(), locator, token,
                parserTraceObserver());
        }));
    return true;
}

bool SctDocumentController::reloadDocument(
    core::LocalGameProject project, const core::AssetLocator& locator) {
    if (busy() || !contains(locator)) return false;
    if (authoringScript(locator)) {
        restoreAuthoringProjection(locator.identityKey());
        synchronizeCatalog(project.assets().snapshot());
        emit operationCompleted(identity(locator), true, false, tr("Authoring projection refreshed."));
        return true;
    }
    begin(Operation::Reloading, locator);
    const auto token = stopSource_.get_token();
    auto workspace = workspace_;
    watcher_.setFuture(QtConcurrent::run(
        [project = std::move(project), locator, token, workspace = std::move(workspace)]() {
            return core::SctPatchCheckpointService::load(
                project, workspace.get(), workspace.get(), locator, token,
                parserTraceObserver());
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
        project, workspace_.get(), workspace_.get(), locator, {},
        parserTraceObserver());
    if (!reopened.load.succeeded() || !reopened.baseline
        || !reopened.patchApplied || reopened.patchConflict) {
        failureDiagnostics_ = reopened.load.infrastructureDiagnostics;
        failurePipelineDiagnostics_ = reopened.load.document
            ? reopened.load.document->diagnostics
            : std::vector<core::SctPipelineDiagnostic>{};
        return false;
    }
    if (!adoptAuthoring(reopened)) return false;
    restoreAuthoringProjection(locator.identityKey());
    state->status = SourceStatus::Current;
    emit editCommitted(identity(locator), tr("Rebased script adopted as one project command."));
    return true;
}

bool SctDocumentController::installTransientDocument(
    const core::AssetLocator& locator,
    std::shared_ptr<const core::SctDocumentSnapshot> provenanceSnapshot,
    const core::SctSemanticState& semanticState) {
    if (busy() || contains(locator) || !provenanceSnapshot
        || !provenanceSnapshot->provenance || !semanticState.document) return false;
    core::SctPatchedLoadResult loaded;
    loaded.baseline = provenanceSnapshot;
    auto snapshot = std::make_shared<core::SctDocumentSnapshot>(*provenanceSnapshot);
    snapshot->document = semanticState.document;
    loaded.load.document = snapshot;
    loaded.authoredArms = semanticState.authoredArms; loaded.textRepairs = semanticState.textRepairs;
    loaded.unboundReferences = semanticState.unboundReferences; loaded.aliases = semanticState.aliases;
    loaded.annotations = semanticState.annotations; loaded.folders = semanticState.folders;
    if (!adoptAuthoring(loaded)) return false;
    restoreAuthoringProjection(locator.identityKey());
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
    const auto unbound = state->session->unboundReferences();
    return core::SctSemanticState{*document,
        {state->session->structuredAuthoring().arms().begin(),
            state->session->structuredAuthoring().arms().end()},
        state->session->workingState().textRepairProvenances(),
        {unbound.begin(), unbound.end()},
        {state->session->aliases().begin(), state->session->aliases().end()},
        {state->session->annotations().begin(), state->session->annotations().end()},
        {state->session->folders().begin(), state->session->folders().end()}};
}

bool SctDocumentController::selectTextConvention(
    const core::AssetLocator& locator,
    const spice::sct::SctKnownTextConvention convention) {
    if (busy()) return false;
    const auto found = documents_.find(locator.identityKey());
    if (found == documents_.end() || !found->second.session
        || !found->second.session->currentSnapshot()->provenance->inspection) return false;
    if (const auto script = authoringScript(locator)) {
        const auto* context = authoring_->state().project.find(*script);
        const auto* content = authoring_->state().project.find(std::get<core::SctContentId>(context->contentUses.front()));
        if (content->physicalPatch || !content->literalOverrides.empty()) {
            InteractionNotice notice; notice.code = QStringLiteral("TextReimportHasEdits");
            notice.message = tr("Undo this script's edits before changing its imported text convention.");
            notice.documentIdentity = identity(locator); emit editRejected(notice); return false;
        }
    }
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
    if (savingAuthoring_) authoringSaveStop_.request_stop();
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
    auto diagnostics = state->session->currentDiagnostics();
    if (state->editBlocked) diagnostics.insert(diagnostics.end(),
        state->blockingDiagnostics.begin(), state->blockingDiagnostics.end());
    return diagnostics;
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
    return state != nullptr && authoring_ && authoring_->isDirty();
}

core::RevisionId SctDocumentController::workingRevision(
    const core::AssetLocator& locator) const {
    const auto* state = findState(locator);
    return state == nullptr || !authoring_ ? core::RevisionId{} : authoring_->state().project.revision;
}

bool SctDocumentController::isSaving(const core::AssetLocator& locator) const {
    (void)locator;
    return savingAuthoring_ != nullptr;
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
    (void)locator;
    return authoring_ && authoring_->canUndo();
}

bool SctDocumentController::canRedo(const core::AssetLocator& locator) const {
    (void)locator;
    return authoring_ && authoring_->canRedo();
}

std::optional<std::string> SctDocumentController::undoDescription(
    const core::AssetLocator& locator) const {
    (void)locator;
    return authoring_ && authoring_->canUndo() ? std::optional{authoring_->undoDescription()} : std::nullopt;
}

std::optional<std::string> SctDocumentController::redoDescription(
    const core::AssetLocator& locator) const {
    (void)locator;
    return authoring_ && authoring_->canRedo() ? std::optional{authoring_->redoDescription()} : std::nullopt;
}

std::vector<core::AssetLocator> SctDocumentController::dirtyLocators() const {
    std::vector<core::AssetLocator> result;
    if (authoring_ && authoring_->isDirty())
        for (const auto& script : authoring_->state().project.scripts)
            result.push_back(authoring_->state().project.find(script.baseline)->source.locator);
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

core::Result<core::SctSemanticFragment> SctDocumentController::captureInstructions(
    const core::AssetLocator& locator,
    const std::span<const spice::sct::SctInstructionId> instructions) const {
    const auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked)
        return core::Result<core::SctSemanticFragment>::failure(core::Diagnostic{
            core::DiagnosticSeverity::Error, core::DiagnosticCode::InvalidSctFragment,
            "The document is not available for copying.", locator.path()});
    return state->session->captureInstructions(instructions);
}

core::Result<core::SctSemanticFragment> SctDocumentController::captureSections(
    const core::AssetLocator& locator,
    const std::span<const spice::sct::SctSectionId> sections) const {
    const auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked)
        return core::Result<core::SctSemanticFragment>::failure(core::Diagnostic{
            core::DiagnosticSeverity::Error, core::DiagnosticCode::InvalidSctFragment,
            "The document is not available for copying.", locator.path()});
    return state->session->captureSections(sections);
}

core::Result<core::SctSemanticFragment> SctDocumentController::captureSemanticUnits(
    const core::AssetLocator& locator,
    const core::SctSemanticSelection& selection) const {
    const auto* state = findState(locator);
    if (state == nullptr || busy() || state->editBlocked)
        return core::Result<core::SctSemanticFragment>::failure(core::Diagnostic{
            core::DiagnosticSeverity::Error, core::DiagnosticCode::InvalidSctFragment,
            "The document is not available for semantic copying.", locator.path()});
    return state->session->captureSemanticUnits(selection);
}

std::vector<std::string> SctDocumentController::suggestSectionNames(
    const core::AssetLocator& locator,
    const core::SctSemanticFragment& fragment) const {
    const auto* state = findState(locator);
    return state == nullptr ? std::vector<std::string>{}
        : core::SctFragmentService::suggestSectionNames(
            state->session->workingState(), fragment);
}

bool SctDocumentController::pasteFragment(const core::AssetLocator& locator,
    const core::SctSemanticFragment& fragment,
    core::SctFragmentPasteDestination destination) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->pasteFragment(
            fragment, std::move(destination)), tr("Fragment pasted."));
}

bool SctDocumentController::pasteFragment(const core::AssetLocator& locator,
    const core::SctSemanticFragment& fragment,
    const core::SctSemanticDestination& destination) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->pasteFragment(
            fragment, destination), tr("Semantic fragment pasted."));
}

bool SctDocumentController::deleteInstructions(const core::AssetLocator& locator,
    const std::span<const spice::sct::SctInstructionId> instructions) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->deleteInstructions(instructions),
            tr("Instructions deleted."));
}

bool SctDocumentController::deleteSections(const core::AssetLocator& locator,
    const std::span<const spice::sct::SctSectionId> sections) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->deleteSections(sections),
            tr("Sections deleted."));
}

bool SctDocumentController::moveInstructionsAfter(const core::AssetLocator& locator,
    const std::span<const spice::sct::SctInstructionId> instructions,
    const spice::sct::SctInstructionId anchor) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->moveInstructionsAfter(
            instructions, anchor), tr("Instructions moved."));
}

bool SctDocumentController::deleteSemanticUnits(
    const core::AssetLocator& locator,
    const core::SctSemanticSelection& selection) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->deleteSemanticUnits(selection),
            tr("Semantic selection deleted."));
}

bool SctDocumentController::moveSemanticUnits(
    const core::AssetLocator& locator,
    const core::SctSemanticSelection& selection,
    const core::SctSemanticMoveDirection direction) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->moveSemanticUnits(selection, direction),
            tr("Semantic selection moved."));
}

bool SctDocumentController::moveSemanticUnits(
    const core::AssetLocator& locator,
    const core::SctSemanticSelection& selection,
    const core::SctSemanticDestination& destination,
    const std::optional<QPoint> globalPosition) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state,
            state->session->moveSemanticUnits(selection, destination),
            tr("Semantic selection moved."), globalPosition);
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
        else return spice::sct::SctSupplementaryTextReference{id};
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

bool SctDocumentController::setVariableAlias(const core::AssetLocator& locator,
    const core::SctVariableKey variable, std::optional<std::string> alias) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->setVariableAlias(
            variable, std::move(alias)), tr("Variable alias updated."));
}

bool SctDocumentController::setAnnotation(const core::AssetLocator& locator,
    core::SctEntityAnnotation annotation) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->setAnnotation(
            std::move(annotation)), tr("Authoring metadata updated."));
}

bool SctDocumentController::createSectionFolder(const core::AssetLocator& locator,
    std::string name, const std::span<const spice::sct::SctSectionId> sections,
    const std::optional<core::SctSectionFolderId> parent) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->createSectionFolder(
            std::move(name), sections, parent), tr("Section folder created."));
}

bool SctDocumentController::updateSectionFolder(const core::AssetLocator& locator,
    core::SctSectionFolder folder) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->updateSectionFolder(
            std::move(folder)), tr("Section folder updated."));
}

bool SctDocumentController::removeSectionFolder(const core::AssetLocator& locator,
    const core::SctSectionFolderId folder) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked
        && applyEditResult(*state, state->session->removeSectionFolder(folder),
            tr("Section folder removed."));
}

bool SctDocumentController::createSupplementaryText(
    const core::AssetLocator& locator, const core::SctCreatedSupplementaryTextKind kind,
    const std::optional<spice::sct::SctSupplementaryTextId> after) {
    auto* state = findState(locator);
    return state != nullptr && !busy() && !state->editBlocked && applyEditResult(*state,
        state->session->createSupplementaryText(kind, after), tr("Supplementary text created."));
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
    if (busy()) return false;
    const auto key = identity(locator);
    if (!authoring_) return false;
    auto result = authoring_->undo();
    if (!result) return false;
    restoreAuthoringProjections();
    emit editCommitted(key, tr("Project undo complete."));
    return true;
}

bool SctDocumentController::redo(const core::AssetLocator& locator) {
    if (busy()) return false;
    const auto key = identity(locator);
    if (!authoring_) return false;
    auto result = authoring_->redo();
    if (!result) return false;
    restoreAuthoringProjections();
    emit editCommitted(key, tr("Project redo complete."));
    return true;
}

bool SctDocumentController::saveDocument(const core::AssetLocator& locator) {
    if (!authoring_ || !workspace_ || authoringLoadFailed_ || savingAuthoring_) return false;
    savingAuthoring_ = authoring_->capture();
    savingAuthoringLocator_ = locator;
    auto workspace = workspace_;
    auto captured = savingAuthoring_;
    auto presentation = authoringPresentation_;
    auto expected = authoringCheckpoint_;
    authoringSaveStop_ = std::stop_source{};
    const auto stop = authoringSaveStop_.get_token();
    authoringSaveWatcher_.setFuture(QtConcurrent::run([workspace, captured, presentation, expected, stop] {
        return core::SctAuthoringStore::save(*workspace, *captured, presentation, expected, stop);
    }));
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
    if (state == nullptr || busy() || publicationLocator_.has_value()
        || state->editBlocked) return false;
    const auto snapshot = state->session->currentSnapshot();
    if (!snapshot || !snapshot->provenance || !snapshot->provenance->inspection)
        return false;
    const auto generation = ++nextPublicationGeneration_;
    publishingAuthoringRevision_ = authoring_ ? authoring_->state().project.revision : core::RevisionId{};
    auto captured = state->session->capturePublicationRevision(generation);
    if (!captured) return false;
    if (authoring_) {
        captured->revision = authoring_->state().project.revision;
        captured->historyStateToken = authoring_->capture();
        if (captured->materialization) {
            captured->materialization->baseRevision = captured->revision;
            captured->materialization->targetRevision = captured->revision;
        }
    }

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
    if (savingAuthoring_) {
        authoringSaveWatcher_.waitForFinished();
        const auto saved = authoringSaveWatcher_.result();
        if (saved && authoring_) authoring_->markSaved(*savingAuthoring_);
        savingAuthoring_.reset(); savingAuthoringLocator_.reset();
    }
    workspace_ = std::move(workspace);
    authoring_.reset(); authoringCheckpoint_.reset(); authoringLoadFailed_ = false;
    if (workspace_) {
        auto loaded = core::SctAuthoringStore::load(*workspace_);
        if (!loaded) {
            failureDiagnostics_ = loaded.diagnostics(); authoringLoadFailed_ = true; return;
        }
        if (loaded.value()) {
            authoringPresentation_ = loaded.value()->presentation;
            authoringCheckpoint_ = loaded.value()->digest;
            auto session = core::SctAuthoringSession::open(std::move(loaded.value()->state));
            if (!session) { failureDiagnostics_ = session.diagnostics(); authoringLoadFailed_ = true; return; }
            authoring_ = std::move(session).takeValue();
            return;
        }
    }
    auto project = core::SctAuthoringProject::create();
    if (!project) { failureDiagnostics_ = project.diagnostics(); authoringLoadFailed_ = true; return; }
    if (workspace_) {
        auto metadata = core::SctWorkspaceAuthoringStore(workspace_->componentPath(
            workspace_->descriptor().components.authoring / L"workspace.json")).load();
        if (!metadata) { failureDiagnostics_ = metadata.diagnostics(); authoringLoadFailed_ = true; return; }
        project.value().workspaceAuthoring = metadata.value();
    }
    auto session = core::SctAuthoringSession::open({project.value(), {}});
    if (!session) { failureDiagnostics_ = session.diagnostics(); authoringLoadFailed_ = true; return; }
    authoring_ = std::move(session).takeValue();
    authoringPresentation_ = {authoring_->state().project.id};
}

std::optional<core::SctScriptId> SctDocumentController::authoringScript(const core::AssetLocator& locator) const {
    if (!authoring_) return {};
    for (const auto& script : authoring_->state().project.scripts)
        if (authoring_->state().project.find(script.baseline)->source.locator == locator) return script.id;
    return {};
}

bool SctDocumentController::adoptAuthoring(const core::SctPatchedLoadResult& load) {
    if (!authoring_ || !load.load.document || load.patchConflict) return false;
    const auto baseline = load.baseline ? load.baseline : load.load.document;
    const auto& provenance = *baseline->provenance;
    const auto& locator = provenance.source().descriptor.locator;
    const auto existing = authoringScript(locator);
    core::SctSemanticState working{load.load.document->document, load.authoredArms, load.textRepairs,
        load.unboundReferences, load.aliases, load.annotations, load.folders};
    core::DatasetIdentity dataset = workspace_ ? workspace_->descriptor().dataset.identity
        : core::DatasetIdentity{{}, {}, provenance.inspection->sourceDatasetFingerprint};
    auto command = authoring_->execute(authoring_->state().project.revision, "Import script", [&](const auto& state) {
        core::SctAuthoringImportRequest request{provenance.source(), dataset,
            {dataset.platform, provenance.textSelectionOrigin == core::SctTextSelectionOrigin::UserSelected},
            provenance.textConvention, locator.path().stem().string()};
        auto adopted = existing
            ? core::SctAuthoringImporter::replaceImportedScript(state.project, state.project.revision, *existing, std::move(request))
            : core::SctAuthoringImporter::import(state.project, state.project.revision, std::move(request));
        if (!adopted) return core::Result<core::SctAuthoringState>::failure(adopted.diagnostics());
        auto programs = state.programs;
        if (existing) std::erase_if(programs, [&](const auto& p) { return p->baseline().id == state.project.find(*existing)->baseline; });
        programs.push_back(adopted.value().program);
        auto edited = core::SctAuthoringMaterializer::replaceWorkingState(adopted.value().project, programs, adopted.value().script, working);
        if (!edited) return core::Result<core::SctAuthoringState>::failure(edited.diagnostics());
        return core::Result<core::SctAuthoringState>::success({std::move(edited).takeValue(), std::move(programs)});
    });
    if (!command) { failureDiagnostics_ = command.diagnostics(); return false; }
    return true;
}

void SctDocumentController::restoreAuthoringProjection(const std::string& key) {
    if (!authoring_) return;
    const auto& project = authoring_->state().project;
    const auto script = std::ranges::find_if(project.scripts, [&](const auto& s) { return project.find(s.baseline)->source.locator.identityKey() == key; });
    if (script == project.scripts.end()) {
        auto found = documents_.find(key);
        if (found != documents_.end()) { retireMaterialization(found->second); retireCheckpoint(found->second); documents_.erase(found); emit documentClosed(QString::fromStdString(key)); }
        return;
    }
    auto working = core::SctAuthoringMaterializer::workingState(project, authoring_->state().programs, script->id);
    if (!working) { failureDiagnostics_ = working.diagnostics(); return; }
    const auto program = *std::ranges::find_if(authoring_->state().programs, [&](const auto& p) { return p->baseline().id == script->baseline; });
    auto baseline = core::SctAuthoringMaterializer::snapshot(*program, std::make_shared<const spice::sct::SctDocument>(program->document()));
    auto snapshot = core::SctAuthoringMaterializer::snapshot(*program, working.value().document);
    auto session = std::make_unique<core::SctEditSession>(baseline, snapshot, working.value().authoredArms,
        working.value().textRepairs, working.value().unboundReferences, working.value().aliases, working.value().annotations, working.value().folders);
    auto found = documents_.find(key);
    if (found == documents_.end()) found = documents_.emplace(key, DocumentState{program->baseline().source.locator, std::move(session)}).first;
    else {
        retireMaterialization(found->second); retireCheckpoint(found->second);
        found->second.session = std::move(session);
        found->second.editBlocked = false; found->second.patchConflict = false;
        found->second.blockingDiagnostics.clear(); found->second.publicationDiagnostics.clear(); found->second.lastPublication.reset();
    }
    emit documentChanged(QString::fromStdString(key), SctDocumentUpdate{SctDocumentUpdateKind::Replacement,
        snapshot, {}, found->second.session->semanticProjection()});
}

void SctDocumentController::restoreAuthoringProjections() {
    std::vector<std::string> keys;
    for (const auto& [key, state] : documents_) keys.push_back(key);
    for (const auto& key : keys) restoreAuthoringProjection(key);
}

core::SctWorkspaceAuthoringState SctDocumentController::workspaceAuthoring() const {
    return authoring_ ? authoring_->state().project.workspaceAuthoring : core::SctWorkspaceAuthoringState{};
}
bool SctDocumentController::adoptWorkspaceCheckpoint() {
    if (!workspace_ || !authoring_ || authoring_->isDirty() || savingAuthoring_) return false;
    auto loaded = core::SctAuthoringStore::load(*workspace_);
    if (!loaded || !loaded.value()) return false;
    auto next = loaded.value()->state;
    for (auto& program : next.programs) {
        const auto prior = std::ranges::find_if(authoring_->state().programs, [&](const auto& p) {
            return p->baseline().id == program->baseline().id && p->baseline().importedDocument == program->baseline().importedDocument;
        });
        if (prior != authoring_->state().programs.end()) program = *prior;
    }
    auto accepted = authoring_->execute(authoring_->state().project.revision, "Promote legacy metadata", [&](const auto&) {
        return core::Result<core::SctAuthoringState>::success(next);
    });
    if (!accepted) { failureDiagnostics_ = accepted.diagnostics(); return false; }
    authoringCheckpoint_ = loaded.value()->digest;
    authoring_->markSaved(authoring_->state());
    restoreAuthoringProjections();
    return true;
}
bool SctDocumentController::discardProjectChanges() {
    if (!workspace_ || savingAuthoring_) return false;
    auto loaded = core::SctAuthoringStore::load(*workspace_);
    if (!loaded) { failureDiagnostics_ = loaded.diagnostics(); return false; }
    if (loaded.value()) {
        // Discard authored changes, while retaining the identity/revision high
        // water marks so a discarded allocation cannot be reused later.
        if (authoring_) {
            auto& project = loaded.value()->state.project;
            const auto revision = std::max(project.revision.value, authoring_->state().project.revision.value);
            if (revision == UINT64_MAX) return false;
            project.revision = {revision + 1};
            project.nextEntityId = std::max(project.nextEntityId, authoring_->state().project.nextEntityId);
            auto saved = core::SctAuthoringStore::save(*workspace_, loaded.value()->state, loaded.value()->presentation, loaded.value()->digest);
            if (!saved) { failureDiagnostics_ = saved.diagnostics(); return false; }
            loaded.value()->digest = saved.value();
        }
        auto session = core::SctAuthoringSession::open(loaded.value()->state);
        if (!session) { failureDiagnostics_ = session.diagnostics(); return false; }
        authoring_ = std::move(session).takeValue(); authoringCheckpoint_ = loaded.value()->digest;
        authoringPresentation_ = loaded.value()->presentation;
    } else {
        auto project = core::SctAuthoringProject::create();
        if (!project) return false;
        auto session = core::SctAuthoringSession::open({project.value(), {}});
        if (!session) return false;
        authoring_ = std::move(session).takeValue(); authoringCheckpoint_.reset();
        authoringPresentation_ = {authoring_->state().project.id};
    }
    restoreAuthoringProjections();
    return true;
}
bool SctDocumentController::setWorkspaceAuthoring(const core::SctWorkspaceAuthoringState& metadata) {
    if (!authoring_ || authoringLoadFailed_) return false;
    auto changed = authoring_->execute(authoring_->state().project.revision, "Edit project metadata", [&](const auto& state) {
        auto next = state; next.project.workspaceAuthoring = metadata;
        return core::Result<core::SctAuthoringState>::success(std::move(next));
    });
    if (!changed) { failureDiagnostics_ = changed.diagnostics(); return false; }
    restoreAuthoringProjections();
    emit editCommitted(QStringLiteral("authoring-project"), tr("Project metadata updated."));
    return true;
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
    QString successMessage,
    std::optional<QPoint> globalPosition) {
    failureDiagnostics_.clear();
    const auto key = identity(state.locator);
    if (!result.committed) {
        InteractionNotice notice;
        notice.code = result.diagnostics.empty()
            ? QStringLiteral("EditRejected")
            : QString::fromStdString(result.diagnostics.front().code);
        notice.message = result.diagnostics.empty()
            ? tr("The edit could not be applied.")
            : QString::fromStdString(result.diagnostics.front().message);
        notice.documentIdentity = key;
        notice.globalPosition = globalPosition;
        if (!result.diagnostics.empty()) notice.target = result.diagnostics.front().target;
        emit editRejected(notice);
        return false;
    }
    const auto script = authoringScript(state.locator);
    const auto working = semanticState(state.locator);
    if (!authoring_ || !script || !working) {
        if (script) restoreAuthoringProjection(state.locator.identityKey());
        else state.editBlocked = true;
        InteractionNotice notice; notice.code = QStringLiteral("ProjectProjectionUnavailable");
        notice.message = tr("The edit could not be captured by the authoring project."); notice.documentIdentity = key;
        emit editRejected(notice); return false;
    }
    auto accepted = authoring_->execute(authoring_->state().project.revision, successMessage.toStdString(), [&](const auto& current) {
        auto next = core::SctAuthoringMaterializer::replaceWorkingState(current.project, current.programs, *script, *working);
        if (!next) return core::Result<core::SctAuthoringState>::failure(next.diagnostics());
        return core::Result<core::SctAuthoringState>::success({std::move(next).takeValue(), current.programs});
    });
    if (!accepted) {
        failureDiagnostics_ = accepted.diagnostics();
        restoreAuthoringProjection(state.locator.identityKey());
        InteractionNotice notice; notice.code = QStringLiteral("ProjectCommandRejected");
        notice.message = QString::fromStdString(failureDiagnostics_.front().message); notice.documentIdentity = key;
        emit editRejected(notice); return false;
    }
    state.publicationDiagnostics.clear();
    failurePipelineDiagnostics_.clear();
    QElapsedTimer notificationTimer;
    notificationTimer.start();
    restoreAuthoringProjection(state.locator.identityKey());
    if (editTimingsEnabled_) {
        qCInfo(salsaSctEditLog).noquote() << QStringLiteral(
            "SALSA edit timing %1: preflight=%2us journal=%3us model-notification=%4us")
            .arg(key).arg(result.preflightMicroseconds)
            .arg(result.journalMicroseconds)
            .arg(notificationTimer.nsecsElapsed() / 1000);
    }
    if (result.suggestedSelection.has_value()
        && result.suggestedSelectionRange.size() <= 1u) {
        emit selectionRequested(key, static_cast<int>(result.suggestedSelection->kind),
            static_cast<qulonglong>(result.suggestedSelection->id));
    }
    if (result.suggestedSelectionRange.size() > 1u) {
        QList<int> kinds;
        QList<qulonglong> ids;
        for (const auto target : result.suggestedSelectionRange) {
            kinds.push_back(static_cast<int>(target.kind));
            ids.push_back(static_cast<qulonglong>(target.id));
        }
        emit selectionRangeRequested(key, kinds, ids);
    }
    emit editCommitted(key, std::move(successMessage));
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
    state.runningAuthoringRevision = authoring_ ? authoring_->state().project.revision : core::RevisionId{};
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
    if (authoring_ && state.runningAuthoringRevision != authoring_->state().project.revision) {
        requestMaterialization(state);
        return;
    }
    if (editTimingsEnabled_) {
        qCInfo(salsaSctEditLog).noquote() << QStringLiteral(
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
        qCInfo(salsaSctStructureLog).noquote() << QStringLiteral(
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
        state.blockingDiagnostics = failurePipelineDiagnostics_;
        auto rollback = state.session->rejectToVerifiedRevision(
            result.baseRevision, state.blockingDiagnostics);
        if (rollback.has_value() && rollback->transition.has_value()) {
            emit documentChanged(QString::fromStdString(identityKey), SctDocumentUpdate{
                SctDocumentUpdateKind::RevisionTransition,
                rollback->snapshot, rollback->transition,
                state.session->semanticProjection()});
            state.requestedMaterializationRevision = rollback->revision;
            state.editBlocked = false;
            state.blockingDiagnostics.clear();
            failurePipelineDiagnostics_.clear();
            InteractionNotice notice;
            notice.code = QStringLiteral("BackgroundVerificationRestored");
            notice.message = tr(
                "Background verification rejected the edit and restored the last verified revision.");
            notice.documentIdentity = QString::fromStdString(identityKey);
            notice.prominent = true;
            emit editRejected(notice);
        } else {
            InteractionNotice notice;
            notice.code = QStringLiteral("BackgroundVerificationBlocked");
            notice.message = tr(
                "Background verification rejected the current revision; editing is paused.");
            notice.documentIdentity = QString::fromStdString(identityKey);
            notice.prominent = true;
            emit editRejected(notice);
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
        const bool current = authoring_ && publishingAuthoringRevision_ == authoring_->state().project.revision;
        if (current) state->publicationDiagnostics = result.diagnostics;
        if (result.receipt) {
            replacedSource = result.receipt->replacedSource;
            newerEdits = !current;
            if (current) state->lastPublication = result.receipt;
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
    if (!result.patchConflict && !adoptAuthoring(result)) {
        emit operationCompleted(QString::fromStdString(key), false, false,
            failureDiagnostics_.empty() ? tr("The script could not be adopted by the authoring project.")
                : QString::fromStdString(failureDiagnostics_.front().message));
        return;
    }
    auto found = documents_.find(key);
    auto makeSession = [&]() {
        if (result.patchApplied) {
            return std::make_unique<core::SctEditSession>(result.baseline,
                result.load.document, result.authoredArms, result.textRepairs,
                result.unboundReferences, result.aliases, result.annotations,
                result.folders);
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
    if (authoringScript(*locator)) restoreAuthoringProjection(key);
    else emit documentChanged(QString::fromStdString(key),
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
