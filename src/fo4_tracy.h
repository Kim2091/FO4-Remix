#pragma once

// Tracy instrumentation layer (2026-07-18, BetaRT port). Compiled out to
// nothing unless the build enables it:
//
//   cmake -B build -DFO4REMIX_ENABLE_TRACY=ON
//
// then connect the Tracy profiler (port 8086) to the running game. Zones
// cover the Remix thread (OnFrame phases, Present), the game render thread
// (hkPresent, SemanticCapture::Tick), and every guarded remixapi call site
// -- the live flame-graph version of the "OnFrame perf" log line, with lock
// waits and single-frame stalls visible instead of averaged away.
//
// FO4_TRACY_SCOPE wants a string LITERAL (Tracy interns the pointer).
// FO4_TRACY_SCOPE_DYN accepts a runtime string (site names) at slightly
// higher cost; only used in RemixCallGuarded.

#if defined(FO4REMIX_ENABLE_TRACY)
#include "tracy/Tracy.hpp"

#define FO4_TRACY_SCOPE(name_literal) ZoneScopedN(name_literal)
#define FO4_TRACY_SCOPE_DYN(name_cstr) \
    ZoneTransientN(fo4TracyZone__, (name_cstr), true)
#define FO4_TRACY_VALUE(value) ZoneValue(static_cast<uint64_t>(value))
#define FO4_TRACY_FRAME_MARK() FrameMark
#define FO4_TRACY_SET_THREAD_NAME(name_literal) ::tracy::SetThreadName(name_literal)
#else
#define FO4_TRACY_SCOPE(name_literal)
#define FO4_TRACY_SCOPE_DYN(name_cstr)
#define FO4_TRACY_VALUE(value)
#define FO4_TRACY_FRAME_MARK()
#define FO4_TRACY_SET_THREAD_NAME(name_literal)
#endif
