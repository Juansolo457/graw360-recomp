# Handoff: GRAW 360 on Windows (2026-10-24)

Read [PLAN.md](PLAN.md) for the phases and exit criteria and [FINDINGS.md](FINDINGS.md) for the
disc and executable facts. This file is the state, the build/run detail and where things live.
Update it at the end of every session; the next model starts here, not from chat history.
**#2 (black video panels) is FIXED (2026-10-24): a mistranslated jump table in the HUD sprite blend switch, see "Root cause" at the end of [CROSSCOM.md](CROSSCOM.md). #13 (black screen under fire) was the same bug: verified 2026-10-24 by grenade damage, black without the #2 override and the red, desaturated damage effect with it. #1 (tutorial border noise) is FIXED in the renderer (2026-10-24): a resolve into freed memory left the HUD atlases stale on the GPU, see "Root cause" at the end of [BORDER.md](BORDER.md) and `tools/patches/rexglue-bd833a2-freed-page-resolve.patch`. The campaign's installed `rexgpu-xenosrd.dll` is stock bd833a2 plus that fix (copy: `.fixed`); how it reaches players (upstream or a source-built plugin) is undecided. #3 (tactical map empty) is FIXED in the code generator (2026-10-24): `vpkd3d128` float16 packs lost the sign when vD == vB, see [TACMAP.md](TACMAP.md) and `tools/patches/rexglue-bd833a2-vpkd3d128-alias.patch`; all three programs were regenerated with a patched CLI; `tools/check_vector_aliasing.py` finds no other instruction with the same hazard.**

## Where it stands
**The game plays.** The front end hands over to the campaign and to Instant Action, as the
console's loader would. The user played the campaign tutorial and the first mission. Audio is
fine. At a 240 Hz guest refresh, the campaign's frame rate is "fantastic" in the user's words.

