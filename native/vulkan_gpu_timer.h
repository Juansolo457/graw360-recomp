// GRAW native renderer: GPU time per pass of the Vulkan frame (--graw_native_gpu_timing), the
// Vulkan counterpart of gpu_timer.h (D3D12), with the same pass keys, groups and log lines so
// the two backends' logs read alike.
//
// The backend names the pass each command it records belongs to: draws on one set of render
// targets (or, with --graw_native_gpu_timing_shaders, one vertex/pixel shader pair), a clear, a
// resolve, the present blit. Whenever the pass changes, a timestamp goes into the frame's command
// buffer, so the span between two timestamps is the GPU time of the pass recorded in it. The
// device calls BeginFrame once the frame-in-flight slot's fence was waited for, so the slot's
// previous frame is read then, never stalling the GPU or the recording thread.
//
// Timestamps are written at the bottom of the pipe: a span runs from the end of the previous
// pass to the end of its own. Neighbouring passes overlap on the GPU, so one span is
// approximate; a frame's spans add up to its GPU time.
//
// Threading: the thread that records the frames (the render thread by default).
#pragma once

#include <rex/ui/vulkan/api.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace graw::native {

class VulkanDevice;
struct ClearCall;
struct ResolveParams;

class VulkanGpuTimer {
 public:
  // Spans per frame; passes past it are charged to the last span.
  static constexpr uint32_t kMaxSpans = 2047;

  ~VulkanGpuTimer();
  // A query pool for `slots` frames in flight. False when the queue has no timestamps.
  bool Initialize(VulkanDevice& device, uint32_t slots);

  // The frame's command buffer was begun in `slot` (its fence was waited for): collects the
  // frame that slot held last and writes the new frame's first timestamp.
  void BeginFrame(VkCommandBuffer cmd, uint32_t slot);
  // The commands recorded next belong to pass `key`.
  void Pass(VkCommandBuffer cmd, uint64_t key) {
    if (!active_) return;  // no BeginFrame yet (the device's first frame starts before Initialize)
    if (key != current_) Switch(cmd, key);
    ++frames_[slot_].spans.back().commands;
  }
  // Before the command buffer is ended: the last timestamp.
  void EndFrame(VkCommandBuffer cmd);

  // Pass keys (gpu_timer.h's layout).
  static uint64_t KeyDraw(const uint32_t* regs);
  static uint64_t KeyClear(const ClearCall& c);
  static uint64_t KeyResolve(const ResolveParams& p);
  static uint64_t KeyPresent();

  // --graw_native_gpu_timing_shaders: draws are timed per (vertex shader, pixel shader) pair on
  // a surface pitch and MSAA mode (KeyDrawShaders), and a pair's spans in a frame form one group.
  void set_per_shader(bool on) { per_shader_ = on; }
  bool per_shader() const { return per_shader_; }
  uint64_t KeyDrawShaders(const uint32_t* regs, uint64_t vs_hash, uint64_t ps_hash);

  // Logs the frames collected since the last call: GPU time per frame, the GPU's idle time
  // between frames, and the costliest pass groups. Resets the window.
  void Log();

 private:
  struct Span {
    uint64_t key;
    uint32_t commands;
  };
  struct Frame {
    std::vector<Span> spans;
    uint64_t number = 0;
    uint32_t queries = 0;
    bool pending = false;  // submitted, not collected yet
  };
  struct SpikeSpan {
    uint64_t ns;
    uint32_t index;
  };
  struct Group {
    uint64_t ns = 0, spans = 0, commands = 0, position = 0;
  };

  void Switch(VkCommandBuffer cmd, uint64_t key);
  void Stamp(VkCommandBuffer cmd);
  void Collect(uint32_t slot);
  bool IsSpike(uint64_t ns);
  void LogSpike(const Frame& f, uint64_t ns);
  std::string Describe(uint64_t key) const;

  VulkanDevice* device_ = nullptr;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  double ns_per_tick_ = 1.0;
  uint64_t tick_mask_ = ~uint64_t(0);
  std::vector<uint64_t> ticks_;  // Collect's scratch
  std::vector<Frame> frames_;
  uint32_t slot_ = 0;
  uint64_t current_ = 0;
  bool active_ = false;  // between BeginFrame and EndFrame
  std::unordered_map<uint64_t, uint32_t> occurrences_;  // this frame's spans per key

  bool per_shader_ = false;
  std::vector<std::pair<uint64_t, uint64_t>> pairs_;  // (VS, PS) hashes by KeyDrawShaders id
  std::unordered_map<uint64_t, uint32_t> pair_ids_;

  // The window Log reports.
  std::unordered_map<uint64_t, Group> groups_;
  uint64_t frames_collected_ = 0, frame_ns_ = 0, frame_ns_max_ = 0, idle_ns_ = 0;
  uint64_t spans_collected_ = 0;
  uint64_t last_end_ = 0;  // end timestamp (ticks) of the last collected frame

  uint64_t next_number_ = 0;
  uint64_t typical_ns_ = 0;  // running mean of frame GPU time, spikes clamped
  std::vector<SpikeSpan> spike_spans_;
  uint32_t spikes_logged_ = 0;
};

}  // namespace graw::native
