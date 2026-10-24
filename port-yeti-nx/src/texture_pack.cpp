// F.E.A.R. 360 - replacement texture pack. See texture_pack.h.

#include "texture_pack.h"

#include <atomic>
#include <chrono>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <unordered_set>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <xxhash.h>

REXCVAR_DEFINE_STRING(fear_texture_pack, "", "F.E.A.R.",
                      "Folder of a replacement texture pack built by tools/texture_pack.py "
                      "(index.tsv + DDS files); empty = the 360's own textures")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// PC Settings' on/off: the folder stays saved while the pack is off.
REXCVAR_DEFINE_BOOL(fear_texture_pack_enabled, true, "F.E.A.R.",
                    "Use the texture pack in fear_texture_pack (off = the 360's own textures)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fear_texture_log_keys, false, "F.E.A.R.",
                    "Log the pack key, format and size of every texture created from guest "
                    "memory (to check them against the offline index)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fear::texture_pack {
namespace {

std::once_flag load_once;
bool loaded = false;
std::unordered_map<uint64_t, Entry> entries;
std::filesystem::path root;
std::mutex bad_mutex;
std::unordered_set<uint64_t> bad_keys;

std::atomic<uint64_t> lookups{0}, hits{0}, failures{0};
std::atomic<int64_t> last_report_ms{0};

bool BlockInfo(VkFormat format, uint32_t& bytes, uint32_t& bw, uint32_t& bh) {
  bw = bh = 1;
  switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
      bytes = 8, bw = bh = 4;
      return true;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
      bytes = 16, bw = bh = 4;
      return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
      bytes = 4;
      return true;
    case VK_FORMAT_R8G8_UNORM:
      bytes = 2;
      return true;
    case VK_FORMAT_R8_UNORM:
      bytes = 1;
      return true;
    default:
      return false;
  }
}

uint64_t MipChainBytes(const Entry& e) {
  uint64_t total = 0;
  for (uint32_t level = 0; level < e.mip_count; ++level) {
    uint32_t w = std::max(e.width >> level, 1u), h = std::max(e.height >> level, 1u);
    total += uint64_t((w + e.block_width - 1) / e.block_width) *
             ((h + e.block_height - 1) / e.block_height) * e.block_bytes;
  }
  return total;
}

// index.tsv: key (16 hex digits), VkFormat number, width, height, mips, data offset, file,
// and optionally (after a tab) a host swizzle override in hex.
void Load() {
  const std::string configured = REXCVAR_GET(fear_texture_pack);
  if (configured.empty() || !REXCVAR_GET(fear_texture_pack_enabled)) return;
  root = std::filesystem::path(configured);
  std::ifstream index(root / "index.tsv");
  if (!index) {
    REXLOG_ERROR("F.E.A.R.: texture pack index not found at {}", (root / "index.tsv").string());
    return;
  }
  std::string line;
  size_t bad = 0;
  while (std::getline(index, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string key_text, file;
    uint32_t format = 0;
    Entry e;
    if (!(fields >> key_text >> format >> e.width >> e.height >> e.mip_count >> e.data_offset) ||
        !std::getline(fields >> std::ws, file)) {
      ++bad;
      continue;
    }
    uint64_t key = 0;
    auto parsed = std::from_chars(key_text.data(), key_text.data() + key_text.size(), key, 16);
    e.format = VkFormat(format);
    if (parsed.ec != std::errc() ||
        !BlockInfo(e.format, e.block_bytes, e.block_width, e.block_height) || !e.mip_count ||
        !e.width || !e.height) {
      ++bad;
      continue;
    }
    if (auto tab = file.find('\t'); tab != std::string::npos) {
      const std::string swizzle = file.substr(tab + 1);
      file.resize(tab);
      auto sw = std::from_chars(swizzle.data(), swizzle.data() + swizzle.size(), e.swizzle, 16);
      if (sw.ec != std::errc() || e.swizzle > 0xFFF) {
        ++bad;
        continue;
      }
    }
    e.file = file;
    e.data_size = MipChainBytes(e);
    entries.emplace(key, std::move(e));
  }
  loaded = !entries.empty();
  REXLOG_INFO("F.E.A.R.: texture pack {}: {} replacements{}", root.string(), entries.size(),
              bad ? fmt::format(", {} unreadable index lines", bad) : std::string());
}

}  // namespace

uint64_t ReplacementKey(const uint8_t* base_data, uint32_t base_size, uint32_t format,
                        uint32_t width, uint32_t height, bool tiled, uint32_t pitch) {
  const uint64_t seed = uint64_t(format & 0x3F) | (uint64_t(width - 1) & 0x1FFF) << 6 |
                        (uint64_t(height - 1) & 0x1FFF) << 19 | uint64_t(tiled) << 32 |
                        (uint64_t(pitch) & 0x1FF) << 33;
  return XXH3_64bits_withSeed(base_data, base_size, seed);
}

bool Enabled() {
  std::call_once(load_once, Load);
  return loaded;
}

bool LogKeys() { return REXCVAR_GET(fear_texture_log_keys); }

const Entry* Find(uint64_t key) {
  if (!Enabled()) return nullptr;
  auto it = entries.find(key);
  if (it == entries.end()) return nullptr;
  std::lock_guard<std::mutex> lock(bad_mutex);
  return bad_keys.count(key) ? nullptr : &it->second;
}

void MarkBad(uint64_t key) {
  {
    std::lock_guard<std::mutex> lock(bad_mutex);
    if (!bad_keys.insert(key).second) return;
  }
  CountLoadFailure();
  auto it = entries.find(key);
  REXLOG_WARN("F.E.A.R.: texture pack file failed to load, using the 360 texture: {}",
              it == entries.end() ? std::string("?") : (root / it->second.file).string());
}

bool ReadData(const Entry& entry, uint8_t* out, uint64_t size) {
  if (size < entry.data_size) return false;
  std::ifstream file(root / entry.file, std::ios::binary);
  if (!file) return false;
  file.seekg(entry.data_offset);
  file.read(reinterpret_cast<char*>(out), std::streamsize(entry.data_size));
  return uint64_t(file.gcount()) == entry.data_size;
}

void CountLookup(bool hit) {
  lookups.fetch_add(1, std::memory_order_relaxed);
  if (hit) hits.fetch_add(1, std::memory_order_relaxed);
}

void CountLoadFailure() { failures.fetch_add(1, std::memory_order_relaxed); }

void ReportStats() {
  if (!loaded) return;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t last = last_report_ms.load(std::memory_order_relaxed);
  if (now - last < 60000 || !last_report_ms.compare_exchange_strong(last, now)) return;
  const uint64_t l = lookups.exchange(0), h = hits.exchange(0), f = failures.exchange(0);
  if (l) {
    REXLOG_INFO("F.E.A.R.: texture pack: {} of {} new textures replaced{}", h, l,
                f ? fmt::format(", {} failed to load", f) : std::string());
  }
}

}  // namespace fear::texture_pack
