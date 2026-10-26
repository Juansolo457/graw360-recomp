// GRAW native renderer: the Vulkan backend's descriptor sets and pipeline layouts
// (vulkan_bindings.h). Follows ReXGlue's src/graphics/vulkan/command_processor.cpp (Xenia,
// Copyright Ben Vanik, BSD-3; adapted for ReXGlue by Tom Clay): its descriptor set layouts,
// GetPipelineLayout and UpdateBindings.
#include "vulkan_bindings.h"

#include "guest_memory_mirror.h"

#include <rex/logging.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>

namespace graw::native {

namespace {

// Guest register file offsets (d3d_state.h, DrawState::regs).
constexpr uint32_t kFloatVertexRegs = 0x4000, kFloatPixelRegs = 0x4400, kFetchRegs = 0x4800,
                   kBoolLoopRegs = 0x4900;
constexpr uint32_t kBoolLoopSize = 4 * (8 + 32), kFetchSize = 4 * 6 * 32;

// Each frame's descriptor pools; another one is added when a frame needs more.
constexpr uint32_t kPoolSets = 4096;

}  // namespace

VulkanBindings::~VulkanBindings() {
  VkDevice d = device_.device();
  if (!d) return;
  const VulkanDevice::Functions& fn = device_.fn();
  for (FramePools& f : frame_pools_)
    for (VkDescriptorPool p : f.pools) fn.vkDestroyDescriptorPool(d, p, nullptr);
  if (shared_pool_) fn.vkDestroyDescriptorPool(d, shared_pool_, nullptr);
  for (auto& [key, l] : layouts_) fn.vkDestroyPipelineLayout(d, l.layout, nullptr);
  for (auto& [key, l] : texture_layouts_) fn.vkDestroyDescriptorSetLayout(d, l, nullptr);
  for (VkDescriptorSetLayout l : {empty_layout_, shared_layout_, constants_layout_})
    if (l) fn.vkDestroyDescriptorSetLayout(d, l, nullptr);
  if (sampler_) fn.vkDestroySampler(d, sampler_, nullptr);
  for (VkImageView v : {view_2d_array_, view_cube_, view_3d_})
    if (v) fn.vkDestroyImageView(d, v, nullptr);
  if (memory_)
    for (VulkanMemory::Image& i : placeholder_images_) memory_->Destroy(i);
}

bool VulkanBindings::Check(VkResult r, const char* what) {
  if (r == VK_SUCCESS) return true;
  REXLOG_ERROR("GRAW native Vulkan: {} failed ({})", what, int(r));
  return false;
}

bool VulkanBindings::Initialize(VulkanMemory& memory, VkBuffer shared) {
  memory_ = &memory;
  uniforms_.SetMemory(&memory);
  const VulkanDevice::Functions& fn = device_.fn();
  const VkDevice d = device_.device();
  const VulkanDevice::ShaderCaps& caps = device_.shader_caps();
  uniform_align_ = std::max<uint64_t>(caps.min_uniform_buffer_offset_alignment, 16);
  const VkShaderStageFlags guest_stages = vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  if (!Check(fn.vkCreateDescriptorSetLayout(d, &li, nullptr, &empty_layout_),
             "the empty descriptor set layout"))
    return false;

  // Set 0: shared memory, split into as many bindings as maxStorageBufferRange needs.
  const uint32_t shared_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(caps.max_storage_buffer_range);
  const uint32_t shared_count = 1u << shared_log2;
  VkDescriptorSetLayoutBinding shared_binding{};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = shared_count;
  shared_binding.stageFlags = guest_stages;
  li.bindingCount = 1;
  li.pBindings = &shared_binding;
  if (!Check(fn.vkCreateDescriptorSetLayout(d, &li, nullptr, &shared_layout_),
             "the shared memory descriptor set layout"))
    return false;

  // Set 1: the constant buffers, each visible to the stages that read it.
  VkDescriptorSetLayoutBinding cb[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    cb[i].binding = i;
    cb[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cb[i].descriptorCount = 1;
  }
  cb[SpirvShaderTranslator::kConstantBufferSystem].stageFlags =
      guest_stages | (caps.geometry_shader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  cb[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags = vertex_stages_;
  cb[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  cb[SpirvShaderTranslator::kConstantBufferBoolLoop].stageFlags = guest_stages;
  cb[SpirvShaderTranslator::kConstantBufferFetch].stageFlags = guest_stages;
  li.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  li.pBindings = cb;
  if (!Check(fn.vkCreateDescriptorSetLayout(d, &li, nullptr, &constants_layout_),
             "the constants descriptor set layout"))
    return false;

  // The shared memory set, written once.
  VkDescriptorPoolSize shared_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_count};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &shared_size;
  if (!Check(fn.vkCreateDescriptorPool(d, &pi, nullptr, &shared_pool_),
             "the shared memory descriptor pool"))
    return false;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = shared_pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &shared_layout_;
  if (!Check(fn.vkAllocateDescriptorSets(d, &ai, &shared_set_), "the shared memory descriptor set"))
    return false;
  std::vector<VkDescriptorBufferInfo> shared_infos(shared_count);
  const uint64_t shared_range = GuestMemoryMirror::kSharedMemorySize >> shared_log2;
  for (uint32_t i = 0; i < shared_count; ++i) shared_infos[i] = {shared, i * shared_range, shared_range};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = shared_set_;
  w.dstBinding = 0;
  w.descriptorCount = shared_count;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = shared_infos.data();
  fn.vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);

  // Placeholder textures: transparent black, cleared by InitializePlaceholders.
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {1, 1, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 6;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!memory.CreateImage(ii, &placeholder_images_[0], "the placeholder 2D array image"))
    return false;
  ii.flags = 0;
  ii.imageType = VK_IMAGE_TYPE_3D;
  ii.arrayLayers = 1;
  if (!memory.CreateImage(ii, &placeholder_images_[1], "the placeholder 3D image")) return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = placeholder_images_[0].image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (!Check(fn.vkCreateImageView(d, &vi, nullptr, &view_2d_array_), "the placeholder 2D view"))
    return false;
  vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vi.subresourceRange.layerCount = 6;
  if (!Check(fn.vkCreateImageView(d, &vi, nullptr, &view_cube_), "the placeholder cube view"))
    return false;
  vi.image = placeholder_images_[1].image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
  vi.subresourceRange.layerCount = 1;
  if (!Check(fn.vkCreateImageView(d, &vi, nullptr, &view_3d_), "the placeholder 3D view"))
    return false;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = VK_LOD_CLAMP_NONE;
  if (!Check(fn.vkCreateSampler(d, &si, nullptr, &sampler_), "the placeholder sampler"))
    return false;
  REXLOG_INFO("GRAW native Vulkan: descriptor sets: shared memory in {} binding(s), uniform "
              "alignment {}, geometry shaders {}",
              shared_count, uniform_align_, caps.geometry_shader ? "yes" : "no");
  return true;
}

void VulkanBindings::InitializePlaceholders(VkCommandBuffer cmd) {
  if (placeholders_ready_) return;
  const VulkanDevice::Functions& fn = device_.fn();
  VkImageMemoryBarrier b[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[i].image = placeholder_images_[i].image;
    b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i ? 1u : 6u};
    b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 2, b);
  const VkClearColorValue black{};
  for (uint32_t i = 0; i < 2; ++i)
    fn.vkCmdClearColorImage(cmd, b[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                            &b[i].subresourceRange);
  for (VkImageMemoryBarrier& x : b) {
    x.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    x.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    x.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    x.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  }
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 2, b);
  placeholders_ready_ = true;
}

VkDescriptorSetLayout VulkanBindings::TextureSetLayout(bool vertex, uint32_t textures,
                                                       uint32_t samplers) {
  if (!textures && !samplers) return empty_layout_;
  const uint32_t key = uint32_t(vertex) << 16 | samplers << 8 | textures;
  auto it = texture_layouts_.find(key);
  if (it != texture_layouts_.end()) return it->second;
  std::vector<VkDescriptorSetLayoutBinding> bindings(textures + samplers);
  const VkShaderStageFlags stages = vertex ? vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < textures + samplers; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = stages;
  }
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = uint32_t(bindings.size());
  li.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (!Check(device_.fn().vkCreateDescriptorSetLayout(device_.device(), &li, nullptr, &layout),
             "a texture descriptor set layout"))
    return VK_NULL_HANDLE;
  texture_layouts_.emplace(key, layout);
  return layout;
}

const VulkanBindings::Layout* VulkanBindings::GetLayout(uint32_t textures_vertex,
                                                        uint32_t samplers_vertex,
                                                        uint32_t textures_pixel,
                                                        uint32_t samplers_pixel) {
  const uint64_t key = uint64_t(textures_vertex) | uint64_t(samplers_vertex) << 16 |
                       uint64_t(textures_pixel) << 32 | uint64_t(samplers_pixel) << 48;
  auto it = layouts_.find(key);
  if (it != layouts_.end()) return &it->second;
  Layout l;
  l.textures_vertex = TextureSetLayout(true, textures_vertex, samplers_vertex);
  l.textures_pixel = TextureSetLayout(false, textures_pixel, samplers_pixel);
  if (!l.textures_vertex || !l.textures_pixel) return nullptr;
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = l.textures_vertex;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = l.textures_pixel;
  VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pi.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  pi.pSetLayouts = sets;
  if (!Check(device_.fn().vkCreatePipelineLayout(device_.device(), &pi, nullptr, &l.layout),
             "a pipeline layout"))
    return nullptr;
  return &layouts_.emplace(key, l).first->second;
}

const VulkanBindings::Layout* VulkanBindings::GetLayout(const SpirvShader& vs,
                                                        const SpirvShader* ps) {
  return GetLayout(uint32_t(vs.GetTextureBindingsAfterTranslation().size()),
                   uint32_t(vs.GetSamplerBindingsAfterTranslation().size()),
                   ps ? uint32_t(ps->GetTextureBindingsAfterTranslation().size()) : 0,
                   ps ? uint32_t(ps->GetSamplerBindingsAfterTranslation().size()) : 0);
}

VkDescriptorPool VulkanBindings::NewPool() {
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPoolSets * SpirvShaderTranslator::kConstantBufferCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kPoolSets * 8},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kPoolSets * 8},
  };
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = kPoolSets;
  pi.poolSizeCount = uint32_t(std::size(sizes));
  pi.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (!Check(device_.fn().vkCreateDescriptorPool(device_.device(), &pi, nullptr, &pool),
             "a descriptor pool"))
    return VK_NULL_HANDLE;
  return pool;
}

