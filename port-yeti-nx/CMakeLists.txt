# grawyeti (port-yeti-nx) - the campaign (Yeti_Final_Xenon.exe) on fear-nx's patched ReXGlue SDK
# with fear-nx's native Vulkan Xenos renderer, the way fear360-recomp's port-nx runs F.E.A.R.
# Same manifest and host code as port-yeti; the renderer is the difference (#2: the stock plugin
# never shows the Cross-Com videos, which the game decodes; fear-nx's renderer fixed the same
# class of bug in F.E.A.R.).
#
# The SDK is built from source (REXSDK_DIR, tools/build.ps1 -Project port-yeti-nx): a copy in
# third_party/rexglue-sdk-nx of ../fear360-recomp/third_party/rexglue-sdk-nx (11c330c; a copy, not a
# junction, because the SDK builds into its own out/ folder and would overwrite fear360's) (ReXGlue c94f5eb +
# fear-nx's native-vulkan patch + fear360's patches). The Xenos plugin is linked into the
# executable whole instead of loaded as a DLL. src/native/ is GPL-3.0-or-later (fear-nx), so
# builds of this program are GPL; see NOTICE.

cmake_minimum_required(VERSION 3.25)
project(grawyeti LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXSDK_DIR)
    message(FATAL_ERROR "port-yeti-nx builds the SDK from source: pass -DREXSDK_DIR=<third_party/rexglue-sdk-nx>.")
endif()
if(NOT REXGLUE_GPU_XENOS_STATIC)
    message(FATAL_ERROR "port-yeti-nx links the Xenos plugin into grawyeti.exe: pass -DREXGLUE_GPU_XENOS_STATIC=ON.")
endif()

# Built for x86-64-v3 (tools/build.ps1); no floating-point contraction (see port-yeti).
add_compile_options($<$<CXX_COMPILER_ID:Clang,AppleClang>:-ffp-contract=off>)

include(generated/rexglue.cmake)

set(GRAW_SHARED_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../port/src")

# fear-nx's renderer (copy; see src/native/UPSTREAM.md). Its tests are not built.
set(GRAW_NATIVE_SOURCES
    src/native/native_system.cpp
    src/native/native_renderer.cpp
    src/native/native_textures.cpp
    src/native/native_geometry_shaders.cpp
    src/display_vblank.cpp
    src/texture_pack.cpp
)

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
    ${GRAW_SHARED_SRC}/cpu_check.cpp
    ${GRAW_NATIVE_SOURCES}
)

set_source_files_properties(${GRAW_SHARED_SRC}/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64-v2"
                            SKIP_PRECOMPILE_HEADERS ON)

add_executable(grawyeti WIN32 ${GRAWYETI_SOURCES})

rexglue_setup_target(grawyeti)
target_compile_definitions(grawyeti PRIVATE GRAW_NX=1)
target_include_directories(grawyeti PRIVATE
    "${GRAW_SHARED_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${REXSDK_DIR}/src/graphics"
    "${REXSDK_DIR}/thirdparty/glslang"
    "${REXSDK_DIR}/thirdparty/renderdoc"   # rex/ui/renderdoc_api.h, private to the plugin
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
)

# The renderer is written against the plugin's internals (shader translator, texture conversion,
# register file, ...), which the DLL build keeps private. Link the static plugin whole so its
# cvars and rex_gpu_create survive too.
target_link_libraries(grawyeti PRIVATE rexgpu-xenos xxHash::xxhash)
if(WIN32)
    target_link_libraries(grawyeti PRIVATE dbghelp shell32 dxgi)   # crash reports, args, display vblank
endif()
target_link_options(grawyeti PRIVATE "LINKER:/WHOLEARCHIVE:$<TARGET_FILE:rexgpu-xenos>")
set_property(TARGET grawyeti APPEND PROPERTY LINK_DEPENDS $<TARGET_FILE:rexgpu-xenos>)
