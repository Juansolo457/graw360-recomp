# Native renderer (branch `native-renderer`)

A D3D12 renderer for the campaign (`grawyeti`) that draws from the game's own Xbox 360 D3D calls
instead of emulating the Xenos GPU. Why: [RENDERER_DISCOVERY.md](RENDERER_DISCOVERY.md) (the
predicated-tiling replays double the command processor's work). The emulated renderer
(ReXGlue's Xenos plugin) stays in the build as the reference and fallback.

## Status (2026-10-25)

**First runs (autopilot, campaign tutorial, RTX 4090):** it works in both modes.
- Menus and gameplay render: native and Xenos captures of the same frame match in geometry,
  textures, HUD, text and markers (PSNR 25-37 dB; the menus differ only by the animated
  background's frame).
- About 1,000 draws per tutorial frame: 0.7-0.8 ms of recording on the render thread, ~0.2 ms
  on the game thread. No failures; primary mode hands the window to ReXGlue's presenter for the
  system dialogs and takes it back.
- Tracker: 2.2 billion dwords in 3 minutes, 0 unknown opcodes, 1 resync (inside BeginTiling,
  once), 3,041 torn packets (some function stops mid-packet before a hooked call).
- Fixed: lit 3D surfaces were ~6% darker than Xenos, shadowed ones up to 25% (no bluish fill
  light). Found with the resolve oracle (`--graw_native_resolve_dump`,
  `tools/compare_resolves.py`): the scene render itself was dark, and the log named a 256x256
  `k_10_11_11` texture as unsupported, a lighting lookup sampled as black. The texture cache now
  expands `k_10_11_11` / `k_11_11_10` to RGBA16 as ReXGlue does: the dumped frame matches
  Xenos to 99% of pixels exact (final image 67.7 dB, was ~25). For comparisons, run both with
  the game's own texture filtering (`--anisotropic_override=-1`): the SDK forces 4x anisotropy
  on the Xenos side by default, the native renderer uses the game's.
- Open: the torn packets and the BeginTiling jump. The 4x MSAA depth resolve differs from Xenos
  in one column per 40-pixel EDRAM tile (sample choice at tile edges).
- HDR10 output (pgr4-native's default) washed the picture out; GRAW defaults to SDR now
  (`--graw_native_hdr=true` to try HDR).

Run it:

```
pwsh -NoProfile -File tools/launch.ps1 -Console -Native       # in the game window; F9 = Xenos
pwsh -NoProfile -File tools/launch.ps1 -Console -NativeSide   # Xenos draws, native in a 2nd window
```

What to look for in `port/logs/run-*~grawyeti-*.log`: `GRAW native` lines (start-up, failures,
`state: tracking device`, `push cursor ... skipped` = the tracker lost the stream there), and with
`-Extra '--graw_native_stream_log=true'` the tracker's counters. If the native path fails it logs
`frame N failed; native path off` and the Xenos plugin takes over again.

## Design

**Based on pgr4-native.** The renderer modules in `native/` are ported from
[shaunsingh/pgr4-native](https://github.com/shaunsingh/pgr4-native) at `8ea20d5` (MIT; the
Xenia/ReXGlue-derived parts are BSD-3; see [native/README.md](../native/README.md)). Both projects
use ReXGlue `bd833a2`. Draws, textures, shaders, render targets, resolves, pipelines and presenting
are generic Xenos-state-to-D3D12 code, kept as close to upstream as possible so later fixes there
can be taken. Renamed to `graw::native` and `graw_native*` options.

**GRAW's front end: state from the push buffer, not the device layout.** pgr4-native reads each
draw's GPU state from the XDK D3D device's register shadow, which needs the exact internals of
PGR4's 2007 XDK (dirty masks, shader objects at fixed offsets, literal constant tables, the
immediate vertex shader load). GRAW is built with an older XDK (2006) whose device differs in
most offsets. So GRAW's tracker (`native/d3d_state.cpp`) decodes **the packets the D3D wrote into
its push buffer** during each hooked call: type-0/1 register writes, `SET_CONSTANT`,
`LOAD_ALU_CONSTANT`, `IM_LOAD` / `IM_LOAD_IMMEDIATE` and the draw packets. That stream is exactly
what the GPU sees. pgr4-native has the same decoder (its trace "tap") and used it as the reference
that proves its shadow tracker correct. The tile replays happen later on the GPU side and are never
decoded, so each draw is drawn once at full size.

- The D3D completes its packets before it calls another function or returns, so at the entry and
  exit of every hooked call the dwords from the last decoded one to the cursor (`device+0`) are
  whole packets.
- The cursor moves by assignment only in MakeSpace and two segment functions, which are hooked
  and report the jump (nothing in between is decoded). Anything else that moves it backwards or
  more than 4 MB is skipped and logged.
- With predicated tiling the D3D writes the window offset and scissor once per tile; after every
  call they are set from the device's untiled scissor rectangle, as pgr4-native does.
- On attach (first call, or after the renderer switch) the tracker starts from the device's
  register shadow and sets all its dirty bits, so the next draw sends the whole state and both
  shaders again.
- Draw geometry (count, base vertex) comes from the call's arguments, the index buffer from the
  call's first `DRAW_INDX` packet (the device's index buffer object as a fallback).
- BeginVertices only reserves: the D3D publishes its packets, and the game its vertices, at
  EndVertices, where the draw is queued.

pgr4-native's draw replacement (`guest_d3d`, a primary-mode speed-up that skips the D3D's packet
building) is not ported: GRAW's tracker reads those packets. If decoding on the game thread turns
out to be a bottleneck, the shadow tracker can be ported later for the hot paths, with the stream
decoder as its check.

## GRAW's D3D (campaign, `Yeti_Final_Xenon.exe`)

All in [native/graw_d3d.h](../native/graw_d3d.h). Functions (r3 = device):

| Function | Role | Arguments | Found from |
|---|---|---|---|
| `sub_824C4E38` | DrawIndexedVertices | prim r4, base vertex r5 (→ `dev+11540`, VGT_INDX_OFFSET), start index r6, count r7 | index buffer `dev+12532`, predicated `DRAW_INDX` |
| `sub_824C4BF8` | DrawVertices | prim r4, start vertex r5, count r6 | auto-index `DRAW_INDX` |
| `sub_824C4578` | BeginVertices | prim r4, count r5, stride r6 (bytes) → vertex space | fetch slot 31 (`0x48BA`) written inline; cursor kept at `dev+13652` |
| `sub_824C4818` | EndVertices | | `dev+0 = dev+13652` |
| `sub_824C4838` | DrawVerticesUP | prim r4, count r5, data r6, stride r7 | BeginVertices + copy + commit |
| `sub_824C48C0` | BeginIndexedVertices | prim r4, base r5, vertices r6, indices r7, format r8, stride r9 | commit at EndVertices |
| `sub_824CDCC8` | Clear | count r4, rects r5, flags r6, D3DCOLOR r7, Z f1, stencil r9 | calls ClearF per rect |
| `sub_824CD598` | ClearF | flags r4, rect r5, color float4 r6, Z f1, stencil r8 | |
| `sub_824D1960` | Resolve | flags r4, rect r5, texture r6, point r7, level r8, slice r9, color r10, Z f1, stencil `r1+92`, params `r1+100` | source = `dev+12536+4i` / `dev+12552` |
| `sub_824CC9C8` | Swap | front buffer texture r4, params r5 | |
| `sub_824D2868` | BeginTiling | flags r4, count r5, rects r6, color r7, Z f1, stencil r9 | stores the tile list, clears through ClearF |
| `sub_824D2CA0` | EndTiling | flags r4, rects r5, texture r6, color r7, Z f1, stencil r9, params r10 | Resolve per tile |
| `sub_824D7E80` | MakeSpace | | submits the segment (`sub_824D7D40`), next one 32-byte aligned |
| `sub_824D7C08` | wrap to the buffer start | | `sub_824D6F00` sets `dev+0/4/8` |
| `sub_824D6F88` | restore a saved segment | | from `dev+13612..` |
| `sub_824D9E98` | the draw flush | | the dirty masks and shadow blocks below |
| `sub_824D03C0` | SetScissorRect | rect r4 | `dev+12832`, packed at `dev+11524/11528` |
| `sub_824D0FB8` | SetDepthStencilSurface | surface r4 | `dev+12552` |

Device (byte offsets; PGR4's in brackets where known):

| Offset | Contents |
|---|---|
| `+0` / `+4` / `+8` | push cursor (last written dword), segment end, MakeSpace threshold [`+48`] |
| `+16..+48` | five 64-bit dirty masks [six at `+0..+40`]; bits per block in `kDirtyBits` |
| `+1152`, `+1920`, `+6016`, `+10112` | fetch, VS, PS, bool/loop constants [same] |
| `+10384` | pointer to the GPU's ring read pointer write-back |
| `+10432` | flags: `0x10` tiling begun, `0x04` predicated tiling |
| `+11456` | register blocks 0x2000 (`+11532` 0x2100, `+11616` 0x2180, `+11636` 0x2200, `+11684` 0x2280, `+11768` 0x2300, `+11920` 0x2380) [+1,088 from PGR4's] |
| `+11524` / `+11528` | window scissor TL/BR (x \| y << 16) [`+10436/+10440`] |
| `+12532` | index buffer object [`+12428`] |
| `+12536..+12552` | color surfaces 0-3, depth surface [`+12432..+12448`] |
| `+12688` | BeginVertices' stride in dwords |
| `+12808` / `+12832` | viewport (6 dwords) / scissor rectangle |
| `+12996`, `+13000`, `+13240` | tile count, tile rectangles, tile rectangles rounded to 32 px (16 bytes each, 15 tiles) |
| `+13504` / `+13520` / `+13524` | BeginTiling's clear color, Z, stencil |
| `+13652` | BeginVertices' pending cursor |

Objects: the resource header is 12 bytes (PGR4's: 24). Texture fetch constant at `+16` [`+28`];
index buffer format at `+0` (bit 31 = 32-bit), address at `+12` [`+24`]. Resolve's default clear
color is the float at `0x82000744`.

## Build

- `tools/build.ps1 -Project port-yeti` builds `native/` into the campaign when
  `third_party/rexglue-sdk-bd833a2` (the SDK source, local only) is there. It compiles the SDK's
  shader translator and texture tables into the game, against the source tree's headers, which
  need four of pgr4-native's SDK patches, kept in `tools/patches/pgr4-native/`: 0021 and 0022
  (shader translator), 0030 (gamma ramp accessors public), 0031 (`gpu_skip_draws`).
- The renderer plugin for `-Native` must have 0031 (and 0030): the plugin installed in
  `port-yeti/out/.../rexgpu-xenosrd.dll` is `.native1` = the shipped fixes (freed-page resolve,
  vblank pacing) + 0030 + 0031. With `gpu_skip_draws` off (the default) it behaves exactly like
  `.fixed3`. A campaign rebuild copies the stock plugin back: reinstall `.native1`.

## Steps

1. Vendor and build. **Done.**
2. Map GRAW's D3D. **Done** (above).
3. Stream tracker and hooks. **Done, untested.**
4. First run (user): `-NativeSide`, then `-Native`. Expected trouble spots: the window scissor
   under tiling, resolves into the HUD textures (#1, #2), BeginVertices draws, shaders patched in
   place, the gamma ramp.
5. Fix what the run shows; then performance (the CPU time of decoding vs the command
   processor's), resolution scale, and later shadows (#22).
