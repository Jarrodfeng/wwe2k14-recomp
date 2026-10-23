// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and
// `plugin reload` in the running coverage-build game. API: probes/probe_api.h.
//
// Current probe (Session 50, step 2: the engine's render commands, docs\native\SCENE_MAP.md): the render thread's
// command interpreter 0x82BB53D0 (r3 = the CRenderCmdManager, r4 = a pointer to the record; the record's first word
// is the opcode - listing 0x82BB53E4-0x82BB53F0) and, on the recording side, the op-15 / op-19 recorders 0x82BB10C0 /
// 0x82C1B350 and the model-part draw 0x82BDED40 (their callers by return address). Logs every 60 guest frames:
//   ops: per frame by opcode, and per recording list (the replay loop 0x82BB5BE8 walks [mgr+132] lists, 68-byte
//        descriptors at [mgr+108]: start slot +16, end slot +12; the cursor at mgr+44 - listing 0x82BB5C0C-0x82BB5C24);
//   rec15 / rec19 / part: callers (return addresses) per frame.
// Once (the 5th window): the whole frame's op sequence, run-length coded, with the list it came from ("L<i>:").
// The Session 48 no-GPU probe is in git history (commit e59dbed and before).

#include "probe_api.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

const Wwe2k14ProbeHost* g_host = nullptr;
constexpr uint32_t kInterp = 0x82BB53D0;
constexpr uint32_t kRec15 = 0x82BB10C0;
constexpr uint32_t kRec19 = 0x82C1B350;
constexpr uint32_t kPart = 0x82BDED40;

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

// Render thread (the interpreter) only.
uint64_t g_frame = UINT64_MAX, g_window_first = 0;
uint32_t g_window_frames = 0, g_windows = 0;
uint64_t g_ops[128] = {};
uint64_t g_list_records[16] = {};
uint64_t g_list_draws[16] = {};  // ops 15 / 19 / 68 per list
bool g_capture = false, g_captured = false;
std::vector<std::pair<uint32_t, uint32_t>> g_seq;  // (list << 8 | op, run length)

// Recording side (game threads).
std::mutex g_rec_mutex;
std::unordered_map<uint64_t, uint64_t> g_callers;  // (hook << 32 | return address) -> calls

void FlushWindow() {
  if (!g_window_frames) return;
  const double n = double(g_window_frames);
  std::string ops;
  for (uint32_t op = 0; op < 128; ++op) {
    if (g_ops[op]) {
      char b[32];
      std::snprintf(b, sizeof(b), " %u:%.1f", op, g_ops[op] / n);
      ops += b;
    }
  }
  Log("[s50][ops] frames %llu-%llu, records per frame by op:%s", (unsigned long long)g_window_first,
      (unsigned long long)g_frame, ops.c_str());
  std::string lists;
  for (uint32_t i = 0; i < 16; ++i) {
    if (g_list_records[i]) {
      char b[48];
      std::snprintf(b, sizeof(b), " L%u %.1f (draws %.1f)", i, g_list_records[i] / n, g_list_draws[i] / n);
      lists += b;
    }
  }
  Log("[s50][ops] records per frame by list:%s", lists.c_str());
  std::vector<std::pair<uint64_t, uint64_t>> callers;
  {
    std::lock_guard<std::mutex> lock(g_rec_mutex);
    callers.assign(g_callers.begin(), g_callers.end());
    g_callers.clear();
  }
  std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  for (uint32_t hook : {kRec15, kRec19, kPart}) {
    std::string line;
    uint32_t k = 0;
    for (const auto& c : callers) {
      if (uint32_t(c.first >> 32) != hook || k >= 24) continue;
      char b[40];
      std::snprintf(b, sizeof(b), " %08X:%.1f", uint32_t(c.first), c.second / n);
      line += b;
      ++k;
    }
    Log("[s50][rec] %s callers per frame:%s", hook == kRec15 ? "op-15 recorder" : hook == kRec19 ? "op-19 recorder"
                                                                                    : "model part draw",
        line.empty() ? " none" : line.c_str());
  }
  std::memset(g_ops, 0, sizeof(g_ops));
  std::memset(g_list_records, 0, sizeof(g_list_records));
  std::memset(g_list_draws, 0, sizeof(g_list_draws));
  g_window_frames = 0;
  ++g_windows;
  g_capture = !g_captured && g_windows == 5;
}

