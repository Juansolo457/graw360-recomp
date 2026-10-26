// GRAW native renderer, Vulkan: the present pass's vertex shader (vulkan_present.h), present.cpp's
// blit vs. A triangle over the viewport (the image's rectangle in the back buffer), with the
// texel position of each pixel.
#version 460

layout(push_constant) uniform Blit {
  vec2 image_size;
  uint ramp_mode;
  uint point_sampled;
} pc;

layout(location = 0) out vec2 texel;

void main() {
  vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);  // Vulkan's clip space: y down
  texel = uv * pc.image_size;
}
