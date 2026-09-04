#include "SalsaCore/Legacy/LegacyConversionService.h"

#include "SalsaCore/Application/ApplicationInfo.h"
#include "SalsaCore/Foundation/Hashing.h"
#include "SalsaCore/Persistence/AtomicFile.h"

#include <Windows.h>
#include <aclapi.h>
#include <appmodel.h>
#include <nlohmann/json.hpp>
#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <sstream>
#include <thread>
#include <utility>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace salsa::core {
namespace {

using Json = nlohmann::ordered_json;

struct LocalHandle final {
    HANDLE value = nullptr;
    ~LocalHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    LocalHandle() = default;
    explicit LocalHandle(HANDLE handle) : value(handle) {}
    LocalHandle(const LocalHandle&) = delete;
    LocalHandle& operator=(const LocalHandle&) = delete;
    LocalHandle(LocalHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    LocalHandle& operator=(LocalHandle&& other) noexcept {
        if (this != &other) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, nullptr); }
        return *this;
    }
};

struct LocalSid final {
    PSID value = nullptr;
    ~LocalSid() { if (value) FreeSid(value); }
};

[[nodiscard]] Diagnostic legacyError(std::string message,
    const std::filesystem::path& path, const DiagnosticCode code) {
    return {DiagnosticSeverity::Error, code, std::move(message), path};
}

[[nodiscard]] LegacyConversionResult failure(const LegacyConversionStatus status,
    std::string message, const std::filesystem::path& path, const DiagnosticCode code) {
    LegacyConversionResult result{}; result.status = status;
    result.diagnostics.push_back(legacyError(std::move(message), path, code));
    return result;
}

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::wstring quote(const std::filesystem::path& path) {
    auto value = path.wstring(); std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') { result.append(slashes * 2u + 1u, L'\\'); result.push_back(character); slashes = 0; continue; }
        result.append(slashes, L'\\'); slashes = 0; result.push_back(character);
    }
    result.append(slashes * 2u, L'\\'); result.push_back(L'\"'); return result;
}

[[nodiscard]] bool regularNoReparse(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

[[nodiscard]] std::filesystem::path uniqueStage(const std::filesystem::path& destination) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.parent_path() / (destination.filename().wstring() + L".staging-"
        + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(now));
}

[[nodiscard]] bool grantAccess(const std::filesystem::path& path, PSID sid,
    const DWORD permissions, const DWORD inheritance) {
    PACL oldAcl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto read = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl, nullptr, &descriptor);
    if (read != ERROR_SUCCESS) return false;
    EXPLICIT_ACCESSW access{}; access.grfAccessPermissions = permissions;
    access.grfAccessMode = GRANT_ACCESS; access.grfInheritance = inheritance;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL updated = nullptr;
    const auto merged = SetEntriesInAclW(1, &access, oldAcl, &updated);
    const auto written = merged == ERROR_SUCCESS
        ? SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr)
        : merged;
    if (updated) LocalFree(updated); if (descriptor) LocalFree(descriptor);
    return written == ERROR_SUCCESS;
}

[[nodiscard]] bool appContainerSid(LocalSid& sid, HRESULT& result) {
    constexpr auto name = L"jahorta.salsa.legacy-converter";
    result = CreateAppContainerProfile(name, L"SALSA Legacy Converter",
        L"Isolated native reader for trusted legacy SALSA projects", nullptr, 0, &sid.value);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        result = DeriveAppContainerSidFromAppContainerName(name, &sid.value);
    return SUCCEEDED(result) && sid.value != nullptr;
}

[[nodiscard]] std::optional<LegacyConversionPhase> phase(const std::string_view value) {
    if (value == "hash") return LegacyConversionPhase::Hashing;
    if (value == "parse") return LegacyConversionPhase::Parsing;
    if (value == "script") return LegacyConversionPhase::Script;
    if (value == "write") return LegacyConversionPhase::Writing;
    if (value == "finalize") return LegacyConversionPhase::Finalizing;
    return std::nullopt;
}

