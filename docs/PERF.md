# Campaign performance at 4K (#11), measured 2026-10-24

Target: 120 fps at 4K (`--resolution_scale 3`, 3840x2160 internal). Measured with the autopilot
(`tools/smoke.ps1 ... "-Extra:--resolution_scale 3 --vsync=false --graw_frame_log <csv>"`) and
`tools/perf_report.py`. The frame log counts the game's own frames (each VdSwap), guest refresh
240 Hz, 16x anisotropic filtering, RelWithDebInfo build.

| Scene | Scale | vsync | Median fps | 1% low |
|---|---|---|---|---|
| Mission 1, start (GRAW save) | 3 (4K) | on | 142 | 93 |
| Mission 1, start | 1 | on | 143 | 93 |
| Mission 1, start | 3 (4K) | off | 250 | 174 |
| Tutorial, after the intro | 3 (4K) | off | 117 | 97 |
| Tutorial, after the intro | 1 | off | 117 | 97 |

- **4K costs nothing measurable:** 1x and 4K give the same numbers, so the GPU isn't the limit.
- **With vsync on** the game is held to the display's refresh (about 144 Hz here).
- **The limit is CPU, on two threads,** each near 100% of a core in the tutorial: the renderer's
  GPU command processor (translating the guest's command buffers) and the game's main thread
  (the recompiled code, which calls VdSwap). They run as a pipeline, so the slower one sets the
  frame rate. Raising it means CPU work in the renderer or the game code, not a setting.
- These scenes are light (standing or walking at a mission start). The play-test in #11 averaged
  83 fps in real play; heavy firefights still need measuring.
- The renderer also logs one rejected texture fetch constant, (00024800 00000000 00000000 00A80000
  00000003 00000010), 348 times: type 0, base 0. That is D3D's "nothing bound" placeholder (a
  shader sampling an empty slot), not a descriptor the recompiled code built wrong (#6).
