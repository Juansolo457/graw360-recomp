// GRAW native renderer, Vulkan: the present pass's fragment shader (vulkan_present.h),
// present.cpp's blit ps without HDR: each texel through the display controller's gamma ramp
// (gamma_ramp.h), then point sampled when the image is shown at its size, else bilinear.
#version 460

layout(push_constant) uniform Blit {
  vec2 image_size;  // the image in texels (the texture can be larger)
  uint ramp_mode;   // RampMode: 0 none, 1 PWL, 2 256-entry table
  uint point_sampled;
} pc;

// The front buffer through a view with its fetch constant's swizzle.
layout(set = 0, binding = 0) uniform sampler2DArray src;
layout(set = 0, binding = 1, std140) uniform Ramp {
  uvec4 ramp[96];  // kPwlWords (or kTableWords) words
};

layout(location = 0) in vec2 texel;
layout(location = 0) out vec4 color;

uint RampWord(uint i) { return ramp[i >> 2][i & 3]; }

vec3 Gamma(vec3 c) {
  if (pc.ramp_mode == 1) {
    uvec3 x = uvec3(c * 1023.0 + 0.5);
    uvec3 w = uvec3(RampWord((x.r >> 3) * 3), RampWord((x.g >> 3) * 3 + 1),
                    RampWord((x.b >> 3) * 3 + 2));
    return min((vec3(w & 0xFFFFu) + vec3((x & 7u) * (w >> 16)) * 0.125) * (1.0 / 65472.0), 1.0);
  }
  if (pc.ramp_mode == 2) {
    uvec3 x = uvec3(c * 255.0 + 0.5);
    return vec3((RampWord(x.r) >> 20) & 1023u, (RampWord(x.g) >> 10) & 1023u,
                RampWord(x.b) & 1023u) * (1.0 / 1023.0);
  }
  return c;
}

vec3 Texel(ivec2 p) { return Gamma(texelFetch(src, ivec3(p, 0), 0).rgb); }

vec3 Image(vec2 t) {
  ivec2 hi = ivec2(pc.image_size) - 1;
  if (pc.point_sampled != 0) return Texel(min(ivec2(t), hi));
  t -= 0.5;
  vec2 f = t - floor(t);
  ivec2 a = clamp(ivec2(floor(t)), ivec2(0), hi), b = min(a + 1, hi);
  vec3 top = mix(Texel(a), Texel(ivec2(b.x, a.y)), f.x);
  vec3 bottom = mix(Texel(ivec2(a.x, b.y)), Texel(b), f.x);
  return mix(top, bottom, f.y);
}

void main() { color = vec4(Image(texel), 1.0); }