void VulkanBindings::BeginFrame(uint64_t frame) {
  if (frame == frame_) return;
  frame_ = frame;
  // The slot's previous frame (frame - kFramesInFlight) is complete: VulkanDevice waited for it
  // before opening this frame's command buffer.
  FramePools& f = frame_pools_[frame % VulkanDevice::kFramesInFlight];
  for (VkDescriptorPool p : f.pools) device_.fn().vkResetDescriptorPool(device_.device(), p, 0);
  f.current = 0;
  constants_set_ = VK_NULL_HANDLE;
}

VkDescriptorSet VulkanBindings::AllocateSet(VkDescriptorSetLayout layout) {
  FramePools& f = frame_pools_[frame_ % VulkanDevice::kFramesInFlight];
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  for (;;) {
    if (f.current == f.pools.size()) {
      VkDescriptorPool p = NewPool();
      if (!p) return VK_NULL_HANDLE;
      f.pools.push_back(p);
    }
    ai.descriptorPool = f.pools[f.current];
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult r = device_.fn().vkAllocateDescriptorSets(device_.device(), &ai, &set);
    if (r == VK_SUCCESS) {
      ++stats_.sets;
      return set;
    }
    if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
      Check(r, "vkAllocateDescriptorSets");
      return VK_NULL_HANDLE;
    }
    ++f.current;  // full: the next pool
  }
}

