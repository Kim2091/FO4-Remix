// FO4RemixWatchdog.exe -- external crash observer (2026-07-12, v3).
//
// v1 (PowerShell) proved the concept; v2 (GetExitCodeProcess) recovered the
// first real exit code: 0x42B / ERROR_PROCESS_ABORTED -- a Win32 code, not
// an NTSTATUS, delivered with every in-process instrument silent. That
// pattern suggests an EXTERNAL TerminateProcess (which executes zero code in
// the victim). v3 attaches as a DEBUGGER to settle it:
//   - every second-chance exception (and first-chance fatal classes) is
//     logged with module+offset and captured in a minidump written from
//     THIS process -- no dying-process constraints;
//   - if the process exits with no prior fatal exception, the kill was
//     external, definitively.
// The routine first-chance AVs of the plugin's guarded stale-pointer reads
// are passed through unlogged (DBG_EXCEPTION_NOT_HANDLED lets the game's own
// SEH handle them exactly as without a debugger).
//
// Output: %LOCALAPPDATA%\CrashDumps\FO4Remix_exitcodes.log (append) and
// FO4Remix_extdump_<pid>.dmp on the first fatal exception.

#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

static FILE* g_log = nullptr;

// Nothing else guarantees %LOCALAPPDATA%\CrashDumps exists (WER only creates
// it when LocalDumps has fired): on a fresh install every fopen_s/CreateFileA
// below failed silently and the watchdog attached, observed, and recorded
// nothing. Idempotent.
static void EnsureDumpDir(const char* localAppData) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\CrashDumps", localAppData);
    CreateDirectoryA(path, nullptr);
}

static void LogLine(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "%04u-%02u-%02u %02u:%02u:%02u ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

// Resolve an address in the TARGET process to module+offset.
static void FormatRemoteAddr(HANDLE proc, void* addr, char* out, size_t outSize) {
    HMODULE mods[1024];
    DWORD needed = 0;
    const char* bestName = "?";
    uintptr_t bestBase = 0;
    static char nameBuf[MAX_PATH];
    if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i) {
            MODULEINFO mi = {};
            if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
            const uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
            if ((uintptr_t)addr >= base &&
                (uintptr_t)addr < base + mi.SizeOfImage) {
                if (GetModuleBaseNameA(proc, mods[i], nameBuf, sizeof(nameBuf))) {
                    bestName = nameBuf;
                }
                bestBase = base;
                break;
            }
        }
    }
    if (bestBase) {
        sprintf_s(out, outSize, "%s+0x%llX", bestName,
                  (unsigned long long)((uintptr_t)addr - bestBase));
    } else {
        sprintf_s(out, outSize, "unmapped:%p", addr);
    }
}

