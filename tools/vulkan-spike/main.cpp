// M0 spike for the native renderer on Vulkan (#41, docs/VULKAN_RENDERER.md). Local only.
//
// Reads the native renderer's pipeline store (<cache_root>/graw-native/pipelines.bin: the guest
// ucode of every shader it drew with, and the vertex/pixel modification pairs it used), translates
// each shader with the SDK's SpirvShaderTranslator for every modification seen, and validates the
// SPIR-V with SPIRV-Tools. The stored modifications are the DXBC translator's; the fields both
// translators share are mapped across.
//   graw_vulkan_spike <pipelines.bin> [<out.csv>]
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include <spirv-tools/libspirv.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using rex::graphics::DxbcShaderTranslator;
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
namespace xenos = rex::graphics::xenos;

namespace {

struct StoredShader {
  uint32_t type;
  std::vector<uint32_t> ucode;  // big-endian, as in guest memory
};

bool ReadStore(const char* path, std::map<uint64_t, StoredShader>& shaders,
               std::map<uint64_t, std::set<uint64_t>>& mods) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> data;
  std::fseek(f, 0, SEEK_END);
  data.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  const bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  if (!ok) return false;
  size_t pos = 0;
  auto take = [&](void* out, size_t n) {
    if (data.size() - pos < n) return false;
    std::memcpy(out, data.data() + pos, n);
    pos += n;
    return true;
  };
  struct {
    char magic[4];
    uint32_t format, key_size, key_version;
  } h;
  if (!take(&h, sizeof(h)) || std::memcmp(h.magic, "P4PS", 4) || h.format != 1) return false;
  std::vector<uint8_t> key(h.key_size);
  for (;;) {
    uint32_t tag;
    if (!take(&tag, 4)) break;
    if (tag == 'S') {
      uint32_t type, dwords;
      uint64_t hash;
      if (!take(&type, 4) || !take(&hash, 8) || !take(&dwords, 4)) break;
      StoredShader s{type, std::vector<uint32_t>(dwords)};
      if (!take(s.ucode.data(), size_t(dwords) * 4)) break;
      shaders.emplace(hash, std::move(s));
    } else if (tag == 'P') {
      uint64_t vs_hash, vs_mod, ps_hash, ps_mod;
      if (!take(&vs_hash, 8) || !take(&vs_mod, 8) || !take(&ps_hash, 8) || !take(&ps_mod, 8) ||
          !take(key.data(), key.size()))
        break;
      mods[vs_hash].insert(vs_mod);
      if (ps_hash) mods[ps_hash].insert(ps_mod);
    } else {
      break;
    }
  }
  return true;
}

// DXBC translator modification -> SPIR-V translator modification (the shared fields).
uint64_t MapModification(bool vertex, uint64_t dxbc_value) {
  const DxbcShaderTranslator::Modification d(dxbc_value);
  SpirvShaderTranslator::Modification s;
  if (vertex) {
    s.vertex.interpolator_mask = d.vertex.interpolator_mask;
    s.vertex.output_point_parameters = d.vertex.output_point_size;
    s.vertex.dynamic_addressable_register_count = d.vertex.dynamic_addressable_register_count;
    s.vertex.user_clip_plane_count = d.vertex.user_clip_plane_count;
    s.vertex.user_clip_plane_cull = d.vertex.user_clip_plane_cull;
    s.vertex.vertex_kill_and = d.vertex.vertex_kill_and;
    s.vertex.point_ps_ucp_mode = d.vertex.point_ps_ucp_mode;
    s.vertex.host_vertex_shader_type = d.vertex.host_vertex_shader_type;
  } else {
    s.pixel.interpolator_mask = d.pixel.interpolator_mask;
    s.pixel.interpolators_centroid = d.pixel.interpolators_centroid;
    s.pixel.dynamic_addressable_register_count = d.pixel.dynamic_addressable_register_count;
    s.pixel.param_gen_enable = d.pixel.param_gen_enable;
    s.pixel.param_gen_interpolator = d.pixel.param_gen_interpolator;
    s.pixel.param_gen_point = d.pixel.param_gen_point;
    s.pixel.depth_stencil_mode =
        SpirvShaderTranslator::Modification::DepthStencilMode(uint32_t(d.pixel.depth_stencil_mode));
  }
  return s.value;
}

}  // namespace

