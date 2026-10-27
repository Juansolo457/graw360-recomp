// GRAW native renderer: the push-buffer state tracker (d3d_state.h). The packet decoder follows
// pgr4-native 8ea20d5's trace tap (pgr4_d3d_trace.cpp Feed/HandlePacket, MIT; see
// native/README.md).
#include "d3d_state.h"

#include "guest_memory.h"
#include "native_thread.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <xxhash.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(graw_native_stream_log, false, "GRAW native",
                    "Log the push-buffer tracker's counters now and then (decoded dwords, "
                    "packets, draws, jumps, resyncs, torn packets, unknown opcodes)");

namespace graw::native {
namespace {

// First register of each constant kind in SET_CONSTANT and LOAD_ALU_CONSTANT: ALU (float),
// fetch, bool, loop, register.
constexpr uint32_t kConstantBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

// A window longer than this is not one call's packets (the D3D's segments are a few KB).
constexpr uint32_t kMaxWindowBytes = 4u << 20;

// Register blocks of the device's shadow, the tracker's starting point at Attach (the widths
// the draw flush sends; graw_d3d.h).
struct ShadowBlock {
  uint32_t offset, reg, count;
};
constexpr ShadowBlock kShadow[] = {
    {dev::kVsConstants, 0x4000, 1024}, {dev::kPsConstants, 0x4400, 1024},
    {dev::kFetchConstants, 0x4800, 192}, {dev::kBoolLoop, 0x4900, 40},
    {dev::kShadow2000, 0x2000, 16},      {dev::kShadow2100, 0x2100, 21},
    {dev::kShadow2180, 0x2180, 5},       {dev::kShadow2200, 0x2200, 12},
    {dev::kShadow2280, 0x2280, 21},      {dev::kShadow2300, 0x2300, 38},
    {dev::kShadow2380, 0x2380, 8},
};

const uint8_t* Physical(uint32_t address) {
  return rex::system::kernel_memory()->TranslatePhysical<const uint8_t*>(address);
}

}  // namespace

void RegLog::Grow(size_t capacity) {
  capacity = std::max({capacity, capacity_ * 2, size_t(4096)});
  auto data = std::make_unique_for_overwrite<RegWrite[]>(capacity);
  if (size_) std::memcpy(data.get(), data_.get(), size_ * sizeof(RegWrite));
  data_ = std::move(data);
  capacity_ = capacity;
}

void MarkAllDirty(uint8_t* base, uint32_t device) {
  for (uint32_t i = 0; i < 5; ++i) {
    const uint32_t a = device + dev::kDirtyMasks + i * 8;
    Store64(base, a, Load64(base, a) | kDirtyBits[i]);
  }
}

GuestStateTracker& GuestState() {
  static GuestStateTracker* tracker = new GuestStateTracker();  // ~100 KB, never freed
  return *tracker;
}

void GuestStateTracker::RecordInto(RegLog* sink) {
  if (sink && !synced_) {
    synced_ = true;
    for (uint32_t reg = 0; reg < kGuestRegisterCount; ++reg)
      if (regs_[reg]) sink->Append({reg, regs_[reg]});
  }
  sink_ = sink;
}

void GuestStateTracker::PutFromGuest(uint32_t reg, const uint8_t* p, uint32_t count) {
  if (reg >= kGuestRegisterCount) return;
  count = std::min(count, kGuestRegisterCount - reg);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    Put(reg + i, __builtin_bswap32(v));
  }
}

void GuestStateTracker::Attach(uint8_t* base, uint32_t device) {
  // A new device, or the first call after Resync: start from its shadow, and make the next draw
  // send everything again (the shaders included, which the shadow does not name), so the
  // tracker's view is complete from that draw on.
  static uint32_t attaches = 0;
  if (++attaches <= 8)
    REXLOG_INFO("GRAW native state: tracking device {:08X} (was {:08X})", device, device_);
  device_ = device;
  cursor_ = Load32(base, device + dev::kPush);
  in_packet_ = false;
  first_draw_valid_ = false;
  for (const ShadowBlock& b : kShadow) PutFromGuest(b.reg, HostPtr(base, device + b.offset), b.count);
  vs_ = ps_ = {};
  MarkAllDirty(base, device);
}

