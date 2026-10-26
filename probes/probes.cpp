// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and
// `plugin reload` in the running coverage-build game. API: probes/probe_api.h.
//
// Current probe (Session 67, mouse on the match setup screens): the UI list widget and its labels. The PLAY / MATCH
// RULES / SELECT ARENA bar after the superstar select is a list of the game's UI system (vtable 0x820660A8):
// sub_82B49A70(list) is its input step (reads the pad, then sub_82B49458(list, [list+88] +- 1)), sub_82B49458 sets
// the selected item (stored at +88 through +12, and +856); +584 layout (0 vertical, 1 horizontal), +588 wrap, +612
// item count, +760 / +764 the 80-byte item records (run 4361, write watch + listing). This probe logs:
//   - each list whose input step runs, when first seen or when its index changes (fields above, +8, +576), and
//     dumps its first 1024 bytes once to ..\..\..\out\s67\list_<addr>.bin;
//   - each text element's set-text (sub_82B53068) once per element and text: the text, its box (world centre +768 /
//     +772, size +448 / +452), the caller and the parent chain (+16, 6 levels);
//   - sub_82B49458 calls that change the index (list, old, new, caller).
// The Session 67 label probe (dumps of one frame's labels, tools\menus\label_dump.py) is in git history (b218776).

#include "probe_api.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

const Wwe2k14ProbeHost* g_host = nullptr;
constexpr uint32_t kSetText = 0x82B53068, kListStep = 0x82B49A70, kSetSelected = 0x82B49458;

std::mutex g_mutex;
std::map<uint32_t, uint32_t> g_list_index;                  // list -> last logged index
std::set<std::pair<uint32_t, std::string>> g_texts_seen;     // (element, text)
thread_local uint32_t t_element = 0, t_lr = 0;
int g_text_lines = 0;

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_host->log(buf);
}

uint32_t R32(uint32_t a) {
  uint32_t v = 0;
  g_host->read32(a, &v);
  return v;
}

float RF(uint32_t a) {
  uint32_t v = R32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void DumpBytes(const char* name, uint32_t addr, uint32_t n) {
  char path[256];
  std::snprintf(path, sizeof(path), "..\\..\\..\\out\\s67\\%s_%08X.bin", name, addr);
  FILE* f = std::fopen(path, "wb");
  if (!f) return Log("cannot write %s", path);
  std::vector<uint8_t> bytes(n, 0);
  for (uint32_t o = 0; o < n; o += 4) g_host->read_bytes(addr + o, &bytes[o], 4);
  std::fwrite(bytes.data(), 1, n, f);
  std::fclose(f);
}

void OnListStep(uint32_t list) {
  const uint32_t index = R32(list + 88);
  {
    std::lock_guard lock(g_mutex);
    auto it = g_list_index.find(list);
    const bool fresh = it == g_list_index.end();
    if (!fresh && it->second == index) return;
    g_list_index[list] = index;
    if (fresh) DumpBytes("list", list, 1024);
  }
  const uint32_t owner = R32(list + 8), tree = owner ? R32(owner + 8) : 0;
  const uint32_t nodes = tree ? R32(tree + 704) : 0, node_count = tree ? R32(tree + 140) : 0, id = R32(list + 576);
  Log("list %08X vt %08X frame %llu: index %u (+856 %u) layout %u wrap %u count %u records %08X x %u; +8 %08X tree %08X "
      "nodes %08X x %u, id %u; +16 %08X +20 %08X +924 %08X",
      list, R32(list), (unsigned long long)g_host->frame(), index, R32(list + 856), R32(list + 584), R32(list + 588),
      R32(list + 612), R32(list + 760), R32(list + 764), owner, tree, nodes, node_count, id, R32(list + 16),
      R32(list + 20), R32(list + 924));
  if (nodes && id < node_count && id < 4096) DumpBytes("node", nodes + id * 272, 272);
}

void OnTextSet(uint32_t element, uint32_t lr) {
  const uint32_t str = R32(element + 1136);
  char text[64] = {};
  if (str) g_host->read_string(str, text, sizeof(text));
  {
    std::lock_guard lock(g_mutex);
    if (!g_texts_seen.insert({element, text}).second || g_text_lines >= 3000) return;
    ++g_text_lines;
  }
  char chain[160] = {};
  uint32_t p = element;
  int n = 0;
  for (int level = 0; level < 6; ++level) {
    p = R32(p + 16);
    if (!p) break;
    n += std::snprintf(chain + n, sizeof(chain) - n, " %08X", p);
  }
  Log("text %08X '%s' at (%.0f, %.0f) size %.0f x %.0f, caller %08X, parents%s", element, text, RF(element + 768),
      RF(element + 772), RF(element + 448), RF(element + 452), lr, chain);
}

}  // namespace

extern "C" {

__declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  g_host = host;
  std::lock_guard lock(g_mutex);
  g_list_index.clear();
  g_texts_seen.clear();
  g_text_lines = 0;
  Log("Session 67 list probe: sub_82B49A70 (list input), sub_82B49458 (set selected), sub_82B53068 (set-text)");
  return WWE2K14_PROBE_API_VERSION;
}

__declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  if (cap < 3) return 0;
  out[0] = kSetText;
  out[1] = kListStep;
  out[2] = kSetSelected;
  return 3;
}

__declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) {
  const uint32_t r3 = uint32_t(g_host->get_gpr(ctx, 3));
  if (addr == kSetText) {
    t_element = r3;
    t_lr = g_host->get_lr(ctx);
  } else if (addr == kListStep) {
    OnListStep(r3);
  } else if (addr == kSetSelected) {
    const uint32_t old = R32(r3 + 88), want = uint32_t(g_host->get_gpr(ctx, 4));
    if (want != old)
      Log("set selected: list %08X %u -> %u, caller %08X, frame %llu", r3, old, want, g_host->get_lr(ctx),
          (unsigned long long)g_host->frame());
  }
}

__declspec(dllexport) void wwe2k14_probe_leave(void* ctx, uint32_t addr) {
  (void)ctx;
  if (addr == kSetText && t_element) OnTextSet(t_element, t_lr);
}

}  // extern "C"
