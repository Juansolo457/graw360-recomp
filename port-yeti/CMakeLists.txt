# grawyeti - Ghost Recon Advanced Warfighter (Xbox 360), the campaign (Yeti_Final_Xenon.exe, Ubisoft
# Paris's Yeti engine) on ReXGlue. Its own translation and manifest; the host code is port/src,
# shared with the front end. SDK boilerplate lives in generated/rexglue.cmake (`rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(grawyeti LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Built for x86-64-v3 (tools/build.ps1: AVX2 and FMA). The translated code turns every PowerPC
# multiply-add into std::fma, which on the SDK's default SSE4.1 target is a call into the C
# runtime's software FMA: 14,803 sites in the campaign's code (#11). No floating-point
# contraction, though: clang would otherwise fuse a separate a*b+c into one FMA, rounding once
# where the PowerPC rounded twice.
add_compile_options($<$<CXX_COMPILER_ID:Clang,AppleClang>:-ffp-contract=off>)

include(generated/rexglue.cmake)

set(GRAW_SHARED_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../port/src")
set(GRAWYETI_SOURCES
    src/main.cpp
    ${GRAW_SHARED_SRC}/crash_report.cpp
    ${GRAW_SHARED_SRC}/shader_debug.cpp
    ${GRAW_SHARED_SRC}/graw_loader.cpp
    ${GRAW_SHARED_SRC}/graw_process.cpp
    ${GRAW_SHARED_SRC}/fpscr_guard.cpp
    ${GRAW_SHARED_SRC}/autopilot.cpp
    ${GRAW_SHARED_SRC}/ui_style.cpp
    ${GRAW_SHARED_SRC}/ui_pad.cpp
    ${GRAW_SHARED_SRC}/frame_timing.cpp
    ${GRAW_SHARED_SRC}/bink_probe.cpp
    ${GRAW_SHARED_SRC}/rtt_probe.cpp
    ${GRAW_SHARED_SRC}/blend_probe.cpp
    ${GRAW_SHARED_SRC}/resolve_probe.cpp
    ${GRAW_SHARED_SRC}/cpu_check.cpp
)

# The CPU check runs before any x86-64-v3 code (src/cpu_check.cpp), so it is built for v2 and
# without the precompiled header.
set_source_files_properties(${GRAW_SHARED_SRC}/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64-v2"
                            SKIP_PRECOMPILE_HEADERS ON)

if(WIN32)
    add_executable(grawyeti WIN32 ${GRAWYETI_SOURCES})
else()
    add_executable(grawyeti ${GRAWYETI_SOURCES})
endif()

rexglue_setup_target(grawyeti GPU_PLUGINS xenos)
target_include_directories(grawyeti PRIVATE "${GRAW_SHARED_SRC}")
if(WIN32)
    target_link_libraries(grawyeti PRIVATE dbghelp shell32)
endif()

# The native D3D12 renderer (native/, docs/NATIVE_RENDERER.md), off at run time unless
# --graw_native. It compiles ReXGlue's shader translator and texture format tables from the SDK
# source tree (the plugin DLL exports none of them): third_party/rexglue-sdk-bd833a2, the same
# commit as the prebuilt SDK.
set(GRAW_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rexglue-sdk-bd833a2"
    CACHE PATH "ReXGlue SDK source tree (bd833a2) for the native renderer")
if(WIN32 AND EXISTS "${GRAW_REXGLUE_SDK_SRC}/src/graphics/pipeline/shader/dxbc_translator.cpp")
    # The tree must carry the SDK patches the native renderer compiles against (tools/setup.ps1
    # applies them): a line each one adds, so an unpatched tree stops here rather than in
    # native/'s compile or link errors.
    foreach(check
            "pgr4-native/0021-*.patch|include/rex/graphics/format/dxbc.h|kSampleB = 74"
            "pgr4-native/0022-*.patch|include/rex/graphics/pipeline/shader/dxbc_translator.h|textures_unsigned"
            "rexglue-bd833a2-gpu-skip-draws.patch|include/rex/graphics/command_processor.h|for presenters"
            "rexglue-bd833a2-gpu-skip-draws.patch|src/graphics/command_processor.cpp|gpu_skip_draws"
            "rexglue-bd833a2-implicit-gradients-switch.patch|src/graphics/pipeline/shader/dxbc_translator_fetch.cpp|bool g_dxbc_implicit_gradients"
            "rexglue-bd833a2-spirv-exp-adjust.patch|src/graphics/pipeline/shader/spirv_translator_fetch.cpp|fetch_constant_word_3_signed"
            "rexglue-bd833a2-spirv-implicit-gradients.patch|src/graphics/pipeline/shader/spirv_translator_fetch.cpp|bool g_spirv_implicit_gradients")
        string(REPLACE "|" ";" check "${check}")
        list(GET check 0 patch)
        list(GET check 1 file)
        list(GET check 2 marker)
        file(READ "${GRAW_REXGLUE_SDK_SRC}/${file}" text)
        string(FIND "${text}" "${marker}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "The native renderer needs the SDK patch tools/patches/${patch} in "
                "${GRAW_REXGLUE_SDK_SRC} (${file} lacks it): run tools/setup.ps1 -Force "
                "(docs/SETUP.md), or remove the tree to build without the native renderer.")
        endif()
    endforeach()
    set(GRAW_NATIVE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../native")
    file(GLOB GRAW_NATIVE_SOURCES CONFIGURE_DEPENDS "${GRAW_NATIVE_SRC}/*.cpp")
    # An object library: its hooks (REX_HOOK_RAW) override the generated functions only when
    # their objects are linked in, and it compiles against the SDK source tree's headers first,
    # which carry the pgr4-native SDK patches the renderer needs (tools/patches/pgr4-native/:
    # shader translator modifications, the command processor's gamma ramp, gpu_skip_draws).
    # The renderer plugin is built from the same tree with the same patches.
    add_library(graw_native OBJECT ${GRAW_NATIVE_SOURCES}
        "${GRAW_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
        "${GRAW_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp")
    target_include_directories(graw_native BEFORE PRIVATE "${GRAW_REXGLUE_SDK_SRC}/include")
    target_include_directories(graw_native PRIVATE "${GRAW_NATIVE_SRC}" "${GRAW_REXGLUE_SDK_SRC}")
    # The Vulkan backend (vulkan_device.h): headers only, the loader is opened at run time; VMA
    # (vulkan_memory.cpp implements it).
    target_include_directories(graw_native PRIVATE
        "${GRAW_REXGLUE_SDK_SRC}/thirdparty/vulkan-headers/include"
        "${GRAW_REXGLUE_SDK_SRC}/thirdparty/vulkan-memory-allocator/include")
    rexglue_apply_target_settings(graw_native)
    target_link_libraries(graw_native PRIVATE rex::runtime rex::xxhash)
    include("${GRAW_NATIVE_SRC}/shader_xlate.cmake")
    target_include_directories(graw_shader_xlate BEFORE PRIVATE "${GRAW_REXGLUE_SDK_SRC}/include")
    target_include_directories(graw_shader_xlate_spirv BEFORE PRIVATE "${GRAW_REXGLUE_SDK_SRC}/include")
    # The Vulkan backend's sources include the SPIR-V translator's and glslang's headers.
    target_include_directories(graw_native PRIVATE
        "${GRAW_REXGLUE_SDK_SRC}/thirdparty/glslang" "${GRAW_REXGLUE_SDK_SRC}/thirdparty/renderdoc")
    target_link_libraries(grawyeti PRIVATE graw_native graw_shader_xlate graw_shader_xlate_spirv
        d3d12 dxgi dxguid)
    target_compile_definitions(grawyeti PRIVATE GRAW_NATIVE_RENDERER=1)
else()
    message(STATUS "grawyeti: native renderer not built (needs Windows and ${GRAW_REXGLUE_SDK_SRC})")
endif()