void GuestStateTracker::Sync(uint8_t* base, uint32_t device, uint32_t fn, bool entry) {
  if (!device) return;
  if (device != device_) Attach(base, device);
  const uint32_t cur = Load32(base, device + dev::kPush);
  if (cur == cursor_) return;
  current_fn_ = fn;
  current_entry_ = entry;
  if (cur > cursor_ && cur - cursor_ <= kMaxWindowBytes) {
    // Each window starts at a packet boundary: the D3D completes its packets before it calls
    // another function or returns. A packet still open here was cut short (or a length was
    // misread); drop it rather than let a bad length swallow the following stream.
    if (in_packet_) {
      ++counters_.torn;
      static uint32_t logged = 0;
      if (++logged <= 24)
        REXLOG_WARN(
            "GRAW native state: torn packet {:08X} at {:08X} still needs {} dwords: window ended "
            "at {:08X}, next one at {:08X} starts {:08X} (thread {}, last {})",
            header_, packet_address_, need_, last_sync_fn_, fn, Load32(base, cursor_ + 4),
            CurrentThreadId(), last_sync_thread_);
      in_packet_ = false;
    }
    for (uint32_t a = cursor_ + 4; a <= cur; a += 4) Feed(base, a, Load32(base, a));
    counters_.dwords += (cur - cursor_) / 4;
  } else {
    // The cursor moved somewhere no hooked jump reported (backwards, or too far).
    ++counters_.resyncs;
    in_packet_ = false;
    static uint32_t logged = 0;
    if (++logged <= 16)
      REXLOG_WARN("GRAW native state: push cursor {:08X} -> {:08X} at {:08X}: skipped", cursor_,
                  cur, fn);
  }
  cursor_ = cur;
  last_sync_fn_ = fn;
  last_sync_thread_ = CurrentThreadId();
}

void GuestStateTracker::Jump(const uint8_t* base, uint32_t device, uint32_t fn) {
  if (!device || device != device_) return;
  ++counters_.jumps;
  if (in_packet_) {
    ++counters_.torn;
    static uint32_t logged = 0;
    if (++logged <= 24)
      REXLOG_WARN(
          "GRAW native state: packet {:08X} at {:08X} open at the jump of {:08X}: has {} dwords, "
          "needs {} more; cursor {:08X} -> {:08X}, then {:08X} {:08X} (thread {})",
          header_, packet_address_, fn, pkt_.size(), need_, cursor_,
          Load32(base, device + dev::kPush), Load32(base, Load32(base, device + dev::kPush) + 4),
          Load32(base, Load32(base, device + dev::kPush) + 8), CurrentThreadId());
    in_packet_ = false;
  }
  cursor_ = Load32(base, device + dev::kPush);
}

void GuestStateTracker::Feed(const uint8_t* base, uint32_t address, uint32_t dw) {
  if (!in_packet_) {
    // Space the D3D reserves and patches later is zero; the D3D never writes register 0, so a
    // zero header is padding, not a one-register type-0 packet.
    if (dw == 0) return;
    header_ = dw;
    packet_address_ = address;
    pkt_.clear();
    switch (dw >> 30) {
      case 0:
      case 3:
        need_ = ((dw >> 16) & 0x3FFF) + 1;
        break;
      case 1:
        need_ = 2;
        break;
      default:
        return;  // type 2: NOP
    }
    in_packet_ = true;
    return;
  }
  pkt_.push_back(dw);
  if (--need_ == 0) {
    in_packet_ = false;
    ++counters_.packets;
    HandlePacket(base);
  }
}

void GuestStateTracker::LoadShader(uint32_t type, uint32_t address, uint32_t dwords) {
  if (!address || !dwords) return;
  ++counters_.shader_loads;
  ShaderRef ref;
  ref.ucode = address;
  ref.dwords = dwords;
  ref.binding = XXH3_64bits(Physical(address), size_t(dwords) * 4);
  (type == 0 ? vs_ : ps_) = ref;
}