void processLine(const std::string& line, const LegacyConversionObserver& observer,
    std::optional<Json>& final) {
    try {
        const auto value = Json::parse(line);
        if (!value.is_object() || value.value("protocol", "") != "jahorta.salsa.legacy-converter-events"
            || value.value("version", 0) != 1 || !value.contains("type")) return;
        if (value.at("type") == "result") { final = value; return; }
        if (value.at("type") != "progress" || !observer) return;
        const auto parsedPhase = phase(value.value("phase", ""));
        if (!parsedPhase) return;
        observer({*parsedPhase, value.value("completed", 0ull), value.value("total", 0ull),
            value.value("current", "")});
    } catch (const std::exception&) {}
}

[[nodiscard]] bool writeReceipt(const LegacyConversionRequest& request,
    const LegacyConversionResult& result, const Json& helperResult) {
    if (!request.receiptDirectory || !result.capsule) return true;
    Json receipt{{"formatId", "jahorta.salsa.legacy-conversion-receipt"}, {"schemaVersion", 1},
        {"applicationVersion", applicationVersion()}, {"capsuleId", result.capsule->capsuleId},
        {"sourcePath", utf8(request.source)}, {"destinationPath", utf8(request.destination)},
        {"retainOriginal", request.retainOriginal}, {"resourceLimitsDisabled", request.disableResourceLimits},
        {"helperResult", helperResult}};
    auto text = receipt.dump(2); text.push_back('\n');
    const auto path = *request.receiptDirectory / (result.capsule->capsuleId + ".json");
    std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    return static_cast<bool>(replaceFileAtomically(path,
        std::as_bytes(std::span{text.data(), text.size()})));
}

}  // namespace

