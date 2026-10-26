// GRAW native renderer: the Vulkan backend's graphics pipelines (vulkan_pipelines.h). The state
// setup follows ReXGlue's src/graphics/vulkan/pipeline_cache.cpp (Xenia, Copyright Ben Vanik,
// BSD-3; adapted for ReXGlue by Tom Clay), host render target path.
#include "vulkan_pipelines.h"

#include "pipeline_store.h"  // ReadWholeFile, OpenFile

#include <rex/logging.h>

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace graw::native {

bool IsValidStoredPipelineKey(const VulkanPipelineKey& k) {
  auto color_format = [](VkFormat f) {
    switch (f) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R16G16_SNORM:
      case VK_FORMAT_R16G16B16A16_SNORM:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT:
        return true;
      default:
        return false;
    }
  };
  bool ok = k.topology <= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY && k.cull <= 3 &&
            (k.samples == 1 || k.samples == 2 || k.samples == 4) && k.depth_compare <= 7 &&
            k.color_count <= 4;
  for (int i = 0; i < 4; ++i)
    ok &= k.front[i] <= 7 && k.back[i] <= 7;  // VkStencilOp and VkCompareOp both end at 7
  for (uint32_t i = 0; i < k.color_count && ok; ++i) {
    const VulkanPipelineKey::Rt& rt = k.rt[i];
    for (uint8_t f : {rt.src, rt.dst, rt.src_a, rt.dst_a})
      ok &= f <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    ok &= rt.op <= VK_BLEND_OP_MAX && rt.op_a <= VK_BLEND_OP_MAX && rt.mask <= 0xF;
    ok &= k.color_format[i] == VK_FORMAT_UNDEFINED || color_format(k.color_format[i]);
  }
  if (k.has_depth)
    ok &= k.depth_format == VK_FORMAT_D24_UNORM_S8_UINT ||
          k.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  return ok;
}

VulkanPipelines::~VulkanPipelines() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;  // queued pipelines are never compiled
  }
  work_cv_.notify_all();
  wait_cv_.notify_all();
  for (std::thread& t : threads_) t.join();
  if (!device_.device()) return;
  SaveCache();
  for (auto& [hash, e] : pipelines_)
    if (VkPipeline p = e->pipeline.load())
      device_.fn().vkDestroyPipeline(device_.device(), p, nullptr);
  if (cache_) device_.fn().vkDestroyPipelineCache(device_.device(), cache_, nullptr);
}

void VulkanPipelines::Initialize(int threads, int extra_threads,
                                 const std::filesystem::path& cache_path) {
  const VulkanDevice::Functions& fn = device_.fn();
  if (!cache_path.empty()) {
    // A cache from another GPU or driver version is not given to the driver at all.
    std::vector<uint8_t> data = ReadWholeFile(cache_path);
    VkPhysicalDeviceProperties props;
    fn.vkGetPhysicalDeviceProperties(device_.physical_device(), &props);
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() >= sizeof(header)) std::memcpy(&header, data.data(), sizeof(header));
    const bool matches = data.size() >= sizeof(header) && header.headerSize >= sizeof(header) &&
                         header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                         header.vendorID == props.vendorID && header.deviceID == props.deviceID &&
                         !std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID,
                                      VK_UUID_SIZE);
    const size_t read = data.size();
    if (!matches) data.clear();
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (fn.vkCreatePipelineCache(device_.device(), &ci, nullptr, &cache_) != VK_SUCCESS) {
      cache_ = VK_NULL_HANDLE;
      REXLOG_WARN("GRAW native Vulkan pipelines: no pipeline cache");
    } else {
      cache_path_ = cache_path;
      REXLOG_INFO("GRAW native Vulkan pipelines: cache {} ({} KB{})", cache_path.string(),
                  data.size() / 1024,
                  read && !matches ? "; the file was another GPU's or driver's" : "");
    }
  }
  for (int i = 0; i < threads + extra_threads; ++i)
    threads_.emplace_back([this, i, threads] { Worker(i == 0, i >= threads); });
  REXLOG_INFO("GRAW native Vulkan pipelines: {} compile threads ({} more while a frame waits)",
              threads, extra_threads);
}

VulkanPipelines::Entry* VulkanPipelines::Find(const VulkanPipelineKey& key, uint64_t hash) const {
  auto [first, last] = pipelines_.equal_range(hash);
  for (auto it = first; it != last; ++it)
    if (!std::memcmp(&it->second->key, &key, sizeof(key))) return it->second.get();
  return nullptr;
}

VulkanPipelines::Entry* VulkanPipelines::Add(const VulkanPipelineKey& key, uint64_t hash) {
  auto e = std::make_unique<Entry>();
  e->key = key;
  return pipelines_.emplace(hash, std::move(e))->second.get();
}

