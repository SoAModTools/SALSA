#include "SalsaCore/Persistence/AtomicFile.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace salsa::core {
namespace {

std::atomic<std::uint64_t> temporaryFileSequence = 0;

[[nodiscard]] Diagnostic persistenceError(
    const DiagnosticCode code,
    std::string message,
    const std::filesystem::path& path,
    const DWORD windowsError = ERROR_SUCCESS) {
    if (windowsError != ERROR_SUCCESS) {
        message += ": ";
        message += std::system_category().message(static_cast<int>(windowsError));
    }
    return { DiagnosticSeverity::Error, code, std::move(message), path };
}

class UniqueHandle final {
public:
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    ~UniqueHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool close() noexcept {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return true;
        }
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return CloseHandle(handle) != FALSE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] std::filesystem::path temporaryPathFor(
    const std::filesystem::path& destination,
    const std::uint64_t sequence) {
    auto name = destination.filename().wstring();
    name += L".tmp-";
    name += std::to_wstring(GetCurrentProcessId());
    name += L"-";
    name += std::to_wstring(sequence);
    return destination.parent_path() / name;
}

}  // namespace

Result<void> replaceFileAtomically(
    const std::filesystem::path& destination,
    const std::span<const std::byte> contents) {
    if (destination.empty() || destination.filename().empty()) {
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceWriteFailed,
            "The checkpoint destination must identify a file.",
            destination));
    }

    std::error_code pathError{};
    const auto absoluteDestination = std::filesystem::absolute(destination, pathError);
    if (pathError) {
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceWriteFailed,
            "The checkpoint destination could not be resolved: " + pathError.message(),
            destination));
    }

    const auto parent = absoluteDestination.parent_path();
    const bool parentExists = std::filesystem::exists(parent, pathError);
    if (pathError || !parentExists || !std::filesystem::is_directory(parent, pathError) || pathError) {
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceWriteFailed,
            "The checkpoint destination's parent directory does not exist or is not accessible.",
            destination));
    }

    std::filesystem::path temporaryPath{};
    HANDLE rawHandle = INVALID_HANDLE_VALUE;
    DWORD createError = ERROR_FILE_EXISTS;
    for (std::size_t attempt = 0; attempt < 32 && rawHandle == INVALID_HANDLE_VALUE; ++attempt) {
        temporaryPath = temporaryPathFor(
            absoluteDestination,
            temporaryFileSequence.fetch_add(1, std::memory_order_relaxed));
        rawHandle = CreateFileW(
            temporaryPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (rawHandle == INVALID_HANDLE_VALUE) {
            createError = GetLastError();
            if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) {
                break;
            }
        }
    }
    if (rawHandle == INVALID_HANDLE_VALUE) {
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceWriteFailed,
            "A temporary checkpoint file could not be created",
            destination,
            createError));
    }

    UniqueHandle handle(rawHandle);
    const auto removeTemporary = [&temporaryPath]() noexcept {
        if (!temporaryPath.empty()) {
            DeleteFileW(temporaryPath.c_str());
        }
    };

    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto remaining = contents.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        const BOOL writeSucceeded = WriteFile(
                handle.get(),
                contents.data() + offset,
                requested,
                &written,
                nullptr);
        if (writeSucceeded == FALSE ||
            written != requested) {
            const DWORD writeError = writeSucceeded == FALSE
                ? GetLastError()
                : ERROR_WRITE_FAULT;
            (void)handle.close();
            removeTemporary();
            return Result<void>::failure(persistenceError(
                DiagnosticCode::PersistenceWriteFailed,
                "The temporary checkpoint file could not be written completely",
                destination,
                writeError));
        }
        offset += written;
    }

    if (FlushFileBuffers(handle.get()) == FALSE) {
        const DWORD flushError = GetLastError();
        (void)handle.close();
        removeTemporary();
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceWriteFailed,
            "The temporary checkpoint file could not be flushed",
            destination,
            flushError));
    }
    if (!handle.close()) {
        const DWORD closeError = GetLastError();
        removeTemporary();
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceWriteFailed,
            "The temporary checkpoint file could not be closed",
            destination,
            closeError));
    }

    if (MoveFileExW(
            temporaryPath.c_str(),
            absoluteDestination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD replaceError = GetLastError();
        removeTemporary();
        return Result<void>::failure(persistenceError(
            DiagnosticCode::PersistenceReplaceFailed,
            "The previous checkpoint could not be replaced atomically",
            destination,
            replaceError));
    }

    return Result<void>::success();
}

}  // namespace salsa::core
