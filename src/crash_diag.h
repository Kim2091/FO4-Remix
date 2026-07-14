#pragma once

// ---------------------------------------------------------------------------
// Last-resort process-death breadcrumbs (2026-07-12).
//
// The crash-hunt instrumentation ladder, in the order a dying process falls
// through it:
//   1. RemixCallGuarded / resolver fences  -- SEH + C++ at known call sites
//   2. std::set_terminate (main.cpp)       -- uncaught C++ exceptions that
//      unwind through this module's EH frames (abort fast-fail class)
//   3. WER events                          -- unhandled SEH, fastfail
//   4. THIS MODULE                         -- deaths that leave NOTHING above:
//      - stack overflow: exception dispatch itself needs stack; when the
//        guard page is spent the kernel kills the process with no WER, no
//        handlers, nothing. A vectored exception handler runs FIRST-CHANCE
//        and can usually still squeeze out one log line.
//      - heap corruption (0xC0000374): often terminates without a report.
//      - deliberate exits: something calls ExitProcess/TerminateProcess
//        (engine fatal handlers do this) -- hook them and log the CALLER
//        (return-address module+offset), plus a diagnostic dump.
//
// Everything here is observation only: handlers always continue the search /
// call the original, so behavior is unchanged except for the breadcrumbs.
// ---------------------------------------------------------------------------
namespace CrashDiag {

// Install the vectored exception handler and the ExitProcess/TerminateProcess
// hooks. Call once at plugin load (initializes MinHook if needed; tolerant of
// it already being initialized).
void Install();

// Log the backtrace of the most recent C++ throw on THIS thread (stashed
// first-chance by the VEH, where the throw stack is still intact -- by the
// time a catch block runs, the unwinder has already destroyed it). Call from
// a catch block right after logging what(); no-op if nothing new was thrown
// on this thread since the last call. Session-capped per site class so a
// throw storm can't flood the log.
void LogLastCxxThrow(const char* site);

}  // namespace CrashDiag
