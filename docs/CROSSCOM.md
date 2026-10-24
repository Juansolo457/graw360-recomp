# Cross-Com and video panels: plan and handoff (#2)

Written 2026-10-24 for the next model picking this up. Read this, then the "Cross-Com" section
of [FINDINGS.md](FINDINGS.md) for the raw evidence and [HANDOFF.md](HANDOFF.md) for building and
running. Issue: [#2](https://github.com/Juansolo457/graw360-recomp/issues/2).

## The bug
In the campaign (`port-yeti`, Yeti_Final_Xenon.exe), every in-game video panel is black:
- the Cross-Com feed, top left of the HUD
- the narration video panel, top right
- the squad portraits and the tutorial clips

Their audio plays. The full-screen menu background video (1280x720, `SP_Background.bik`) plays
correctly. The user ranks this the top bug. The in-game map (#3) is next.

## What is now proven
Each step below was checked, not assumed.

1. **The file is read and decoded.** The `.bik` files open and are read in full, and the audio
   track plays.
2. **The game-side player works.** The probe (`port/src/bink_probe.cpp`, `--graw_debug_bink`,
   run 2026-10-24 00:05) watched the Cross-Com video object at `0x420E39E0`:
   - Bink object `0x42A92180`, 160x120, 584 frames.
   - The frame number advances at about 20 fps: frame 40 at +2 s, 441 at +22 s.
   - The "has a frame" byte (+24) is set, BinkWait and the ready path behave normally.
   - Every one of the 82-312 draws per 2 s found a frame and took the draw path.

   So the game calls the Bink draw (`sub_82340EE8`) every frame with a valid frame. The menu
   video object (`0x40BE9FC0`, 1280x720) looks the same, and that one is visible.
3. **The video shader is right.** Bink's three-plane YUV pixel shader is compiled at runtime by
   the game's own (recompiled) D3DX compiler. Its stored microcode (shader `E4F08314542740C2`,
   `tools/xsh_dump.py`) matches the HLSL exactly.
4. **The planes reach the GPU.** fear-nx's renderer (an experiment, see below) logged the 160x120
   and 80x64 `k_8` planes being uploaded several times a second.
5. **It is not this renderer's alone.** fear-nx's native Vulkan renderer was also black, and broke
   the rest of the game. That build (`port-yeti-nx`) stays local and is not the way forward.

**Conclusion:** the frame is drawn, but into something the HUD never shows. The difference from
the working menu video is where the picture goes.
- The menu video is a full-screen draw. It most likely goes straight to the frame (not yet
  checked).
- The Cross-Com video gets its own 160x120 engine texture at creation (video+68, below). The
  Bink draw most likely renders into it, and the HUD samples it.

## How the game renders a video panel
All addresses are in Yeti_Final_Xenon.exe. The decrypted image is `private/xex/yeti.bin`, a raw
file whose sections are listed by the PE header. The generated code is in
`port-yeti/generated/yeti_final_xenon/`, where each PowerPC instruction is a comment above its
C++.

| Address | What it is |
|---|---|
| `sub_821720C8` | creates a video panel; calls the factory below |
| `sub_82171A70` | factory: allocates the 76-byte video object, runs the base constructor `sub_82340158`, sets the vtable `0x8209A5CC` (`[0x82340028, 0x823400A0, ...]`, next to the string `D:\video\`), allocates a 184-byte Bink holder at +72 and calls `sub_8233FDA0` |
| `sub_8233FDA0` | opens the Bink file (`BinkOpen` and friends at `0x825B6D98`-`0x825B70F8`), creates the plane textures (`sub_82340D90`). **Then it creates the panel's output texture**: `sub_82194720(164, 0)` allocates a 164-byte engine texture object, and `sub_821947D8(tex, w, h, 4, 0, 1, 1, 0)` makes it the video's size (160x120 for Cross-Com) with engine format 4. It is stored at video+68 |
| `sub_82340648` | per-tick update: BinkDoFrame `0x825B8CE0`, BinkNextFrame `0x825B9280`, BinkWait `0x825B8348`, BinkGetRealtime `0x825B86E8`; sets +24 ("has a frame") |
| `sub_823400A0` | vtable slot 1, the draw: if +24, call `sub_82340EE8`; else `Clear` (`0x824CDCC8`, flags 15) to black |
| `sub_82340EE8` | Bink's own draw: samplers 0-2 = the planes, `SetPixelShaderConstantF(0, 0x825A9120, 4)` (the YUV matrix, c3.w = 1.0), vertex declaration `0x102`, shaders from `0x828F18BC` (vertex) and `0x828F18B4` (pixel), and `DrawPrimitiveUP` of a 4-vertex strip in pixels `(0,0)-(w,h)`. Device methods at +404 are called with 0 before and 1 after (probably the viewport transform off and back on) |
| `sub_823409C8` | start-up: compiles Bink's three shaders from HLSL text at `0x82041CD0` / `0x82041EF8` / `0x82042318` |
| `*0x828F18B0` | the D3D device. Its vtable-style function pointers sit at offsets 136-504 |

### The render-to-texture path (found 2026-10-24, static, see FINDINGS.md for the evidence)
| Address | What it is |
|---|---|
| `sub_821D6838` | the per-frame video pass (function-pointer table entry `0x820A0B60`): walks the list at `0x820F13F0` and calls `sub_821BB8F0` per node |
| `sub_821BB8F0(node)` | **renders one video into its texture.** node+12 = video, `tex = video+68`. Push RT `sub_8211E750(renderer, tex+40, 0, 0)` → `video->vtable[1](video, tex)` (`sub_823400A0`) → resolve `sub_82194CC0(tex, 1, 0, 1)` → pop `sub_8211E7E8(renderer)` |
| `sub_8211E870(renderer, rt)` | binds a render-target descriptor: D3D `SetRenderTarget(dev, 0, rt+0->+4)` = `0x824D1188`, `SetDepthStencilSurface(dev, rt+4->+4)` = `0x824D0FB8`, viewport from rt+172 (`sub_82113880`). The RT stack is at renderer+15052 |
| `sub_82194CC0` → `sub_82114588` | the resolve: `D3DDevice_Resolve` = **`sub_824D2CA0(dev, 0x100, NULL, D3Dtex, NULL, 0, 0, clear, 0.0, 0, 0)`** at `0x821146DC`; the per-rect worker is `sub_824D1960` |
| `sub_821947D8` | engine format 4 = `D3DFMT_X8R8G8B8` `0x28280186` (tiled); args `(tex, w, h, fmt, depthFmt=0, ?, 1, mode=0)`. Every texture it makes gets EDRAM surfaces at tex+40 (180-byte descriptor), sized up to 32: 160x128 for 160x120 |
| engine texture | 52 bytes: +4 D3DTexture (fetch constant at D3DTexture+28), +12/+14 w/h, +33 cube flag, +40 RT descriptor (rt+0 color surface, rt+4 depth, rt+8 mode, rt+172 viewport rect) |
| `sub_821BBAB0` | HUD side: stores video+68 into a material texture slot (slot+12), so the HUD samples it through the generic material path |

So the whole chain exists and runs: bind the 160x128 EDRAM target, Bink draws a pixel-space
quad into it, resolve to the tiled X8R8G8B8 texture, restore, and the HUD samples that texture.
No step is video-specific. What is not known is which step the renderer loses.

## Plan
Work in order. Every step that builds or opens a game window needs the user's OK first (see the
rules below).

### 1. Static: find the render-to-texture path (done 2026-10-24)
Done, see the table above and FINDINGS.md. Not done statically: the exact HUD draw call that
samples the texture (the probe finds it by address).

### 2. Probe: what the renderer sees (one build, ask first)
Add midasm hooks, as in `bink_probe.cpp` (port-yeti manifest, off unless `--graw_debug_rtt`
or similar is set). Hook addresses and the registers live there:

| Hook at | Where | Registers | Log |
|---|---|---|---|
| `0x821BB988` | `sub_821BB8F0`, after the push, before the virtual draw | r29 = node, r31 = tex | tex, tex+4 (D3DTexture), tex+12/+14, tex+40 (rt), rt+0/+4 (surfaces; their +4 = D3D surface), rt+8, rt+172..178. Only for tex == the Cross-Com's video+68 (video `0x420E39E0` last run; read it from node+12 → +68 rather than hard-coding) |
| `0x8211E8A8` | `sub_8211E870`, the `bl` to `SetRenderTarget` | r3 = device, r5 = D3D color surface | the surface's header dwords (dump 40 bytes; the EDRAM colour info sits after the 20-byte D3DResource header: base tile, pitch, format, MSAA) |
| `0x821146DC` | `sub_82114588`, the `bl` to `D3DDevice_Resolve` | r3 = device, r4 = flags, r6 = D3DTexture | flags, and the 6-dword fetch constant at D3DTexture+28: base address, pitch, size, format, endian, tiled. Also log at the function's `blr` to prove it returned |
| `0x821BB9B4` | `sub_821BB8F0`, after the resolve | r31 = tex | a per-second count; pairs each draw with its resolve |
| the HUD fetch | at a D3D `SetTexture` in the HUD draw, found by matching the D3DTexture pointer logged above (identify the D3D `SetTexture` from the vtable-style device pointers at offsets 136-504 used by `sub_82340EE8`, or by xref of the material slot `sub_821BBAB0` writes) | | the sampler index, the fetch constant, the draw that follows |

Decode the fetch constant with the Xenos definitions in the SDK headers
(`xenos.h`, `GPUTEXTURE_FETCH_CONSTANT`) rather than by hand. A hook that reads guest memory
must check the page is committed (`shader_debug.cpp`, `Readable`).

With the probe lines in `port/logs/run-*~grawyeti-*.log`, compare against the renderer's own
trace (`--log_level trace` shows resolves and texture uploads with addresses). Either:
- the resolve never happens or targets another address (a game-side cause: a missed function,
  wrong switch, or an alias the translation got wrong); or
- the resolve happens but the texture cache never picks it up for the HUD's fetch (a renderer
  cause: the texture cache keyed on a different address, format or tiling).

### 3. Renderer side, if step 2 points there
The stock plugin is a prebuilt DLL. For source-level logging inside it, the local `port-yeti-nx`
build links the same Xenos plugin from source: run it with `--nx_renderer xenos`. Note its SDK
copy (`third_party/rexglue-sdk-nx`, ReXGlue c94f5eb plus fear-nx patches) is older than the
stock nightly, so first confirm the bug reproduces there.
- **Where to look:** the D3D12 render-target cache's resolve (`src/graphics/d3d12/`,
  `render_target_cache`), the texture cache's invalidation (`pipeline/texture/cache.cpp`), and
  `shared_memory.cpp`'s watch and invalidation.
- **Candidates:**
  - Small resolves (160x120) whose destination is in a page range the texture cache already
    holds as a CPU-written texture.
  - Resolves to a format or endianness the HUD fetch reads differently.
  - Resolves skipped because the render target was never "used" in the renderer's view, for
    example after a `DrawPrimitiveUP` with the viewport transform off.
  - The EDRAM surface is 160x128 but the resolve and the viewport are 160x120; a scaled
    (2x) resolve of a non-tile-aligned height may be clipped or mis-sized. `-Scale 1` tests it.
  - The resolve flag `0x100` (clear the render target after resolve) with a 0.0 clear value:
    if the renderer clears before it copies, the texture is black every frame.
- **Cheap A/B runs** (the user runs them, no build), front end with `-Extra`:
  - `--readback_resolve` was already tried and didn't help.
  - Still to try: `--render_target_path_d3d12 rov`, `--gamma_render_target_as_unorm16=false`,
    and resolution scale 1 (`-Scale 1`). A scaled-resolve bug would show at 2x only.

### 4. Fix, then check the related bugs
The same render-to-texture path may explain other campaign bugs, so check them after the fix:
- the HUD border noise (#1)
- the empty tactical map (#3, a rendered texture too)
- the black screen under fire (#13, a full-screen effect)

## Hand-off for the implementing model (2026-10-24)
Step 1 is done and no code was changed. Your job is steps 2 to 4. In order:
1. Write `port/src/rtt_probe.cpp` with the five hooks in the step 2 table (copy the pattern and
   the manifest wiring of `bink_probe.cpp`; one cvar, off by default). Restrict the per-draw
   logging to the Cross-Com texture, and print the fetch constant decoded.
2. Ask the user before building (`tools/build.ps1 -Project port-yeti`) and give them the launch
   line (`tools/launch.ps1 -Windowed "-Extra:--graw_debug_rtt=true"`); they test. Ask for the
   log file and read it yourself.
3. Decide game side or renderer side from the log:
   - no `SetRenderTarget`/`Resolve` lines for the texture: a game-side miss (re-check the
     function list for `sub_821D6838` and its table, and the switch tables).
   - lines present and sane: renderer side. Try the cheap A/B runs first (`-Scale 1`,
     `--render_target_path_d3d12=rov`, `--gamma_render_target_as_unorm16=false`), then step 3
     with the source-level `port-yeti-nx --nx_renderer xenos` build.
4. Whatever the fix is, check #3 (map) and #13 (black under fire) afterwards: they most likely
   use the same `sub_8211E750` / `sub_82194CC0` path (`sub_8214DED8`, `sub_821A4040`,
   `sub_821A5110` are its other users).
Record every result in FINDINGS.md and the state in HANDOFF.md before ending the session.

## Tools added for this
- `port/src/bink_probe.cpp`: the video-object probe (`--graw_debug_bink=true`). Six hooks in
  `port-yeti/grawyeti_manifest.toml`, under "Bink video probe".
- `tools/xsh_dump.py`: decodes the renderer's shader storage
  (`port/cache/shaders/shareable/555307D5.xsh`; only readable while the game is closed).
  `--dump_shaders=<dir>` also works and writes every shader's microcode and disassembly: it
  needs the `=`. A bare `--dump_shaders <dir>` reads as `true`, so the 2026-10-23 run wrote to
  `port/true/` (2,288 files, ignored, still there).
- `tools/check_switch_tables.py private/xex/yeti.bin port-yeti/generated/yeti_final_xenon`:
  rerun after every codegen. It reported 388 switches, 0 wrong, on 2026-10-24.

## Running the probe again
The game window opens on the user's screen. Ask first.

```
pwsh -NoProfile -File tools/build.ps1 -Project port-yeti
pwsh -NoProfile -File tools/launch.ps1 -Windowed "-Extra:--graw_debug_bink=true"
```
- `-Extra` needs the colon form, because PowerShell takes a value that starts with `--` as a
  parameter.
- The probe lines ("GRAW bink ...") land in the campaign's log,
  `port/logs/run-<time>~grawyeti-<time>.log`.

## Rules from the user (they apply to every session)
- **Builds and windows:** announce and wait for an OK before any build or anything that opens a
  game window. The user is often debugging another game in another session.
- **Their game:** never close or kill the user's running game.
- **Testing:** the user prefers to test. Hand over the launch command, or launch it when they
  say so.
- **Commits:** as `juansolo457 <5344231+Juansolo457@users.noreply.github.com>`. The user's
  personal email must never reach a commit.
- **Game data:** never commit it (ISOs, XEX/EXE, extracted assets, decrypted images, captures,
  generated translations, logs, saves). Releases are source-only.
- **Shared SDK:** don't write into fear360-recomp's files. GRAW's patched SDK is its own copy:
  a junction once made a GRAW build overwrite fear360's libraries.
- **`.recomp.json`:** the user maintains it.

## Step 2 probe: written 2026-10-24 (port/src/rtt_probe.cpp)
`--graw_debug_rtt=true`, seven hooks in `port-yeti/grawyeti_manifest.toml` under
"Render-to-texture probe". Built from the plan above, with these corrections from checking each
address against the generated code:
- **tex+40 is a pointer** to the render-target descriptor (`lwz r4,40(r31)` before the push),
  not the descriptor itself.
- **The Begin hook is at `0x821BB954`**, right after the gate passes and r31 = video+68 is
  loaded, so the push's `SetRenderTarget` falls inside the pass. `0x821BB988` comes after the
  push. The gate (`lbz r9,77(r10)` at `0x821BB944`) can skip the whole pass: the probe counts
  entries against passes.
- **The resolve has two branches.** Mode 1 render targets use the per-rect `sub_824D1960`
  (bl at `0x821146A8`). The video's mode 0 uses `D3DDevice_Resolve` (bl at `0x821146DC`). Both
  are hooked.
- **The HUD fetch:** `0x824C6F88` is D3D `SetTexture(device, sampler, texture)`. The Bink draw
  binds its planes with it, and the slot is device+(3176+sampler)*4. The probe hooks its entry
  and counts binds of each video's resolved D3DTexture outside the video pass. If the count is
  0, the HUD never samples the texture through this call. That would itself be a finding (an
  inlined fetch-constant write, or a game-side miss).

What it logs, in the campaign's log (`GRAW rtt ...`):
- For the first three passes of each video texture:
  - the engine texture, its D3DTexture and the decoded fetch constant (base, pitch, tiled,
    format, endian, size, exp_adjust);
  - the descriptor's colour/depth surfaces, mode and viewport, plus 16 raw dwords of the D3D
    colour surface;
  - the `SetRenderTarget` surface, and the resolve's flags and destination, with a check that
    the destination is the engine texture's own.
- The first two `SetTexture` binds of each result.
- Every 2 s: passes, SetRenderTargets, resolves, ends and HUD binds per texture.

## Step 2 result (run 2026-10-24 06:13; Cross-Com still black, as expected for a probe)
- **The menu background video uses the same render-to-texture path and is visible.** Engine
  texture `0x409B43B0`, 1280x720, D3D `0x409B44D0`, render-target descriptor mode **1**,
  colour surface `0x5D4E4680`.
- **The Cross-Com uses that path the same way.** Engine texture `0x420F2640`, 160x120, D3D
  `0x429C3650`, mode 1, colour surface `0x5EAED8D0`, viewport 0,0 160x120.
- **For both, per pass:**
  - one `SetRenderTarget` of their own surface;
  - one **per-rect** resolve (`sub_824D1960`; mode 1, so not `D3DDevice_Resolve`, and Fable's
    "mode 0" was wrong);
  - one end;
  - about one HUD `SetTexture` of the result, always on sampler 0.

  The `+77` gate skips about one pass per 2 s.
- So the game side is the same for a working and a broken panel. It is not a missing resolve
  and not a missing bind. What is left:
  1. **The HUD draw for the Cross-Com box itself.** Its pixel shader, its other samplers and its
     blend state may differ from the full-screen video's. A second texture that is missing or
     invalid (#6: 213 "invalid texture fetch constant" warnings) would make it black.
  2. **The size:** 160x120 is not a multiple of 32 (the EDRAM surface is 160x128), and the
     menu's 1280x720 is. A renderer bug in small or unaligned resolves would hit only the panels.
- **Probe bug:** the fetch-constant decode is wrong. D3DTexture+28 is not the fetch constant in
  this XDK (2.0.2417). The texture and the colour surface both read `00001414 00000000
  00000200` there, a shared header field. Find the real offset before decoding.
  - Raw colour surface: `01040001 <seq> FFFF0000 00000000 80000000 00000086 000FE09F 00001414
    00000000 00000200 00014000 00000000 028000A0 000002E0`. Dword 12 packs height-1 and
    width-1? (`028000A0`: 0x280 = 640 and 0xA0 = 160; the menu's is `14000500`.)
  - D3D textures: menu `[00001414 00000000 00000200 AE000D02 B0000E02 B0000D02]`, Cross-Com
    `[00001414 00000000 00000200 5E68D8A0 00000010 82658488]` from +28.

## Next probe (step 2b)
At the HUD's `SetTexture` of a video texture, keep a flag, then at the next draw (the D3D
`DrawPrimitive`/`DrawIndexedPrimitive` the HUD uses, to be found) log:
- the bound pixel shader;
- every sampler's texture (device+(3176+i)*4, i = 0..15);
- the blend and alpha-test state.

Compare that between the menu video's draw and the Cross-Com's draw. Also dump the D3DTexture's
first 64 bytes, to fix the fetch-constant offset.

## Canary comparison history (2026-10-24)

**Final result: Cross-Com works in Canary 9e03494 with depth clamp ON and OFF.**
The OFF run also has a correct-looking crosshair and tactical map per the user. No recomp
fix has been made. The preparation/pending entries below are historical; see the last A/B
entry and HANDOFF.md for the current state.

### Preparation (before tests)
- User approved downloading Canary, not launching it. Downloaded official release
  `9e03494`: https://github.com/xenia-canary/xenia-canary/releases/tag/9e03494.
- Windows archive SHA-256 verified against GitHub's asset digest:
  `6b8dce0dada1948b6b053eb43c9ead033e560735f224661f7e9ff3bc4e58d2f0`.
- Extracted under ignored `private/xenia-canary/9e03494/`; portable.txt and an explicit
  `graw-comparison.toml` override set `[GPU] force_depth_clamp = true`.
- No emulator launched and no build run. User tests their ISO from
  `D:\x360-recomps\GRAW 1\` through the front end to the first campaign Cross-Com clip.
- Pending: compare Cross-Com visibility (also note HUD border and tactical map).
  If visible, compare Canary GPU code with the local patched SDK; otherwise finish the HUD
  draw probe. No conclusion about Canary rendering yet.

### Canary first run: campaign handoff failed (2026-10-24)
User authorized launch. Front end ran, but selecting Campaign closed Canary. Log loads only
`GAME:\default.xex`; no Yeti module load is recorded. Its tail repeats
`Execute(82216540): failed to find function` and also reports `Execute(82593CA0)` failures.
Both addresses exist in the front-end translation. This suggests a failure during the
handoff/teardown, but the log does not establish its exact cause. No Cross-Com comparison
was reached. Config still confirms `force_depth_clamp = true`.
Preserved log and 134-byte launch_data.bin under the ignored Canary directory as
`frontend-handoff-failed-20261024.log` and `frontend-launch-data-20261024.bin`.
Proposed next test, pending user launch approval: same ISO and config with
`--launch_module=Yeti_Final_Xenon.exe` to bypass the front-end handoff. Canary's release source
and generated config document this option. Direct campaign startup is not yet tested.

### Canary campaign comparison: Cross-Com WORKS (2026-10-24)
- Direct ISO launch with `--launch_module=Yeti_Final_Xenon.exe` reached campaign.
  User confirms Cross-Com video displays and the recomp's UI problems appear absent.
  User also sees twitching/missing environment textures in Canary that are absent in the
  recomp. This is a reference for specific passes, not evidence to replace the whole renderer.
- Log confirms Yeti module load, RTX 4090, resolution scale 1x1, and
  `force_depth_clamp = true`. It also contains invalid GPU upload ranges and failed resolve
  destination allocations; those messages alone do not identify the Cross-Com cause.
- Preserved log/config privately as `campaign-crosscom-working-20261024.log/.toml` in
  `private/xenia-canary/9e03494/`. User's visual observation is the evidence of video visibility.
- First concrete source difference: Canary 9e03494
  `src/xenia/gpu/d3d12/pipeline_cache.cc` sets
  `depth_clip = !clip_disable && !cvars::force_depth_clamp`; SDK 11c330cf
  `src/graphics/d3d12/pipeline_cache.cpp:1431` only sets `depth_clip = !clip_disable`.
  SDK source has no force_depth_clamp option. Canary's comment describes passes losing alpha
  inputs to near-plane clipping in other titles; it is not proof of this game's root cause.
- Prepared `graw-depth-clamp-off.toml`, identical to the working config except this option is
  false. Pending user approval: launch the same direct campaign with this config. If Cross-Com
  goes black, backport the opt-in D3D12 clamp behavior and test in the source-built Xenos path.
  If it stays visible, continue shader/texture/resolve comparison and HUD draw probe.
- #1/#3/#13 are not individually verified as fixed; no recomp fix has been tested.

### Lost Odyssey comparison method reference
User suggested the recent Lost Odyssey recomp. Reviewed freefrank/LostOdysseyRecomp:
https://github.com/freefrank/LostOdysseyRecomp/blob/main/docs/archive/notes/xenia-render-comparison.md
and docs/notes/rendering-validation.md. Its historical comparison aligned scenes with Xenia,
kept original-resolution reference captures, distinguished animation/camera differences, and
isolated indexing, resolve stride, and material gamma/channel issues. These are methodological
references, not evidence that Lost Odyssey's specific fixes apply to GRAW. No code copied.

### Canary clamp-off A/B: video still works (2026-10-24)
User confirms Cross-Com video displays with force_depth_clamp=false. Also reports crosshair
and tactical map look fixed and the run is smooth. This was Canary, not the recompilation.
Log/config preserved privately as campaign-clamp-off-working-20261024.log/.toml.
Depth clamp is therefore not required for the working Cross-Com in this Canary test; do not
backport it as the presumed #2 fix. Continue comparison of Canary GPU and SDK texture/resolve/
HUD state paths. User said crosshair, not specifically the HUD border, so #1 remains unverified;
#13 was not tested. Recomp fixes remain unimplemented/unverified.

## Canary vs ReXGlue: fork point and bisect (2026-10-24)
- Sources, both in ignored `third_party/`:
  - `xenia-canary/` at 9e03494, with history but no file blobs (`--filter=blob:none`);
  - `rexglue-sdk-bd833a2/`, the exact source of the stock nightly the play build uses.
    `rexglue-sdk-nx/` is an older, patched copy, so don't diff against it.
- **The fork point is around Canary cec9ca0e, 2026-01-17.**
  - Every historical version of `texture_util.cc`, `texture_info.cc` and
    `render_target_cache.cc` was scored against ReXGlue's copies after normalising names and
    formatting. The closest match is cec9ca0e (2026-01-17), then 28b69c21 (2026-01-06).
  - There are 346 Canary GPU commits between cec9ca0e and 9e03494. Many touch resolves, EDRAM,
    tiling and texture invalidation, for example:
    - e1f4a0d0, tiling window offsets in EDRAM bases;
    - 9781a75a, stale texture state after invalidation;
    - 9da69348, EDRAM color/depth aliases;
    - aed81ca9, guest access and unwatched invalidation;
    - 437a7280, single-sample EDRAM addressing.
  - The normalised diffs of the main GPU files run from 100 to 2,700 changed statements each,
    too many to read blind.
- **The bisect.** Test whether the Cross-Com works in Canary 263e91b (2026-01-20, the first
  release after the fork point), unpacked in `private/xenia-canary/263e91b/` with the same
  `graw-depth-clamp-off.toml`:
  - **It works:** the bug is something ReXGlue changed in its copy, or a host difference such
    as the plugin interface. Diff ReXGlue against cec9ca0e, not against 9e03494.
  - **It's black:** a Canary commit between 2026-01-20 and 2026-10-23 fixed it. Bisect the
    release builds (a list is in the ignored scratch notes; older ones are in
    `xenia-canary/xenia-canary-releases`, newer ones in `xenia-canary/xenia-canary` releases).
    About 8 runs pin one commit, which can then be ported.

## Bisect result: January Canary works, so ReXGlue broke it (2026-10-24)
- **Canary 263e91b (2026-01-20, just after the fork point) shows the Cross-Com and the map**
  (user's run, clamp off). So the fault came in with ReXGlue's changes to its copy. Later Canary
  fixes are not the answer.
- ReXGlue's first commit is 96bf707, 2026-01-22. Since then, 60 of its commits touch
  `src/graphics`. Suspects, in order:
  1. **`clear_memory_page_state`.** ReXGlue's default is **true**, Canary's is **false** (an
     opt-in "Team Ninja" hack there). When true, each frame end runs
     `SetSystemPageBlocksValidWithGpuDataWritten`: pages valid only from CPU uploads lose
     their valid bit, and only GPU-written pages stay valid. 6124b36 (2026-08-13) rewrote that
     refresh. It decides which copy of a texture the HUD reads.
  2. **Other defaults that differ from Canary:**
     - `execute_unclipped_draw_vs_on_cpu`: false here, true in Canary;
     - `gpu_allow_invalid_fetch_constants`: false here, true in Canary;
     - `readback_memexport`: true here, false in Canary;
     - `primitive_processor_cache_min_indices`: 0 here, 4096 in Canary.

     And ReXGlue-only options: `pre_mask_resolve_l2_block` (6379943/bb4a536, resolve
     pre-masking), plus `mrt_edram_used_range_clamp_to_min`,
     `snorm16_render_target_full_range` and
     `execute_unclipped_draw_vs_on_cpu_for_psi_render_backend`, which 7a2a2fc set to true.
  3. **Not it:** `direct_host_resolve`. `TryResolveCopyDirectly` only runs preflight checks and
     then calls the same `DumpRenderTargets` path Canary uses.
- All of these are options in the stock plugin (checked in `rexgpu-xenosrd.dll`), so they
  can be tested with launch options and **no build**:
  - Run A: `--clear_memory_page_state=false` alone.
  - Run B, if A is black: every Canary-matching value at once, plus `-Scale 1`, as Canary ran
    at 1x.
- **Run A (`--clear_memory_page_state=false`): still black.**
- **Run B (all Canary-matching values above, `-Scale 1`): still black.** So no renderer option
  explains it. The cause is in ReXGlue's code changes since 96bf707. The March ports
  (e6407f3, 38cbd30, 8b2672d, f20b3b2, "xenia-edge") rewrote much of the D3D12 render-target
  and texture caches.
- **Code comparison with Canary 263e91b** (worktree `third_party/xenia-canary-263e91b/`,
  normalised diffs in the session scratchpad):
  - Shared memory: Canary keeps a third page-flag set, "valid and written by a GPU resolve",
    and `RequestRange(..., bool* any_data_resolved_out)`, so each texture records
    `base_resolved` / `mips_resolved`. ReXGlue removed both.
  - Canary uses that only for gamma (sRGB) handling and resolution-scaled sampling offsets.
    On its own it would not black out an X8R8G8B8 texture at 1x.
  - `RangeWrittenByGpu`, the resolve-to-texture-cache notification, is the same in both: fire
    the watches, then mark the pages GPU-written.
  - The remaining differences are in ReXGlue's reworked texture cache (430 + 501 changed
    statements) and D3D12 render-target cache (781). Too much to read blind.
- **Next: an instrumented renderer.** `third_party/rexglue-sdk-bd833a2` is the exact source of
  the prebuilt `rexgpu-xenosrd.dll`.
  - Build that plugin from source with logging keyed to the Cross-Com texture: resolve
    destination and extent, the `RangeWrittenByGpu` watch firing, texture-cache invalidation
    and reload, and the fetch bound for the HUD draw.
  - Drop the DLL in beside `grawyeti.exe` in place of the prebuilt one (same commit, same ABI).
  - Compare with a Canary run whose log shows the same events.
- **Instrumented renderer built** (2026-10-24):
  - The source is `third_party/rexglue-sdk-bd833a2` plus `tools/patches/rexglue-bd833a2-graw-rtt-trace.patch`,
    which adds `include/rex/graphics/graw_trace.h` and hooks the D3D12 resolve and the texture
    cache.
  - Built target `rexgpu-xenos` only: `cmake --preset win-amd64`, then `--build --preset
    win-amd64-relwithdebinfo --target rexgpu-xenos`, with the `.tools` LLVM in a VS DevShell.
  - The submodules' symlinks were checked out as text (core.symlinks=false), so they were
    replaced with copies first.
  - All 107 of its `rexruntimerd.dll` imports exist in the prebuilt runtime.
  - Installed over `port-yeti/out/build/win-amd64-relwithdebinfo/rexgpu-xenosrd.dll`, with the
    original at `rexgpu-xenosrd.dll.stock`. A port-yeti rebuild copies the prebuilt back.
  - Option `--graw_trace_rtt_max_width=<px>`: resolves at most that wide, and texture-cache
    bind/load/invalidate events on their destinations, logged as `GRAW trace ...` with 2 s
    summaries.
- **Trace run 1 (2026-10-24 07:23), Cross-Com still black:** the texture side works.
  - Every frame: `resolve 160x120 to 0FECB000` (dest format 6 = k_8_8_8_8, from EDRAM
    color_base 736, not scaled), then the watch fires (`invalidate 0FECB000 by_gpu`), then
    `load base 0FECB000 160x120`, then the HUD binds `fetch 0 base 0FECB000 160x120 fmt 6`
    (tiled, pitch 160, swizzle ZYX1). All four happen one for one, about 75 a second.
  - So the HUD samples a freshly reloaded copy of a resolve that is itself black. The picture is
    lost before the resolve, in the Bink draw into the 160-wide render target, or in what that
    target holds in EDRAM.
  - Every other panel that is black for the user resolves from the same EDRAM `color_base 736`
    with a narrow pitch: 64x80 at `1F99E000` / `10F5D000`, and 128x80 and 128x760 at
    `1FAEC000` / `1FAF8000`. The visible menu video renders full width (1280).
- **Trace build 2:** `IssueDraw` now logs each draw into a render target no wider than the
  limit: its outcome (no rasterization, no host vertices, render-target update failed, pipeline
  not ready, drawn), plus colour mask, normalised mask, VTE/clip/mode registers, blend, depth,
  window offset, scissors, viewport and vertex count.
- **Trace run 2 (07:27), still black; the user saw the panel flash for about half a second.**
  The Bink draw into the Cross-Com target is issued normally every frame:
  - `draw pitch 160 base 736 fmt 0 (k_8_8_8_8) msaa 0: drawn`, with a pixel shader;
  - `RB_COLOR_MASK` F (normalised F), depth off, alpha test off, blend ONE/ZERO;
  - VTE `0x400` (screen-space XY), clip disabled, window scissor 160x120, 4 vertices.

  So the draw runs and writes black. The suspect is now its input: the Y/Cr/Cb planes the CPU
  rewrites every frame. A half-second flash fits a first upload followed by CPU writes the
  texture cache never picks up.
- **Trace build 3:** traced draws also log the textures they sample, with decoded fetch
  constants, and add their ranges to the texture-cache trace, so CPU-write invalidations and
  reloads of the planes show up.
- **Trace run 3 (07:30), still no video.** The user saw the Cross-Com intro/outro animation (a
  blue flash) for the first time, the HUD border turned noisy again, and animations looked
  slightly fast. The heavier logging changed what reached the screen, which points to a race.
  - The Bink planes are fine. Each video draw samples `k_8`, linear, pitch 256 textures: a
    160x120 Y and two 80x64 chroma planes, rotating through frame buffers at
    `0x10C7C000`-`0x10CAB000`. Every CPU write invalidates them (`invalidate ... by_gpu
    false`), and they reload before the next draw.
  - In the same frame, EDRAM tile 736 is reused by colour targets of pitch 80/128/160 in formats
    0 (8_8_8_8) and 2 (2_10_10_10), and by depth-only passes (`modecontrol 5`, 32x32 shadow
    resolves from `depth_base 736`).
- **Ruled out by reading:** these match January Canary function for function.
  - `RequestTextures` reload logic: `ResetTextureBindings` clears the keys, so outdated
    textures reload.
  - `MarkRangeAsResolved` and `RangeWrittenByGpu`.
  - The accumulated-render-target validity check (a loop condition instead of an `if`, but the
    same effect).
  - Float constant dirty marking: both gate on `frame_open_`.
- **Next, no build needed:** `--async_shader_compilation=false`. The option is ReXGlue-only and
  on by default, and a913a8e moved vertex-shader translation to background threads. Run B
  didn't include it.
- **`--async_shader_compilation=false` (07:34, instrumented DLL, trace off):**
  - The **HUD border draws correctly** (#1).
  - The 3D scene gets a strong **pink/magenta cast**: red and blue kept, green lost.
  - The Cross-Com is still black. No GPU errors in the log.

  With async on, `IssueDraw` skips any draw whose pipeline isn't ready, and some draws evidently
  never become ready: the border is one. The pink cast looks like a full-screen colour pass
  (the campaign has `HDRColorRemap` shaders) that async had been skipping, and that reads a
  wrong texture now it runs. That is the same class of fault as the Cross-Com: a GPU-produced
  texture read by a later pass.
- The stock DLL is back in place (`rexgpu-xenosrd.dll`; the traced build is kept as
  `rexgpu-xenosrd.dll.traced`). Next is a clean A/B: stock DLL with async off, to confirm the
  border fix and the pink cast come from the option and not from the instrumented build.
- **Stock DLL, `--async_shader_compilation=false` (07:37):** the colours are normal, the border
  is back to green static (#1), and the Cross-Com is still black. The instrumented build with
  the same option gave a correct border and a pink scene. Different builds give different
  garbage, so passes that sample resolved textures are reading undefined data.
  - **Hypothesis:** the texture upload (`SharedMemory::RequestRanges`) treats resolved pages
    as invalid and re-uploads them from guest memory over the GPU's resolve result. Guest memory
    is zero there for the Cross-Com (black) and holds stale data for the border (static).
  - The barrier handling (`CommitUAVWritesAndTransitionBuffer`, `UseForReading` in the
    texture load) matches January Canary.
  - **Trace build 4:** logs any `RequestRanges` upload that overlaps a traced resolve
    destination ("UPLOAD from guest memory over traced ...").

## Root cause found: overlapping memory (2026-10-24 07:40 trace)
**The video planes and the video output textures overlap in guest physical memory.** That
can't happen on a console, and it explains black, garbage and run-to-run variation. Two
campaign videos played at once, both into black panels:

| Range | Contents |
|---|---|
| `10C64000`-`10C78000` | video A output texture (160x120 X8R8G8B8 tiled, 0x14000) |
| `10C78000`-`10C98000` | video A Bink planes: Cr `78000`, Cb `7C000`, Y `80000`; second frame `88000`/`8C000`/`90000` (0x20000) |
| `10C7F000`-`10C93000` | **video B output texture**, inside A's planes |
| `10C93000`-`10CB3000` | video B planes: `93000`/`97000`/`9B000`, `A3000`/`A7000`/`AB000`, overlapping A's second frame |

- Video B's texture sits 0x7000 into A's plane block, and B's planes start right after that
  texture. Whatever placed them treated A's block as 0x7000 bytes long, but Bink uses 0x20000
  bytes.
- So every CPU-decoded Bink frame overwrites the other panel's resolved picture. The texture
  cache, correctly, reloads it from the overwritten memory.
- Uploads over resolved ranges happen once each, at first use, so the "re-upload" hypothesis
  was wrong. The overwrites come from the overlap.
- **The renderer is not the cause.** It handles what it's given correctly. The earlier renderer
  findings (async skipping the border, `clear_memory_page_state` and so on) stand on their own.
- `MmAllocatePhysicalMemoryEx` is the same in ReXGlue and Canary. So the under-sized block comes
  from above the kernel: the game's or Bink's own allocation code, or a size calculation, which
  Canary runs through its JIT and we run as recompiled C++. **Suspect: a codegen error in that
  code.**
- **Next:** a run with `-Trace`, where every kernel call logs its arguments (allocation sizes and
  addresses), together with the renderer trace for the texture addresses. Find which
  allocation returned `10C78000` (virtual address = physical + heap base), its requested size,
  and whether the 0x7000 comes from the game's heap or from Bink's size calculation.
- **`-Trace` run (07:44), user's report:** the Cross-Com and top-right panels play their
  intro/outro animation, but the video area stays black. The HUD border is static again. That
  fits the overlap: the frame animation is drawn straight onto the HUD, while the video passes
  through the overlapped textures. The log of this run is being analysed for the allocation
  sizes; see HANDOFF.md.

## Correction: the overlap is not the root cause (2026-10-24 07:44 `-Trace` run)
The kernel trace (`MmAllocatePhysicalMemoryEx` / `MmFreePhysicalMemory`, virtual `0xE0000000`
heap = physical + 0x1000 offset) shows a **clean** lifecycle for the one video that played:

| Time | Event |
|---|---|
| 07:45:17.314 | allocated: texture `10C7F000` (0x14000, protect 0x404 write-combine), planes `10C93000`/`10C97000` (0x4000), `10C9B000` (0x8000), `10CA3000`/`10CA7000` (0x4000), `10CAB000` (0x8000). No overlap |
| 07:45:17-22 | `resolve 160x120 to 10C7F000` and the HUD bind, 60-85 a second; planes sampled from those buffers |
| 07:45:21.883 | all seven blocks freed together; resolves stop |

The user still saw a black panel, with the intro/outro animation playing. So during playback
nothing overlapped. The overlap in the 07:40 run was most likely one video's freed buffers being
reused by the next (two videos, close together), so it's a side effect at most.
**The root cause is open again.** The established facts:
- the game side works: frames decode, the Bink draw is issued with colour writes on;
- every texture-cache step happens: planes reload after CPU writes, the output reloads after
  each resolve, the HUD binds it;
- the stock renderer's options don't fix it, and January Canary (the fork point) shows it.

**Most promising next checks:**
1. **Address mapping of the `0xE0000000` heap.** Planes and texture live there (4 KB pages,
   physical = virtual - 0xE0000000 + 0x1000). If the CPU's writes and the GPU's reads disagree
   by that 0x1000 offset, the GPU reads the wrong page (zeros means black). Compare ReXGlue's
   `memory` heap setup and `TranslatePhysical` with Xenia's for the E0 heap. A quick test:
   during playback, hash the guest bytes at the plane's virtual address, and the bytes the
   texture cache uploads for its physical address.
2. **Look at the pixels.** Dump the resolved 160x120 texture and the Y plane right after
   upload, from the instrumented renderer (read back from shared memory once). That shows
   whether the planes are zero on the GPU side (a mapping or upload problem) or the draw's
   output is black (a shader or constant problem: Bink's c0-c3 at pixel constant 0-3).
3. Or a RenderDoc capture of one frame with the panel up (RenderDoc isn't installed; the user
   has allowed downloads).

## Follow-up: E0 mapping inspection and staging-byte probe (2026-10-24)
- Compared stock SDK bd833a2 with local Canary 263e91b: MapViews maps E0 with backing offset
  0x100001000, rounded down by host allocation granularity; PhysicalHeap adds 0x1000 on Windows.
  TranslateVirtual compensates for that offset; TranslatePhysical uses physical_membase plus
  the masked physical address. The generated grawyeti_pch.h REX_RAW_ADDR and scalar load/store
  macros also add 0x1000 for E0 addresses. No static inconsistency found in these paths.
- This does not prove runtime bytes match, or cover every possible CPU memory access path.
- Extended the existing tracked RTT patch with a D3D12 UploadRanges probe, after memcpy into
  staging and before CopyBufferRegion. Reports first-page FNV-1a hash, nonzero byte count and
  min/max, plus guarded hash of the corresponding TranslateVirtual E0 alias. Sequential reads
  may race CPU writes; a mismatch is diagnostic evidence, not proof by itself.
- Off unless existing RTT trace is enabled; first three samples per tracked physical address.
  This samples CPU staging, NOT GPU resource readback, and produces no pixel dump files.
- Not built or run yet. Need approved plugin-only build and user test before drawing conclusions.
  Actual GPU readback/RenderDoc and Bink pixel constants remain next if upload bytes look valid.

### Staging-byte probe build validation
User approved building and automated testing. Plugin-only build succeeded (existing SDK header
warnings). All 108 runtime imports resolve against the campaign runtime; probe marker is present.
Reverse-apply check confirms the tracked patch matches the SDK changes. Repository audit and
whitespace checks pass. Installed only after confirming no GRAW process running; previous traced
DLL preserved as rexgpu-xenosrd.dll.before-upload-probe and stock backup untouched.
No game launched and no runtime pixel result yet. Test with --graw_trace_rtt_max_width=200.

### Staging-byte result (07:55 run): nonzero bytes, matching aliases
User reports video still black and closed the game. Campaign log:
port/logs/run-20261024-075444~grawyeti-075503.log.
23 upload snapshots and 23 alias comparisons; every alias matched, none unreadable. Chroma
plane starts 0FEE4000/0FEF4000 have 1280 nonzero bytes per sampled 4096, maxima 132-133 and
changing hashes. A sampled page within Y (0FEFD000, Y starts 0FEFC000) has 2560 nonzero bytes,
maximum 237. Samples cover only the first page of each upload chunk, not every entire plane.
The output's initial CPU upload at 0FED0000 is zero; this does NOT show the GPU resolve is zero.
No sampled evidence for the E0 4KB mismatch. GPU texture conversion, shader output and resolve
contents remain unverified.

Prepared a separate opt-in --graw_readback_video=true diagnostic in the same patch. At 1x,
reads back shared GPU memory after 160-pitch RGBA resolves numbered 1, 30 and 90. Logs full
raw-range hash and per-byte-lane nonzero/max counts; includes tiled padding, so nonzero bytes
alone do not prove visible pixels. Uses existing readback allocation/barrier/queue-wait pattern;
never writes readback into guest memory. GPU stalls can change timing. Not built/tested yet.

### GPU resolve readback probe built and installed
User approved build. Plugin-only build passed; all 108 runtime imports available and readback
marker verified. Patch reverse-apply check and repository audit passed. Installed after checking
no GRAW process; prior staging-only DLL saved as rexgpu-xenosrd.dll.before-gpu-readback.
Installed SHA-256: 59DAFD98A62EA039326AD7BF12C2B31D9E32545DCBE13D889FCF728D438AA6FD.
No game launched yet. Next approved test command:
`pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1 "-Extra:--graw_trace_rtt_max_width=200 --graw_readback_video=true"`.

## GPU readback result: resolved data is NOT all black (08:05 run)
User reports Cross-Com still black while it is visible. Log:
port/logs/run-20261024-080432~grawyeti-080447.log. No game was stopped by the agent.
At 1x the three GPU readbacks of output physical 10C7F000 (81920-byte tiled range) show:

| Resolve sample | Hash | Nonzero byte lanes | Maximum byte lanes |
|---|---|---|---|
| 1 | 6C5EB2BF33BE206C | 19200/19200/19200/19199 | 255/242/231/236 |
| 30 | 21F81D6814BBF502 | 19200/19200/19200/19197 | 255/250/229/242 |
| 90 | 66F048943A0314A2 | 19200/19200/19200/19200 | 255/233/225/231 |

160*120 is 19200. Substantial changing data exists in GPU shared memory immediately after
resolve, despite the initial CPU contents being zero. This overturns the earlier inference
that a fresh texture reload necessarily meant the resolve was itself black. These are raw
counts including padding, NOT an inspected/untiled picture, so correct video colours/content
are not yet proven. Readback stalls can affect timing; black remained visible in this run.

The following HUD bind uses the SAME physical 10C7F000, 160x120, format 6, tiled, pitch 160,
scaled 0, swizzle A0A, outdated_mask 1; raw fetch is
8142C802 10C7F086 000EE09F 01281414 00000003 00000210.
Next localize between resolved shared memory and displayed HUD: inspect converted host texture
contents and final HUD pixel shader, samplers, constants, blend state and UVs. Prefer one
RenderDoc frame or bounded post-texture-load readback plus HUD-draw trace. Do not treat bind/
reload logs alone as proof the converted host texture or descriptor is correct.

### Converted host texture inspection prepared and launched (08:08)
User authorized build and launch. Added --graw_readback_texture=true: copies subresource 0 of
160x120 R8G8B8A8 host textures after conversion/copy, on eligible loads 1, 30 and 90. Dedicated
readback buffers are retained until GetCompletedSubmission reaches their submission; no wait
or submission reset inside the draw. Excludes row padding from RGBA counts/hash, writes RGB PPM
images to local logs/graw-converted-<physical>-<sample>.ppm (before SRV swizzle, alpha counted
but not stored in PPM). Only maps completed buffers; restores original texture resource state.
Build and 108 runtime-import checks passed; patch reverse-apply and repository audit passed.
Previous DLL backed up as rexgpu-xenosrd.dll.before-texture-readback. Launched at 1x with trace
and converted-texture readback, WITHOUT the synchronous resolve readback option. Front-end log:
port/logs/run-20261024-080834.log. Awaiting campaign clip, then inspect PPMs and matching log.

## Converted texture confirmed: recognizable Cross-Com image (08:18 run)
User reports Cross-Com still invisible/black on screen. Campaign log:
port/logs/run-20261024-080834~grawyeti-081749.log.
Converted resource at physical 10C7F000 is DXGI format 27 (R8G8B8A8_TYPELESS).
Sample 1 is all zero, but samples 30 and 90 contain changing, recognizable Cross-Com character
images (face, goggles, headgear and background). Both PPMs were decoded and visually inspected.
Sample 30 RGBA nonzero counts: 19197/19200/19200/19200; sample 90: 19200 in every channel.
Local captures: port/logs/graw-converted-10C7F000-{1,30,90}.ppm; inspection PNGs alongside.
These files are private/ignored and must never be committed.

This is stronger than raw resolve statistics: the actual converted host texture contains the
video. Exact colour/channel correctness is not yet established (capture is before SRV swizzle),
and the first sampled load is black, but later loads are clearly populated. The probe is
asynchronous and does not flush submissions inside a draw. User still sees a black panel.

Next investigate final HUD consumption: SRV component mapping and descriptor binding, the actual
HUD draw's PS/hash and all sampler bindings, constants, blend/alpha/depth state and UVs. Compare
the working menu video with Cross-Com. Do not restart decoding/allocation/resolve speculation
without new contradictory evidence. No fix identified; game left untouched.

### Final HUD-draw probe built/launched (08:30; result pending)
--graw_trace_hud=true records candidate final draws using format-6 RGBA textures at 160x120
or 1280x720, limited to three per PS hash/base/size. Trigger is a used sampler, not the old
incorrect D3DTexture header decode. Logged immediately before draw emission after binding
updates: VS/PS microcode hashes, used sampler mask, bindless mode, PS constant-buffer address,
all first 16 fetch slots plus any higher used slots, cached host resource pointers/descriptor
indices and swizzles, used pixel float constants and blend/depth/scissor/stencil state.
It captures successful draw paths, not all early pipeline skips; raw vertex/UV data and actual
constant-buffer contents are not dumped. Descriptor logs show cached binding identities, not
an independent GPU descriptor-heap readback. These limits matter when interpreting the trace.
User asked to finish this test before committing/pushing progress for Claude. Build, runtime
import verification, marker check, patch reverse-apply and repository audit passed. Previous
DLL saved as rexgpu-xenosrd.dll.before-hud-probe. Launched 1x with HUD trace and converted-texture
capture, no synchronous GPU resolve readback. Front-end log port/logs/run-20261024-083033.log.
Await campaign menu and Cross-Com, then inspect logs and record outcome before push.

## HUD comparison and scoped depth diagnostic (2026-10-24)
Campaign log run-20261024-083033~grawyeti-083057.log: user confirms black Cross-Com;
screenshot also shows patterned green/blue HUD border and lower-left panel corruption.
Menu video (1D735000,1280x720) and Cross-Com (10C7F000,160x120) use the same PS
3445071A7FACCDE5 and VS 163102E6668FF616, slot0, used mask1, bindless, swizzle A0A.
No used PS float constants logged. Menu descriptor168, video descriptor501.
Depth control differs: menu 00700760, Cross-Com 00700762 (bit1 z_enable).
This is a lead, not a root cause. Converted texture samples30/90 again contain video.

User approved a build and launch of --graw_video_no_depth=true. Diagnostic matches BOTH
shader hashes above and slot0 type2 format6 160x120. Clears normalized host z_enable and
z_write_enable before shader modifications/render-target/pipeline setup; guest registers
unchanged, stencil unchanged, default off. Logs first three overrides including address and
before/after depth. Plugin build passed; patch reverse check passed. Installed campaign DLL,
previous version backed up as .before-depth-test. Launched 1x with --graw_trace_hud=true,
front log port/logs/run-20261024-083616.log. Runtime result pending. This is not a permanent fix.

### Scoped depth-disable result: FAILED (08:36 run)
User reports Cross-Com came up but remains black. Campaign log:
port/logs/run-20261024-083616~grawyeti-083633.log.
At 08:36:53.790/.805/.815 the override logs physical10C7F000 depth00700762 -> 00700760.
Subsequent aggregate counts confirm it continued matching. Thus the diagnostic did execute;
simple depth rejection of this identified HUD draw is not sufficient to explain the symptom.
Stencil, clipping/UVs, descriptor contents/indexing, blending and later overdraw remain open.
No permanent fix. Next inspect an actual frame's draw output and subsequent pixel history,
preferably RenderDoc, or bounded after-HUD-draw readback plus vertex/descriptor inspection.
Game not stopped. Latest installed DLL is diagnostic; flag defaults off. Source patch retained.

## `--d3d12_bindless=false` (08:40 run, Claude): no fix
With bindless off (and async off), the Cross-Com and the top-right video are still black.
The scene got a green/olive colour cast and the HUD border stayed static. The descriptor slot
(168 vs 501) is not simply stale in the bindless path, and renderer options are exhausted.

## Next: rebuild the renderer back toward January Canary (the working fork point)
ReXGlue's Xenos plugin is Canary's January 2026 GPU code (ReXGlue 96bf707, 2026-01-22) plus 60
`src/graphics` commits. Canary from that January (263e91b) shows the Cross-Com and the map, and
the video data is intact up to the final HUD draw (Astra's readbacks). So the break is in one of
those ReXGlue commits. Plan:
1. In `third_party/rexglue-sdk-bd833a2`, on a local branch, revert the functional renderer
   commits while keeping the build, plugin-ABI (c22a33e, 55a836f), cvar and logging
   refactors. The candidates, roughly by size:
   - e6407f3, 8b2672d, 38cbd30 and f20b3b2: the March "port backend/pipeline/xenia-edge"
     imports;
   - c1ca695 (root SRV/UAV, viewport caching);
   - dc9cece (swizzle lookup cache);
   - a913a8e (async VS translation);
   - 7a2a2fc (cvar tuning);
   - 6379943 and bb4a536 (resolve pre-mask);
   - 4d3abf6 (gamma UNorm16);
   - 6124b36 (page-valid flags);
   - 75e339b, 8e6baae and acb88c3.
2. Build the plugin only, drop it in beside `grawyeti.exe` (stock saved as `.stock`), and test.
3. If the Cross-Com shows, bisect: re-apply halves of the reverted set until the breaking commit
   is found. Then fix it, or ship our own build of the plugin with that commit reverted.
4. If reverting everything still shows black, the difference is outside `src/graphics` (the
   runtime: memory, threads, presenter). Then the fallback is the user's idea: build January
   Canary's GPU code as a fresh plugin against the plugin ABI, as the F.E.A.R. port does with
   fear-nx's renderer (Xenia is BSD-3-Clause, so licensing allows it).

## Renderer revert, step 1: nine commits reverted (built 08:44, not yet tested)
Local branch `graw-revert-to-january` in `third_party/rexglue-sdk-bd833a2`. The diff from
bd833a2 is saved as `tools/patches/rexglue-bd833a2-revert-partial.patch`. Astra's diagnostics
from the working tree are saved separately in the ignored
`third_party/graw-diagnostics-working.patch`.
- **Reverted cleanly:**
  - 8e6baae (ZPD sentinel), acb88c3 (7e3to32 mantissa);
  - 6124b36 (page-valid flags), 6379943 and bb4a536 (resolve pre-mask);
  - 75e339b (vblank pacing), a913a8e (async VS translation);
  - **c1ca695 (root SRV/UAV and viewport caching: binding, where Astra lost the picture)**;
  - 7a2a2fc (cvar tuning).
- **Conflicted, not reverted yet** (later refactors touch the same lines; hand merge needed):
  f20b3b2 (xenia-edge registers/PM4), dc9cece (swizzle cache), 8b2672d (shader
  translator/pipeline port), 38cbd30 (Vulkan port), e6407f3 (D3D12 backend port), 0bd3521
  (core controls), 4d3abf6 (gamma UNorm16, 17 files).
- **Kept:** plug-in extraction and ABI (c22a33e, 55a836f), the setup split (506eee8), the
  PrimitiveProcessor mutex, debug markers, DRED/EndQuery fixes, build, cvar and logging refactors.
- Plugin-only build passed, and all 107 runtime imports resolve. Installed as the campaign's
  `rexgpu-xenosrd.dll`. The previous diagnostic DLL is `rexgpu-xenosrd.dll.before-revert`, and
  the stock one is still `.stock`.
- **Test:** `tools/launch.ps1 -Windowed "-Extra:--async_shader_compilation=false"`. If the
  Cross-Com shows, bisect the nine. If not, hand-merge the conflicting seven, starting with the
  D3D12 backend port e6407f3 and the shared shader/pipeline port 8b2672d.
- **Revert step 1 result (08:46): still black.** None of the nine clean reverts is the cause.

## Renderer revert, step 2: the March imports undone (built 08:53, not yet tested)
On branch `graw-revert-to-january` (18 commits over bd833a2; full diff in
`tools/patches/rexglue-bd833a2-revert-partial.patch`), with the plugin **built with Vulkan off**
(`-DREXGLUE_USE_VULKAN=OFF`; GRAW runs on D3D12), additionally reverted:
- **e6407f3 (the D3D12 backend port).** `d3d12/command_processor.{h,cpp}` was rebuilt from
  `e6407f3^` plus only the later plumbing (3bce420 DRED, ed9fd43 markers, 42e01f9 perf
  counters, 8cbc4c4 null guards, 71782a3 trace removal). This also removes the March
  occlusion-query, memexport-readback and vertex-buffer-residency code. The frame-end
  `clear_memory_page_state` call is restored as in Canary.
- **8b2672d (the shared pipeline port: texture cache, shared memory, render-target cache).**
  Shared memory is back to January's `SystemPageFlagsBlock` layout, plus a
  `SetSystemPageBlocksValidWithGpuDataWritten` with Canary semantics. The leftover Vulkan
  32-bit index pre-conversion is removed.
- **f20b3b2 (xenia-edge registers/PM4).** WAIT_REG_MEM polls through a volatile reference
  again.
- **dc9cece (fuzzy alpha epsilon, swizzle lookup cache).**
- A duplicate `gamma_render_target_as_unorm16` definition is removed.

Kept on purpose:
- 0bd3521: its `LOAD_ALU_CONSTANT` change routes through `WriteRegistersFromMem`, which matches
  Canary's bulk path.
- 4d3abf6: gamma, ported from Xenia itself.
- 38cbd30: Vulkan only.

All 107 runtime imports resolve. Installed; step 1's DLL is `rexgpu-xenosrd.dll.revert-step1`.
Test: `tools/launch.ps1 -Windowed "-Extra:--async_shader_compilation=false"`.
- **Revert step 2 result (09:01): still black.** With the March D3D12, pipeline, PM4 and
  swizzle-cache imports undone, the Cross-Com and the top-right video are still black.

## Paused (2026-10-24, user's call; still the #1 issue)
Where it stands:
- Proven fine: Bink decode and the game-side player, Bink's shader compilation, the plane
  uploads, the resolve, and the converted output texture. Astra's readback shows the actual
  video frames in it.
- The panel is lost in the final HUD draw, which uses the same shaders as the working menu
  video.
- None of these fix it: the renderer options, bindless off, depth off for that draw, and
  reverting ReXGlue's renderer changes back to roughly January (two steps, all functional
  changes except 0bd3521, 4d3abf6 and the Vulkan port).
- Since January Canary (263e91b) shows it, and our renderer now holds essentially the same
  logic, the difference is most likely **outside the renderer DLL**: the host
  (`rexruntime`: memory views, threads, timing, presenter or ImGui overlay compositing), or the
  game code itself (recompiled vs Canary's JIT), for example the HUD draw's vertices/UVs or
  something the game computes, like a panel visibility flag.

Ideas for when this resumes:
1. One RenderDoc frame of the recomp with the panel up, compared with the same frame in Canary.
   It shows the HUD draw's actual inputs and output, and whether something later overdraws it.
2. Dump the HUD draw's vertex data (positions/UVs) in both. Wrong UVs would sample a black
   border texel.
3. The user's fallback: build January Canary's GPU code as a renderer plug-in, the way the
   F.E.A.R. port uses fear-nx's. Our revert result suggests this alone won't fix it if the
   cause is host- or game-side.

Renderer DLLs in `port-yeti/out/build/win-amd64-relwithdebinfo/`:
- `rexgpu-xenosrd.dll` is still the **step-2 revert build** (the game was running when work
  stopped);
- `.stock` is the prebuilt original, and `.revert-step1`, `.revert-step2`, `.before-*` and
  `.traced` are experiments.
- Restore the stock one with
  `cp rexgpu-xenosrd.dll.stock rexgpu-xenosrd.dll` (game closed), or by rebuilding port-yeti.
- The revert branch is `graw-revert-to-january` in `third_party/rexglue-sdk-bd833a2`; the
  patches are in `tools/patches/`.

## Resumed: recomp as the main program, Canary as the reference (Lost Odyssey method)
Decision (user, 2026-10-24): keep the recompilation as the product and use Xenia Canary as the
visual reference, comparing the same frame draw by draw, as Lost Odyssey Recomp did. The stock
renderer DLL is back in place for this.
- **Tools:** RenderDoc 1.46 (portable) in the ignored `private/tools/renderdoc/`.
  - `tools/launch.ps1 -RenderDoc`: runs the recomp under RenderDoc with `--opt-hook-children`,
    so the campaign spawned by the front end is captured. Captures go to `port/logs/renderdoc/`.
  - `tools/renderdoc_canary.ps1 [-Build 263e91b]`: Canary on the user's ISO under RenderDoc.
    Captures go to `private/xenia-canary/<build>/renderdoc/`.
  - Press Print Screen in the game window to capture; F12 is Canary's own screenshot key.
  - **First recomp attempt (09:20) captured nothing.** RenderDoc did hook both programs: each log
    says "RenderDoc API initialized", and ReXGlue only reads the API to enable debug markers. But
    no `.rdc` or RenderDoc log was written anywhere. Windows 11 maps Print Screen to the Snipping
    Tool by default, which likely swallowed the key. **Use F12 in the recomp.** For Canary,
    where F12 is its own screenshot key, use the RenderDoc UI (qrenderdoc, "Launch Application",
    then "Trigger Capture"), or turn off the Windows Print Screen shortcut.
- **Procedure:**
  1. Capture one frame with the Cross-Com clip up in each program, at the same point of the
     first mission.
  2. In each capture, find the HUD draw that samples the 160x120 panel texture: Canary's
     equivalent of our PS `3445071A7FACCDE5` / VS `163102E6668FF616` draw.
  3. Compare its vertex positions and UVs, its bound texture (contents in Texture Viewer), its
     viewport/scissor, blend and depth state, and the render target before and after. Then
     check the later draws over that screen area for overdraw.
  4. The first difference names the cause: host-side (runtime) or game-side (recompiled code).
- Captures and logs contain game imagery: private, never committed.
