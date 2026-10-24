# grawyeti - Ghost Recon Advanced Warfighter (Xbox 360), the campaign (Yeti_Final_Xenon.exe, Ubisoft
# Paris's Yeti engine) on ReXGlue. Its own translation and manifest; the host code is port/src,
# shared with the front end. SDK boilerplate lives in generated/rexglue.cmake (`rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(grawyeti LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
)

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
