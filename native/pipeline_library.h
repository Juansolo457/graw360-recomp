// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): the D3D12
// pipeline library (split out of pipeline_store.h).
//
// PipelineLibrary: an ID3D12PipelineLibrary in <cache_root>\graw-native\pipelines.d3d12lib, so
// the driver's compiled pipelines are loaded instead of compiled again. Saved by a pipeline
// worker when they have been idle for a while. Threading: Open once, before any other call;
// Create from any thread, Save from one at a time.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace graw::native {

class PipelineLibrary {
 public:
  // root_signature_hash: CreateBindlessRootSignature's blob_hash. A pipeline stored with another
  // root signature does not load with this one, so it is part of every name.
  void Open(ID3D12Device* device, const std::filesystem::path& path, uint64_t root_signature_hash);
  // Loads the pipeline from the library, or creates it and stores it there. *loaded: whether
  // it came from the library.
  HRESULT Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ID3D12PipelineState** out,
                 bool* loaded);
  bool dirty() const { return dirty_.load(std::memory_order_relaxed); }
  // Writes the library if a pipeline was stored since the last save.
  void Save();

 private:
  ID3D12Device* device_ = nullptr;
  std::vector<uint8_t> blob_;  // must outlive library_, so it is declared first
  Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library_;
  std::filesystem::path path_;
  uint64_t root_signature_hash_ = 0;
  std::atomic<bool> dirty_{false};
  std::atomic<uint32_t> loaded_{0}, stored_{0};
  // Names being loaded or stored: the library does not synchronize two loads of one pipeline.
  std::mutex busy_mutex_;
  std::unordered_set<uint64_t> busy_;
};

}  // namespace graw::native
