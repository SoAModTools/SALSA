#include "SalsaCore/Persistence/SctWorkspaceRebase.h"
#include "SalsaCore/Authoring/SctAuthoringStore.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"
#include "SalsaCore/Persistence/PatchEnvelopeFileStore.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <set>

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] Diagnostic rebaseError(std::string message,
    const std::filesystem::path& path = {},
    const DiagnosticCode code = DiagnosticCode::InvalidSctPatch) {
    return {DiagnosticSeverity::Error, code, std::move(message), path};
}

[[nodiscard]] Result<std::vector<std::byte>> readBytes(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return Result<std::vector<std::byte>>::failure(rebaseError(
        "A rebase artifact could not be opened.", path,
        DiagnosticCode::PersistenceReadFailed));
    const auto end = stream.tellg();
    if (end < 0) return Result<std::vector<std::byte>>::failure(rebaseError(
        "A rebase artifact has an invalid size.", path,
        DiagnosticCode::PersistenceReadFailed));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))
        return Result<std::vector<std::byte>>::failure(rebaseError(
            "A rebase artifact could not be read completely.", path,
            DiagnosticCode::PersistenceReadFailed));
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::string> digest(
    const std::span<const std::byte> bytes) {
    const auto value = sha256(bytes);
    if (!value) return Result<std::string>::failure(value.diagnostics());
    return Result<std::string>::success(value.value().toHex());
}

[[nodiscard]] std::string contextToken(const std::string& workspaceId,
    const AssetLocator& locator, const SourceRevision& oldRevision,
    const std::optional<SourceRevision>& newRevision,
    const std::string& patchDigest) {
    std::string seed = workspaceId + '|' + locator.identityKey() + '|'
        + oldRevision.digest.toHex() + '|'
        + (newRevision ? newRevision->digest.toHex() : "missing") + '|'
        + patchDigest;
    const auto value = sha256(std::as_bytes(std::span{seed.data(), seed.size()}));
    return value ? value.value().toHex() : seed;
}

class SingleAssetProject final : public GameProjectContext, public AssetCatalog {
public:
    SingleAssetProject(DatasetContext dataset, SourceAssetSnapshot source)
        : dataset_(std::move(dataset)), source_(std::move(source)),
          catalog_{{source_.descriptor}, dataset_.identity.fingerprint} {}
    [[nodiscard]] const DatasetContext& dataset() const noexcept override {
        return dataset_;
    }
    [[nodiscard]] const AssetCatalog& assets() const noexcept override { return *this; }
    [[nodiscard]] const AssetCatalogSnapshot& snapshot() const noexcept override {
        return catalog_;
    }
    [[nodiscard]] Result<SourceAssetSnapshot> loadAsset(
        const AssetLocator& locator) const override {
        if (locator != source_.descriptor.locator)
            return Result<SourceAssetSnapshot>::failure(rebaseError(
                "The in-memory rebase source does not contain this asset."));
        return Result<SourceAssetSnapshot>::success(source_);
    }
private:
    DatasetContext dataset_;
    SourceAssetSnapshot source_;
    AssetCatalogSnapshot catalog_;
};

[[nodiscard]] SctLoadResult loadWithConvention(
    const GameProjectContext& project, const AssetLocator& locator,
    const std::optional<spice::sct::SctKnownTextConvention> convention,
    const std::stop_token stopToken) {
    auto loaded = SctDocumentLoader::load(project, locator, stopToken);
    if (!loaded.succeeded() || stopToken.stop_requested()) return loaded;
    if (loaded.document->provenance->textConvention == convention) return loaded;
    return SctDocumentLoader::materialize(loaded.inspection, convention,
        SctTextSelectionOrigin::UserSelected, stopToken);
}

