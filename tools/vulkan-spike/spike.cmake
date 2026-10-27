# M0 spike for the native renderer on Vulkan (#41, docs/VULKAN_RENDERER.md). Local only.
# Builds graw_vulkan_spike inside the SDK's own CMake build (Vulkan on), without editing the SDK:
#   cmake --preset win-amd64 -B out/win-amd64-vulkan -DREXGLUE_USE_VULKAN=ON
#         -DREXGLUE_BUILD_TESTS=OFF -DCMAKE_PROJECT_ReXGlue_INCLUDE=<this file>
# The target is defined at the end of the SDK's top-level CMakeLists (its targets exist then).
# The SDK only uses SPIRV-Tools' headers; the validator library is built here from its copy
# (a subdirectory can't be added during deferred execution).
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/spirv-headers")
add_subdirectory("${CMAKE_SOURCE_DIR}/thirdparty/spirv-tools" "${CMAKE_BINARY_DIR}/spike-spirv-tools"
    EXCLUDE_FROM_ALL)
set(GRAW_VULKAN_SPIKE_TARGETS "${CMAKE_CURRENT_LIST_DIR}/spike_targets.cmake")
cmake_language(EVAL CODE "cmake_language(DEFER CALL include [[${GRAW_VULKAN_SPIKE_TARGETS}]])")
