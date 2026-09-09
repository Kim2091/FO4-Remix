#include "crash_diag.h"
#include "remix_renderer.h"  // WriteDiagDump

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <intrin.h>  // _ReturnAddress

#include "f4se/PluginAPI.h"  // _MESSAGE

namespace CrashDiag {

// ---------------------------------------------------------------------------
// Caller identification for the exit hooks: resolve a return address to
// module + offset. Static buffers only -- this runs on dying-process paths.
// ---------------------------------------------------------------------------
static void FormatCaller(void* retAddr, char* out, size_t outSize) {
    char modName[MAX_PATH] = "?";
    uintptr_t off = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)retAddr, &mod) && mod) {
        GetModuleFileNameA(mod, modName, sizeof(modName));
        off = (uintptr_t)retAddr - (uintptr_t)mod;
    }
    // Basename only: full paths bloat the one log line we may get.
    const char* base = modName;
    for (const char* p = modName; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    sprintf_s(out, outSize, "%s+0x%llX", base, (unsigned long long)off);
}

// ---------------------------------------------------------------------------
// C++-throw backtrace stash (2026-07-14, the Present "invalid vector
// subscript" wedge). The RemixGuard catch blocks log what() but by then the
// throw stack is gone -- the unwinder tore it down reaching the catch. The
// VEH below runs FIRST-CHANCE, on the throwing thread, with the throw stack
// intact: stash the raw frames per-thread here, and let the catch site
// decide whether they're worth a log line (LogLastCxxThrow). Capture is a
// single RtlCaptureStackBackTrace -- cheap enough to run unconditionally on
// every 0xE06D7363, including historic SubmitDrawable throw storms.
// ---------------------------------------------------------------------------
struct CxxThrowStash {
    void*    frames[28];
    USHORT   count;
    unsigned seq;        // bumped per capture
    unsigned loggedSeq;  // last seq consumed by LogLastCxxThrow
};
static thread_local CxxThrowStash t_cxxThrow = {};

void LogLastCxxThrow(const char* site) {
    CxxThrowStash& st = t_cxxThrow;
    if (st.seq == 0 || st.seq == st.loggedSeq) return;
    st.loggedSeq = st.seq;

    // Present throws are the wedge signature we're hunting; everything else
    // shares a smaller budget so a Create/Draw throw storm can't eat the log.
    static std::atomic<int> sPresentLogged{0};
    static std::atomic<int> sOtherLogged{0};
    const bool isPresent = site && strcmp(site, "Present") == 0;
    std::atomic<int>& bucket = isPresent ? sPresentLogged : sOtherLogged;
    if (bucket.fetch_add(1, std::memory_order_relaxed) >= (isPresent ? 6 : 3)) {
        return;
    }

    // One frame per segment, module-basename+offset; two log lines max so a
    // deep stack doesn't overflow the formatter.
    char line[1600];
    int  pos = 0;
    pos += sprintf_s(line + pos, sizeof(line) - pos,
                     "FO4RemixPlugin: [CrashDiag] throw@%s backtrace:", site);
    const int n = (int)st.count;
    for (int i = 0; i < n && pos < (int)sizeof(line) - 96; ++i) {
        char frame[MAX_PATH + 32];
        FormatCaller(st.frames[i], frame, sizeof(frame));
        pos += sprintf_s(line + pos, sizeof(line) - pos, " %s;", frame);
    }
    _MESSAGE("%s", line);
}

