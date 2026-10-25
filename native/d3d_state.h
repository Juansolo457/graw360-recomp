// GRAW native renderer: Xenos register state per draw, decoded from the guest D3D's push buffer.
// Structs (ShaderRef, DrawState, RegLog) from pgr4-native 8ea20d5 (MIT; see native/README.md);
// the tracker is GRAW's own.
//
// pgr4-native reads each draw's GPU state from the XDK D3D device's register shadow, which needs
// the exact internals of PGR4's XDK. GRAW's D3D is from an older XDK with another layout
// (graw_d3d.h), so this tracker follows what the D3D writes into its push buffer instead, as
// pgr4-native's trace tap does (it is the reference that proved pgr4-native's shadow tracker
// right): the D3D completes its packets before it calls another function or returns, so at
// the entry and exit of every hooked call (graw_d3d_hooks.cpp) the dwords between the last
// decoded one and the device's push cursor (device+0) are whole packets. Type-0/1 register
// writes, SET_CONSTANT, LOAD_ALU_CONSTANT and the shader loads (IM_LOAD, IM_LOAD_IMMEDIATE)
// update the register file and the bound shaders; the draw packets give the index buffer.
//
// The cursor moves to another place by assignment in a few functions only (MakeSpace and the
// segment functions, graw_d3d.h fn::), which the hooks report with Jump(): what was written up to
// the move is decoded first, and nothing between the old and the new place is. A window that
// goes backwards or is implausibly long is skipped the same way and counted as a resync.
//
// Predicated tiling: the D3D writes the window offset and scissor once per tile (predicated
// packets, so the last tile's copy would win); after every call they are set from the device's
// untiled scissor rectangle instead, as pgr4-native does. The tile replays themselves happen on
// the GPU side later and are never decoded: each draw is seen once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "graw_d3d.h"
#include "hook_args.h"

namespace graw::native {

constexpr uint32_t kGuestRegisterCount = 0x5000;

// A shader as the GPU loads it: IM_LOAD's physical ucode address and size in dwords, or for
// IM_LOAD_IMMEDIATE (the ucode inside the push buffer) the push-buffer address with `immediate`
// pointing at the tracker's copy (big-endian dwords, valid until the next immediate load of that
// type) and `immediate_seq` counting those loads (a cache key: push-buffer addresses get reused).
// `binding` hashes the ucode at its load: the D3D can patch a shader's vertex fetches in place
// for another declaration and load it again at the same address and size.
struct ShaderRef {
  uint32_t object = 0;  // unused (pgr4-native: the guest D3D shader object)
  uint32_t ucode = 0;   // physical address of the ucode, big-endian dwords
  uint32_t dwords = 0;
  const uint32_t* immediate = nullptr;
  uint32_t immediate_seq = 0;
  uint64_t binding = 0;
};

struct DrawState {
  // The register file as the GPU sees it at this draw, host byte order, kGuestRegisterCount
  // entries (0x4000 VS constants, 0x4400 PS constants, 0x4800 fetch constants, 0x4900
  // bool/loop, 0x2000.. render/raster state). Valid until the next hooked D3D call.
  const uint32_t* regs = nullptr;
  // Fetch constant slot 0..31 (6 dwords; texture fetch) or vertex fetch 0..95 (2 dwords).
  const uint32_t* fetch(uint32_t slot) const { return regs + 0x4800 + slot * 6; }

  HookFn fn{};
  uint32_t device = 0;
  uint32_t primitive = 0;  // xenos::PrimitiveType (VGT_DRAW_INITIATOR bits 0-5)
  uint32_t count = 0;      // indices (indexed) or vertices
  bool indexed = false;
  // Index buffer (indexed draws): physical address of the first index of this draw.
  uint32_t index_address = 0;
  bool index_32bit = false;
  uint32_t index_endian = 0;  // xenos::Endian of the indices
  uint32_t base_vertex = 0;   // = regs[0x2102] VGT_INDX_OFFSET
  // BeginVertices: the inline vertices (vertex fetch 93-95, fetch constant 31).
  uint32_t inline_vertices = 0;  // physical, 0 if none

  ShaderRef vs, ps;
};

// One register value, for the render thread's copy of the register file (renderer.h).
struct RegWrite {
  uint32_t reg, value;
};

// Register writes in order, appended through a cursor: Reserve(n) makes room for n more and
// returns where they go, Commit(end) keeps the ones written before `end`.
class RegLog {
 public:
  size_t size() const { return size_; }
  const RegWrite& operator[](size_t i) const { return data_[i]; }
  void clear() { size_ = 0; }
  RegWrite* Reserve(size_t n) {
    if (capacity_ - size_ < n) [[unlikely]]
      Grow(size_ + n);
    return data_.get() + size_;
  }
  void Commit(const RegWrite* end) { size_ = size_t(end - data_.get()); }
  void Append(RegWrite w) {
    *Reserve(1) = w;
    ++size_;
  }

