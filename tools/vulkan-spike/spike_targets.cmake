# graw_vulkan_spike: the SDK's Xenos ucode analysis and SPIR-V translator plus SPIRV-Tools, with
# the include paths, definitions and options of the Vulkan renderer plugin they normally live in.
set(_g "${CMAKE_SOURCE_DIR}/src/graphics")
add_executable(graw_vulkan_spike
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
    ${_g}/format/ucode.cpp
    ${_g}/flags.cpp
    ${_g}/registers.cpp
    ${_g}/xenos.cpp
    ${_g}/util/draw.cpp
    ${_g}/pipeline/shader/shader.cpp
    ${_g}/pipeline/shader/translator.cpp
    ${_g}/pipeline/shader/translator_disasm.cpp
    ${_g}/pipeline/shader/spirv_builder.cpp
    ${_g}/pipeline/shader/spirv.cpp
    ${_g}/pipeline/shader/spirv_translator.cpp
    ${_g}/pipeline/shader/spirv_translator_alu.cpp
    ${_g}/pipeline/shader/spirv_translator_fetch.cpp
    ${_g}/pipeline/shader/spirv_translator_memexport.cpp
    ${_g}/pipeline/shader/spirv_translator_rb.cpp)
foreach(_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES CXX_STANDARD)
    get_target_property(_v rexgpu-xenos ${_prop})
    if(_v)
        set_property(TARGET graw_vulkan_spike PROPERTY ${_prop} "${_v}")
    endif()
endforeach()
target_link_libraries(graw_vulkan_spike PRIVATE
    rexruntime xxHash::xxhash Vulkan::Headers glslang::SPIRV glslang SPIRV-Tools-static)
get_target_property(_out rexruntime RUNTIME_OUTPUT_DIRECTORY)
if(_out)
    set_target_properties(graw_vulkan_spike PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_out}")
endif()
unset(_g)
unset(_out)
