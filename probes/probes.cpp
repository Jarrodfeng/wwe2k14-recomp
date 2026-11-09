// wwe2k14 - the probe plugin (Session 30, #4): edit, rebuild (pwsh -File tools\probes\build_probes.ps1) and
// `plugin reload` in the running coverage-build game. API: probes/probe_api.h.
//
// Current probe: Session 98, Superstar Importer diagnostic run 2 (the run-1 probe, tag imp1 -> imp2, plus: requests for
// SSF* / EMD / IGST paths, the whole live record of id 397 at the first 16 record dumps (`rec397`), the gate table
// read-only (our build writes it: --wwe2k14_dlc_gate_extra), and Q6 the select grid's walk sub_8282C648 logged with a
// one-shot column steer toward the DOWNLOADED tab (`walk-in` / `walk-out` / `steer`). out\s98\importer\run2\prep.md
// (local) has the questions. Run 1's version: git history (75475721). Earlier probes are in git history (the S67 list /
// text probe: probes.cpp at 03a39fee; the S67 label probe: b218776). Every line starts "[wwe2k14][probe] imp2 <tag>";
// "chain" = the guest return addresses: the caller (lr)
// first, then the guest back-chain walk (360 convention: a non-leaf function saves its return address at -8 from its
// caller's stack pointer; best effort), resolve with py tools\analysis\resolve_probe_chain.py LOG.
//
//   Q1 who builds DLC file names (CH385_DLC04.PAC has no "_dlc" literal in the image):
//     path   sub_8281C228 = content manager slot 17, snprintf(buf, size, "dlc%d:\%s", r4 index, r7 name): every call,
//            the name (and the 2 words before it: a catalog file record holds s16 a, s16 b at -8), result, chain
//     req    sub_82C37E48 = pac loader request (path string at [r4]): paths containing DLC / SSFD / CHAR, chain
//     fmt    sub_82C34EF8 = snprintf(buf, size, fmt, ...): outputs containing "dlc" (not the two known content formats)
//     cpy    sub_82C34588 = strncpy-like (dst, size, src): outputs containing "_dlc"
//     find   sub_8281F080 = catalog file-name binary search (r4 name): name and result
//     parse  sub_8281F150 = catalog parser: result (0 = pack dropped)
//   Q3 roster records [0x8376F3C0] + 204*(id-100) for ids 102 / 385 / 397 / 499 (+0 id, +2, +6 name text, +16 active,
//     +17 pack code): `rec` at every record write (sub_827D8EF0) to those slots, `recs` dumps after the chEtc reader
//     (sub_8342B740), after the save block A copy (sub_827D8E30) and at each roster list build.
//   Q4 roster lists: `list` sub_82927390 (menu, lists, kind, flag) and `split` sub_82924E00: the 6 lists (count + ids)
//     after each; `ssfd` CSuperStarReader ctor sub_8293E720 / ReadEnd sub_8293E440 (args, filled portrait slots);
//     `avail` sub_82817598 (container, char id) -> result, each distinct (id, result) once.
//   Q5 the pack-id gate table 0x82035E9C (5 words): `gate` at the first catalog parse (read only in run 2).

#include "probe_api.h"

#include <windows.h>

#include <atomic>
#include <cctype>
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

constexpr uint32_t kPathBuilder = 0x8281C228, kRequest = 0x82C37E48, kSprintf = 0x82C34EF8, kStrncpy = 0x82C34588,
                   kCatFind = 0x8281F080, kCatParse = 0x8281F150, kRecWrite = 0x827D8EF0, kBlockA = 0x827D8E30,
                   kSkillEnd = 0x8342B740, kListBuild = 0x82927390, kListSplit = 0x82924E00, kSsfdCtor = 0x8293E720,
                   kSsfdEnd = 0x8293E440, kAvail = 0x82817598, kGridWalk = 0x8282C648;
constexpr uint32_t kFmtDlcPath = 0x82035C98, kFmtDlcRoot = 0x82035C90;  // "dlc%d:\%s", "dlc%d"
constexpr uint32_t kRosterBase = 0x8376F3C0, kGateTable = 0x82035E9C;
constexpr int kIds[4] = {102, 385, 397, 499};

