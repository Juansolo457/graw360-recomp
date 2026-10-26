// GRAW native renderer: guest physical memory mirrored in the GPU's shared memory
// (guest_memory_mirror.h). Moved out of draw.cpp (DrawRenderer::Impl) unchanged.
#include "guest_memory_mirror.h"

#include "clock.h"
#include "log.h"

#include <rex/logging.h>

#include <fmt/format.h>

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#undef DrawState
#endif

namespace graw::native {

bool GuestMemoryMirror::Initialize(rex::memory::Memory* memory_in, Sink* sink,
                                   const std::string& trace_path, bool coherency_check) {
  memory = memory_in;
  membase = memory->physical_membase();
  sink_ = sink;
  // Zeroed: make_unique value-initializes, and std::atomic's default constructor value-initializes
  // its value (C++20).
  valid = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
  hot = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
  armed_frame = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
  write_frame = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
  quick_rewrites = std::make_unique<std::atomic<uint8_t>[]>(kPageCount);
  copy_hash = std::make_unique<std::atomic<uint64_t>[]>(kPageCount);
  hot_hashes = std::make_unique<uint16_t[]>(kPageCount);
  pending = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
  segment_word_epoch = std::make_unique<uint32_t[]>(kPageCount / 64);
  segment_pages = std::make_unique<uint64_t[]>(kPageCount / 64);
  if (!trace_path.empty()) trace.file = std::fopen(trace_path.c_str(), "wb");
  if (coherency_check) {
    coherency = std::make_unique<Coherency>();
    REXLOG_INFO("GRAW native draw: coherency check on");
  }
  invalidation_handle = memory->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  arm_thread = std::thread([this] { WatchThread(); });
#if defined(_WIN32)
  SetThreadDescription(arm_thread.native_handle(), L"GRAW native watches");
#endif
  return true;
}

void GuestMemoryMirror::Shutdown() {
  if (arm_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(arm_mutex);
      arm_stopping = true;
    }
    arm_cv.notify_one();
    arm_thread.join();
  }
  if (invalidation_handle && memory) {
    memory->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle);
    invalidation_handle = nullptr;
  }
  if (trace.file) {
    trace.Flush();
    std::fclose(trace.file);
    trace.file = nullptr;
  }
}

bool GuestMemoryMirror::InSharedMemory(uint32_t start, uint32_t length) {
  if (start < kSharedMemorySize && kSharedMemorySize - start >= length) return true;
  GRAW_WARN_ONCE("GRAW native draw: range {:08X}+{:X} outside physical memory", start, length);
  return false;
}

