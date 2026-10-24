// F.E.A.R. 360 - replacement texture pack (PC textures in place of the 360's).
//
// A pack is a folder built offline by tools/texture_pack.py from the player's own PC copy:
// index.tsv maps a key of a 360 texture (an xxHash of its base level's guest bytes, seeded
// with its format and size) to a DDS file with the replacement, at its own size and with its
// own mips. The texture cache asks Find() when it creates a texture from guest memory; on a
// hit it uploads the DDS instead of untiling the 360 data.
//
// The key must be computed identically by the renderer and by the offline indexer
// (port-nx/tools/texindex.cpp); both call ReplacementKey().

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace fear::texture_pack {

// Entry::swizzle when the replacement keeps the 360 host format's channel layout.
constexpr uint32_t kSameSwizzle = UINT32_MAX;

struct Entry {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_count = 0;
  // Bytes per block and block size of `format` (BC: 4x4; uncompressed: 1x1).
  uint32_t block_bytes = 0;
  uint32_t block_width = 1;
  uint32_t block_height = 1;
  // Which host channel holds each guest component (xenos swizzle encoding, as the renderer's
  // host format swizzle), when the replacement's layout differs from the 360 host format's:
  // e.g. a PC DXT texture (RGB) in place of a 360 k_8_8_8_8 one (BGR in memory).
  uint32_t swizzle = kSameSwizzle;
  // DDS file in the pack and where its mip data starts.
  std::string file;
  uint32_t data_offset = 0;
  uint64_t data_size = 0;
};

// The 360 texture's identity: base level bytes as they sit in guest memory, and the fetch
// fields that decide their meaning.
uint64_t ReplacementKey(const uint8_t* base_data, uint32_t base_size, uint32_t format,
                        uint32_t width, uint32_t height, bool tiled, uint32_t pitch);

// Whether a pack is configured and loaded (loads it on the first call).
bool Enabled();
// Whether new texture keys should be logged (fear_texture_log_keys).
bool LogKeys();
const Entry* Find(uint64_t key);
// Reads the replacement's mip data (tightly packed, level after level) into `out`.
bool ReadData(const Entry& entry, uint8_t* out, uint64_t size);
// After a failed load: Find() stops answering for this key, so the 360 texture is used.
// Thread-safe (the texture streaming worker calls it).
void MarkBad(uint64_t key);

// Counters for the log (ReportStats prints and clears them about once a minute).
void CountLookup(bool hit);
void CountLoadFailure();
void ReportStats();

}  // namespace fear::texture_pack