[[nodiscard]] std::vector<std::byte> receiptBytes(
    const SctWorkspaceRebasePlan& plan,
    const SctPatchRebasePreview& preview,
    const std::span<const SctMergeResolution> resolutions) {
    Json root{{"formatId", "jahorta.salsa.patch-rebase-receipt"},
        {"schemaVersion", 1}, {"planId", plan.corePlan.id},
        {"asset", pathUtf8(plan.candidate.locator.path())},
        {"oldSourceRevision", plan.candidate.oldSourceRevision.digest.toHex()},
        {"newSourceRevision", plan.newSource.descriptor.revision.digest.toHex()},
        {"oldPatchDigest", plan.candidate.patchFileDigest},
        {"newPatchPayloadDigest", preview.rebasedPatch
            ? digest(preview.serializedPatch).value() : std::string{}},
        {"resolutions", Json::array()}};
    for (const auto& resolution : resolutions)
        root["resolutions"].push_back(Json{{"conflictId", resolution.conflictId},
            {"kind", static_cast<int>(resolution.kind)}});
    auto text = root.dump(2);
    text.push_back('\n');
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}

}  // namespace

SctRebaseDiscovery SctWorkspaceRebaseService::discover(
    const LocalGameProject& project, const LocalSalsaWorkspace& workspace,
    const std::stop_token stopToken) {
    SctRebaseDiscovery result;
    const auto patchRoot = workspace.componentPath(
        workspace.descriptor().components.patches);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(patchRoot, error)) {
        if (stopToken.stop_requested()) {
            result.diagnostics.push_back({DiagnosticSeverity::Info,
                DiagnosticCode::Cancelled, "Stale-patch discovery was cancelled.", patchRoot});
            return result;
        }
        if (error) break;
        if (!entry.is_regular_file(error) || error
            || !entry.path().filename().wstring().ends_with(L".salsa-patch.json")) continue;
        const auto bytes = readBytes(entry.path());
        if (!bytes) {
            result.diagnostics.insert(result.diagnostics.end(),
                bytes.diagnostics().begin(), bytes.diagnostics().end());
            continue;
        }
        const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
            bytes.value().size());
        const auto envelope = PatchEnvelopeCodec::deserialize(text);
        if (!envelope || envelope.value().affectedAssets.size() != 1u) {
            result.diagnostics.push_back(rebaseError(
                "A patch file could not be inventoried for rebase.", entry.path()));
            continue;
        }
        const auto& expectation = envelope.value().affectedAssets.front();
        const auto current = std::ranges::find(project.snapshot().assets,
            expectation.locator, &AssetDescriptor::locator);
        const std::optional<SourceRevision> revision = current == project.snapshot().assets.end()
            ? std::nullopt : std::optional<SourceRevision>{current->revision};
        if (revision && *revision == expectation.expectedRevision) continue;
        const auto patchDigest = digest(bytes.value());
        if (!patchDigest) {
            result.diagnostics.insert(result.diagnostics.end(),
                patchDigest.diagnostics().begin(), patchDigest.diagnostics().end());
            continue;
        }
        result.candidates.push_back({expectation.locator,
            expectation.expectedRevision, revision, entry.path(), patchDigest.value(),
            contextToken(workspace.descriptor().workspaceId, expectation.locator,
                expectation.expectedRevision, revision, patchDigest.value()),
            !revision.has_value()});
    }
    if (error) result.diagnostics.push_back(rebaseError(
        "The workspace patch directory could not be scanned: " + error.message(),
        patchRoot, DiagnosticCode::PersistenceReadFailed));
    std::ranges::sort(result.candidates, {}, [](const auto& candidate) {
        return candidate.locator.identityKey();
    });
    return result;
}