std::mutex g_mutex;
std::atomic<int> g_lines_path{0}, g_lines_req{0}, g_lines_fmt{0}, g_lines_cpy{0}, g_lines_find{0}, g_lines_avail{0},
    g_lines_rec{0}, g_lines_walk{0};
std::atomic<uint32_t> g_steered{0};  // bit per player index: the one-shot column steer is done (Q6)
struct Parked {
  int col = 0, row = 0, budget = 0;
  bool done = false;
  unsigned long long until = 0;  // guest frame: parking lapses 45 frames after its last use (the list's own DOWNs
                                 // come later)
};
Parked g_parked[6];  // per player: the DOWNLOADED tab cell once reached (under g_mutex)
std::atomic<int> g_full397{0};
std::atomic<bool> g_gate_done{false};
std::set<std::pair<uint32_t, uint32_t>> g_avail_seen;  // (id, result)
std::map<uint32_t, uint32_t> g_rec_callers;            // record-write caller -> count since the last dump

struct Frame {
  uint32_t addr, a, b, c, lr;
  uint32_t player = 0, down = 0;  // run 2: the grid walk
  int scol = 0;
};
thread_local std::vector<Frame> t_frames;

void Log(const char* fmt, ...) {
  char buf[4096];
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
uint32_t R16(uint32_t a) {
  uint16_t v = 0;
  g_host->read16(a, &v);
  return v;
}
uint32_t R8(uint32_t a) {
  uint8_t v = 0;
  g_host->read8(a, &v);
  return v;
}
std::string Str(uint32_t a, uint32_t cap = 160) {
  if (!a) return "(null)";
  char s[260] = {};
  if (!g_host->read_string(a, s, cap < sizeof(s) ? cap : sizeof(s))) return "(unreadable)";
  return s;
}
bool HasCi(const std::string& s, const char* needle) {
  std::string u = s, n = needle;
  for (char& c : u) c = char(std::tolower((unsigned char)c));
  for (char& c : n) c = char(std::tolower((unsigned char)c));
  return u.find(n) != std::string::npos;
}
bool Take(std::atomic<int>& counter, int cap) { return counter.fetch_add(1) < cap; }

// The caller (lr) then the guest back chain from r1 (the caller's frame at function entry).
std::string Chain(void* ctx, int depth = 14) {
  char out[400];
  int n = std::snprintf(out, sizeof(out), "%08X", g_host->get_lr(ctx));
  uint32_t sp = uint32_t(g_host->get_gpr(ctx, 1));
  for (int i = 0; i < depth && n < int(sizeof(out)) - 12; ++i) {
    uint32_t top = 0, ret = 0;
    if (!g_host->read32(sp, &top) || top <= sp || top - sp > 0x100000 || (top & 3)) break;
    if (!g_host->read32(top - 8, &ret)) break;
    n += std::snprintf(out + n, sizeof(out) - n, " %08X", ret);
    sp = top;
  }
  return out;
}

unsigned long long Fr() { return (unsigned long long)g_host->frame(); }

void Push(uint32_t addr, uint32_t a, uint32_t b, uint32_t c, uint32_t lr) { t_frames.push_back({addr, a, b, c, lr}); }
bool Pop(uint32_t addr, Frame* f) {
  for (size_t i = t_frames.size(); i-- > 0;) {
    if (t_frames[i].addr != addr) continue;
    *f = t_frames[i];
    t_frames.resize(i);  // frames above it were skipped (longjmp etc.): drop them too
    return true;
  }
  return false;
}

void DumpIds(const char* when, uint32_t lr) {
  const uint32_t base = R32(kRosterBase);
  char line[1024];
  int n = std::snprintf(line, sizeof(line), "imp2 recs %s frame %llu lr %08X base %08X:", when, Fr(), lr, base);
  for (int id : kIds) {
    const uint32_t r = base + 204u * uint32_t(id - 100);
    n += std::snprintf(line + n, sizeof(line) - n, " [%d: +0=%u +2=%u +6=%u +16=%u +17=%u]", id, R16(r), R16(r + 2),
                       R16(r + 6), R8(r + 16), R8(r + 17));
  }
  std::string callers;
  {
    std::lock_guard lock(g_mutex);
    for (auto& [c, k] : g_rec_callers) {
      char b[40];
      std::snprintf(b, sizeof(b), " %08X x%u", c, k);
      callers += b;
    }
    g_rec_callers.clear();
  }
  Log("%s; record writes since the last dump by caller:%s", line, callers.empty() ? " none" : callers.c_str());
  // Run 2: the mod character's whole live record (204 bytes, guest order) at the first 16 dumps.
  if (g_full397.fetch_add(1) < 16) {
    char hex[204 * 2 + 1] = {};
    const uint32_t r = base + 204u * (397 - 100);
    for (uint32_t i = 0; i < 204; ++i) std::snprintf(hex + 2 * i, 3, "%02X", R8(r + i));
    Log("imp2 rec397 %s frame %llu @%08X: %s", when, Fr(), r, hex);
  }
}

// One list of the select screen's list array: u32 count + up to 1000 u32 entries (stride 4004).
void DumpLists(const char* tag, uint32_t arr) {
  for (int k = 0; k < 6; ++k) {
    const uint32_t lst = arr + 4004u * uint32_t(k), count = R32(lst);
    if (count > 1000) {
      Log("imp2 %s list%d @%08X count %u (not a list?)", tag, k, lst, count);
      continue;
    }
    std::string ids;
    int prev = -10, run_start = -10;
    auto flush = [&](int last) {
      if (run_start < 0) return;
      char b[32];
      if (last == run_start) std::snprintf(b, sizeof(b), " %d", run_start);
      else std::snprintf(b, sizeof(b), " %d-%d", run_start, last);
      ids += b;
    };
    std::string marked;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = R32(lst + 4 + 4 * i);
      if (v & 0x80000000u) {  // top-bit marker: list it apart
        char b[24];
        std::snprintf(b, sizeof(b), " %u*", v & 0x7FFFFFFFu);
        marked += b;
      }
      const int id = int(v & 0xFFFF);
      if (id == prev + 1) {
        prev = id;
        continue;
      }
      flush(prev);
      run_start = prev = id;
    }
    flush(prev);
    if (ids.size() > 3000) ids.resize(3000);
    Log("imp2 %s list%d @%08X count %u frame %llu: order-runs%s%s%s", tag, k, lst, count, Fr(), ids.c_str(),
        marked.empty() ? "" : " | top-bit:", marked.c_str());
  }
}

