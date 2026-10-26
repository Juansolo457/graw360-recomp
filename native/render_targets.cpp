// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): host render targets and Clear. See render_targets.h for the
// model (one host target per guest surface, no EDRAM emulation).
#include "render_targets.h"

#include "guest_memory.h"
#include "hook_args.h"
#include "log.h"

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace graw::native {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint32_t kMaxViews = 256;     // RTVs and DSVs each; PGR4 uses ~20 surfaces per frame
constexpr uint32_t kNullRtvs = 3;       // null RTVs for 1, 2 and 4 samples, at the heap's start
constexpr uint32_t kEdramTiles = 2048;  // 10 MiB of 80x16-sample tiles
constexpr uint32_t kMaxHeight = 2048;  // as surfaces.cpp

// Host DXGI format of a guest color render target format (UNKNOWN if unsupported).
DXGI_FORMAT ColorFormat(uint32_t f) {
  switch (f) {
    case 0:  // k_8_8_8_8
    case 1:  // k_8_8_8_8_GAMMA (gamma not applied on the host yet)
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:   // k_2_10_10_10_FLOAT (7e3)
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16: same EDRAM data, 16-bit blending
    case 7:   // k_16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4:
      return DXGI_FORMAT_R16G16_SNORM;  // k_16_16 (-32..32 on Xenos; scaled)
    case 5:
      return DXGI_FORMAT_R16G16B16A16_SNORM;  // k_16_16_16_16
    case 6:
      return DXGI_FORMAT_R16G16_FLOAT;
    case 14:
      return DXGI_FORMAT_R32_FLOAT;
    case 15:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

// Rows of pixels the EDRAM holds from `base` for this surface (what a non-tiling guest could
// render); the host target is at least this tall so it rarely has to grow.
uint32_t EdramRows(uint32_t base, uint32_t pitch, uint32_t samples, bool wide) {
  const uint32_t sx = samples >= 4 ? 2 : 1, sy = samples >= 2 ? 2 : 1;
  const uint32_t tiles_x = std::max(1u, (pitch * sx * (wide ? 2 : 1) + 79) / 80);
  const uint32_t free_tiles = base < kEdramTiles ? kEdramTiles - base : 0;
  return (free_tiles / tiles_x) * 16 / sy;
}

EdramGrid GridOf(const RenderTarget& rt) {
  return GetEdramGrid(rt.depth, rt.edram_base, rt.pitch, rt.samples, rt.guest_format);
}

// A pending clear's rectangle: empty for the whole target.
bool IsWhole(const D3D12_RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

bool Contains(const D3D12_RECT& outer, const D3D12_RECT& r) {
  return outer.left <= r.left && outer.top <= r.top && outer.right >= r.right &&
         outer.bottom >= r.bottom;
}

bool Intersects(const D3D12_RECT& a, const D3D12_RECT& b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// One clear call of `v` over `rects` (none: the whole target).
void Record(ID3D12GraphicsCommandList* list, RenderTarget* rt, const ClearValue& v,
            const D3D12_RECT* rects, UINT n) {
  if (rt->depth) {
    RenderTargets::Transition(list, rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAGS(v.flags), v.depth, v.stencil, n,
                                n ? rects : nullptr);
  } else {
    RenderTargets::Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rt->view, v.color, n, n ? rects : nullptr);
  }
}

}  // namespace

RenderTargets::RenderTargets() = default;
RenderTargets::~RenderTargets() = default;

bool RenderTargets::Initialize(ID3D12Device* device) {
  device_ = device;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kMaxViews + kNullRtvs;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)),
             "RT CreateDescriptorHeap(RTV)"))
    return false;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = kMaxViews;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)),
             "RT CreateDescriptorHeap(DSV)"))
    return false;
  rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  rtv_start_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = kMaxViews; i-- > 0;) {
    free_rtv_.push_back(kNullRtvs + i);
    free_dsv_.push_back(i);
  }
  // Null RTVs fill the gaps below the highest bound color target (D3D12 needs a descriptor in
  // every slot of OMSetRenderTargets; the sample count of a null view must match the others).
  for (uint32_t i = 0; i < kNullRtvs; ++i) {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    v.ViewDimension = i == 0 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DMS;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * rtv_stride_;
    device->CreateRenderTargetView(nullptr, &v, h);
  }
  return true;
}

