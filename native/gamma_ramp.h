// GRAW native renderer: the display controller's gamma ramp, which both present passes apply
// (present.cpp's D3D12 blit, vulkan_present.cpp; docs/VULKAN_RENDERER.md, M5).
//
// The ramp as the Xenos command processor holds it from the guest's DC_LUT register writes: 128
// PWL entries per channel (DC_LUT_PWL_DATA: base 0:15, delta 16:31) for 2_10_10_10 front
// buffers, 256 table entries (DC_LUT_30_COLOR: blue 0:9, green 10:19, red 20:29) for 8_8_8_8
// ones, as D3D's presentation parameters set them up. The ramp math follows Xenia's
// apply_gamma_pwl / apply_gamma_table (BSD-3-Clause, see THIRD_PARTY_NOTICES.md). The ramp
// applies to each texel before filtering, so a scaled image filters display values like the
// console's scaler does.
//
// Threading: the presenting thread (the command processor's thread writes the ramp).
#pragma once

#include <cstdint>

namespace rex::graphics {
class CommandProcessor;
}

namespace graw::native {

enum RampMode : uint32_t { kRampNone = 0, kRampPwl = 1, kRampTable = 2 };
constexpr uint32_t kPwlWords = 128 * 3, kTableWords = 256;

// The Xenos plugin's command processor; null before it exists.
const rex::graphics::CommandProcessor* GuestCommandProcessor();

// Output of a PWL ramp channel for a 10-bit input (the shaders' math), for the log.
double PwlOut(const uint32_t* words, uint32_t channel, uint32_t x);

class GammaRamp {
 public:
  // Copies the guest's current ramp of `mode` into `out` (kPwlWords or kTableWords words; may
  // be write-combined memory) and logs its first changes (`frame`: the frame index). False
  // without a command processor yet.
  bool Read(RampMode mode, uint64_t frame, uint32_t* out);
  // The last ramp's output for 1.0 per channel, display-encoded.
  const float* white() const { return white_; }

 private:
  const rex::graphics::CommandProcessor* command_processor_ = nullptr;
  uint32_t last_[kPwlWords] = {};
  RampMode last_mode_ = kRampNone;
  uint32_t changes_logged_ = 0;
  float white_[3] = {1.0f, 1.0f, 1.0f};
};

}  // namespace graw::native
