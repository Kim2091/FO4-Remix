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

# Copy remix_c.h from dxvk-remix-gmod if not present
set(REMIX_HEADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../dxvk-remix-gmod/public/include/remix/remix_c.h")
set(REMIX_HEADER_DST "${CMAKE_CURRENT_SOURCE_DIR}/extern/remix/remix_c.h")
if(EXISTS "${REMIX_HEADER_SRC}" AND NOT EXISTS "${REMIX_HEADER_DST}")
    file(COPY "${REMIX_HEADER_SRC}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/extern/remix/")
    message(STATUS "Copied remix_c.h from dxvk-remix-gmod")
endif()

# F4SE minimal sources — only what we need for a plugin
# We use our own compatibility header (f4se_compat.h) instead of xse::common
add_library(f4se_minimal STATIC
    "${F4SE_SDK_DIR}/f4se_common/Relocation.cpp"
    "${F4SE_SDK_DIR}/f4se/GameCamera.cpp"
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
    src/present_hook.h
    src/present_hook.cpp
    src/remix_api.h
    src/remix_api.cpp
    src/remix_renderer.h
    src/remix_renderer.cpp
    src/camera.h
    src/camera.cpp
    src/scene_extractor.h
    src/scene_extractor.cpp
    src/light_extractor.h
    src/light_extractor.cpp
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
    d3d11
    dxgi
    shell32
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
