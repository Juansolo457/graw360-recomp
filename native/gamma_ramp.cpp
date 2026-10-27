// GRAW native renderer: the display controller's gamma ramp (gamma_ramp.h).
#include "gamma_ramp.h"

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include <algorithm>
#include <cstring>

namespace graw::native {

static_assert(sizeof(rex::graphics::reg::DC_LUT_PWL_DATA) == 4 &&
              sizeof(rex::graphics::reg::DC_LUT_30_COLOR) == 4);

const rex::graphics::CommandProcessor* GuestCommandProcessor() {
  rex::Runtime* runtime = rex::Runtime::instance();
  // The Xenos plugin's graphics system is the only IGraphicsSystem this title runs with.
  auto* graphics =
      runtime ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system()) : nullptr;
  return graphics ? graphics->command_processor() : nullptr;
}

double PwlOut(const uint32_t* words, uint32_t channel, uint32_t x) {
  const uint32_t w = words[(x >> 3) * 3 + channel];
  return std::min(1.0, ((w & 0xFFFFu) + (x & 7u) * (w >> 16) / 8.0) / 65472.0);
}

bool GammaRamp::Read(RampMode mode, uint64_t frame, uint32_t* out) {
  if (!command_processor_ && !(command_processor_ = GuestCommandProcessor())) return false;
  const uint32_t* words =
      mode == kRampPwl
          ? reinterpret_cast<const uint32_t*>(command_processor_->gamma_ramp_pwl_rgb())
          : reinterpret_cast<const uint32_t*>(command_processor_->gamma_ramp_256_entry_table());
  const uint32_t count = mode == kRampPwl ? kPwlWords : kTableWords;
  // The command processor's thread writes the ramp as it executes the guest's register writes;
  // a ramp read during an update shows for one frame, the next frame reads the finished one.
  uint32_t ramp[kPwlWords];
  for (uint32_t i = 0; i < count; ++i) ramp[i] = __atomic_load_n(words + i, __ATOMIC_RELAXED);
  std::memcpy(out, ramp, count * 4);
  if (mode != last_mode_ || std::memcmp(ramp, last_, count * 4)) {
    last_mode_ = mode;
    std::memcpy(last_, ramp, count * 4);
    for (uint32_t c = 0; c < 3; ++c)
      white_[c] = mode == kRampPwl ? float(PwlOut(ramp, c, 1023))
                                   : float((ramp[255] >> (20 - 10 * c)) & 1023) / 1023.0f;
    if (changes_logged_ < 8) {
      ++changes_logged_;
      if (mode == kRampPwl)
        REXLOG_INFO(
            "GRAW native present: PWL gamma ramp at frame {}: 0.25/0.5/0.75 -> R {:.3f} "
            "{:.3f} {:.3f}, G {:.3f} {:.3f} {:.3f}, B {:.3f} {:.3f} {:.3f}",
            frame, PwlOut(ramp, 0, 256), PwlOut(ramp, 0, 512), PwlOut(ramp, 0, 767),
            PwlOut(ramp, 1, 256), PwlOut(ramp, 1, 512), PwlOut(ramp, 1, 767), PwlOut(ramp, 2, 256),
            PwlOut(ramp, 2, 512), PwlOut(ramp, 2, 767));
      else
        REXLOG_INFO(
            "GRAW native present: gamma table at frame {}: 64/128/192 -> R {} {} {} (of 1023)",
            frame, (ramp[64] >> 20) & 1023, (ramp[128] >> 20) & 1023, (ramp[192] >> 20) & 1023);
    }
  }
  return true;
}

}  // namespace graw::native