void GuestStateTracker::LoadShaderImmediate(uint32_t type, uint32_t address,
                                            const uint32_t* ucode_host, uint32_t dwords) {
  if (!dwords) return;
  ++counters_.immediate_loads;
  std::vector<uint32_t>& copy = type == 0 ? vs_imm_ : ps_imm_;
  copy.resize(dwords);
  for (uint32_t i = 0; i < dwords; ++i) copy[i] = __builtin_bswap32(ucode_host[i]);
  ShaderRef ref;
  ref.ucode = GpuAddress(address);
  ref.dwords = dwords;
  ref.immediate = copy.data();
  ref.immediate_seq = ++imm_seq_;
  (type == 0 ? vs_ : ps_) = ref;
}

void GuestStateTracker::HandlePacket(const uint8_t* base) {
  (void)base;
  const uint32_t type = header_ >> 30;
  if (type == 0) {
    const uint32_t reg = header_ & 0x7FFF;
    const bool one = (header_ >> 15) & 1;
    for (size_t i = 0; i < pkt_.size(); ++i) Put(one ? reg : reg + uint32_t(i), pkt_[i]);
    return;
  }
  if (type == 1) {
    Put(header_ & 0x7FF, pkt_[0]);
    Put((header_ >> 11) & 0x7FF, pkt_[1]);
    return;
  }
  const uint32_t op = (header_ >> 8) & 0x7F;
  const bool predicated = header_ & 1;
  auto range = [&](uint32_t reg, size_t first) {
    if (predicated) {
      ++counters_.predicated_writes;
      // Which registers the D3D writes per tile (renderer discovery: each needs its untiled
      // value, see AfterCall).
      static uint32_t logged = 0;
      static std::vector<bool> seen(kGuestRegisterCount);
      if (logged < 64 && reg < kGuestRegisterCount && !seen[reg]) {
        seen[reg] = true;
        ++logged;
        std::string values;
        for (size_t i = first; i < pkt_.size() && i < first + 8; ++i)
          values += fmt::format(" {:08X}", pkt_[i]);
        REXLOG_INFO("GRAW native state: predicated write {:04X} x{}:{} (op {:02X}, thread {})", reg,
                    pkt_.size() - first, values, op, CurrentThreadId());
      }
    }
    for (size_t i = first; i < pkt_.size(); ++i) Put(reg + uint32_t(i - first), pkt_[i]);
  };
  switch (op) {
    case 0x2D: {  // SET_CONSTANT
      if (pkt_.empty()) break;
      const uint32_t index = pkt_[0] & 0x7FF, kind = (pkt_[0] >> 16) & 0xFF;
      if (kind < std::size(kConstantBase)) range(kConstantBase[kind] + index, 1);
      break;
    }
    case 0x55:  // SET_CONSTANT2
    case 0x56:  // SET_SHADER_CONSTANTS
      if (!pkt_.empty()) range(pkt_[0] & 0xFFFF, 1);
      break;
    case 0x2F: {  // LOAD_ALU_CONSTANT: from guest memory, as it is now
      if (pkt_.size() < 3) break;
      const uint32_t address = pkt_[0] & 0x3FFFFFFF, index = pkt_[1] & 0x7FF,
                     kind = (pkt_[1] >> 16) & 0xFF, n = pkt_[2] & 0xFFF;
      if (kind < std::size(kConstantBase) && n)
        PutFromGuest(kConstantBase[kind] + index, Physical(address), n);
      break;
    }
    case 0x27:  // IM_LOAD: type (0 vertex, 1 pixel) in the address's low bits
      if (pkt_.size() >= 2) LoadShader(pkt_[0] & 3, pkt_[0] & ~3u, pkt_[1] & 0xFFFF);
      break;
    case 0x2B: {  // IM_LOAD_IMMEDIATE: type, size, then the ucode
      if (pkt_.size() < 2) break;
      const uint32_t dwords = std::min<uint32_t>(pkt_[1] & 0xFFFF, uint32_t(pkt_.size() - 2));
      // The ucode starts after the header and the two dwords: its push-buffer address is the key.
      LoadShaderImmediate(pkt_[0] & 3, packet_address_ + 12, pkt_.data() + 2, dwords);
      break;
    }
    case 0x22:  // DRAW_INDX: viz query, initiator, [index base, index size]
    case 0x36:  // DRAW_INDX_2: initiator, inline indices
    case 0x34:
    case 0x35:
      ++counters_.draws;
      // The D3D's own one-point draws (DRAW_INDX_2, one auto-indexed point, no index data),
      // dozens per frame before Swap and BeginTiling, are not counted.
      if (current_entry_ && !(op == 0x36 && pkt_.size() == 1 && pkt_[0] == 0x00010081)) {
        ++counters_.unhooked_draws;
        static uint32_t logged = 0;
        if (++logged <= 40)
          REXLOG_WARN(
              "GRAW native state: draw packet {:08X} ({} dwords, initiator {:08X}) at {:08X} "
              "written before call {:08X}: not from a hooked draw",
              header_, pkt_.size(), op == 0x22 ? (pkt_.size() > 1 ? pkt_[1] : 0) : pkt_[0],
              packet_address_, current_fn_);
      }
      if (!first_draw_valid_) {
        first_draw_valid_ = true;
        first_draw_op_ = op;
        first_draw_initiator_ =
            op == 0x22 ? (pkt_.size() > 1 ? pkt_[1] : 0) : (pkt_.empty() ? 0 : pkt_[0]);
        first_draw_index_base_ = op == 0x22 && pkt_.size() >= 4 ? pkt_[2] : 0;
        first_draw_index_size_ = op == 0x22 && pkt_.size() >= 4 ? pkt_[3] : 0;
      }
      break;
    case 0x3F:  // INDIRECT_BUFFER: a command buffer the D3D calls (draws the hooks never see)
    case 0x37: {
      ++counters_.indirect;
      static uint32_t logged = 0;
      if (logged < 40 && pkt_.size() >= 2) {
        ++logged;
        REXLOG_INFO("GRAW native state: INDIRECT_BUFFER {:08X} {} dwords at {:08X} (call {:08X})",
                    pkt_[0], pkt_[1] & 0xFFFFF, packet_address_, current_fn_);
      }
      break;
    }
    case 0x10:  // NOP
    case 0x3B:  // INVALIDATE_STATE
    case 0x3C:  // WAIT_REG_MEM
    case 0x3D:  // MEM_WRITE
    case 0x3E:  // REG_TO_MEM
    case 0x21:  // REG_RMW
    case 0x26:  // WAIT_FOR_IDLE
    case 0x44:  // COND_EXEC
    case 0x45:  // COND_WRITE
    case 0x46:  // EVENT_WRITE
    case 0x58:  // EVENT_WRITE_SHD
    case 0x59:  // EVENT_WRITE_CFL
    case 0x5A:  // EVENT_WRITE_EXT
    case 0x5B:  // EVENT_WRITE_ZPD
    case 0x50:  // SET_BIN_MASK
    case 0x51:  // SET_BIN_SELECT
    case 0x60:  // SET_BIN_MASK_LO
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x54:  // INTERRUPT
    case 0x23:  // VIZ_QUERY
    case 0x25:  // SET_STATE
    case 0x04:  // SET_SHADER_BASES
    case 0x64:  // XE_SWAP
      break;
    default:
      ++counters_.unknown_ops;
      break;
  }
}

