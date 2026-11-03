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

## Deck test 13: where the frame's time goes (2026-11-02)

One session in Gaming Mode: 43 minutes of the campaign (77,295 frames), Steam's 30 fps limit on, the
limiter off (`graw_max_fps` and `graw_present_interval` commented out). The user: "played great at
a solid 30". The frame log's new columns (`swap_acquire_us`, `gpu_*`, `guest_cpu_ms` and the rest)
and `deck-sensors.py` give the split:

| | Test 9 | Test 11 (limiter on) | Test 13 |
|---|---|---|---|
| Frames over 40 ms | 2.1% | 8.5% | 0.64% |

Test 13 also: 29.9 fps average, 33.3 ms median, 36.7 ms at the 99th percentile. 57 frames took over
100 ms, the longest 7.1 s, and those weigh on the 1% low (14.8 fps): 20 of 2,576 seconds showed
under 27 frames, most of the worst in the first 2.5 minutes and around minute 31. What they were
isn't known yet.

A row's `gpu_*` columns and `swap_fence_us` describe the frame before it: they're read when the next
slot's fence signals, and with two slots that's the previous frame's. The GPU columns below are
aligned to the frame that drew those draws. The first row's `guest_*` and `render_*` usage columns
are cumulative since the thread started, because they have no baseline yet: skip that row.

Medians by draws, in ms:

| Draws | Frames | Over 40 ms | GPU | GPU idle before it | Game thread CPU | Render thread CPU |
|---|---|---|---|---|---|---|
| 300-1000 | 53,118 | 0.17% | 7.5 | 25.7 | 10.8 | 3.0 |
| 1000-1500 | 12,051 | 0.34% | 9.2 | 24.1 | 14.0 | 4.4 |
| 1500-2000 | 6,459 | 1.24% | 14.6 | 18.6 | 16.7 | 6.3 |
| 2000-2500 | 3,519 | 5.51% | 19.5 | 13.1 | 18.7 | 7.5 |
| 2500-3000 | 436 | 13.07% | 22.7 | 8.3 | 19.8 | 8.4 |
| 3000+ | 15 | 40% | 21.4 | 0.4 | 24.0 | 11.2 |

What this changes:
- **The game thread isn't over budget.** It needs 24 ms of CPU at 3000+ draws. The older
  `guest_begin` to swap figures (42 ms there) include waits. It barely waits for a CPU core either
  (medians under 64 µs), and the limiter and ring waits are 0 at the median.
- **Frames over 40 ms may point at the GPU, but this isn't settled.** Such a frame waits 7.8 ms on
  the previous frame's fence (6 µs for all frames), and that previous frame's GPU figure is 38.2 ms
  (7.9 ms for all frames). The slow frame's own GPU figure is only 19.9 ms, starting 0.5 ms after the
  one before. The 38.2 ms can include a wait for the swapchain image, so it may not all be GPU work.
- **The GPU clock stays low.** It averages 437-494 MHz per five minutes at 13-14 W, under the 15 W
  cap. The sensor samples once a second, so it can't show a clock ramping up for one slow frame.
  Whether a slow clock causes the slow frames is the next thing to check.
- **The CPU runs hot:** 90-94 C averaged over each five minutes, up to 99.9 C, fan near 7,000 rpm,
  with the process using 76 ms of CPU per 33 ms frame (median). Threads that spin instead of
  sleeping are worth checking first.

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

## Sleep, don't spin (2026-10-25)
On the Steam Deck the CPU and GPU share one ~15 W budget. The overlay showed all CPU threads at
60-100% and the GPU clock at 200-900 MHz: spinning threads were taking the GPU's power. Two
waits spun:
- **The game's wait for room in the GPU command ring** (`sub_824D7830` polls the read pointer
  through `sub_824E0A00`; about half the main thread's samples). A mid-asm hook at the loop's
  back-edge (`0x824D78C8`, reached only while waiting) sleeps 200 us on a high-resolution
  waitable timer: `GrawRingWaitYield` in `port/src/frame_timing.cpp`,
  `--graw_ring_wait_sleep_us` (0 = spin as on the 360).
- **The vblank thread's last 2 ms before each vblank.** It now sleeps on a high-resolution
  waitable timer to 0.25 ms before the deadline and spins only that
  (`tools/patches/rexglue-bd833a2-vblank-pacing.patch`).

360 settings, 30 fps, PC, 3 minutes of play (the old rows are the earlier runs in the section
above, not the same scene):

| | before the vblank fix | vblank fix | this change |
|---|---|---|---|
| frames within 33.3 +-1 ms | 49% | 74% | 82% |
| frames under 25 ms | 18.8% | 5.3% | 1.3% |
| frames over 40 ms | 2.0% | 0.7% | 0.1% |

The whole process averaged ~0.7 of one core. The play-tester called it the best run so far, with
no judder. The Deck result is pending.
