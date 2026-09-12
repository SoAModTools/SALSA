#include "SalsaCore/Authoring/SctPublicationSet.h"
#include "SalsaCore/Authoring/SctRouteAuthoring.h"
#include <Windows.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
namespace salsa::core {
namespace {
namespace fs = std::filesystem;
const fs::path journal = L".salsa-publication/transactions";
Diagnostic error(std::string message) {
    return {DiagnosticSeverity::Error, DiagnosticCode::SctExportFailed, std::move(message)};
}
std::string pathKey(const fs::path &p) {
    auto s = p.generic_string();
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
class OutputLease final {
    HANDLE handle_ = nullptr;
    bool acquired_ = false;

  public:
    explicit OutputLease(const fs::path &root) {
        std::error_code ec;
        auto canonical = fs::weakly_canonical(root, ec);
        if (ec)
            return;
        auto key = pathKey(canonical);
        auto hash = sha256(std::as_bytes(std::span(key)));
        if (!hash)
            return;
        auto suffix = hash.value().toHex();
        std::wstring name = L"Local\\SALSA-publication-" + std::wstring(suffix.begin(), suffix.end());
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_) {
            auto status = WaitForSingleObject(handle_, 0);
            acquired_ = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
        }
    }
    ~OutputLease() {
        if (acquired_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }
    explicit operator bool() const {
        return acquired_;
    }
};
Result<std::optional<Sha256Digest>> digest(const fs::path &path) {
    std::error_code ec;
    if (!fs::exists(path, ec) && !ec)
        return Result<std::optional<Sha256Digest>>::success({});
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return Result<std::optional<Sha256Digest>>::failure(error("Cannot read output: " + path.string()));
    auto size = stream.tellg();
    if (size < 0)
        return Result<std::optional<Sha256Digest>>::failure(error("Cannot size output."));
    stream.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (!bytes.empty())
        stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!stream)
        return Result<std::optional<Sha256Digest>>::failure(error("Cannot finish reading output."));
    auto hash = sha256(bytes);
    if (!hash)
        return Result<std::optional<Sha256Digest>>::failure(hash.diagnostics());
    return Result<std::optional<Sha256Digest>>::success(hash.value());
}
nlohmann::json receiptJson(const SctPublicationSetReceipt &r) {
    using Json = nlohmann::json;
    Json j{{"format", "jahorta.salsa.publication-set-receipt"},
           {"schemaVersion", 1},
           {"project", r.project.value},
           {"revision", std::to_string(r.revision.value)},
           {"transaction", r.transactionId},
           {"qualification", "Valid only with a verified transaction and matching hashes for every output."},
           {"routes", Json::array()},
           {"outputs", Json::array()}};
    for (auto id : r.routes)
        j["routes"].push_back(std::to_string(id.value));
    for (const auto &o : r.outputs) {
        const auto &options = o.target.options;
        Json records = Json::array();
        for (const auto &record : o.realization.records) {
            Json locations = Json::array();
            for (const auto &loc : record.locations) {
                if (const auto *entity = std::get_if<spice::sct::SctDocumentEntityId>(&loc))
                    locations.push_back({{"kind", "entity"},
                                         {"type", entity->index()},
                                         {"id", std::visit(
                                                    [](auto id) -> std::string {
                                                        if constexpr (std::is_same_v<decltype(id), std::monostate>)
                                                            return "0";
                                                        else
                                                            return std::to_string(id.value());
                                                    },
                                                    *entity)}});
                else if (const auto *site = std::get_if<spice::sct::SctParameterSite>(&loc))
                    locations.push_back(
                        {{"kind", "parameter"},
                         {"instruction", std::to_string(site->instruction.value())},
                         {"parameter", site->parameter.schemaIndex},
                         {"group", site->parameter.repeatedGroupOrdinal ? Json(*site->parameter.repeatedGroupOrdinal)
                                                                        : Json(nullptr)}});
                else
                    throw std::runtime_error("Unsupported generated mapping location in publication receipt.");
            }
            records.push_back({{"type", record.entity.index()},
                               {"id", std::visit([](auto id) { return std::to_string(id.value); }, record.entity)},
                               {"locations", locations}});
        }
        j["outputs"].push_back({{"script", std::to_string(o.target.script.value)},
                                {"path", o.target.relativePath.generic_string()},
                                {"sha256", o.digest.toHex()},
                                {"platform", static_cast<unsigned>(options.platform)},
                                {"characters", static_cast<unsigned>(options.textEncoding.characters)},
                                {"messageSpace", static_cast<unsigned>(options.textEncoding.messageSpace)},
                                {"byteOrder", static_cast<unsigned>(options.byteOrder)},
                                {"wrapper", static_cast<unsigned>(options.wrapper)},
                                {"realization", o.realization.key.realization.value},
                                {"mappings", records}});
    }
    return j;
}

bool confined(const fs::path &root, const fs::path &relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_path() ||
        relative.lexically_normal() != relative)
        return false;
    for (const auto &c : relative)
        if (c == L".." || c == L"." || c.empty())
            return false;
    // Existing reparse/symlink components cannot redirect installation outside the selected root.
    auto path = root;
    for (const auto &c : relative) {
        path /= c;
        std::error_code ec;
        auto status = fs::symlink_status(path, ec);
        if (!ec && fs::is_symlink(status))
            return false;
    }
    std::error_code ec;
    const auto resolved = fs::weakly_canonical(root / relative, ec);
    if (ec)
        return false;
    const auto base = pathKey(fs::weakly_canonical(root, ec)) + "/";
    return !ec && pathKey(resolved).starts_with(base);
}
} // namespace
std::vector<WorkspaceTransactionResult> SctPublicationSet::recover(const fs::path &root) {
    OutputLease lease(root);
    if (!lease)
        return {{"",
                 WorkspaceTransactionStatus::RecoveryBlocked,
                 {error("Another publication owns this output directory.")}}};
    if (!confined(root, journal))
        return {{"",
                 WorkspaceTransactionStatus::RecoveryBlocked,
                 {error("Publication journal is not confined to the selected root.")}}};
    return WorkspaceTransactionService::recoverAll(root, journal);
}
Result<SctPreparedPublicationSet> SctPublicationSet::prepare(const SctPublicationSetRequest &request,
                                                             std::stop_token stop) {
    auto fail = [](std::string s) { return Result<SctPreparedPublicationSet>::failure(error(std::move(s))); };
    try {
        if (!request.captured || request.expectedRevision != request.captured->project.revision)
            return fail("Captured authoring revision is missing or stale.");
        if (request.root.empty() || !request.root.is_absolute() || !fs::is_directory(request.root))
            return fail("Select an existing absolute output directory.");
        OutputLease lease(request.root);
        if (!lease)
            return fail("Another publication owns this output directory.");
        for (const auto &r : recover(request.root))
            if (!r.succeeded())
                return Result<SctPreparedPublicationSet>::failure(r.diagnostics);
        if (stop.stop_requested())
            return fail("Publication cancelled before preparation.");
        auto scripts = SctRouteAuthoring::closure(request.captured->project, request.seeds);
        if (scripts.empty() || request.targets.size() != scripts.size())
            return fail("Supply one target for every script in the route dependency closure.");
        std::set<SctScriptId> seen;
        std::set<std::string> paths;
        SctPreparedPublicationSet prepared;
        auto uuid = generateSctAuthoringUuid();
        if (!uuid)
            return Result<SctPreparedPublicationSet>::failure(uuid.diagnostics());
        prepared.receipt_.project = request.captured->project.id;
        prepared.receipt_.revision = request.expectedRevision;
        prepared.receipt_.root = fs::weakly_canonical(request.root);
        prepared.receipt_.transactionId = uuid.value();
        prepared.transaction_ = {uuid.value(), request.captured->project.id.value, "sct-publication-set",
                                 std::to_string(request.expectedRevision.value)};
        SctAuthoringMaterializationRequest materialize{
            std::make_shared<const SctAuthoringProject>(request.captured->project), request.captured->programs,
            scripts};
        auto targets = request.targets;
        std::ranges::sort(targets, {}, &SctPublicationTarget::script);
        for (const auto &target : targets) {
            if (!std::ranges::binary_search(scripts, target.script) || !seen.insert(target.script).second)
                return fail("Duplicate or unexpected script target.");
            auto locator = AssetLocator::fromRelativePath(target.relativePath);
            if (!locator || !confined(request.root, target.relativePath) ||
                pathKey(target.relativePath).starts_with(".salsa-publication") ||
                !paths.insert(pathKey(target.relativePath)).second)
                return fail(
                    "Output paths must be unique, normalized, and confined; publication journal paths are reserved.");
            const auto *script = request.captured->project.find(target.script);
            if (!script)
                return fail("Output script is missing.");
            const auto *baseline = request.captured->project.find(script->baseline);
            if (!baseline)
                return fail("Output baseline is missing.");
            // A load names the game asset, not a host-selected alias. Paired outputs retain these names.
            bool routeMember = false;
            for (const auto &c : request.captured->project.connections)
                if (c.route && (c.route->sourceScript == target.script || c.route->destinationScript == target.script))
                    routeMember = true;
            if (routeMember &&
                pathKey(target.relativePath.filename()) != pathKey(baseline->source.locator.path().filename()))
                return fail("Route output filenames must match their explicit imported game-asset bindings.");
            const auto destination = request.root / target.relativePath;
            auto hash = digest(destination);
            if (!hash)
                return Result<SctPreparedPublicationSet>::failure(hash.diagnostics());
            if (target.originalSource &&
                pathKey(fs::weakly_canonical(*target.originalSource)) == pathKey(fs::weakly_canonical(destination))) {
                if (!target.allowSourceReplacement)
                    return fail("Replacing an original source requires explicit source-replacement authorization.");
                if (!hash.value() || *hash.value() != baseline->source.revision.digest)
                    return fail("Original source changed since import; source replacement refused.");
            }
            prepared.transaction_.mutations.push_back(
                {target.relativePath,
                 hash.value().has_value(),
                 hash.value() ? std::optional{hash.value()->toHex()} : std::nullopt,
                 {}});
            materialize.outputOptions.emplace(target.script, spice::sct::SctDocumentExportOptions{
                                                                 target.options.platform, target.options.textEncoding,
                                                                 target.options.byteOrder, target.options.wrapper});
        }
        for (const auto &c : request.captured->project.connections)
            if (c.route && seen.contains(c.route->sourceScript)) {
                prepared.receipt_.routes.push_back(c.id);
                const auto &a = materialize.outputOptions.at(c.route->sourceScript);
                const auto &b = materialize.outputOptions.at(c.route->destinationScript);
                if (a.targetPlatform != b.targetPlatform || a.byteOrder != b.byteOrder)
                    return fail("Connected output profiles must use the same platform and byte order.");
            }
        prepared.outputs_ = SctAuthoringMaterializer::materialize(materialize, stop);
        if (!prepared.outputs_.succeeded()) {
            auto ds = prepared.outputs_.diagnostics;
            for (const auto &s : prepared.outputs_.scripts) {
                ds.insert(ds.end(), s.infrastructureDiagnostics.begin(), s.infrastructureDiagnostics.end());
                for (const auto &d : s.diagnostics)
                    if (d.severity == DiagnosticSeverity::Error)
                        ds.push_back(error(d.message));
            }
            if (ds.empty())
                ds.push_back(error(stop.stop_requested() ? "Publication cancelled during encoding."
                                                         : "Output encoding or verification failed."));
            return Result<SctPreparedPublicationSet>::failure(std::move(ds));
        }
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto &output = *prepared.outputs_.scripts[i].prepared;
            auto bytes = std::as_bytes(std::span(output.bytes));
            prepared.transaction_.mutations[i].replacement = std::vector<std::byte>(bytes.begin(), bytes.end());
            prepared.receipt_.outputs.push_back({targets[i], output.digest, output.realization});
        }
        const auto receiptText = receiptJson(prepared.receipt_).dump(2);
        const auto receiptBytes = std::as_bytes(std::span(receiptText));
        prepared.transaction_.mutations.push_back({fs::path(".salsa-publication/receipts") / (uuid.value() + ".json"),
                                                   false,
                                                   {},
                                                   std::vector<std::byte>(receiptBytes.begin(), receiptBytes.end())});
        return Result<SctPreparedPublicationSet>::success(std::move(prepared));
    } catch (const std::exception &e) {
        return fail(e.what());
    }
}
Result<void> SctPublicationSet::verifyReceipt(const SctPublicationSetReceipt &receipt) {
    if (!receipt.project.valid() || !receipt.revision.value || receipt.outputs.empty())
        return Result<void>::failure(error("Publication receipt is incomplete."));
    if (!validSctAuthoringUuid(receipt.transactionId))
        return Result<void>::failure(error("Invalid publication transaction identity."));
    const auto receiptPath = fs::path(".salsa-publication/receipts") / (receipt.transactionId + ".json");
    const auto manifestPath = journal / receipt.transactionId / "transaction.json";
    if (!confined(receipt.root, receiptPath) || !confined(receipt.root, manifestPath))
        return Result<void>::failure(error("Receipt or journal escaped its output root."));
    try {
        std::ifstream saved(receipt.root / receiptPath), manifest(receipt.root / manifestPath);
        if (!saved || !manifest)
            return Result<void>::failure(error("The complete publication receipt or verified transaction is missing."));
        nlohmann::json stored, transaction;
        saved >> stored;
        manifest >> transaction;
        auto currentReceipt = digest(receipt.root / receiptPath);
        bool receiptRecorded = false;
        if (!currentReceipt || !currentReceipt.value() ||
            transaction.at("mutations").size() != receipt.outputs.size() + 1)
            return Result<void>::failure(error("Publication receipt has a different artifact set from its journal."));
        for (const auto &mutation : transaction.at("mutations"))
            if (mutation.at("relativePath") == receiptPath.generic_string())
                receiptRecorded = mutation.at("afterSha256") == currentReceipt.value()->toHex();
        if (!receiptRecorded)
            return Result<void>::failure(error("Publication receipt bytes changed after verification."));
        if (stored != receiptJson(receipt) || transaction.at("status") != "verified" ||
            transaction.at("workspaceId") != receipt.project.value)
            return Result<void>::failure(
                error("Publication receipt does not match its verified complete transaction."));
    } catch (const std::exception &) {
        return Result<void>::failure(error("Publication receipt or transaction is malformed."));
    }
    std::set<SctScriptId> scripts;
    for (const auto &output : receipt.outputs) {
        if (!scripts.insert(output.target.script).second || !confined(receipt.root, output.target.relativePath))
            return Result<void>::failure(error("Publication receipt contains a duplicated script or unsafe output."));
        const auto &k = output.realization.key;
        if (k.project != receipt.project || k.revision != receipt.revision || k.script != output.target.script ||
            !k.realization.valid())
            return Result<void>::failure(error("Publication receipt has stale realization qualifications."));
        auto current = digest(receipt.root / output.target.relativePath);
        if (!current || !current.value() || *current.value() != output.digest)
            return Result<void>::failure(error("Published companion file is missing or has changed."));
    }
    return Result<void>::success();
}
SctPublicationSetResult SctPublicationSet::install(const SctPreparedPublicationSet &prepared, std::stop_token stop,
                                                   const WorkspaceTransactionHooks &hooks) {
    SctPublicationSetResult result;
    OutputLease lease(prepared.receipt_.root);
    if (!lease) {
        result.diagnostics.push_back(error("Another publication owns this output directory."));
        return result;
    }
    if (stop.stop_requested()) {
        result.cancelled = true;
        result.status = WorkspaceTransactionStatus::RolledBack;
        return result;
    }
    for (const auto &r : recover(prepared.receipt_.root))
        if (!r.succeeded()) {
            result.diagnostics = r.diagnostics;
            return result;
        }
    for (const auto &m : prepared.transaction_.mutations)
        if (!confined(prepared.receipt_.root, m.relativePath)) {
            result.diagnostics.push_back(error("Output confinement changed after preparation."));
            return result;
        }
    for (const auto &m : prepared.transaction_.mutations) {
        std::error_code ec;
        fs::create_directories((prepared.receipt_.root / m.relativePath).parent_path(), ec);
        if (ec) {
            result.diagnostics.push_back(error("Cannot create confined output directories: " + ec.message()));
            return result;
        }
    }
    auto staged = WorkspaceTransactionService::prepare(prepared.receipt_.root, journal, prepared.transaction_);
    if (staged.status != WorkspaceTransactionStatus::Prepared) {
        result.status = staged.status;
        result.diagnostics = staged.diagnostics;
        return result;
    }
    WorkspaceTransactionHooks combined{[&](size_t n) {
        return !stop.stop_requested() && (!hooks.continueAfterMutation || hooks.continueAfterMutation(n));
    }};
    auto installed = WorkspaceTransactionService::commitPrepared(prepared.receipt_.root, journal,
                                                                 prepared.transaction_.transactionId, combined);
    result.status = installed.status;
    result.diagnostics = installed.diagnostics;
    result.cancelled = stop.stop_requested();
    if (result.cancelled ||
        (installed.status != WorkspaceTransactionStatus::Verified && !hooks.continueAfterMutation)) {
        auto rollback = WorkspaceTransactionService::rollbackPrepared(prepared.receipt_.root, journal,
                                                                      prepared.transaction_.transactionId);
        result.status = rollback.status;
        result.diagnostics.insert(result.diagnostics.end(), rollback.diagnostics.begin(), rollback.diagnostics.end());
    }
    if (installed.status == WorkspaceTransactionStatus::Verified && !result.cancelled) {
        auto checked = verifyReceipt(prepared.receipt_);
        if (checked)
            result.receipt = prepared.receipt_;
        else {
            result.status = WorkspaceTransactionStatus::RecoveryBlocked;
            result.diagnostics = checked.diagnostics();
        }
    }
    return result;
}
} // namespace salsa::core
