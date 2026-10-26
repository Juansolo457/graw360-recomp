// GRAW native renderer: the Vulkan backend (vulkan_backend.h).
#include "vulkan_backend.h"

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "game_window.h"
#include "vulkan_memory.h"
#include "vulkan_shaders.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cmath>
#include <utility>

REXCVAR_DEFINE_BOOL(graw_native_vulkan_validation, false, "GRAW native",
                    "With --graw_native_api=vulkan: enable the Khronos validation layer when it "
                    "is installed (the Vulkan SDK); its errors and warnings go to the log");
REXCVAR_DECLARE(bool, graw_native_render_thread);
REXCVAR_DECLARE(std::string, graw_native_watch_trace);
REXCVAR_DECLARE(bool, graw_native_coherency_check);

namespace graw::native {

// The GPU memory the backend owns. Destroyed after the device went idle (~VulkanBackend).
struct VulkanBackend::Gpu {
  VulkanMemory memory;
  // Guest physical memory: what the translated shaders read vertices (and the index buffer is
  // bound) from, kept current by GuestMemoryMirror's copies.
  VulkanMemory::Buffer shared;
  // Vertex and index ranges copied into shared memory (the guest thread's with the render
  // thread: tagged by Prepare, recycled at EndFramePrepare).
  VulkanUploadPool vertex_upload{16u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
  ~Gpu() { memory.Destroy(shared); }
};

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() {
  mirror_.Shutdown();
  if (device_) device_->WaitIdle();
}

bool VulkanBackend::Initialize(const Options& o) {
  if (!o.primary) {
    REXLOG_ERROR("GRAW native Vulkan: needs --graw_native_primary (no debug window yet)");
    return false;
  }
  device_ = std::make_unique<VulkanDevice>();
  if (!device_->Initialize(o.game_window, o.vsync, REXCVAR_GET(graw_native_vulkan_validation)))
    return false;
  shaders_ = std::make_unique<VulkanShaderCache>(*device_);
  rex::memory::Memory* memory = rex::system::kernel_memory();
  membase_ = memory->physical_membase();

  gpu_ = std::make_unique<Gpu>();
  if (!gpu_->memory.Initialize(*device_)) return false;
  gpu_->vertex_upload.SetMemory(&gpu_->memory);
  if (!gpu_->memory.CreateBuffer(GuestMemoryMirror::kSharedMemorySize,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 false, &gpu_->shared, "shared memory buffer (512 MB)"))
    return false;
  threaded_ = REXCVAR_GET(graw_native_render_thread);
  if (!mirror_.Initialize(memory, this, REXCVAR_GET(graw_native_watch_trace),
                          REXCVAR_GET(graw_native_coherency_check)))
    return false;
  mirror_.SetCounters(threaded_ ? &prep_upload_bytes_ : &stats_.upload_bytes,
                      threaded_ ? &prep_uploads_ : &stats_.uploads,
                      threaded_ ? &prep_diag_ : &diag_);
  REXLOG_INFO(
      "GRAW native Vulkan: up; draws aren't recorded yet (M3c-1: shaders, guest memory, cleared "
      "frames)");
  return true;
}

rex::graphics::SpirvShader* VulkanBackend::LoadShader(const ShaderRef& ref,
                                                      rex::graphics::xenos::ShaderType type) {
  if (!ref.ucode || !ref.dwords) return nullptr;
  // Declaration-patched vertex shaders (IM_LOAD_IMMEDIATE) come from the tracker's copy.
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase_ + ref.ucode);
  return shaders_->Load(type, ucode, ref.dwords);
}

bool VulkanBackend::AllocateUpload(uint32_t size, uint64_t tag, GuestMemoryMirror::Upload* out) {
  VulkanUploadPool::Alloc a;
  if (!gpu_->vertex_upload.Allocate(size, 16, tag, &a)) return false;
  out->cpu = a.cpu;
  out->buffer = reinterpret_cast<void*>(a.buffer);
  out->offset = a.offset;
  return true;
}

// As DrawRenderer::Impl::Prepare (draw.cpp): shaders, then the draw's ranges into the batch's
// copies, with the copy segment bookkeeping.
void VulkanBackend::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                            std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) {
  // The render thread can't read the shaders later: the tracker's patched copies change.
  prep->vs = LoadShader(state.vs, rex::graphics::xenos::ShaderType::kVertex);
  prep->ps = LoadShader(state.ps, rex::graphics::xenos::ShaderType::kPixel);
  if (!prep->vs) return;
  mirror_.upload_fence = upload_tag;
  const bool indexed_ok = !state.indexed || state.index_address;
  // Straight into the batch: copies made before a failed range still go out (their pages are
  // marked valid already).
  mirror_.preparing = true;
  mirror_.segment_conflict = false;
  mirror_.draw_reads.clear();
  prep->upload_ok = indexed_ok && mirror_.RequestVertices(state, *prep->vs, *copies);
  mirror_.preparing = false;
  if (mirror_.segment_conflict ||
      mirror_.segment_exact.size() >= GuestMemoryMirror::kSegmentExactLimit) {
    mirror_.NewSegment();
    prep->copy_segment = true;
  }
  for (const GuestMemoryMirror::Read& read : mirror_.draw_reads) mirror_.SegmentAdd(read);
  if (mirror_.coherency)
    for (const GuestMemoryMirror::Read& read : mirror_.draw_reads)
      mirror_.coherency->segment_reads.push_back({read.begin, read.end});
}

