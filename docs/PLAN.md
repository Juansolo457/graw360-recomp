# GRAW (Xbox 360) static recompilation: plan to the title screen

Goal of this plan: boot the Xbox 360 **Tom Clancy's Ghost Recon Advanced Warfighter** natively
on Windows and reach the **Press Start / main menu** screen, with the game's own code running
and drawing. Everything after that (playable missions, 4K, high frame rate, Linux, the Yeti
multiplayer executable) is listed at the end as later phases, in the order they'd come.

Written 2026-10-23 from the recon in [FINDINGS.md](FINDINGS.md). The build/run detail and the
current state live in [HANDOFF.md](HANDOFF.md).

## The two models and what we take from each

| | Lost Odyssey Recomp ([freefrank/LostOdysseyRecomp](https://github.com/freefrank/LostOdysseyRecomp)) | fear360-recomp (ours, `../fear360-recomp`) |
|---|---|---|
| Translator | XenonRecomp (patched fork in `tools/`), `python tools/ppc_codegen.py generate` → `LostOdysseyRecompLib/ppc/` | ReXGlue SDK (`rexglue codegen <manifest.toml>` → `port/generated/default/`) |
| Runtime | Their own, Unleashed-Recomp style: hand-written kernel/XAM shims, plume RHI (D3D12, Vulkan, Metal), XenosRecomp shaders, FFmpeg audio | ReXGlue's Xenia-derived runtime: kernel HLE, VFS, XAudio, Xenos GPU plugin (D3D12/Vulkan) that reads the game's PM4 stream |
| First boot cost | Months: every kernel call and the whole GPU path written by hand before anything draws | Days: F.E.A.R. reached its menu on the first build; N3 likewise |
| Strengths to copy | Repo conventions: a status ledger with "separate facts" (built / ran / accepted / released), a docs index, dated notes vs. maintained guides, an importer (folder/XEX/ISO), a roadmap tracked as issues | The toolchain, scripts and lessons: manifest-first codegen, headless smoke runs, window capture, the `gpu_plugin = "xenos"` trap, the controller-slot trap, source-only releases |
| Where it hurts | Enormous surface before first light; native renderer per game | The stock Xenos plugin corrupted F.E.A.R.'s menus (fixed by fear-nx's renderer); PM4 parsing costs CPU |

**Decision:** ReXGlue, the same toolchain and layout as fear360-recomp, because the goal here is
a first boot, and that is what ReXGlue is fastest at. Lost Odyssey's lesson is applied to the
*project*, not the toolchain: a status ledger, dated findings, a plan with exit criteria, and a
user-facing importer later. If the stock renderer fails on GRAW the way it did on F.E.A.R., the
fallback is the port-nx recipe (fear-nx's patched SDK built from source with a native Vulkan
Xenos renderer), which this repo is laid out to accept as a second `port-*/` directory.

## What we are booting (summary of FINDINGS.md)

- One disc, one main executable: `default.xex`, title `555307D5`, built 2006-02-23, XDK
  **2.0.2417** (a launch-window title, 9 months older than F.E.A.R.'s 3529). Encrypted, basic
  compression, 9.75 MB image at `0x82000000`, entry `0x82200FE0`.
- Engine: Red Storm's own GR3 Xenon engine (`RedStorm_GhostRecon3-Xenon`), with Havok physics,
  Bink video, XACT audio (`.xwb/.xsb`), a "Magma" UI/package layer (`magma.glb`,
  `magma_launcher.glb`), and HLSL shaders compiled **at runtime** by the XGraphics microcode
  compiler linked into the image (`data/*.hlsl`, `NewYetiShadersX360.sdf`, a
  `game:\shader_cache\` path). Xenia's "Diesel" label is wrong; Diesel is the PC version's
  engine (GRIN).
- A second executable, `Yeti_Final_Xenon.exe` (Ubisoft Paris's Yeti engine, `Yeti.big`
  1.29 GB), is the **multiplayer** game, started from the main one through
  `XamLoaderLaunchTitle`. Not needed for the title screen.
- Kernel surface: 133 `xboxkrnl` + 65 `xam` imports (F.E.A.R.: 347 + 338). Every import
  ReXGlue has no implementation for is Xbox Live or voice (`XNetCreateKey`,
  `XamUserCreateStatsEnumerator`, `XamVoiceSubmitPacket`, ...), none of it on the boot path.
- Boot path, from the asset names: `UbiLogo.bik` → `ESRB.bik` → `CortezIntro_*.bik` →
  the "Launcher" front end (`magma_launcher.glb`, `launcher_art`, `PressStart`,
  `gr_static_launcher_bootstrap.xwb`) → `L_MainMenu` over `MM_Background.bik`.
- Xenia Canary: `state-playable` since 2026-05 with `gpu-drawing-corrupt` and
  `requires_force_depth_clamp`. So the game's kernel and GPU usage is already understood by the
  code ReXGlue's runtime descends from, and the depth-clamp and night-time artifacts are the
  renderer problems to expect.

## Phases

### Phase 0: dump and recon — done (2026-10-23)
Disc listed and extracted with `tools/xdvdfs.py`, both executables inspected
(`tools/xexinfo.py`), the main image decrypted (`tools/xex_basic_decrypt.py`) and its imports
checked against the SDK (`tools/xeximports.py`). Results in [FINDINGS.md](FINDINGS.md).

### Phase 1: first translation and build
1. `rexglue init` scaffold in `port/` (done), `tools/build.ps1` runs codegen then the build.
2. **Manifest iteration:** the analyser's `UnresolvedCall` errors become `[entrypoint.functions]`
   entries after checking each target is a function start (`powerpc-none-elf-objdump` on the
   decrypted image). Round 1: four tail-call targets, all real functions (FINDINGS.md).
3. Build `graw.exe` with clang (the translation is roughly half F.E.A.R.'s size, so expect
   ~100 MB of C++ and a shorter compile).
- **Exit:** `graw.exe` links; codegen reports no analysis errors or warnings.

### Phase 2: boot to Press Start
Run headless first (`tools/smoke.ps1 -Seconds 60 -Captures 3`), read the log, fix, repeat. The
failure classes to expect, in the order they'd appear:

1. **Kernel/XAM calls** the runtime logs as unimplemented or failing. All are expected to be
   Live/network (`XNet*`, `XamSession*`, `XamUser*Enumerator`); the fix is to make them return
   "not signed in / no network" cleanly. Where the SDK already stubs them, nothing to do; where
   it doesn't, a small SDK patch in `patches/` (fear360 keeps 13 such patches).
2. **File system.** The game reads from `game:\` (`data\`, `video\`, `Sound_Bin\`,
   `Language\`); `game_root = "assets"` serves the extracted disc. Watch for the `d:\`-prefixed
   debug paths (`d:\aiparams.xml`, `d:\TestOldBE`) and for `game:\shader_cache\`: a shipping
   build may try to **write** its compiled shaders there. If it does, either
   `--allow_game_relative_writes` for the test, or redirect that folder to the user data root.
3. **Runtime shader compilation.** Unusual for a 360 title: the image carries the XGraphics
   HLSL → microcode compiler and compiles `data/*.hlsl` at load. That is only translated code
   running, so it should work, but it is CPU heavy on first load (and the reason for the
   `shader_cache` folder). Note the first-frame time; a cache in user data is the fix later.
4. **GPU.** The stock Xenos plugin (D3D12 on Windows) is the first try. Known from Xenia: the
   game needs **depth clamping forced** (`requires_force_depth_clamp`) and shows corruption at
   night and from the helicopter. For the title screen this probably means a black or
   partially drawn menu until the depth-clamp equivalent is found in the plugin. If menus
   or videos come out as garbage the way F.E.A.R.'s did, switch to the port-nx recipe.
5. **Video.** The logos and intro are Bink, decoded by the game's own code into textures. On
   F.E.A.R.'s stock build the videos were black but the game moved on; expect the same and
   don't block on it. Pressing A/Start skips them.
6. **Input and profile.** Press Start needs a signed-in player on the controller that pressed
   it. ReXGlue signs in user 0 only; the Logitech virtual XInput device on the user's PC takes
   slot 0 (F.E.A.R. and N3 both hit this). Disable that device, or adopt fear360's
   `SharedAssignment` (every controller drives player 1) in `graw_app.h`.
7. **Audio.** XACT wave banks via XAudio; the bootstrap bank loads before the front end.
   Audio failures are logged, not fatal.
8. **The Yeti hand-off.** Choosing Multiplayer calls `XamLoaderLaunchTitle` with
   `Yeti_Final_Xenon.exe`. Out of scope now; make sure the runtime's reaction is a logged
   message, not a crash.

- **Exit:** a capture of the Press Start screen from our own `graw.exe`, then the main menu
  with its background video or a placeholder, navigable with the pad. Recorded in
  [HANDOFF.md](HANDOFF.md) with the build commit and the log.

### Phase 3: into the game (next, not part of this plan's goal)
Campaign mission 1 loads and plays; saves work (`XContent` in the user data root); the
night-vision and helicopter rendering issues Xenia lists are checked on our renderer.

### Phase 4: the PC version treatment (as fear360)
Integer resolution scaling to 4K, the game's AA and 16× AF, display-locked pacing above 30 fps
(check the engine's timestep first: GRAW 360 ran at 30), a settings panel and Quit in the
front end, crash reports, Steam Deck build.

### Phase 5: multiplayer executable
`Yeti_Final_Xenon.exe` as a second recomp target with the same host code (the F.E.A.R. Files
pattern: a second manifest and translation, one `src/`), reached from the main game's menu the
way the collection launcher switches games. Local/system-link play only; Xbox Live is gone.

## Risks and unknowns

| Risk | Why it matters | Mitigation |
|---|---|---|
| XDK 2417 is older than anything ReXGlue has been tested on here | Early D3D/XAM behaviours (ring buffer setup, `VdInitializeEDRAM`, `XGetVideoMode` layouts) could differ | Xenia runs this title, and ReXGlue's kernel is Xenia's; patch the SDK where it isn't |
| Runtime shader compilation | Slow first load; the compiler is 360 code we can't shortcut; a write to `game:\` | Measure; cache under user data; `--allow_game_relative_writes` only for tests |
| Stock renderer corruption (depth clamp, night, helicopter) | Could block the menu, will block gameplay | Depth clamp in the plugin; port-nx recipe as fallback |
| Press Start without a profile | Game waits forever | Controller slot 0 fix / shared assignment |
| Legal | Same as F.E.A.R.: releases are **source only**, never binaries with translated game code, never game data | `.gitignore`, `private/`, NOTICE |

## Not in scope of this plan
Xbox Live, voice chat, achievements beyond the SDK's local emulation, the PC version's assets,
and any binary release.
