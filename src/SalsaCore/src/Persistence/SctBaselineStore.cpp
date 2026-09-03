#include "SalsaCore/Persistence/SctBaselineStore.h"

#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"

#include <fstream>
#include <limits>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic baselineError(std::string message,
    const std::filesystem::path& path,
    const DiagnosticCode code = DiagnosticCode::SctBaselineCorrupt) {
    return {DiagnosticSeverity::Error, code, std::move(message), path};
}

[[nodiscard]] Result<std::vector<std::byte>> readBytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Result<std::vector<std::byte>>::failure(baselineError(
        "The retained SCT baseline could not be opened.", path,
        DiagnosticCode::PersistenceReadFailed));
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end)
            > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        return Result<std::vector<std::byte>>::failure(baselineError(
            "The retained SCT baseline is too large to read.", path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))
        return Result<std::vector<std::byte>>::failure(baselineError(
            "The retained SCT baseline could not be read completely.", path));
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<void> verify(const SourceRevision& revision,
    const std::span<const std::byte> bytes, const std::filesystem::path& path) {
    auto digest = sha256(bytes);
    if (!digest) return Result<void>::failure(digest.diagnostics());
    if (digest.value() != revision.digest)
        return Result<void>::failure(baselineError(
            "The retained SCT baseline does not match its source revision.", path));
    return Result<void>::success();
}

}  // namespace

DirectorySctBaselineStore::DirectorySctBaselineStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::filesystem::path DirectorySctBaselineStore::path(
    const SourceRevision& revision) const {
    return root_ / (revision.digest.toHex() + ".sct-source");
}

Result<std::optional<std::vector<std::byte>>> DirectorySctBaselineStore::loadBaseline(
    const SourceRevision& revision) const {
    const auto sourcePath = path(revision);
    std::error_code error;
    const bool exists = std::filesystem::exists(sourcePath, error);
    if (error) return Result<std::optional<std::vector<std::byte>>>::failure(
        baselineError("The retained SCT baseline could not be inspected: "
            + error.message(), sourcePath, DiagnosticCode::PersistenceReadFailed));
    if (!exists) return Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
    auto bytes = readBytes(sourcePath);
    if (!bytes) return Result<std::optional<std::vector<std::byte>>>::failure(
        bytes.diagnostics());
    auto verified = verify(revision, bytes.value(), sourcePath);
    if (!verified) return Result<std::optional<std::vector<std::byte>>>::failure(
        verified.diagnostics());
    return Result<std::optional<std::vector<std::byte>>>::success(
        std::optional<std::vector<std::byte>>{std::move(bytes).takeValue()});
}

Result<void> DirectorySctBaselineStore::retainBaseline(
    const SourceRevision& revision, const std::span<const std::byte> bytes) const {
    const auto sourcePath = path(revision);
    auto valid = verify(revision, bytes, sourcePath);
    if (!valid) return valid;
    auto existing = loadBaseline(revision);
    if (!existing) return Result<void>::failure(existing.diagnostics());
    if (existing.value()) return Result<void>::success();
    return replaceFileAtomically(sourcePath, bytes);
}

}  // namespace salsa::core