VkPipeline VulkanPipelines::Get(const VulkanPipelineKey& key, bool* pending, bool* is_new) {
  *pending = false;
  *is_new = false;
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  Entry* e = Find(key, hash);
  if (!e) {
    *is_new = true;
    e = Add(key, hash);
    if (threads_.empty()) {
      Compile(e);
    } else {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(e);
      }
      work_cv_.notify_one();
    }
  }
  switch (e->state.load(std::memory_order_acquire)) {
    case kReady:
      return e->pipeline.load(std::memory_order_relaxed);
    case kFailed:
      return VK_NULL_HANDLE;
    default:
      break;
  }
  *pending = true;
  if (e->pending_frame != frame_serial_) {
    e->pending_frame = frame_serial_;
    frame_pending_.push_back(e);
    // A prewarm compile not started yet: ahead of the queue now.
    std::lock_guard<std::mutex> lock(mutex_);
    if (e->in_prewarm) {
      e->in_prewarm = false;
      prewarm_jobs_.erase(std::find(prewarm_jobs_.begin(), prewarm_jobs_.end(), e));
      jobs_.push_front(e);
      work_cv_.notify_one();
    }
  }
  return VK_NULL_HANDLE;
}

void VulkanPipelines::Prewarm(const VulkanPipelineKey& key) {
  if (threads_.empty()) return;
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  if (Find(key, hash)) return;
  Entry* e = Add(key, hash);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    e->in_prewarm = true;
    prewarm_jobs_.push_back(e);
    ++stats_.prewarmed;
  }
  work_cv_.notify_one();
}

uint64_t VulkanPipelines::WaitForFrame(std::chrono::milliseconds budget) {
  uint64_t us = 0;
  if (!frame_pending_.empty() && budget.count() > 0) {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    waiting_ = true;
    wait_cv_.notify_all();
    ready_cv_.wait_for(lock, budget, [this] {
      return std::none_of(frame_pending_.begin(), frame_pending_.end(), [](const Entry* e) {
        return e->state.load(std::memory_order_acquire) == kCompiling;
      });
    });
    waiting_ = false;
    us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count());
  }
  frame_pending_.clear();
  ++frame_serial_;
  return us;
}

VulkanPipelineStats VulkanPipelines::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

size_t VulkanPipelines::queued() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_.size() + prewarm_jobs_.size();
}

void VulkanPipelines::Compile(Entry* e) {
  const VkPipeline p = Create(e->key);
  e->pipeline.store(p, std::memory_order_relaxed);
  e->state.store(p ? kReady : kFailed, std::memory_order_release);
}

void VulkanPipelines::Worker(bool saves, bool on_wait) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto has_work = [this] { return stopping_ || !jobs_.empty() || !prewarm_jobs_.empty(); };
  for (;;) {
    if (on_wait) {
      wait_cv_.wait(lock, [this] { return stopping_ || (waiting_ && !jobs_.empty()); });
    } else if (saves && cache_dirty_.load(std::memory_order_relaxed)) {
      // Saved once no new pipeline came for 3 s (the burst is over).
      if (!work_cv_.wait_for(lock, std::chrono::seconds(3), has_work)) {
        lock.unlock();
        SaveCache();
        lock.lock();
        continue;
      }
    } else {
      work_cv_.wait(lock, has_work);
    }
    if (stopping_) return;
    std::deque<Entry*>& queue = !jobs_.empty() ? jobs_ : prewarm_jobs_;
    Entry* e = queue.front();
    queue.pop_front();
    e->in_prewarm = false;
    lock.unlock();
    Compile(e);
    lock.lock();
    ready_cv_.notify_all();
  }
}

void VulkanPipelines::SaveCache() {
  if (!cache_ || cache_path_.empty() || !cache_dirty_.exchange(false)) return;
  const VulkanDevice::Functions& fn = device_.fn();
  size_t size = 0;
  if (fn.vkGetPipelineCacheData(device_.device(), cache_, &size, nullptr) != VK_SUCCESS) return;
  std::vector<uint8_t> data(size);
  if (fn.vkGetPipelineCacheData(device_.device(), cache_, &size, data.data()) != VK_SUCCESS) {
    cache_dirty_.store(true);
    return;
  }
  data.resize(size);
  std::filesystem::path tmp = cache_path_;
  tmp += ".tmp";
  std::FILE* f = OpenFile(tmp, "wb");
  if (!f) return;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  std::error_code ec;
  if (ok) std::filesystem::rename(tmp, cache_path_, ec);
  REXLOG_INFO("GRAW native Vulkan pipelines: cache saved, {} KB{}", data.size() / 1024,
              ok && !ec ? "" : " FAILED");
}