// Run 2: the gate table as the parser sees it at the first parse (word 0 = 00060001 once --wwe2k14_dlc_gate_extra=6
// wrote it; read only: the run-1 write test is done).
void GateTest() {
  if (g_gate_done.exchange(true)) return;
  uint32_t w[5];
  for (int i = 0; i < 5; ++i) w[i] = R32(kGateTable + 4u * uint32_t(i));
  Log("imp2 gate frame %llu table %08X: %08X %08X %08X %08X %08X (word 0 = 00060001 expected with the switch)", Fr(),
      kGateTable, w[0], w[1], w[2], w[3], w[4]);
}

// Q6 (run 2): the select grid's walk sub_8282C648 (listing): r3 model, r4 player (0..5), r5 / r6 = four direction words
// stored at +320..+332 of its frame (`std r5,320` / `std r6,328`; +320 = UP, +324 = DOWN: a start on row 5 with UP
// goes to row 4, with DOWN to row 0, 0x8282C700-0x8282C728), r7 / r8 = &column / &row: read at entry, zeroed, the result
// stored at the end together with the model's copy at model + 0x1B14C / 0x1B150 + 32 * player (0x8282CAEC-0x8282CB08).
// Logged per call. Test-only steer (DOWN walks only, per player): the first DOWN result not in column 4 is moved to
// column 4 (both copies); in column 4 a result in column 3 can only be the tab row's snap (columns 3-5 -> 3 =
// DOWNLOADED, mouse_menu.h kTabs), so that cell is "parked" and the next 6 DOWN walks are kept on it; the route then
// presses A to open the tab. Logged as "imp2 steer". Set kSteer false to only log.
constexpr bool kSteer = true;