bool GuestMemoryMirror::RequestRange(uint32_t start, uint32_t length, bool watch,
                                      std::vector<VertexCopy>& out) {
  if (!length) return true;
  if (!InSharedMemory(start, length)) return false;
  const bool exact = !watch || upload_every_draw.load(std::memory_order_relaxed);
  if (preparing) draw_reads.emplace_back(start, start + length, exact);
  const uint64_t fence = upload_fence;
  PrepareDiag& pd = *diag_;
  auto copy = [&](uint32_t from, uint32_t size) {
    if (preparing && !segment_conflict) segment_conflict = SegmentReads(from, from + size);
    if (coherency && preparing) CheckSegmentCopy(from, size);
    if (!sink_->MapShared(from, size)) return false;
    Upload a;
    if (!sink_->AllocateUpload(size, fence, &a)) return false;
    if (coherency) {
      // The upload is made from the shadow, so the two hold the same bytes.
      std::memcpy(coherency->shadow.get() + from, membase + from, size);
      std::memcpy(a.cpu, coherency->shadow.get() + from, size);
      coherency->frame_copies.emplace_back(from, size);
    } else {
      std::memcpy(a.cpu, membase + from, size);
    }
    out.emplace_back(a.buffer, a.offset, from, size);
    *upload_bytes_ += size;
    ++*uploads_;
    return true;
  };
  if (exact) {
    ++pd.unwatched;
    return copy(start, length);
  }
  ++pd.ranges;
  const uint32_t first = start >> kPageShift, last = (start + length - 1) >> kPageShift;
  pd.range_pages += last - first + 1;
  const uint32_t trace_request =
      trace.file ? trace.request.fetch_add(1, std::memory_order_relaxed) : 0;
  const uint32_t frame = watch_frame.load(std::memory_order_relaxed);
  // Hashed pages whose content changed, copied in runs of adjacent pages.
  uint32_t changed_first = 0, changed_end = 0;
  auto copy_changed = [&] {
    if (changed_first == changed_end ||
        copy(changed_first << kPageShift, (changed_end - changed_first) << kPageShift))
      return true;
    // Hashed as changed but not copied: the next request copies them. The watch thread may
    // have found a pending one unchanged meanwhile, hence the valid bits.
    for (uint32_t q = changed_first; q < changed_end; ++q)
      copy_hash[q].store(0, std::memory_order_relaxed);
    SetPageBits(valid.get(), changed_first, changed_end, false);
    return false;
  };
  uint64_t hashed = 0, changed = 0;
  uint32_t p = first;
  while (p <= last) {
    // Skip valid pages that are neither hot nor pending a 64-page word at a time (whole vertex
    // buffers are requested per draw).
    const uint32_t w = p >> 6;
    // Pending before valid: a pending bit seen clear makes the valid bit the watch thread left
    // visible.
    const uint64_t pending_bits = pending[w].load(std::memory_order_acquire);
    const uint64_t hot_bits = hot[w].load(std::memory_order_relaxed);
    uint64_t work =
        (~valid[w].load(std::memory_order_relaxed) | hot_bits | pending_bits) & (~0ull << (p & 63));
    if (w == last >> 6) work &= ~0ull >> (63 - (last & 63));
    if (!work) {
      p = (w + 1) << 6;
      continue;
    }
    p = (w << 6) + uint32_t(std::countr_zero(work));
    const uint64_t bit = 1ull << (p & 63);
    if ((hot_bits | pending_bits) & bit) {
      ++hashed;
      const uint64_t hash = PageHash(p);
      if (hash != copy_hash[p].load(std::memory_order_relaxed)) {
        copy_hash[p].store(hash, std::memory_order_relaxed);
        hot_hashes[p] = 0;
        ++changed;
        if (changed_end != p) {
          if (!copy_changed()) return false;
          changed_first = p;
        }
        changed_end = p + 1;
        ++p;
        continue;
      }
      if ((pending_bits & bit) || ++hot_hashes[p] < kHotMaxHashes) {
        ++p;
        continue;
      }
      // Hot, but read far more often than written: watched again. Its copy is current.
      hot[w].fetch_and(~bit, std::memory_order_relaxed);
      quick_rewrites[p].store(0, std::memory_order_relaxed);
      armed_frame[p].store(frame, std::memory_order_relaxed);
      pending[w].fetch_or(bit, std::memory_order_relaxed);
      QueueArm(p, p + 1, trace_request);
      ++p;
      continue;
    }
    uint32_t run_end = p + 1;
    while (run_end <= last) {
      const uint32_t rw = run_end >> 6;
      if ((valid[rw].load(std::memory_order_relaxed) | hot[rw].load(std::memory_order_relaxed) |
           pending[rw].load(std::memory_order_relaxed)) &
          (1ull << (run_end & 63)))
        break;
      ++run_end;
    }
    // Hashed before the copy, so a write between the two shows as a change.
    for (uint32_t q = p; q < run_end; ++q)
      copy_hash[q].store(PageHash(q), std::memory_order_relaxed);
    hashed += run_end - p;
    ++pd.range_runs;
    // Not copied: neither valid nor pending, so the next request copies them.
    if (!copy(p << kPageShift, (run_end - p) << kPageShift)) return false;
    for (uint32_t q = p; q < run_end; ++q) armed_frame[q].store(frame, std::memory_order_relaxed);
    SetPageBits(pending.get(), p, run_end, true);
    QueueArm(p, run_end, trace_request);
    p = run_end;
  }
  if (hashed) {
    hash_pages.fetch_add(hashed, std::memory_order_relaxed);
    hash_copies.fetch_add(changed, std::memory_order_relaxed);
  }
  if (!copy_changed()) return false;
  if (coherency) CheckRequest(start, length);
  return true;
}

