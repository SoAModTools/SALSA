#include "Worker.h"
#include <Windows.h>

#include <Psapi.h>
#include <stdexcept>
namespace salsa::survey {
namespace fs = std::filesystem;
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle &) = delete;
};
std::wstring quote(const std::wstring &s) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
} // namespace
ChildResult runIsolated(const fs::path &exe, const std::vector<std::wstring> &arguments,
                        std::chrono::milliseconds timeout, const std::atomic_bool &stopped) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto command = quote(exe.wstring());
    for (const auto &argument : arguments)
        command += L" " + quote(argument);
    Handle group(CreateJobObjectW(nullptr, nullptr));
    if (!group.value)
        throw std::runtime_error("Cannot create worker job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(group.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Cannot configure worker job");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &process))
        throw std::runtime_error("Cannot launch worker");
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(group.value, child.value)) {
        TerminateProcess(child.value, 2);
        throw std::runtime_error("Cannot isolate worker");
    }
    if (ResumeThread(thread.value) == DWORD(-1))
        throw std::runtime_error("Cannot start worker");
    std::string status;
    while (true) {
        auto wait = WaitForSingleObject(child.value, 100);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait != WAIT_TIMEOUT)
            throw std::runtime_error("Cannot wait for worker");
        if (stopped) {
            status = "cancelled";
            break;
        }
        if (Clock::now() - start > timeout) {
            status = "timeout";
            break;
        }
    }
    if (!status.empty()) {
        TerminateJobObject(group.value, 130);
        WaitForSingleObject(child.value, 5000);
    }
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(child.value, &memory, sizeof(memory));
    DWORD code = 2;
    GetExitCodeProcess(child.value, &code);
    return {code, status, static_cast<std::uint64_t>(memory.PeakWorkingSetSize),
            std::chrono::duration<double, std::milli>(Clock::now() - start).count()};
}
} // namespace salsa::survey