void GuestStateTracker::AfterCall(const uint8_t* base, uint32_t device) {
  if (!device || device != device_) return;
  // Window scissor: sub_824CDD78 writes PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR straight into the
  // stream from the rectangle SetScissorRect (sub_824D03C0) keeps in the device (15-bit signed
  // x | y << 16); with predicated tiling the D3D writes one clamped copy per tile instead. The
  // untiled form (offset 0), as pgr4-native's tracker sets it.
  const uint32_t tl = Load32(base, device + dev::kScissorTL),
                 br = Load32(base, device + dev::kScissorBR);
  auto field = [](uint32_t v) { return uint32_t(int32_t(v << 17) >> 17); };
  Put(0x2080, 0);
  Put(0x2081, ((field(tl >> 16) << 16) & 0x3FFF0000u) | (field(tl) & 0x3FFFu));
  Put(0x2082, ((field(br >> 16) << 16) & 0x3FFF0000u) | (field(br) & 0x3FFFu));

  if (REXCVAR_GET(graw_native_stream_log)) {
    static uint64_t calls = 0;
    if (++calls % 200000 == 0) {
      const Counters& c = counters_;
      REXLOG_INFO(
          "GRAW native state: {} dwords, {} packets, {} draw packets, {} jumps, {} resyncs, {} "
          "torn, {} unknown opcodes, {} predicated writes, {} shader loads, {} immediate, {} "
          "indirect buffers, {} unhooked draws",
          c.dwords, c.packets, c.draws, c.jumps, c.resyncs, c.torn, c.unknown_ops,
          c.predicated_writes, c.shader_loads, c.immediate_loads, c.indirect, c.unhooked_draws);
    }
  }
}

