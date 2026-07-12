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
// Vectored exception handler: first-chance, add-only (cannot be displaced by
// later SetUnhandledExceptionFilter calls). Logs ONLY the silent-killer
// codes; routine first-chance AVs (the resolver's guarded stale-pointer
// reads) pass through untouched.
// ---------------------------------------------------------------------------
static LONG CALLBACK VectoredBreadcrumb(EXCEPTION_POINTERS* xp) {
    const DWORD code = xp && xp->ExceptionRecord
        ? xp->ExceptionRecord->ExceptionCode : 0;
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

void Install() {
    AddVectoredExceptionHandler(1 /* first */, VectoredBreadcrumb);

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        _MESSAGE("FO4RemixPlugin: [CrashDiag] MinHook init failed (%d); "
                 "exit hooks unavailable", (int)init);
        return;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* pExit = k32 ? (void*)GetProcAddress(k32, "ExitProcess") : nullptr;
    void* pTerm = k32 ? (void*)GetProcAddress(k32, "TerminateProcess") : nullptr;

    bool exitHooked = pExit &&
        MH_CreateHook(pExit, &hkExitProcess,
                      reinterpret_cast<void**>(&g_origExitProcess)) == MH_OK &&
        MH_EnableHook(pExit) == MH_OK;
    bool termHooked = pTerm &&
        MH_CreateHook(pTerm, &hkTerminateProcess,
                      reinterpret_cast<void**>(&g_origTerminateProcess)) == MH_OK &&
        MH_EnableHook(pTerm) == MH_OK;

    _MESSAGE("FO4RemixPlugin: [CrashDiag] installed (VEH=1 ExitProcess=%d "
             "TerminateProcess=%d)", exitHooked ? 1 : 0, termHooked ? 1 : 0);
}

}  // namespace CrashDiag