void PrintSequence() {
  std::string line;
  uint32_t part = 0;
  for (const auto& s : g_seq) {
    char b[32];
    std::snprintf(b, sizeof(b), " L%u:%u", s.first >> 8, s.first & 0xFF);
    line += b;
    if (s.second > 1) {
      std::snprintf(b, sizeof(b), "x%u", s.second);
      line += b;
    }
    if (line.size() > 900) {
      Log("[s50][seq] frame %llu part %u:%s", (unsigned long long)g_frame, part++, line.c_str());
      line.clear();
    }
  }
  if (!line.empty()) Log("[s50][seq] frame %llu part %u:%s", (unsigned long long)g_frame, part, line.c_str());
  Log("[s50][seq] frame %llu: %zu runs", (unsigned long long)g_frame, g_seq.size());
}

void OnInterp(void* ctx) {
  const uint64_t frame = g_host->frame();
  if (frame != g_frame) {
    if (g_frame != UINT64_MAX) {
      ++g_window_frames;
      if (g_capture) {
        PrintSequence();
        g_capture = false;
        g_captured = true;
        g_seq.clear();
      }
      if (g_window_frames >= 60) FlushWindow();
    }
    if (!g_window_frames) g_window_first = frame;
    g_frame = frame;
  }
  const uint32_t mgr = uint32_t(g_host->get_gpr(ctx, 3));
  const uint32_t rec = R32(uint32_t(g_host->get_gpr(ctx, 4)));
  const uint32_t op = R32(rec) & 127;
  ++g_ops[op];
  // Which list: the cursor (mgr+44) within a descriptor's [start, end) slots.
  uint32_t list = 15;
  const uint32_t lists = std::min<uint32_t>(R32(mgr + 132), 15);
  const uint32_t desc = R32(mgr + 108), cursor = R32(mgr + 44);
  for (uint32_t i = 0; i < lists; ++i) {
    const uint32_t start = R32(desc + 68 * i + 16), end = R32(desc + 68 * i + 12);
    if (cursor >= start && cursor < end) {
      list = i;
      break;
    }
  }
  ++g_list_records[list];
  if (op == 15 || op == 19 || op == 68) ++g_list_draws[list];
  if (g_capture) {
    const uint32_t key = list << 8 | op;
    if (!g_seq.empty() && g_seq.back().first == key) {
      ++g_seq.back().second;
    } else if (g_seq.size() < 20000) {
      g_seq.emplace_back(key, 1);
    }
  }
}

}  // namespace

extern "C" __declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  g_host = host;
  Log("[s50][ops] probe loaded: interpreter 0x%08X, recorders 0x%08X / 0x%08X, model part 0x%08X", kInterp, kRec15,
      kRec19, kPart);
  return WWE2K14_PROBE_API_VERSION;
}

extern "C" __declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  const uint32_t fns[] = {kInterp, kRec15, kRec19, kPart};
  int n = 0;
  for (uint32_t f : fns) {
    if (n < cap) out[n++] = f;
  }
  return n;
}

extern "C" __declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) {
  if (addr == kInterp) {
    OnInterp(ctx);
    return;
  }
  const uint64_t key = uint64_t(addr) << 32 | g_host->get_lr(ctx);
  std::lock_guard<std::mutex> lock(g_rec_mutex);
  ++g_callers[key];
}

extern "C" __declspec(dllexport) void wwe2k14_probe_shutdown(void) {}