void OnEnter(void* ctx, uint32_t addr) {
  const uint32_t r3 = uint32_t(g_host->get_gpr(ctx, 3)), r4 = uint32_t(g_host->get_gpr(ctx, 4)),
                 r5 = uint32_t(g_host->get_gpr(ctx, 5)), r6 = uint32_t(g_host->get_gpr(ctx, 6)),
                 r7 = uint32_t(g_host->get_gpr(ctx, 7)), lr = g_host->get_lr(ctx);
  switch (addr) {
    case kPathBuilder:
      if (g_lines_path.load() < 800) {
        Push(addr, r4, r5, r7, lr);
        Log("imp2 path-in idx %d name '%s' (@%08X, words before %08X %08X) frame %llu chain %s", int(r4),
            Str(r7).c_str(), r7, R32(r7 - 8), R32(r7 - 4), Fr(), Chain(ctx).c_str());
      }
      break;
    case kRequest: {
      const std::string p = Str(R32(r4));
      if ((HasCi(p, "dlc") || HasCi(p, "ssf") || HasCi(p, "char") || HasCi(p, "emd") || HasCi(p, "igst")) &&
          Take(g_lines_req, 600))
        Log("imp2 req '%s' reader %08X r5 %08X r6 %08X words +4 %08X +8 %08X frame %llu chain %s", p.c_str(), r3, r5,
            r6, R32(r4 + 4), R32(r4 + 8), Fr(), Chain(ctx).c_str());
      break;
    }
    case kSprintf:
      if (r5 != kFmtDlcPath && r5 != kFmtDlcRoot && g_lines_fmt.load() < 300) Push(addr, r3, r5, 0, lr);
      break;
    case kStrncpy:
      if (g_lines_cpy.load() < 200) Push(addr, r3, r5, 0, lr);
      break;
    case kCatFind:
      if (g_lines_find.load() < 600) Push(addr, r3, r4, 0, lr);
      break;
    case kCatParse:
      GateTest();
      Push(addr, r3, r4, 0, lr);
      break;
    case kRecWrite: {
      const int slot = int(r3);
      {
        std::lock_guard lock(g_mutex);
        ++g_rec_callers[lr];
      }
      for (int id : kIds) {
        if (slot != id - 100 || !Take(g_lines_rec, 200)) continue;
        Log("imp2 rec write slot %d (id %d) from src %08X: +0=%u +2=%u +6=%u +16=%u +17=%u; old +0=%u +6=%u +16=%u "
            "+17=%u; frame %llu chain %s",
            slot, id, r4, R16(r4), R16(r4 + 2), R16(r4 + 6), R8(r4 + 16), R8(r4 + 17),
            R16(R32(kRosterBase) + 204u * uint32_t(slot)), R16(R32(kRosterBase) + 204u * uint32_t(slot) + 6),
            R8(R32(kRosterBase) + 204u * uint32_t(slot) + 16), R8(R32(kRosterBase) + 204u * uint32_t(slot) + 17), Fr(),
            Chain(ctx, 6).c_str());
      }
      break;
    }
    case kBlockA:
      Log("imp2 blockA copy src %08X -> base %08X frame %llu chain %s", r3, R32(kRosterBase), Fr(), Chain(ctx).c_str());
      DumpIds("before-blockA", lr);
      Push(addr, r3, 0, 0, lr);
      break;
    case kSkillEnd:
      Log("imp2 chEtc reader end r3 %08X r4 %08X r5 %08X frame %llu chain %s", r3, r4, r5, Fr(), Chain(ctx).c_str());
      Push(addr, r3, 0, 0, lr);
      break;
    case kListBuild:
      Log("imp2 list build menu %08X lists %08X kind %u flag %u frame %llu chain %s", r3, r4, r5, r6, Fr(),
          Chain(ctx, 6).c_str());
      DumpIds("at-list-build", lr);
      Push(addr, r4, r5, r6, lr);
      break;
    case kListSplit:
      Push(addr, r4, r3, 0, lr);
      break;
    case kSsfdCtor:
      Log("imp2 ssfd ctor this %08X r4 %u r5 %08X frame %llu chain %s", r3, r4, r5, Fr(), Chain(ctx, 6).c_str());
      break;
    case kSsfdEnd:
      Log("imp2 ssfd readend this %08X r4 %08X r5 %08X r6 %08X frame %llu chain %s", r3, r4, r5, r6, Fr(),
          Chain(ctx, 6).c_str());
      Push(addr, r3, 0, 0, lr);
      break;
    case kAvail:
      Push(addr, r3, r4, 0, lr);
      break;
    case kGridWalk: {
      const uint32_t r8 = uint32_t(g_host->get_gpr(ctx, 8));
      const uint64_t d1 = g_host->get_gpr(ctx, 5), d2 = g_host->get_gpr(ctx, 6);
      const int col = int(R32(r7)), row = int(R32(r8));
      if (Take(g_lines_walk, 400))
        Log("imp2 walk-in player %u start col %d row %d dirs up %u down %u w3 %u w4 %u model %08X frame %llu lr %08X",
            r4, col, row, uint32_t(d1 >> 32), uint32_t(d1), uint32_t(d2 >> 32), uint32_t(d2), r3, Fr(), lr);
      Push(addr, r7, r8, r3, lr);
      t_frames.back().player = r4, t_frames.back().down = uint32_t(d1) != 0, t_frames.back().scol = col;
      break;
    }
  }
}