Result<SctWorkspaceRebasePlan> SctWorkspaceRebaseService::build(
    const LocalGameProject& project, const LocalSalsaWorkspace& workspace,
    const SctStalePatchCandidate& candidate,
    const std::span<const SctReconciliationDecision> decisions,
    const std::stop_token stopToken) {
    if (candidate.sourceMissing || !candidate.newSourceRevision)
        return Result<SctWorkspaceRebasePlan>::failure(rebaseError(
            "The current source asset is missing; its patch cannot be rebased.",
            candidate.locator.path(), DiagnosticCode::AssetNotFound));
    const auto patchBytes = readBytes(candidate.patchPath);
    if (!patchBytes) return Result<SctWorkspaceRebasePlan>::failure(patchBytes.diagnostics());
    const auto currentPatchDigest = digest(patchBytes.value());
    if (!currentPatchDigest || currentPatchDigest.value() != candidate.patchFileDigest)
        return Result<SctWorkspaceRebasePlan>::failure(rebaseError(
            "The saved patch changed after stale-patch discovery.", candidate.patchPath,
            DiagnosticCode::SourceChanged));
    const std::string patchText(reinterpret_cast<const char*>(patchBytes.value().data()),
        patchBytes.value().size());
    auto envelope = PatchEnvelopeCodec::deserialize(patchText);
    if (!envelope) return Result<SctWorkspaceRebasePlan>::failure(envelope.diagnostics());
    auto patch = SalsaScriptPatchCodec::deserialize(envelope.value().payload.bytes);
    if (!patch) return Result<SctWorkspaceRebasePlan>::failure(patch.diagnostics());
    auto retained = workspace.loadBaseline(candidate.oldSourceRevision);
    if (!retained) return Result<SctWorkspaceRebasePlan>::failure(retained.diagnostics());
    if (!retained.value()) return Result<SctWorkspaceRebasePlan>::failure(rebaseError(
        "The exact old source baseline required for rebase is missing.",
        candidate.locator.path(), DiagnosticCode::SctBaselineMissing));
    auto newSource = project.loadAsset(candidate.locator);
    if (!newSource) return Result<SctWorkspaceRebasePlan>::failure(newSource.diagnostics());
    if (newSource.value().descriptor.revision != *candidate.newSourceRevision)
        return Result<SctWorkspaceRebasePlan>::failure(rebaseError(
            "The current source changed while the rebase was being prepared.",
            candidate.locator.path(), DiagnosticCode::SourceChanged));

    DatasetContext oldDataset = project.dataset();
    oldDataset.identity.fingerprint = envelope.value().sourceDatasetFingerprint;
    SingleAssetProject oldProject(oldDataset,
        {AssetDescriptor{candidate.locator, retained.value()->size(),
            candidate.oldSourceRevision}, *retained.value()});
    auto oldLoaded = loadWithConvention(oldProject, candidate.locator,
        patch.value().sourceTextConvention, stopToken);
    auto newLoaded = loadWithConvention(project, candidate.locator,
        patch.value().sourceTextConvention, stopToken);
    if (!oldLoaded.succeeded() || !newLoaded.succeeded()) {
        std::vector<Diagnostic> diagnostics = oldLoaded.infrastructureDiagnostics;
        diagnostics.insert(diagnostics.end(), newLoaded.infrastructureDiagnostics.begin(),
            newLoaded.infrastructureDiagnostics.end());
        if (diagnostics.empty()) diagnostics.push_back(rebaseError(
            "A source baseline could not be materialized for rebase."));
        return Result<SctWorkspaceRebasePlan>::failure(std::move(diagnostics));
    }
    std::vector<SctReconciliationDecision> copied(decisions.begin(), decisions.end());
    auto core = SctPatchRebaseService::build({candidate.locator,
        candidate.oldSourceRevision, *candidate.newSourceRevision,
        {oldLoaded.document->document, {}, {}}, patch.value(),
        {newLoaded.document->document, {}, {}}, workspace.descriptor().workspaceId,
        candidate.contextToken, std::move(copied)});
    if (!core) return Result<SctWorkspaceRebasePlan>::failure(core.diagnostics());
    return Result<SctWorkspaceRebasePlan>::success({candidate,
        std::move(newSource).takeValue(), newLoaded.document,
        std::move(envelope).takeValue(),
        std::move(core).takeValue()});
}

