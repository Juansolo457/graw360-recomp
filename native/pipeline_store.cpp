// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): pipelines that survive runs (see pipeline_store.h).
#include "pipeline_store.h"

#include <rex/logging.h>
#include <rex/types.h>

#include <xxhash.h>

#include <cstring>
#include <system_error>

namespace graw::native {

namespace {

constexpr char kMagic[4] = {'P', '4', 'P', 'S'};
constexpr uint32_t kFormat = 1;
constexpr uint32_t kShaderTag = 'S', kPipelineTag = 'P';

struct Header {
  char magic[4];
  uint32_t format, key_size, key_version;
};

}  // namespace

std::FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  wchar_t wide[8] = {};
  for (size_t i = 0; mode[i] && i < 7; ++i) wide[i] = wchar_t(mode[i]);
  return _wfopen(path.c_str(), wide);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  if (std::FILE* f = OpenFile(path, "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
      data.resize(size_t(size));
      if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
  }
  return data;
}

PipelineStore::~PipelineStore() {
  if (file_) std::fclose(file_);
}

void PipelineStore::Open(const std::filesystem::path& dir, uint32_t key_size,
                         uint32_t key_version) {
  if (key_size > kMaxKeySize) {
    REXLOG_WARN("GRAW native pipelines: key of {} bytes is over {}, no pipeline store", key_size,
                kMaxKeySize);
    return;
  }
  key_size_ = key_size;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = dir / "pipelines.bin";
  const std::vector<uint8_t> data = ReadWholeFile(path);
  size_t pos = 0, good = 0;
  auto take = [&](void* out, size_t n) {
    if (data.size() - pos < n) return false;
    std::memcpy(out, data.data() + pos, n);
    pos += n;
    return true;
  };
  Header h;
  if (take(&h, sizeof(h)) && !std::memcmp(h.magic, kMagic, 4) && h.format == kFormat &&
      h.key_size == key_size && h.key_version == key_version) {
    good = pos;
    for (;;) {
      uint32_t tag;
      if (!take(&tag, 4)) break;
      if (tag == kShaderTag) {
        uint32_t type, dwords;
        uint64_t hash;
        Shader s;
        if (!take(&type, 4) || !take(&hash, 8) || !take(&dwords, 4) ||
            data.size() - pos < size_t(dwords) * 4)
          break;
        s.type = type;
        s.ucode.resize(dwords);
        take(s.ucode.data(), size_t(dwords) * 4);
        shaders_written_.insert(hash);
        loaded_shaders_.emplace(hash, std::move(s));
      } else if (tag == kPipelineTag) {
        Pipeline p;
        p.key.resize(key_size);
        if (!take(&p.vs_hash, 8) || !take(&p.vs_mod, 8) || !take(&p.ps_hash, 8) ||
            !take(&p.ps_mod, 8) || !take(p.key.data(), key_size))
          break;
        pipelines_written_.insert(XXH3_64bits(data.data() + good + 4, pos - good - 4));
        loaded_pipelines_.push_back(std::move(p));
      } else {
        break;
      }
      good = pos;
    }
  }
  if (good && good != data.size()) {
    std::filesystem::resize_file(path, good, ec);
    if (ec) {
      // Appending after the torn tail would hide every later record from the next run. The
      // records read stay for this run's prewarm and are written again when next created.
      REXLOG_WARN("GRAW native pipelines: cannot cut {} ({}), starting over", path.string(),
                  ec.message());
      good = 0;
      shaders_written_.clear();
      pipelines_written_.clear();
    }
  }
  if (good) {
    file_ = OpenFile(path, "ab");
  } else {
    file_ = OpenFile(path, "wb");
    if (file_) {
      Header nh;
      std::memcpy(nh.magic, kMagic, 4);
      nh.format = kFormat;
      nh.key_size = key_size;
      nh.key_version = key_version;
      std::fwrite(&nh, sizeof(nh), 1, file_);
      dirty_ = true;
    }
  }
  if (!file_) {
    REXLOG_WARN("GRAW native pipelines: cannot open {} for writing", path.string());
    return;
  }
  REXLOG_INFO("GRAW native pipelines: {} pipelines, {} shaders from earlier runs ({})",
              loaded_pipelines_.size(), loaded_shaders_.size(), path.string());
}

void PipelineStore::RecordShader(uint32_t type, uint64_t hash, const uint32_t* host_ucode,
                                 size_t dwords) {
  if (!file_ || !shaders_written_.insert(hash).second) return;
  std::vector<uint32_t> guest(dwords);
  for (size_t i = 0; i < dwords; ++i) guest[i] = rex::byte_swap(host_ucode[i]);
  const uint32_t n = uint32_t(dwords);
  std::fwrite(&kShaderTag, 4, 1, file_);
  std::fwrite(&type, 4, 1, file_);
  std::fwrite(&hash, 8, 1, file_);
  std::fwrite(&n, 4, 1, file_);
  std::fwrite(guest.data(), 4, dwords, file_);
  dirty_ = true;
}

void PipelineStore::RecordPipeline(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash,
                                   uint64_t ps_mod, const void* key) {
  if (!file_) return;
  uint8_t record[32 + kMaxKeySize];
  std::memcpy(record, &vs_hash, 8);
  std::memcpy(record + 8, &vs_mod, 8);
  std::memcpy(record + 16, &ps_hash, 8);
  std::memcpy(record + 24, &ps_mod, 8);
  std::memcpy(record + 32, key, key_size_);
  if (!pipelines_written_.insert(XXH3_64bits(record, 32 + key_size_)).second) return;
  std::fwrite(&kPipelineTag, 4, 1, file_);
  std::fwrite(record, 1, 32 + key_size_, file_);
  dirty_ = true;
}

void PipelineStore::Flush() {
  if (!dirty_) return;
  dirty_ = false;
  std::fflush(file_);
}

}  // namespace graw::native
