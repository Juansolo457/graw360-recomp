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
