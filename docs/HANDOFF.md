# Handoff: GRAW 360 on Windows (2026-10-23, day 1)

Read [PLAN.md](PLAN.md) for the phases and exit criteria and [FINDINGS.md](FINDINGS.md) for the
disc and executable facts. This file is the state, the build/run detail and where things live.
Update it at the end of every session; the next model starts here, not from chat history.

## Where it stands
**Phase 1 done, Phase 2 in progress.** `graw.exe` builds from the translation (29,678 functions,
237 generated files, 121 MB of C++, ~3 minutes of compile on 14 jobs, codegen clean). The
runtime starts, mounts the disc (`game:` and `d:` → `port/assets`), loads the XEX, binds the
198 imports, and the game runs its start-up: it reads `data\strings_x.res` and `.txt`, probes
for mods and DLC, enumerates content, registers its GPU interrupt callback, then **crashes
inside the D3DX HLSL compiler linked into the game**, compiling an in-memory shader (a null
value on the parser's value stack at grammar rule 79). Full analysis: [FINDINGS.md](FINDINGS.md),
"Round 4". Crash reports with guest stacks land in `port/logs/crash-*.txt`.

| Round | Manifest | Result |
|---|---|---|
| 1 | bare `rexglue init` | codegen: 4 `UnresolvedCall` errors (tail-call targets) |
| 2 | + those 4 | codegen clean, `graw.exe` links; run: fatal "unregistered function" at `0x827AE040` after 1.7 s |
| 3 | + 200 candidates from `tools/find_missed_functions.py` | 2 codegen warnings (two candidates split functions); run: null read at guest `0x8`, no stack |
| 4 | − the 2 splitters (`0x821F116C`, `0x8224A2DC`); crash reporter added | same null read, now with a stack: the HLSL compiler's parser |

### Next step: see what the game asks the compiler to compile
1. Add a hook at the entry of `sub_82226928` (the game's call into the compiler; its caller
   `sub_82226B00` is renderer setup) that logs the arguments. In `port/graw_manifest.toml`,
   inside the entrypoint table (a top-level `[[midasm_hook]]` is silently ignored):
   ```toml
   [[entrypoint.midasm_hook]]
   name = "GrawDumpShaderCompile"
   address = 0x82226928          # or the bl into sub_8224A808 inside it, once r3..r8 are set up
   registers = ["r3", "r4", "r5", "r6", "r7", "r8"]
   ```
   and `void GrawDumpShaderCompile(PPCRegister& r3, …)` in a new `port/src/shader_debug.cpp`
   (fear360's `port-nx/src/anim_timing.*` is the pattern; `#include <rex/ppc/context.h>`,
   guest pointers are offsets from the runtime's memory base). D3DXCompileShader takes
   (source, length, defines, include, entry, profile, flags, …): disassemble `sub_82226928`
   first (recipe below) to see which register carries the text, then log the text and the
   entry/profile strings.
2. If the text is intact HLSL: the fault is the compiler's own path under our runtime. Log every
   reduction before rule 79 (a hook at `0x822AE47C`, the call into the dispatcher, with `r4` and
   `r5`) to find the rule that pushed nothing, and read its action.
3. If the text is wrong or empty: follow the pointer back through `sub_82226B00` to where it is
   built (likely `rendererdata.glb` or a table in the image) and see which kernel result it
   depends on.
4. Then `tools/build.ps1` and `tools/smoke.ps1 -Seconds 75` again. After the compiler, the
   expected stops are the ones in PLAN.md Phase 2: VFS, kernel stubs, the first GPU frame.

## How it is built and run
```
pwsh -NoProfile -File tools/extract.ps1 -Iso 'D:\x360-recomps\GRAW 1\<disc>.iso'   # once, ~4.8 GB
pwsh -NoProfile -File tools/build.ps1 [-Jobs 14]        # codegen when the manifest/XEX changed, then build
pwsh -NoProfile -File tools/smoke.ps1 -Seconds 75 [-Captures 3]   # headless; log in port/logs/smoke-*.log
pwsh -NoProfile -File tools/smoke.ps1 -Seconds 30 -Extra '--log_level trace --log_noisy'  # every kernel call with arguments
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
- `port/src/graw_app.h` installs the crash reporter and sets `config.gpu_plugin = "xenos"` in
  `OnPreSetup` (without the plugin the screen stays black; F.E.A.R. lesson).

## Analysis tools (all read-only on the game's files)
| | |
|---|---|
| `tools/xdvdfs.py <iso> list \| extract <dir> [names]` | XDVDFS reader; `tools/extract.ps1` wraps it |
| `tools/xexinfo.py <xex>` | XEX2 headers: title, build time, entry, image size, encryption, XDK libraries |
| `tools/xex_basic_decrypt.py <xex> <out.bin>` | the decrypted, decompressed image at `0x82000000` (basic compression only). Ours: `private/xex/graw.bin`, 10,223,616 bytes |
| `tools/xeximports.py <xex> <image.bin> [<rexglue-sdk src>]` | kernel/XAM import ordinals, named and checked against the SDK's export tables; the SDK source used was `../fear360-recomp/third_party/rexglue-sdk-nx` |
| `tools/find_missed_functions.py <image.bin> port/generated/default` | function entries the analyser missed (method tables, callbacks built in code), as manifest lines; from fear360. Candidates that split a function show up as codegen `Unresolved …` warnings: find the entry between the warned addresses and drop it |
| `../fear360-recomp/third_party/rexglue-sdk-nx/tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc:common -EB --adjust-vma=0x82000000 --start-address=0x… --stop-address=0x… private/xex/graw.bin` | disassemble a range of the image |
| `private/xex/graw.strings.txt` | every printable string of 6+ characters in the image (a 3-line Python regex scan) |
| the generated code | `port/generated/default/graw_recomp.N.cpp`: every PowerPC instruction is a comment above its translation, so a crash report's file:line gives the guest instruction; `lis/addi` pairs in a function name the strings it uses |

## Where things live
| | |
|---|---|
| `port/graw_manifest.toml` | project + entrypoint config and `[entrypoint.functions]`, with a comment per round saying where each group came from |
| `port/src/graw_app.h`, `src/main.cpp` | host app: Xenos plugin, crash reporter; game-specific host code goes next to them (fear360's `fear_*.cpp` are the pattern: controller sharing, settings panel) |
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
- Codegen does not rerun on its own after a manifest edit in a plain `cmake --build`;
  `build.ps1` always runs the `graw_codegen` target first.
- `python -I` for every script that reads game files. Windows paths for Python arguments
  (`D:\...`), not Git Bash `/d/...` ones. Long heredocs with backslashes get mangled by the
  Bash tool here; write scripts to a file first.

## Licensing reminders
Our files are MIT (`LICENSE`, `NOTICE`); ReXGlue is BSD-3-Clause with Xenia's notices; if
fear-nx's renderer is ever brought in, that directory and any build linking it are GPL-3.0.
Never commit game data, generated translations, captures or saves. Releases are source only.
