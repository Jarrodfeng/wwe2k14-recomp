// drawn_slots_unit: the offline proof of lane CAWTPOSE (Session 111, docs\research\s111\CAWTPOSE.md): on a Create a Superstar item change the OLD character
// stays its slot's drawn body from the worker's slot store (sub_822E7570 +B0) until the editor's flip (sub_822E7CA0), and the NEW one is the body from the flip.
// Usage: drawn_slots_unit [--plant no_keep|no_hide_name|read_freed] [--expect-fail]     Exit 0 = every case holds (with --expect-fail: the planted old rule was caught).
// Build: tools\scene\build_drawn_slots_unit.ps1 (header only). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <cstdio>
#include <cstring>
#include <map>

#include "scene_live/drawn_slots.h"

namespace ds = scene_inputs::drawn_slots;

static std::map<uint32_t, uint32_t> g_mem;
static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (!ok) ++g_fail;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
static uint32_t g_freed_lo = 0, g_freed_hi = 0, g_freed_reads = 0;   // S111 FREEHOOK: a character block the game freed ([lo, hi), 0 = none): Rd counts the reads of it
static bool Rd(uint32_t a, uint32_t& v) {
  if (g_freed_hi && a >= g_freed_lo && a < g_freed_hi) ++g_freed_reads;
  auto it = g_mem.find(a);
  if (it == g_mem.end()) return false;
  v = it->second;
  return true;
}
// A built character: skeleton link and the four words the editor's build / flip write.
static void Char(uint32_t ch, uint32_t skel, uint32_t draw_on, uint32_t shown) {
  g_mem[ch + ds::kCharSkeleton] = skel;
  g_mem[ch + ds::kCharDrawOn] = draw_on;
  g_mem[ch + ds::kCharShownA] = shown;
  g_mem[ch + ds::kCharShownB] = shown;
}
struct Latch {
  ds::Out out[ds::kSlots];
  uint32_t n = 0;
  bool Has(uint32_t ch, int32_t slot, bool shown) const {
    for (uint32_t k = 0; k < n; ++k)
      if (out[k].ch == ch && out[k].slot == slot && out[k].shown == shown) return true;
    return false;
  }
};
static Latch Step(ds::Departed& d, uint32_t slot0_ch, uint32_t slot0_skel, uint32_t faults) {
  ds::Kept occ[ds::kSlots]{};
  occ[0] = ds::Kept{slot0_ch, slot0_skel};
  Latch l;
  l.n = d.Step(&Rd, occ, l.out, faults);
  return l;
}

