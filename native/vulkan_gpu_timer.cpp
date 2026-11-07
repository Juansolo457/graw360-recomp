// GRAW native renderer: GPU time per pass of the Vulkan frame (vulkan_gpu_timer.h). The keys,
// groups, spikes and log lines follow gpu_timer.cpp (D3D12, from pgr4-native 8ea20d5, MIT; see
// native/README.md); only the timestamps are Vulkan's.
#include "vulkan_gpu_timer.h"

#include "log.h"
#include "resolve_params.h"
#include "surfaces.h"
#include "vulkan_device.h"

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace graw::native {

namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using enum rex::graphics::Register;  // XE_GPU_REG_*

constexpr uint32_t kMaxQueries = VulkanGpuTimer::kMaxSpans + 1;

// Key layout (gpu_timer.cpp): kind in bits 0-2, the occurrence in the frame in bits 3-7 (set by
// Switch), then the kind's fields from bit 8 in the order the Key* functions write them.
// kShaders: draws keyed by their shader pair (per_shader_); counted as draws in the totals.
enum Kind : uint32_t { kStart, kDraw, kClear, kResolve, kPresent, kShaders, kKindCount };
constexpr const char* kKindNames[kKindCount] = {"start",   "draws",  "clears",
                                                "resolves", "present", "shaders"};
constexpr uint32_t kKindMask = 7;
constexpr uint32_t kOccurrenceShift = 3, kOccurrenceMax = 31;
constexpr uint32_t kFieldShift = 8;

class KeyWriter {
 public:
  explicit KeyWriter(Kind kind) : key_(kind) {}
  KeyWriter& Put(uint32_t value, uint32_t bits) {
    key_ |= uint64_t(value & ((1u << bits) - 1)) << shift_;
    shift_ += bits;
    return *this;
  }
  uint64_t key() const { return key_; }

 private:
  uint64_t key_;
  uint32_t shift_ = kFieldShift;
};

class KeyReader {
 public:
  explicit KeyReader(uint64_t key) : key_(key) {}
  uint32_t Get(uint32_t bits) {
    const uint32_t v = uint32_t(key_ >> shift_) & ((1u << bits) - 1);
    shift_ += bits;
    return v;
  }

 private:
  uint64_t key_;
  uint32_t shift_ = kFieldShift;
};

Kind KindOf(uint64_t key) { return Kind(key & kKindMask); }

// The key without its occurrence: the pass, whichever run of it in the frame.
uint64_t WithoutOccurrence(uint64_t key) {
  return key & ~(uint64_t(kOccurrenceMax) << kOccurrenceShift);
}

const char* ColorFormatName(uint32_t f) {
  return xenos::GetColorRenderTargetFormatName(xenos::ColorRenderTargetFormat(f));
}
const char* DepthFormatName(uint32_t f) {
  return xenos::GetDepthRenderTargetFormatName(xenos::DepthRenderTargetFormat(f));
}

}  // namespace

VulkanGpuTimer::~VulkanGpuTimer() {
  if (pool_) device_->fn().vkDestroyQueryPool(device_->device(), pool_, nullptr);
}

bool VulkanGpuTimer::Initialize(VulkanDevice& device, uint32_t slots) {
  device_ = &device;
  if (!device.timestamp_mask()) return false;
  ns_per_tick_ = device.timestamp_period_ns();
  tick_mask_ = device.timestamp_mask();
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qi.queryCount = slots * kMaxQueries;
  if (device.fn().vkCreateQueryPool(device.device(), &qi, nullptr, &pool_) != VK_SUCCESS) {
    pool_ = VK_NULL_HANDLE;
    return false;
  }
  frames_.resize(slots);
  for (Frame& f : frames_) f.spans.reserve(kMaxSpans);
  ticks_.resize(kMaxQueries);
  REXLOG_INFO("GRAW native Vulkan: GPU timing on ({:.2f} ns per timestamp tick, up to {} spans "
              "per frame{})",
              ns_per_tick_, kMaxSpans, per_shader_ ? ", draws per shader pair" : "");
  return true;
}

void VulkanGpuTimer::Stamp(VkCommandBuffer cmd) {
  Frame& f = frames_[slot_];
  device_->fn().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                                    slot_ * kMaxQueries + f.queries++);
}

void VulkanGpuTimer::BeginFrame(VkCommandBuffer cmd, uint32_t slot) {
  Collect(slot);
  slot_ = slot;
  Frame& f = frames_[slot];
  f.spans.clear();
  f.number = next_number_++;
  f.queries = 0;
  occurrences_.clear();
  current_ = kStart;
  // Outside any render pass: the device calls this right after beginning the command buffer.
  device_->fn().vkCmdResetQueryPool(cmd, pool_, slot * kMaxQueries, kMaxQueries);
  Stamp(cmd);
  f.spans.push_back({kStart, 0});
  active_ = true;
}

