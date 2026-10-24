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
