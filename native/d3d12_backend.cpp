// GRAW native renderer: the D3D12 backend (d3d12_backend.h). Moved out of renderer.cpp
// unchanged (docs/VULKAN_RENDERER.md, M1).
#include "d3d12_backend.h"

#include "clock.h"
#include "d3d12_device.h"
#include "draw.h"
#include "frame_pacer.h"
#include "game_window.h"
#include "gpu_timer.h"
#include "present.h"
#include "profile.h"
#include "render_targets.h"
#include "resolve.h"
#include "textures.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace graw::native {

D3D12Backend::D3D12Backend() = default;
D3D12Backend::~D3D12Backend() = default;

bool D3D12Backend::Initialize(const Options& o) {
  device_ = std::make_unique<D3D12Device>();
  if (o.primary) {
    device_->set_vsync(o.vsync);
    device_->set_frame_latency_waitable(o.frame_latency_waitable);
  }
  if (!device_->Initialize(o.width, o.height, o.primary ? L"GRAW native" : L"GRAW native (debug)",
                           static_cast<HWND>(o.game_window)))
    return false;
  if (o.gpu_timing) {
    if (device_->EnableGpuTimer()) {
      gpu_timer_ = device_->gpu_timer();
      gpu_timer_->set_per_shader(o.gpu_timing_shaders);
    } else
      REXLOG_ERROR("GRAW native: GPU timing unavailable");
  }
  rt_ = std::make_unique<RenderTargets>();
  if (!rt_->Initialize(device_->device())) return false;
  resolver_ = std::make_unique<Resolver>();
  if (!resolver_->Initialize(device_->device(), rt_.get(), device_->command_queue())) return false;
  resolver_->set_gpu_timer(gpu_timer_);
  textures_ = std::make_unique<TextureCache>(device_->device(), rex::system::kernel_memory());
  textures_->SetResolvedLookup([this](uint32_t a) { return resolver_->Find(a); });
  textures_->SetCaptureOrder(o.texture_capture_order);
  resolver_->SetChangedCallback([this](uint32_t a) { textures_->OnResolved(a); });
  draw_ = std::make_unique<DrawRenderer>(*this);
  if (!draw_->Initialize()) return false;
  present_ = std::make_unique<Presenter>(*this);
  if (!present_->Initialize()) return false;
  return true;
}

void D3D12Backend::SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) {
  pacer_ = pacer;
  paces_frames_ = paces_frames;
}

ID3D12Device* D3D12Backend::d3d12() const { return device_->device(); }
ID3D12CommandQueue* D3D12Backend::command_queue() const { return device_->command_queue(); }
ID3D12GraphicsCommandList* D3D12Backend::command_list() const { return device_->command_list(); }
uint64_t D3D12Backend::frame_fence() const { return device_->frame_fence(); }

// ---------------------------------------------------------------------------------------------
// Guest thread.

void D3D12Backend::BeginCopySegment() { draw_->BeginCopySegment(); }

void D3D12Backend::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                           std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) {
  draw_->Prepare(state, upload_tag, prep, copies, audit);
}

void D3D12Backend::StartAudit(uint32_t frames) { draw_->StartAudit(frames); }

PrepareCounters D3D12Backend::EndFramePrepare(uint64_t done) {
  return draw_->EndFramePrepare(done);
}

bool D3D12Backend::CaptureRequested(uint64_t frame) { return present_->CaptureRequested(frame); }
bool D3D12Backend::capture_holds_guest() const { return present_->capture_holds_guest(); }
bool D3D12Backend::capture_holding_guest() const { return present_->capture_holding_guest(); }

// ---------------------------------------------------------------------------------------------
// Recording thread.

void D3D12Backend::AddPrepareCounters(const PrepareCounters& c) { draw_->AddPrepareCounters(c); }
void D3D12Backend::LogFrames(uint32_t frames) { draw_->LogFrames(frames); }

void D3D12Backend::BeginFrame(uint64_t frame_index) {
  frame_index_ = frame_index;
  const uint64_t fence = device_->frame_fence(), done = device_->completed_fence();
  rt_->BeginFrame(frame_index_, fence, done);
  resolver_->set_linear_output(device_->hdr());
  resolver_->BeginFrame(frame_index_, fence, done);
}

void D3D12Backend::Draw(const DrawState& state, const DrawPrep* prep) {
  if (gpu_timer_) {
    uint64_t key;
    if (gpu_timer_->per_shader() && prep && prep->vs) {
      const auto [vs, ps] = DrawRenderer::ShaderHashes(*prep);
      key = gpu_timer_->KeyDrawShaders(state.regs, vs, ps);
    } else {
      key = GpuTimer::KeyDraw(state.regs);
    }
    gpu_timer_->Pass(device_->command_list(), key);
  }
  draw_->OnDraw(state, prep);
}

void D3D12Backend::Clear(const ClearCall& c) {
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyClear(c));
  draw_->LogClear(c);
  rt_->Clear(device_->command_list(), c.regs, c.bound_colors, c.depth_bound, c.height, c.p);
}

