// GRAW native renderer: guest physical memory mirrored in the GPU's shared memory
// (docs/VULKAN_RENDERER.md, M3b). Moved out of draw.cpp unchanged, so the D3D12 and Vulkan
// backends share it.
//
// The translated vertex shaders read guest memory from "shared memory", a GPU buffer mirroring
// guest physical memory. RequestRange copies the ranges a draw reads that aren't current there,
// and write-watches the pages it copied: a guest write faults, clears the page's valid bit, and
// the next request copies it again. Pages the game rewrites every few frames go "hot": hashed
// on each request instead of watched. Watches are armed on a thread of their own. Copy segments
// let a batch's copies go ahead of its draws. --graw_native_coherency_check and
// --graw_native_watch_trace check and record all of it.
//
// The GPU API side is the Sink: it gives the shared buffer memory under a range (MapShared) and
// upload memory to copy into (AllocateUpload); the copies themselves (VertexCopy) are recorded
// by the backend.
#pragma once

#include "backend.h"  // VertexCopy, PrepareDiag

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/system/xmemory.h>
#undef DrawState  // winuser.h (via rex/platform.h) maps it to DrawStateA/W

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace graw::native {

class GuestMemoryMirror {
 public:
  static constexpr uint32_t kSharedMemorySize = 512u << 20;  // guest physical address space
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageCount = kSharedMemorySize >> kPageShift;
  // The memory unwatches at most the 4 MB-aligned block around a write fault
  // (PhysicalHeap::TriggerCallbacks), so the invalidation callback looks no further.
  static constexpr uint32_t kUnwatchWindow = 4u << 20;
  // The Xenos plugin's invalidation callbacks let the memory unwatch at most the 256 KB-aligned
  // block around a fault: the pages of one 64-bit word of the valid bits.
  static constexpr uint32_t kPluginUnwatchBlock = 256u << 10;
  static_assert(kPluginUnwatchBlock == 64u << kPageShift);

  // Upload memory for one copy: `cpu` to write, `buffer` + `offset` for the backend's copy
  // command (VertexCopy::src, src_offset).
  struct Upload {
    void* cpu = nullptr;
    void* buffer = nullptr;
    uint64_t offset = 0;
  };
  class Sink {
   public:
    virtual ~Sink() = default;
    // Gives the shared buffer memory under [start, start + length) if it is reserved. Any
    // thread; queued ahead of the copies the caller records afterwards.
    virtual bool MapShared(uint32_t start, uint32_t length) = 0;
    // `size` bytes of upload memory, free once the frame or Prepare tag `tag` is done.
    virtual bool AllocateUpload(uint32_t size, uint64_t tag, Upload* out) = 0;
  };

  GuestMemoryMirror() = default;
  ~GuestMemoryMirror() { Shutdown(); }
  GuestMemoryMirror(const GuestMemoryMirror&) = delete;
  GuestMemoryMirror& operator=(const GuestMemoryMirror&) = delete;

  // The page state, the write watch callback and the watch thread. `trace_path`:
  // --graw_native_watch_trace (empty: off). False on failure (logged).
  bool Initialize(rex::memory::Memory* memory, Sink* sink, const std::string& trace_path,
                  bool coherency_check);
  // Where RequestRange counts the bytes and copies it makes, and its draw diag (the guest
  // thread's with the render thread, the recording thread's otherwise).
  void SetCounters(uint64_t* upload_bytes, uint64_t* uploads, PrepareDiag* diag) {
    upload_bytes_ = upload_bytes;
    uploads_ = uploads;
    diag_ = diag;
  }
  // Stops the watch thread, unregisters the callback, closes the trace. Idempotent.
  void Shutdown();

  rex::memory::Memory* memory = nullptr;
  const uint8_t* membase = nullptr;  // guest physical memory
  std::unique_ptr<std::atomic<uint64_t>[]> valid;  // one bit per page: shared memory is current
  void* invalidation_handle = nullptr;
  // Fence (or Prepare's tag) RequestRange tags vertex upload memory with.
  uint64_t upload_fence = 0;
  // kDebugUploadEveryDraw (draw-debug.txt): every range copied whole, unwatched.
  std::atomic<bool> upload_every_draw{false};

  // Copy segments (DrawRenderer::Prepare): what the current segment's draws read. Watched
  // ranges are copied in whole pages, so their reads are kept one bit per page, valid in the
  // 64-page words whose epoch is the segment's. Unwatched ranges (inline vertices, packed
  // back to back in the guest's command buffer and often several to a page) are copied to the
  // byte, so their reads are kept as byte intervals.
  struct Read {
    uint32_t begin, end;
    bool exact;
  };
  // This many exact intervals end the segment: the overlap test scans them all.
  static constexpr size_t kSegmentExactLimit = 64;
  uint32_t segment_epoch = 1;
  std::unique_ptr<uint32_t[]> segment_word_epoch;
  std::unique_ptr<uint64_t[]> segment_pages;
  std::vector<std::pair<uint32_t, uint32_t>> segment_exact;
  // The draw being prepared: its ranges, and whether one of its copies overwrites memory the
  // segment reads.
  std::vector<Read> draw_reads;
  bool preparing = false, segment_conflict = false;
  static uint64_t PageMask(uint32_t w, uint32_t first, uint32_t last) {
    uint64_t m = ~0ull;
    if (w == first >> 6) m &= ~0ull << (first & 63);
    if (w == last >> 6) m &= ~0ull >> (63 - (last & 63));
    return m;
  }
  void NewSegment() {
    ++segment_epoch;
    segment_exact.clear();
    if (coherency) coherency->segment_reads.clear();
  }
  bool SegmentReads(uint32_t begin, uint32_t end) const {
    for (const auto& [b, e] : segment_exact)
      if (b < end && begin < e) return true;
    const uint32_t first = begin >> kPageShift, last = (end - 1) >> kPageShift;
    for (uint32_t w = first >> 6; w <= last >> 6; ++w)
      if (segment_word_epoch[w] == segment_epoch && (segment_pages[w] & PageMask(w, first, last)))
        return true;
    return false;
  }
  void SegmentAdd(const Read& read) {
    if (read.exact) {
      // Successive inline blocks are separated by the draw packets and constant writes around
      // them: the merged interval covers those too, which no draw copies.
      constexpr uint32_t kMergeGap = 16 << 10;
      if (!segment_exact.empty() && segment_exact.back().second + kMergeGap >= read.begin &&
          read.end + kMergeGap >= segment_exact.back().first) {
        auto& last = segment_exact.back();
        last = {std::min(last.first, read.begin), std::max(last.second, read.end)};
      } else {
        segment_exact.emplace_back(read.begin, read.end);
      }
      return;
    }
    const uint32_t first = read.begin >> kPageShift, last = (read.end - 1) >> kPageShift;
    for (uint32_t w = first >> 6; w <= last >> 6; ++w) {
      if (segment_word_epoch[w] != segment_epoch) {
        segment_word_epoch[w] = segment_epoch;
        segment_pages[w] = 0;
      }
      segment_pages[w] |= PageMask(w, first, last);
    }
  }

  // Whether [start, start + length) lies in guest physical memory (logged once if not).
  bool InSharedMemory(uint32_t start, uint32_t length);
  // The vertex ranges a draw's vertex shader fetches (its vertex fetch constants) and its index
  // range, through RequestRange; copies go to the end of `out`. Triangle fan indices are only
  // checked: the backend converts them from guest memory itself.
  bool RequestVertices(const DrawState& s, const rex::graphics::Shader& vs,
                       std::vector<VertexCopy>& out);
  bool RequestRange(uint32_t start, uint32_t length, bool watch, std::vector<VertexCopy>& out);
  // Sets (or clears) the bits of pages [first, end) in a page bitmap (valid, pending). acq_rel
  // orders what each thread wrote before it ahead of the other's reads after it: RequestRange's
  // copy_hash stores before a valid bit it clears, the watch thread's valid bits before the
  // pending bits it clears.
  static void SetPageBits(std::atomic<uint64_t>* words, uint32_t first, uint32_t end, bool set) {
    for (uint32_t q = first; q < end;) {
      const uint32_t word_end = std::min(end, (q | 63) + 1);
      const uint32_t n = word_end - q;
      const uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << (q & 63);
      if (set)
        words[q >> 6].fetch_or(mask, std::memory_order_acq_rel);
      else
        words[q >> 6].fetch_and(~mask, std::memory_order_acq_rel);
      q = word_end;
    }
  }
  // A watched page may be unwatched once its valid bit is clear: the next request copies it
  // again and re-arms the watch. Unless the range must be exact, the pages around it that hold
  // no valid copy are offered for unwatching too (InvalidRunAround). Valid neighbours stay
  // watched: unwatching them too (one fault per kPluginUnwatchBlock instead of per 4 KB page)
  // tripled the bytes re-copied per frame.
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start,
                                                         uint32_t length, bool exact) {
    auto* self = static_cast<GuestMemoryMirror*>(context);
    self->watch_faults.fetch_add(1, std::memory_order_relaxed);
    if (start >= kSharedMemorySize || !length) return {start, length};
    const uint32_t end = std::min(kSharedMemorySize, start + length);
    const uint32_t first = start >> kPageShift, last = (end - 1) >> kPageShift;
    const uint32_t frame = self->watch_frame.load(std::memory_order_relaxed);
    for (uint32_t p = first; p <= last; ++p) {
      const uint64_t bit = 1ull << (p & 63);
      std::atomic<uint64_t>& valid_word = self->valid[p >> 6];
      const bool was_valid = valid_word.fetch_and(~bit, std::memory_order_relaxed) & bit;
      if (self->trace.file)
        self->trace.Add(was_valid ? WatchTrace::kFaultValid : WatchTrace::kFaultOther, p, 1, 0);
      if (!was_valid) continue;
      std::atomic<uint8_t>& quick = self->quick_rewrites[p];
      uint8_t n = quick.load(std::memory_order_relaxed);
      n = frame - self->armed_frame[p].load(std::memory_order_relaxed) > kHotRewriteFrames
              ? 0
              : std::min<uint8_t>(n + 1, 2);
      quick.store(n, std::memory_order_relaxed);
      if (n == 2) self->hot[p >> 6].fetch_or(bit, std::memory_order_relaxed);
      // The page after one written since the previous frame: a buffer being filled in order.
      // Its valid pages ahead, to the end of the kPluginUnwatchBlock (the valid word), are
      // invalidated with it, so the fill faults about once per block instead of once per page. In
      // the race this cut the faults on valid pages 4x, and under 1 % of the pages invalidated
      // ahead went unwritten.
      const uint32_t previous = p ? self->write_frame[p - 1].load(std::memory_order_relaxed) : 0;
      self->write_frame[p].store(frame + 1, std::memory_order_relaxed);
      const uint32_t shift = (p & 63) + 1;
      if (!previous || frame + 1 - previous > 1 || shift == 64) continue;
      const uint32_t ahead =
          uint32_t(std::countr_one(valid_word.load(std::memory_order_relaxed) >> shift));
      if (!ahead) continue;
      valid_word.fetch_and(~(((1ull << ahead) - 1) << shift), std::memory_order_relaxed);
      // Counted as written, so a fault on the next block's first page continues the sequence.
      for (uint32_t q = p + 1; q <= p + ahead; ++q)
        self->write_frame[q].store(frame + 1, std::memory_order_relaxed);
      if (self->trace.file) self->trace.Add(WatchTrace::kAhead, p + 1, ahead, 0);
    }
    return exact ? std::pair{start, length} : self->InvalidRunAround(first, last);
  }
  // Pages [first, last] widened to the nearest valid page on each side within the
  // kUnwatchWindow block, as a byte range. Pages without a valid copy lose nothing by being
  // unwatched (the next request copies and re-arms them). The memory unwatches only the
  // intersection of every callback's range, so with exact ranges a texture or buffer being
  // filled faulted once per 4 KB page although no consumer still watched its other pages (the
  // race's write faults came in runs of about 10 adjacent pages). A page made valid meanwhile is
  // re-armed after the unwatch: both hold the global lock.
  std::pair<uint32_t, uint32_t> InvalidRunAround(uint32_t first, uint32_t last) const {
    constexpr uint32_t kWindowPages = kUnwatchWindow >> kPageShift;
    const uint32_t window_first = first & ~(kWindowPages - 1);
    const uint32_t window_last = last | (kWindowPages - 1);
    uint32_t w = first >> 6;
    uint64_t bits = valid[w].load(std::memory_order_relaxed) & ((1ull << (first & 63)) - 1);
    while (!bits && w > window_first >> 6) bits = valid[--w].load(std::memory_order_relaxed);
    const uint32_t lo = bits ? (w << 6) + 64 - uint32_t(std::countl_zero(bits)) : window_first;
    w = last >> 6;
    bits = valid[w].load(std::memory_order_relaxed) & ~((2ull << (last & 63)) - 1);
    while (!bits && w < window_last >> 6) bits = valid[++w].load(std::memory_order_relaxed);
    const uint32_t end = bits ? (w << 6) + uint32_t(std::countr_zero(bits)) : window_last + 1;
    return {lo << kPageShift, (end - lo) << kPageShift};
  }
  // Dynamic vertex data. A valid page written again within kHotRewriteFrames of being armed,
  // twice in a row (once is more often a buffer being reused), becomes hot: data the game
  // rewrites every few frames, where each write fault and re-arm (a protection change, and a
  // TLB shootdown across the process's threads: tens of µs each under Wine) costs far more than
  // hashing the page. Hot pages are not watched: each request hashes them (about 0.1 µs) and
  // copies those whose content changed since the last copy. A hot page hashed kHotMaxHashes
  // times without a change (a large buffer that many draws read whole) is watched again, which
  // then costs less. Frames are counted by watch_frame on the thread that calls RequestRange.
  static constexpr uint32_t kHotRewriteFrames = 16;
  static constexpr uint16_t kHotMaxHashes = 64;
  std::unique_ptr<std::atomic<uint64_t>[]> hot;            // one bit per page
  std::unique_ptr<std::atomic<uint32_t>[]> armed_frame;    // watch_frame of the last arm
  std::unique_ptr<std::atomic<uint8_t>[]> quick_rewrites;  // quick rewrites in a row (to 2)
  // PageHash of the page taken just before its last whole-page copy, 0: none or not copied.
  std::unique_ptr<std::atomic<uint64_t>[]> copy_hash;
  std::unique_ptr<uint16_t[]> hot_hashes;  // hashes since the content last changed
  uint64_t PageHash(uint32_t page) const {
    return XXH3_64bits(membase + (size_t(page) << kPageShift), 1u << kPageShift) | 1;
  }
  // watch_frame + 1 of the last write, 0: none.
  std::unique_ptr<std::atomic<uint32_t>[]> write_frame;
  std::atomic<uint32_t> watch_frame{0};

  // Watches are armed on a thread of their own (WatchThread): a protection change takes about
  // 20 µs under Wine (mprotect and its TLB shootdown), and geometry streamed in arms hundreds in
  // a frame (td-175: 100-420 in about 25 frames of the race, 1.6-6.5 ms of the game thread).
  // A run RequestRange copies is hashed just before the copy and stays pending until then,
  // hashed by every request like a hot page. The watch thread marks it valid, arms it, and keeps
  // each page valid that still hashes as copied: a write before the arm shows in the hash, and
  // one after it faults and clears the valid bit, which the watch thread does not set again.
  // Runs queued back to back are armed with one protection change.
  std::unique_ptr<std::atomic<uint64_t>[]> pending;  // one bit per page
  struct ArmRun {
    uint32_t first, end;  // pages
    uint32_t request;     // WatchTrace's request number
  };
  std::mutex arm_mutex;
  std::condition_variable arm_cv;
  std::vector<ArmRun> arm_queue;
  bool arm_idle = false, arm_stopping = false;
  std::thread arm_thread;
  void QueueArm(uint32_t first, uint32_t end, uint32_t request);
  void WatchThread();

  // Pages hashed (hot, pending, before a copy) and copied as they changed since EndFrame.
  std::atomic<uint64_t> hash_pages{0}, hash_copies{0};
  // Write-watch faults (InvalidationThunk calls) since the last EndFrame; any guest thread.
  std::atomic<uint64_t> watch_faults{0};
  // The watch thread's protection changes, their pages, the pages found written since their
  // copy, and its time, since EndFrame.
  std::atomic<uint64_t> watch_arms{0}, watch_arm_pages{0}, watch_arm_changed{0}, watch_arm_ns{0};

  // --graw_native_watch_trace. Record: type (2 bits) << 62 | frame (22) << 40 | page (17) << 23
  // | pages (9, saturated) << 14 | request (14): arms of one RequestRange call share the request
  // number. Faults come from any guest thread, hence the mutex.
  struct WatchTrace {
    enum Type : uint64_t { kArm, kFaultValid, kFaultOther, kAhead };
    std::mutex mutex;
    std::vector<uint64_t> records;
    std::FILE* file = nullptr;
    uint32_t frame = 0;
    std::atomic<uint32_t> request{0};
    void Add(Type type, uint32_t page, uint32_t pages, uint32_t req) {
      std::lock_guard<std::mutex> lock(mutex);
      records.push_back(uint64_t(type) << 62 | uint64_t(frame & 0x3FFFFF) << 40 |
                        uint64_t(page & 0x1FFFF) << 23 | uint64_t(std::min(pages, 511u)) << 14 |
                        (req & 0x3FFF));
    }
    void Flush() {
      std::lock_guard<std::mutex> lock(mutex);
      if (file && !records.empty()) std::fwrite(records.data(), 8, records.size(), file);
      records.clear();
    }
  } trace;

  // --graw_native_coherency_check; RequestRange's thread only.
  struct Coherency {
    // What shared memory holds once the copies recorded so far are done, in Prepare order.
    std::unique_ptr<uint8_t[]> shadow =
        std::make_unique_for_overwrite<uint8_t[]>(kSharedMemorySize);
    // watch_frame + 1 when a page last matched guest memory, per request and at the Swap.
    std::unique_ptr<uint32_t[]> checked = std::make_unique<uint32_t[]>(kPageCount);
    std::unique_ptr<uint32_t[]> swap_checked = std::make_unique<uint32_t[]>(kPageCount);
    struct Range {
      uint32_t start, length;
      uint8_t slot;
    };
    std::vector<Range> ranges;  // watched ranges read since the last Swap
    // Every byte range the current copy segment's draws read (SegmentReads is conservative).
    std::vector<std::pair<uint32_t, uint32_t>> segment_reads;
    uint8_t slot = 0xFF;           // vertex fetch constant being requested, 0xFF: indices
    uint64_t stale = 0, late = 0;  // pages, since the last Swap
    std::vector<std::pair<uint32_t, uint32_t>> frame_copies;  // (start, size) since the Swap
  };
  std::unique_ptr<Coherency> coherency;
  void CheckRequest(uint32_t start, uint32_t length);
  void CheckSegmentCopy(uint32_t from, uint32_t size);
  void CheckLateWrites();

 private:
  Sink* sink_ = nullptr;
  uint64_t* upload_bytes_ = nullptr;
  uint64_t* uploads_ = nullptr;
  PrepareDiag* diag_ = nullptr;
};

}  // namespace graw::native
