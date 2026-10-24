# Findings: Ghost Recon Advanced Warfighter (Xbox 360)

From the user's own disc. Hashes, names, sizes and addresses only, never content. Everything
here applies to `default.xex` SHA-256 `295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83`.

## Dump and executables (2026-10-23)
| | |
|---|---|
| ISO | 7,834,892,288 bytes (Redump-verified USA/Europe multilingual disc); XDVDFS game partition at `0x0FD90000`; 1,038 files, 4.76 GiB |
| `default.xex` | 8,826,880 bytes, title `555307D5`, media `617C5216`, v0.0.0.1, built 2006-02-23 16:40 UTC, original PE `default.pe`, load `0x82000000`, image `0x9C0000`, entry `0x82200FE0`, encrypted + basic compression; decrypted image 10,223,616 bytes |
| `GR3Xenon_Release.exe` | same size and headers, built two minutes earlier (16:38), entry `0x821EB308`, different content throughout: a sibling build left on the disc. The console loads `default.xex`; so do we |
| `Yeti_Final_Xenon.exe` | 6,922,240 bytes, same title ID, built 2006-02-16, entry `0x82402660`, image `0x950000`. Ubisoft Paris's Yeti engine: the single-player **campaign** (first thought to be multiplayer; see "The disc's three programs"), with `Yeti.big` (1.29 GB), `NewYetiShadersX360.sdf/.sat` and `NewShaderlist.txt` |
| XDK | 2.0.**2417** (LIBCPMT, XAPILIB .4, XBOXKRNL, XGRAPHC, XONLINE, D3D9, D3DX9, XACT, XAUD, X3DAUD, XHV). Yeti adds XMP and the LTCG audio libs |
| Data | `data/` 1.6 GB: 10 maps as `*_map.glb` + `*_map_asynch.glb` (Red Storm's GLB packages), `.mis` missions, `.xpr` textures (intel, mapshots, env maps), `.xwb/.xsb` XACT banks per mission and language, `.bik` menu backgrounds and the 411 MB attract movie; `video/` ~300 Bink cutscenes and tips; `Sound_Bin/` 1.5 GB of `.SB4/.SS4` (Yeti's sound banks, multiplayer); `Language/*.str`; `$SystemUpdate/su20076000` |
| Shaders | `data/*.hlsl` and `data/ShaderFragmentsModel3/*.h`: **HLSL source on the disc**; the image contains the XGraphics microcode compiler (`d:\xenon\nov05\...\xgraphics\ucode\compiler\...` source paths) and a `game:\shader_cache\` path |

## Engine identification (from the decrypted image's strings)
- Red Storm's GR3 Xenon engine: `CLIENT.Ph.RedStorm_GhostRecon3-Xenon`, `D:\GR2_GraphableMemory_%d.log`, RTTI for Red Storm's networking (`RSNetwork`, `RSRNP*` reliable UDP, `RSXbLink*` system link), `IkeStateMgr` saves, `PersistentActor` `.cms` files.
- Middleware: Havok ("Havok Physics evaluation key" strings, `ragdoll.hkx`, `treadsmotion.dat`), Bink (`BINKDATA`, `.bik` names), XACT (`cortez.xgs`, `gr_*.xwb/.xsb`), zlib/libpng/libjpeg, a "Magma" UI and package system (`MagmaManager.cpp`, `magma.glb`, `magma_launcher.glb`, `MagmaPackages`, `Magma Font`).
- Boot flow by asset name: `UbiLogo.bik`, `ESRB.bik`, `CortezIntro_US/Euro.bik` → "Launcher" front end (`launcher_art`, `launcher_art_sp`, `PressStart`, `PressStartLaunch`, `Menus_LauncherLoad`, `Mus_Menu_Launcher`, `gr_static_launcher_bootstrap.xwb`) → `L_MainMenu` (`MM_Background.bik`, `SP_Background.bik`, `MP_Background.bik`).
- Hand-off to the other programs: the strings `Yeti_Final_Xenon.exe` and `gr3xenon_release.exe` are in the main image, and `XamLoaderLaunchTitle` / `XamLoaderSetLaunchData` are imported.
- Xenia Canary [issue 464](https://github.com/xenia-canary/game-compatibility/issues/464): `state-playable` (2026-05-17), `gpu-drawing-corrupt` (flicker on a road section, artifacts at night, helicopter view, rare shadow glitches; one glitch reproduced on real hardware), `requires_force_depth_clamp` (2026-09-25). Its `tech-engine-diesel` label is wrong (Diesel is the PC version's engine).

## Kernel imports (tools/xeximports.py against ReXGlue `c94f5eb`+fear-nx)
- `xboxkrnl.exe`: **133** distinct ordinals (385 import records in all). Threads, events, mutants, semaphores, timers, critical sections, TLS, files (`NtCreateFile/ReadFile/WriteFile/QueryDirectoryFile/...`), virtual and physical memory, `Vd*` video (ring buffer, swap, EDRAM retrain, display mode, gamma, scaler command buffer), `XAudio*` render driver, `XMACreateContext`, `KeLockL2`, `__C_specific_handler`, `RtlUnwind`. F.E.A.R. imports 347.
- `xam.xex`: **65** ordinals. `NetDll_*` (sockets, XNet, QoS), `XamInput*`, `XamLoader*` (launch title / launch data), `XamUser*` (XUID, name, sign-in state, privileges, profile settings), `XamContent*`, `XamEnumerate`, notifications, `XamShowSigninUI`, `XamShowMessageBoxUIEx`, achievements, `XamVoice*`, `XamSession*`, `XGetLanguage`, `XGetVideoMode`. F.E.A.R. imports 338.
- Without an implementation in the SDK source: `NetDll_XNetCreateKey/RegisterKey/UnregisterKey/QosLookup`, `XamShowPlayerReviewUI`, `XamShowGamerCardUIForXUID`, `XamUserCreateStatsEnumerator`, `XamVoiceSubmitPacket` (all Live/voice), `VdInitializeEDRAM` (a stub in Xenia), `DbgPrint`/`sprintf`/`_vsnprintf` (C runtime exports the regex missed) and the kernel variables (`XboxKrnlVersion`, `VdGlobalDevice`, `KeTimeStampBundle`, ...) which the loader provides rather than code. Nothing on the boot path.

## Codegen round 1 (2026-10-23, ReXGlue nightly `bd833a2`)
`rexglue codegen` on the bare manifest: analysis found **4 `UnresolvedCall` errors**, each a
`b` (tail call) to a target "not in any function":

| Target | From | What the target is (objdump on the decrypted image) |
|---|---|---|
| `0x82151A00` | `0x82145660` | virtual-call trampoline: loads vtable slot 0x50, `bctr`; followed by more of the same |
| `0x82145AA0` | `0x82145EA4` | 5-instruction accessor (`lwz`, `mulli`, `add`, `lbz`, `blr`) |
| `0x821472B8` | `0x82147314` | 2-instruction getter (`lhz`, `blr`), followed by `addi r3,-4; b` thunks (multiple-inheritance adjustors) |
| `0x82589048` | `0x82592D7C` | a real function body (compares, branches) |

All four are function starts the scanner skipped because nothing but a tail call reaches them.
Added to `[entrypoint.functions]` in `port/graw_manifest.toml`. Round 2 result: see
[HANDOFF.md](HANDOFF.md).

## Codegen rounds 2–3 and the first runs (2026-10-23)
- **Round 2** (the 4 entries added): codegen clean, 237 files, 121 MB of C++, 29,483 functions
  registered at start-up. First headless run: the runtime mounts `port/assets` as
  `\Device\Harddisk0\Partition1` with `game:` and `d:` symlinks, loads the XEX (probes for
  `default.xexp`, a title update, harmless), binds 65 + 133 imports, loads 38 achievement
  entries, prepares the module, then dies 1.7 s in: `[FATAL] Call to invalid or unregistered
  function at guest address 0x827AE040`. That address is a 5-instruction thunk in the static
  initialiser region (two `lis/addi` address loads and a tail call), reached through a pointer.
- `tools/find_missed_functions.py` on the decrypted image (method-table slots pointing at
  function boundaries, plus `lis/addi` address pairs built in code) listed **200 candidates**,
  `0x827AE040` among them (from a table at `0x827D1A14`).
- **Round 3** (all 200 added): codegen warned twice, `Unresolved b target 0x821F10E0 from
  0x821F1188` and `Unresolved conditional branch to 0x8224A284 from 0x8224A6B4`. Both mean a
  candidate landed *inside* a function and cut it: `0x821F116C` (pointed to from data at
  `0x82095D98`) and `0x8224A2DC` (from `0x820960D8`). The two words are data that happen to
  equal code addresses at instruction boundaries, not entries; dropped. 29,678 functions.
- Round 3 run: the game got further. It probes `game:\mods\origmiss\` and `game:\mods\dlc_cert\`
  (DLC/mod folders not on the disc: `NtCreateFile` fails cleanly), enumerates content (0 items),
  creates a notification listener, registers its GPU interrupt callback (`SetInterruptCallback`,
  so Direct3D is initialising), looks for the debug flag file `d:\CompareBackEnds`, then faults:
  `Unhandled guest access violation: read of guest 0x00000008` (a null-object dereference) with
  no stack in the log. Added fear360's crash reporter (`port/src/crash_report.*`) so the next
  fault names the recompiled function. Round 4 (the two splitters dropped) did not change the
  fault, so the splitters were not its cause.

## Round 4: the first real crash is inside the HLSL compiler (2026-10-23)
With the two splitters dropped and fear360's crash reporter linked in (`port/src/crash_report.*`),
the same fault gets a guest stack (`port/logs/crash-20261023-195041.txt`):

| Frame | Function | What it is (strings it references, disassembly) |
|---|---|---|
| #0 | `sub_822AC1E8` +0x37BE, generated line `graw_recomp.99.cpp:10890` | the parser's **semantic-action dispatcher**: a `switch` over 300 grammar rules through a 16-bit offset table at `0x82030000` (base `0x822AC2BC`); the rule's values are popped from a list at `8(r30)` into a 16-slot array at `176(r1)` after a `memset` to zero. Faulting instruction `0x822ACA00 lwz r11,8(r4)` in the case for **rule 79** (`li r4,79; li r5,2`: two values expected) with `r4 = value 0 = NULL`: the value list held fewer than two entries |
| #1 | `sub_822AD650` | the **yacc parser** ("syntax error", "yacc stack overflow"; its signed-halfword `lhax` table lookups are translated correctly, as `int16_t`) |
| #2 | `sub_822AE690` | parse driver |
| #3 | `sub_822498A0` | **D3DX shader compile**: "'%s': entrypoint not found", "unrecognized compiler target '%s'", "New back end failed: %08x. Old back end used instead", `d:\TestOldBE`, "Microcode comparison failed. Please send your HLSL code to " |
| #4 | `sub_8224A808` | compile wrapper |
| #5 | `sub_82226928` | the game's call into the compiler, with the macros `_XBOX_CPU`, `_XBOX_GPU_FINAL` |
| #6–#9 | `sub_82226B00`, `sub_82226F98`, `sub_82227010`, `sub_822272F0` | the game's shader/material setup |
| #10–#13 | `sub_823840D0` … `sub_82345B58` | renderer initialisation |
| #14–#19 | `sub_820DBA20` … `sub_820D0438` | engine start-up from `main` (`xstart`) |

So the game's first act after reading `data\strings_x.res` and `strings_x.txt` is to compile an
HLSL shader **held in memory** (no `.hlsl` or `.h` file was opened: trace log
`port/logs/trace-test.log`, run with `--log_level trace --log_noisy`), probing
`d:\CompareBackEnds` first and allocating a 1 MB arena. The parser then reduces rule 79 with an
empty value stack. On the console this path cannot crash, so either (a) the source text handed
to the compiler is not what the console sees (a pointer or length built from something our
runtime returns differently), or (b) an earlier semantic action failed silently here (an
allocation, a symbol lookup) and pushed nothing, or (c) a translation fault in the compiler's
code (the dispatcher and `lhax` are right; the lexer was not checked). Next: look at the text by
hooking `sub_82226928`'s entry and dumping its arguments (recipe in HANDOFF.md).

Also fixed this round: the crash reporter's log line still said "F.E.A.R."; renamed.

## Round 5: a misread jump table, then the start screen (2026-10-23)
**What the compiler was given.** A midasm hook just before the compile call at `0x82226A38`
(`port/src/shader_debug.cpp`) shows a D3DXCompileShader-shaped call: r6 = entry `"main"`,
r8 = profile `"vs_3_0"`, r9 = flags `0x100`, r4 = the define list (`_XBOX_CPU`,
`_XBOX_GPU_FINAL`). The source is an in-memory buffer, and the text was fine.

**Why the parser saw a NULL.** A second hook on every reduction (`0x822AE480`, before the call to
the semantic-action dispatcher, logging rule, values needed and values on the stack) showed rule
110 turning a valid node into NULL, rule 108 doing the same, then rule 81 dereferencing the NULL.
The generated `switch` in `sub_822AC1E8` was **shifted by two cases**: case N jumped where the
guest table's entry N−2 points (rule 81 ran rule 79's action, 110 ran 108's).

**Root cause in ReXGlue** (`src/codegen/function_scanner.cpp`, `detectJumpTable`, nightly
`bd833a2` and source `77d8b00`): scanning backwards from the `bctr`, the detector builds the
table address from `lis r12,0x8203` + `addi r12,r12,0` = `0x82030000`. Because the low 16 bits
are still zero, the branch `else if ((tableAddr & 0xFFFF) == 0)` treats it as "high half only"
and adds the next `addi` it meets further back, an unrelated `addi r10,r10,-4` at `0x822AC25C`.
The table was read from `0x8202FFFC`. Any jump table (or base) on a 64 KB boundary can hit this;
worth an upstream issue.

**Fix:** an `[[entrypoint.switch_tables]]` override with the 300 correct labels (keyed by the
`bctr` at `0x822AC2B8`, index register r27). `tools/check_switch_tables.py` replays every
generated switch from the instruction comments above it, recomputes the targets from the image
and writes overrides for mismatches: **466 switches, 1 wrong** before the override, 0 after.

**Then:** one more unregistered function, the qsort comparator `0x825C6000` that the compiler's
back end passes by address. It follows a never-returning `bl`, so the previous function swallowed
it. `tools/find_missed_functions.py` gained an "after a bl" pass: 3 hits, 1 real (the other two,
`0x821FB5F4` and `0x825C0004`, are return points). The first version of the debug hook then
crashed on its own, following a float in r10 into unmapped memory; it now checks pages first and
is off by default (`--graw_debug_shader_compile` turns it on).

**Result:** the start-up shader compiles succeed (`main` vs/ps, `FontVertexShader`,
`FontPixelShader` and more), the D3D12 Xenos plugin creates a 1280×2048 colour and depth target
and a dozen pipelines, the controller connects, and **the game reaches its start screen**. The
user pressed Start, entered the campaign and reached its **loading screen**, where the game ended
(section below).

## The campaign "crash" at the loading screen was a hand-off (2026-10-23)
Session `port/logs/smoke-20261023-202516.log` (a smoke run the user played in: `--headless`
still shows the window) ended 35 s after the controller connected with
`KernelState::TerminateTitle` and a `0xC0000374` exit. The trace run that followed
(`port/logs/run-20261023-203813.log`, 66 MB, `tools/launch.ps1 -Trace`, Start then Campaign) ends
with a 3.6 MB `MmAllocatePhysicalMemoryEx` (the loading screen) and `TerminateTitle` 4 ms later:
no file error, no exception. In ReXGlue only three things call `TerminateTitle`: closing the
window (which logs its own message, absent here), `XamLoaderTerminateTitle` and
`XamLoaderLaunchTitle`, and neither of those logs.

The shader-header messages are harmless. The trace shows the include handler first probing the
bare name with `NtQueryFullAttributesFile("HDRToneMap.h")` (no device, fails), then opening
`game:\data\HDRToneMap.h`, which succeeds and is read. Each header logs one error and one warning
per include.

## The disc's three programs (2026-10-23)
`default.xex` calls `XamLoaderLaunchTitle` from one function, `sub_822012A8` (which also checks for
the `XLNI_DET_MEDIA` / `XLNI_DASH_ARCADE` names). Its four callers pass `gr3xenon_release.exe`
(from `sub_82170EE0`, after `XamLoaderSetLaunchData`), `Yeti_Final_Xenon.exe` (no launch data set
at that site) and `default.xex` (twice). Which menu item leads where was settled by the user's
session through the hand-off (`port-sp/logs/smoke-20261023-204822-restart.log`): four picks of
**Campaign** requested `Yeti_Final_Xenon.exe` (16 bytes of launch data), and **Instant Action**
requested `gr3xenon_release.exe` (15 bytes) and loaded a co-op "Defend" mission.

| Executable | Engine | What it is | Project |
|---|---|---|---|
| `default.xex` | Red Storm's GR3 | the front end: start screen, menus (`PressStartLaunch`, `magma_launcher.glb`, `L_MainMenu`) | `port/`, `graw.exe` |
| `GR3Xenon_Release.exe` | Red Storm's GR3 | Instant Action, co-op and multiplayer (the `data/` maps: arroyo, boneyard, temple, ...) | `port-mp/`, `grawmp.exe` |
| `Yeti_Final_Xenon.exe` | Ubisoft Paris's Yeti | the single-player campaign (`Yeti.big`, the `Sound_Bin/*.SB4/.SS4` banks, `NewYetiShaders*`) | `port-yeti/`, `grawyeti.exe` |

`default.xex` and `GR3Xenon_Release.exe` share the engine and the same 65 + 133 imports but are
different builds: the decrypted images differ in 2,035,033 of 2,555,904 words, with code shifted
throughout. Yeti imports 38 XAM and 134 kernel functions; all have SDK implementations apart from
the loader-provided variables and C formatting helpers. ReXGlue records a launch request in
`XamModule::LoaderData::launch_path` and nothing reads it, so the process just ends.

**The hand-off** (`port/src/graw_loader.*`): when the main guest thread ends with a launch path,
the host writes the launch data and flags to a temporary file and starts the program holding
that executable's translation with this run's arguments and its own log file
(`<first log>~<program>-<time>.log`), after this process exits (`graw_process.*`, from fear360).
The new program loads the launch data back into its XAM loader data before its module starts.
A program that isn't built sends the player back to the front end. Started alone, the GR3
program asks for `default.xex` with 16 bytes of launch data, as it would on a console.

**The GR3 translation** (`port-mp/`): the same recipe as the front end, applied before the first
run: 4 tail-call targets, 200 method-table and code-built entries, the qsort comparator after a
never-returning `bl` (`0x825C5A90`; four other after-`bl` hits are mid-function), two splitters
dropped (`0x82233C7C`, `0x822B84CC`). All 466 switches check out, because this build's parser
table isn't on a 64 KB boundary.

## Open: Instant Action draws a black 3D view (2026-10-23)
The user's clip (`Desktop 2026.10.23 - 20.50.58.04.mp4`): a co-op Defend mission runs, with the
HUD (timer, ammo, grenades, compass, kill feed "PLAYER TWO" / "Rebel", enemy markers), the
tactical map overlay, the pause menu (In-Game Options, Abort Mission, Multiplayer Menu) and the
**drone camera picture-in-picture showing terrain**. Everything behind the HUD is black. The log
has no GPU errors. So 3D rendering into a texture works and the main view doesn't. Candidates:
the HDR path (tone map / final blit into the front buffer), the depth range (Xenia lists the game
as `requires_force_depth_clamp`; ReXGlue's plugin has no such option), or a render-target format.
Plugin options to try: `--render_target_path_d3d12 rov` (Xenia testers mention rov/rtv for this
game), `--gamma_render_target_as_unorm16`, `--depth_float24_convert_in_pixel_shader`.

## The campaign program (Yeti) and its FP exception (2026-10-23)
`port-yeti/` translates `Yeti_Final_Xenon.exe` (`grawyeti.exe`): 4 tail-call targets, 100
table/code entries, the qsort comparator after a `bl` (`0x82502C90`), one getter tail-called from a
new thunk (`0x823E2F90`), 2 splitters dropped (`0x823FD544`, `0x82431D84`); 177 generated files,
all 388 switches check out.

Its first runs died after a second on the audio worker thread with `C000008F` (floating-point
inexact result) and `MXCSR = 0x0020`: every FP exception unmasked. ReXGlue keeps each context's
host MXCSR in a cached value (`ctx.fpscr.csr`) and writes the whole value to the CPU whenever the
translated code changes the flush mode (`enableFlushMode` / `disableFlushMode` around vector
code: ~21,000 sites in Yeti) or the rounding mode (`mtfsf` -> `storeFromGuest`). Guest threads
seed that value with `InitHost()` (all exceptions masked); the context the audio worker uses for
the game's mixer callback (`0x82328998` here) is never seeded, so its value is 0 and the mixer's
first vector op writes MXCSR = 0. The front end and the GR3 program have the same exposure
(callbacks `0x82593CA0`, `0x82593700`) but their mixers didn't trip it. Fix
(`port/src/fpscr_guard.cpp`): a `GrawGuardFpscr` hook that runs `InitHost()` on the current
context, at each program's audio callback entry and before each of its five `mtfsf` sites. Worth
an upstream fix in the SDK's audio worker.

With that, `grawyeti.exe` started alone boots to the game's **Campaign menu** (Play game, New
game, Load game, Tips, Missions, Statistics, Credits; capture
`private/captures/grawyeti-20261023-210417.png`).

## Controllers, sign-in, storage and Live (2026-10-23)
The user asked whether the game is stuck looking for an Xbox hard drive or Xbox Live. Evidence so
far: none of the three programs logs a failed sign-in, content or network call, and none imports
a device selector (the game uses the default storage); the stops were the hand-off and the FP bug.
Two related things did show up:
- **Controller slot.** SDL lists the Logitech virtual pad (`046D:0000`, "XInput Controller #1")
  as controller 0 and the Xbox Series controller as 1. ReXGlue signs in only player 1, so the
  user played as an unsigned player 2 ("PLAYER TWO" in the Instant Action kill feed); in F.E.A.R.
  that made saving fail. Ported fear360's fix: `graw_shared_controllers` (default on), every
  controller drives player 1 (`SharedAssignment`).
- **`D:\Dummy`.** Yeti opens `D:\Dummy` for writing at start-up and gets `0xC0000022` (access
  denied: the VFS doesn't allow writes to `game:`/`d:`). It looks like a writable-drive probe;
  check what Yeti does with the result when saving (New game, then a checkpoint, then Load game).

## Cross-Com and the narration panel are black (#2)
- The campaign opens and reads each Cross-Com `.bik` in full, and the clip's audio plays, so
  decoding runs. Ruled out on the stock renderer: `--readback_resolve`, invalid fetch constants.
- fear-nx's native Vulkan renderer (`port-yeti-nx`, 2026-10-23) did not fix it: still black, and
  the rest of the game broke (over 1,000 empty texture lookups per 10 s, an unhandled 2048x2048
  format 23 texture, stretches at 9 fps). Not the way forward; kept local only.
- Its log shows how the Cross-Com video reaches the GPU: two sets of three `k_8` (format 2)
  planes, one 160x120 and two 80x64 (Y, Cr, Cb; no alpha plane), at `0x0FEE4000`/`E8000`/`EC000`
  and `0x0FEF4000`/`F8000`/`FC000`, re-uploaded several times a second. So the CPU-decoded frames
  land in texture memory; the black comes later.
- The campaign has no precompiled shaders. Bink's shaders are HLSL text in `.rdata`
  (`yeti.bin` file offsets `0x41CD0` pass-through vertex shader, `0x41EF8` three-plane and
  `0x42318` four-plane pixel shaders) that the game's own D3DX compiler (recompiled, like the
  front end's) compiles at runtime. The pixel shader samples `.x` of each plane into
  `c = (Y, Cr, Cb, consts.x)` and outputs `dot(tor|tog|tob, c)` with constants `c0`-`c3`. Black
  means the planes sample as 0, the constants are 0, or the compiled microcode is wrong.
- The compiled Bink shader is correct. `--dump_shaders` wrote nothing (the renderer loads its
  shaders from `port/cache/shaders/shareable/555307D5.xsh` instead), so the stored microcode was
  read from that file (big-endian dwords after a 12-byte header per shader). Pixel shader
  `E4F08314542740C2`: `tfetch` tf0/tf1/tf2 into r1.x/y/z, `maxs` puts c3.x in r1.w and c3.w in
  oC0.w, `dp4` c0/c1/c2 with r1 into oC0.x/y/z. That matches the HLSL, so the game's recompiled
  D3DX compiler is not the fault.
- The game's Bink code (Yeti_Final_Xenon.exe):
  - `sub_823409C8` compiles the three shaders at start-up and keeps them at `0x828F18B4`
    (three-plane), `0x828F18B8` (four-plane), `0x828F18BC` (vertex); the device is `*0x828F18B0`.
  - `sub_82340EE8` draws a frame: the three planes on samplers 0-2, the YUV constants from a
    table at `0x825A9120` into c0-c3, then a pixel-space quad (the pass-through vertex shader,
    with the viewport transform switched off around it).
  - `sub_823400A0` is the video object's draw: if the byte at +24 is set it calls
    `sub_82340EE8`; **otherwise it clears the box to black** (`0x824CDCC8`, flags 15).
  - `sub_82340648` is the update: BinkDoFrame (`0x825B8CE0`) / BinkNextFrame (`0x825B9280`)
    until BinkWait (`0x825B8348`) returns non-zero, then sets +24; BinkGetRealtime
    (`0x825B86E8`) under 25 % of the read buffer changes the state at +40 (a 4-entry switch).
  So the box is black either because +24 never sets (Bink's timer or buffering state), or
  because the draw lands somewhere the HUD doesn't show.
- `src/bink_probe.cpp` (port-yeti hooks, `--graw_debug_bink`) logs each video object's state on
  change and per 2 s: updates, draws with and without a frame, BinkWait results, ready sets,
  frame number, read buffer.
