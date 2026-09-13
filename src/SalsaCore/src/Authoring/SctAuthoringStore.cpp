#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SalsaCore/Authoring/SctProjectContainer.h"
#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <set>

namespace salsa::core {
namespace {
using Json = nlohmann::ordered_json;
Diagnostic error(std::string message, DiagnosticCode code = DiagnosticCode::PersistenceReadFailed) {
    return {DiagnosticSeverity::Error, code, std::move(message)};
}
Result<std::string> read(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size > SctProjectContainer::MaximumContainerBytes) return Result<std::string>::failure(error("Project file exceeds its read limit."));
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<std::string>::failure(error("Authoring checkpoint cannot be opened."));
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty()) input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input || input.peek() != std::ifstream::traits_type::eof()) return Result<std::string>::failure(error("Authoring checkpoint changed or cannot be read completely."));
    return Result<std::string>::success(std::move(text));
}
std::span<const std::byte> bytes(const std::string& text) { return std::as_bytes(std::span(text.data(), text.size())); }
Result<void> recover(const LocalSalsaWorkspace& workspace) {
    for (const auto& result : WorkspaceTransactionService::recoverAll(workspace.descriptor().root, workspace.descriptor().components.transactions)) {
        if (!result.succeeded()) return Result<void>::failure(result.diagnostics.empty()
            ? std::vector{error("Workspace recovery is blocked.")} : result.diagnostics);
    }
    return Result<void>::success();
}
}
std::filesystem::path SctAuthoringStore::relativePath(const LocalSalsaWorkspace& workspace) {
    return workspace.descriptor().components.authoring / L"project.salsaproject";
}
Result<std::optional<SctAuthoringCheckpoint>> SctAuthoringStore::load(const LocalSalsaWorkspace& workspace, std::stop_token stop, SctLoadingObserver observer) {
    if (stop.stop_requested()) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring reopen cancelled.", DiagnosticCode::Cancelled));
    if (observer) observer({"Recovering project", 0, 0, {}, false});
    auto recovered = recover(workspace);
    if (!recovered) return Result<std::optional<SctAuthoringCheckpoint>>::failure(recovered.diagnostics());
    if (observer) observer({"Reading project", 0, 0, {}});
    if (stop.stop_requested()) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring reopen cancelled.", DiagnosticCode::Cancelled));
    const auto path = workspace.componentPath(relativePath(workspace));
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring checkpoint status is unavailable."));
    if (!exists) return Result<std::optional<SctAuthoringCheckpoint>>::success(std::nullopt);
    if (observer) observer({"Reading project", 0, 0, {}});
    auto text = read(path);
    if (!text) return Result<std::optional<SctAuthoringCheckpoint>>::failure(text.diagnostics());
    if (observer) observer({"Decompressing project", 0, 0, {}});
    auto unpacked = SctProjectContainer::decode(bytes(text.value()));
    if (!unpacked) return Result<std::optional<SctAuthoringCheckpoint>>::failure(unpacked.diagnostics());
    try {
        const auto& manifest = unpacked.value().at("project.json");
        const std::string manifestText(reinterpret_cast<const char*>(manifest.data()), manifest.size());
        std::vector<std::set<std::string>> objects;
        const auto root = Json::parse(manifestText, [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 128) throw std::runtime_error("Checkpoint nesting limit exceeded.");
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
                throw std::runtime_error("Duplicate checkpoint field.");
            else if (event == Json::parse_event_t::object_end) objects.pop_back();
            return true;
        });
        if (root.size() != 6 || root.at("format") != "jahorta.salsa.authoring-checkpoint"
            || !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion") != 2
            || root.at("workspaceId") != workspace.descriptor().workspaceId)
            return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Unsupported or mismatched authoring checkpoint."));
        if (observer) observer({"Decoding authoring state", 0, 0, {}});
        auto project = SctAuthoringCodec::decode(root.at("semantic").dump());
        if (!project) return Result<std::optional<SctAuthoringCheckpoint>>::failure(project.diagnostics());
        auto presentation = SctAuthoringPresentationCodec::decode(root.at("presentation").dump(), project.value());
        if (!presentation) return Result<std::optional<SctAuthoringCheckpoint>>::failure(presentation.diagnostics());
        const auto& bindings = root.at("bindings");
        if (!bindings.is_object() || bindings.size() != project.value().baselines.size())
            return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Checkpoint baseline identity bindings are incomplete."));
        std::set<std::string> allowed{"project.json"};
        for (const auto& baseline : project.value().baselines) allowed.insert("baselines/" + std::to_string(baseline.id.value) + ".sct");
        for (const auto& [name, data] : unpacked.value()) if (!allowed.contains(name) && !name.starts_with("legacy/"))
            return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Unexpected project record."));
        SctImportedPrograms programs;
        for (const auto& baseline : project.value().baselines) {
            if (stop.stop_requested()) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring reopen cancelled.", DiagnosticCode::Cancelled));
            const auto key = "baselines/" + std::to_string(baseline.id.value) + ".sct";
            if (bindings.at(std::to_string(baseline.id.value)) != key)
                return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Baseline record binding mismatch."));
            if (observer) observer({"Verifying baseline", programs.size(), project.value().baselines.size(), baseline.source.locator.identityKey()});
            auto restored = SctAuthoringImporter::retain(project.value(), baseline.id, unpacked.value().at(key));
            if (!restored) return Result<std::optional<SctAuthoringCheckpoint>>::failure(restored.diagnostics());
            programs.push_back(std::move(restored).takeValue());
        }
        for (const auto& script : project.value().scripts) {
            if (script.contentUses.size() != 1 || !std::holds_alternative<SctContentId>(script.contentUses.front())
                || !project.value().find(std::get<SctContentId>(script.contentUses.front()))->authoredState)
                return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Project has no complete authored program."));
        }
        SctAuthoringState state{std::move(project).takeValue(), std::move(programs)};
        for (const auto& [name, data] : unpacked.value()) if (name.starts_with("legacy/")) state.retainedEvidence.emplace(name, data);
        if (observer) observer({"Validating authoring state", 0, 0, {}});
        auto valid = SctAuthoringSession::validate(state);
        if (!valid) return Result<std::optional<SctAuthoringCheckpoint>>::failure(valid.diagnostics());
        auto digest = sha256(bytes(text.value()));
        if (!digest) return Result<std::optional<SctAuthoringCheckpoint>>::failure(digest.diagnostics());
        if (stop.stop_requested()) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring reopen cancelled.", DiagnosticCode::Cancelled));
        return Result<std::optional<SctAuthoringCheckpoint>>::success(SctAuthoringCheckpoint{
            std::move(state), std::move(presentation).takeValue(), digest.value()}, valid.diagnostics());
    } catch (const std::exception& e) {
        return Result<std::optional<SctAuthoringCheckpoint>>::failure(error(std::string("Malformed authoring checkpoint: ") + e.what()));
    }
}
Result<Sha256Digest> SctAuthoringStore::save(const LocalSalsaWorkspace& workspace, const SctAuthoringState& state,
    const SctAuthoringPresentation& presentation, std::optional<Sha256Digest> expected, std::stop_token stop,
    const WorkspaceTransactionHooks& hooks) {
    try {
    auto valid = SctAuthoringSession::validate(state);
    if (!valid) return Result<Sha256Digest>::failure(valid.diagnostics());
    auto semantic = SctAuthoringCodec::encode(state.project);
    auto view = SctAuthoringPresentationCodec::encode(presentation, state.project);
    if (!semantic) return Result<Sha256Digest>::failure(semantic.diagnostics());
    if (!view) return Result<Sha256Digest>::failure(view.diagnostics());
    auto recovered = recover(workspace);
    if (!recovered) return Result<Sha256Digest>::failure(recovered.diagnostics());
    auto previous = load(workspace, stop);
    if (!previous) return Result<Sha256Digest>::failure(previous.diagnostics());
    for (const auto& program : state.programs) {
        if (stop.stop_requested()) return Result<Sha256Digest>::failure(error("Authoring save cancelled.", DiagnosticCode::Cancelled));
        auto retained = workspace.retainBaseline(program->baseline().source.revision, program->bytes());
        if (!retained) return Result<Sha256Digest>::failure(retained.diagnostics());
    }
    auto artifact = mutation(workspace, state, presentation, expected);
    if (!artifact) return Result<Sha256Digest>::failure(artifact.diagnostics());
    auto digest = sha256(*artifact.value().replacement);
    if (!digest) return Result<Sha256Digest>::failure(digest.diagnostics());
    auto transaction = generateSctAuthoringUuid();
    if (!transaction) return Result<Sha256Digest>::failure(transaction.diagnostics());
    std::vector<WorkspaceArtifactMutation> mutations;
    mutations.push_back(std::move(artifact).takeValue());
    // These are generated checkpoints for the existing rebase machinery. The
    // semantic project remains authoritative; they are never read as live edits.
    for (const auto& script : state.project.scripts) {
        const auto& baseline = *state.project.find(script.baseline);
        const auto program = *std::ranges::find_if(state.programs, [&](const auto& p) { return p->baseline().id == baseline.id; });
        auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script.id);
        if (!working) return Result<Sha256Digest>::failure(working.diagnostics());
        // The legacy rebase envelope cannot express source attachment removal.
        // Carry that immutable evidence only in its derived snapshot; never
        // put it back into the authored program or fresh executable output.
        auto rebaseDocument = std::make_shared<spice::sct::SctDocument>(*working.value().document);
        rebaseDocument->opaqueAttachments = program->document().opaqueAttachments;
        auto rebaseState = working.value(); rebaseState.document = std::move(rebaseDocument);
        auto patch = SalsaScriptPatchService::diff({std::make_shared<const spice::sct::SctDocument>(program->document())}, rebaseState, baseline.textConvention);
        if (!patch) return Result<Sha256Digest>::failure(patch.diagnostics());
        auto payload = SalsaScriptPatchCodec::serialize(patch.value());
        if (!payload) return Result<Sha256Digest>::failure(payload.diagnostics());
        auto envelope = PatchEnvelopeCodec::serialize({baseline.datasetFingerprint, {{baseline.source.locator, baseline.source.revision}}, {},
            {std::string(SalsaScriptPatchCodec::PayloadType), SalsaScriptPatchCodec::SchemaVersion, payload.value()}});
        if (!envelope) return Result<Sha256Digest>::failure(envelope.diagnostics());
        const auto path = workspace.patchPath(baseline.source.locator);
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) return Result<Sha256Digest>::failure(error("Generated patch status is unavailable."));
        std::optional<std::string> prior;
        if (exists) {
            auto text = read(path); if (!text) return Result<Sha256Digest>::failure(text.diagnostics());
            auto hash = sha256(bytes(text.value())); if (!hash) return Result<Sha256Digest>::failure(hash.diagnostics());
            prior = hash.value().toHex();
        }
        const auto data = bytes(envelope.value());
        mutations.push_back({path.lexically_relative(workspace.descriptor().root), exists, prior, std::vector<std::byte>(data.begin(), data.end())});
    }
    if (previous.value()) for (const auto& script : previous.value()->state.project.scripts) {
        const auto& baseline = *previous.value()->state.project.find(script.baseline);
        if (std::ranges::any_of(state.project.scripts, [&](const auto& s) {
                return state.project.find(s.baseline)->source.locator == baseline.source.locator;
            })) continue;
        const auto path = workspace.patchPath(baseline.source.locator);
        std::error_code ec; const bool exists = std::filesystem::exists(path, ec);
        if (ec) return Result<Sha256Digest>::failure(error("Retired generated patch status is unavailable."));
        if (!exists) continue;
        auto old = read(path); if (!old) return Result<Sha256Digest>::failure(old.diagnostics());
        auto hash = sha256(bytes(old.value())); if (!hash) return Result<Sha256Digest>::failure(hash.diagnostics());
        mutations.push_back({path.lexically_relative(workspace.descriptor().root), true, hash.value().toHex(), {}});
    }
    WorkspaceTransactionRequest request{transaction.value(), workspace.descriptor().workspaceId,
        "authoring-project-save", state.project.id.value, std::move(mutations)};
    if (stop.stop_requested()) return Result<Sha256Digest>::failure(error("Authoring save cancelled.", DiagnosticCode::Cancelled));
    auto prepared = WorkspaceTransactionService::prepare(workspace.descriptor().root, workspace.descriptor().components.transactions, request);
    if (!prepared.succeeded()) return Result<Sha256Digest>::failure(prepared.diagnostics);
    // Once prepared, complete the durable transaction; cancellation cannot report
    // failure while silently leaving a committed replacement behind.
    auto committed = WorkspaceTransactionService::commitPrepared(workspace.descriptor().root, workspace.descriptor().components.transactions, transaction.value(), hooks);
    if (committed.status != WorkspaceTransactionStatus::Verified)
        return Result<Sha256Digest>::failure(committed.diagnostics.empty()
            ? std::vector{error("Authoring save requires recovery.")} : committed.diagnostics);
    return Result<Sha256Digest>::success(digest.value());
    } catch (const std::exception& e) { return Result<Sha256Digest>::failure(error(std::string("Project save failed: ") + e.what())); }
}
Result<WorkspaceArtifactMutation> SctAuthoringStore::mutation(const LocalSalsaWorkspace& workspace,
    const SctAuthoringState& state, const SctAuthoringPresentation& presentation, std::optional<Sha256Digest> expected) {
    try {
    auto valid = SctAuthoringSession::validate(state);
    if (!valid) return Result<WorkspaceArtifactMutation>::failure(valid.diagnostics());
    auto semantic = SctAuthoringCodec::encode(state.project);
    auto view = SctAuthoringPresentationCodec::encode(presentation, state.project);
    if (!semantic) return Result<WorkspaceArtifactMutation>::failure(semantic.diagnostics());
    if (!view) return Result<WorkspaceArtifactMutation>::failure(view.diagnostics());
    Json bindings = Json::object();
    SctProjectRecords records = state.retainedEvidence;
    auto programs = state.programs;
    std::ranges::sort(programs, {}, [](const auto& p) { return p->baseline().id; });
    for (const auto& program : programs) {
        const auto key = "baselines/" + std::to_string(program->baseline().id.value) + ".sct";
        bindings[std::to_string(program->baseline().id.value)] = key;
        records.emplace(key, program->bytes());
    }
    // Capsules remain evidence. Bundle existing files without interpreting the
    // Python object graph or making those records execution state.
    std::set<std::string> capsules;
    for (const auto& script : state.project.scripts) if (script.legacyOrigin) capsules.insert(script.legacyOrigin->capsuleId);
    for (const auto& capsule : capsules) {
        if (capsule.empty() || capsule.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            return Result<WorkspaceArtifactMutation>::failure(error("Invalid legacy capsule identity."));
        const auto root = workspace.componentPath(workspace.descriptor().components.imports / capsule / "capsule");
        std::error_code ec;
        const bool exists = std::filesystem::exists(root, ec);
        if (ec) return Result<WorkspaceArtifactMutation>::failure(error("Capsule evidence cannot be inspected."));
        if (!exists) continue; // An origin reference may precede local capsule availability.
        const auto noReparsePoint = [](const auto& path) {
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        if (!noReparsePoint(root) || !noReparsePoint(root.parent_path()))
            return Result<WorkspaceArtifactMutation>::failure(error("Capsule links cannot enter a project container."));
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!noReparsePoint(entry.path())) return Result<WorkspaceArtifactMutation>::failure(error("Capsule links cannot enter a project container."));
            if (!entry.is_regular_file()) continue;
            auto data = read(entry.path());
            if (!data) return Result<WorkspaceArtifactMutation>::failure(data.diagnostics());
            const auto key = "legacy/" + capsule + "/" + entry.path().lexically_relative(root).generic_string();
            const auto raw = bytes(data.value()); records.emplace(key, std::vector<std::byte>(raw.begin(), raw.end()));
        }
    }
    const auto text = Json{{"format", "jahorta.salsa.authoring-checkpoint"}, {"schemaVersion", 2},
        {"workspaceId", workspace.descriptor().workspaceId}, {"semantic", Json::parse(semantic.value())},
        {"presentation", Json::parse(view.value())}, {"bindings", bindings}}.dump(2);
    records.emplace("project.json", std::vector<std::byte>(bytes(text).begin(), bytes(text).end()));
    auto packed = SctProjectContainer::encode(records);
    if (!packed) return Result<WorkspaceArtifactMutation>::failure(packed.diagnostics());
    return Result<WorkspaceArtifactMutation>::success({relativePath(workspace), expected.has_value(),
        expected ? std::optional{expected->toHex()} : std::nullopt, std::move(packed).takeValue()});
    } catch (const std::exception& e) { return Result<WorkspaceArtifactMutation>::failure(error(std::string("Project save failed: ") + e.what())); }
}
} // namespace salsa::core
