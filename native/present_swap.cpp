// GRAW native renderer: the guest Swap's front buffer, read for both backends (present.h; the
// rest of present.h is the D3D12 presenter, present.cpp).
#include "present.h"

#include "guest_memory.h"

namespace graw::native {

void Presenter::ReadSwap(const uint8_t* base, const HookArgs& args, SwapCall* out) {
  // The front buffer texture's fetch constant (D3DBaseTexture + 16 in GRAW's XDK; Swap
  // sub_824CC9C8 reads +20..+28).
  out->texture = args.r[1];
  if (out->texture)
    for (int i = 0; i < 6; ++i)
      out->fetch[i] = Load32(base, out->texture + kTextureFetchOffset + 4 * i);
}

}  // namespace graw::native