void GuestMemoryMirror::QueueArm(uint32_t first, uint32_t end, uint32_t request) {
  bool wake;
  {
    std::lock_guard<std::mutex> lock(arm_mutex);
    arm_queue.push_back({first, end, request});
    wake = std::exchange(arm_idle, false);
  }
  if (wake) arm_cv.notify_one();
}

void GuestMemoryMirror::WatchThread() {
  std::vector<ArmRun> runs;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(arm_mutex);
      while (arm_queue.empty() && !arm_stopping) {
        arm_idle = true;
        arm_cv.wait(lock);
      }
      if (arm_queue.empty()) return;
      runs.swap(arm_queue);
    }
    const uint64_t t0 = NowNs();
    std::sort(runs.begin(), runs.end(),
              [](const ArmRun& a, const ArmRun& b) { return a.first < b.first; });
    uint64_t arms = 0, pages = 0, changed = 0;
    for (size_t i = 0; i < runs.size();) {
      const uint32_t first = runs[i].first;
      uint32_t end = runs[i].end;
      size_t j = i + 1;
      while (j < runs.size() && runs[j].first == end) end = runs[j++].end;
      // Valid before the arm: a write after it faults and clears the bit, which stays clear.
      SetPageBits(valid.get(), first, end, true);
      memory->EnablePhysicalMemoryAccessCallbacks(first << kPageShift, (end - first) << kPageShift,
                                                  true, false);
      for (uint32_t q = first; q < end; ++q) {
        if (PageHash(q) == copy_hash[q].load(std::memory_order_relaxed)) continue;
        // Written between its copy and the arm: the next request copies it again.
        SetPageBits(valid.get(), q, q + 1, false);
        ++changed;
      }
      SetPageBits(pending.get(), first, end, false);
      if (trace.file) trace.Add(WatchTrace::kArm, first, end - first, runs[i].request);
      ++arms;
      pages += end - first;
      i = j;
    }
    runs.clear();
    watch_arms.fetch_add(arms, std::memory_order_relaxed);
    watch_arm_pages.fetch_add(pages, std::memory_order_relaxed);
    watch_arm_changed.fetch_add(changed, std::memory_order_relaxed);
    watch_arm_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
}

void GuestMemoryMirror::CheckRequest(uint32_t start, uint32_t length) {
  Coherency& c = *coherency;
  const uint32_t stamp = watch_frame.load(std::memory_order_relaxed) + 1;
  if (c.ranges.empty() || c.ranges.back().start != start || c.ranges.back().length != length)
    c.ranges.push_back({start, length, c.slot});
  const uint32_t end = start + length;
  for (uint32_t p = start >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
    if (c.checked[p] == stamp) continue;
    const uint32_t lo = std::max(start, p << kPageShift), hi = std::min(end, (p + 1) << kPageShift);
    if (!std::memcmp(membase + lo, c.shadow.get() + lo, hi - lo)) {
      if (hi - lo == 1u << kPageShift) c.checked[p] = stamp;
      continue;
    }
    const uint64_t bit = 1ull << (p & 63);
    const bool is_valid = valid[p >> 6].load(std::memory_order_relaxed) & bit;
    const bool hashed = (hot[p >> 6].load(std::memory_order_relaxed) |
                         pending[p >> 6].load(std::memory_order_relaxed)) &
                        bit;
    // A write after this request copied or trusted the page: it faulted (valid cleared) or, on a
    // hot or pending page, changed the hash since. Either way the next request copies it.
    const bool written_since =
        hashed ? PageHash(p) != copy_hash[p].load(std::memory_order_relaxed) : !is_valid;
    if (written_since) continue;
    ++c.stale;
    uint32_t differ = 0, first = UINT32_MAX;
    for (uint32_t i = lo; i < hi; ++i)
      if (membase[i] != c.shadow[i]) {
        ++differ;
        first = std::min(first, i);
      }
    GRAW_LOG_FIRST(200, WARN,
                   "GRAW native coherency: frame {} stale page {:05X} (request {:08X}+{:X}, slot "
                   "{}): {} of {} bytes differ from {:08X}, {} armed at frame {}, last write fault "
                   "frame {}",
                   stamp - 1, p, start, length, c.slot, differ, hi - lo, first,
                   hashed ? "hashed" : "valid", armed_frame[p].load(std::memory_order_relaxed),
                   int64_t(write_frame[p].load(std::memory_order_relaxed)) - 1);
  }
}