| Program | Disc file | What it is | State |
|---|---|---|---|
| `port/` → `graw.exe` | `default.xex` | front end: start screen, menus | works; hands over to the others |
| `port-mp/` → `grawmp.exe` | `GR3Xenon_Release.exe` | Instant Action, co-op, multiplayer (Red Storm's GR3 engine) | a co-op mission plays; the 3D view is dark or black (#9) |
| `port-yeti/` → `grawyeti.exe` | `Yeti_Final_Xenon.exe` | the campaign (Ubisoft Paris's Yeti engine) | plays through the front end; the rendering bugs below are open |
| `port-yeti-nx/` → `grawyeti.exe` | same | the campaign on fear-nx's native Vulkan renderer | experiment, **local only**: worse than stock, Cross-Com still black. Not the way forward |

Rendering in the campaign is the open front. These issues are filed on
[GitHub](https://github.com/Juansolo457/graw360-recomp/issues):
- **#2 video panels black (Cross-Com, narration panel)**: fixed 2026-10-24 (switch override in
  the campaign manifest), see [CROSSCOM.md](CROSSCOM.md).
- **#3 tactical map empty**: fixed 2026-10-24 (code generator, float16 pack sign), see
  [TACMAP.md](TACMAP.md).
- Performance at 4K: see [PERF.md](PERF.md) (CPU-bound on two threads; 4K itself is free).
- #1 HUD border, #4 window checkerboard, #5 speckle and shadows, #6 textures and bump maps, #7
  allies' weapons. #13 black screen under fire: fixed by #2's override (verified, see below).
- #8 tiny name dialog: fixed 2026-10-24, the runtime's dialogs now scale with the window height
  (`port/src/ui_style.cpp`).
- #10 saving: works (2026-10-24). A fresh New game writes `GRAW.sav`, its thumbnail and header
  under the user data root, the content enumerator then lists it, and Load game restores Mission
  1 from the "GRAW" save. The `D:\Dummy` write probe is refused because D: is the read-only disc,
  as on a retail console; nothing depends on it.
- #4 window checkerboard: the facades near the start of Mission 1 look like broken and frosted
  panes (jagged dark panes), possibly the real art; compare with a console recording before
  treating it as a bug.
- Other open issues: #11 frame rate in heavy scenes, #12 image quality defaults, #14 FOV 90.

What is in place:
- **Hand-off:** `graw_loader.*` and `graw_process.*`.
- **Controllers:** shared controllers.
- **Dialogs:** a readable font and pad navigation (`ui_style`, `ui_pad`).
- **FP guard:** `fpscr_guard` works around the audio worker's FP state.
- **Jump tables:** the front end's jump-table override.
- **Speed:** an x86-64-v3 build (one-instruction FMA), and the guest refresh raised to 240 Hz
  (`launch.ps1 -RefreshRate`; game speed unchanged).
- **Frame timing:** `frame_timing.cpp`, a 10 s fps summary plus `--graw_frame_log`.
- **Video probe:** `bink_probe.cpp`.

### Next steps
1. [CROSSCOM.md](CROSSCOM.md): step 1 (the static trace of the render-to-texture path) is done,
   2026-10-24. Next is step 2, the render-target/resolve probe, and the hand-off section there
   says how. The first build needs the user's OK.
2. The tactical map (#3), which is likely the same render-to-texture path.
3. FOV 90 (#14): find the campaign's projection setup, then add a hook and a launch option.

### port-yeti-nx: the campaign on fear-nx's renderer (#2, tried 2026-10-23, local only)
The Cross-Com box and the narration panel are black on the stock renderer although the game
reads and decodes the Bink videos (their audio plays). fear-nx's native Vulkan renderer fixed the
same class of bug in F.E.A.R., so `port-yeti-nx/` builds the campaign the way fear360's `port-nx`
builds F.E.A.R.: same manifest and host code as `port-yeti`, `GRAW_NX` defined, fear-nx's
renderer in `src/native/` (GPL-3.0-or-later, see NOTICE) and the Xenos plugin linked in.
- SDK: `third_party/rexglue-sdk-nx` (not tracked), a **copy** of
  `../fear360-recomp/third_party/rexglue-sdk-nx` (11c330c) without its `out/`. Not a junction:
  the SDK builds its libraries into its own `out/`, so a junction overwrote fear360's.
- `pwsh -NoProfile -File tools/build.ps1 -Project port-yeti-nx`: first build ~5 minutes (SDK
  from source). 388 switches, 0 wrong (`check_switch_tables.py private/xex/yeti.bin ...`).
- `pwsh -NoProfile -File tools/launch.ps1 -Windowed -Nx`: the front end hands Campaign to it
  (`--graw_campaign_exe`) with `--nx_renderer native --nx_native_resolution_scale <Scale>`.
  `-Extra '--nx_renderer xenos'` runs the stock plugin inside the same executable, to compare.
- Its SDK lacks `InputSystem::GetStateForUI`; `ui_pad.cpp` uses `GetState` under `GRAW_NX`.
- Play-tested 2026-10-23: the game was badly broken in play (empty texture lookups, 9 fps
  stretches) and Cross-Com was still black. Kept as a source-level copy of the Xenos plugin
  (`--nx_renderer xenos`) for renderer logging, see CROSSCOM.md step 3.

## How it is built and run
```
pwsh -NoProfile -File tools/extract.ps1 -Iso 'D:\x360-recomps\GRAW 1\<disc>.iso'   # once, ~4.8 GB
pwsh -NoProfile -File tools/build.ps1 [-Project port|port-mp|port-yeti] [-Jobs 14]   # codegen when needed, then build; build all three
pwsh -NoProfile -File tools/smoke.ps1 -Seconds 75 [-Captures 3]   # its window shows on the user's screen; log in port/logs/smoke-*.log
pwsh -NoProfile -File tools/smoke.ps1 -Seconds 30 -Extra '--log_level trace --log_noisy'  # every kernel call with arguments
pwsh -NoProfile -File tools/smoke.ps1 -Project port-yeti -Seconds 60 -CaptureEvery 3 -FreshProfile -Autopilot tools/autopilot/campaign-menu-probe.txt
pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1 [-Trace] # a window on the user's screen: say so first; log port/logs/run-*.log
pwsh -NoProfile -File tools/capture_window.ps1 -ProcessName graw  # screenshot even when covered
```
- Toolchain: `tools/setup.ps1` (docs/SETUP.md) downloads it into `.tools/` (ReXGlue SDK nightly
  `0.10.0.24-dev.gbd833a2`, LLVM 20.1.8, CMake 3.31.8, Ninja 1.13.1), and builds the patched
  code generator and the campaign's renderer plugin from `third_party/rexglue-sdk-bd833a2` (four
  patches in `tools/patches`; `build.ps1 -Project port-yeti` installs the plugin). On the
  maintainer's PC `.tools/` is a junction to `../n3-360-recomp/reference/N3-Recomp/.tools`
  with the same files. `build.ps1` enters a
  VS 2022 DevShell (vcvars64 breaks on a quoted PATH entry), checks the XEX hash, runs
  `graw_codegen`, reconfigures once on the first run (the recomp target only exists after the
  sources do), then builds `port/out/build/win-amd64-relwithdebinfo/graw.exe` with
  `rexruntimerd.dll` and `rexgpu-xenosrd.dll` beside it.
- The stock SDK is a prebuilt package (D3D12 only on Windows). If the renderer has to change
  (depth clamp, menu corruption), the recipe is fear360's `port-nx`: build fear-nx's patched SDK
  from source and link the Xenos plugin statically; see `../fear360-recomp/docs/HANDOFF.md`.
- `port/src/graw_app.h` installs the crash reporter and sets `config.gpu_plugin = "xenos"` in
  `OnPreSetup` (without the plugin the screen stays black; F.E.A.R. lesson).

## Analysis tools (all read-only on the game's files)
| | |
|---|---|
| `tools/xdvdfs.py <iso> list \| extract <dir> [names]` | XDVDFS reader; `tools/extract.ps1` wraps it |
| `tools/xexinfo.py <xex>` | XEX2 headers: title, build time, entry, image size, encryption, XDK libraries |
| `tools/xex_basic_decrypt.py <xex> <out.bin>` | the decrypted, decompressed image at `0x82000000` (basic compression only). Ours: `private/xex/graw.bin`, 10,223,616 bytes |
| `tools/xeximports.py <xex> <image.bin> [<rexglue-sdk src>]` | kernel/XAM import ordinals, named and checked against the SDK's export tables; the SDK source used was `../fear360-recomp/third_party/rexglue-sdk-nx` |
| `tools/check_switch_tables.py <image.bin> port/generated/default [--toml out.toml]` | replays every generated `switch` against the image and writes `[[entrypoint.switch_tables]]` overrides for mismatches. Rerun after every codegen |
| `tools/find_missed_functions.py <image.bin> port/generated/default` | function entries the analyser missed (method tables, callbacks built in code), as manifest lines; from fear360. Candidates that split a function show up as codegen `Unresolved …` warnings: find the entry between the warned addresses and drop it |
| `../fear360-recomp/third_party/rexglue-sdk-nx/tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc:common -EB --adjust-vma=0x82000000 --start-address=0x… --stop-address=0x… private/xex/graw.bin` | disassemble a range of the image |
| `tools/xsh_dump.py <555307D5.xsh> [--hash H]` | decodes the renderer's shader storage (Xenos microcode of every translated shader); the game must be closed |
| `private/xex/graw.strings.txt` | every printable string of 6+ characters in the image (a 3-line Python regex scan) |
| the generated code | `port/generated/default/graw_recomp.N.cpp`: every PowerPC instruction is a comment above its translation, so a crash report's file:line gives the guest instruction; `lis/addi` pairs in a function name the strings it uses |

## Where things live
| | |
|---|---|
| `port/graw_manifest.toml` | project + entrypoint config and `[entrypoint.functions]`, with a comment per round saying where each group came from |
| `port/src/graw_app.h`, `src/main.cpp` | host app: Xenos plugin, crash reporter; game-specific host code goes next to them (fear360's `fear_*.cpp` are the pattern: controller sharing, settings panel) |
| `port/src/shader_debug.cpp` | two debug hooks (`--graw_debug_shader_compile`, off by default): the HLSL compile call's arguments, and every parser reduction into `logs/parser-trace.txt` |
| `port/src/graw_loader.*`, `graw_process.*` | the hand-off between the three programs (launch data, own log per program, wait for the previous run) |
| `port/src/autopilot.*`, `tools/autopilot/*.txt` | scripted virtual controller (`--graw_autopilot <script>`, `tools/smoke.ps1 -Autopilot`): button and stick timetables, no window focus needed. The window still opens on the user's screen: ask before unattended runs |
| `port/src/bink_probe.cpp` | campaign video-object probe for #2 (`--graw_debug_bink=true`, hooks in `port-yeti`'s manifest) |
| `port/src/fpscr_guard.cpp` | seeds the FP state of the audio worker's context (hooks in all three manifests) |
| `port-mp/`, `port-yeti/` | the GR3 and Yeti projects: own manifest, `src/main.cpp`, `generated/rexglue.cmake`; `assets` is a junction to `port/assets` |
| `port/src/crash_report.*` | from fear360 (MIT): `logs/crash-<time>.txt` with the exception, registers and the guest stack (`sub_<360 address>` with the generated file and line), plus a minidump |
| `port/generated/rexglue.cmake`, `CMakePresets.json` | `rexglue init` output (BSD, tracked); `generated/default/` is the translation (ignored) |
| `port/assets/` | the extracted disc (ignored): `default.xex`, `data/`, `video/`, `Sound_Bin/`, `Language/`, `Yeti*` |
| `port/logs/`, `port/user-smoke/`, `port/cache-smoke/` | smoke runs and crash reports; `port/user/`, `port/cache/` for `launch.ps1` |
| `private/` | `game/` (both executables), `xex/` (decrypted image, strings, imports), `build-*.log`, `missed-round1.txt`, `sub_822AD650.cpp` (the parser's generated code, extracted), `captures/` |
| `docs/PLAN.md`, `FINDINGS.md`, this file | plan, facts, state |

## Known traps (from the sister projects)
- A Logitech virtual XInput device takes controller slot 0 on this PC, making the Xbox pad
  player 2 with no profile: Press Start then does nothing useful. Disable that device, or port
  fear360's `SharedAssignment` (`fear_app.h`, `fear_shared_controllers`).
- `--resolution 720p` tells the guest it is on a 720p display; without it ReXGlue squeezes to
  4:3 at 200% DPI. The launch and smoke scripts pass it.
- `tools/smoke.ps1` passes `--headless`, but the window still appears and the user played in
  one by mistake (it is how the start screen was first seen). Say so before running it.
- A hook that reads guest memory must check the page is committed (`shader_debug.cpp`,
  `Readable`): registers hold floats and stale values.
- Codegen does not rerun on its own after a manifest edit in a plain `cmake --build`;
  `build.ps1` always runs the `graw_codegen` target first.
- `python -I` for every script that reads game files. Windows paths for Python arguments
  (`D:\...`), not Git Bash `/d/...` ones. Long heredocs with backslashes get mangled by the
  Bash tool here; write scripts to a file first.

## Licensing reminders
Our files are MIT (`LICENSE`, `NOTICE`); ReXGlue is BSD-3-Clause with Xenia's notices; if
fear-nx's renderer is ever brought in, that directory and any build linking it are GPL-3.0.
Never commit game data, generated translations, captures or saves. Releases are source only.

## Current #2 handoff (2026-10-24): root cause OPEN

Latest test FAILED: user still sees black Cross-Com with scoped depth testing disabled.
Log port/logs/run-20261024-083616~grawyeti-083633.log confirms three detailed overrides
at physical 10C7F000: normalized depth 00700762 -> 00700760, followed by continued counts.
This weakens simple depth rejection of that draw; does not exclude later overdraw or stencil.

Read the final sections of CROSSCOM.md for evidence. Earlier memory overlap was withdrawn.
- Canary 263e91b displays Cross-Com/map on the same disc; recomp does not.
- CPU staging first-page samples were nonzero and matched E0 virtual aliases (23/23).
- GPU resolve readbacks contain substantial changing data.
- Converted GPU host texture samples30/90 visibly contain the Cross-Com character. Sample1
  was zero. Images are local ignored port/logs/graw-converted-10C7F000-{1,30,90}.ppm (and PNG).
  Capture is before SRV swizzle; exact colour correctness is not proven.
- Working menu and Cross-Com final draws share PS3445071A7FACCDE5, VS163102E6668FF616,
  slot0 only, used mask1, swizzle A0A and bindless mode. No used PS float constants logged.
  Their depth enable difference motivated the failed test. Cached descriptors are logged,
  not independently read from the GPU heap. Vertex/UV data and actual packed constants absent.
- Screenshot shows black video and green/blue patterned border/lower-left HUD corruption.

Next best investigation: capture a frame in RenderDoc and inspect the identified HUD draw's
actual SRV, sampler, vertex positions/UVs, interpolants, target pixels before/after, stencil,
blend/alpha and later overdraw. If capture unavailable, add bounded readback immediately after
that draw and inspect actual descriptor indexing / VS constants / vertex data. Compare these
specific paths against Canary263e91b; do not replace the whole renderer without isolating its
runtime/memory integration differences. No fix exists yet; instrumentation is diagnostic only.

Source: third_party/rexglue-sdk-bd833a2. Reproducible source-only changes in
 tools/patches/rexglue-bd833a2-graw-rtt-trace.patch (reverse-apply check passed).
Installed campaign DLL port-yeti/out/build/win-amd64-relwithdebinfo/rexgpu-xenosrd.dll is
instrumented with opt-in --graw_video_no_depth=true. .before-depth-test preserves prior probe;
.stock preserves original. Do not enable depth override for baseline tests.
Other flags: --graw_trace_hud=true; --graw_readback_texture=true (asynchronous images);
--graw_readback_video=true (synchronous GPU resolve samples, stalls); --graw_trace_rtt_max_width=200.
Last launch: pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1
"-Extra:--graw_video_no_depth=true --graw_trace_hud=true". Build passed. Game left untouched.

Ask before every build/launch; never close/kill user's game. Downloads allowed. User tests.
Read campaign logs yourself. Shared checkout: explicit-path commits only, identity
juansolo457 <5344231+Juansolo457@users.noreply.github.com>. Audit before pushing, fetch first,
ask before future pushes. Never commit game data, logs, captures or generated translations.
Do not write to ../fear360-recomp.

## Earlier #2 handoff after Canary comparison (2026-10-24, superseded)
**Cross-Com works in Xenia Canary 9e03494 with depth clamp ON and OFF.** The user also
confirmed a correct-looking crosshair and tactical map in the OFF run. These were Canary
runs, not recompilation builds. No recomp code was changed or built; no fix is claimed.
Canary also showed environment twitching/missing textures absent in the recomp, so use it
as a reference for specific passes rather than assuming its entire renderer is preferable.

The older Next steps and HANDOFF-ASTRA.md comparison proposal are superseded by this result.
The RTT probe already established matching game-side passes for menu and Cross-Com.
Next: compare Canary 9e03494 GPU code with third_party/rexglue-sdk-nx (11c330cf), especially
texture cache, resolve, shared-memory invalidation and HUD draw state. Depth clamp is not
the demonstrated cause. Finish the HUD-draw probe if needed; its D3DTexture+28 fetch decode
is known wrong. Record evidence in CROSSCOM.md, then propose a targeted fix and ask before
building. User confirms in game; afterward test #3, #1 and #13 individually.

Canary's front-end-to-campaign handoff failed with function lookup errors. Direct launch of
Yeti_Final_Xenon.exe from the ISO worked. Local Canary is in ignored
private/xenia-canary/9e03494/. Configs: graw-comparison.toml (clamp on),
graw-depth-clamp-off.toml (off). Launch from that directory with:

```powershell
.\xenia_canary.exe "D:\x360-recomps\GRAW 1\Tom Clancy's Ghost Recon - Advanced Warfighter (USA, Europe) (En,Fr,De,Es,It).iso" --config=graw-depth-clamp-off.toml --launch_module=Yeti_Final_Xenon.exe
```

Ask before every launch/build/download; never stop the user's game. Logs and config snapshots
are private, not committed: campaign-crosscom-working-20261024.log/.toml (on),
campaign-clamp-off-working-20261024.log/.toml (off), frontend-handoff-failed-20261024.log.
See CROSSCOM.md for details and the Lost Odyssey comparison-method reference. Crosshair
visibility does not specifically verify the HUD border (#1); under-fire black screen (#13)
was not tested. The root cause remains unknown.

## Taking damage unattended (#13, 2026-10-24)
Controller configuration A (pause menu > Options > Controller): X selects the weapon (the frag
grenade is the next one), RT fires or throws, B toggles night vision, Y is select/action, LB the
engagement mode, RB switches the camera side, Back the tactical map. From the "GRAW" save, looking
down (right stick down) and throwing a grenade at the player's feet gives the damage effect a few
seconds later: a red vignette with a desaturated screen. Without the #2 switch override in the
manifest the same frames are entirely black, which was #13. The local autopilot is
`private/autopilot/load-graw-nade.txt` (looks down at 80 s; X at 84 and 100 s, B at 92 and 110 s, RT after
each).

## Parked 2026-10-25: state and next step
- **The game logic is built for ~30 fps** (#17): at 30 (the default now, `launch.ps1`; `-Console`
  for 720p and the game's own filtering) squad commands, the rally point (#19) and the AI behave
  as on the 360; at 60 allies' weapons vanish and the rally point blocks. #7 (weapons vanishing)
  still happens at 30 after explosions, so it is also a separate bug.
- **Installed campaign renderer:** stock bd833a2 + `rexglue-bd833a2-freed-page-resolve.patch` +
  `rexglue-bd833a2-vblank-pacing.patch` (`port-yeti/out/build/.../rexgpu-xenosrd.dll.fixed2`).
  Every campaign build copies the SDK's stock DLL back: reinstall `.fixed2` after building.
  (Since 2026-10-26 `build.ps1` installs the plugin `tools/setup.ps1` built instead: these
  patches plus `gpu-skip-draws`.)
  The SDK work tree keeps the diagnostics (`rexglue-bd833a2-graw-rtt-trace.patch`), the codegen
  fix (`rexglue-bd833a2-vpkd3d128-alias.patch`, generated code needs the patched CLI, see
  TACMAP.md) and the vblank change.
- **Steam Deck test copy:** `D:\x360-recomps\packages\graw360-steamdeck-20261025.zip` (private:
  game files + saves; 30 Hz, 720p).
- **Next (step 1 of 60+ fps):** find the campaign's main loop (the per-frame update on the main
  thread that calls VdSwap; the frame timer is `sub_82406748`, `mftb`) and log the time step it
  hands each subsystem; then run the same scripted scene at 30 and 60 and diff which systems step
  per frame instead of by time (animation playback, AI, squad orders, weapon attachment). The
  number of culprits decides whether 60 (and 120) is realistic. The renderer also needs headroom
  for 60+: the command processor reaches ~16 ms in the plaza fight (#11).
- **Other open from the play-tests:** #21 first-load stutter (pipeline compilation, depth-only
  pipelines are created synchronously), #18 AI parity check against a real 360, #20 menu videos,
  #15 one executable, #16 LOD and shadow resolution.
- **Tools added:** `tools/sample_profile.py` (sampling profiler with per-second bins),
  `tools/check_vector_aliasing.py`, `tools/launch.ps1 -Console`, portable layout (programs + game\
  + user\ in one folder).
