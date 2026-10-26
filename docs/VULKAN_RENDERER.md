# Native renderer on Vulkan (#41): plan

Draft, 2026-10-26. The goal is the campaign's native renderer (`native/`) running on Vulkan, so the
Steam Deck and Linux run it natively (#38) instead of through Proton's D3D12-to-Vulkan layer.
On the Deck that layer cost too much: ~70% GPU and CPU in a busy section, an uneven frame rate,
and a native-only shimmer. D3D12 stays the Windows default and must not change behaviour.

## Decisions (2026-10-26)
- **Linux is native, never Proton or Wine.** The Steam Deck build is a native Linux program.
- **Vulkan for Linux; D3D12 stays on Windows** until Vulkan proves better there (M6 compares
  them).
- **Deck target: 60 fps with no drops, on the native renderer.** The emulated renderer isn't
  shipped on the Deck. A Linux build with the emulated renderer is only a development step
  (booting the programs, host code) on the way to M7, not a release.
- **0021 isn't ported.** The shimmer seen through Proton is expected to go with the native
  Vulkan path; M7 checks it.

## The Deck profile
The Deck build is the Xbox 360 game, its own code, through the native Vulkan renderer, set up to
hold 60 fps. It doesn't add PC extras.
- **Resolution:** the 360's 1280x720 by default, fit to the Deck's 1280x800 screen. If that
  doesn't hold 60, render lower and scale up. The native renderer has no resolution scale yet,
  so a scale-down (internal resolution below 720p, upscaled at present) is part of M7.
- **Other levers, in the order to try them:**
  - the game's 4x MSAA (off, or 2x);
  - anisotropic filtering (the game's own, no forced 16x);
  - shadow map resolution (the game's own, never raised);
  - the pipeline cache warmed before play, so there are no compile hitches.
- **CPU:** the Deck's Zen 2 cores are much faster per thread than the 360's in-order 3.2 GHz
  PowerPC cores. The recompiled game logic itself should fit easily; the Proton run's ~70% CPU
  included the D3D12-to-Vulkan layer. Measure on the Deck anyway: the game thread, the
  renderer's recording and the presenter.
- **Done when:** frame-time logs show no frame over 16.7 ms in the busiest scenes, the GRAW
  save's plaza fight among them.

## Starting point
- `native/` is ~16,400 lines in 49 files. The D3D12-specific part, ~1,000 direct D3D12/DXGI
  references, is in `draw.cpp` (3,900 lines), `textures.cpp`, `resolve.cpp`, `render_targets.cpp`,
  `present.cpp`, `d3d12_device.*`, `pipeline_store.*`, `upload_pool.h`, `gpu_timer.*` and
  `geometry_shader.cpp`.
- Already API-independent:
  - the push-buffer tracker (`d3d_state.*`) and the guest D3D hooks (`graw_d3d_hooks.cpp`);
  - texture layout and untiling (`texture_layout.*`);
  - the bool specialization, which works on Xenos ucode (`shader_specialize.*`);
  - the frame pacer's logic, the clock and the logging.
- **Shaders:**
  - Guest shaders go through the SDK's `DxbcShaderTranslator`, compiled into the app
    (`shader_xlate.cmake`), with pgr4-native's 0021/0022 changes.
  - The resolve and present shaders are HLSL compiled at run time with `D3DCompile`
    (`d3dcompiler_47.dll`, Windows only).
  - The geometry shaders for point sprites and rectangles are hand-assembled DXBC.
- **The SDK has the Vulkan counterpart of everything the D3D12 renderer was ported from:**
  - `src/graphics/vulkan/`: command processor, pipeline cache, render target cache, texture cache
    and shared memory, ~20,000 lines from Xenia;
  - `SpirvShaderTranslator`, ~11,700 lines;
  - `src/ui/vulkan/`: instance, device, memory allocator, presenter, upload buffer pool and
    submission tracker.

  The native D3D12 renderer was built by porting from the SDK's D3D12 backend, and the Vulkan
  one follows the same method with the Vulkan backend as its source.

## Design
**One renderer, two GPU backends behind a narrow interface of our own.** Everything that
decides what to draw stays shared. The backend owns:
- the device, the frames and their sync;
- upload memory and buffers;
- textures, samplers and views;
- render targets, clears and copies;
- pipelines (create, async compile, the on-disk cache);
- recording a draw (bindings, dynamic state, draw);
- resolves (copy and compute passes);
- present, the debug window and captures;
- GPU timing.

Two implementations: `d3d12/` (today's code, moved, not rewritten) and `vulkan/` (new).
Selected with `--graw_native_api=d3d12|vulkan`: the default is d3d12 on Windows, vulkan
elsewhere.

**Why not plume (as #41 first suggested).** plume (MIT, used by Unleashed Recompiled) unifies
the API calls, but the hard part here is the binding model and the shaders. The SDK's two
translators each assume their own:
- the DXBC translator, a D3D12 root signature;
- the SPIR-V translator, Xenia's Vulkan descriptor-set layout.

Following the SDK's Vulkan backend directly keeps the translated shaders and the pipeline
layout consistent, as the D3D12 port does today. plume's Vulkan layer would sit between them
and need adapting either way. The interface above keeps the option open if we change our mind.

**Shaders on Vulkan:**
- **Guest shaders:** the SDK's `SpirvShaderTranslator` with the host-render-targets path (the
  same choice as the DXBC side).
  - Bool specialization works unchanged (ucode level).
  - 0022 (textures known unsigned) needs its SPIR-V equivalent, if it's needed at all. The
    Vulkan backend has its own signedness handling, to check first.
  - 0021 (`sample_b`) is a performance change and isn't ported. It's also the prime suspect for
    the Deck shimmer (#40).
- **Geometry shaders:** the Vulkan pipeline cache already builds SPIR-V geometry shaders for the
  same Xenos primitive types. Port those.
- **Resolve and present shaders:** compile the HLSL to SPIR-V at build time with DXC
  (`-spirv`), or rewrite them in GLSL for glslang (an SDK submodule). Embed the result. This
  also removes the run-time `D3DCompile` dependency on Windows.

**Formats to handle up front:**
- `D24_UNORM_S8_UINT` isn't available on AMD under Vulkan. Like the SDK's Vulkan render target
  cache (`render_target_cache.cpp`, which checks the format's support and falls back), the
  guest's `kD24S8` gets `D32_SFLOAT_S8_UINT` there, and the polygon offset has to be scaled for a
  float depth buffer. Follow the Vulkan backend's depth bias handling (`command_processor.cpp`).
- The texture formats the D3D12 path expands (`k_10_11_11` to RGBA16, ...) follow the Vulkan
  texture cache's table.

## Milestones
Each one ends with something running. Vulkan comes up on Windows first, so it can be compared
with D3D12 on the same PC (an RTX 4090), with the capture tools we have (`capture.request`,
`--graw_native_resolve_dump`, `tools/compare_resolves.py`).

| # | Milestone | Done when |
|---|---|---|
| M0 | **Spike** (2-3 days): build the SDK with `REXGLUE_USE_VULKAN=ON` on Windows; translate GRAW's shaders with `SpirvShaderTranslator` inside the app; read the Vulkan pipeline layout it expects | A list of every GRAW shader translating to valid SPIR-V (spirv-val); the binding model written down |
| M1 | **Backend seam**: move the D3D12 code behind the interface, no behaviour change | D3D12 captures and frame times identical to before; CodeRabbit-reviewed PR |
| M2 | **Vulkan device and present**: instance/device through the SDK's `ui::vulkan` provider and allocator, swapchain, frame sync, debug window | `--graw_native_api=vulkan` opens the window and presents cleared frames on Windows; validation layers clean |
| M3 | **Pipelines and draws**: SPIR-V shaders, pipeline layout, async compile, `VkPipelineCache` plus our store, dynamic state | Menus draw on Vulkan |
| M4 | **Textures, samplers, render targets**: untiled uploads, formats, depth formats above, MSAA, clears | The tutorial's 3D scene matches D3D12 captures (PSNR as in NATIVE_RENDERER.md) |
| M5 | **Resolves, the Cross-Com feed and the tactical map, gamma, captures, GPU timing** | A full GRAW-save run matches D3D12: HUD, Cross-Com, map, particles |
| M6 | **Parity and performance on Windows**: Vulkan vs D3D12 frame times at 60 and 120 fps | Within ~10% of D3D12; user play-test on Vulkan |
| M7 | **Linux and the Steam Deck**, with phase 1 of #38 (native Linux build) | The campaign runs natively on the Deck on the native Vulkan renderer at **60 fps with no drops** in the busiest scenes (frame-time logs: no frame over 16.7 ms), no shimmer |

Phase 1 of #38 (the Linux build, booting on the SDK's emulated renderer) runs alongside M0-M5 as
a development step. It doesn't touch `native/` and isn't shipped.

**Campaign first.** The native renderer covers the campaign program only. The front end (menus)
and Instant Action draw through the emulated renderer today and move to the native renderer
later, as their own milestone after M7. Their D3D is the same XDK; the hooks need each
program's own addresses.

## M0 in detail
- **Corpus:** `<cache_root>/graw-native/pipelines.bin`, the native renderer's pipeline store. It
  holds the guest ucode of every shader the renderer has drawn with, and the (vertex, pixel)
  modification pairs used (`PipelineStore`). The smoke runs' store (`port-yeti/cache-smoke`,
  1.3 MB) covers the tutorial, the GRAW save and the fire tests. No game run needed. It's game
  code: it stays in the ignored cache folders.
- **Tool:** a small spike program (`tools/vulkan-spike/`, local only) built against the SDK
  source with `REXGLUE_USE_VULKAN=ON`, in its own build folder so the D3D12 plugin build isn't
  touched. It:
  - reads the store, analyzes each shader's ucode;
  - translates it with `SpirvShaderTranslator` (host render targets, no fragment shader
    interlock);
  - validates the SPIR-V with SPIRV-Tools (`spirv-val`);
  - writes a table: shader, stage, size, valid or not, and any translator errors.
- **Modifications:** the stored ones are the DXBC translator's (its own `Modification` union).
  The spike maps the fields both share (dynamic addressable register count, host vertex shader
  type, interpolator mask) and translates with the SPIR-V translator's defaults otherwise. Exact
  mapping is M3's job.
- **Also to read:** the Vulkan pipeline cache's pipeline layout (which bindings each descriptor
  set has, push constants) and the render target cache's format choices. These answer the
  binding-model question.
- **Done when:** the table shows every shader in the corpus translating to valid SPIR-V (or a
  short list of failures with causes), and the binding model is written down here.

## M0 results (2026-10-26)
**Every GRAW shader translates to valid SPIR-V.**
- `tools/vulkan-spike/` (local) translated the native renderer's whole shader corpus with the
  SDK's `SpirvShaderTranslator`:
  - `port-yeti/cache-smoke`: 814 shaders in 952 vertex and pixel variants, every one translated
    and valid under `spirv-val` (Vulkan 1.2);
  - `port-yeti/cache`: 338 shaders, 364 variants.
- Translation is fast: the 952 variants take ~5 s in one thread, including validation.
- Results per variant: `private/vulkan-spike/m0-*.csv`.

**One device requirement: `scalarBlockLayout`.** The translator's system-constants uniform block
puts a member at offset 28 that the standard and relaxed layout rules reject. All 952 fail
`spirv-val` without scalar block layout, and pass with it. The SDK's Vulkan device enables
`scalarBlockLayout` (`vulkan_device.cpp`). RADV on the Deck and current desktop drivers support
it, so it's a requirement to check at start-up, not a blocker.

**The binding model (the SDK's Vulkan backend, which these shaders are written for):**

| Set | Contents | Changes |
|---|---|---|
| 0 | Shared memory (guest RAM) as storage buffers, split only where `maxStorageBufferRange` < 512 MB; EDRAM only on the interlock path we don't use | Never |
| 1 | Five uniform buffers: system constants, vertex float constants, pixel float constants, bool/loop constants, fetch constants | Per draw, from transient pools |
| 2 | Vertex textures: N sampled images, then M samplers, per shader | Per draw |
| 3 | Pixel textures: the same | Per draw |

There are no push constants. The texture sets' layouts come from each translated shader's
bindings (`SpirvShader::Get{Texture,Sampler}BindingsAfterTranslation`).

Our D3D12 path is bindless, with no per-draw descriptor writes. The SPIR-V translator isn't. At
~2,000 draws per frame, writing two to three descriptor sets per draw is the CPU cost to watch on
the Deck. Options for M3:
- write sets per draw as the SDK does (simplest, proven);
- cache texture sets by their contents;
- `VK_KHR_push_descriptor` for sets 1-3 (supported by RADV).

Start with the SDK's way, measure in M6/M7, and switch if it shows.

**Mapping the modifications:** the DXBC and SPIR-V translators' modification fields
correspond, and the spike maps them field by field. M3 reuses the mapping. 0021/0022's DXBC-only
fields are dropped.

**Estimate after M0:** unchanged at 3-5 weeks. The shader side is lower risk than feared. The
work is the backend port itself (M1-M5).

## M1 status (2026-10-26)
**Done for the frame path.**
- `native/backend.h`: the `Backend` interface and the API-free types the host queues between
  threads. `DrawPrep` holds `rex::graphics::Shader*`, `VertexCopy` an opaque upload buffer, and
  the draw and texture stats moved there.
- `native/d3d12_backend.h/.cpp`: the device, module wiring, swap and window ownership, moved
  out of `renderer.cpp` unchanged. The modules take `D3D12Backend&`.
- `renderer.cpp` is the API-free host: hooks, render-thread queue, stats and CSV, renderer
  switch, pacing.

**Test** (GRAW save, native primary, 145 s): no errors, no failed or unsupported draws, 16.67 ms
frames, gameplay with the HUD and the Cross-Com feed rendering. Gameplay stretch against the
pre-refactor clean-folder run (another part of the level, so a sanity check only):

| | draws/frame | µs/draw | swap µs | game thread µs |
|---|---|---|---|---|
| before | 1,256 | 0.94 | 452 | 284 |
| M1 | 1,549 | 0.86 | 340 | 257 |

**Left in M1, needed before Linux (M7), not Windows:**
- The guest-call decoders still live in D3D12 module files: `RenderTargets::DecodeClear`,
  `DecodeResolve`, `Presenter::ReadSwap` and their structs, including `D3D12_RECT` in
  `ClearParams`.
- `FramePacer` attaches to a DXGI waitable and a D3D12 fence.
- `renderer.cpp` uses Win32 for its thread names and `Sleep`.

## M2 status (2026-10-26)
**Done.** `--graw_native_api=vulkan` (default d3d12 on Windows, vulkan elsewhere) selects
`VulkanBackend`.
- `native/vulkan_device.h/.cpp`:
  - opens the loader at run time (`vulkan-1.dll` / `libvulkan.so.1`, VK_NO_PROTOTYPES): the
    prebuilt SDK the game links has no Vulkan, and only the SDK tree's headers are used;
  - picks a Vulkan 1.2 GPU with `scalarBlockLayout` and enables it;
  - keeps two frames in flight, and a swapchain on the game window that is recreated when out
    of date;
  - `--graw_native_vulkan_validation` routes the Khronos layer's messages to the log.
- `native/vulkan_backend.h/.cpp`:
  - counts the game's draws, clears and resolves without drawing them;
  - presents a cleared, slowly color-cycling frame per guest Swap;
  - owns and releases the game window like the D3D12 path;
  - primary mode only, no display pacing yet.

**Test** (GRAW save, 70 s, validation on, RTX 4090):
- Vulkan 1.4.351, swapchain 2560x1440, 3 images, mailbox;
- 4,057 frames, all presented, 16.67 ms;
- **0 validation errors or warnings**;
- the slow frames are the first one (swapchain creation) and the save's loading at ~28 s.

The D3D12 default, run after it, is unchanged: gameplay with HUD and Cross-Com, 1,888
draws/frame at 0.76 µs/draw, no errors.

## Risks
- **The SPIR-V translator's output for GRAW is unproven.** M0 exists to find gaps before
  anything else is built.
- **The binding model:** the D3D12 path uses bindless descriptor heaps. Vulkan needs descriptor
  indexing (Vulkan 1.2, on the Deck and current desktop drivers) or the SDK's per-draw
  descriptor sets. Pick in M0, by what the SPIR-V translator emits.
- **Debugging on the Deck** is slower than on the PC. Doing M1-M6 on Windows keeps most bugs on
  the PC, where RenderDoc and the capture tools work.
- **60 fps with no drops on the Deck** is a CPU question as much as a GPU one: the Deck's CPU is
  several times slower than the build PC's. The game thread, the recompiled code and the
  renderer's recording time all need measuring on the Deck in M7, and probably work after it.
- **Size:** M1 and M3-M5 are the bulk. Rough estimate: 3-5 weeks of focused work. M0 will tighten
  it.
