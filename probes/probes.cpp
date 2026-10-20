// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and
// `plugin reload` in the running coverage-build game. API: probes/probe_api.h.
//
// Current probe (Session 36, true freeze): the per-frame task runner sub_82C302A0(owner) (CGameMainThread, every frame).
//   owner +0 / +8: a list of global objects (vtable slots 1 = before, 2 = after, 3 = late);
//   owner +32 / +40: task groups, 16 bytes each (+0 first task, +8 end); per group, the sequence manager [owner+68]
//   vtable slot 4 (r4 = group index) says whether the group runs; then each task with [task+8] != 0 gets its slot 1.
// Command file out\freeze\probe_cmd.txt, read every 30 calls:
//   "dump N"          dump the owner, its globals and every group's tasks once (N: a new number dumps again)
//   "skip MASK"       hex bitmask of groups whose tasks are switched off for the call (their +8 cleared on enter,
//                     put back on leave), 0 = none
//   "skipt A,B,..."   also switch off single tasks by address (hex)
// The sequence manager's slot 4 results per group are logged when they change.
// The Session 34 wm30 probe is in git history (commit 0607a24).

#include "probe_api.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace {

const Wwe2k14ProbeHost* g_host = nullptr;
constexpr uint32_t kRunner = 0x82C302A0;
constexpr uint32_t kGate = 0x824F5C38;  // the sequence manager's slot 4 (in game): run group r4?
constexpr uint64_t kProtected = 1ull << 14;  // group 14 holds 0x82C3B908 (frame sync): skipping it hung run 034

std::mutex g_mutex;
int g_dump_id = -1, g_dumped = -1;
uint64_t g_skip_mask = 0;
std::set<uint32_t> g_skip_tasks;
std::set<uint32_t> g_skip_fns;  // "skipf A,B,...": tasks whose update (vtable slot 1) is one of these
uint64_t g_calls = 0;
uint64_t g_gate_mask = 0;          // "gate MASK": the gate answers 0 for these groups
uint64_t g_allowed = 0, g_allowed_last = ~0ull, g_seen_groups = 0;
thread_local uint32_t t_gate_group = 0;
// "dt S [MS]": while set, the timing block's step fields say S seconds per update (+0x1C, +0x24 = 1000 S, +0x3C = 60 S)
// and, if given, MS whole ms (+0x28); the game's values are saved when it is set and put back when it is cleared.
constexpr uint32_t kTiming = 0x8392DC38;
float g_dt = -1.0f;
int g_dt_ms = -1;
bool g_dt_saved = false;
uint32_t g_saved_1c = 0, g_saved_24 = 0, g_saved_28 = 0, g_saved_3c = 0;
uint32_t F2U(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
std::vector<std::pair<uint32_t, uint32_t>> g_restore;  // task, old +8

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

void ReadCmd() {
  std::FILE* f = std::fopen("C:\\Users\\Jarrod\\wwe2k14-recomp\\out\\freeze\\probe_cmd.txt", "r");
  if (!f) return;
  char line[512];
  uint64_t mask = 0;
  std::set<uint32_t> tasks, fns;
  float dt = -1.0f;
  int dt_ms = -1;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "dump ", 5) == 0) g_dump_id = std::atoi(line + 5);
    if (std::strncmp(line, "dt ", 3) == 0) {
      char* end;
      dt = std::strtof(line + 3, &end);
      dt_ms = int(std::strtol(end, &end, 10));
      if (dt_ms == 0 && !std::strchr(line + 3, ' ')) dt_ms = -1;
    }
    if (std::strncmp(line, "gate ", 5) == 0) g_gate_mask = uint64_t(std::strtoull(line + 5, nullptr, 16)) & ~kProtected;
    if (std::strncmp(line, "skip ", 5) == 0) mask = uint64_t(std::strtoull(line + 5, nullptr, 16));
    if (std::strncmp(line, "skipt ", 6) == 0 || std::strncmp(line, "skipf ", 6) == 0) {
      auto& dst = line[4] == 'f' ? fns : tasks;
      char* p = line + 6;
      while (*p) {
        char* end;
        const uint32_t a = uint32_t(std::strtoul(p, &end, 16));
        if (end == p) break;
        if (a) dst.insert(a);
        p = end;
        while (*p == ',' || *p == ' ') ++p;
      }
    }
  }
  std::fclose(f);
  if (dt != g_dt || dt_ms != g_dt_ms) Line("dt %g ms %d -> %g ms %d", g_dt, g_dt_ms, dt, dt_ms);
  g_dt = dt;
  g_dt_ms = dt_ms;
  if (fns != g_skip_fns) Line("skip functions: %zu", fns.size());
  g_skip_fns = fns;
  if (mask != g_skip_mask || tasks != g_skip_tasks) {
    Line("skip mask %llX -> %llX, %zu single tasks, frame %llu", (unsigned long long)g_skip_mask, (unsigned long long)mask, tasks.size(),
         static_cast<unsigned long long>(g_host->frame()));
  }
  g_skip_mask = mask & ~kProtected;
  g_skip_tasks = tasks;
}