// --xsh <NNNNNNNN.xsh> <out dir>: the Xenos plugin's shader storage (ucode only); writes the
// ucode disassembly and the SPIR-V (default modification) of every pixel shader that samples
// three or more textures, as <hash>.txt.
int DumpXsh(const char* path, const char* out_dir) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return 2;
  uint32_t file_header[2];
  if (std::fread(file_header, sizeof(file_header), 1, f) != 1) return 2;
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), true, true, false);
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  rex::string::StringBuffer disasm;
  size_t dumped = 0, pixel = 0;
  for (;;) {
    uint64_t hash;
    uint32_t bits;
    if (std::fread(&hash, 8, 1, f) != 1 || std::fread(&bits, 4, 1, f) != 1) break;
    const uint32_t dwords = bits & 0x7FFFFFFF;
    const auto type = xenos::ShaderType(bits >> 31);
    std::vector<uint32_t> ucode(dwords);
    if (dwords && std::fread(ucode.data(), 4, dwords, f) != dwords) break;
    if (type != xenos::ShaderType::kPixel) continue;
    ++pixel;
    auto shader = std::make_unique<SpirvShader>(type, hash, ucode.data(), ucode.size());
    shader->AnalyzeUcode(disasm);
    Shader::Translation* t = shader->GetOrCreateTranslation(
        translator.GetDefaultPixelShaderModification(shader->GetDynamicAddressableRegisterCount(0)));
    if (!translator.TranslateAnalyzedShader(*t) || !t->is_valid()) continue;
    if (shader->GetTextureBindingsAfterTranslation().size() < 3) continue;
    std::string spirv;
    const auto& bin = t->translated_binary();
    tools.Disassemble(reinterpret_cast<const uint32_t*>(bin.data()), bin.size() / 4, &spirv,
                      SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES | SPV_BINARY_TO_TEXT_OPTION_INDENT);
    char name[512];
    std::snprintf(name, sizeof(name), "%s/%016llX.txt", out_dir, (unsigned long long)hash);
    if (std::FILE* o = std::fopen(name, "w")) {
      std::fprintf(o, "textures:");
      for (const auto& b : shader->GetTextureBindingsAfterTranslation())
        std::fprintf(o, " tf%u/dim%u/%s", uint32_t(b.fetch_constant), uint32_t(b.dimension),
                     b.is_signed ? "s" : "u");
      std::fprintf(o, "\nsamplers: %zu\n\n%s\n\n%s\n",
                   shader->GetSamplerBindingsAfterTranslation().size(),
                   shader->ucode_disassembly().c_str(), spirv.c_str());
      std::fclose(o);
      ++dumped;
    }
  }
  std::fclose(f);
  std::printf("pixel shaders %zu, dumped %zu\n", pixel, dumped);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 3 && !std::strcmp(argv[1], "--xsh")) return DumpXsh(argv[2], argv[3]);
  if (argc < 2) {
    std::fprintf(stderr, "usage: graw_vulkan_spike <pipelines.bin> [<out.csv>]\n");
    return 2;
  }
  std::map<uint64_t, StoredShader> stored;
  std::map<uint64_t, std::set<uint64_t>> mods;
  if (!ReadStore(argv[1], stored, mods)) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  std::FILE* csv = argc > 2 ? std::fopen(argv[2], "w") : nullptr;
  if (csv) std::fprintf(csv, "hash,stage,dwords,modification,translated,spirv_bytes,valid,error\n");

  // Every feature on (SPIR-V 1.5), host render targets with native 2x MSAA, no fragment shader
  // interlock: what a Vulkan 1.2+ desktop or Steam Deck driver gives the Vulkan backend.
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), true, true, false);
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string last_message;
  tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& p,
                               const char* m) {
    last_message = "word " + std::to_string(p.index) + ": " + m;
  });
  // Block layouts as the Vulkan backend's device allows them (uniformBufferStandardLayout /
  // scalarBlockLayout: both on desktop drivers and RADV on the Deck). --strict-layout: off.
  spvtools::ValidatorOptions options;
  const bool strict_layout = argc > 3 && !std::strcmp(argv[3], "--strict-layout");
  const bool standard_only = argc > 3 && !std::strcmp(argv[3], "--standard-layout-only");
  if (!strict_layout) {
    options.SetUniformBufferStandardLayout(true);
    if (!standard_only) options.SetScalarBlockLayout(true);
  }
  rex::string::StringBuffer disasm;

  size_t shaders = 0, translations = 0, translated = 0, valid = 0;
  std::map<std::string, size_t> failures;
  for (const auto& [hash, s] : stored) {
    const auto type = xenos::ShaderType(s.type);
    const bool vertex = type == xenos::ShaderType::kVertex;
    auto shader = std::make_unique<SpirvShader>(type, hash, s.ucode.data(), s.ucode.size());
    shader->AnalyzeUcode(disasm);
    ++shaders;
    std::set<uint64_t> spirv_mods;
    if (auto it = mods.find(hash); it != mods.end()) {
      for (uint64_t m : it->second) spirv_mods.insert(MapModification(vertex, m));
    }
    if (spirv_mods.empty()) {
      spirv_mods.insert(vertex ? translator.GetDefaultVertexShaderModification(
                                     shader->GetDynamicAddressableRegisterCount(0))
                               : translator.GetDefaultPixelShaderModification(
                                     shader->GetDynamicAddressableRegisterCount(0)));
    }
    for (uint64_t m : spirv_mods) {
      ++translations;
      Shader::Translation* t = shader->GetOrCreateTranslation(m);
      std::string error;
      bool ok = translator.TranslateAnalyzedShader(*t) && t->is_valid();
      if (!ok) {
        error = "translation failed";
        for (const auto& e : t->errors()) {
          error = e.message;
          break;
        }
      }
      bool is_valid = false;
      const auto& bin = t->translated_binary();
      if (ok) {
        ++translated;
        last_message.clear();
        is_valid = tools.Validate(reinterpret_cast<const uint32_t*>(bin.data()), bin.size() / 4,
                                  options);
        if (is_valid) {
          ++valid;
        } else {
          error = last_message;
        }
      }
      if (!error.empty()) {
        // Group by message: drop the "word N: " position.
        std::string key = error;
        if (key.rfind("word ", 0) == 0) key = key.substr(key.find(':') + 2);
        ++failures[key.substr(0, 160)];
      }
      if (csv) {
        for (char& c : error) if (c == ',' || c == '\n') c = ' ';
        std::fprintf(csv, "%016llX,%s,%zu,%016llX,%d,%zu,%d,%s\n", (unsigned long long)hash,
                     vertex ? "vs" : "ps", s.ucode.size(), (unsigned long long)m, ok ? 1 : 0,
                     bin.size(), is_valid ? 1 : 0, error.c_str());
      }
    }
  }
  if (csv) std::fclose(csv);
  std::printf("shaders %zu, translations %zu, translated %zu, valid SPIR-V %zu\n", shaders,
              translations, translated, valid);
  for (const auto& [message, count] : failures) std::printf("  %5zu x %s\n", count, message.c_str());
  return translated == translations && valid == translations ? 0 : 1;
}