uint32_t RenderTargets::AllocView(bool depth) {
  auto& list = depth ? free_dsv_ : free_rtv_;
  if (list.empty()) return UINT32_MAX;
  uint32_t i = list.back();
  list.pop_back();
  return i;
}

void RenderTargets::Retire(ComPtr<ID3D12Resource> resource) {
  if (resource) retired_.push_back({std::move(resource), frame_fence_});
}

bool RenderTargets::Create(RenderTarget* rt, uint32_t height) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = std::max(1u, rt->pitch);
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = rt->resource_format;
  d.SampleDesc.Count = rt->samples;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Flags =
      rt->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  const D3D12_RESOURCE_STATES initial =
      rt->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  ComPtr<ID3D12Resource> res;
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr,
                                                IID_PPV_ARGS(&res));
  if (FAILED(hr)) {
    REXLOG_ERROR("GRAW native RT: CreateCommittedResource {}x{} fmt {} x{} failed: {:08X}", d.Width,
                 d.Height, int(d.Format), rt->samples, uint32_t(hr));
    return false;
  }
  if (log_)
    std::fprintf(log_, "rt created %s base %u pitch %u x%u format %d %ux%u%s\n",
                 rt->depth ? "depth" : "color", rt->edram_base, rt->pitch, rt->samples,
                 int(rt->view_format), uint32_t(d.Width), height, rt->resource ? " (grown)" : "");
  Retire(std::move(rt->resource));
  rt->resource = std::move(res);
  rt->state = initial;
  rt->width = uint32_t(d.Width);
  rt->height = height;
  rt->pending_count = 0;  // a grown target loses the old one's content
  if (rt->view_index == UINT32_MAX) {
    rt->view_index = AllocView(rt->depth);
    if (rt->view_index == UINT32_MAX) {
      REXLOG_ERROR("GRAW native RT: out of {} views", rt->depth ? "depth" : "color");
      return false;
    }
  }
  if (rt->depth) {
    D3D12_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = rt->view_format;
    v.ViewDimension =
        rt->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    rt->view = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rt->view_index) * dsv_stride_;
    device_->CreateDepthStencilView(rt->resource.Get(), &v, rt->view);
  } else {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = rt->view_format;
    v.ViewDimension =
        rt->samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
    rt->view = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rt->view_index) * rtv_stride_;
    device_->CreateRenderTargetView(rt->resource.Get(), &v, rt->view);
  }
  return true;
}

