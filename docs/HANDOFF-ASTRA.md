# Handoff for Astra: the black Cross-Com and video panels (#2)

Written 2026-10-24. This is the entry point. The full evidence and every address are in
[CROSSCOM.md](CROSSCOM.md) (plan, the static trace, both probe results). Building and running
are in [HANDOFF.md](HANDOFF.md).

## The problem
Ghost Recon Advanced Warfighter (Xbox 360, title 555307D5) is statically recompiled with
ReXGlue. The campaign is `port-yeti/`, from Yeti_Final_Xenon.exe, Ubisoft Paris's Yeti engine.
- **What fails:** every in-game video panel is black: the Cross-Com feed (top left), the
  narration video panel (top right), and the squad portraits. Their audio plays.
- **What works:** the full-screen menu background video. The rest of the campaign plays at a
  high frame rate.
- **Priority:** this is the user's top bug. The tactical map (#3) is next, and probably shares
  the cause.

## What is ruled out (each one was tested, not assumed)
| Suspect | How it was ruled out |
|---|---|
| File reading and Bink decoding | the files are read in full, the audio plays, and the decoded planes reach texture memory |
| The game's video player | `bink_probe.cpp`: the Cross-Com object (160x120, 584 frames) decodes at about 20 fps, its ready flag is set, and every draw takes the frame path |
| Bink's YUV shader, compiled at runtime by the game's recompiled D3DX compiler | its stored microcode (`tools/xsh_dump.py`, shader `E4F08314542740C2`) matches the HLSL exactly |
| The render-to-texture path (`sub_821BB8F0`: SetRenderTarget, Bink draw, resolve, restore) | `rtt_probe.cpp`: the **visible menu video goes through exactly the same path** (its own surface, a per-rect resolve in mode 1, one HUD `SetTexture` per pass on sampler 0) as the black Cross-Com |
| The renderer as a whole | fear-nx's native Vulkan renderer (`port-yeti-nx`, built from source) is black too, and breaks the rest of the game. Kept as a source-level copy of the stock Xenos plugin (`--nx_renderer xenos`) |
| Resolution scale | the user's early tests at `-Scale 1` were black too |
| `--readback_resolve` | no change |