int main(int argc, char** argv) {
  uint32_t faults = 0;
  bool expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc && !std::strcmp(argv[i + 1], "no_keep")) faults |= ds::kPlantNoKeep, ++i;
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc && !std::strcmp(argv[i + 1], "no_hide_name")) faults |= ds::kPlantNoHideName, ++i;
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc && !std::strcmp(argv[i + 1], "read_freed")) faults |= ds::kPlantReadFreed, ++i;
    else if (!std::strcmp(argv[i], "--expect-fail")) expect_fail = true;
  }
  const uint32_t A = 0x40000000u, SA = 0x40100000u, B = 0x40200000u, SB = 0x40300000u, C = 0x40400000u, SC = 0x40500000u;
  ds::Departed d;

  // 1. A is shown and in slot 0: nothing departed.
  Char(A, SA, 1, 1);
  Latch l = Step(d, A, SA, faults);
  Check(l.n == 0, "steady: the table's occupant is the body, nothing kept");

  // 2. Item change, the worker's build: slot 0 names B (its node and update task off: sub_823CC4B8 +28, sub_822E7570 +584; its shown words set at +4F8..+51C);
  //    the game still draws A (its +1208 stays 1 until the flip).
  Char(B, SB, 0, 1);
  l = Step(d, B, SB, faults);
  Check(l.Has(A, 0, true) && l.n == 1, "build: the old character A stays slot 0's drawn body");
  Check(d.KeptChar(0) == A, "build: the update hook's slot match sees A in slot 0");
  Check(!ds::GameDraws(&Rd, B, SB), "build: the game does not draw the new character B yet (+1208 = 0)");
  l = Step(d, B, SB, faults);
  Check(l.Has(A, 0, true) && l.n == 1, "build, next latch: A is still kept");
  l = Step(d, B, SB, faults);
  Check(l.Has(A, 0, true) && l.n == 1, "build, a second ask of the same latch: the same list");

  // 3. The flip (sub_822E7CA0): A off, B on.
  Char(A, SA, 0, 0);
  Char(B, SB, 1, 1);
  l = Step(d, B, SB, faults);
  Check(!(faults & ds::kPlantNoKeep) ? (l.Has(A, 0, false) && l.n == 1) : l.n == 0, "flip: A is named once more, hidden (its stamps are cleared)");
  Check(d.KeptChar(0) == 0, "flip: nothing kept");
  Check(ds::GameDraws(&Rd, B, SB), "flip: the game draws B");
  l = Step(d, B, SB, faults);
  Check(l.n == 0, "after the flip: A is forgotten, B is the table's body");

  // 4. The next item change: C built into A's buffer (A destroyed: its memory no longer links its skeleton); B is the old one now.
  g_mem[A + ds::kCharSkeleton] = 0;
  Char(C, SC, 0, 1);
  l = Step(d, C, SC, faults);
  Check(l.Has(B, 0, true) && l.n == 1, "second change: B stays slot 0's body, A is never revived");

  // 5. An occupant that leaves while the game does NOT draw it is not kept, and is named hidden ONCE at its departure (the latch clears its draw stamps: no linger
  //    window outlives the game's hide). Twin no_hide_name: forgotten unnamed.
  ds::Departed d2;
  Char(A, SA, 1, 1);
  Step(d2, A, SA, faults);
  Char(A, SA, 0, 1);
  l = Step(d2, 0, 0, faults);
  Check(l.Has(A, 0, false) && l.n == 1 && d2.KeptChar(0) == 0, "a departed character the game hides is named hidden once, not kept");
  l = Step(d2, 0, 0, faults);
  Check(l.n == 0, "a departed hidden character is forgotten after its one naming");

  // 5b. The game's departure: released from the table, then destroyed (the base destructor sub_823CCDD0 +108 clears [char+48]): named hidden once.
  ds::Departed d2b;
  const uint32_t E = 0x40800000u, SE = 0x40900000u;
  Char(E, SE, 1, 1);
  Step(d2b, E, SE, faults);
  g_mem[E + ds::kCharSkeleton] = 0;
  l = Step(d2b, 0, 0, faults);
  Check(l.Has(E, 0, false) && l.n == 1 && d2b.KeptChar(0) == 0, "a destroyed departed character is named hidden once, not kept");

  // 6. Unreadable words: neither kept nor named hidden (never guessed either way).
  ds::Departed d3;
  const uint32_t D = 0x40600000u, SD = 0x40700000u;
  g_mem[D + ds::kCharSkeleton] = SD;
  Step(d3, D, SD, faults);
  l = Step(d3, 0, 0, faults);
  Check(l.n == 0, "a departed character with unreadable words is not kept and not named");

  // 7. Per owner, and the game's publication: a second owner starts empty; the published word follows its owner's kept character and is cleared when the owner goes.
  {
    std::atomic<uint32_t> pub[ds::kSlots]{};
    {
      ds::Departed g(pub);
      Char(A, SA, 1, 1);
      Step(g, A, SA, faults);
      Step(g, B, SB, faults);   // A departs, still drawn
      Check(pub[0].load() == (faults & ds::kPlantNoKeep ? 0u : A) && g.KeptChar(0) == pub[0].load(), "publish: the owner's kept character is published");
      ds::Departed fresh;
      Check(fresh.KeptChar(0) == 0, "per owner: a new owner carries nothing of another's");
    }
    Check(pub[0].load() == 0, "publish: cleared when the owner goes");
  }

  // 8. S111 FREEHOOK (docs\research\s111\FREEHOOK.md): the game destroys a character our reader still remembers (its destructor body sub_825A1510; the block is freed right after,
  //    sub_825A2A30 +34). Its hook calls Forget: the published words drop it at once, and no later Step reads its block, whether it was the slot's kept departed body or its last
  //    occupant (the match: released in an update task, destroyed in the post-barrier tick, before the next latch). Twin read_freed: the old Step (nothing forgotten) reads the freed block.
  {
    const uint32_t F = 0x40A00000u, SF = 0x40B00000u, kBlock = 0x1000u;   // the words a Step reads: +48 .. +1208
    std::atomic<uint32_t> pub[ds::kSlots]{};
    ds::Departed g(pub);
    Char(F, SF, 1, 1);
    Char(A, SA, 1, 1);
    Step(g, F, SF, faults);
    Step(g, A, SA, faults);   // F departs, still drawn: kept and published
    const bool kept = (faults & ds::kPlantNoKeep) || (g.KeptChar(0) == F && pub[0].load() == F);
    g.Forget(F);
    const bool dropped = g.KeptChar(0) == 0 && pub[0].load() == 0;
    g_freed_lo = F, g_freed_hi = F + kBlock, g_freed_reads = 0;
    const Latch k1 = Step(g, A, SA, faults);
    const uint32_t kept_reads = g_freed_reads;
    g_freed_lo = g_freed_hi = 0;
    Check(kept && dropped && kept_reads == 0 && k1.n == 0, "freed: a destroyed kept character is dropped at once and never read again");
    ds::Departed h;
    Char(F, SF, 1, 1);
    Step(h, F, SF, faults);   // F is slot 0's occupant
    h.Forget(F);              // released and destroyed before the next latch
    g_freed_lo = F, g_freed_hi = F + kBlock, g_freed_reads = 0;
    const Latch k2 = Step(h, 0, 0, faults);
    const uint32_t occ_reads = g_freed_reads;
    g_freed_lo = g_freed_hi = 0;
    Check(occ_reads == 0 && k2.n == 0, "freed: a destroyed last occupant is never read by the next latch");
  }

  if (expect_fail) {
    std::printf(g_fail ? "planted fault caught (%d checks failed)\n" : "planted fault NOT caught\n", g_fail);
    return g_fail ? 0 : 1;
  }
  std::printf(g_fail ? "%d checks FAILED\n" : "all checks hold\n", g_fail);
  return g_fail ? 1 : 0;
}
