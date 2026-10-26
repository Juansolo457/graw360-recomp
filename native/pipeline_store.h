// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): pipelines that survive runs.
//
// PipelineStore: the shaders (guest ucode) and pipeline keys DrawRenderer has created
// pipelines for, appended to <cache_root>\graw-native\pipelines.bin as they are first seen.
// At start-up DrawRenderer reads them back and translates and compiles them ahead of their
// first draw (prewarm), so a second run of the same route has no draws waiting for pipelines.
// Records are appended and flushed once per frame: the game is usually killed, not shut down.
// Threading: the renderer's thread (DrawRenderer's On* calls) only.
// The Vulkan backend keeps one too, in its own directory (vulkan_backend.cpp). API-free: the
// D3D12 pipeline library is in pipeline_library.h.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace graw::native {

// A file's bytes (empty if it can't be read), and std::fopen with wide paths on Windows.
std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path);
std::FILE* OpenFile(const std::filesystem::path& path, const char* mode);

class PipelineStore {
 public:
  struct Shader {
    uint32_t type;                // xenos::ShaderType
    std::vector<uint32_t> ucode;  // as in guest memory (big-endian)
  };
  struct Pipeline {
    uint64_t vs_hash, vs_mod, ps_hash, ps_mod;  // ucode_data_hash and modification; ps_hash 0: none
    std::vector<uint8_t> key;                   // the pipeline key with its shader pointers zeroed
  };

  ~PipelineStore();

  // Reads <dir>\pipelines.bin (started over if its header does not match key_size and
  // key_version, or cut at the last whole record) and opens it for appending. Stays closed if
  // key_size is over kMaxKeySize.
  void Open(const std::filesystem::path& dir, uint32_t key_size, uint32_t key_version);
  bool is_open() const { return file_ != nullptr; }

  // Records from earlier runs, in the order they were first seen.
  const std::vector<Pipeline>& loaded_pipelines() const { return loaded_pipelines_; }
  const Shader* loaded_shader(uint64_t hash) const {
    auto it = loaded_shaders_.find(hash);
    return it == loaded_shaders_.end() ? nullptr : &it->second;
  }

  // Appends what is not in the file yet. host_ucode: Shader::ucode_dwords() (host order). key:
  // the key_size bytes given to Open.
  void RecordShader(uint32_t type, uint64_t hash, const uint32_t* host_ucode, size_t dwords);
  void RecordPipeline(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash, uint64_t ps_mod,
                      const void* key);
  // Once per frame: makes the records appended since the last call durable.
  void Flush();

  static constexpr uint32_t kMaxKeySize = 256;

 private:
  std::FILE* file_ = nullptr;
  uint32_t key_size_ = 0;
  bool dirty_ = false;
  std::unordered_map<uint64_t, Shader> loaded_shaders_;
  std::vector<Pipeline> loaded_pipelines_;
  std::unordered_set<uint64_t> shaders_written_, pipelines_written_;
};

}  // namespace graw::native