RenderTarget* RenderTargets::Get(bool depth, uint32_t surface_info, uint32_t info,
                                 uint32_t height) {
  const uint32_t pitch = surface_info & 0x3FFF;
  const uint32_t samples = SurfaceSamples(surface_info);
  const uint32_t base = info & 0xFFF;
  DXGI_FORMAT resource, view, srv, stencil_srv = DXGI_FORMAT_UNKNOWN;
  uint32_t guest_format;
  bool wide = false;
  if (depth) {
    guest_format = (info >> 16) & 1;
    if (guest_format) {  // kD24FS8: 20e4 float depth, kept as 32-bit float on the host
      resource = DXGI_FORMAT_R32G8X24_TYPELESS;
      view = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      stencil_srv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    } else {  // kD24S8
      resource = DXGI_FORMAT_R24G8_TYPELESS;
      view = DXGI_FORMAT_D24_UNORM_S8_UINT;
      srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      stencil_srv = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    }
  } else {
    guest_format = (info >> 16) & 0xF;
    view = ColorFormat(guest_format);
    if (view == DXGI_FORMAT_UNKNOWN) {
      GRAW_LOG_FIRST(8, WARN, "GRAW native RT: unsupported color format {}", guest_format);
      return nullptr;
    }
    resource = srv = view;
    wide = IsColor64bpp(guest_format);
  }
  if (pitch == 0) return nullptr;
  const uint64_t key = (uint64_t(depth) << 63) | (uint64_t(samples >> 1) << 60) |
                       (uint64_t(base) << 44) | (uint64_t(pitch) << 30) | uint64_t(view);
  GetMemo& memo = get_memo_[(key ^ (key >> 30) ^ (key >> 44) ^ (key >> 60)) & 15];
  if (memo.rt && memo.key == key && std::min(height, kMaxHeight) <= memo.rt->height) {
    memo.rt->guest_format = guest_format;
    memo.rt->last_frame = frame_;
    return memo.rt;
  }
  uint32_t want = std::max(std::min(height, kMaxHeight), EdramRows(base, pitch, samples, wide));
  want = std::min((std::max(want, 8u) + 7) & ~7u, kMaxHeight);
  auto it = targets_.find(key);
  RenderTarget* rt;
  if (it == targets_.end()) {
    auto owned = std::make_unique<RenderTarget>();
    rt = owned.get();
    rt->depth = depth;
    rt->edram_base = base;
    rt->pitch = pitch;
    rt->samples = samples;
    rt->resource_format = resource;
    rt->view_format = view;
    rt->srv_format = srv;
    rt->stencil_srv_format = stencil_srv;
    rt->view_index = UINT32_MAX;
    if (!Create(rt, want)) {
      if (rt->view_index != UINT32_MAX) (depth ? free_dsv_ : free_rtv_).push_back(rt->view_index);
      return nullptr;
    }
    targets_.emplace(key, std::move(owned));
  } else {
    rt = it->second.get();
    if (std::min(height, kMaxHeight) > rt->height) {
      // Taller than anything this surface had: a new, taller target. The old content is lost
      // (not seen in PGR4's traces: every surface is created at its EDRAM height or more).
      REXLOG_WARN("GRAW native RT: growing {} base {} pitch {} x{} from {} to {} rows",
                  depth ? "depth" : "color", base, pitch, samples, rt->height, want);
      if (!Create(rt, want)) return nullptr;
    }
  }
  rt->guest_format = guest_format;
  rt->last_frame = frame_;
  memo = {key, rt};
  return rt;
}

RenderTarget* RenderTargets::GetColor(const SurfaceRegs& regs, uint32_t index, uint32_t height) {
  return Get(false, regs.surface_info, regs.color_info[index & 3], height);
}

RenderTarget* RenderTargets::GetDepth(const SurfaceRegs& regs, uint32_t height) {
  return Get(true, regs.surface_info, regs.depth_info, height);
}

void RenderTargets::Transition(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                               D3D12_RESOURCE_STATES state) {
  if (!rt || rt->state == state) return;
  D3D12_RESOURCE_BARRIER b = TransitionBarrier(rt->resource.Get(), rt->state, state);
  list->ResourceBarrier(1, &b);
  rt->state = state;
}

