# HUD border noise in the tutorial (#1): findings

Status 2026-10-24: **fixed** in the renderer (`tools/patches/rexglue-bd833a2-freed-page-resolve.patch`),
see "Root cause" at the end. The 3D view was never inset: the noise band was the HUD frame art,
drawn from a stale GPU copy of its texture. Earlier sections are kept as the investigation record;
their theories about an inset composite are superseded.

## The symptom, measured

- In the training mission (US Army Training Camp) the 3D view is drawn into a centred
  rectangle of about 1120x576 of the 1280x720 frame (80-pixel side margins, 72-pixel top and
  bottom). The margin shows coloured, tile-patterned noise and the cyan HUD frame is missing.
- The HUD elements (ammo box, Cross-Com, objective text) are at the same positions as in Mission
  1, so only the 3D image is inset, not the HUD.
- **The real game doesn't do this.** The Xbox Series X (backward compatibility) walkthrough of the
  training mission (YouTube `nsEMUE3z_JE`) shows a full-screen 3D view with the thin cyan HUD
  frame near the edges.
- Mission 1 (loaded from a save) is full-screen with the clean cyan frame. An older Mission 1
  capture (frame 4990, its helicopter intro, before the #2 fix) also had the inset, with black
  margins, so the inset isn't tutorial-only; it depends on game state.

## What the margin is

- Only 2-7% of the margin's pixels change between screenshots 2 s apart, so it isn't drawn each
  frame: it's stale memory. The sun-glare overlay does draw over it, so it is the frame's render
  memory, which the 3D image just doesn't cover.
- The pattern (a regular ~8-pixel motif, teal and green like the campaign menu) is old data read
  in another format, not a game texture. The static texture (128x128, grey noise) and the
  Cross-Com scanline texture (32x256) both decode correctly.
- The renderer's host render targets are committed (zeroed) resources, so it isn't uninitialized
  host memory. ReXGlue's guest heap is the same code as Xenia's (no decommit on free).

## Ruled out

- **Renderer options.** Running with Canary's GPU/D3D12 values for every option that differs
  (`execute_unclipped_draw_vs_on_cpu`, `gpu_allow_invalid_fetch_constants`,
  `clear_memory_page_state`, `readback_memexport`, `non_seamless_cube_map`,
  `primitive_processor_cache_min_indices`, `present_safe_area_x/y`) changes nothing.
- **The front-buffer copy.** The renderer's resolve trace (`rexgpu-xenosrd.dll.blend-trace`,
  `--graw_trace_rtt_max_width=4096`) shows the same resolves in the tutorial and Mission 1, all
  full 1280x720 from EDRAM base 736; the clears after them use colour 0.
- **The game's D3D Clear and Resolve calls.** `port/src/resolve_probe.cpp`
  (`--graw_debug_resolve`) logs identical Clear calls in both; D3DDevice_Resolve itself is called
  only once (the frame's copies go through D3D's own swap path).
- **The scene's viewport.** In a tutorial capture the scene draws' NDC scale is (1, 1) with only
  the half-pixel offset: the 3D scene targets the full 1280x720.
- **`sub_821851D0`'s view effect.** The tutorial (not Mission 1) runs this full-screen effect:
  it copies the scene to a texture and draws it back on a camera-facing quad (x = ±0.888889,
  1 unit ahead, ±0.5 high). Forcing its gate (`sub_821457A0`'s result at `0x8218561C`) false
  leaves the inset unchanged, so it isn't the cause.

## Where it must be

The scene is rendered full-size, so the inset happens in the composition after it (the
post-process chain: the bloom downsamples 52x720, 64x80 ... and the full-screen quad at
`0x8211A4DC`, from `sub_82119858` called by `sub_82186400`). That pass isn't in any tutorial
capture so far: each capture holds one predicated-tiling tile of the scene (scissor 1280x256), the
resolves and the presenter.

## Later findings (same day)

- **The margin is the GRAW HUD's edge blur.** In Mission 1 (capture 4990) the pixel is written by a
  full-screen grid mesh (1,086 vertices, event 18042; Canary's event 19716 is the same mesh with
  the same texture coordinates) that mixes the 1280x720 scene copy with a 256x200 blurred copy,
  by a per-vertex factor (about 0.5 at the edges). Issue #1's own title says the border should
  be blur. In the tutorial the blurred copy shows tile-noise instead, and so does the Cross-Com
  square, which samples a 256x200 texture too: in the tutorial that texture holds stale data.
- **More ruled out:** the view rectangle (`sub_82140938`, all 1280x720 with fractions 0,0,1,1 in
  both runs) and the tutorial-only pass `sub_8217BEF8` (skipping it changes nothing).
- **Not a resolution problem:** the noise appears at 1x (720p internal) too.

## Round 3 (2026-10-24 afternoon): what the front buffer holds

Dumped with `--readback_resolve=full --graw_dump_physical ...` (resolve results copied back to
guest memory, then written to files; `port/src/resolve_probe.cpp`) and untiled offline with
Xenia's GetTiledOffset2D:

- **The front buffer** (physical 1ED08000, 1280x720, 8:8:8:8, tiled; from VdSwap's fetch
  constant) shows the noise margins exactly as on screen.
- **The inner rectangle is the whole scene shrunk**, not a crop: about (80,77)-(1202,653) of
  1280x720, i.e. NDC x ±0.876, y +0.786..-0.814. Every HUD element sits inside it; the cyan frame
  (which lives in the margin) is missing.
- **The blur textures are fine:** 1FA14000 and 1FA4C000 (256x200) hold clean full-frame blurred
  copies of the scene. The edge blur's source is not the noise.
- **Ruled out this round:** heap memory (`--scribble_heap` leaves the noise identical, so it is
  deterministic, not fresh allocations); the single-channel 1280x720 / 640x360 textures after the
  scene copy (zeroing them changes nothing); dropped draws (no renderer errors besides the
  "nothing bound" fetch); D3D SetViewport (all viewports full: 1280x720 and the post-process
  sizes); D3D SetScissorRect (scissor test off); the full-screen post quad (`sub_82115298`
  builds (0,0)-(1280,720)); `sub_821851D0`'s quad (its projection, view+140 = 1.429/2.54, puts the
  quad at ±1.27, past the screen edges).
