# Handoff: GRAW 360 on Windows (2026-10-23, day 1)

Read [PLAN.md](PLAN.md) for the phases and exit criteria and [FINDINGS.md](FINDINGS.md) for the
disc and executable facts. This file is the state, the build/run detail and where things live.
Update it at the end of every session; the next model starts here, not from chat history.

## Where it stands
**Phase 1 done, Phase 2 started.** `graw.exe` builds from the first translation (29,483
functions, 237 generated files, 121 MB of C++, ~3 minutes of compile on 14 jobs). The runtime
starts: VFS mounted (`game:` and `d:` both point at `port/assets`), XEX loaded, 133 + 65 imports
bound, shader storage initialised. The first headless run then died 1.7 s in with
`Call to invalid or unregistered function at guest address 0x827AE040`: a function the analyser
missed, reached through a pointer. That is the expected Phase 2 loop (manifest → codegen →
smoke), and the tooling for it is in place.

| Round | Manifest | Result |
|---|---|---|
| 1 | bare `rexglue init` | codegen: 4 `UnresolvedCall` errors (tail-call targets; FINDINGS.md) |
| 2 | + those 4 | codegen clean, `graw.exe` links; smoke: fatal at `0x827AE040` after 1.7 s |
| 3 | + 200 candidates from `tools/find_missed_functions.py` (includes `0x827AE040`) | see the next section |

### Round 3 and after (fill in)
- Build log: `private/build-third.log`. Look for `Unresolved conditional branch` warnings: each
  names a candidate that is really a label inside a function; remove it from the manifest.
- Then `tools/smoke.ps1 -Seconds 75` again. Expected next stops, in order: another missed
  function (add it; rerun the finder if the game got further), a VFS miss (`VFS: entry not found`
  lines are harmless probes unless the game then fails), a kernel import the SDK stubs badly, the
  GPU plugin's first frame.

## How it is built and run
```
pwsh -NoProfile -File tools/extract.ps1 -Iso 'D:\x360-recomps\GRAW 1\<disc>.iso'   # once, ~4.8 GB
pwsh -NoProfile -File tools/build.ps1 [-Jobs 14]        # codegen when the manifest/XEX changed, then build
pwsh -NoProfile -File tools/smoke.ps1 -Seconds 75 [-Captures 3]   # headless; log in port/logs/smoke-*.log
pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1         # a window on the user's screen: say so first
pwsh -NoProfile -File tools/capture_window.ps1 -ProcessName graw  # screenshot even when covered
```
- Toolchain: `.tools/` is a junction to `../n3-360-recomp/reference/N3-Recomp/.tools` (ReXGlue
  SDK nightly `0.10.0.24-dev.gbd833a2`, LLVM 20.1.8, CMake 3.31.8, Ninja). `build.ps1` enters a
  VS 2022 DevShell (vcvars64 breaks on a quoted PATH entry), checks the XEX hash, runs
  `graw_codegen`, reconfigures once on the first run (the recomp target only exists after the
  sources do), then builds `port/out/build/win-amd64-relwithdebinfo/graw.exe` with
  `rexruntimerd.dll` and `rexgpu-xenosrd.dll` beside it.
- The stock SDK is a prebuilt package (D3D12 only on Windows). If the renderer has to change
  (depth clamp, menu corruption), the recipe is fear360's `port-nx`: build fear-nx's patched SDK
  from source and link the Xenos plugin statically; see `../fear360-recomp/docs/HANDOFF.md`.
- `port/src/graw_app.h` sets `config.gpu_plugin = "xenos"` in `OnPreSetup` (without it the
  screen stays black; F.E.A.R. lesson).

## Analysis tools (all read-only on the game's files)
| | |
|---|---|
| `tools/xdvdfs.py <iso> list \| extract <dir> [names]` | XDVDFS reader; `tools/extract.ps1` wraps it |
| `tools/xexinfo.py <xex>` | XEX2 headers: title, build time, entry, image size, encryption, XDK libraries |
| `tools/xex_basic_decrypt.py <xex> <out.bin>` | the decrypted, decompressed image at `0x82000000` (basic compression only). Ours: `private/xex/graw.bin`, 10,223,616 bytes |
| `tools/xeximports.py <xex> <image.bin> [<rexglue-sdk src>]` | kernel/XAM import ordinals, named and checked against the SDK's export tables; the SDK source used was `../fear360-recomp/third_party/rexglue-sdk-nx` |
| `tools/find_missed_functions.py <image.bin> port/generated/default` | function entries the analyser missed (method tables, callbacks built in code), as manifest lines; from fear360 |
| `../fear360-recomp/third_party/rexglue-sdk-nx/tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc:common -EB --adjust-vma=0x82000000 --start-address=0x… --stop-address=0x… private/xex/graw.bin` | disassemble a range of the image |
| `private/xex/graw.strings.txt` | every printable string of 6+ characters in the image (regenerate with a 3-line Python scan) |

## Where things live
| | |
|---|---|
| `port/graw_manifest.toml` | project + entrypoint config and `[entrypoint.functions]`, with a comment per round saying where each group came from |
| `port/src/graw_app.h`, `src/main.cpp` | host app; game-specific host code goes next to them (fear360's `fear_*.cpp` are the pattern: crash reports, controller sharing, settings panel) |
| `port/generated/rexglue.cmake`, `CMakePresets.json` | `rexglue init` output (BSD, tracked); `generated/default/` is the translation (ignored) |
| `port/assets/` | the extracted disc (ignored): `default.xex`, `data/`, `video/`, `Sound_Bin/`, `Language/`, `Yeti*` |
| `port/logs/`, `port/user-smoke/`, `port/cache-smoke/` | smoke runs; `port/user/`, `port/cache/` for `launch.ps1` |
| `private/` | `game/` (both executables, decrypted image), `xex/` (image, strings, imports), `build-*.log`, `missed-round*.txt`, `captures/` |
| `docs/PLAN.md`, `FINDINGS.md`, this file | plan, facts, state |

## Known traps (from the sister projects)
- A Logitech virtual XInput device takes controller slot 0 on this PC, making the Xbox pad
  player 2 with no profile: Press Start then does nothing useful. Disable that device, or port
  fear360's `SharedAssignment` (`fear_app.h`, `fear_shared_controllers`).
- `--resolution 720p` tells the guest it is on a 720p display; without it ReXGlue squeezes to
  4:3 at 200% DPI. The launch and smoke scripts pass it.
- Codegen does not rerun on its own after a manifest edit in a plain `cmake --build`;
  `build.ps1` always runs the `graw_codegen` target first.
- `python -I` for every script that reads game files. Windows paths for Python arguments
  (`D:\...`), not Git Bash `/d/...` ones.

## Licensing reminders
Our files are MIT (`LICENSE`, `NOTICE`); ReXGlue is BSD-3-Clause with Xenia's notices; if
fear-nx's renderer is ever brought in, that directory and any build linking it are GPL-3.0.
Never commit game data, generated translations, captures or saves. Releases are source only.