void OnLeave(void* ctx, uint32_t addr) {
  Frame f{};
  if (!Pop(addr, &f)) return;
  const uint32_t ret = uint32_t(g_host->get_gpr(ctx, 3));
  switch (addr) {
    case kGridWalk: {
      int col = int(R32(f.a)), row = int(R32(f.b));
      if (Take(g_lines_walk, 400))
        Log("imp2 walk-out player %u -> col %d row %d (r3 %08X) frame %llu", f.player, col, row, ret, Fr());
      if (!kSteer || f.player >= 6 || !f.down) break;
      // The walk's own result copy (listing 0x8282CAEC-0x8282CB08): model + 0x1B14C / 0x1B150 + 32 * player.
      const uint32_t own = f.c + 0x1B14Cu + 32u * f.player;
      const uint32_t bit = 1u << f.player;
      std::lock_guard lock(g_mutex);
      if (g_parked[f.player].budget > 0 && Fr() <= g_parked[f.player].until) {  // on the tab: DOWNs stay there
        --g_parked[f.player].budget;
        g_parked[f.player].until = Fr() + 45;
        g_host->write32(f.a, uint32_t(g_parked[f.player].col));
        g_host->write32(f.b, uint32_t(g_parked[f.player].row));
        g_host->write32(own, uint32_t(g_parked[f.player].col));
        g_host->write32(own + 4, uint32_t(g_parked[f.player].row));
        Log("imp2 steer player %u: kept on the tab (col %d row %d instead of %d / %d), %d more frame %llu", f.player,
            g_parked[f.player].col, g_parked[f.player].row, col, row, g_parked[f.player].budget, Fr());
      } else if (f.scol == 4 && col == 3 && (g_steered.load() & bit) && !g_parked[f.player].done) {
        // In the steered column 4 the only way to column 3 is the tab row's snap (3-5 -> 3 = DOWNLOADED).
        g_parked[f.player] = {col, row, 6, true, Fr() + 45};
        Log("imp2 steer player %u: reached the tab row (col %d row %d): parked for the next 6 DOWN walks frame %llu",
            f.player, col, row, Fr());
      } else if (!(g_steered.load() & bit) && col != 4) {
        g_steered.fetch_or(bit);
        g_host->write32(f.a, 4);
        g_host->write32(own, 4);
        Log("imp2 steer player %u: DOWN result column %d -> 4 (row %d) frame %llu", f.player, col, row, Fr());
      }
      break;
    }
    case kPathBuilder:
      if (Take(g_lines_path, 800)) Log("imp2 path-out idx %d -> '%s' frame %llu", int(f.a), Str(f.b).c_str(), Fr());
      break;
    case kSprintf: {
      const std::string s = Str(f.a);
      if (HasCi(s, "dlc") && Take(g_lines_fmt, 300))
        Log("imp2 fmt '%s' -> '%s' frame %llu lr %08X", Str(f.b, 80).c_str(), s.c_str(), Fr(), f.lr);
      break;
    }
    case kStrncpy: {
      const std::string s = Str(f.a);
      if (HasCi(s, "_dlc") && Take(g_lines_cpy, 200))
        Log("imp2 cpy '%s' (src %08X) frame %llu lr %08X", s.c_str(), f.b, Fr(), f.lr);
      break;
    }
    case kCatFind:
      if (Take(g_lines_find, 600))
        Log("imp2 find cat %08X '%s' -> %08X frame %llu lr %08X", f.a, Str(f.b).c_str(), ret, Fr(), f.lr);
      break;
    case kCatParse:
      Log("imp2 parse %08X -> %u (pack id field sub+136 = %u, revision sub+138 = %u) frame %llu lr %08X", f.a, ret,
          R16(f.a + 136), R16(f.a + 138), Fr(), f.lr);
      break;
    case kBlockA:
      DumpIds("after-blockA", f.lr);
      break;
    case kSkillEnd:
      DumpIds("after-chEtc-reader", f.lr);
      break;
    case kListBuild:
      Log("imp2 list build done kind %u flag %u frame %llu", f.b, f.c, Fr());
      DumpLists("list", f.a);
      break;
    case kListSplit:
      DumpLists("split", f.a);
      break;
    case kSsfdEnd: {
      int filled = 0;
      std::string dlc;
      for (uint32_t k = 0; k < 426; ++k) {
        if (!R32(f.a + 52 + 8 * k)) continue;
        ++filled;
        if (k >= 277) {  // ids 377+ and the extra slots
          char b[16];
          std::snprintf(b, sizeof(b), " %u", k < 400 ? k + 100 : k + 9600);  // keys >= 10000: slot = key - 9600
          dlc += b;
        }
      }
      Log("imp2 ssfd readend done this %08X: %d of 426 slots filled; filled ids >= 377 (slots >= 400 shown as "
          "their key, slot + 9600):%s frame %llu",
          f.a, filled, dlc.empty() ? " none" : dlc.c_str(), Fr());
      break;
    }
    case kAvail: {
      bool fresh;
      {
        std::lock_guard lock(g_mutex);
        fresh = g_avail_seen.insert({f.b, ret}).second;
      }
      if (fresh && Take(g_lines_avail, 600))
        Log("imp2 avail container %08X id %u -> %u frame %llu lr %08X", f.a, f.b, ret, Fr(), f.lr);
      break;
    }
  }
}

}  // namespace