SctWorkspaceRebaseCommitResult SctWorkspaceRebaseService::commit(
    const LocalGameProject& project, const LocalSalsaWorkspace& workspace,
    const SctWorkspaceRebasePlan& plan,
    const SctPatchRebasePreview& preview,
    const std::span<const SctMergeResolution> resolutions) {
    const std::array selections{SctWorkspaceRebaseCommitSelection{
        &plan, &preview, resolutions}};
    return commitSelected(project, workspace, selections);
}

SctWorkspaceRebaseCommitResult SctWorkspaceRebaseService::commitSelected(
    const LocalGameProject& project, const LocalSalsaWorkspace& workspace,
    const std::span<const SctWorkspaceRebaseCommitSelection> selections) {
    SctWorkspaceRebaseCommitResult result;
    if (selections.empty()) {
        result.diagnostics.push_back(rebaseError(
            "At least one complete rebase preview must be selected for commit."));
        return result;
    }

    std::vector<WorkspaceArtifactMutation> mutations;
    auto authored = SctAuthoringStore::load(workspace);
    if (!authored) { result.diagnostics = authored.diagnostics(); return result; }
    std::set<std::filesystem::path> mutationPaths;
    std::set<std::string> assets;
    std::string aggregatePlanId;
    for (const auto& selection : selections) {
        if (selection.plan == nullptr || selection.preview == nullptr) {
            result.diagnostics.push_back(rebaseError(
                "A selected rebase commit item is incomplete."));
            return result;
        }
        const auto& plan = *selection.plan;
        const auto& preview = *selection.preview;
        if (preview.merge.status != SctMergePreviewStatus::Ready
            || !preview.rebasedPatch
            || preview.merge.planId != plan.corePlan.mergePlan.id
            || !assets.insert(plan.candidate.locator.identityKey()).second) {
            result.diagnostics.push_back(rebaseError(
                "Only unique assets with complete conflict-free previews can be committed."));
            return result;
        }
        const auto current = project.loadAsset(plan.candidate.locator);
        const auto currentPatch = readBytes(plan.candidate.patchPath);
        const auto currentPatchDigest = currentPatch ? digest(currentPatch.value())
            : Result<std::string>::failure(currentPatch.diagnostics());
        if (!current
            || current.value().descriptor.revision != plan.newSource.descriptor.revision
            || !currentPatchDigest
            || currentPatchDigest.value() != plan.candidate.patchFileDigest) {
            result.stale = true;
            result.diagnostics.push_back(rebaseError(
                "The source or saved patch changed after the rebase preview.",
                plan.candidate.patchPath, DiagnosticCode::SourceChanged));
            return result;
        }

        const auto patchRelative = std::filesystem::relative(
            plan.candidate.patchPath, workspace.descriptor().root);
        std::optional<std::vector<std::byte>> replacement;
        if (!preview.rebasedPatch->empty() || authored.value()) {
            PatchEnvelope envelope{project.dataset().identity.fingerprint,
                {{plan.candidate.locator, plan.newSource.descriptor.revision}}, {},
                {std::string(SalsaScriptPatchCodec::PayloadType),
                    SalsaScriptPatchCodec::SchemaVersion, preview.serializedPatch}};
            const auto encoded = PatchEnvelopeCodec::serialize(envelope);
            if (!encoded) {
                result.diagnostics = encoded.diagnostics();
                return result;
            }
            const auto bytes = std::as_bytes(std::span{
                encoded.value().data(), encoded.value().size()});
            replacement = std::vector<std::byte>{bytes.begin(), bytes.end()};
        }
        if (!mutationPaths.insert(patchRelative).second) {
            result.diagnostics.push_back(rebaseError(
                "Two selected rebases target the same patch artifact.", patchRelative));
            return result;
        }
        mutations.push_back({patchRelative, true,
            plan.candidate.patchFileDigest, std::move(replacement)});

        const auto baselineRelative = workspace.descriptor().components.baselines
            / (plan.newSource.descriptor.revision.digest.toHex() + ".sct-source");
        const auto existingBaseline = workspace.loadBaseline(
            plan.newSource.descriptor.revision);
        if (!existingBaseline) {
            result.diagnostics = existingBaseline.diagnostics();
            return result;
        }
        if (!existingBaseline.value() && mutationPaths.insert(baselineRelative).second)
            mutations.push_back({baselineRelative, false, std::nullopt,
                plan.newSource.bytes});

        const auto receiptRelative = workspace.descriptor().components.receipts
            / (plan.corePlan.id + ".patch-rebase.json");
        if (!mutationPaths.insert(receiptRelative).second) {
            result.diagnostics.push_back(rebaseError(
                "Two selected rebases target the same receipt artifact.", receiptRelative));
            return result;
        }
        mutations.push_back({receiptRelative, false, std::nullopt,
            receiptBytes(plan, preview, selection.resolutions)});
        aggregatePlanId += plan.corePlan.id;
        aggregatePlanId.push_back('|');
        result.committedAssets.push_back(plan.candidate.locator);
        if (authored.value()) {
            auto& state = authored.value()->state;
            const auto script = std::ranges::find_if(state.project.scripts, [&](const auto& s) {
                return state.project.find(s.baseline)->source.locator == plan.candidate.locator;
            });
            if (script != state.project.scripts.end()) {
                const auto oldBaseline = script->baseline;
                const auto& provenance = *plan.newBaselineSnapshot->provenance;
                auto imported = SctAuthoringImporter::replaceImportedScript(state.project, state.project.revision, script->id,
                    {plan.newSource, project.dataset().identity, {project.dataset().identity.platform ? project.dataset().identity.platform : script->platform,
                        provenance.textSelectionOrigin == SctTextSelectionOrigin::UserSelected}, provenance.textConvention, script->name});
                if (!imported) { result.diagnostics = imported.diagnostics(); return result; }
                auto working = SalsaScriptPatchService::apply({std::make_shared<const spice::sct::SctDocument>(imported.value().program->document())}, *preview.rebasedPatch);
                if (!working) { result.diagnostics = working.diagnostics(); return result; }
                std::erase_if(state.programs, [&](const auto& p) { return p->baseline().id == oldBaseline; });
                state.programs.push_back(imported.value().program);
                auto next = SctAuthoringMaterializer::replaceWorkingState(imported.value().project, state.programs, imported.value().script, working.value());
                if (!next) { result.diagnostics = next.diagnostics(); return result; }
                state.project = std::move(next).takeValue();
            }
        }
    }

    if (authored.value()) {
        auto artifact = SctAuthoringStore::mutation(workspace, authored.value()->state, authored.value()->presentation, authored.value()->digest);
        if (!artifact) { result.diagnostics = artifact.diagnostics(); return result; }
        mutations.push_back(std::move(artifact).takeValue());
    }

    const auto aggregateDigest = sha256(std::as_bytes(std::span{
        aggregatePlanId.data(), aggregatePlanId.size()}));
    if (!aggregateDigest) {
        result.diagnostics = aggregateDigest.diagnostics();
        result.committedAssets.clear();
        return result;
    }
    const auto planId = aggregateDigest.value().toHex();
    const auto transactionId = "rebase-" + planId.substr(0, 32);
    WorkspaceTransactionRequest transaction{transactionId,
        workspace.descriptor().workspaceId, "patch-rebase", planId,
        std::move(mutations)};
    result.transaction = WorkspaceTransactionService::prepare(
        workspace.descriptor().root,
        workspace.descriptor().components.transactions, transaction);
    if (result.transaction.status != WorkspaceTransactionStatus::Prepared) {
        result.diagnostics = result.transaction.diagnostics;
        result.committedAssets.clear();
        return result;
    }
    result.transaction = WorkspaceTransactionService::commitPrepared(
        workspace.descriptor().root,
        workspace.descriptor().components.transactions, transactionId);
    result.committed = result.transaction.status == WorkspaceTransactionStatus::Verified;
    result.diagnostics = result.transaction.diagnostics;
    if (!result.committed) result.committedAssets.clear();
    return result;
}

}  // namespace salsa::core