bool VulkanBindings::Upload(Constants& c, const void* data, uint32_t size, uint64_t frame,
                            bool* changed) {
  if (c.frame == frame && c.last.size() == size && !std::memcmp(c.last.data(), data, size)) {
    *changed = false;
    return true;
  }
  VulkanUploadPool::Alloc a;
  if (!uniforms_.Allocate(size, uniform_align_, frame, &a)) return false;
  std::memcpy(a.cpu, data, size);
  c.last.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  c.info = {a.buffer, a.offset, size};
  c.frame = frame;
  ++stats_.constant_uploads;
  stats_.constant_bytes += size;
  *changed = true;
  return true;
}

VkDescriptorSet VulkanBindings::TextureSet(const Layout& layout, bool vertex,
                                           const SpirvShader& shader) {
  const auto& textures = shader.GetTextureBindingsAfterTranslation();
  const auto& samplers = shader.GetSamplerBindingsAfterTranslation();
  VkDescriptorSet set = AllocateSet(vertex ? layout.textures_vertex : layout.textures_pixel);
  if (!set) return VK_NULL_HANDLE;
  image_infos_.clear();
  for (const SpirvShader::TextureBinding& t : textures) {
    VkDescriptorImageInfo& i = image_infos_.emplace_back();
    i.imageView = t.dimension == xenos::FetchOpDimension::k3DOrStacked ? view_3d_
                  : t.dimension == xenos::FetchOpDimension::kCube      ? view_cube_
                                                                        : view_2d_array_;
    i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  for (size_t s = 0; s < samplers.size(); ++s) image_infos_.emplace_back().sampler = sampler_;
  VkWriteDescriptorSet w[2] = {};
  uint32_t n = 0;
  if (!textures.empty()) {
    w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[n].dstSet = set;
    w[n].dstBinding = 0;
    w[n].descriptorCount = uint32_t(textures.size());
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[n].pImageInfo = image_infos_.data();
    ++n;
  }
  if (!samplers.empty()) {
    w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[n].dstSet = set;
    w[n].dstBinding = uint32_t(textures.size());
    w[n].descriptorCount = uint32_t(samplers.size());
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[n].pImageInfo = image_infos_.data() + textures.size();
    ++n;
  }
  device_.fn().vkUpdateDescriptorSets(device_.device(), n, w, 0, nullptr);
  return set;
}

bool VulkanBindings::Bind(VkCommandBuffer cmd, const Layout& layout, const uint32_t* regs,
                          const SpirvShader& vs, const SpirvShader* ps,
                          const SpirvShaderTranslator::SystemConstants& sys, uint64_t frame) {
  BeginFrame(frame);
  auto fail = [this](const char* what) {
    if (!bind_failed_logged_) {
      bind_failed_logged_ = true;
      REXLOG_ERROR("GRAW native Vulkan: draw bindings failed ({}); logged once", what);
    }
    return false;
  };

  // Constant buffers. Float constants are packed in the order of the shader's constant map,
  // at least one vec4 (the layout always has both float buffers).
  bool changed = false, any_changed = false;
  using T = SpirvShaderTranslator;
  if (!Upload(constants_[T::kConstantBufferSystem], &sys, sizeof(sys), frame, &changed))
    return fail("uniform memory");
  any_changed |= changed;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const rex::graphics::Shader* shader = stage ? static_cast<const rex::graphics::Shader*>(ps)
                                                : &vs;
    const uint32_t base = stage ? kFloatPixelRegs : kFloatVertexRegs;
    float_scratch_.assign(16, 0);
    if (shader) {
      const auto& map = shader->constant_register_map();
      float_scratch_.resize(16 * std::max(map.float_count, 1u), 0);
      uint8_t* out = float_scratch_.data();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = map.float_bitmap[i];
        while (bits) {
          const uint32_t c = uint32_t(std::countr_zero(bits));
          bits &= bits - 1;
          std::memcpy(out, regs + base + (i << 8) + (c << 2), 16);
          out += 16;
        }
      }
    }
    if (!Upload(constants_[stage ? T::kConstantBufferFloatPixel : T::kConstantBufferFloatVertex],
                float_scratch_.data(), uint32_t(float_scratch_.size()), frame, &changed))
      return fail("uniform memory");
    any_changed |= changed;
  }
  if (!Upload(constants_[T::kConstantBufferBoolLoop], regs + kBoolLoopRegs, kBoolLoopSize, frame,
              &changed))
    return fail("uniform memory");
  any_changed |= changed;
  if (!Upload(constants_[T::kConstantBufferFetch], regs + kFetchRegs, kFetchSize, frame, &changed))
    return fail("uniform memory");
  any_changed |= changed;

  if (any_changed || !constants_set_) {
    constants_set_ = AllocateSet(constants_layout_);
    if (!constants_set_) return fail("constants descriptor set");
    VkWriteDescriptorSet w[T::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < T::kConstantBufferCount; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = constants_set_;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[i].pBufferInfo = &constants_[i].info;
    }
    device_.fn().vkUpdateDescriptorSets(device_.device(), T::kConstantBufferCount, w, 0, nullptr);
  } else {
    ++stats_.constant_set_reuses;
  }

  VkDescriptorSet sets[T::kDescriptorSetCount] = {shared_set_, constants_set_, VK_NULL_HANDLE,
                                                  VK_NULL_HANDLE};
  if (layout.textures_vertex != empty_layout_ &&
      !(sets[T::kDescriptorSetTexturesVertex] = TextureSet(layout, true, vs)))
    return fail("vertex texture descriptor set");
  if (ps && layout.textures_pixel != empty_layout_ &&
      !(sets[T::kDescriptorSetTexturesPixel] = TextureSet(layout, false, *ps)))
    return fail("pixel texture descriptor set");

  // Sets 0 and 1, then the texture sets the shaders have.
  const VulkanDevice::Functions& fn = device_.fn();
  const uint32_t count = sets[2] ? (sets[3] ? 4 : 3) : 2;
  fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 0, count, sets,
                             0, nullptr);
  if (!sets[2] && sets[3])
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 3, 1, &sets[3],
                               0, nullptr);
  return true;
}

}  // namespace graw::native
