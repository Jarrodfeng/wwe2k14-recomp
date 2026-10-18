// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and
// `plugin reload` in the running coverage-build game. API: probes/probe_api.h.
//
// Current probe: the text manager's GetString (sub_822563F8, r4 = text id -> r3 = the string; docs/systems/menus.md):
// logs each distinct id once with its text. Change kPrefix / the filter below and reload to see a probe change live.

#include "probe_api.h"

#include <cstdio>
#include <mutex>
#include <set>
#include <unordered_map>

namespace {

const Wwe2k14ProbeHost* g_host = nullptr;
constexpr const char* kPrefix = "GetString";
constexpr uint32_t kGetString = 0x822563F8;

std::mutex g_mutex;
std::set<uint32_t> g_seen;                        // ids already logged
std::unordered_map<uint32_t, uint32_t> g_id_of;   // host thread -> the id of its GetString call in flight

}  // namespace

extern "C" {

__declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  g_host = host;
  std::lock_guard lock(g_mutex);
  g_seen.clear();
  g_id_of.clear();
  return WWE2K14_PROBE_API_VERSION;
}

__declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  if (cap < 1) return 0;
  out[0] = kGetString;
  return 1;
}

__declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) {
  if (addr != kGetString) return;
  const uint32_t id = uint32_t(g_host->get_gpr(ctx, 4));
  std::lock_guard lock(g_mutex);
  g_id_of[g_host->thread_id()] = id;
}

__declspec(dllexport) void wwe2k14_probe_leave(void* ctx, uint32_t addr) {
  if (addr != kGetString) return;
  uint32_t id = 0;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_id_of.find(g_host->thread_id());
    if (it == g_id_of.end()) return;
    id = it->second;
    g_id_of.erase(it);
    if (!g_seen.insert(id).second) return;  // each id once
  }
  char text[96] = {};
  const uint32_t str = uint32_t(g_host->get_gpr(ctx, 3));
  if (!g_host->read_string(str, text, sizeof(text))) std::snprintf(text, sizeof(text), "(unreadable 0x%08X)", str);
  char line[160];
  std::snprintf(line, sizeof(line), "%s 0x%04X = '%s' (frame %llu)", kPrefix, id, text,
                static_cast<unsigned long long>(g_host->frame()));
  g_host->log(line);
}

__declspec(dllexport) void wwe2k14_probe_shutdown(void) {}

}  // extern "C"