void Dump(uint32_t owner) {
  const uint32_t mgr = R32(owner + 68), mvt = R32(mgr);
  Line("dump %d at frame %llu: owner %08X mgr %08X vt %08X slots 1 %08X 2 %08X 3 %08X 4 %08X; mgr+0 (seq) %08X vt %08X",
       g_dump_id, static_cast<unsigned long long>(g_host->frame()), owner, mgr, mvt, R32(mvt + 4), R32(mvt + 8),
       R32(mvt + 12), R32(mvt + 16), R32(mgr), R32(R32(mgr)));
  for (uint32_t p = R32(owner + 0), e = R32(owner + 8), i = 0; p < e && i < 64; p += 4, ++i) {
    const uint32_t o = R32(p), vt = R32(o);
    Line("  global %u: %08X vt %08X slots 1 %08X 2 %08X 3 %08X", i, o, vt, R32(vt + 4), R32(vt + 8), R32(vt + 12));
  }
  const uint32_t g0 = R32(owner + 32), g1 = R32(owner + 40);
  Line("  groups %u (owner +80 %08X +88 %08X)", (g1 - g0) / 16, R32(owner + 80), R32(owner + 88));
  for (uint32_t g = g0, gi = 0; g < g1 && gi < 64; g += 16, ++gi) {
    const uint32_t b = R32(g), e = R32(g + 8);
    Line("  group %u: %u tasks (%08X %08X %08X %08X)", gi, (e - b) / 4, R32(g), R32(g + 4), R32(g + 8), R32(g + 12));
    for (uint32_t p = b, i = 0; p < e && i < 128; p += 4, ++i) {
      const uint32_t t = R32(p);
      if (t == 0xFFFFFFFFu) {
        Line("    task %u: -1", i);
        continue;
      }
      const uint32_t vt = R32(t);
      char name[48] = {};
      g_host->read_string(R32(t + 12), name, sizeof(name));
      Line("    task %u: %08X vt %08X upd %08X on %08X | %08X %08X %08X %08X %08X '%s'", i, t, vt, R32(vt + 4),
           R32(t + 8), R32(t + 4), R32(t + 12), R32(t + 16), R32(t + 20), R32(t + 24), name);
    }
  }
}

}  // namespace

extern "C" {

__declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  g_host = host;
  std::lock_guard lock(g_mutex);
  g_dumped = -1;
  ReadCmd();
  g_dumped = g_dump_id;  // a reload does not repeat the file's dump; bump N to dump
  Line("freeze probe loaded: skip mask %llX, dump id %d", (unsigned long long)g_skip_mask, g_dump_id);
  return WWE2K14_PROBE_API_VERSION;
}

__declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  if (cap < 2) return 0;
  out[0] = kRunner;
  out[1] = kGate;
  return 2;
}

__declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) {
  if (addr == kGate) {
    t_gate_group = uint32_t(g_host->get_gpr(ctx, 4));
    return;
  }
  if (addr != kRunner) return;
  std::lock_guard lock(g_mutex);
  if (g_seen_groups && g_allowed != g_allowed_last) {
    Line("gate: groups run %llX (of %llX) at frame %llu (gate mask %llX)", (unsigned long long)g_allowed,
         (unsigned long long)g_seen_groups, static_cast<unsigned long long>(g_host->frame()),
         (unsigned long long)g_gate_mask);
    g_allowed_last = g_allowed;
  }
  g_allowed = g_seen_groups = 0;
  const uint32_t owner = uint32_t(g_host->get_gpr(ctx, 3));
  if (++g_calls % 30 == 0) ReadCmd();
  if (g_dump_id != g_dumped) {
    g_dumped = g_dump_id;
    Dump(owner);
  }
  g_restore.clear();
  if (g_dt >= 0.0f) {
    if (!g_dt_saved) {
      g_saved_1c = R32(kTiming + 0x1C), g_saved_24 = R32(kTiming + 0x24), g_saved_28 = R32(kTiming + 0x28),
      g_saved_3c = R32(kTiming + 0x3C);
      g_dt_saved = true;
      Line("dt: saved %08X %08X %u %08X", g_saved_1c, g_saved_24, g_saved_28, g_saved_3c);
    }
    g_host->write32(kTiming + 0x1C, F2U(g_dt));
    g_host->write32(kTiming + 0x24, F2U(g_dt * 1000.0f));
    g_host->write32(kTiming + 0x3C, F2U(g_dt * 60.0f));
    if (g_dt_ms >= 0) g_host->write32(kTiming + 0x28, uint32_t(g_dt_ms));
  } else if (g_dt_saved) {
    g_host->write32(kTiming + 0x1C, g_saved_1c);
    g_host->write32(kTiming + 0x24, g_saved_24);
    g_host->write32(kTiming + 0x28, g_saved_28);
    g_host->write32(kTiming + 0x3C, g_saved_3c);
    g_dt_saved = false;
    Line("dt: restored");
  }
  if (!g_skip_mask && g_skip_tasks.empty() && g_skip_fns.empty()) return;
  const uint32_t g0 = R32(owner + 32), g1 = R32(owner + 40);
  for (uint32_t g = g0, gi = 0; g < g1 && gi < 64; g += 16, ++gi) {
    const bool whole = (g_skip_mask >> gi) & 1;
    for (uint32_t p = R32(g), e = R32(g + 8); p < e; p += 4) {
      const uint32_t t = R32(p);
      if (t == 0xFFFFFFFFu || !t) continue;
      if (!whole && !g_skip_tasks.count(t) && !g_skip_fns.count(R32(R32(t) + 4))) continue;
      const uint32_t on = R32(t + 8);
      if (!on) continue;
      g_restore.push_back({t, on});
      g_host->write32(t + 8, 0);
    }
  }
}

__declspec(dllexport) void wwe2k14_probe_leave(void* ctx, uint32_t addr) {
  if (addr == kGate) {
    const uint32_t gi = t_gate_group;
    if (gi >= 64) return;
    if ((g_gate_mask >> gi) & 1) g_host->set_gpr(ctx, 3, 0);
    g_seen_groups |= 1ull << gi;
    if (uint32_t(g_host->get_gpr(ctx, 3))) g_allowed |= 1ull << gi;
    return;
  }
  if (addr != kRunner) return;
  std::lock_guard lock(g_mutex);
  for (auto& [t, on] : g_restore) g_host->write32(t + 8, on);
  g_restore.clear();
}

__declspec(dllexport) void wwe2k14_probe_shutdown(void) {
  std::lock_guard lock(g_mutex);
  for (auto& [t, on] : g_restore) g_host->write32(t + 8, on);
  g_restore.clear();
}

}  // extern "C"
