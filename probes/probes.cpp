// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and
// `plugin reload` in the running coverage-build game. API: probes/probe_api.h.
//
// Current probe (Session 34, 30 Years of WrestleMania; docs/systems/wm30.md): the jump experiment.
//   S = [0x839295F0] the mode state; A = [S+8] (record key at A+12); M = [S+12] the match-time table object,
//   U = [S+16] the menu's table object. A table object: slot of kind k at +52+4k, its size at +136+4(k-1), its
//   compressed source at +216+4(k-1); kind 2 = the current era's records, kinds 15-19 = the five eras, kind 4 = the
//   46-match index. sub_82A9E0C8(obj) decompresses every kind with a size and an empty slot.
// sub_82A9DFB8(obj, era, r5) loads an object's tables from "/DSC<era>/R<era>" (evtdsc<era>.pac; fallback
// "/CSDT/R<era>"). If out\wm30\probe_key.txt holds "KEY ERA" (a record id and its era 0-4): that load gets ERA and
// A+12 = KEY at once; again on entry to sub_827A9EB0 (creates the match builder from A+12). S / A / M are dumped there.
// Logs the first lookups sub_82A9DB90(obj, kind, key, field) of each distinct (caller, kind, key).
// The Session 33 pause-menu probe is in git history (commit 312db14).

#include "probe_api.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace {

const Wwe2k14ProbeHost* g_host = nullptr;
constexpr uint32_t kLookup = 0x82A9DB90;
constexpr uint32_t kCreate = 0x827A9EB0;
constexpr uint32_t kModeState = 0x839295F0;
constexpr uint32_t kLoadEra = 0x82A9DFB8;
constexpr uint32_t kSetRecord = 0x82AA5478;  // script op: A+0x0C = [r5+4] (the era script's "this match")

std::mutex g_mutex;
std::set<std::pair<uint32_t, uint32_t>> g_seen;
int g_key = -1, g_era = -1, g_step = -1;  // g_step = the moment code (index column 3), e.g. 10200

void Line(const char* fmt, ...) {
  char buf[512];
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

void Dump(const char* name, uint32_t base, int rows) {
  Line("%s %08X", name, base);
  for (int row = 0; row < rows; ++row) {
    char line[256];
    int n = std::snprintf(line, sizeof(line), "  +%03X:", row * 32);
    for (int i = 0; i < 8; ++i) n += std::snprintf(line + n, sizeof(line) - n, " %08X", R32(base + row * 32 + i * 4));
    g_host->log(line);
  }
}

}  // namespace

extern "C" {

__declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  g_host = host;
  std::lock_guard lock(g_mutex);
  g_seen.clear();
  g_key = g_era = g_step = -1;
  if (std::FILE* f = std::fopen("C:\\Users\\Jarrod\\wwe2k14-recomp\\out\\wm30\\probe_key.txt", "r")) {
    if (std::fscanf(f, "%d %d %d", &g_key, &g_era, &g_step) < 2) g_key = g_era = -1;
    std::fclose(f);
  }
  Line("wm30 probe: jump to key %d era %d step %d", g_key, g_era, g_step);
  return WWE2K14_PROBE_API_VERSION;
}

__declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  if (cap < 4) return 0;
  out[0] = kLookup;
  out[1] = kCreate;
  out[2] = kLoadEra;
  out[3] = kSetRecord;
  return 4;
}

__declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) {
  std::lock_guard lock(g_mutex);
  if (addr == kCreate) {
    const uint32_t s = R32(kModeState), a = R32(s + 8), m = R32(s + 12), u = R32(s + 16);
    Line("create sub_827A9EB0 r3 %08X frame %llu", uint32_t(g_host->get_gpr(ctx, 3)),
         static_cast<unsigned long long>(g_host->frame()));
    static int dumps = 0;
    if (dumps++ < 3) {
      Dump("S", s, 2);
      Dump("A", a, 2);
      Dump("M", m, 2);
    }
    (void)u;
    if (false && g_key >= 0 && R32(a + 12) != uint32_t(g_key)) {
      // A+0x0C, +0x14, +0x24: the current record id (all three = the era's first record after an era load, run 031).
      const int ok = g_host->write32(a + 12, uint32_t(g_key)) && g_host->write32(a + 20, uint32_t(g_key)) &&
                     g_host->write32(a + 36, uint32_t(g_key));
      Line("jump: A+0C/+14/+24 = %d (%s)", g_key, ok ? "ok" : "FAILED");
    }
    return;
  }
  if (addr == kSetRecord) {
    const uint32_t args = uint32_t(g_host->get_gpr(ctx, 5)), v = R32(args + 4);
    Line("set record op: args %08X record %d from %08X frame %llu", args, int(v), g_host->get_lr(ctx),
         static_cast<unsigned long long>(g_host->frame()));
    if (g_step < 0 && g_key >= 0 && int(v) != g_key) {
      const int ok = g_host->write32(args + 4, uint32_t(g_key));
      Line("jump: script record %d -> %d (%s)", int(v), g_key, ok ? "ok" : "FAILED");
    }
    return;
  }
  if (addr == kLoadEra) {
    Line("load era: obj %08X era %u r5 %u (cached era %u) from %08X frame %llu", uint32_t(g_host->get_gpr(ctx, 3)),
         uint32_t(g_host->get_gpr(ctx, 4)), uint32_t(g_host->get_gpr(ctx, 5)), R32(uint32_t(g_host->get_gpr(ctx, 3)) + 296),
         g_host->get_lr(ctx), static_cast<unsigned long long>(g_host->frame()));
    if (g_era >= 0 && g_era <= 4) {
      g_host->set_gpr(ctx, 4, uint64_t(g_era));
      const uint32_t sel = R32(R32(kModeState) + 4) + 2344;
      Line("  selection [S+4]+2344 = %08X: %08X %08X %08X %08X %08X %08X %08X %08X", sel, R32(sel), R32(sel + 4),
           R32(sel + 8), R32(sel + 12), R32(sel + 16), R32(sel + 20), R32(sel + 24), R32(sel + 28));
      const uint32_t a = R32(R32(kModeState) + 8);
      Line("  A at the era load: +08 %08X +0C %08X +10 %08X +14 %08X +24 %08X", R32(a + 8), R32(a + 12), R32(a + 16),
           R32(a + 20), R32(a + 36));
      // One shot: the game then steps the code itself (10100 -> 10101 between two calls, run 033).
      static bool done = false;
      const int ok = !done && g_step >= 0 && g_host->write32(sel + 4, uint32_t(g_era)) &&
                     g_host->write32(sel + 8, uint32_t(g_step));
      if (ok) done = true;
      Line("jump: era -> %d, selection +4 = %d, +8 (moment code) = %d (%s)", g_era, g_era, g_step,
           ok ? "ok" : "not written");
    }
    return;
  }
  if (addr != kLookup) return;
  const uint32_t kind = uint32_t(g_host->get_gpr(ctx, 4)), key = uint32_t(g_host->get_gpr(ctx, 5)),
                 lr = g_host->get_lr(ctx);
  if (g_seen.insert({lr, kind << 16 | (key & 0xFFFF)}).second && g_seen.size() < 300 && kind != 4)
    Line("lookup obj %08X kind %u key %d field %u from %08X (frame %llu)", uint32_t(g_host->get_gpr(ctx, 3)), kind,
         int(key), uint32_t(g_host->get_gpr(ctx, 6)), lr, static_cast<unsigned long long>(g_host->frame()));
}

__declspec(dllexport) void wwe2k14_probe_leave(void*, uint32_t) {}

__declspec(dllexport) void wwe2k14_probe_shutdown(void) {}

}  // extern "C"
