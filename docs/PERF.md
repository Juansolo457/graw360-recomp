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

## Stutter at 60 Hz: the Windows timer resolution (2026-10-25)
With the guest display at 60 Hz (the default since #17), a play-test felt uneven: median 60 fps
but ~half the frames 3 ms early or late, ~3% a full 33 ms (a missed refresh) and a 1% low of
~28 fps, worst in the helicopter ride over the city.
- **Not CPU load.** Uncapped (`--vsync=false`) the same stretch ran at 133 fps median with a
  99th-percentile frame of 13.7 ms: the game needs well under a 60 Hz frame.
- **Sleep granularity.** `tools/sample_profile.py` (a sampling profiler: pauses the busiest
  threads ~1000 times a second and names the code with each module's PDB) showed the renderer's
  command processor 49% inside `Sleep` (`WAIT_REG_MEM` with vsync on sleeps `wait/256` ms) and the
  game's main thread spinning in D3D waits. ReXGlue never raises Windows' timer resolution, so
  each `Sleep(1)` lasts up to 15.6 ms; the guest vblank thread (`graphics_system.cpp`) polls with
  `Sleep(1)` and then fires every vblank that's due, so vblanks came late and in bursts.
- **Fix:** every program requests a 0.5 ms timer resolution at start-up (`NtSetTimerResolution`,
  `--graw_timer_resolution_us`, `port/src/frame_timing.cpp`) and opts out of Windows 11 ignoring
  it in the background.

| 60 Hz, Mission 1 route, 1440p | before | after |
|---|---|---|
| average fps | 56.2 | 59.6 |
| 1% low | 28.2 | 52.3 |
| 99th-percentile frame | 35.4 ms | 19.1 ms |
| frames ~33 ms (missed refresh) | 224 | 16 |

Still uneven: ~20% of frames take 17.5-25 ms, because the vblank thread still polls on a 1 ms
sleep instead of waiting for each vblank's deadline (a renderer-plugin change).

## 30 fps is the game's rate; even vblanks (2026-10-25)
Play-tests showed the campaign's logic is built for ~30 fps (#17): at 60 allies' weapons vanish
(#7), the rally point can't complete (#19), squad commands lag and enemies react late; at 30
(`launch.ps1 -Console`) it plays like the 360. `launch.ps1` now defaults to a 30 Hz guest display.
30 fps then felt uneven: frames between 31 and 35 ms and ~15% under 25 ms, because the guest
vblank thread polled on a 1 ms sleep and fired every vblank that was due at once. The thread
now waits for each vblank's own deadline (coarse sleep to 2 ms before, then yield) and restarts
after a stall instead of firing a burst (`tools/patches/rexglue-bd833a2-vblank-pacing.patch`, in the
renderer plugin). 360 settings, plaza fight, 60 s:

| | old timer | new timer |
|---|---|---|
| frames within 33.3 +-1 ms | 51.8% | 71.1% |
| frames under 25 ms | 15.7% | 6.2% |
| frame-time std. dev. | 7.3 ms | 5.4 ms |

(The frame log times the game's VdSwap calls, i.e. when the CPU hands a frame over, not when it
is shown, so some of the remaining spread is the CPU running a frame ahead.)
