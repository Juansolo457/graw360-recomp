// GRAW 360 - debugging the start-up crash in the HLSL compiler linked into the game.
//
// Two midasm hooks (port/graw_manifest.toml):
//  - GrawShaderCompileCall, at 0x82226A38: the game's D3DX compile wrapper (sub_82226928) just
//    before it calls the compiler (sub_8224A808). Logs r3..r10 and any string or buffer they
//    reach, and writes long text it finds (the shader source) to logs/shader-<n>.txt.
//  - GrawParserReduce, at 0x822AE480: the yacc parser (sub_822AD650) just before it calls the
//    semantic-action dispatcher (sub_822AC1E8) with r4 = grammar rule, r5 = values the rule pops.
//    r3 is the semantic state; its value list is at +8 (node: +8 value, +12 next). Writes every
//    reduction to logs/parser-trace.txt and logs the first one with fewer values than it needs.
// Both are on while graw_debug_shader_compile is set (off by default since the parser fix).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "host_memory.h"

REXCVAR_DEFINE_BOOL(graw_debug_shader_compile, false, "GRAW",
                    "Log the game's HLSL compile calls and every parser reduction "
                    "(logs/shader-*.txt, logs/parser-trace.txt)");

namespace {

std::mutex g_mutex;
int g_compiles = 0;
FILE* g_trace = nullptr;
uint64_t g_reductions = 0;
bool g_reported_short = false;

uint8_t* Base() { return rex::system::kernel_memory()->virtual_membase(); }

// A guest address whose host page is committed and readable (a register can hold a float or a
// stale value that points at unmapped guest memory; the first version of this hook crashed on one).
bool Readable(uint32_t a, size_t n) {
  if (a < 0x10000) return false;
  return graw::HostReadable(Base() + a, n);
}
bool Plausible(uint32_t a) { return a < 0xC0000000u && Readable(a, 64); }

uint32_t Load32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Base() + a, 4);
  return rex::byte_swap(v);
}

// The printable C string at a, or empty if it isn't one (at least 2 characters).
std::string Text(uint32_t a, size_t limit) {
  if (!Plausible(a)) return {};
  std::string s;
  for (size_t i = 0; i < limit; ++i) {
    if ((i & 0xFFF) == 0 && !Readable(a + static_cast<uint32_t>(i), 1)) break;
    const uint8_t c = Base()[a + i];
    if (c == 0) break;
    if (c != '\n' && c != '\r' && c != '\t' && (c < 32 || c > 126)) return {};
    s.push_back(static_cast<char>(c));
  }
  return s.size() >= 2 ? s : std::string{};
}

std::string Words(uint32_t a, int count) {
  std::string out;
  char buf[16];
  for (int i = 0; i < count; ++i) {
    std::snprintf(buf, sizeof(buf), "%08X ", Load32(a + 4 * i));
    out += buf;
  }
  return out;
}

void SaveText(const std::string& name, const std::string& text) {
  if (FILE* f = std::fopen(name.c_str(), "wb")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
}

// One register: its value, a string it points at, the words there, and strings one level down.
void Describe(int compile, const char* name, uint32_t v) {
  std::string line = std::string(name) + "=" + fmt::format("{:08X}", v);
  if (Plausible(v)) {
    const std::string s = Text(v, 4096);
    if (!s.empty()) {
      line += fmt::format(" \"{}\"", s.size() > 120 ? s.substr(0, 120) + "..." : s);
      if (s.size() > 200) {
        const std::string file = fmt::format("logs/shader-{}-{}.txt", compile, name);
        SaveText(file, Text(v, 1 << 20));
        line += " -> " + file;
      }
    } else {
      line += " [" + Words(v, 8) + "]";
      for (int i = 0; i < 16; ++i) {
        const uint32_t p = Load32(v + 4 * i);
        const std::string t = Text(p, 1 << 20);
        if (t.empty()) continue;
        line += fmt::format(" +{}:\"{}\"", 4 * i, t.size() > 80 ? t.substr(0, 80) + "..." : t);
        if (t.size() > 200) {
          const std::string file = fmt::format("logs/shader-{}-{}+{}.txt", compile, name, 4 * i);
          SaveText(file, t);
          line += " -> " + file;
        }
      }
    }
  }
  REXLOG_INFO("GRAW shader compile {}: {}", compile, line);
}

}  // namespace

void GrawShaderCompileCall(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                           PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10) {
  if (!REXCVAR_GET(graw_debug_shader_compile)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const int n = ++g_compiles;
  const char* names[] = {"r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10"};
  const uint32_t values[] = {r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32};
  for (int i = 0; i < 8; ++i) Describe(n, names[i], values[i]);
  if (g_trace) std::fprintf(g_trace, "--- compile %d\n", n);
}

void GrawParserReduce(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  if (!REXCVAR_GET(graw_debug_shader_compile)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_trace) g_trace = std::fopen("logs/parser-trace.txt", "w");
  const uint32_t state = r3.u32;
  const uint32_t rule = r4.u32, need = r5.u32;
  uint32_t have = 0;
  std::string values;
  if (Plausible(state)) {
    uint32_t node = Load32(state + 8);
    while (Plausible(node) && have < 64) {
      if (have < 4) values += fmt::format("{:08X} ", Load32(node + 8));
      ++have;
      node = Load32(node + 12);
    }
  }
  ++g_reductions;
  if (g_trace) {
    std::fprintf(g_trace, "%llu rule %u need %u have %u  %s\n",
                 static_cast<unsigned long long>(g_reductions), rule, need, have, values.c_str());
    std::fflush(g_trace);
  }
  if (have < need && !g_reported_short) {
    g_reported_short = true;
    REXLOG_WARN("GRAW parser: reduction {} rule {} needs {} values, the stack has {} ({})",
                g_reductions, rule, need, have, values);
  }
}
