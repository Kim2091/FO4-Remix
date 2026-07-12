// FO4RemixWatchdog.exe -- external exit-code observer (2026-07-12).
//
// Spawned by the plugin at load with the game's PID on the command line.
// Waits on the process from OUTSIDE and appends its NTSTATUS exit code to
// %LOCALAPPDATA%\CrashDumps\FO4Remix_exitcodes.log. Exists because the
// crash class under investigation dies without executing a single
// in-process instrument (no WER, no VEH, no exit hooks, no terminate) --
// but the kernel always reports an exit status to a waiting observer.
// The first PowerShell incarnation of this hit .NET's ExitCode quirk for
// processes it didn't start; GetExitCodeProcess has no such failure mode.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    const DWORD pid = strtoul(cmdLine ? cmdLine : "", nullptr, 10);
    if (!pid) return 1;

    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, pid);
    if (!proc) return 2;

    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);

    char dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) return 3;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\CrashDumps\\FO4Remix_exitcodes.log", dir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u pid=%lu exit=0x%08lX\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond, pid, code);
        fclose(f);
    }
    return 0;
}
