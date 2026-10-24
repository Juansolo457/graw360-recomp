# grawmp - Ghost Recon Advanced Warfighter (Xbox 360), the GR3 program (GR3Xenon_Release.exe:
# Instant Action, co-op and multiplayer)
# on ReXGlue. Its own translation and manifest; the host code is port/src, shared with the front
# end. SDK boilerplate lives in generated/rexglue.cmake (emitted by `rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(grawmp LANGUAGES CXX)

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
set(GRAWMP_SOURCES
    src/main.cpp
    ${GRAW_SHARED_SRC}/crash_report.cpp
    ${GRAW_SHARED_SRC}/shader_debug.cpp
    ${GRAW_SHARED_SRC}/graw_loader.cpp
    ${GRAW_SHARED_SRC}/graw_process.cpp
    ${GRAW_SHARED_SRC}/fpscr_guard.cpp
    ${GRAW_SHARED_SRC}/autopilot.cpp
    ${GRAW_SHARED_SRC}/ui_style.cpp
    ${GRAW_SHARED_SRC}/ui_pad.cpp
    ${GRAW_SHARED_SRC}/cpu_check.cpp
)

# The CPU check runs before any x86-64-v3 code (src/cpu_check.cpp), so it is built for v2 and
# without the precompiled header.
set_source_files_properties(${GRAW_SHARED_SRC}/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64-v2"
                            SKIP_PRECOMPILE_HEADERS ON)

if(WIN32)
    add_executable(grawmp WIN32 ${GRAWMP_SOURCES})
else()
    add_executable(grawmp ${GRAWMP_SOURCES})
endif()

rexglue_setup_target(grawmp GPU_PLUGINS xenos)
target_include_directories(grawmp PRIVATE "${GRAW_SHARED_SRC}")
if(WIN32)
    target_link_libraries(grawmp PRIVATE dbghelp shell32)
endif()
