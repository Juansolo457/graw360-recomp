# graw - Ghost Recon Advanced Warfighter (Xbox 360) on ReXGlue.
#
# SDK boilerplate lives in generated/rexglue.cmake (emitted by `rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(graw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Built for x86-64-v3 (tools/build.ps1: AVX2 and FMA). The translated code turns every PowerPC
# multiply-add into std::fma, which on the SDK's default SSE4.1 target is a call into the C
# runtime's software FMA: 14,803 sites in the campaign's code (#11). No floating-point
# contraction, though: clang would otherwise fuse a separate a*b+c into one FMA, rounding once
# where the PowerPC rounded twice.
add_compile_options($<$<CXX_COMPILER_ID:Clang,AppleClang>:-ffp-contract=off>)

include(generated/rexglue.cmake)

set(GRAW_SOURCES
    src/main.cpp
    src/crash_report.cpp
    src/shader_debug.cpp
    src/graw_loader.cpp
    src/graw_process.cpp
    src/fpscr_guard.cpp
    src/autopilot.cpp
    src/ui_style.cpp
    src/ui_pad.cpp
    src/frame_timing.cpp
    src/cpu_check.cpp
)

# The CPU check runs before any x86-64-v3 code (src/cpu_check.cpp), so it is built for v2 and
# without the precompiled header.
set_source_files_properties(src/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64-v2"
                            SKIP_PRECOMPILE_HEADERS ON)

if(WIN32)
    add_executable(graw WIN32 ${GRAW_SOURCES})
else()
    add_executable(graw ${GRAW_SOURCES})
endif()

# The Xenos plugin is copied next to graw.exe so the runtime can load it.
rexglue_setup_target(graw GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(graw PRIVATE dbghelp shell32)   # crash reports; CommandLineToArgvW (graw_process.cpp)
endif()
