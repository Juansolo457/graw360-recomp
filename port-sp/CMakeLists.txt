# grawsp - Ghost Recon Advanced Warfighter (Xbox 360), the campaign program (GR3Xenon_Release.exe)
# on ReXGlue. Its own translation and manifest; the host code is port/src, shared with the front
# end. SDK boilerplate lives in generated/rexglue.cmake (emitted by `rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(grawsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(GRAW_SHARED_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../port/src")
set(GRAWSP_SOURCES
    src/main.cpp
    ${GRAW_SHARED_SRC}/crash_report.cpp
    ${GRAW_SHARED_SRC}/shader_debug.cpp
    ${GRAW_SHARED_SRC}/graw_loader.cpp
    ${GRAW_SHARED_SRC}/graw_process.cpp
)

if(WIN32)
    add_executable(grawsp WIN32 ${GRAWSP_SOURCES})
else()
    add_executable(grawsp ${GRAWSP_SOURCES})
endif()

rexglue_setup_target(grawsp GPU_PLUGINS xenos)
target_include_directories(grawsp PRIVATE "${GRAW_SHARED_SRC}")
if(WIN32)
    target_link_libraries(grawsp PRIVATE dbghelp shell32)
endif()