- **The letterbox mask** in the old Mission 1 capture (event 18077) is the HUD frame art
  (512x256 BC2, 16 vertices); with the #2 fix it draws One/InvSrcColor like Canary, so it no longer
  hides the margin.

## Next steps

0. Find which 256x200 texture the edge mesh and the Cross-Com square sample in the tutorial,
   and why it isn't written there (in Mission 1 it holds the blurred scene).
1. Capture the composition pass: trigger several consecutive frames
   (`tools/rdoc_trigger.py` with close times), or capture from the guest side by dumping the
   vertex and texture-coordinate data of every draw in `sub_82119858`'s chain in the tutorial and
   in Mission 1 (the per-draw probe already logs DrawPrimitiveUP vertices).
2. Find the value that differs between the two (a scale or rectangle in the post-process
   constants), then trace where the engine computes it and audit that code against the PowerPC.

## Tools added for this

- `tools/rdoc_trigger.py`: triggers RenderDoc captures on a timetable through the target-control
  API, so captures work unattended (with the autopilot).
- `port/src/resolve_probe.cpp`: the game's D3D Resolve and Clear calls with their callers.
- `port/src/blend_probe.cpp`: the per-draw catalogue now also logs each DrawPrimitiveUP site's
  vertices (all floats, full precision).

## Root cause (2026-10-24 evening): a resolve into freed memory left a stale texture

Found by logging every draw from the scene composite to the swap, then skipping draws by index and
by texture:
- **The scene is full-screen.** The edge-blur grid (vs 9932BC1F8BCC0099) gets correct vertices
  (positions 0..1280 x 0..720, texture coordinates 0..1); the HUD sits where it does in Mission 1.
- **The band is the HUD frame art.** Skipping every 512x256 DXT3 texture (`--graw_skip_sampling=512x256f19`,
  a size/format match, since texture addresses move between runs) gives a clean full-screen view.
  The same textures hold the Cross-Com icon, the stance icon and the minimap frame, which were
  noise too.
- **Guest memory is right, the GPU copy is wrong.** The atlas's guest bytes, dumped at draw time
  and untiled, are the correct cyan frame art. Forcing one invalidation of that range
  (`--graw_invalidate_atlas`) makes the frame draw correctly.
- **Why the GPU copy is stale.** Shared-memory history for the atlas range (`G` = written by the
  GPU, `I` = invalidated by the CPU, `U` = uploaded): the menu's front buffer (1D977000+398000)
  is resolved there every frame until 29.09 s; the game frees it (`I`); 18 ms later one more
  resolve lands in it (`G`), the last frame's copy, which the GPU runs behind the CPU. Nothing
  after: the HUD atlas is loaded into that memory, but the CPU writes are never seen.
  `SharedMemory::MakeRangeValid` marks GPU-written pages valid and asks for write watches, but
  `PhysicalHeap::EnableAccessCallbacks` skips inaccessible (freed) pages. So the pages stay
  "valid, GPU-written" with no watch, and the texture keeps the menu's last frame forever.
  This is also why Mission 1 loaded from a save was clean: its atlas lands elsewhere.
- **Fix:** `MakeRangeValid` no longer marks GPU-written pages valid when the guest's physical heap
  has them unallocated; they're uploaded from guest memory when next used. Writing a resolve into
  freed memory has no meaning, so nothing is lost. The fix is 28 lines in
  `src/graphics/shared_memory.cpp`, as `tools/patches/rexglue-bd833a2-freed-page-resolve.patch`
  against bd833a2.
- **Verified** with stock bd833a2 plus only that patch (plugin built with Vulkan off, installed as
  the campaign's `rexgpu-xenosrd.dll`; copy `rexgpu-xenosrd.dll.fixed`) at 4K: the tutorial is
  full-screen with the cyan frame and clean icons, and Mission 1 is unchanged. In the logged run,
  920 pages of the menu front buffer and 20 of a Cross-Com video texture were caught.
- **Still to decide:** how players get it. The build uses the SDK's prebuilt plugin, so the fix
  needs either an upstream ReXGlue change or building the plugin from source in `tools/build.ps1`.

Experiment options added on the way (diagnostics patch `tools/patches/rexglue-bd833a2-graw-rtt-trace.patch`,
local plugin builds only): `--graw_skip_anchor/--graw_skip_draws_from/--graw_skip_draws_to/--graw_skip_step`
(drop draws by index from the scene composite), `--graw_skip_sampling` (by texture base, shader hash low
32 bits, `k8`, or `WxHfF`), `--graw_grid_no_depth`, `--graw_invalidate_atlas`, and with
`--graw_trace_rtt_max_width` the per-draw sequence log, the atlas dump and the shared-memory history.
