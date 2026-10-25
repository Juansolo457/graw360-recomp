# graw_shader_xlate: ReXGlue's Xenos ucode analysis and DXBC shader translator (BSD-3, from
# Xenia), built from the SDK source tree as a static library for the app. In the SDK these
# sources only exist inside the rexgpu-xenos plugin DLL, which exports nothing.
#
#   set(GRAW_REXGLUE_SDK_SRC <SDK source root>)   # default: thirdparty/rexglue-sdk of the game
#   include(<this file>)
#   target_link_libraries(<target> PRIVATE graw_shader_xlate)
#
# Requires the rex::runtime and rex::xxhash imported targets (find_package(rexglue)).
if(NOT GRAW_REXGLUE_SDK_SRC)
    set(GRAW_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk")
endif()
set(_graw_gfx "${GRAW_REXGLUE_SDK_SRC}/src/graphics")
add_library(graw_shader_xlate STATIC
    ${_graw_gfx}/format/ucode.cpp
    ${_graw_gfx}/pipeline/shader/shader.cpp
    ${_graw_gfx}/pipeline/shader/translator.cpp
    ${_graw_gfx}/pipeline/shader/translator_disasm.cpp
    ${_graw_gfx}/pipeline/shader/dxbc.cpp
    ${_graw_gfx}/pipeline/shader/dxbc_translator.cpp
    ${_graw_gfx}/pipeline/shader/dxbc_translator_alu.cpp
    ${_graw_gfx}/pipeline/shader/dxbc_translator_fetch.cpp
    ${_graw_gfx}/pipeline/shader/dxbc_translator_memexport.cpp
    ${_graw_gfx}/pipeline/shader/dxbc_translator_om.cpp
    ${GRAW_REXGLUE_SDK_SRC}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_include_directories(graw_shader_xlate PRIVATE "${GRAW_REXGLUE_SDK_SRC}")
# The translator sources define three GPU cvars the plugin DLL registers too; the shim turns the
# app's copies into unregistered storage (see the header).
# (Force-included: /FI for clang-cl and MSVC, -include for clang's GNU driver, as GRAW builds.)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(graw_shader_xlate PRIVATE
        "/FI${CMAKE_CURRENT_LIST_DIR}/sdk_cvar_shim.h" -Wno-unused-variable)
else()
    target_compile_options(graw_shader_xlate PRIVATE
        -include "${CMAKE_CURRENT_LIST_DIR}/sdk_cvar_shim.h" -Wno-unused-variable)
endif()
target_link_libraries(graw_shader_xlate PUBLIC rex::runtime rex::xxhash)
unset(_graw_gfx)