// ---------------------------------------------------------------------------
// Vectored exception handler: first-chance, add-only (cannot be displaced by
// later SetUnhandledExceptionFilter calls). Logs ONLY the silent-killer
// codes; routine first-chance AVs (the resolver's guarded stale-pointer
// reads) pass through untouched.
// ---------------------------------------------------------------------------
static LONG CALLBACK VectoredBreadcrumb(EXCEPTION_POINTERS* xp) {
    const DWORD code = xp && xp->ExceptionRecord
        ? xp->ExceptionRecord->ExceptionCode : 0;
    if (code == 0xE06D7363UL /* MSVC C++ exception */) {
        // Stash only -- never log here. skip=0 keeps the dispatcher frames;
        // they anchor the read and cost nothing offline.
        t_cxxThrow.count = RtlCaptureStackBackTrace(
            0, ARRAYSIZE(t_cxxThrow.frames), t_cxxThrow.frames, nullptr);
        ++t_cxxThrow.seq;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0xC00000FDUL /* STATUS_STACK_OVERFLOW */ ||
        code == 0xC0000374UL /* STATUS_HEAP_CORRUPTION */) {
        static std::atomic<int> sOnce{0};
        if (sOnce.fetch_add(1, std::memory_order_relaxed) < 2) {
            char site[MAX_PATH + 32];
            FormatCaller(xp->ExceptionRecord->ExceptionAddress, site, sizeof(site));
            // One line, then get out of the way. On stack overflow even this
            // may not survive -- but any line beats total silence, and the
            // handler frame is tiny by design.
            _MESSAGE("FO4RemixPlugin: [CrashDiag] FATAL code=0x%08lX at %s thread=%lu",
                     code, site, GetCurrentThreadId());
            if (code == 0xC0000374UL) {
                // Heap corruption leaves enough stack for a dump; a stack
                // overflow does not (dbghelp would double-fault).
                RemixRenderer::WriteDiagDump("heap");
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// ExitProcess / TerminateProcess hooks: engine fatal handlers and libraries
// that "handle" errors by quitting leave no WER event and no exception at
// all. Log WHO is exiting (return-address module) and drop a dump so the
// stacks at exit time are recoverable. Behavior unchanged -- the original
// always runs.
// ---------------------------------------------------------------------------
typedef void (WINAPI* PFN_ExitProcess)(UINT);
typedef BOOL (WINAPI* PFN_TerminateProcess)(HANDLE, UINT);
static PFN_ExitProcess      g_origExitProcess = nullptr;
static PFN_TerminateProcess g_origTerminateProcess = nullptr;
static std::atomic<int>     g_exitDumpOnce{0};

static void LogExit(const char* api, UINT exitCode, void* retAddr) {
    char site[MAX_PATH + 32];
    FormatCaller(retAddr, site, sizeof(site));
    _MESSAGE("FO4RemixPlugin: [CrashDiag] %s(%u) called from %s thread=%lu",
             api, exitCode, site, GetCurrentThreadId());
    if (g_exitDumpOnce.fetch_add(1, std::memory_order_relaxed) == 0) {
        RemixRenderer::WriteDiagDump("exit");
    }
}

static void WINAPI hkExitProcess(UINT exitCode) {
    LogExit("ExitProcess", exitCode, _ReturnAddress());
    g_origExitProcess(exitCode);
}

static BOOL WINAPI hkTerminateProcess(HANDLE process, UINT exitCode) {
    if (process == GetCurrentProcess() ||
        GetProcessId(process) == GetCurrentProcessId()) {
        LogExit("TerminateProcess", exitCode, _ReturnAddress());
    }
    return g_origTerminateProcess(process, exitCode);
}

// ---------------------------------------------------------------------------
// ntdll-layer exit hooks: kernel32's ExitProcess forwards to
// RtlExitUserProcess, and NtTerminateProcess is the final stop for
// everything -- callers that go to ntdll directly (some engine fatal
// handlers, CRT fastpath exits) never touch the kernel32 exports above.
// ---------------------------------------------------------------------------
typedef VOID (NTAPI* PFN_RtlExitUserProcess)(NTSTATUS);
typedef LONG (NTAPI* PFN_NtTerminateProcess)(HANDLE, NTSTATUS);
static PFN_RtlExitUserProcess g_origRtlExitUserProcess = nullptr;
static PFN_NtTerminateProcess g_origNtTerminateProcess = nullptr;

static VOID NTAPI hkRtlExitUserProcess(NTSTATUS status) {
    LogExit("RtlExitUserProcess", (UINT)status, _ReturnAddress());
    g_origRtlExitUserProcess(status);
}

static LONG NTAPI hkNtTerminateProcess(HANDLE process, NTSTATUS status) {
    // NULL/pseudo-handle = current process (that's how RtlExitUserProcess's
    // final kill and direct self-terminations arrive here).
    if (!process || process == GetCurrentProcess() ||
        GetProcessId(process) == GetCurrentProcessId()) {
        LogExit("NtTerminateProcess", (UINT)status, _ReturnAddress());
    }
    return g_origNtTerminateProcess(process, status);
}

// ---------------------------------------------------------------------------
// External exit-code watchdog. Everything above dies WITH the process; a
// separate observer does not. Spawn a hidden PowerShell that waits on this
// PID and appends "<timestamp> pid=N exit=0xNNNNNNNN" to
// %LOCALAPPDATA%\CrashDumps\FO4Remix_exitcodes.log. The exit code alone
// discriminates the silent-death classes (0xC00000FD stack overflow,
// 0xC0000409 fastfail, DXGI/driver kill codes, clean exits) even when the
// process is too dead to run a single in-process instruction.
// ---------------------------------------------------------------------------
static void SpawnExitCodeWatchdog() {
    // FO4RemixWatchdog.exe lives next to this DLL (installed together). The
    // first incarnation was an inline PowerShell one-liner, but .NET's
    // Process.ExitCode is unreliable for processes it didn't start (came
    // back empty on the first real crash); the exe uses GetExitCodeProcess.
    char dllPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&SpawnExitCodeWatchdog, &self) ||
        !GetModuleFileNameA(self, dllPath, MAX_PATH)) {
        _MESSAGE("FO4RemixPlugin: [CrashDiag] watchdog: DLL path resolution failed");
        return;
    }
    char* slash = strrchr(dllPath, '\\');
    if (slash) *(slash + 1) = '\0';

    char cmd[MAX_PATH * 2];
    sprintf_s(cmd, "\"%sFO4RemixWatchdog.exe\" %lu",
              dllPath, GetCurrentProcessId());

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        _MESSAGE("FO4RemixPlugin: [CrashDiag] exit-code watchdog spawned "
                 "(FO4Remix_exitcodes.log)");
    } else {
        _MESSAGE("FO4RemixPlugin: [CrashDiag] watchdog spawn failed (err=%lu cmd=%s)",
                 GetLastError(), cmd);
    }
}

void Install() {
    AddVectoredExceptionHandler(1 /* first */, VectoredBreadcrumb);

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        _MESSAGE("FO4RemixPlugin: [CrashDiag] MinHook init failed (%d); "
                 "exit hooks unavailable", (int)init);
        SpawnExitCodeWatchdog();
        return;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* pExit = k32 ? (void*)GetProcAddress(k32, "ExitProcess") : nullptr;
    void* pTerm = k32 ? (void*)GetProcAddress(k32, "TerminateProcess") : nullptr;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    void* pRtlExit = ntdll ? (void*)GetProcAddress(ntdll, "RtlExitUserProcess") : nullptr;
    void* pNtTerm  = ntdll ? (void*)GetProcAddress(ntdll, "NtTerminateProcess") : nullptr;

    bool exitHooked = pExit &&
        MH_CreateHook(pExit, &hkExitProcess,
                      reinterpret_cast<void**>(&g_origExitProcess)) == MH_OK &&
        MH_EnableHook(pExit) == MH_OK;
    bool termHooked = pTerm &&
        MH_CreateHook(pTerm, &hkTerminateProcess,
                      reinterpret_cast<void**>(&g_origTerminateProcess)) == MH_OK &&
        MH_EnableHook(pTerm) == MH_OK;
    bool rtlHooked = pRtlExit &&
        MH_CreateHook(pRtlExit, &hkRtlExitUserProcess,
                      reinterpret_cast<void**>(&g_origRtlExitUserProcess)) == MH_OK &&
        MH_EnableHook(pRtlExit) == MH_OK;
    bool ntTermHooked = pNtTerm &&
        MH_CreateHook(pNtTerm, &hkNtTerminateProcess,
                      reinterpret_cast<void**>(&g_origNtTerminateProcess)) == MH_OK &&
        MH_EnableHook(pNtTerm) == MH_OK;

    _MESSAGE("FO4RemixPlugin: [CrashDiag] installed (VEH=1 ExitProcess=%d "
             "TerminateProcess=%d RtlExitUserProcess=%d NtTerminateProcess=%d)",
             exitHooked ? 1 : 0, termHooked ? 1 : 0,
             rtlHooked ? 1 : 0, ntTermHooked ? 1 : 0);

    SpawnExitCodeWatchdog();
}

}  // namespace CrashDiag
