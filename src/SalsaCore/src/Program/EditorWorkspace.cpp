#include "SalsaCore/Program/EditorWorkspace.h"
#include "SalsaCore/Persistence/AtomicFile.h"
#include "ProgramInternal.h"
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
} // namespace
Result<std::shared_ptr<EditorWorkspace>> EditorWorkspaceIO::open(const EditorOpenRequest &request, std::stop_token stop,
                                                                 const SctLoadingObserver &observer) {
    try {
        const auto start = std::chrono::steady_clock::now();
        auto out = std::make_shared<EditorWorkspace>();
        out->service = std::make_shared<ProgramService>();
        progress(observer, stop, "Inspecting dataset inventory");
        auto dataset = LocalGameProject::inspect(request.dataset, stop, [&](const DatasetScanProgress &p) {
            if (observer)
                observer({p.phase == DatasetScanPhase::Discovering ? "Discovering dataset assets"
                                                                   : "Reading dataset fingerprints",
                          p.completed, p.total.value_or(0), p.currentPath.generic_string()});
        });
        if (!dataset)
            return Result<std::shared_ptr<EditorWorkspace>>::failure(dataset.diagnostics());
        auto diagnostics = dataset.diagnostics();
        out->dataset = std::make_shared<const LocalGameProject>(std::move(dataset).takeValue());
        progress(observer, stop, "Assessing workspace");
        auto assessment = take(LocalSalsaWorkspace::assess(request.workspace, out->dataset->dataset()));
        require(!assessment.requiresReassociation() ||
                    request.acceptance == WorkspaceDatasetAcceptance::UserConfirmedReassociation,
                "Workspace belongs to another dataset. Explicit reassociation is required before opening it.");
        out->workspace = std::make_shared<const LocalSalsaWorkspace>(
            take(LocalSalsaWorkspace::openOrCreate(request.workspace, out->dataset->dataset(), request.acceptance)));
        const auto &w = *out->workspace;
        const auto path = w.componentPath(projectPath(w));
        progress(observer, stop, "Recovering workspace transactions", false);
        for (const auto &recovered :
             WorkspaceTransactionService::recoverAll(w.descriptor().root, w.descriptor().components.transactions))
            require(recovered.status == WorkspaceTransactionStatus::Verified ||
                        recovered.status == WorkspaceTransactionStatus::RolledBack,
                    "Workspace recovery is blocked");
        progress(observer, stop, "Reading authoring checkpoint");
        // Existing unsupported authoring containers fail in the binary decoder.
        // Never silently replace them with a newly imported native collection.
        if (std::filesystem::exists(path)) {
            ProgramOpenRequest open;
            open.path = path;
            open.progress = observer;
            auto loaded = out->service->openProject(std::move(open), stop).get();
            if (!loaded)
                return Result<std::shared_ptr<EditorWorkspace>>::failure(loaded.diagnostics());
            out->status = loaded.value().status;
            out->presentation = std::move(loaded.value().presentation);
            if (loaded.value().encodedDigest)
                out->checkpointDigest = loaded.value().encodedDigest->toHex();
        } else {
            const auto patches = w.componentPath(w.descriptor().components.patches);
            require(!std::filesystem::exists(patches) ||
                        std::filesystem::directory_iterator(patches) == std::filesystem::directory_iterator{},
                    "This workspace has retired patch authoring without a binary project. Choose a fresh workspace; "
                    "existing files were not replaced.");
            progress(observer, stop, "Importing script collection");
            NativeLoadRequest load;
            load.project = {uuid()};
            load.workers = request.nativeWorkers;
            load.progress = observer;
            for (const auto &asset : out->dataset->snapshot().assets) {
                auto convention = request.textConvention;
                if (auto found = request.textOverrides.find(asset.locator.path().generic_string());
                    found != request.textOverrides.end())
                    convention = found->second;
                NativeLoadItem item{SctAuthoringBaseline{{},
                                                         out->dataset->dataset().identity.fingerprint,
                                                         asset,
                                                         {uuid()},
                                                         convention,
                                                         {},
                                                         {request.dataset.platform, convention.has_value()}}};
                item.context.name = asset.locator.path().stem().string();
                item.context.platform = request.dataset.platform;
                item.context.region = request.dataset.region;
                item.workingBytes = std::max<std::uint64_t>(1024 * 1024, asset.byteSize * 8);
                if (convention) {
                    item.options.sourceTextEncoding = spice::sct::sctTextEncodingFor(*convention);
                    item.options.footerTextPromotion = spice::sct::SctFooterTextPromotionPolicy::TrustSelectedEncoding;
                }
                item.read = [dataset = out->dataset, workspace = out->workspace,
                             locator = asset.locator](std::stop_token stop) {
                    if (stop.stop_requested())
                        return Result<std::vector<std::uint8_t>>::failure(failure("Script import cancelled"));
                    auto source = dataset->loadAsset(locator);
                    if (!source)
                        return Result<std::vector<std::uint8_t>>::failure(source.diagnostics());
                    auto retained = workspace->retainBaseline(source.value().descriptor.revision, source.value().bytes);
                    if (!retained)
                        return Result<std::vector<std::uint8_t>>::failure(retained.diagnostics());
                    const auto bytes = std::as_bytes(std::span(source.value().bytes));
                    std::vector<std::uint8_t> native(bytes.size());
                    std::memcpy(native.data(), bytes.data(), bytes.size());
                    return Result<std::vector<std::uint8_t>>::success(std::move(native));
                };
                load.items.push_back(std::move(item));
            }
            auto imported = out->service->load(std::move(load), stop).get();
            if (!imported)
                return Result<std::shared_ptr<EditorWorkspace>>::failure(imported.diagnostics());
            diagnostics.insert(diagnostics.end(), imported.diagnostics().begin(), imported.diagnostics().end());
            out->status = imported.value();
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
