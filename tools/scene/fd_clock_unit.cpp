// S103 FD / FDP: the stage-page pass object's clock reader (scene_inputs::ReadStagePageClock, inputs_core.h) over a fake guest built with the REAL task runner layout (docs\systems\task_runner.md, sub_82C2FEB8):
// owner [0x83930050]; [owner+32] = POINTER to a 35-entry array of 16-byte groups, [owner+40] = its end; group g = +0 begin, +4 capacity end, +8 cursor (a registration stores the task pointer at the cursor and adds 4).
// Group 11 holds the page object, found by its vtable slot 1 == sub_825BA448; its t / run / entrance words at +108 / +112 / +88. Compile only, no game. Exit 1 on a failure.
// `--plant stale_cache` trusts the cached pointer without revalidating (a second match's new object is missed); `--plant inline_groups` is the FD first version (the group table inline at owner+208 / +216, no
// pointer): both must fail.
#include <cstdio>
#include <cstring>
#include <map>

#include "../../src/scene_live/inputs_core.h"

namespace si = scene_inputs;
using rdr_scene::StageLookRow;

static std::map<uint32_t, uint32_t> g_mem;
static bool Rd(uint32_t a, void* dst, size_t n) {
  if (a % 4 || n % 4) return false;
  uint32_t* d = static_cast<uint32_t*>(dst);
  for (size_t i = 0; i < n / 4; ++i) {
    auto it = g_mem.find(a + uint32_t(4 * i));
    if (it == g_mem.end()) return false;
    d[i] = si::Swap32(it->second);   // big-endian guest
  }
  return true;
}
static void W(uint32_t a, uint32_t v) { g_mem[a] = v; }
static void WF(uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); g_mem[a] = v; }
static uint32_t R(uint32_t a) { return g_mem[a]; }
static int g_fail = 0;
static void Expect(bool ok, const char* what) { if (!ok) std::printf("FAIL  %s\n", what), ++g_fail; }

static void MakePage(uint32_t obj, float t, uint32_t run, uint32_t ent) {
  W(obj, 0x8201F294u);
  W(obj + 88, ent), WF(obj + 108, t), W(obj + 112, run);
}

// the real layout: the group array at `groups`, group g's task words at tasks + 0x1000 * g (16 slots each, empty at the start)
static constexpr uint32_t kGroups = 35, kSlots = 16;
static void BuildRunner(uint32_t owner, uint32_t groups, uint32_t tasks) {
  W(0x83930050u, owner);
  W(owner + 32, groups), W(owner + 40, groups + 16 * kGroups);
  for (uint32_t g = 0; g < kGroups; ++g) {
    const uint32_t t = tasks + 0x1000 * g;
    W(groups + 16 * g + 0, t), W(groups + 16 * g + 4, t + 4 * kSlots), W(groups + 16 * g + 8, t);
  }
}
// sub_82C2FEB8(owner, group, obj): r10 = [owner+32]; r11 = (group << 4) + r10; if ([r11+4] - [r11+8] >= 4) { [[r11+8]] = obj; [r11+8] += 4 }
static void Register(uint32_t owner, uint32_t group, uint32_t obj) {
  const uint32_t e = (group << 4) + R(owner + 32);
  if (R(e + 4) - R(e + 8) >= 4) { W(R(e + 8), obj); W(e + 8, R(e + 8) + 4); }
}

