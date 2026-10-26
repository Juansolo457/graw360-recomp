// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): the D3D12
// pipeline library (pipeline_library.h).
#include "pipeline_library.h"

#include "pipeline_store.h"  // ReadWholeFile, OpenFile

#include <rex/logging.h>

#include <xxhash.h>

#include <cstring>
#include <cwchar>
#include <system_error>

namespace graw::native {

void PipelineLibrary::Open(ID3D12Device* device, const std::filesystem::path& path,
                           uint64_t root_signature_hash) {
  device_ = device;
  path_ = path;
  root_signature_hash_ = root_signature_hash;
  if (path.empty()) return;  // cache off: Create compiles every pipeline
  Microsoft::WRL::ComPtr<ID3D12Device1> device1;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
    REXLOG_WARN("GRAW native pipelines: no ID3D12Device1, no pipeline library");
    return;
  }
  blob_ = ReadWholeFile(path);
  HRESULT hr = E_FAIL;
  if (!blob_.empty()) {
    hr = device1->CreatePipelineLibrary(blob_.data(), blob_.size(), IID_PPV_ARGS(&library_));
    if (FAILED(hr))
      REXLOG_INFO("GRAW native pipelines: library {} not usable ({:08X}), starting over",
                  path.string(), uint32_t(hr));
  }
  if (FAILED(hr)) {
    blob_.clear();
    library_.Reset();
    hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library_));
    if (FAILED(hr)) {
      REXLOG_WARN("GRAW native pipelines: CreatePipelineLibrary failed {:08X}", uint32_t(hr));
      library_.Reset();
      return;
    }
  }
  REXLOG_INFO("GRAW native pipelines: library {} ({} KB)", path.string(), blob_.size() / 1024);
}

HRESULT PipelineLibrary::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                ID3D12PipelineState** out, bool* loaded) {
  *loaded = false;
  if (!library_) return device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
  // Name: the root signature, the description without its pointers, then every shader's bytecode.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC plain;
  std::memcpy(&plain, &desc, sizeof(plain));  // with the padding the caller zeroed
  plain.pRootSignature = nullptr;
  for (D3D12_SHADER_BYTECODE* b : {&plain.VS, &plain.PS, &plain.DS, &plain.HS, &plain.GS})
    b->pShaderBytecode = nullptr;
  plain.StreamOutput = {};
  plain.InputLayout = {};
  plain.CachedPSO = {};
  uint64_t h = XXH3_64bits_withSeed(&plain, sizeof(plain), root_signature_hash_);
  for (const D3D12_SHADER_BYTECODE* b : {&desc.VS, &desc.PS, &desc.DS, &desc.HS, &desc.GS})
    if (b->BytecodeLength) h = XXH3_64bits_withSeed(b->pShaderBytecode, b->BytecodeLength, h);
  {
    std::lock_guard<std::mutex> lock(busy_mutex_);
    if (!busy_.insert(h).second)  // the same pipeline on another worker: bypass the library
      return device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
  }
  wchar_t name[20];
  std::swprintf(name, 20, L"%016llX", static_cast<unsigned long long>(h));
  HRESULT hr = library_->LoadGraphicsPipeline(name, &desc, IID_PPV_ARGS(out));
  if (SUCCEEDED(hr)) {
    *loaded = true;
    loaded_.fetch_add(1, std::memory_order_relaxed);
  } else {
    hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
    if (SUCCEEDED(hr) && SUCCEEDED(library_->StorePipeline(name, *out))) {
      stored_.fetch_add(1, std::memory_order_relaxed);
      dirty_.store(true, std::memory_order_relaxed);
    }
  }
  std::lock_guard<std::mutex> lock(busy_mutex_);
  busy_.erase(h);
  return hr;
}

void PipelineLibrary::Save() {
  if (!library_ || !dirty_.exchange(false, std::memory_order_relaxed)) return;
  std::vector<uint8_t> data(library_->GetSerializedSize());
  const HRESULT hr = library_->Serialize(data.data(), data.size());
  if (FAILED(hr)) {
    // A pipeline stored since GetSerializedSize (another worker) makes the buffer too small.
    dirty_.store(true, std::memory_order_relaxed);
    REXLOG_WARN("GRAW native pipelines: library Serialize failed {:08X}", uint32_t(hr));
    return;
  }
  std::filesystem::path tmp = path_;
  tmp += L".tmp";
  std::FILE* f = OpenFile(tmp, "wb");
  if (!f) return;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  std::error_code ec;
  if (ok) std::filesystem::rename(tmp, path_, ec);
  REXLOG_INFO("GRAW native pipelines: library saved, {} KB ({} loaded, {} stored this run){}",
              data.size() / 1024, loaded_.load(), stored_.load(), ok && !ec ? "" : " FAILED");
}

}  // namespace graw::native