bool RenderTargets::Bind(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs,
                         uint32_t color_mask, bool depth_used, uint32_t height, BoundTargets* out) {
  *out = BoundTargets{};
  const uint32_t samples = SurfaceSamples(regs.surface_info);
  out->samples = samples;
  out->width = UINT32_MAX;
  out->height = UINT32_MAX;
  D3D12_CPU_DESCRIPTOR_HANDLE null_rtv = rtv_start_;
  null_rtv.ptr += size_t(samples == 4 ? 2 : samples == 2 ? 1 : 0) * rtv_stride_;
  for (uint32_t i = 0; i < 4; ++i) {
    out->rtv[i] = null_rtv;
    if (!(color_mask >> i & 1)) continue;
    RenderTarget* rt = GetColor(regs, i, height);
    if (!rt) return false;
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (rt->fill_seq != fills_.seq()) SyncEdram(list, rt);
    if (rt->pending_count) Flush(list, rt);
    out->color[i] = rt;
    out->rtv[i] = rt->view;
    out->color_count = i + 1;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (depth_used) {
    RenderTarget* rt = GetDepth(regs, height);
    if (!rt) return false;
    Transition(list, rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (rt->pending_count) Flush(list, rt);
    out->depth = rt;
    out->dsv = rt->view;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (out->width == UINT32_MAX) out->width = out->height = 0;
  // The draw renders over these tiles (conservatively: its targets' rows above `height`).
  if (!fills_.empty()) {
    for (uint32_t i = 0; i < out->color_count; ++i)
      if (out->color[i]) DropFills(*out->color[i], height);
    if (out->depth) DropFills(*out->depth, height);
  }
  return true;
}

void RenderTargets::Clear(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs,
                          uint32_t bound_colors, bool depth_bound, uint32_t height,
                          const ClearParams& p) {
  // What the clear leaves in the EDRAM, in pixels from the surface's base. A whole-surface
  // clear covers the rows of the call's height hint (the target itself may be taller).
  const std::vector<D3D12_RECT>& edram = p.edram_rects.empty() ? p.rects : p.edram_rects;
  auto edram_rows = [&](const RenderTarget& rt) {
    if (edram.empty()) return height ? std::min(height, rt.height) : rt.height;
    uint32_t rows = 0;
    for (const D3D12_RECT& r : edram) rows = std::max(rows, uint32_t(std::max<LONG>(r.bottom, 0)));
    return rows;
  };
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(p.flags >> i & 1) || !(bound_colors >> i & 1)) continue;
    RenderTarget* rt = GetColor(regs, i, height);
    if (!rt) continue;
    ClearValue v;
    v.flags = 1;
    std::memcpy(v.color, p.color, sizeof(v.color));
    Defer(list, rt, v, p.rects);
    if (edram.empty()) {
      RecordFill(*rt, {0, 0, LONG(rt->pitch), LONG(edram_rows(*rt))}, p.color);
    } else {
      for (const D3D12_RECT& r : edram) RecordFill(*rt, r, p.color);
    }
    rt->fill_seq = fills_.seq();
  }
  D3D12_CLEAR_FLAGS df = D3D12_CLEAR_FLAGS(0);
  if (p.flags & 0x10) df |= D3D12_CLEAR_FLAG_DEPTH;
  if (p.flags & 0x20) df |= D3D12_CLEAR_FLAG_STENCIL;
  if (df && depth_bound) {
    RenderTarget* rt = GetDepth(regs, height);
    if (rt) {
      ClearValue v;
      v.flags = df;
      v.depth = std::clamp(p.depth, 0.0f, 1.0f);
      v.stencil = p.stencil;
      Defer(list, rt, v, p.rects);
      if (!fills_.empty()) DropFills(*rt, edram_rows(*rt));
    }
  }
}

void RenderTargets::Defer(ID3D12GraphicsCommandList* list, RenderTarget* rt, const ClearValue& v,
                          const std::vector<D3D12_RECT>& rects) {
  DropCovered(rt, v.flags, rects.data(), rects.size());
  // Outside the rectangles the target shows what the EDRAM holds there.
  if (!rects.empty() && rt->fill_seq != fills_.seq()) SyncEdram(list, rt);
  const size_t n = std::max<size_t>(rects.size(), 1);
  if (rt->pending_count + n > kMaxPendingClears) Flush(list, rt);
  if (n > kMaxPendingClears) {
    Record(list, rt, v, rects.data(), UINT(rects.size()));
    return;
  }
  if (rects.empty()) {
    rt->pending[rt->pending_count++] = {v, {}};
  } else {
    for (const D3D12_RECT& r : rects) rt->pending[rt->pending_count++] = {v, r};
  }
}

void RenderTargets::DropCovered(RenderTarget* rt, uint32_t flags, const D3D12_RECT* rects,
                                size_t n) {
  PendingClear* const begin = rt->pending.data();
  PendingClear* const end =
      std::remove_if(begin, begin + rt->pending_count, [&](const PendingClear& c) {
        if ((flags & c.value.flags) != c.value.flags) return false;
        if (!n) return true;
        return !IsWhole(c.rect) && std::any_of(rects, rects + n, [&](const D3D12_RECT& r) {
          return Contains(r, c.rect);
        });
      });
  const uint32_t kept = uint32_t(end - begin);
  if (log_ && kept != rt->pending_count)
    std::fprintf(log_, "rt pending clears of %s base %u pitch %u x%u: %u dropped\n",
                 rt->depth ? "depth" : "color", rt->edram_base, rt->pitch, rt->samples,
                 rt->pending_count - kept);
  rt->pending_count = kept;
}

void RenderTargets::Flush(ID3D12GraphicsCommandList* list, RenderTarget* rt) {
  // Neighbours of one value (a tiled clear's tiles) go in one call.
  D3D12_RECT rects[kMaxPendingClears];
  uint32_t calls = 0;
  for (uint32_t i = 0; i < rt->pending_count; ++calls) {
    const PendingClear& first = rt->pending[i];
    UINT n = 0;
    if (IsWhole(first.rect)) {
      ++i;
    } else {
      for (; i < rt->pending_count && !IsWhole(rt->pending[i].rect) &&
             rt->pending[i].value == first.value;
           ++i)
        rects[n++] = rt->pending[i].rect;
    }
    Record(list, rt, first.value, rects, n);
  }
  if (log_)
    std::fprintf(log_, "rt pending clears of %s base %u pitch %u x%u: %u recorded in %u calls\n",
                 rt->depth ? "depth" : "color", rt->edram_base, rt->pitch, rt->samples,
                 rt->pending_count, calls);
  rt->pending_count = 0;
}

void RenderTargets::PrepareRead(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                                const D3D12_RECT& r) {
  if (!rt->depth) SyncEdram(list, rt);
  if (std::any_of(rt->pending.begin(), rt->pending.begin() + rt->pending_count,
                  [&](const PendingClear& c) { return IsWhole(c.rect) || Intersects(c.rect, r); }))
    Flush(list, rt);
}

void RenderTargets::RecordFill(const RenderTarget& rt, const D3D12_RECT& r, const float color[4]) {
  fills_.Record(GridOf(rt), rt.guest_format, r, color);
}

void RenderTargets::DropFills(const RenderTarget& rt, uint32_t rows) {
  fills_.Drop(GridOf(rt), rows);
}

void RenderTargets::SyncEdram(ID3D12GraphicsCommandList* list, RenderTarget* rt) {
  const uint64_t since = rt->fill_seq;
  rt->fill_seq = fills_.seq();
  if (rt->depth || since == rt->fill_seq) return;
  const EdramGrid g = GridOf(*rt);
  fills_.ForEach(since, g, rt->guest_format, rt->width, rt->height,
                 [&](const EdramFills::Fill& f, const D3D12_RECT* rects, uint32_t nr) {
    // The pending clears are older than the fill: those under it are never seen.
    if (rt->pending_count) {
      DropCovered(rt, 1, rects, nr);
      if (rt->pending_count) Flush(list, rt);
    }
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rt->view, f.color, nr, rects);
    if (log_)
      std::fprintf(log_,
                   "edram fill seq %llu at tile %u pitch %u color %g,%g,%g,%g applied to base %u "
                   "pitch %u x%u rows %ld-%ld (%u rects)\n",
                   (unsigned long long)f.seq, f.base, f.pitch, f.color[0], f.color[1], f.color[2],
                   f.color[3], g.base, g.pitch, rt->samples, rects[0].top, rects[nr - 1].bottom,
                   nr);
    GRAW_LOG_FIRST(4, INFO,
                   "GRAW native RT: EDRAM fill at tile {} pitch {} (alpha {}) applied to base {} "
                   "pitch {} x{} rows {}-{}",
                   f.base, f.pitch, f.color[3], g.base, g.pitch, rt->samples, rects[0].top,
                   rects[nr - 1].bottom);
  });
}

void RenderTargets::BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [&](const Retired& r) { return r.fence <= completed_fence; }),
                 retired_.end());
  for (auto it = targets_.begin(); it != targets_.end();) {
    RenderTarget* rt = it->second.get();
    if (rt->last_frame + kMaxAge < frame) {
      // Last used kMaxAge frames ago: no list in flight references it, but retire it with the
      // current fence anyway (cheap) so the rule is the same everywhere.
      if (log_)
        std::fprintf(log_, "rt dropped %s base %u pitch %u x%u (last used frame %llu)\n",
                     rt->depth ? "depth" : "color", rt->edram_base, rt->pitch, rt->samples,
                     (unsigned long long)rt->last_frame);
      Retire(std::move(rt->resource));
      (rt->depth ? free_dsv_ : free_rtv_).push_back(rt->view_index);
      it = targets_.erase(it);
      for (GetMemo& m : get_memo_) m = {};
    } else {
      ++it;
    }
  }
}

}  // namespace graw::native
