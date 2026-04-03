// Compatibility header providing types that F4SE SDK expects from xse::common.
// This avoids needing to pull in the full xse-common dependency.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <new>
#include <windows.h>
#include <shlobj.h>

// Base integer types used throughout F4SE
typedef uint8_t  UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;
typedef int8_t   SInt8;
typedef int16_t  SInt16;
typedef int32_t  SInt32;
typedef int64_t  SInt64;

// Compile-time size check
#define STATIC_ASSERT(a) static_assert(a, #a)

// Simple debug log — write to F4SE plugin log
namespace F4SECompat {
    inline FILE* GetLogFile() {
        static FILE* s_log = nullptr;
        if (!s_log) {
            char path[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, path))) {
                strcat_s(path, "\\My Games\\Fallout4\\F4SE\\FO4RemixPlugin.log");
                fopen_s(&s_log, path, "w");
            }
        }
        return s_log;
    }
}

#define _MESSAGE(fmt, ...) do { \
    FILE* _f = F4SECompat::GetLogFile(); \
    if (_f) { fprintf(_f, fmt "\n", ##__VA_ARGS__); fflush(_f); } \
} while(0)

#define _WARNING(fmt, ...) _MESSAGE("WARNING: " fmt, ##__VA_ARGS__)
#define _ERROR(fmt, ...)   _MESSAGE("ERROR: " fmt, ##__VA_ARGS__)

// ASSERT macro used by BranchTrampoline and others
#define ASSERT(a) do { if (!(a)) { _MESSAGE("ASSERT FAILED: %s", #a); __debugbreak(); } } while(0)

// NOTE: MEMBER_FN_PREFIX and DEFINE_MEMBER_FN are defined in f4se_common/Utilities.h.
// We do NOT redefine them here — let F4SE's own headers provide them.
