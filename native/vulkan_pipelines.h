// GRAW native renderer: the Vulkan backend's graphics pipelines (docs/VULKAN_RENDERER.md, M3c-3,
// M4d).
//
// A pipeline per key: the shader modules, the pipeline layout, and the fixed state the guest's
// registers select, as draw.cpp's PipelineKey with Vulkan values. Dynamic state (viewport,
// scissor, depth bias, blend constants, stencil masks and reference) as in the SDK's Vulkan
// pipeline cache, so pipelines don't multiply with values that change per draw. Pipelines are
// for dynamic rendering (the attachment formats are in the key). Point, rectangle and quad lists
// add a geometry shader (vulkan_geometry_shader.h, M4c).
//
// M4d, as draw.cpp's D3D12 pipelines: with worker threads new pipelines compile there, and Get
// returns VK_NULL_HANDLE with *pending for one still compiling (its draw is skipped). Such a
// frame is held (the backend doesn't present it) and waits after its submission for the
// pipelines its draws skipped (WaitForFrame, at most a budget), compiled meanwhile by every
// worker, the extra ones too. Prewarm queues the compile of a key behind the draws'. A
// VkPipelineCache is loaded from the cache file when its header is this GPU's and driver's, and
// saved when no compile came for 3 s, and when the pipelines are destroyed. Entries are never
// removed. WarmUp, at start, puts every worker on the queue until it drains (the warm-up).
// Threading: Initialize, Get, Prewarm, WaitForFrame, WarmUp on the recording thread; the workers
// compile.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace graw::native {

struct VulkanPipelineKey {
  VkShaderModule vs;
  VkShaderModule ps;  // VK_NULL_HANDLE: no fragment shader
  VkShaderModule gs;  // VK_NULL_HANDLE: no geometry shader
  VkPipelineLayout layout;
  uint8_t topology;           // VkPrimitiveTopology
  uint8_t primitive_restart;  // all ones index restarts the strip
  uint8_t discard;            // rasterizer discard
  uint8_t cull;               // VkCullModeFlags
  uint8_t front_cw;
  uint8_t polygon_line;       // wireframe
  uint8_t depth_clamp;
  uint8_t samples;            // 1, 2, 4
  uint8_t depth_test, depth_write, depth_compare, stencil_test;
  uint8_t front[4], back[4];  // fail, pass, depth fail, compare (Vulkan values)
  uint8_t color_count;
  uint8_t has_depth;
  uint8_t pad[2];
  VkFormat color_format[4];
  VkFormat depth_format;
  struct Rt {
    uint8_t blend, src, dst, op, src_a, dst_a, op_a, mask;  // VkBlendFactor / VkBlendOp values
  } rt[4];
  uint8_t pad_end[4];
};
// No implicit padding: keys are hashed and compared as bytes.
static_assert(sizeof(VulkanPipelineKey) == 112);
// Of the keys in the backend's pipeline store: a change of the key or its meaning starts the
// store over.
constexpr uint32_t kVulkanPipelineKeyVersion = 1;

// Whether a key read from disk (the backend's pipeline store) holds only values Vulkan accepts
// for its enums and formats; its handles are not checked.
bool IsValidStoredPipelineKey(const VulkanPipelineKey& key);

struct VulkanPipelineStats {
  uint64_t created = 0, failed = 0;
  uint64_t cache_hits = 0;  // created from the VkPipelineCache (creation feedback)
  uint64_t prewarmed = 0;   // compiles queued by Prewarm
  double create_us = 0, max_create_us = 0;
};

class VulkanPipelines {
 public:
  explicit VulkanPipelines(VulkanDevice& device) : device_(device) {}
  ~VulkanPipelines();

  // threads: compile workers (0: Get compiles, no prewarm queue); extra_threads: more that
  // compile only while a frame waits. cache_path: the VkPipelineCache file (empty: none).
  void Initialize(int threads, int extra_threads, const std::filesystem::path& cache_path);

  // The pipeline for `key` (zeroed before filling: hashed as bytes), queued or created if new
  // (*is_new). VK_NULL_HANDLE: compiling (*pending; a prewarm compile moves ahead) or failed
  // (logged once per key).
  VkPipeline Get(const VulkanPipelineKey& key, bool* pending, bool* is_new);
  // Queues the compile of `key` behind the draws' compiles, if it is new.
  void Prewarm(const VulkanPipelineKey& key);

  // Whether a draw of the frame being recorded was skipped for a pipeline still compiling.
  bool frame_pending() const { return !frame_pending_.empty(); }
  // After the frame's submission: waits up to `budget` for the pipelines its draws skipped;
  // returns the microseconds waited.
  uint64_t WaitForFrame(std::chrono::milliseconds budget);
  // The warm-up: waits up to `budget` for every queued compile (prewarm's included), with every
  // worker on them, the extra ones too. `progress` gets (compiled, queued at the start) about
  // once a second. Returns the compiles still queued or running (0: done).
  size_t WarmUp(std::chrono::milliseconds budget,
                const std::function<void(size_t, size_t)>& progress);

  VulkanPipelineStats stats() const;
  size_t count() const { return pipelines_.size(); }
  size_t queued() const;

 private:
  enum : uint8_t { kCompiling, kReady, kFailed };
  struct Entry {
    VulkanPipelineKey key;
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<uint8_t> state{kCompiling};
    bool in_prewarm = false;    // still in prewarm_jobs_ (under mutex_)
    uint64_t pending_frame = 0;  // last frame_serial_ it joined frame_pending_ (recording thread)
  };
  Entry* Find(const VulkanPipelineKey& key, uint64_t hash) const;
  Entry* Add(const VulkanPipelineKey& key, uint64_t hash);
  // Creates the pipeline (and the stats); VK_NULL_HANDLE if it failed.
  VkPipeline Create(const VulkanPipelineKey& key);
  void Compile(Entry* e);
  // `saves`: also saves the cache once compiles stopped; `on_wait`: compiles only while a frame
  // waits.
  void Worker(bool saves, bool on_wait);
  void SaveCache();

  VulkanDevice& device_;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  std::filesystem::path cache_path_;
  std::atomic<bool> cache_dirty_{false};
  std::unordered_multimap<uint64_t, std::unique_ptr<Entry>> pipelines_;

  std::vector<std::thread> threads_;
  mutable std::mutex mutex_;
  std::condition_variable work_cv_;   // a job was queued
  std::condition_variable wait_cv_;   // a frame began waiting (the extra workers)
  std::condition_variable ready_cv_;  // a job finished (notified under mutex_)
  std::deque<Entry*> jobs_;           // draws are waiting for these
  std::deque<Entry*> prewarm_jobs_;   // taken only when jobs_ is empty
  bool waiting_ = false;              // WaitForFrame is waiting
  bool warming_ = false;              // WarmUp is waiting
  uint32_t compiling_ = 0;            // compiles in progress
  bool stopping_ = false;
  std::vector<Entry*> frame_pending_;
  uint64_t frame_serial_ = 1;
  VulkanPipelineStats stats_;  // under mutex_
};

}  // namespace graw::native
