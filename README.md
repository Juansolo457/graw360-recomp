# GRAW 360 Recompiled

The Xbox 360 **Tom Clancy's Ghost Recon Advanced Warfighter** (2006), the console-exclusive
Red Storm version with the third-person camera and the Cross-Com drone, statically recompiled to
run natively on PC.

> Unofficial. **No game data included.** Bring your own Xbox 360 disc. Releases, when there are
> any, are source only: you build from your own disc.

## What it is

A **static recompilation**: the game's PowerPC code is translated to x86-64 C++ ahead of time with
[ReXGlue](https://github.com/rexglue/rexglue-sdk), and a game-specific runtime stands in for the
Xbox. Not an emulator, not a decompilation. The same approach and toolchain as
[fear360-recomp](https://github.com/Juansolo457/fear360-recomp); the project conventions borrow
from [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp).

## Status

**Boots, with work left in each mode.** The disc holds three programs, each recompiled
separately, and the front end hands over between them as the console did:

- the front end (start screen, menus) works;
- Instant Action and co-op load and play, but the 3D view is still black (HUD, menus and the
  drone camera draw);
- the campaign program boots to its Campaign menu; starting a mission is next.

- [docs/PLAN.md](docs/PLAN.md): the plan to the title screen, phases, exit criteria, risks
- [docs/FINDINGS.md](docs/FINDINGS.md): what the disc and the executable are
- [docs/HANDOFF.md](docs/HANDOFF.md): current state, how to build and run, where things live

## Build and run (Windows)

```powershell
pwsh -NoProfile -File tools/extract.ps1 -Iso 'D:\path\to\your\graw.iso'   # ~4.8 GB into port/assets
pwsh -NoProfile -File tools/build.ps1                                        # front end: codegen + build
pwsh -NoProfile -File tools/build.ps1 -Project port-mp                       # Instant Action / multiplayer
pwsh -NoProfile -File tools/build.ps1 -Project port-yeti                     # campaign
pwsh -NoProfile -File tools/smoke.ps1 -Seconds 60 -Captures 3               # headless boot check
pwsh -NoProfile -File tools/launch.ps1 -Windowed -Scale 1                    # run it
```

Supported executable: `default.xex` title `555307D5`, SHA-256 `295ac4de…c023a` (USA/Europe
multilingual disc). The toolchain (ReXGlue SDK, LLVM 20, CMake, Ninja) is reached through the
`.tools/` junction; see [docs/HANDOFF.md](docs/HANDOFF.md).

## Layout

```
port/        the front end (default.xex): manifest, src/ (host code shared by all three), assets/ (your disc, ignored)
port-mp/     Instant Action, co-op and multiplayer (GR3Xenon_Release.exe)
port-yeti/   the campaign (Yeti_Final_Xenon.exe)
tools/       extract, build, launch, smoke, window capture, XDVDFS reader, XEX inspector/decryptor, import survey
docs/        plan, findings, handoff
private/     your captures, logs and decrypted images (ignored)
```

## Legal

- No assets, executables, generated code, keys or proprietary code are distributed. Never
  attach builds or game files to a release. Before pushing, run `python -I tools/audit_repo.py`
  (CI runs it too).
- Three sets of terms, file by file in [NOTICE](NOTICE), full texts in [LICENSES/](LICENSES/):
  - **MIT** ([LICENSE](LICENSE)): our own tools, docs and glue.
  - **GPL-3.0-or-later** ([LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)):
    `port-yeti-nx/src/native/` (fear-nx ← Project8Recomp ← nfsmw-nx); the port-yeti-nx build
    that links it is GPL.
  - **BSD-3-Clause**: ReXGlue (with Xenia's notices), its `rexglue init` scaffolding and our SDK
    patches in `tools/patches/`.
- Ghost Recon is a trademark of Ubisoft Entertainment. Not affiliated with Ubisoft, Red Storm
  Entertainment, Ubisoft Paris or Microsoft.

## Credits

- Red Storm Entertainment and Ubisoft Paris: the game
- Tom Clay and contributors: [ReXGlue](https://github.com/rexglue/rexglue-sdk); the [Xenia](https://github.com/xenia-project/xenia) team
- Inspiration: [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp), [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), [fear-nx](https://github.com/infriker/fear-nx)
- Built with [Claude Code](https://claude.com/claude-code)
