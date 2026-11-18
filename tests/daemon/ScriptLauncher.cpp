// Windows launcher for script fixtures: `<name>.exe` runs `<name>.exe.py` with the Python
// interpreter given at build time (KETPLUS_SCRIPT_INTERPRETER), passing its own arguments and
// stdio through. The interpreter lives in a job object that dies with the launcher, so a
// provider the daemon kills does not leave the script behind.
#include <windows.h>

#include <string>

namespace {

// The command line without its first token (the program), as the caller wrote it.
std::wstring argumentsOf(const std::wstring& commandLine) {
    size_t index = 0;
    if (index < commandLine.size() && commandLine[index] == L'"') {
        index = commandLine.find(L'"', index + 1);
        index = index == std::wstring::npos ? commandLine.size() : index + 1;
    } else {
        index = commandLine.find_first_of(L" \t");
        if (index == std::wstring::npos)
            index = commandLine.size();
    }
    while (index < commandLine.size() && (commandLine[index] == L' ' || commandLine[index] == L'\t'))
        ++index;
    return commandLine.substr(index);
}

} // namespace

int wmain() {
    wchar_t self[32768];
    const DWORD length = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    if (length == 0 || length >= std::size(self))
        return 126;
    const std::wstring script = std::wstring(self, length) + L".py";
    const std::wstring interpreter = L"" KETPLUS_SCRIPT_INTERPRETER;

    std::wstring commandLine = L"\"" + interpreter + L"\" \"" + script + L"\"";
    if (const std::wstring rest = argumentsOf(GetCommandLineW()); !rest.empty())
        commandLine += L" " + rest;

    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &child))
        return 127;
    if (job)
        AssignProcessToJobObject(job, child.hProcess);
    ResumeThread(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(child.hProcess, &exitCode);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return int(exitCode);
}
