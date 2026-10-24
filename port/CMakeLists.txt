# graw - Ghost Recon Advanced Warfighter (Xbox 360) on ReXGlue.
#
# SDK boilerplate lives in generated/rexglue.cmake (emitted by `rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(graw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(GRAW_SOURCES
    src/main.cpp
    src/crash_report.cpp
    src/shader_debug.cpp
    src/graw_loader.cpp
    src/graw_process.cpp
    src/fpscr_guard.cpp
    src/autopilot.cpp
)

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
