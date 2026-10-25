# Renderer discovery: a native renderer for the campaign? (branch `renderer-discovery`)

Started 2026-10-25. This is a look at what a custom renderer would take and what it would buy.
It is not a decision. The project's default stays incremental work on the emulated renderer
(measure a bottleneck, fix it, repeat); this branch checks whether a native renderer is the
better long-term path for GRAW.

## Today: the emulated renderer

The guest's Xbox 360 D3D library (the XDK's, linked into the game) builds PM4 packets into a
ring buffer. ReXGlue's Xenos plugin (`rexgpu-xenosrd.dll`, Xenia's D3D12 backend) replays them
on its `GPU Commands` thread, emulating the Xenos GPU and its 10 MB EDRAM. It works, and at 30 fps
with the recent timing fixes it plays well at 4K on PC (see [PERF.md](PERF.md)). But:
- The plaza fight's dips are command-processor bound (`WriteRegister`, `SetupContext`,
  `GetRegisterInfo` top the profile).
- Pipeline compilation stutters on a cold cache (#21).
- Resolution, shadow and LOD work (#22, #23) fights a layer built to emulate 360 limits.

## How PGR4-native does it

[shaunsingh/pgr4-native](https://github.com/shaunsingh/pgr4-native) (MIT; files ported from
Xenia/ReXGlue are BSD-3-Clause) is a ReXGlue recompilation of Project Gotham Racing 4 with its
own D3D12 renderer. Read from its `docs/architecture.md`:
- **It hooks the guest D3D library, not the PM4 stream.** Each hooked function (draws, clear,
  resolve, Swap, state setters) calls the original first, so the guest device stays exactly as
  the game expects, then reads the Xenos register state the D3D left in its device (a register
  shadow at fixed offsets with dirty masks) and records D3D12 work.
- **Draws happen once.** The game uses predicated tiling: the D3D replays each frame's draws
  once per EDRAM tile. At the API each draw arrives once, before tiling, and the native renderer
  draws it once at full size.
- **Two modes.** Beside the Xenos plugin (a debug window; captures compare both front buffers
  by PSNR) or primary (presents in the game window; the Xenos command processor still parses
  the stream for fences, interrupts and occlusion queries but skips draws, via an SDK patch
  `--gpu_skip_draws`). F9 switches renderers live.
- **Threads.** A render thread records D3D12 from captured commands, untile workers convert
  textures, a pipeline thread compiles and prewarms stored pipelines, and a pacer thread paces on
  the display.
- **Shaders.** Xenos ucode goes through ReXGlue's DXBC translator, specialized for bool
  constants.
- **Surface table.** `scripts/analysis/scan_d3d_surface.py` maps the D3D library from the
  generated C++ (packet builders, the device's register-shadow offsets, setters and getters by
  the register they touch) into `src/d3d_surface.inc`.
- **Size.** About 16,000 lines of renderer code (`src/native/`), plus SDK patches.
- **Frame rate.** The repo shows no fps figures. It says the garage "works at high frame rates",
  and motion blur is scaled by frame time so it looks the same as at 30 fps.

UnleashedRecomp and MarathonRecomp replace their guest D3D the same way.

## GRAW's D3D library (campaign, `Yeti_Final_Xenon.exe`)

Mapped with `tools/scan_d3d.py`, which lists the functions that build PM4 type-3 packet
headers, by opcode and predicate bit, and who calls them:

```
python -I tools/scan_d3d.py port-yeti/generated/yeti_final_xenon --lo 0x824C0000 --hi 0x824E2000
```

The library sits at about `0x824C0000`–`0x824E2000` (the lower bound isn't pinned yet).

| Function | What it is | Evidence |
|---|---|---|
| `sub_824C4578`, `sub_824C48C0`, `sub_824C4BF8`, `sub_824C4E38` | The draw entry points (likely DrawVertices / DrawIndexedVertices and variants) | Each builds a predicated `DRAW_INDX` with `SET_BIN_MASK`; ~20 engine call sites in `0x8211xxxx`–`0x821Bxxxx` |
| `sub_824D1960` | Resolve or Clear (a 3-vertex `DRAW_INDX_2` with immediate shader loads, a fence wait) | `IM_LOAD_IMMEDIATE`, predicated `DRAW_INDX_2`, `WAIT_REG_MEM`, `EVENT_WRITE` |
| `sub_824CD598`, `sub_824CD178` | Internal rect draws used by it (and by `sub_824D2868`) | `DRAW_INDX_2` with immediate shaders |
| `sub_824D0A68`, `sub_824DF7B8` | Tiling: set the bin mask and select | `SET_BIN_MASK`, `SET_BIN_SELECT` |
| `sub_824D32B0` | Occlusion query | `EVENT_WRITE_ZPD` |
| `sub_824CC9C8` | Swap | calls `VdGetSystemCommandBuffer` and `VdSwap` |
| `sub_824D8198` | Ring buffer set-up | `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, `ME_INIT` |
| `sub_824DDC38`, `sub_824DE3F0` | Device start-up | `VdInitializeEngines`, `VdSetGraphicsInterruptCallback` |
| `sub_824D7830` / `sub_824E0A00` | Waiting for ring space | the busy-wait fixed on 2026-10-25 ([PERF.md](PERF.md)) |
| `sub_824D7298` | Indirect buffer (the tile replays run through it) | `INDIRECT_BUFFER` |

**GRAW uses predicated tiling.** All four draw functions issue predicated packets. The campaign
renders 1280x720 with 4x MSAA: 3.7 M samples x (4 bytes colour + 4 depth) = ~29 MB, about three
times the 10 MB EDRAM, so roughly three tiles per frame. ReXGlue's command processor runs a
predicated packet in every pass whose bin select matches (`command_processor.cpp`,
`(bin_select_ & bin_mask_) != 0`). So the emulated path probably processes most of the frame's
draws and register writes about three times. That would fit the plaza being command-processor
bound. **Not measured yet:** step 1 below.

## What a native renderer could buy GRAW

- **CPU:** each draw processed once instead of once per tile, and no PM4 parsing or register-file
  emulation (the current hot spots).
- **Resolution and quality:** no EDRAM. Render targets at any size and any MSAA. #22's shadow maps
  become a size change in one place.
- **Pipelines:** a pipeline key built from GRAW's own state, compiled on a worker, cached across
  runs and prewarmed (#21).
- **Frame rate:** a native pacer could later decouple presentation from the game's 30 Hz logic
  (#17). That's separate work and needs the game's timestep found first.

The costs:
- Mostly porting PGR4's modules (MIT; Xenia-derived parts BSD), then adapting them to GRAW's XDK
  version: device offsets and draw state differ between XDK releases (GRAW 2006, PGR4 2007).
- GRAW-specific rendering that must keep working: render-to-texture HUD and video panels
  (#1, #2), resolves into memory the game reuses, occlusion queries, memexport if used.
- Weeks of work before the campaign renders correctly, and the risk of not knowing until late.
  Hence the side-by-side mode first: the Xenos plugin stays the reference.

## Steps (each ends in a measurement)

1. **Measure the tile replay factor.** Count per frame, in the command processor: `SET_BIN_SELECT`
   changes, predicated packets executed vs skipped, and draws. If draws run ~3x, the CPU case for
   an API-level renderer is made. One cheap counter in the renderer plugin.
2. **Name the API surface.** Port PGR4's `scan_d3d_surface.py` idea: find the device's register
   shadow (the type-0 flush sequences in the draw functions) and name the setters and getters by
   register. Compare GRAW's offsets with PGR4's `SHADOW` table to see how far the XDK versions
   differ.
3. **Trace at the API.** Mid-asm hooks (or `REX_HOOK_RAW`) on the four draws, Clear/Resolve and
   Swap. Per frame, count calls and log render targets and shaders. Check against the PM4 counts
   from step 1 (API draws x tiles = PM4 draws).
4. **Side-by-side prototype.** Bring up PGR4's native renderer modules against GRAW's device in a
   debug window beside the Xenos plugin: first clears and the front buffer, then opaque geometry,
   then textures, then resolves. Compare frames with PSNR.
5. **Decide** from steps 1-4 whether to make it primary, and with what fallback (an F9 switch
   back to the Xenos plugin).

Related: [PERF.md](PERF.md), [BORDER.md](BORDER.md) (the resolve-into-freed-memory fix),
[CROSSCOM.md](CROSSCOM.md) (HUD render-to-texture), issues #17, #21, #22, #23.
