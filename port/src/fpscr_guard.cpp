// GRAW 360 - keep floating-point exceptions masked when the game writes the FPSCR.
//
// ReXGlue keeps each context's host MXCSR in a cached value (ctx.fpscr.csr) and writes the whole
// value to the CPU whenever the translated code changes the flush mode (enableFlushMode /
// disableFlushMode around vector code, ~21,000 sites in Yeti) or the rounding mode (mtfsf ->
// storeFromGuest). Guest threads seed it with InitHost(): every exception masked. The audio
// worker's context, which runs the game's mixer callback, is not seeded, so its cached value is 0
// and the mixer's first vector op writes MXCSR with every FP exception unmasked. The campaign
// (Yeti) then died on the next inexact result (C000008F, MXCSR 0x0020; docs/FINDINGS.md, "The
// campaign's FP exception").
//
// GrawGuardFpscr runs at the entry of each program's audio callback and just before each mtfsf
// ([[entrypoint.midasm_hook]] in each manifest) and seeds the context the way InitHost does:
// the current MXCSR with every exception masked.

#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

void GrawGuardFpscr() {
  PPCContext* ctx = rex::runtime::current_ppc_context();
  if (!ctx) return;
  ctx->fpscr.InitHost();
}