extern "C" {

__declspec(dllexport) int wwe2k14_probe_init(const Wwe2k14ProbeHost* host) {
  g_host = host;
  if (host->version < 2) return 0;  // needs write32 (Q5)
  Log("imp2 Session 98 importer run-2 probe loaded at frame %llu (15 functions, walk steer %s)", Fr(),
      kSteer ? "on" : "off");
  DumpIds("at-plugin-load", 0);
  return WWE2K14_PROBE_API_VERSION;
}

__declspec(dllexport) int wwe2k14_probe_functions(uint32_t* out, int cap) {
  const uint32_t list[] = {kPathBuilder, kRequest,   kSprintf,   kStrncpy,  kCatFind,  kCatParse, kRecWrite,
                           kBlockA,      kSkillEnd,  kListBuild, kListSplit, kSsfdCtor, kSsfdEnd,  kAvail,
                           kGridWalk};
  const int n = int(sizeof(list) / sizeof(list[0]));
  if (cap < n) return 0;
  for (int i = 0; i < n; ++i) out[i] = list[i];
  return n;
}

__declspec(dllexport) void wwe2k14_probe_enter(void* ctx, uint32_t addr) { OnEnter(ctx, addr); }

__declspec(dllexport) void wwe2k14_probe_leave(void* ctx, uint32_t addr) { OnLeave(ctx, addr); }

}  // extern "C"
