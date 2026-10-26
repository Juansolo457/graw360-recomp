# Build and play from your own disc (Windows)

This project ships **source only**. No download contains the game. The game's code is
translated from your copy of the Xbox 360 disc on your own PC, and the game data comes from the
same disc. You need your own disc of **Tom Clancy's Ghost Recon Advanced Warfighter** for the
Xbox 360.

The first setup and build take 30–60 minutes, most of it compiling. After that, rebuilds take a
minute or two.

## 1. What you need

**Hardware**
- A 64-bit CPU with AVX2 and FMA: Intel Haswell (2013) or AMD Excavator (2015) and newer. The
  game stops with a message on older CPUs.
- A GPU with a current Direct3D 12 driver (NVIDIA, AMD or Intel).
- About 12 GB of free disk space (toolchain, SDK source, three builds, the extracted disc).
- An Xbox controller, plugged in before you start.

**Software**
- Windows 10 or 11, 64-bit.
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) (Community or Build Tools)
  with the **Desktop development with C++** workload (MSVC v143 and a Windows 11 SDK). The build
  uses its libraries and command environment; the compiler itself is LLVM.
- [Git for Windows](https://git-scm.com/download/win).
- [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows)
  (`pwsh`).
- [Python](https://www.python.org/downloads/) 3.10 or newer, on PATH.

## 2. Get the code and set up the toolchain

```
git clone https://github.com/Juansolo457/graw360-recomp
cd graw360-recomp
pwsh -NoProfile -File tools/setup.ps1
```

`setup.ps1` does three things and skips any that are done already:

1. It downloads the toolchain into `.tools/`, each file from its official release and checked
   against its SHA-256:

   | Tool | Version | Ends up as |
   |---|---|---|
   | [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20261002-bd833a2a) | nightly 0.10.0.24-dev.gbd833a2 | `.tools/rexglue-sdk/win-amd64` |
   | [LLVM](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.8) | 20.1.8 | `.tools/llvm` (its installer runs silently) |
   | [CMake](https://github.com/Kitware/CMake/releases/tag/v3.31.8) | 3.31.8 | `.tools/cmake/cmake-3.31.8-windows-x86_64` |
   | [Ninja](https://github.com/ninja-build/ninja/releases/tag/v1.13.1) | 1.13.1 | `.tools/ninja` |

2. It clones ReXGlue's source at the same commit into `third_party/rexglue-sdk-bd833a2`. Then it
   applies this project's patches from `tools/patches` and checks the result against the exact
   source the release was built from. The patches:
   - `vpkd3d128-alias`: a code generator fix; without it the tactical map and the Cross-Com feed
     draw off screen ([TACMAP.md](TACMAP.md)).
   - `freed-page-resolve`: the HUD textures no longer show stale images
     ([BORDER.md](BORDER.md)).
   - `vblank-pacing`: even frame pacing.
   - `gpu-skip-draws`: lets the campaign's native renderer take over drawing.
   - pgr4-native's `0021` and `0022`: shader translator changes the native renderer is built
     against.
   - `implicit-gradients-switch`: lets `--graw_native_implicit_gradients=false` turn 0021 off
     (a Steam Deck shimmer test).
3. From that source, it builds the patched code generator and the campaign's renderer plugin.

## 3. Extract your disc

You need a disc image (ISO) of the game. A normal PC drive can't read Xbox 360 game discs:
dump your own disc with a drive and tool made for it. Then extract it:

```
pwsh -NoProfile -File tools/extract.ps1 -Iso "<your GRAW ISO>"
```

This unpacks the disc into `port/assets` (about 4.8 GB). The campaign and Instant Action use the
same files through links. The build checks each executable and accepts only the USA/Europe
multilingual disc, title `555307D5`, the version the translation was made for:

| Program | File | SHA-256 |
|---|---|---|
| Front end | `default.xex` | `295ac4ded72c95bb86e9d9441828bb5b193f863afb5a20a993275e01144cdb83` |
| Campaign | `Yeti_Final_Xenon.exe` | `32735d0908bd45e965d427de6d1be2f995cb9ef44f6633b5ee7df5f34dbaadfb` |
| Instant Action | `GR3Xenon_Release.exe` | `4d8a0abb93f3867df53e229a54f55435a57b403f65781d5737237ff8c36c023a` |

Nothing from the disc ever goes into the repository: these folders are ignored by git.

## 4. Build

```
pwsh -NoProfile -File tools/build.ps1
pwsh -NoProfile -File tools/build.ps1 -Project port-yeti
pwsh -NoProfile -File tools/build.ps1 -Project port-mp
```

Each command translates one program's code into C++ and compiles it:
- the front end: `port/out/build/win-amd64-relwithdebinfo/graw.exe`;
- the campaign: `port-yeti/.../grawyeti.exe`;
- Instant Action: `port-mp/.../grawmp.exe`.

These programs contain code translated from your disc: keep them to yourself.

## 5. Play

```
pwsh -NoProfile -File tools/launch.ps1 -Native
```

This starts the front end. Pick Campaign or Instant Action from its menus, as on the console:
it starts the right program. `-Native` draws the campaign with its own Direct3D 12 renderer
(F9 switches to the emulated renderer and back). Useful options, all described at the top of
`tools/launch.ps1`:

| Option | Effect |
|---|---|
| `-Scale 1..3` | internal resolution: 1 = 1280×720 (the console's), 2 = 2560×1440 (default), 3 = 3840×2160 |
| `-Fps 120` | the campaign at 120 fps (with `-Native`); the default is the console's pacing |
| `-Console` | as the 360 draws it: 720p, the game's own texture filtering, 30 fps |
| `-Windowed` | a window instead of full screen |

For example, `tools/launch.ps1 -Native -Scale 3 -Fps 120` gives 4K at 120 fps.

Saves stay in `port/user`.

## When something goes wrong

- **"... is not the supported GRAW executable":** your disc is a different version from the one
  above. Only those executables are supported.
- **"Toolchain missing", or a warning that the patched renderer plugin isn't built:** run
  `tools/setup.ps1` again (with `-Force` to start the SDK source over).
- **"The generated code has the vpkd3d128 aliasing bug":** the patched code generator isn't
  built. Run `tools/setup.ps1`.
- **A crash:** the game writes `crash-<time>.txt` to a `logs` folder (`port/logs` or
  `port-yeti/logs`), next to each run's log. Please attach both to an issue on GitHub.

## Linux (in progress, #38)

The campaign builds on Linux x86-64 but isn't playable there yet: it runs on ReXGlue's emulated
renderer, which isn't meant for the Steam Deck, until the native renderer's Vulkan backend draws
(docs/VULKAN_RENDERER.md). Only the campaign has been built so far, on Ubuntu 26.04 (WSL2) with
clang 21.

1. Packages (Ubuntu names): `clang lld cmake ninja-build git python3 autoconf libgtk-3-dev
   libx11-xcb-dev libxss-dev libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev
   libasound2-dev libpulse-dev libpipewire-0.3-dev libvulkan-dev`.
2. Clone this repository on a Linux file system (a Windows checkout's CRLF line endings break the
   SDK's FFmpeg build) and put your disc's files in `port/assets` (`port-yeti/assets` links to it).
3. `tools/setup-linux.sh`: the patched SDK from source, built and installed for RelWithDebInfo and
   Release (the code generator). The first run takes a while.
4. `tools/build-linux.sh` (the campaign; `--project port` or `port-mp` for the others, not built
   yet).

A build made on a recent distribution needs that distribution's glibc (2.43 on Ubuntu 26.04),
which SteamOS doesn't have: Deck builds will need an older build environment.