void VulkanBackend::RecordCopies(const VertexCopy* copies, size_t count) {
  if (!count) return;
  ++stats_.copy_batches;
  const VulkanDevice::Functions& fn = device_->fn();
  VkCommandBuffer cmd = device_->command_buffer();
  // Earlier draws' reads of shared memory (vertex fetches, the index buffer) before the copies'
  // writes, and the copies before later draws' reads.
  VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = gpu_->shared.buffer;
  b.size = VK_WHOLE_SIZE;
  constexpr VkPipelineStageFlags kReadStages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kReadAccess = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  b.srcAccessMask = kReadAccess;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fn.vkCmdPipelineBarrier(cmd, kReadStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &b,
                          0, nullptr);
  for (size_t i = 0; i < count; ++i) {
    const VertexCopy& c = copies[i];
    const VkBufferCopy region{c.src_offset, c.dst, c.size};
    fn.vkCmdCopyBuffer(cmd, reinterpret_cast<VkBuffer>(c.src), gpu_->shared.buffer, 1, &region);
  }
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = kReadAccess;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kReadStages, 0, 0, nullptr, 1, &b,
                          0, nullptr);
}

void VulkanBackend::Draw(const DrawState& state, const DrawPrep* prep) {
  using rex::graphics::xenos::ShaderType;
  if (prep) {
    // The guest thread copied the ranges (and marked their pages valid): they go into shared
    // memory whether or not the draw is issued.
    RecordCopies(prep->copies, prep->copy_count);
    if (!prep->upload_ok) {
      ++stats_.failed;
      return;
    }
  }
  auto* vs = static_cast<SpirvShader*>(prep ? prep->vs : LoadShader(state.vs, ShaderType::kVertex));
  auto* ps = static_cast<SpirvShader*>(prep ? prep->ps : LoadShader(state.ps, ShaderType::kPixel));
  if (!vs) {
    ++stats_.skipped;
    return;
  }
  const ShaderModifications mods = SelectSpirvModifications(
      shaders_->translator(), *vs, ps, ReadShaderDrawRegs(state.regs, state.primitive));
  if (!mods.draw) {
    ++stats_.skipped;
    return;
  }
  if (!prep) {
    mirror_.upload_fence = device_->frame_fence();
    copies_.clear();
    const bool ok = mirror_.RequestVertices(state, *vs, copies_);
    // The copies made before a failed range go out: their pages are marked valid already.
    RecordCopies(copies_.data(), copies_.size());
    if (!ok) {
      ++stats_.failed;
      return;
    }
  }
  const bool ok = shaders_->Module(*vs, mods.vertex) &&
                  (!mods.pixel_shader_used || shaders_->Module(*ps, mods.pixel));
  // M3c-1: shaders and vertex data ready, not drawn yet.
  if (ok)
    ++stats_.unsupported;
  else
    ++stats_.failed;
}