 private:
  void Grow(size_t capacity);
  std::unique_ptr<RegWrite[]> data_;
  size_t size_ = 0, capacity_ = 0;
};

// Sets the dirty bits of every state block the draw flush sends and the shader reload bits, so
// the next draw writes the whole shadow and both shaders into the stream (the renderer switch
// hands the game back to the Xenos plugin, which skipped the shader loads meanwhile).
void MarkAllDirty(uint8_t* base, uint32_t device);

class GuestStateTracker {
 public:
  // Decodes what the D3D wrote since the last call, up to the device's push cursor. At the entry
  // and the exit of every hooked call. `fn` names the call, for the log.
  void Sync(uint8_t* base, uint32_t device, uint32_t fn);
  // After a call that moved the push cursor by assignment (graw_d3d.h fn::kMakeSpace and the
  // segment functions): continue from the new cursor; what the old and new place hold between
  // the last decoded dword and the new cursor was not written in this pass.
  void Jump(const uint8_t* base, uint32_t device);
  // At the exit of every hooked call, after Sync: the registers the D3D writes per tile, set
  // from the device (window offset and scissor).
  void AfterCall(const uint8_t* base, uint32_t device);
  // The draw `args` describes (the outermost draw call, after its exit's Sync and AfterCall).
  bool ReadDrawState(const uint8_t* base, const HookArgs& args, DrawState* out);
  // Marks the start of a draw call's window: the first draw packet decoded after it is the
  // call's (its index buffer).
  void BeginDrawCall() { first_draw_valid_ = false; }

  const uint32_t* regs() const { return regs_; }
  uint32_t device() const { return device_; }
  const ShaderRef& vs() const { return vs_; }
  const ShaderRef& ps() const { return ps_; }
  // From now on appends every register write that changes a value to `*sink`, in order (null:
  // none). The first sink also gets every non-zero register, as the copy it feeds starts zeroed
  // like regs_. The render thread replays these into its own register file (renderer.cpp).
  void RecordInto(RegLog* sink);
  // Forgets the device after calls the tracker did not see: the next call attaches again and
  // starts from the device's register shadow.
  void Resync() { device_ = 0; }

  struct Counters {
    uint64_t dwords = 0, packets = 0, draws = 0, jumps = 0, resyncs = 0, torn = 0;
    uint64_t unknown_ops = 0, predicated_writes = 0, shader_loads = 0, immediate_loads = 0;
  };
  const Counters& counters() const { return counters_; }

 private:
  void Attach(uint8_t* base, uint32_t device);
  void Put(uint32_t reg, uint32_t value) {
    if (reg >= kGuestRegisterCount || regs_[reg] == value) return;
    regs_[reg] = value;
    if (sink_) sink_->Append({reg, value});
  }
  // `count` registers from guest memory at host pointer `p` (big-endian dwords).
  void PutFromGuest(uint32_t reg, const uint8_t* p, uint32_t count);
  void Feed(const uint8_t* base, uint32_t address, uint32_t dw);
  void HandlePacket(const uint8_t* base);
  void LoadShader(uint32_t type, uint32_t address, uint32_t dwords);
  void LoadShaderImmediate(uint32_t type, uint32_t address, const uint32_t* ucode_be,
                           uint32_t dwords);

  uint32_t device_ = 0;
  uint32_t cursor_ = 0;  // the last decoded dword (guest address)
  uint32_t last_sync_fn_ = 0;      // the hooked call of the last Sync (torn packet log)
  uint32_t last_sync_thread_ = 0;  // and its thread
  // Decoder.
  bool in_packet_ = false;
  uint32_t header_ = 0, need_ = 0, packet_address_ = 0;
  std::vector<uint32_t> pkt_;  // host order
  // The call's first draw packet: DRAW_INDX's index base and size dwords.
  bool first_draw_valid_ = false;
  uint32_t first_draw_op_ = 0, first_draw_initiator_ = 0, first_draw_index_base_ = 0,
           first_draw_index_size_ = 0;

  ShaderRef vs_, ps_;
  std::vector<uint32_t> vs_imm_, ps_imm_;  // big-endian ucode of the last immediate loads
  uint32_t imm_seq_ = 0;
  uint32_t regs_[kGuestRegisterCount] = {};
  RegLog* sink_ = nullptr;  // RecordInto
  bool synced_ = false;     // a sink got the whole register file
  Counters counters_;
};

GuestStateTracker& GuestState();

}  // namespace graw::native