## What is left: the two leads
1. **The HUD draw that shows the Cross-Com texture.** It is the only step not compared yet.
   - The full-screen video and the Cross-Com box are drawn by different HUD code.
   - The box's material may combine the video with a second texture (a frame, a mask, a noise
     or scanline layer), or use a shader or blend state that the renderer gets wrong.
   - The campaign already has missing textures and 213 "invalid texture fetch constant"
     warnings (#6). A missing second texture would make the box black.
2. **The texture's size.** 160x120 is not a multiple of 32, so its EDRAM surface is 160x128. The
   menu's 1280x720 is aligned. A renderer bug in small or unaligned resolves would hit only the
   panels.

## The probe that was about to be written: the HUD draw
- **Trigger:** `rtt_probe.cpp` already sees the HUD bind the video texture: D3D `SetTexture` at
  `0x824C6F88`, arguments (device, sampler, texture). Set a thread-local "armed" flag there.
- **What to hook:** the D3D draw calls, logging the device state when armed. Candidates found in
  the D3D library code; check each one's arguments before relying on it:
  - `0x824C4838`: DrawPrimitiveUP (used by Bink's own draw `sub_82340EE8`: device, type, count,
    data, stride).
  - `0x824C4578`, `0x824C48C0`, `0x824C4BF8`, `0x824C4E38`: other draws. 4E38 has 24 call sites
    and 4BF8 has 11, so those two are the likely DrawIndexedPrimitive and DrawPrimitive. All of
    them check the device's vertex declaration at device+11540 first.
  - `0x824D7E80` reserves command-buffer space. It is called by every draw but also by
    resolves and clears, so it is too broad to hook.
  - `0x824D4868` is an empty function (`blr`).
- **Device state** (device = `*0x828F18B0` in Bink code, or renderer+4 with the renderer at
  `*0x8265803C`):
  - the pixel shader is at device+12944 (`SetPixelShader` `0x824C8D28` stores it there);
  - the vertex shader store is in `SetVertexShader` `0x824C9098` (`stw r31,20476(r30)`, so check
    it);
  - the sampler textures are at device+(3176+i)*4 for i = 0..15 (`SetTexture`'s slot math);
  - render states (blend, alpha test) are not mapped yet.
- **Log at the first draw after the bind:**
  - the pixel shader pointer, plus its microcode hash, which can be matched against the shader
    storage with `tools/xsh_dump.py`;
  - all 16 sampler textures;
  - the vertex count.
- **Compare:** the same for the menu video's draw, the one that works.
- **Fix in the probe first:** D3DTexture+28 is not the fetch constant in this XDK (2.0.2417). The
  texture and the surface both read `00001414 00000000 00000200` there. Dump the first 64 bytes
  of the D3DTexture and find the real offset.

## Compare with Xenia (worth doing first, and cheap)
ReXGlue's renderer is derived from Xenia's, so a working Xenia makes a reference to diff against.
- Xenia's compatibility trackers:
  - [xenia-project/game-compatibility#451](https://github.com/xenia-project/game-compatibility/issues/451):
    old master builds, menus only, with screen garbage.
  - [xenia-canary/game-compatibility#464](https://github.com/xenia-canary/game-compatibility/issues/464):
    **state-playable**. Testers finished the campaign on Canary builds 8e1b286 and ec18a89, with
    some graphical glitches. Its labels are `requires_force_depth_clamp` and
    `gpu-drawing-corrupt`.
- A Canary screenshot from that issue shows the HUD border drawn correctly, as a thin cyan
  frame. Ours is noise or black (#1). The back-button icon also draws there. **No screenshot
  shows a Cross-Com clip, and no comment mentions Cross-Com.** So whether Xenia shows it is
  unknown.
- **To settle it** (needs the user's OK: a download, and a window on their screen):
  1. Get the latest Xenia Canary from its official GitHub releases.
  2. Run the user's own disc image (`D:\x360-recomps\GRAW 1\`) with `force_depth_clamp` on, as
     the label says.
  3. Play to the first Cross-Com clip (the start of the first mission).
- **What the answer means:**
  - **Canary shows it:** the bug is in what ReXGlue's renderer took from Xenia, or added. Diff
    Canary's `src/xenia/gpu/` against the SDK's `src/graphics/`, starting with
    `texture_cache`, `render_target_cache` resolve and `shared_memory`. Also check whether the
    SDK has an equivalent of `force_depth_clamp`.
  - **Canary is black too:** it is a long-standing Xenia-family renderer bug. Go back to the HUD
    draw probe.
- Canary also draws the HUD border right. Diffing for #1 may lead to the same code.

## Tools in the repo
- `port/src/bink_probe.cpp` (`--graw_debug_bink=true`) and `port/src/rtt_probe.cpp`
  (`--graw_debug_rtt=true`): campaign probes, off by default. Their hooks are in
  `port-yeti/grawyeti_manifest.toml`.
- `tools/xsh_dump.py`: decodes the renderer's shader storage,
  `port/cache/shaders/shareable/555307D5.xsh`. The game must be closed.
- `tools/check_switch_tables.py private/xex/yeti.bin port-yeti/generated/yeti_final_xenon`:
  rerun after every codegen. 388 switches, 0 wrong, as of the last build.
- The generated code, `port-yeti/generated/yeti_final_xenon/*.cpp`: each PowerPC instruction is a
  comment above its C++. A function starts at `DEFINE_REX_FUNC(sub_XXXXXXXX)`, and instruction N
  of it is at the start address + 4*(N-1).
- Shader dumps: `--dump_shaders=<dir>` needs the `=`. A bare flag dumps to `port/true/`, which
  is ignored and has 2,288 files from 2026-10-23.

## Rules from the user (they apply to every session)
- **Builds and windows:** announce and wait for an OK before any build, download, or anything
  that opens a game window. The user is often debugging another game in another session.
- **Their game:** never close or kill the user's running game.
- **Testing:** the user tests. Give them the launch line, or launch only when they say so. Then
  read the campaign log yourself: `port/logs/run-<time>~grawyeti-<time>.log`.
- **Commits:** as `juansolo457 <5344231+Juansolo457@users.noreply.github.com>`. Ask before
  pushing.
- **Game data:** never commit it (ISOs, XEX/EXE, extracted assets, decrypted images, shader
  dumps, captures, generated translations, logs, saves). Releases are source-only.
- **Shared SDK:** don't write into `../fear360-recomp`.
- **Launch options:** pass them through `-Extra` in the colon form, with `=` for values:
  `tools/launch.ps1 -Windowed "-Extra:--graw_debug_rtt=true"`.