// the planted twins: `stale_cache` = a copy of the reader without the revalidation of the cached object; `inline_groups` = the group table read inline at owner+208 (the S103 FD first version)
static bool g_plant_stale = false, g_plant_inline = false;
static bool Read(uint32_t& cache, uint32_t& miss, StageLookRow& out) {
  if (g_plant_stale && cache) {
    uint32_t ent = 0, run = 0, tb = 0;
    if (!si::ReadWord(Rd, cache + si::kPageEntranceOff, ent) || !si::ReadWord(Rd, cache + si::kPageClockOff, tb) || !si::ReadWord(Rd, cache + si::kPageRunOff, run)) return false;
    out = StageLookRow();
    out.kind = rdr_scene::kSlPageClock;
    std::memcpy(&out.dir[0], &tb, 4);
    out.flags = (run != 0 ? 1u : 0u) | (ent != 0 ? 2u : 0u);
    return true;
  }
  if (g_plant_inline && !cache) {
    uint32_t owner = 0, b = 0, e = 0;
    if (!si::ReadWord(Rd, si::kTaskOwnerGlobal, owner)) return false;
    const uint32_t g = owner + 32 + 16 * si::kPageClockGroup;
    if (!si::ReadWord(Rd, g, b) || !si::ReadWord(Rd, g + 8, e) || e < b || e - b > 4096 || e == b) return false;
    for (uint32_t p = b; p < e; p += 4) {
      uint32_t t = 0, vt = 0, fn = 0;
      if (si::ReadWord(Rd, p, t) && si::ReadWord(Rd, t, vt) && si::ReadWord(Rd, vt + 4, fn) && fn == si::kPageUpdateFn) { cache = t; break; }
    }
    if (!cache) return false;
  }
  return si::ReadStagePageClock(Rd, cache, miss, out);
}

