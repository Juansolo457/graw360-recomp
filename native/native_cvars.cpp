// GRAW native renderer: the cvars both backends and the renderer host read, compiled on every
// platform (the D3D12 renderer's own are in its files, the Vulkan backend's in
// vulkan_backend.cpp).
#include <rex/cvar.h>

#include <cstdint>
#include <string>

REXCVAR_DEFINE_BOOL(graw_native_async_pipelines, true, "GRAW native",
                    "Compile new pipeline states on worker threads, the frame's draws that need "
                    "them skipped, instead of compiling each at its draw");
REXCVAR_DEFINE_INT32(graw_native_pipeline_threads, 0, "GRAW native",
                     "Pipeline compile threads with --graw_native_async_pipelines (0: a quarter "
                     "of the hardware threads, 1 to 4)");
REXCVAR_DEFINE_BOOL(graw_native_pipeline_wait, true, "GRAW native",
                    "With --graw_native_async_pipelines, a frame whose draws need pipelines still "
                    "compiling is not shown and waits for them at its end, up to 0.5 s (false: "
                    "show it without those draws)");
REXCVAR_DEFINE_BOOL(graw_native_pipeline_cache, true, "GRAW native",
                    "Keep the shaders and pipelines the native renderer creates under "
                    "<cache_root>\\pgr4-native and compile them at start-up in later runs "
                    "(prewarm, plus a D3D12 pipeline library)");
REXCVAR_DEFINE_STRING(graw_native_watch_trace, "", "GRAW native",
                      "Binary log of vertex write-watch events (arms and faults, one 64-bit "
                      "record each, see WatchTrace) for offline analysis of the watch policy");
REXCVAR_DEFINE_BOOL(graw_native_coherency_check, false, "GRAW native",
                    "Debug: keep a CPU copy of what shared memory holds and compare the vertex and "
                    "index ranges draws read with guest memory, per request and again at the Swap "
                    "(stale pages: a write the page watch missed; late pages: data rewritten after "
                    "a draw of the frame read it), and check copy segments against every read. "
                    "Slow (512 MB of host memory, a memcmp of every range read)");
REXCVAR_DEFINE_BOOL(graw_native_implicit_gradients, true, "GRAW native",
                    "Experiment (Steam Deck shimmer): translate computed-LOD texture fetches in "
                    "pixel shaders with sample_b and the GPU's own derivatives (pgr4-native 0021; "
                    "the Vulkan renderer's SPIR-V shaders too, with OpImageSampleImplicitLod); "
                    "false: explicit coarse gradients, as before 0021");
REXCVAR_DEFINE_BOOL(graw_native_luma_probe, false, "GRAW native",
                    "Read back three rows of every front buffer (at 10, 40 and 80 % of its "
                    "height) and log their mean brightness in the frame CSV, to find frames "
                    "drawn partly black");