PrepareCounters VulkanBackend::EndFramePrepare(uint64_t done) {
  PrepareCounters c;
  if (mirror_.coherency) {
    mirror_.CheckLateWrites();
    c.stale_pages = std::exchange(mirror_.coherency->stale, 0);
    c.late_pages = std::exchange(mirror_.coherency->late, 0);
  }
  mirror_.watch_frame.fetch_add(1, std::memory_order_relaxed);
  gpu_->vertex_upload.EndFrame(done);
  c.upload_bytes = std::exchange(prep_upload_bytes_, 0);
  c.uploads = std::exchange(prep_uploads_, 0);
  c.diag = std::exchange(prep_diag_, {});
  return c;
}

void VulkanBackend::AddPrepareCounters(const PrepareCounters& c) {
  stats_.upload_bytes += c.upload_bytes;
  stats_.uploads += c.uploads;
  stats_.stale_pages += c.stale_pages;
  stats_.late_pages += c.late_pages;
}

void VulkanBackend::AfterSwap(uint64_t frame_index, uint64_t completed) {
  frame_index_ = frame_index;
  stats_.watch_faults = mirror_.watch_faults.exchange(0, std::memory_order_relaxed);
  stats_.watch_arms = mirror_.watch_arms.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_pages = mirror_.watch_arm_pages.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_changed = mirror_.watch_arm_changed.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_ns = mirror_.watch_arm_ns.exchange(0, std::memory_order_relaxed);
  stats_.hash_pages = mirror_.hash_pages.exchange(0, std::memory_order_relaxed);
  stats_.hash_copies = mirror_.hash_copies.exchange(0, std::memory_order_relaxed);
  if (!threaded_) {
    mirror_.watch_frame.fetch_add(1, std::memory_order_relaxed);
    gpu_->vertex_upload.EndFrame(completed);
  }
}

void VulkanBackend::SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) {
  paces_frames_ = paces_frames;
  if (pacer) REXLOG_WARN("GRAW native Vulkan: frame pacing on the display isn't supported yet");
}

void VulkanBackend::ReleaseGameWindow() {
  if (paces_frames_) paces_frames_->store(false, std::memory_order_relaxed);
  if (device_ && device_->has_swapchain()) device_->DetachSwapchain();
  if (window_taken_) {
    ConnectSdkPresenter(true);
    window_taken_ = false;
    REXLOG_INFO("GRAW native: the game window shows ReXGlue's presenter");
  }
}

bool VulkanBackend::OwnGameWindow(bool own) {
  if (own == window_taken_) return true;
  if (!own) {
    ReleaseGameWindow();
    return true;
  }
  ConnectSdkPresenter(false);
  window_taken_ = true;
  return device_->AttachSwapchain();
}

Backend::SwapResult VulkanBackend::Swap(const SwapCall& swap, std::optional<bool> own_window) {
  SwapResult r;
  if (own_window && !OwnGameWindow(*own_window)) return r;
  // A slow hue cycle (about 8 s per turn at 60 fps), dark enough not to dazzle.
  const float t = float(frame_index_) * (6.2831853f / 480.0f);
  const float color[4] = {0.15f + 0.1f * std::sin(t), 0.15f + 0.1f * std::sin(t + 2.094f),
                          0.15f + 0.1f * std::sin(t + 4.189f), 1.0f};
  if (frame_index_ >= frames_logged_ + 150) {
    frames_logged_ = frame_index_;
    const VulkanShaderStats& s = shaders_->stats();
    REXLOG_INFO("GRAW native Vulkan shaders: {} shaders, {} translations ({:.0f} us total, max "
                "{:.0f} us), {} modules, {} failed",
                s.shaders, s.translations, s.translate_us, s.max_translate_us, s.modules,
                s.failures);
  }
  r.fence = device_->frame_fence();
  if (!device_->EndFrame(window_taken_, color)) return r;
  const VulkanDevice::EndFrameTimes& et = device_->end_frame_times();
  r.presented = et.presented;
  r.presents = et.presents;
  r.submit_ns = et.submit_ns;
  r.present_ns = et.present_ns;
  r.completed = device_->completed_fence();
  r.ok = true;
  return r;
}

}  // namespace graw::native
