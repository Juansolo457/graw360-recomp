# Findings: Ghost Recon Advanced Warfighter (Xbox 360)

From the user's own disc. Hashes, names, sizes and addresses only, never content. Everything
here applies to `default.xex` SHA-256 `295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83`.

## Dump and executables (2026-10-23)
| | |
|---|---|
| ISO | 7,834,892,288 bytes (Redump-verified USA/Europe multilingual disc); XDVDFS game partition at `0x0FD90000`; 1,038 files, 4.76 GiB |
| `default.xex` | 8,826,880 bytes, title `555307D5`, media `617C5216`, v0.0.0.1, built 2006-02-23 16:40 UTC, original PE `default.pe`, load `0x82000000`, image `0x9C0000`, entry `0x82200FE0`, encrypted + basic compression; decrypted image 10,223,616 bytes |
| `GR3Xenon_Release.exe` | same size and headers, built two minutes earlier (16:38), entry `0x821EB308`, different content throughout: a sibling build left on the disc. The console loads `default.xex`; so do we |
| `Yeti_Final_Xenon.exe` | 6,922,240 bytes, same title ID, built 2006-02-16, entry `0x82402660`, image `0x950000`. Ubisoft Paris's Yeti engine: the multiplayer game, with `Yeti.big` (1.29 GB), `NewYetiShadersX360.sdf/.sat` and `NewShaderlist.txt` |
| XDK | 2.0.**2417** (LIBCPMT, XAPILIB .4, XBOXKRNL, XGRAPHC, XONLINE, D3D9, D3DX9, XACT, XAUD, X3DAUD, XHV). Yeti adds XMP and the LTCG audio libs |
| Data | `data/` 1.6 GB: 10 maps as `*_map.glb` + `*_map_asynch.glb` (Red Storm's GLB packages), `.mis` missions, `.xpr` textures (intel, mapshots, env maps), `.xwb/.xsb` XACT banks per mission and language, `.bik` menu backgrounds and the 411 MB attract movie; `video/` ~300 Bink cutscenes and tips; `Sound_Bin/` 1.5 GB of `.SB4/.SS4` (Yeti's sound banks, multiplayer); `Language/*.str`; `$SystemUpdate/su20076000` |
| Shaders | `data/*.hlsl` and `data/ShaderFragmentsModel3/*.h`: **HLSL source on the disc**; the image contains the XGraphics microcode compiler (`d:\xenon\nov05\...\xgraphics\ucode\compiler\...` source paths) and a `game:\shader_cache\` path |

## Engine identification (from the decrypted image's strings)
- Red Storm's GR3 Xenon engine: `CLIENT.Ph.RedStorm_GhostRecon3-Xenon`, `D:\GR2_GraphableMemory_%d.log`, RTTI for Red Storm's networking (`RSNetwork`, `RSRNP*` reliable UDP, `RSXbLink*` system link), `IkeStateMgr` saves, `PersistentActor` `.cms` files.
- Middleware: Havok ("Havok Physics evaluation key" strings, `ragdoll.hkx`, `treadsmotion.dat`), Bink (`BINKDATA`, `.bik` names), XACT (`cortez.xgs`, `gr_*.xwb/.xsb`), zlib/libpng/libjpeg, a "Magma" UI and package system (`MagmaManager.cpp`, `magma.glb`, `magma_launcher.glb`, `MagmaPackages`, `Magma Font`).
- Boot flow by asset name: `UbiLogo.bik`, `ESRB.bik`, `CortezIntro_US/Euro.bik` → "Launcher" front end (`launcher_art`, `launcher_art_sp`, `PressStart`, `PressStartLaunch`, `Menus_LauncherLoad`, `Mus_Menu_Launcher`, `gr_static_launcher_bootstrap.xwb`) → `L_MainMenu` (`MM_Background.bik`, `SP_Background.bik`, `MP_Background.bik`).
- Multiplayer hand-off: the string `Yeti_Final_Xenon.exe` is in the main image, and `XamLoaderLaunchTitle` / `XamLoaderSetLaunchData` are imported.
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
