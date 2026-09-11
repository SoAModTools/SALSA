#include "SalsaCore/Authoring/SctAuthoringStore.h"
#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>

namespace salsa::core {
namespace {
using Json = nlohmann::ordered_json;
Diagnostic error(std::string message, DiagnosticCode code = DiagnosticCode::PersistenceReadFailed) {
    return {DiagnosticSeverity::Error, code, std::move(message)};
}
Result<std::string> read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<std::string>::failure(error("Authoring checkpoint cannot be opened."));
    std::string text{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) return Result<std::string>::failure(error("Authoring checkpoint cannot be read."));
    return Result<std::string>::success(std::move(text));
}
std::span<const std::byte> bytes(const std::string& text) { return std::as_bytes(std::span(text.data(), text.size())); }
// Guard deterministic reimport identity across parser upgrades. IDs are scoped
// to this baseline/document, and must not be rebound merely because bytes match.
Result<std::string> signature(const SctImportedProgram& program) {
    Json entities = Json::array();
    for (const auto& section : program.document().sections) {
        entities.push_back({"section", std::to_string(section.id.value()), section.nameBytes, section.content.index()});
        if (const auto* code = std::get_if<spice::sct::SctScriptSectionContent>(&section.content))
            for (const auto& instruction : code->instructions)
                entities.push_back({"instruction", std::to_string(instruction.id.value()), instruction.opcode});
        if (const auto* text = std::get_if<spice::sct::SctStringSectionContent>(&section.content))
            entities.push_back({"string", std::to_string(text->string.id.value())});
    }
    for (const auto& text : program.document().supplementaryText) entities.push_back({"supplementary", std::to_string(text.id.value())});
    for (const auto& opaque : program.document().opaqueAttachments) entities.push_back({"opaque", std::to_string(opaque.id.value())});
    auto digest = sha256(bytes(entities.dump()));
    if (!digest) return Result<std::string>::failure(digest.diagnostics());
    return Result<std::string>::success(digest.value().toHex());
}
Result<void> recover(const LocalSalsaWorkspace& workspace) {
    for (const auto& result : WorkspaceTransactionService::recoverAll(workspace.descriptor().root, workspace.descriptor().components.transactions)) {
        if (!result.succeeded()) return Result<void>::failure(result.diagnostics.empty()
            ? std::vector{error("Workspace recovery is blocked.")} : result.diagnostics);
    }
    return Result<void>::success();
}
}
std::filesystem::path SctAuthoringStore::relativePath(const LocalSalsaWorkspace& workspace) {
    return workspace.descriptor().components.authoring / L"project.json";
}
Result<std::optional<SctAuthoringCheckpoint>> SctAuthoringStore::load(const LocalSalsaWorkspace& workspace, std::stop_token stop) {
    auto recovered = recover(workspace);
    if (!recovered) return Result<std::optional<SctAuthoringCheckpoint>>::failure(recovered.diagnostics());
    const auto path = workspace.componentPath(relativePath(workspace));
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring checkpoint status is unavailable."));
    if (!exists) return Result<std::optional<SctAuthoringCheckpoint>>::success(std::nullopt);
    auto text = read(path);
    if (!text) return Result<std::optional<SctAuthoringCheckpoint>>::failure(text.diagnostics());
    try {
        std::vector<std::set<std::string>> objects;
        const auto root = Json::parse(text.value(), [&](int, Json::parse_event_t event, Json& value) {
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
                throw std::runtime_error("Duplicate checkpoint field.");
            else if (event == Json::parse_event_t::object_end) objects.pop_back();
            return true;
        });
        if (root.size() != 6 || root.at("format") != "jahorta.salsa.authoring-checkpoint"
            || !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion") != 1
            || root.at("workspaceId") != workspace.descriptor().workspaceId)
            return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Unsupported or mismatched authoring checkpoint."));
        auto project = SctAuthoringCodec::decode(root.at("semantic").dump());
        if (!project) return Result<std::optional<SctAuthoringCheckpoint>>::failure(project.diagnostics());
        auto presentation = SctAuthoringPresentationCodec::decode(root.at("presentation").dump(), project.value());
        if (!presentation) return Result<std::optional<SctAuthoringCheckpoint>>::failure(presentation.diagnostics());
        const auto& bindings = root.at("bindings");
        if (!bindings.is_object() || bindings.size() != project.value().baselines.size())
            return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Checkpoint baseline identity bindings are incomplete."));
        SctImportedPrograms programs;
        for (const auto& baseline : project.value().baselines) {
            if (stop.stop_requested()) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Authoring reopen cancelled.", DiagnosticCode::Cancelled));
            auto retained = workspace.loadBaseline(baseline.source.revision);
            if (!retained) return Result<std::optional<SctAuthoringCheckpoint>>::failure(retained.diagnostics());
            if (!retained.value()) return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Required immutable authoring baseline is missing."));
            auto restored = SctAuthoringImporter::restore(project.value(), baseline.id, std::move(*retained.value()), stop);
            if (!restored) return Result<std::optional<SctAuthoringCheckpoint>>::failure(restored.diagnostics());
            auto key = signature(*restored.value());
            if (!key) return Result<std::optional<SctAuthoringCheckpoint>>::failure(key.diagnostics());
            if (bindings.at(std::to_string(baseline.id.value)) != key.value())
                return Result<std::optional<SctAuthoringCheckpoint>>::failure(error("Reimport changed physical identities; the checkpoint cannot be rebound automatically."));
            programs.push_back(std::move(restored).takeValue());
        }
        SctAuthoringState state{std::move(project).takeValue(), std::move(programs)};
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
        auto patch = SalsaScriptPatchService::diff({std::make_shared<const spice::sct::SctDocument>(program->document())}, working.value(), baseline.textConvention);
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
}
Result<WorkspaceArtifactMutation> SctAuthoringStore::mutation(const LocalSalsaWorkspace& workspace,
    const SctAuthoringState& state, const SctAuthoringPresentation& presentation, std::optional<Sha256Digest> expected) {
    auto valid = SctAuthoringSession::validate(state);
    if (!valid) return Result<WorkspaceArtifactMutation>::failure(valid.diagnostics());
    auto semantic = SctAuthoringCodec::encode(state.project);
    auto view = SctAuthoringPresentationCodec::encode(presentation, state.project);
    if (!semantic) return Result<WorkspaceArtifactMutation>::failure(semantic.diagnostics());
    if (!view) return Result<WorkspaceArtifactMutation>::failure(view.diagnostics());
    Json bindings = Json::object();
    auto programs = state.programs;
    std::ranges::sort(programs, {}, [](const auto& p) { return p->baseline().id; });
    for (const auto& program : programs) {
        auto key = signature(*program);
        if (!key) return Result<WorkspaceArtifactMutation>::failure(key.diagnostics());
        bindings[std::to_string(program->baseline().id.value)] = key.value();
    }
    const auto text = Json{{"format", "jahorta.salsa.authoring-checkpoint"}, {"schemaVersion", 1},
        {"workspaceId", workspace.descriptor().workspaceId}, {"semantic", Json::parse(semantic.value())},
        {"presentation", Json::parse(view.value())}, {"bindings", bindings}}.dump(2);
    return Result<WorkspaceArtifactMutation>::success({relativePath(workspace), expected.has_value(),
        expected ? std::optional{expected->toHex()} : std::nullopt, std::vector<std::byte>(bytes(text).begin(), bytes(text).end())});
}
} // namespace salsa::core
