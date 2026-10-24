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

Not yet found: **the code that binds the video+68 texture as a render target, calls vtable slot
1, and then resolves**, and the HUD draw that samples the result. They are the next step.

## Plan
Work in order. Every step that builds or opens a game window needs the user's OK first (see the
rules below).

### 1. Static: find the render-to-texture path (no build)
- Find the readers of video+68: `lwz rX,68(rY)` where rY is a video object, in functions that
  also call through the vtable at +4 (`lwz r11,4(r11)` / `mtctr` / `bctrl`). Start from
  `sub_821720C8` and its callers, and from the Yeti texture class around `sub_82194720` and
  `sub_821947D8`.
- In that path, identify the guest D3D calls that set and restore the render target
  (`SetRenderTarget`/`SetDepthStencilSurface`), the viewport, and the `Resolve` (on the 360 a
  render target lives in EDRAM and only reaches the texture through a resolve).
- Decode engine format 4 and the flags `1, 1` in `sub_821947D8`: the D3D format and whether it
  is a render target, tiled or linear.
- Find the HUD draw that samples video+68's texture, and which fetch constant it uses.

### 2. Probe: what the renderer sees (one build, ask first)
Add midasm hooks, as in `bink_probe.cpp` (port-yeti manifest, off unless a cvar is set), at the
calls found in step 1. Log:
- the render-target surface, its EDRAM base, pitch, format and MSAA when the Bink draw runs;
- the `Resolve` source rectangle and destination (texture base address, pitch, format,
  exponent bias);
- the guest address and format of the texture the HUD then samples.

Then compare against the stock renderer's resolve path. Either:
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
- **Cheap A/B runs** (the user runs them, no build), front end with `-Extra`:
  - `--readback_resolve` was already tried and didn't help.
  - Still to try: `--render_target_path_d3d12 rov`, `--gamma_render_target_as_unorm16=false`,
    and resolution scale 1 (`-Scale 1`). A scaled-resolve bug would show at 2x only.

### 4. Fix, then check the related bugs
The same render-to-texture path may explain other campaign bugs, so check them after the fix:
- the HUD border noise (#1)
- the empty tactical map (#3, a rendered texture too)
- the black screen under fire (#13, a full-screen effect)

## Tools added for this
- `port/src/bink_probe.cpp`: the video-object probe (`--graw_debug_bink=true`). Six hooks in
  `port-yeti/grawyeti_manifest.toml`, under "Bink video probe".
- `tools/xsh_dump.py`: decodes the renderer's shader storage
  (`port/cache/shaders/shareable/555307D5.xsh`; only readable while the game is closed).
  `--dump_shaders` writes nothing for shaders loaded from storage.
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