LegacyConversionResult LegacyConversionService::convert(
    const LegacyConversionRequest& request, const std::stop_token stop,
    const LegacyConversionObserver& observer) {
    if (!request.trustedInputConfirmed) return failure(LegacyConversionStatus::Rejected,
        "Confirm that this legacy project comes from a trusted source before conversion.",
        request.source, DiagnosticCode::LegacyConversionNotTrusted);
    if (!regularNoReparse(request.source) || !regularNoReparse(request.converterExecutable)
        || request.destination.empty() || request.destination.parent_path().empty())
        return failure(LegacyConversionStatus::Rejected,
            "The source, destination, or bundled converter path is invalid.", request.source,
            DiagnosticCode::LegacyConversionInvalidRequest);
    std::error_code error;
    if (std::filesystem::exists(request.destination, error) || error)
        return failure(LegacyConversionStatus::Rejected,
            "The capsule destination already exists.", request.destination,
            DiagnosticCode::LegacyConversionInvalidRequest);
    if (observer) observer({LegacyConversionPhase::Staging, 0, 3, ""});
    const auto stage = uniqueStage(request.destination);
    const auto runtime = stage / L"runtime";
    const auto input = stage / L"input";
    const auto output = stage / L"output";
    const auto stagedConverter = runtime / L"SalsaLegacyConverter.exe";
    const auto stagedInput = input / L"source.prj";
    const auto stagedCapsule = output / L"capsule";
    if (!std::filesystem::create_directories(runtime, error) || error
        || !std::filesystem::create_directories(input, error) || error
        || !std::filesystem::create_directories(output, error) || error) {
        std::filesystem::remove_all(stage, error);
        return failure(LegacyConversionStatus::Failed,
            "Controlled conversion staging could not be created.", stage,
            DiagnosticCode::LegacyConversionFailed);
    }
    const auto cleanup = [&] { std::error_code ignored; std::filesystem::remove_all(stage, ignored); };
    if (!CopyFileW(request.converterExecutable.c_str(), stagedConverter.c_str(), TRUE)) {
        cleanup(); return failure(LegacyConversionStatus::Failed,
            "The bundled converter could not be staged.", stagedConverter,
            DiagnosticCode::LegacyConversionFailed);
    }
    if (observer) observer({LegacyConversionPhase::Staging, 1, 3, ""});
    WIN32_FILE_ATTRIBUTE_DATA before{}, after{};
    if (!GetFileAttributesExW(request.source.c_str(), GetFileExInfoStandard, &before)
        || !CopyFileW(request.source.c_str(), stagedInput.c_str(), TRUE)
        || !GetFileAttributesExW(request.source.c_str(), GetFileExInfoStandard, &after)
        || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow
        || before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime
        || before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime) {
        cleanup(); return failure(LegacyConversionStatus::Rejected,
            "The selected project changed while its controlled copy was being staged.", request.source,
            DiagnosticCode::LegacyConversionInvalidRequest);
    }
    if (observer) observer({LegacyConversionPhase::Staging, 2, 3, ""});
    LocalSid sid;
    const auto isolationFailure = [&](const std::string_view boundary) {
        cleanup(); return failure(LegacyConversionStatus::Failed,
            "The no-capability AppContainer " + std::string(boundary)
                + " boundary could not be established.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    };
    HRESULT profileResult = S_OK;
    if (!appContainerSid(sid, profileResult)) return isolationFailure(
        "profile (HRESULT " + std::to_string(static_cast<std::uint32_t>(profileResult)) + ")");
    if (!grantAccess(stage, sid.value, FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
            CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) return isolationFailure("stage ACL");
    if (!grantAccess(runtime, sid.value, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
            CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) return isolationFailure("runtime ACL");
    if (!grantAccess(input, sid.value, FILE_GENERIC_READ,
            CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) return isolationFailure("input ACL");
    if (!grantAccess(output, sid.value, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE,
            CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) return isolationFailure("output ACL");

    SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    LocalHandle readPipe, writePipe;
    if (!CreatePipe(&readPipe.value, &writePipe.value, &pipeSecurity, 0)
        || !SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0)) {
        cleanup(); return failure(LegacyConversionStatus::Failed,
            "The isolated converter output pipe could not be created.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    }
    LocalHandle nullInput(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &pipeSecurity, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput.value || nullInput.value == INVALID_HANDLE_VALUE) {
        cleanup(); return failure(LegacyConversionStatus::Failed,
            "The isolated converter input handle could not be created.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) {
        cleanup(); return failure(LegacyConversionStatus::Failed,
            "The AppContainer process attributes could not be initialized.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    }
    SECURITY_CAPABILITIES capabilities{}; capabilities.AppContainerSid = sid.value;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
            &capabilities, sizeof(capabilities), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes); cleanup();
        return failure(LegacyConversionStatus::Failed,
            "The AppContainer process token could not be configured.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    }
    std::array inheritedHandles{writePipe.value, nullInput.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles.data(), inheritedHandles.size() * sizeof(HANDLE), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes); cleanup();
        return failure(LegacyConversionStatus::Failed,
            "The isolated converter handle boundary could not be configured.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdOutput = writePipe.value;
    startup.StartupInfo.hStdError = writePipe.value; startup.StartupInfo.hStdInput = nullInput.value;
    startup.lpAttributeList = attributes;
    auto command = quote(stagedConverter) + L" convert " + quote(stagedInput) + L" " + quote(stagedCapsule);
    if (request.retainOriginal) command += L" --retain-original";
    if (request.disableResourceLimits) command += L" --disable-resource-limits";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(L'\0');
    PROCESS_INFORMATION processInfo{};
    const auto created = CreateProcessW(stagedConverter.c_str(), mutableCommand.data(), nullptr, nullptr,
        TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
        stage.c_str(), &startup.StartupInfo, &processInfo);
    const auto createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) { cleanup(); return failure(LegacyConversionStatus::Failed,
        "The isolated native converter could not be started (Win32 error "
            + std::to_string(createError) + ").", stagedConverter,
        DiagnosticCode::LegacyConversionIsolationFailed); }
    LocalHandle process(processInfo.hProcess), thread(processInfo.hThread);
    writePipe = LocalHandle{};
    LocalHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    jobLimits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!request.disableResourceLimits) {
        jobLimits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        // The disk-spooled converter stays near 1 GiB on the 295 MiB US v7
        // fixture. Keep room for the larger characterized fixtures and record
        // decompression without restoring the former graph-sized allowance.
        jobLimits.JobMemoryLimit = 4ull << 30;
    }
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
            &jobLimits, sizeof(jobLimits)) || !AssignProcessToJobObject(job.value, process.value)
        || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 5); cleanup();
        return failure(LegacyConversionStatus::Failed,
            "The converter could not be assigned to its mandatory Job Object.", stage,
            DiagnosticCode::LegacyConversionIsolationFailed);
    }
    if (observer) observer({LegacyConversionPhase::Staging, 3, 3, ""});
    std::string pending; std::optional<Json> helperResult;
    for (;;) {
        if (stop.stop_requested()) {
            TerminateJobObject(job.value, 6); WaitForSingleObject(process.value, 5000); cleanup();
            return failure(LegacyConversionStatus::Cancelled, "Legacy conversion was cancelled.",
                request.source, DiagnosticCode::LegacyConversionCancelled);
        }
        DWORD available = 0;
        if (PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 8192> buffer{}; DWORD read = 0;
            if (ReadFile(readPipe.value, buffer.data(),
                    std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr)) {
                pending.append(buffer.data(), read);
                for (auto newline = pending.find('\n'); newline != std::string::npos;
                     newline = pending.find('\n')) {
                    processLine(pending.substr(0, newline), observer, helperResult);
                    pending.erase(0, newline + 1u);
                }
            }
        }
        if (WaitForSingleObject(process.value, 25) == WAIT_OBJECT_0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    DWORD exitCode = 5; GetExitCodeProcess(process.value, &exitCode);
    DWORD available = 0;
    while (PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) && available) {
        std::array<char, 8192> buffer{}; DWORD read = 0;
        if (!ReadFile(readPipe.value, buffer.data(), std::min<DWORD>(available,
                static_cast<DWORD>(buffer.size())), &read, nullptr)) break;
        pending.append(buffer.data(), read);
    }
    for (auto newline = pending.find('\n'); newline != std::string::npos;
         newline = pending.find('\n')) {
        processLine(pending.substr(0, newline), observer, helperResult);
        pending.erase(0, newline + 1u);
    }
    if (!pending.empty()) processLine(pending, observer, helperResult);
    if (exitCode != 0 || !helperResult) {
        const auto rejected = exitCode == 4;
        const auto message = helperResult ? helperResult->value("message", "Legacy conversion failed.")
                                          : "The native converter ended without a valid result.";
        const auto resourceLimited = message.find("resource limit") != std::string::npos;
        cleanup(); return failure(rejected ? LegacyConversionStatus::Rejected : LegacyConversionStatus::Failed,
            message, request.source, resourceLimited ? DiagnosticCode::LegacyConversionResourceLimit
                : rejected ? DiagnosticCode::LegacyConversionInvalidRequest
                           : DiagnosticCode::LegacyConversionFailed);
    }
    if (observer) observer({LegacyConversionPhase::Validating, 0, 1, ""});
    auto validated = LegacyCapsuleReader::validate(stagedCapsule,
        request.disableResourceLimits ? LegacyCapsuleValidationLimits{
            UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}
                                      : LegacyCapsuleValidationLimits{});
    if (!validated) { cleanup(); LegacyConversionResult result{}; result.status = LegacyConversionStatus::Failed;
        result.diagnostics = validated.diagnostics(); return result; }
    if (observer) observer({LegacyConversionPhase::Validating, 1, 1, ""});
    if (observer) observer({LegacyConversionPhase::Finalizing, 0, 1, ""});
    if (!MoveFileExW(stagedCapsule.c_str(), request.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        cleanup(); return failure(LegacyConversionStatus::Failed,
            "The validated capsule could not be installed atomically.", request.destination,
            DiagnosticCode::LegacyConversionFailed);
    }
    cleanup();
    auto finalSummary = std::move(validated).takeValue(); finalSummary.root = request.destination;
    LegacyConversionResult result{};
    result.status = finalSummary.status == LegacyCapsuleStatus::Ready
        ? LegacyConversionStatus::Ready : LegacyConversionStatus::ActionRequired;
    result.capsule = std::move(finalSummary);
    if (!writeReceipt(request, result, *helperResult))
        result.diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::PersistenceWriteFailed,
            "The machine-local conversion receipt could not be saved.", request.receiptDirectory});
    if (observer) observer({LegacyConversionPhase::Finalizing, 1, 1, ""});
    return result;
}

}  // namespace salsa::core