static void WriteExternalDump(HANDLE proc, DWORD pid, DWORD tid,
                              EXCEPTION_RECORD* rec) {
    char dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) return;
    EnsureDumpDir(dir);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\CrashDumps\\FO4Remix_extdump_%lu.dmp", dir, pid);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    // Rebuild EXCEPTION_POINTERS in OUR address space: the record came from
    // the debug event; the context is fetched from the stopped thread.
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_ALL;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, tid);
    BOOL haveCtx = thread && GetThreadContext(thread, &ctx);

    EXCEPTION_POINTERS xp = {};
    xp.ExceptionRecord = rec;
    xp.ContextRecord   = haveCtx ? &ctx : nullptr;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId          = tid;
    mei.ExceptionPointers = &xp;
    mei.ClientPointers    = FALSE;

    const BOOL ok = MiniDumpWriteDump(
        proc, pid, file,
        (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                        MiniDumpWithIndirectlyReferencedMemory |
                        MiniDumpWithUnloadedModules),
        rec ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (thread) CloseHandle(thread);
    LogLine("pid=%lu external dump %s: %s", pid, ok ? "written" : "FAILED", path);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    // On-demand hang capture: "FO4RemixWatchdog.exe dump <pid>" writes an
    // immediate all-thread-stacks dump of a LIVE process and exits. Built
    // for the deadlock hunt (2026-07-12): a hung process is a perfect crime
    // scene -- every thread parked on exactly the lock it waits for -- and
    // this needs no debugger attach, so it coexists with the watchdog
    // instance already attached to the same PID.
    if (cmdLine && strncmp(cmdLine, "dump", 4) == 0) {
        const DWORD dumpPid = strtoul(cmdLine + 4, nullptr, 10);
        if (!dumpPid) return 1;
        HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                  FALSE, dumpPid);
        if (!proc) return 2;
        char dir[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) return 3;
        EnsureDumpDir(dir);
        char path[MAX_PATH];
        sprintf_s(path, "%s\\CrashDumps\\FO4Remix_hang_%lu.dmp", dir, dumpPid);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { CloseHandle(proc); return 4; }
        const BOOL ok = MiniDumpWriteDump(
            proc, dumpPid, file,
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                            MiniDumpWithIndirectlyReferencedMemory |
                            MiniDumpWithUnloadedModules),
            nullptr, nullptr, nullptr);
        CloseHandle(file);
        CloseHandle(proc);
        return ok ? 0 : 5;
    }

    const DWORD pid = strtoul(cmdLine ? cmdLine : "", nullptr, 10);
    if (!pid) return 1;

    char dir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) {
        EnsureDumpDir(dir);
        char path[MAX_PATH];
        sprintf_s(path, "%s\\CrashDumps\\FO4Remix_exitcodes.log", dir);
        fopen_s(&g_log, path, "a");
    }

    if (!DebugActiveProcess(pid)) {
        // Another debugger is attached (or access denied): fall back to the
        // v2 wait-and-report behavior rather than observing nothing.
        LogLine("pid=%lu debugger attach FAILED (err=%lu); passive wait", pid,
                GetLastError());
        HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                  FALSE, pid);
        if (proc) {
            WaitForSingleObject(proc, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(proc, &code);
            LogLine("pid=%lu exit=0x%08lX (passive)", pid, code);
            CloseHandle(proc);
        }
        if (g_log) fclose(g_log);
        return 0;
    }
    // If this watchdog dies, do NOT take the game down with it.
    DebugSetProcessKillOnExit(FALSE);
    LogLine("pid=%lu debugger attached", pid);

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    bool fatalSeen = false;
    bool dumpWritten = false;

    for (;;) {
        DEBUG_EVENT ev = {};
        if (!WaitForDebugEvent(&ev, INFINITE)) break;

        DWORD continueStatus = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD& rec = ev.u.Exception.ExceptionRecord;
            const DWORD code = rec.ExceptionCode;
            const bool firstChance = ev.u.Exception.dwFirstChance != 0;
            // Debugger-protocol pseudo-exceptions MUST be swallowed with
            // DBG_CONTINUE the way real debuggers do -- the raisers rely on
            // an attached debugger consuming them. Passing them through as
            // unhandled killed the game 3s after attach on the first field
            // run (exit 0x406D1388: a thread-naming exception raised for
            // OUR benefit).
            if (code == 0x406D1388UL ||   // MS_VC_EXCEPTION (SetThreadName)
                code == 0x40010006UL ||   // DBG_PRINTEXCEPTION_C
                code == 0x40010007UL) {   // DBG_PRINTEXCEPTION_WIDE_C
                continueStatus = DBG_CONTINUE;
                break;
            }
            // Breakpoints: only the attach-handshake INT3 (the FIRST one)
            // is protocol noise. Swallowing every later breakpoint/single-
            // step forever consumed the plugin's own __debugbreak() asserts
            // -- execution silently continued PAST failed asserts -- and
            // bypassed any game/DRM SEH that raises-and-handles int3 (its
            // handler never ran while the watchdog was attached).
            // DBG_EXCEPTION_NOT_HANDLED hands them to in-process SEH exactly
            // as without a debugger; an unhandled one comes back second-
            // chance and the fatal path below logs and dumps it.
            if (code == 0x80000003UL ||   // STATUS_BREAKPOINT
                code == 0x4000001FUL ||   // STATUS_WX86_BREAKPOINT
                code == 0x80000004UL) {   // STATUS_SINGLE_STEP
                static bool s_attachBreakSeen = false;
                if (!s_attachBreakSeen && firstChance &&
                    code != 0x80000004UL) {
                    s_attachBreakSeen = true;   // the attach INT3
                    continueStatus = DBG_CONTINUE;
                    break;
                }
                static int s_bpLogs = 0;
                if (s_bpLogs < 8) {
                    ++s_bpLogs;
                    char site[MAX_PATH + 32];
                    FormatRemoteAddr(proc, rec.ExceptionAddress, site,
                                     sizeof(site));
                    LogLine("pid=%lu post-attach %s-chance code=0x%08lX at %s "
                            "tid=%lu (passed to in-process SEH)",
                            pid, firstChance ? "first" : "SECOND", code, site,
                            (unsigned long)ev.dwThreadId);
                }
                // Falls through to the ordinary NOT_HANDLED path below.
            }
            // Pass ordinary exceptions to the game's own handlers (the
            // plugin's guarded stale-pointer AVs arrive here first-chance
            // constantly; they are handled in-process by design).
            continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            const bool fatalClass =
                code == 0xC00000FDUL ||   // stack overflow
                code == 0xC0000374UL ||   // heap corruption
                code == 0xC0000409UL;     // fastfail (debugger-visible)
            if (!firstChance || fatalClass) {
                char site[MAX_PATH + 32];
                FormatRemoteAddr(proc, rec.ExceptionAddress, site, sizeof(site));
                LogLine("pid=%lu %s-chance exception code=0x%08lX at %s tid=%lu",
                        pid, firstChance ? "first" : "SECOND", code, site,
                        (unsigned long)ev.dwThreadId);
                fatalSeen = true;
                if (!dumpWritten && proc) {
                    dumpWritten = true;
                    WriteExternalDump(proc, pid, ev.dwThreadId,
                                      const_cast<EXCEPTION_RECORD*>(&rec));
                }
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            LogLine("pid=%lu exit=0x%08lX (%s prior fatal exception)",
                    pid, ev.u.ExitProcess.dwExitCode,
                    fatalSeen ? "WITH" : "NO");
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            if (proc) CloseHandle(proc);
            if (g_log) fclose(g_log);
            return 0;
        // The debugger owns these file handles; attaching to a modded FO4
        // delivers one LOAD_DLL per already-loaded module (hundreds), so
        // not closing them leaked handles for the whole session.
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hFile) {
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) {
                CloseHandle(ev.u.LoadDll.hFile);
            }
            break;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, continueStatus);
    }

    if (proc) CloseHandle(proc);
    if (g_log) fclose(g_log);
    return 0;
}