VkPipeline VulkanPipelines::Create(const VulkanPipelineKey& k) {
  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = k.vs;
  stages[stage_count++].pName = "main";
  if (k.gs) {
    stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[stage_count].module = k.gs;
    stages[stage_count++].pName = "main";
  }
  if (k.ps) {
    stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = k.ps;
    stages[stage_count++].pName = "main";
  }

  // Vertices come from shared memory (the shaders' vfetch): no vertex input.
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(k.topology);
  input_assembly.primitiveRestartEnable = k.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.rasterizerDiscardEnable = k.discard ? VK_TRUE : VK_FALSE;
  raster.depthClampEnable = k.depth_clamp ? VK_TRUE : VK_FALSE;
  raster.polygonMode = k.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = VkCullModeFlags(k.cull);
  raster.frontFace = k.front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Dynamic, and only meaningful with a depth attachment (as the SDK).
  raster.depthBiasEnable = k.has_depth ? VK_TRUE : VK_FALSE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(k.discard ? 1 : k.samples);

  VkPipelineDepthStencilStateCreateInfo depth_stencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = k.depth_test ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = k.depth_write ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = VkCompareOp(k.depth_compare);
  depth_stencil.stencilTestEnable = k.stencil_test ? VK_TRUE : VK_FALSE;
  auto stencil = [](const uint8_t* s) {
    VkStencilOpState o{};
    o.failOp = VkStencilOp(s[0]);
    o.passOp = VkStencilOp(s[1]);
    o.depthFailOp = VkStencilOp(s[2]);
    o.compareOp = VkCompareOp(s[3]);
    return o;
  };
  depth_stencil.front = stencil(k.front);
  depth_stencil.back = stencil(k.back);

  VkPipelineColorBlendAttachmentState blend[4] = {};
  for (uint32_t i = 0; i < k.color_count; ++i) {
    const VulkanPipelineKey::Rt& rt = k.rt[i];
    blend[i].blendEnable = rt.blend ? VK_TRUE : VK_FALSE;
    blend[i].srcColorBlendFactor = VkBlendFactor(rt.src);
    blend[i].dstColorBlendFactor = VkBlendFactor(rt.dst);
    blend[i].colorBlendOp = VkBlendOp(rt.op);
    blend[i].srcAlphaBlendFactor = VkBlendFactor(rt.src_a);
    blend[i].dstAlphaBlendFactor = VkBlendFactor(rt.dst_a);
    blend[i].alphaBlendOp = VkBlendOp(rt.op_a);
    blend[i].colorWriteMask = VkColorComponentFlags(rt.mask);
  }
  VkPipelineColorBlendStateCreateInfo color_blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blend.attachmentCount = k.color_count;
  color_blend.pAttachments = blend;

  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = k.color_count;
  rendering.pColorAttachmentFormats = k.color_count ? k.color_format : nullptr;
  if (k.has_depth) {
    rendering.depthAttachmentFormat = k.depth_format;
    rendering.stencilAttachmentFormat = k.depth_format;
  }

  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rendering;
  ci.stageCount = stage_count;
  ci.pStages = stages;
  ci.pVertexInputState = &vertex_input;
  ci.pInputAssemblyState = &input_assembly;
  ci.pViewportState = &viewport;
  ci.pRasterizationState = &raster;
  ci.pMultisampleState = &multisample;
  ci.pDepthStencilState = &depth_stencil;
  ci.pColorBlendState = &color_blend;
  ci.pDynamicState = &dynamic;
  ci.layout = k.layout;
  // Whether the driver found it in the pipeline cache (core in Vulkan 1.3).
  VkPipelineCreationFeedback feedback{};
  VkPipelineCreationFeedbackCreateInfo feedback_info{
      VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
  feedback_info.pPipelineCreationFeedback = &feedback;
  rendering.pNext = &feedback_info;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const auto start = std::chrono::steady_clock::now();
  const VkResult r =
      device_.fn().vkCreateGraphicsPipelines(device_.device(), cache_, 1, &ci, nullptr, &pipeline);
  const double us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  const bool hit =
      (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
      (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.create_us += us;
    stats_.max_create_us = std::max(stats_.max_create_us, us);
    if (r == VK_SUCCESS) {
      ++stats_.created;
      stats_.cache_hits += hit;
    } else {
      ++stats_.failed;
    }
  }
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: vkCreateGraphicsPipelines failed ({}), topology {} colors {}",
                 int(r), k.topology, k.color_count);
    return VK_NULL_HANDLE;
  }
  if (!hit) cache_dirty_.store(true, std::memory_order_relaxed);
  return pipeline;
}

}  // namespace graw::native
