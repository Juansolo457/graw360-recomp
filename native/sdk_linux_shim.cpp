// GRAW native renderer: what the SDK's shader translators (shader_xlate.cmake) need from the SDK
// outside their sources. On Windows the game links them from the SDK; on Linux they're private to
// the renderer plugin (librexgpu-xenos), which the game loads at run time and doesn't link, so the
// app has its own copies, as sdk_cvar_shim.h keeps its own cvars: the D3D10 sample positions
// (rex/graphics/util/draw.h, the values of the SDK's src/graphics/util/draw.cpp, BSD-3) and the
// dump_shaders cvar's storage, unregistered, so the plugin's --dump_shaders stays the plugin's
// (the native renderer dumps its own with --graw_native_vulkan_dump_shaders).
#if !defined(_WIN32)

#include <cstdint>
#include <string>

namespace rex::graphics::draw_util {

extern const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
extern const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

}  // namespace rex::graphics::draw_util

std::string& FLAGS_dump_shaders_storage_() {
  static std::string storage;
  return storage;
}

#endif