int main(int argc, char** argv) {
  g_plant_stale = argc == 3 && std::strcmp(argv[1], "--plant") == 0 && std::strcmp(argv[2], "stale_cache") == 0;
  g_plant_inline = argc == 3 && std::strcmp(argv[1], "--plant") == 0 && std::strcmp(argv[2], "inline_groups") == 0;
  const uint32_t owner = 0x40100000u, groups = 0x40600000u, tasks = 0x40700000u, a = 0x40300000u, b = 0x40400000u, other = 0x40500000u, decoy = 0x40510000u;
  W(0x8201F298u, si::kPageUpdateFn);   // vtable 0x8201F294 + 4
  W(0x82010004u, 0x82AAAAAAu);         // some other task's slot 1
  W(other, 0x82010000u);
  BuildRunner(owner, groups, tasks);
  // the old wrong place: owner+208 / +216 hold plausible words pointing at a decoy list holding a real-looking page object (never read as a group)
  const uint32_t dlist = 0x40800000u;
  MakePage(decoy, 77.f, 1, 1);
  W(dlist, decoy);
  W(owner + 208, dlist), W(owner + 216, dlist + 4);
  // decoy page objects registered in the NEIGHBOUR groups 10 and 12 (a different task group's object is never taken)
  MakePage(0x40520000u, 55.f, 1, 0), MakePage(0x40530000u, 66.f, 1, 0);
  Register(owner, 10, 0x40520000u);
  Register(owner, 12, 0x40530000u);
  // group 11: another task, an empty (-1) slot, then the page object (3 registrations; the -1 poked in as a removal would leave it)
  Register(owner, 11, other), Register(owner, 11, 0xFFFFFFFFu), Register(owner, 11, a);
  MakePage(a, 0.f, 0, 0);
  const uint32_t arr = R(groups + 16 * 11);   // group 11's begin
  StageLookRow r;
  uint32_t cache = 0, miss = 0;
  Expect(Read(cache, miss, r) && cache == a, "finds the page object in group 11 through [owner+32] (skips other tasks and the -1 slot; not the decoys at owner+208, nor groups 10 / 12)");
  Expect(r.kind == rdr_scene::kSlPageClock && r.dir[0] == 0.f && r.flags == 0, "ring intro: t 0, not running, no entrance word");
  WF(a + 108, 304.f), W(a + 112, 1);
  Expect(Read(cache, miss, r) && r.dir[0] == 304.f && r.flags == 1, "running at t 304 (cached object, read again)");
  W(a + 88, 1);
  Expect(Read(cache, miss, r) && r.flags == 3, "entrance word sets bit 1");
  // scene_freeze.h's use of the same walk (kAll): every group's tasks, the -1 slot skipped, in order (groups 10, 11, 12 here)
  {
    const auto rd = [](uint32_t x, uint32_t& v) { auto it = g_mem.find(x); if (it == g_mem.end()) return false; v = it->second; return true; };
    uint32_t seen[8], n = 0;
    const bool ok = task_walk::ForEachGroupTask(rd, owner, task_walk::kAll, [&](uint32_t t, uint32_t) { if (n < 8) seen[n++] = t; return false; });
    Expect(ok && n == 4 && seen[0] == 0x40520000u && seen[1] == other && seen[2] == a && seen[3] == 0x40530000u, "ForEachGroupTask(kAll): groups 10, 11, 12 in order, the -1 slot skipped");
  }
  // the census info: [owner+32], the group count and group 11's task count
  {
    uint32_t c2 = 0, m2 = 0;
    StageLookRow r2;
    task_walk::GroupInfo gi;
    const bool ok = si::ReadStagePageClock(Rd, c2, m2, r2, &gi);
    Expect(ok && gi.base == groups && gi.count == kGroups && gi.tasks == 3 && gi.begin == arr, "census info: groups_ptr, 35 groups, group 11 holds 3 task words");
  }
  // the match ends, a second match builds a NEW object at another address (the old one is freed and overwritten)
  W(a, 0x82010000u);
  MakePage(b, 0.f, 0, 0);
  W(arr + 8, b);
  bool ok2 = false;
  for (int i = 0; i < int(si::kPageClockRescan) + 1 && !ok2; ++i) ok2 = Read(cache, miss, r) && cache == b;
  Expect(ok2 && r.dir[0] == 0.f && r.flags == 0, "second match: a new object, t restarts at 0 (the freed object is not trusted)");
  WF(b + 108, 90.f), W(b + 112, 1);
  Expect(Read(cache, miss, r) && r.dir[0] == 90.f, "the new object's clock is followed");
  W(b, 0x82010000u);   // gone (menus)
  uint32_t found = 0;
  for (int i = 0; i < 20; ++i) found += Read(cache, miss, r) ? 1 : 0;
  Expect(found == 0 && cache == 0, "no object: no row");
  // only a decoy in group 10 / 12 and the decoy list at owner+208: still no row
  found = 0;
  for (int i = 0; i < 20; ++i) found += Read(cache, miss, r) ? 1 : 0;
  Expect(found == 0, "decoys in groups 10 / 12 and at owner+208 are never taken");
  // [owner+32] == 0, unreadable, and a group array that is not 16-byte entries: no row (a fresh page object is in group 11 each time)
  W(a, 0x8201F294u), W(arr + 8, a);
  MakePage(a, 5.f, 1, 0);
  const uint32_t saved = R(owner + 32);
  W(owner + 32, 0);
  { uint32_t c = 0, m = 0, f = 0; for (int i = 0; i < 20; ++i) f += Read(c, m, r) ? 1 : 0; Expect(f == 0, "[owner+32] == 0 gives no row"); }
  g_mem.erase(owner + 32);
  { uint32_t c = 0, m = 0, f = 0; for (int i = 0; i < 20; ++i) f += Read(c, m, r) ? 1 : 0; Expect(f == 0, "[owner+32] unreadable gives no row"); }
  W(owner + 32, saved), W(owner + 40, saved + 16 * kGroups + 8);   // an end that is not a whole number of entries
  { uint32_t c = 0, m = 0, f = 0; for (int i = 0; i < 20; ++i) f += Read(c, m, r) ? 1 : 0; Expect(f == 0, "a group array that is not whole 16-byte entries gives no row"); }
  W(owner + 40, saved + 16 * 11);   // fewer than 12 groups
  { uint32_t c = 0, m = 0, f = 0; for (int i = 0; i < 20; ++i) f += Read(c, m, r) ? 1 : 0; Expect(f == 0, "fewer than 12 groups gives no row"); }
  W(owner + 40, saved + 16 * kGroups);
  { uint32_t c = 0, m = 0; Expect(Read(c, m, r) && c == a, "restored runner: found again"); }
  std::printf("fd_clock_unit: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}