bool GuestStateTracker::ReadDrawState(const uint8_t* base, const HookArgs& a, DrawState* s) {
  const uint32_t device = a.device;
  if (!device || device != device_) return false;
  // Field by field rather than from a DrawState{}: the render thread's queue entries are
  // value-initialized already, and this runs for every draw on the guest thread.
  s->regs = regs_;
  s->fn = a.fn;
  s->device = device;
  s->primitive = a.r[1] & 0x3F;
  s->indexed = false;
  s->index_address = 0;
  s->index_32bit = false;
  s->index_endian = 0;
  s->base_vertex = 0;
  s->inline_vertices = 0;
  // The call's first DRAW_INDX with an index buffer (source select 0, DMA).
  const bool dma = first_draw_valid_ && first_draw_op_ == 0x22 &&
                   ((first_draw_initiator_ >> 6) & 3) == 0 && first_draw_index_base_;
  auto index_from_packet = [&] {
    s->index_address = first_draw_index_base_;
    s->index_32bit = (first_draw_initiator_ >> 11) & 1;
    s->index_endian = first_draw_index_size_ >> 30;
  };
  switch (a.fn) {
    case HookFn::kDrawIndexedVertices: {  // (dev, prim, base vertex, start index, index count)
      s->base_vertex = a.r[2];
      s->count = a.r[4];
      s->indexed = true;
      if (dma) {
        index_from_packet();
      } else if (const uint32_t ib = Load32(base, device + dev::kIndexBuffer)) {
        // As sub_824C4E38 builds the packet: 16-bit indices swap 8-in-16, 32-bit 8-in-32.
        s->index_32bit = Load32(base, ib + kIndexBufferFormat) >> 31;
        s->index_endian = s->index_32bit ? 2 : 1;
        s->index_address = GpuAddress(Load32(base, ib + kIndexBufferAddress) +
                                      a.r[3] * (s->index_32bit ? 4 : 2));
      } else {
        return false;
      }
      break;
    }
    case HookFn::kDrawVertices:  // (dev, prim, start vertex, vertex count)
      s->base_vertex = a.r[2];
      s->count = a.r[3];
      break;
    case HookFn::kBeginVertices:   // (dev, prim, vertex count, stride)
    case HookFn::kDrawVerticesUP:  // (dev, prim, vertex count, data, stride)
      s->count = a.r[2];
      // Vertex fetch 95 (fetch constant 31, dwords 4-5): the inline vertices, which the call
      // wrote as a type-0 packet before its draw.
      s->inline_vertices = regs_[0x4800 + 31 * 6 + 4] & ~3u;
      break;
    case HookFn::kBeginIndexedVertices:  // (dev, prim, base vertex, vertices, index count, ...)
      s->base_vertex = a.r[2];
      s->count = a.r[4];
      s->indexed = true;
      s->inline_vertices = regs_[0x4800 + 31 * 6 + 4] & ~3u;
      if (!dma) return false;
      index_from_packet();
      break;
    default:
      return false;
  }
  s->vs = vs_;
  s->ps = ps_;
  return true;
}

}  // namespace graw::native
