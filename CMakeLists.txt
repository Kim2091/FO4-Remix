cmake_minimum_required(VERSION 3.18)
project(FO4RemixPlugin VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# F4SE SDK paths
set(F4SE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../f4se-0.7.7")

# Fetch MinHook
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# Copy remix_c.h from a dxvk-remix source. Prefer the in-source RTX Remix Plus
# checkout (dxvk-remix/) over the legacy sibling (../dxvk-remix-gmod/). Always
# refresh the snapshot at configure time so an upstream ABI change picked up
# via 'git pull' is reflected on the next build without manual cache deletion.
set(REMIX_HEADER_DST "${CMAKE_CURRENT_SOURCE_DIR}/extern/remix/remix_c.h")
set(REMIX_HEADER_SRC "")
foreach(REMIX_CANDIDATE
        "${CMAKE_CURRENT_SOURCE_DIR}/dxvk-remix/public/include/remix/remix_c.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../dxvk-remix/public/include/remix/remix_c.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../dxvk-remix/public/include/remix/remix_c.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../dxvk-remix/public/include/remix/remix_c.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../dxvk-remix-gmod/public/include/remix/remix_c.h")
    if(EXISTS "${REMIX_CANDIDATE}")
        set(REMIX_HEADER_SRC "${REMIX_CANDIDATE}")
        break()
    endif()
endforeach()
if(REMIX_HEADER_SRC)
    file(COPY "${REMIX_HEADER_SRC}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/extern/remix/")
    message(STATUS "Refreshed remix_c.h from ${REMIX_HEADER_SRC}")
else()
    message(WARNING "remix_c.h source not found; using cached snapshot at ${REMIX_HEADER_DST}")
endif()

# F4SE minimal sources — only what we need for a plugin
# We use our own compatibility header (f4se_compat.h) instead of xse::common
add_library(f4se_minimal STATIC
    "${F4SE_SDK_DIR}/f4se_common/Relocation.cpp"
    "${F4SE_SDK_DIR}/f4se/GameCamera.cpp"
    "${F4SE_SDK_DIR}/f4se/GameForms.cpp"
)
target_include_directories(f4se_minimal PUBLIC
    "${F4SE_SDK_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"   # for f4se_compat.h
)
target_compile_definitions(f4se_minimal PUBLIC
    RUNTIME_VERSION=RUNTIME_VERSION_1_11_191
)
# Force-include our compatibility header before anything else
target_compile_options(f4se_minimal PUBLIC /FI"f4se_compat.h")

# Plugin DLL
add_library(FO4RemixPlugin SHARED
    src/main.cpp
    src/config.h
    src/config.cpp
    src/present_hook.h
    src/present_hook.cpp
    src/remix_api.h
    src/remix_api.cpp
    src/remix_renderer.h
    src/remix_renderer.cpp
    src/camera.h
    src/camera.cpp
    src/bs_extraction.h
    src/bs_extraction.cpp
    src/draw_capture.h
    src/draw_capture.cpp
    src/fo4_diagnostics.h
    src/fo4_diagnostics.cpp
    src/startup_diag.h
    src/startup_diag.cpp
    src/semantic_capture.h
    src/semantic_capture.cpp
    src/resolvers/lighting_static.h
    src/resolvers/lighting_static.cpp
    src/resolvers/water.h
    src/resolvers/water.cpp
    src/weather_bridge.h
    src/weather_bridge.cpp
    src/f4se_compat.h
)

target_include_directories(FO4RemixPlugin PRIVATE
    "${F4SE_SDK_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern"
)

target_link_libraries(FO4RemixPlugin PRIVATE
    f4se_minimal
    minhook
    d3d9
    d3d11
    dxgi
    shell32
    psapi
    winmm
)

target_compile_definitions(FO4RemixPlugin PRIVATE
    RUNTIME_VERSION=RUNTIME_VERSION_1_11_191
)

# Force-include compatibility header
target_compile_options(FO4RemixPlugin PRIVATE /FI"f4se_compat.h")

# Output name without lib prefix
set_target_properties(FO4RemixPlugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "FO4RemixPlugin"
)