void GuestMemoryMirror::CheckSegmentCopy(uint32_t from, uint32_t size) {
  Coherency& c = *coherency;
  const uint32_t end = from + size;
  for (const auto& [b, e] : c.segment_reads) {
    if (!(b < end && from < e)) continue;
    if (segment_conflict) return;
    GRAW_LOG_FIRST(
        200, WARN,
        "GRAW native coherency: frame {} copy {:08X}+{:X} overwrites {:08X}-{:08X}, read "
        "earlier in its copy segment, without a new segment",
        watch_frame.load(std::memory_order_relaxed), from, size, b, e);
    return;
  }
}

void GuestMemoryMirror::CheckLateWrites() {
  Coherency& c = *coherency;
  const uint32_t stamp = watch_frame.load(std::memory_order_relaxed) + 1;
  uint64_t late = 0, stale = 0;
  uint32_t late_ranges = 0, slot_pages[256] = {};
  const Coherency::Range* first_late = nullptr;
  for (const Coherency::Range& r : c.ranges) {
    const uint32_t end = r.start + r.length;
    bool any = false;
    for (uint32_t p = r.start >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
      if (c.swap_checked[p] == stamp) continue;
      c.swap_checked[p] = stamp;
      const uint32_t lo = std::max(r.start, p << kPageShift);
      const uint32_t hi = std::min(end, (p + 1) << kPageShift);
      if (!std::memcmp(membase + lo, c.shadow.get() + lo, hi - lo)) continue;
      const uint64_t bit = 1ull << (p & 63);
      if ((valid[p >> 6].load(std::memory_order_relaxed) & bit) &&
          !((hot[p >> 6].load(std::memory_order_relaxed) |
             pending[p >> 6].load(std::memory_order_relaxed)) &
            bit)) {
        // Valid and watched, yet guest memory differs: a write the watch missed.
        ++stale;
        GRAW_LOG_FIRST(200, WARN,
                       "GRAW native coherency: frame {} stale page {:05X} at the Swap (range "
                       "{:08X}+{:X}, slot {}), armed at frame {}",
                       stamp - 1, p, r.start, r.length, r.slot,
                       armed_frame[p].load(std::memory_order_relaxed));
        continue;
      }
      ++late;
      ++slot_pages[r.slot];
      any = true;
    }
    if (any) {
      ++late_ranges;
      if (!first_late) first_late = &r;
    }
  }
  c.late += late;
  c.stale += stale;
  if (late) {
    auto slots = [&] {
      std::string list;
      for (uint32_t s = 0; s < 256; ++s)
        if (slot_pages[s]) list += fmt::format(" {}:{}", s == 0xFF ? -1 : int(s), slot_pages[s]);
      return list;
    };
    GRAW_LOG_FIRST(400, INFO,
                   "GRAW native coherency: frame {} {} pages of {} ranges rewritten after a draw "
                   "read them (first {:08X}+{:X}); pages per slot{}",
                   stamp - 1, late, late_ranges, first_late->start, first_late->length, slots());
  }
  c.ranges.clear();
}

}  // namespace graw::native