void D3D12Backend::Resolve(const ResolveParams& p) {
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyResolve(p));
  draw_->LogResolve(p);
  // Sets its own compute root signature, pipeline and descriptor heap on the list.
  resolver_->Resolve(device_->command_list(), p);
  draw_->InvalidateListState();
}

// The texture copies recorded so far read upload memory the untile workers fill: while untiles
// are still running, the next draw or resolve tries again.
Backend::SubmitResult D3D12Backend::SubmitEarly() {
  if (!textures_->UploadsDone()) return SubmitResult::kNotReady;
  profile::Scope scope(profile::kSubmit);
  if (!CloseUploadLists() || !device_->Submit(upload_lists_)) return SubmitResult::kFailed;
  draw_->InvalidateListState(DrawRenderer::ListBreak::kSubmit);
  return SubmitResult::kSubmitted;
}

bool D3D12Backend::CloseUploadLists() {
  profile::Scope scope(profile::kUploadList);
  upload_lists_.clear();
  return textures_->CloseUploadLists(&upload_lists_);
}

void D3D12Backend::ReleaseGameWindow() {
  D3D12Device& d = *device_;
  if (paces_frames_) paces_frames_->store(false, std::memory_order_relaxed);
  if (d.has_swapchain()) {
    d.DetachSwapchain();
    if (pacer_) pacer_->Detach();
  }
  if (window_taken_) {
    ConnectSdkPresenter(true);
    window_taken_ = false;
    REXLOG_INFO("GRAW native: the game window shows ReXGlue's presenter");
  }
}

bool D3D12Backend::OwnGameWindow(bool own) {
  D3D12Device& d = *device_;
  if (own == d.has_swapchain()) return true;
  if (!own) {
    ReleaseGameWindow();
    return true;
  }
  ConnectSdkPresenter(false);
  window_taken_ = true;
  if (!d.AttachSwapchain()) return false;
  if (pacer_) {
    HANDLE waitable = d.TakeFrameLatencyWaitable(FramePacer::kMaxFrameLatency);
    if (!waitable) return false;
    pacer_->Attach(waitable, d.fence());
    if (paces_frames_) paces_frames_->store(pacer_->pacing(), std::memory_order_relaxed);
  }
  return true;
}

Backend::SwapResult D3D12Backend::Swap(const SwapCall& swap, std::optional<bool> own_window) {
  SwapResult r;
  D3D12Device& d = *device_;
  if (gpu_timer_) gpu_timer_->Pass(d.command_list(), GpuTimer::KeyPresent());
  // A scene's first frame without the draws whose pipelines were compiling: the previous frame
  // stays on screen while the next one waits for them.
  r.incomplete = draw_->FrameIncomplete();
  if (own_window && !OwnGameWindow(*own_window)) return r;
  const bool shown = !r.incomplete && d.has_swapchain();
  // The resolved front buffer into the back buffer (PRESENT in and out).
  present_->OnSwap(swap, shown);
  // The frame's texture copies read upload memory the untile workers fill.
  const uint64_t u0 = NowNs();
  textures_->FinishUploads();
  r.untile_ns = NowNs() - u0;
  draw_->BeforeSubmit();
  r.fence = d.frame_fence();
  if (!CloseUploadLists() || !d.EndFrame(shown, upload_lists_)) return r;
  const D3D12Device::EndFrameTimes& et = d.end_frame_times();
  r.submit_ns = et.submit_ns;
  r.present_ns = et.present_ns;
  r.fence_ns = et.fence_ns;
  r.presented = et.presented;
  r.presents = et.presents;
  r.completed = d.completed_fence();
  r.ok = true;
  return r;
}

void D3D12Backend::AfterSwap(uint64_t frame_index, uint64_t completed) {
  frame_index_ = frame_index;
  device_->SetTitleFrame(frame_index_);
  textures_->Retire(completed);
  draw_->EndFrame(completed);
  resolver_->AfterSwap();
  present_->AfterEndFrame();
}

// ---------------------------------------------------------------------------------------------
// Statistics.

const DrawStats& D3D12Backend::draw_stats() const { return draw_->stats(); }
void D3D12Backend::ResetDrawStats() { draw_->ResetStats(); }
const TextureStats& D3D12Backend::texture_stats() const { return textures_->stats(); }
uint64_t D3D12Backend::texture_resident_bytes() const { return textures_->resident_bytes(); }
uint64_t D3D12Backend::texture_count() const { return textures_->texture_count(); }
bool D3D12Backend::front_buffer_found() const { return present_->front_buffer_found(); }
const LumaProbe& D3D12Backend::luma_probe() const { return present_->luma_probe(); }

double D3D12Backend::gpu_last_frame_ms() const {
  return gpu_timer_ ? gpu_timer_->last_frame_ms() : 0.0;
}

void D3D12Backend::LogGpuTiming() {
  if (!gpu_timer_) return;
  gpu_timer_->Log();
  for (uint64_t hash : gpu_timer_->TakeNewTopShaders())
    REXLOG_INFO("GRAW native GPU: shader {:016X} ucode:\n{}", hash, draw_->ShaderDisassembly(hash));
}

}  // namespace graw::native