void VulkanGpuTimer::Switch(VkCommandBuffer cmd, uint64_t key) {
  Frame& f = frames_[slot_];
  current_ = key;
  if (f.spans.size() >= kMaxSpans) {
    GRAW_WARN_ONCE(
        "GRAW native Vulkan: GPU timing: more than {} passes in a frame; the rest are charged to "
        "the last one",
        kMaxSpans);
    return;
  }
  Stamp(cmd);
  if (KindOf(key) == kShaders) {
    f.spans.push_back({key, 0});
    return;
  }
  uint32_t& n = occurrences_[key];
  f.spans.push_back({key | uint64_t(std::min(n, kOccurrenceMax)) << kOccurrenceShift, 0});
  ++n;
}

void VulkanGpuTimer::EndFrame(VkCommandBuffer cmd) {
  if (!active_) return;
  Stamp(cmd);
  frames_[slot_].pending = true;
  active_ = false;
}

void VulkanGpuTimer::Collect(uint32_t slot) {
  Frame& f = frames_[slot];
  if (!f.pending) return;
  f.pending = false;
  if (f.queries < 2) return;
  // The slot's fence was waited for: every query is available, so no wait flag is needed.
  if (device_->fn().vkGetQueryPoolResults(device_->device(), pool_, slot * kMaxQueries, f.queries,
                                          f.queries * sizeof(uint64_t), ticks_.data(),
                                          sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  const uint64_t* ts = ticks_.data();
  const auto ns = [this](uint64_t from, uint64_t to) {
    return uint64_t(double((to - from) & tick_mask_) * ns_per_tick_);
  };
  const uint64_t begin = ts[0], end = ts[f.queries - 1];
  const uint64_t frame_ns = ns(begin, end);
  const bool spike = IsSpike(frame_ns);
  if (spike) spike_spans_.clear();
  // One timestamp more than spans: span i runs from ts[i] to ts[i + 1].
  const size_t spans = std::min<size_t>(f.spans.size(), f.queries - 1);
  for (size_t i = 0; i < spans; ++i) {
    const uint64_t span_ns = ns(ts[i], ts[i + 1]);
    Group& g = groups_[f.spans[i].key];
    g.ns += span_ns;
    ++g.spans;
    g.commands += f.spans[i].commands;
    g.position += i;
    if (spike) spike_spans_.push_back({span_ns, uint32_t(i)});
  }
  if (spike) LogSpike(f, frame_ns);
  ++frames_collected_;
  frame_ns_ += frame_ns;
  frame_ns_max_ = std::max(frame_ns_max_, frame_ns);
  if (last_end_) {
    const uint64_t idle = ns(last_end_, begin);
    // A wrapped or reordered pair reads as a huge gap: only plausible idle time counts.
    if (idle < 1000000000ull) idle_ns_ += idle;
  }
  last_end_ = end;
  spans_collected_ += spans;
}

bool VulkanGpuTimer::IsSpike(uint64_t ns) {
  if (!ns) return false;
  if (!typical_ns_) {
    typical_ns_ = ns;
    return false;
  }
  // 1.75x the running mean and at least 5 ms more: a frame the player sees as a hitch.
  const bool spike = ns > typical_ns_ * 7 / 4 && ns - typical_ns_ > 5000000;
  const uint64_t clamped = std::min(ns, typical_ns_ * 2);
  typical_ns_ = typical_ns_ - typical_ns_ / 32 + clamped / 32;
  return spike;
}

void VulkanGpuTimer::LogSpike(const Frame& f, uint64_t ns) {
  constexpr uint32_t kSpikesPerWindow = 4, kSpansShown = 4;
  if (spikes_logged_ >= kSpikesPerWindow) return;
  ++spikes_logged_;
  const size_t shown = std::min<size_t>(kSpansShown, spike_spans_.size());
  std::partial_sort(spike_spans_.begin(), spike_spans_.begin() + shown, spike_spans_.end(),
                    [](const SpikeSpan& a, const SpikeSpan& b) { return a.ns > b.ns; });
  std::string spans;
  char item[256];
  for (size_t i = 0; i < shown; ++i) {
    const SpikeSpan& s = spike_spans_[i];
    const uint64_t key = f.spans[s.index].key;
    const Group& g = groups_[key];
    std::snprintf(item, sizeof(item), "%s%.2f ms (mean %.2f) span %u: %s", i ? " | " : "",
                  double(s.ns) / 1e6, g.spans ? double(g.ns) / 1e6 / double(g.spans) : 0.0,
                  s.index, Describe(WithoutOccurrence(key)).c_str());
    spans += item;
  }
  REXLOG_INFO("GRAW native GPU spike: frame {} {:.2f} ms (typical {:.2f}), {} spans: {}", f.number,
              double(ns) / 1e6, double(typical_ns_) / 1e6, spike_spans_.size(), spans);
}

std::string VulkanGpuTimer::Describe(uint64_t key) const {
  KeyReader r(key);
  char s[192];
  switch (KindOf(key)) {
    case kStart:
      return "frame start (before the first pass)";
    case kDraw: {
      const uint32_t pitch = r.Get(14), samples = r.Get(3), writes = r.Get(4), c0_format = r.Get(4),
                     c0_base = r.Get(12), depth_used = r.Get(1), depth_format = r.Get(1),
                     depth_base = r.Get(12);
      int n = std::snprintf(s, sizeof(s), "draws   pitch %u %ux", pitch, samples);
      if (writes)
        n += std::snprintf(s + n, sizeof(s) - n, ", color rt0 %s@%u (writes %X)",
                           ColorFormatName(c0_format), c0_base, writes);
      if (depth_used)
        std::snprintf(s + n, sizeof(s) - n, ", depth %s@%u", DepthFormatName(depth_format),
                      depth_base);
      return s;
    }
    case kClear: {
      const uint32_t flags = r.Get(6), pitch = r.Get(14), samples = r.Get(3), c0_format = r.Get(4),
                     c0_base = r.Get(12), depth_base = r.Get(12);
      std::snprintf(s, sizeof(s), "clear   pitch %u %ux, flags %02X, rt0 %s@%u, depth @%u", pitch,
                    samples, flags, ColorFormatName(c0_format), c0_base, depth_base);
      return s;
    }
    case kResolve: {
      const uint32_t source = r.Get(3), samples = r.Get(3), source_format = r.Get(4),
                     width = r.Get(13), height = r.Get(13), dest_format = r.Get(6),
                     clears = r.Get(2);
      const auto* info = rex::graphics::FormatInfo::Get(dest_format);
      std::snprintf(s, sizeof(s), "resolve %s %ux%u %ux -> %s%s%s",
                    source == 4 ? DepthFormatName(source_format) : ColorFormatName(source_format),
                    width, height, samples, info ? info->name : "?",
                    clears & 1 ? ", clears color" : "", clears & 2 ? ", clears depth" : "");
      return s;
    }
    case kPresent:
      return "present blit";
    case kShaders: {
      KeyReader k(key);
      const uint32_t pitch = k.Get(14), samples = k.Get(3), id = k.Get(24);
      const auto [vs, ps] = id < pairs_.size() ? pairs_[id] : std::pair<uint64_t, uint64_t>{};
      std::snprintf(s, sizeof(s), "shaders VS %016llX PS %016llX, pitch %u %ux",
                    (unsigned long long)vs, (unsigned long long)ps, pitch, samples);
      return s;
    }
    default:
      return "?";
  }
}

uint64_t VulkanGpuTimer::KeyDraw(const uint32_t* regs) {
  const uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  const reg::RB_SURFACE_INFO surface{.value = surface_info};
  const reg::RB_COLOR_INFO color0{.value = regs[XE_GPU_REG_RB_COLOR_INFO]};
  const reg::RB_DEPTH_INFO depth{.value = regs[XE_GPU_REG_RB_DEPTH_INFO]};
  const reg::RB_DEPTHCONTROL depth_control{.value = regs[XE_GPU_REG_RB_DEPTHCONTROL]};
  const uint32_t mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  uint32_t writes = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if ((mask >> (4 * i)) & 15) writes |= 1u << i;
  return KeyWriter(kDraw)
      .Put(surface.surface_pitch, 14)
      .Put(SurfaceSamples(surface_info), 3)
      .Put(writes, 4)
      .Put(uint32_t(color0.color_format), 4)
      .Put(color0.value & 0xFFF, 12)
      .Put(depth_control.z_enable || depth_control.stencil_enable, 1)
      .Put(uint32_t(depth.depth_format), 1)
      .Put(depth.value & 0xFFF, 12)
      .key();
}

uint64_t VulkanGpuTimer::KeyClear(const ClearCall& c) {
  const reg::RB_SURFACE_INFO surface{.value = c.regs.surface_info};
  const reg::RB_COLOR_INFO color0{.value = c.regs.color_info[0]};
  return KeyWriter(kClear)
      .Put(c.p.flags, 6)
      .Put(surface.surface_pitch, 14)
      .Put(SurfaceSamples(c.regs.surface_info), 3)
      .Put(uint32_t(color0.color_format), 4)
      .Put(color0.value & 0xFFF, 12)
      .Put(c.regs.depth_info & 0xFFF, 12)
      .key();
}

uint64_t VulkanGpuTimer::KeyResolve(const ResolveParams& p) {
  const uint32_t source_format =
      p.source == 4
          ? uint32_t(reg::RB_DEPTH_INFO{.value = p.regs.depth_info}.depth_format)
          : uint32_t(reg::RB_COLOR_INFO{.value = p.regs.color_info[p.source & 3]}.color_format);
  return KeyWriter(kResolve)
      .Put(p.source, 3)
      .Put(SurfaceSamples(p.regs.surface_info), 3)
      .Put(source_format, 4)
      .Put(uint32_t(std::clamp(p.x1 - p.x0, 0, 8191)), 13)
      .Put(uint32_t(std::clamp(p.y1 - p.y0, 0, 8191)), 13)
      .Put(p.dest_format, 6)
      .Put((p.clear_flags >> 8) & 3, 2)
      .key();
}

uint64_t VulkanGpuTimer::KeyPresent() { return KeyWriter(kPresent).key(); }

uint64_t VulkanGpuTimer::KeyDrawShaders(const uint32_t* regs, uint64_t vs_hash, uint64_t ps_hash) {
  const uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  const reg::RB_SURFACE_INFO surface{.value = surface_info};
  const auto [it, added] =
      pair_ids_.try_emplace(vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull), uint32_t(pairs_.size()));
  if (added) pairs_.emplace_back(vs_hash, ps_hash);
  return KeyWriter(kShaders)
      .Put(surface.surface_pitch, 14)
      .Put(SurfaceSamples(surface_info), 3)
      .Put(it->second, 24)
      .key();
}

void VulkanGpuTimer::Log() {
  if (!frames_collected_) return;
  const double n = double(frames_collected_);
  const auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / n; };
  double kind_ms[kKindCount] = {};
  std::vector<std::pair<uint64_t, const Group*>> top;
  top.reserve(groups_.size());
  for (const auto& [key, g] : groups_) {
    kind_ms[KindOf(key) == kShaders ? kDraw : KindOf(key)] += ms(g.ns);
    top.emplace_back(key, &g);
  }
  REXLOG_INFO(
      "GRAW native GPU: frames {} | GPU {:.2f} ms/frame (max {:.2f}), idle between frames "
      "{:.2f} ms, spans/frame {:.0f} | {} {:.2f}, {} {:.2f}, {} {:.2f}, {} {:.2f}, {} {:.2f} ms",
      frames_collected_, ms(frame_ns_), double(frame_ns_max_) / 1e6, ms(idle_ns_),
      double(spans_collected_) / n, kKindNames[kDraw], kind_ms[kDraw], kKindNames[kClear],
      kind_ms[kClear], kKindNames[kResolve], kind_ms[kResolve], kKindNames[kPresent],
      kind_ms[kPresent], kKindNames[kStart], kind_ms[kStart]);
  // The costliest groups, in frame order.
  constexpr size_t kTop = 24;
  const size_t shown = std::min(kTop, top.size());
  std::partial_sort(top.begin(), top.begin() + shown, top.end(),
                    [](const auto& a, const auto& b) { return a.second->ns > b.second->ns; });
  top.resize(shown);
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
    return double(a.second->position) / double(a.second->spans) <
           double(b.second->position) / double(b.second->spans);
  });
  const double frame_ms = ms(frame_ns_);
  for (const auto& [key, g] : top) {
    const uint32_t occurrence = uint32_t(key >> kOccurrenceShift) & kOccurrenceMax;
    REXLOG_INFO(
        "GRAW native GPU: {:6.3f} ms {:5.1f} %  span {:4.0f}  #{:<2} x{:.2f}  {:5.0f} cmds  {}",
        ms(g->ns), frame_ms > 0 ? 100.0 * ms(g->ns) / frame_ms : 0.0,
        double(g->position) / double(g->spans), occurrence, double(g->spans) / n,
        double(g->commands) / n, Describe(WithoutOccurrence(key)));
  }
  groups_.clear();
  frames_collected_ = frame_ns_ = frame_ns_max_ = idle_ns_ = spans_collected_ = 0;
  spikes_logged_ = 0;
}

}  // namespace graw::native
