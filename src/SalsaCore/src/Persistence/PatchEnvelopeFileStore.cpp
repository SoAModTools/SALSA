#include "SalsaCore/Persistence/PatchEnvelopeFileStore.h"

#include "SalsaCore/Persistence/AtomicFile.h"
#include "SalsaCore/Persistence/PatchEnvelopeCodec.h"

#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace salsa::core {
namespace {

[[nodiscard]] Diagnostic readError(
    std::string message,
    const std::filesystem::path& path) {
    return {
        DiagnosticSeverity::Error,
        DiagnosticCode::PersistenceReadFailed,
        std::move(message),
        path,
    };
}

[[nodiscard]] std::vector<Diagnostic> withPath(
    const std::vector<Diagnostic>& diagnostics,
    const std::filesystem::path& path) {
    auto contextual = diagnostics;
    for (auto& diagnostic : contextual) {
        if (!diagnostic.path.has_value()) {
            diagnostic.path = path;
        }
    }
    return contextual;
}

[[nodiscard]] Result<std::string> readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return Result<std::string>::failure(readError(
            "The patch-envelope file could not be opened for reading.",
            path));
    }

    const auto end = input.tellg();
    if (end < 0 ||
        static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        static_cast<std::uintmax_t>(end) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return Result<std::string>::failure(readError(
            "The patch-envelope file size cannot be represented by this process.",
            path));
    }

    std::string contents(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!contents.empty() &&
        !input.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
        return Result<std::string>::failure(readError(
            "The patch-envelope file could not be read completely.",
            path));
    }
    return Result<std::string>::success(std::move(contents));
}

}  // namespace

Result<std::optional<PatchEnvelope>> PatchEnvelopeFileStore::load(
    const std::filesystem::path& path) const {
    if (path.empty()) {
        return Result<std::optional<PatchEnvelope>>::failure(readError(
            "The patch-envelope path must identify a file.",
            path));
    }

    std::error_code existenceError{};
    const bool exists = std::filesystem::exists(path, existenceError);
    if (existenceError) {
        return Result<std::optional<PatchEnvelope>>::failure(readError(
            "The patch-envelope path could not be inspected: " + existenceError.message(),
            path));
    }
    if (!exists) {
        return Result<std::optional<PatchEnvelope>>::success(std::nullopt);
    }

    auto contents = readTextFile(path);
    if (!contents) {
        return Result<std::optional<PatchEnvelope>>::failure(contents.diagnostics());
    }
    auto decoded = PatchEnvelopeCodec::deserialize(contents.value());
    if (!decoded) {
        return Result<std::optional<PatchEnvelope>>::failure(
            withPath(decoded.diagnostics(), path));
    }
    return Result<std::optional<PatchEnvelope>>::success(
        std::optional<PatchEnvelope>{ std::move(decoded).takeValue() });
}

Result<void> PatchEnvelopeFileStore::checkpoint(
    const std::filesystem::path& path,
    const PatchEnvelope& envelope) const {
    auto serialized = PatchEnvelopeCodec::serialize(envelope);
    if (!serialized) {
        return Result<void>::failure(withPath(serialized.diagnostics(), path));
    }

    const auto text = std::span<const char>(
        serialized.value().data(),
        serialized.value().size());
    return replaceFileAtomically(path, std::as_bytes(text));
}

}  // namespace salsa::core
