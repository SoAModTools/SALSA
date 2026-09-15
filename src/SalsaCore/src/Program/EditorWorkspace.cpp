#include "SalsaCore/Program/EditorWorkspace.h"
#include "SalsaCore/Persistence/AtomicFile.h"
#include "ProgramInternal.h"
#include "NativeCollection.h"
#include <set>
#include <mutex>
#include "SpiceMLD/MldDocumentImporter.h"
#include <chrono>
#include <cstring>
#include <cwchar>

namespace salsa::core::program {
namespace {
template <class T> T take(Result<T> r) {
    if (!r) {
        std::string text;
        for (const auto &d : r.diagnostics()) {
            if (!text.empty())
                text += '\n';
            text += d.message;
        }
        throw std::runtime_error(text);
    }
    return std::move(r).takeValue();
}
void check(Result<void> r) {
    if (!r)
        throw std::runtime_error(r.diagnostics().empty() ? "Operation failed" : r.diagnostics().front().message);
}
void progress(const SctLoadingObserver &observer, std::stop_token stop, const char *phase, bool cancellable = true) {
    require(!stop.stop_requested(), "Workspace operation cancelled");
    if (observer)
        observer({phase, 0, 0, {}, cancellable});
}
std::string uuid() {
    return take(generateSctAuthoringUuid());
}
std::filesystem::path projectPath(const LocalSalsaWorkspace &w) {
    return w.descriptor().components.authoring / L"project.salsaproject";
}
std::uint64_t elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}
// Baselines are staged on the destination volume, then moved after the scan's
// final identity is accepted. The stage owns only its freshly created directory.
class ImportBaselines {
public:
    explicit ImportBaselines(const std::filesystem::path& workspace)
        : root_(std::filesystem::absolute(workspace).lexically_normal().parent_path() / (".salsa-import-" + uuid())),
          store_(root_) {
        std::filesystem::create_directories(root_.parent_path());
        require(std::filesystem::create_directory(root_), "Could not create import baseline stage");
    }
    ~ImportBaselines() { std::error_code ignored; std::filesystem::remove_all(root_, ignored); }
    Result<void> retain(const CapturedSource& source) {
        auto result = store_.retainBaseline(source);
        if (result) { std::lock_guard lock(mutex_); revisions_.insert(source.descriptor().revision); }
        return result;
    }
    void publish(const LocalSalsaWorkspace& workspace) {
        DirectorySctBaselineStore destination(workspace.componentPath(workspace.descriptor().components.baselines));
        for (const auto& revision : revisions_) {
            auto existing = take(destination.loadBaseline(revision));
            if (existing) continue;
            // Both directories normally share a volume. A custom component on a
            // different volume uses a file copy; the original SCT is never reread.
            std::error_code ec;
            std::filesystem::rename(store_.path(revision), destination.path(revision), ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy_file(store_.path(revision), destination.path(revision),
                                           std::filesystem::copy_options::none, ec);
                if (ec) throw std::runtime_error("Could not publish an imported source baseline: " + ec.message());
            }
        }
    }
private:
    std::filesystem::path root_;
    DirectorySctBaselineStore store_;
    std::mutex mutex_;
    std::set<SourceRevision> revisions_;
};
} // namespace
Result<std::shared_ptr<EditorWorkspace>> EditorWorkspaceIO::open(const EditorOpenRequest &request, std::stop_token stop,
                                                                 const SctLoadingObserver &observer) {
    try {
        const auto start = std::chrono::steady_clock::now();
        auto out = std::make_shared<EditorWorkspace>();
        out->service = std::make_shared<ProgramService>();
        progress(observer, stop, "Inspecting workspace layout");
        const auto layout = take(LocalSalsaWorkspace::inspectLayout(request.workspace));
        bool hasCheckpoint = false;
        if (layout) {
            progress(observer, stop, "Recovering workspace transactions", false);
            for (const auto& recovered : WorkspaceTransactionService::recoverAll(layout->root, layout->components.transactions))
                require(recovered.status == WorkspaceTransactionStatus::Verified ||
                        recovered.status == WorkspaceTransactionStatus::RolledBack, "Workspace recovery is blocked");
            hasCheckpoint = std::filesystem::exists(layout->root / layout->components.authoring / L"project.salsaproject");
            if (!hasCheckpoint) {
                const auto patches = layout->root / layout->components.patches;
                require(!std::filesystem::exists(patches) || std::filesystem::is_empty(patches),
                    "This workspace has retired patch authoring without a binary project. Choose a fresh workspace; "
                    "existing files were not replaced.");
            }
        }
        std::unique_ptr<ImportBaselines> baselines;
        std::unique_ptr<NativeDatasetPreparation> native;
        DatasetScanPipeline pipeline;
        pipeline.workers = request.nativeWorkers;
        std::mutex progressMutex;
        auto report = [&](const SctLoadingProgress& p) {
            std::lock_guard lock(progressMutex);
            if (observer) observer(p);
        };
        if (!hasCheckpoint) {
            baselines = std::make_unique<ImportBaselines>(request.workspace);
            native = std::make_unique<NativeDatasetPreparation>(SctAuthoringProjectId{uuid()});
            pipeline.sourceExpansionFactor = 8;
            pipeline.consume = [&](std::size_t ordinal, const CapturedSource& source, std::stop_token token) {
                auto retained = baselines->retain(source);
                if (!retained) return retained;
                auto convention = request.textConvention;
                if (auto found = request.textOverrides.find(source.descriptor().locator.path().generic_string());
                    found != request.textOverrides.end()) convention = found->second;
                SctScriptContext context;
                context.name = source.descriptor().locator.path().stem().string();
                context.platform = request.dataset.platform;
                context.region = request.dataset.region;
                spice::sct::SctDocumentImportOptions options;
                if (convention) options.sourceTextEncoding = spice::sct::sctTextEncodingFor(*convention);
                return native->append(ordinal, source, std::move(context), {uuid()}, convention, options, token, report);
            };
        }
        progress(observer, stop, hasCheckpoint ? "Inspecting dataset inventory" : "Scanning and importing scripts");
        auto dataset = LocalGameProject::inspect(request.dataset, stop, [&](const DatasetScanProgress& p) {
            report({p.phase == DatasetScanPhase::Discovering ? "Discovering dataset assets"
                : hasCheckpoint ? "Reading dataset fingerprints" : "Script ready",
                p.completed, p.total.value_or(0), p.currentPath.generic_string()});
        }, pipeline);
        if (!dataset) return Result<std::shared_ptr<EditorWorkspace>>::failure(dataset.diagnostics());
        auto diagnostics = dataset.diagnostics();
        out->dataset = std::make_shared<const LocalGameProject>(std::move(dataset).takeValue());
        std::shared_ptr<const Program> prepared;
        if (native) {
            auto finished = native->finish(out->dataset->dataset().identity.fingerprint, stop, report);
            if (!finished) return Result<std::shared_ptr<EditorWorkspace>>::failure(finished.diagnostics());
            diagnostics.insert(diagnostics.end(), finished.diagnostics().begin(), finished.diagnostics().end());
            prepared = std::move(finished).takeValue();
            native.reset();
        }
        progress(observer, stop, "Assessing workspace");
        auto assessment = take(LocalSalsaWorkspace::assess(request.workspace, out->dataset->dataset()));
        require(!assessment.requiresReassociation() ||
                    request.acceptance == WorkspaceDatasetAcceptance::UserConfirmedReassociation,
                "Workspace belongs to another dataset. Explicit reassociation is required before opening it.");
        progress(observer, stop, "Opening workspace", false);
        out->workspace = std::make_shared<const LocalSalsaWorkspace>(
            take(LocalSalsaWorkspace::openOrCreate(request.workspace, out->dataset->dataset(), request.acceptance)));
        const auto& w = *out->workspace;
        const auto path = w.componentPath(projectPath(w));
        if (hasCheckpoint) {
            progress(observer, stop, "Reading authoring checkpoint");
            ProgramOpenRequest open;
            open.path = path;
            open.progress = observer;
            auto loaded = out->service->openProject(std::move(open), stop).get();
            if (!loaded) return Result<std::shared_ptr<EditorWorkspace>>::failure(loaded.diagnostics());
            out->status = loaded.value().status;
            out->presentation = std::move(loaded.value().presentation);
            if (loaded.value().encodedDigest) out->checkpointDigest = loaded.value().encodedDigest->toHex();
        } else {
            require(!std::filesystem::exists(path), "Workspace checkpoint appeared during import; reopen the workspace");
            progress(observer, stop, "Publishing imported baselines", false);
            baselines->publish(w);
            baselines.reset();
            progress(observer, stop, "Installing script collection");
            auto installed = out->service->adopt({}, std::move(prepared), stop).get();
            if (!installed) return Result<std::shared_ptr<EditorWorkspace>>::failure(installed.diagnostics());
            out->status = installed.value();
            out->needsInitialSave = true;
        }
        progress(observer, stop, "Preparing script catalog");
        out->catalog = take(out->service->catalog(out->status.current).get());
        out->openedMicroseconds = elapsed(start);
        return Result<std::shared_ptr<EditorWorkspace>>::success(std::move(out), std::move(diagnostics));
    } catch (const std::exception &e) {
        return Result<std::shared_ptr<EditorWorkspace>>::failure(failure(e.what()));
    }
}
Result<EditorSaveResult> EditorWorkspaceIO::save(const EditorWorkspace &session, Revision revision,
                                                 const Presentation &presentation, std::stop_token stop,
                                                 const SctLoadingObserver &observer) {
    try {
        auto start = std::chrono::steady_clock::now();
        require(session.service && session.workspace, "No workspace is installed");
        progress(observer, stop, "Capturing authoring checkpoint");
        auto cp = take(session.service->checkpoint(revision, stop).get());
        const auto &w = *session.workspace;
        auto mutation = take(
            ProgramPersistence::mutation(cp, presentation, projectPath(w), session.checkpointDigest, stop, observer));
        auto digest = take(sha256(*mutation.replacement)).toHex();
        progress(observer, stop, "Saving authoring project", false);
        WorkspaceTransactionRequest request{
            uuid(), w.descriptor().workspaceId, "program-checkpoint", uuid(), {std::move(mutation)}};
        auto prepared =
            WorkspaceTransactionService::prepare(w.descriptor().root, w.descriptor().components.transactions, request);
        if (!prepared.succeeded())
            return Result<EditorSaveResult>::failure(prepared.diagnostics);
        auto committed = WorkspaceTransactionService::commitPrepared(
            w.descriptor().root, w.descriptor().components.transactions, request.transactionId);
        if (committed.status != WorkspaceTransactionStatus::Verified)
            return Result<EditorSaveResult>::failure(committed.diagnostics);
        auto status = take(session.service->markSaved(cp.status.current, cp.status.historyState).get());
        return Result<EditorSaveResult>::success({status, std::move(digest), elapsed(start)});
    } catch (const std::exception &e) {
        return Result<EditorSaveResult>::failure(failure(e.what()));
    }
}
Result<ExportedScript> EditorWorkspaceIO::publish(const EditorWorkspace &session, Revision revision, SctScriptId script,
                                                  const spice::sct::SctDocumentExportOptions &options,
                                                  const std::filesystem::path &destination, bool replaceSource,
                                                  std::stop_token stop, const SctLoadingObserver &observer) {
    try {
        progress(observer, stop, "Capturing script for native export");
        auto cp = take(session.service->checkpoint(revision, stop).get());
        if (session.dataset && !replaceSource) {
            const auto root = std::filesystem::weakly_canonical(session.dataset->dataset().root);
            const auto target = std::filesystem::weakly_canonical(destination);
            auto r = root.begin(), t = target.begin();
            for (; r != root.end() && t != target.end() && _wcsicmp(r->c_str(), t->c_str()) == 0; ++r, ++t) {
            }
            require(r != root.end(),
                    "Source replacement must be explicitly enabled for destinations inside the dataset");
        }
        progress(observer, stop, "Lowering and encoding native SCT");
        ProgramMaterializer materializer;
        auto result = materializer.exportScript(cp, revision, script, {uuid()}, options, stop);
        if (!result)
            return result;
        require(take(session.service->status().get()).current == revision,
                "Authoring changed while export was prepared");
        progress(observer, stop, "Publishing native SCT", false);
        check(replaceFileAtomically(destination, std::as_bytes(std::span(result.value().bytes))));
        return result;
    } catch (const std::exception &e) {
        return Result<ExportedScript>::failure(failure(e.what()));
    }
}
Result<smores::script::Resource> EditorWorkspaceIO::inspectResource(const EditorWorkspace &session,
                                                                    const AssetLocator &locator,
                                                                    smores::script::ResourceRole role,
                                                                    std::stop_token stop) {
    try {
        require(!stop.stop_requested(), "Resource selection cancelled");
        auto source = take(session.dataset->loadContextAsset(locator));
        spice::mld::MldImportOptions options;
        if (session.dataset->dataset().identity.platform)
            options.platformHint = *session.dataset->dataset().identity.platform == GamePlatform::GameCube
                                       ? spice::mld::MldPlatform::GameCube
                                       : spice::mld::MldPlatform::Dreamcast;
        auto parsed = spice::mld::MldDocumentImporter::importBytes(
            {reinterpret_cast<const std::uint8_t *>(source.bytes.data()), source.bytes.size()}, options);
        require(!stop.stop_requested(), "Resource selection cancelled");
        require(parsed.ok() && parsed.document, "SPICE could not inspect the selected MLD");
        const auto digest = source.descriptor.revision.digest.toHex();
        smores::script::ResourceIdentity identity{session.dataset->dataset().identity.fingerprint.digest.toHex(),
                                                  locator.identityKey(), digest, "spice-mld-s6-v1:" + digest};
        return Result<smores::script::Resource>::success(smores::script::describe(identity, role, *parsed.document));
    } catch (const std::exception &e) {
        return Result<smores::script::Resource>::failure(failure(e.what()));
    }
}
} // namespace salsa::core::program
