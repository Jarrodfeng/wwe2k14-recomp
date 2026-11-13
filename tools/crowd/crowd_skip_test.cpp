// Unit test of the crowd promotion's speed half (Session 101, lane 50): the decision logic in src\scene_live\crowd_skip_core.h (the very file the
// binder and the game-side hooks include), on a fake guest memory laid out as sub_8250BAF0 lays out the manager. No game, no SDK, no GPU.
//   crowd_skip_test.exe            -> checks the shipped logic; exit 0 = all pass
//   crowd_skip_test.exe --plant N  -> the same checks against a deliberately wrong policy N (a positive control): must exit 1
// Plants: ignore_kept, ignore_stale, drop_trigger, ignore_reads, card_without_ours, no_clean_wait, wrong_ring, unknown_group_skips, scene_crowd_open, stale_map, ignore_census.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "../../src/scene_live/crowd_skip_core.h"

namespace cs = wwe2k14::crowd_skip;

static int g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      ++g_fail;                                                       \
      std::printf("FAIL line %d: %s\n", __LINE__, msg);               \
    }                                                                 \
  } while (0)

static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }

// A fake guest memory: word address -> value.
struct Mem {
  std::map<uint32_t, uint32_t> w;
  uint32_t operator()(uint32_t a) const {
    auto it = w.find(a);
    return it == w.end() ? 0u : it->second;
  }
};

static const uint32_t kManager = 0x40000000u;

// The manager as sub_8250BAF0 reads it: write ring in word 548; for group g: count at 612 + 28 g + 16, array pointer at word 153 + 7 g + 5 + ring, model j's buffer = array[j].
static Mem BuildManager(uint32_t write_ring) {
  Mem m;
  m.w[kManager + 548] = write_ring;
  for (uint32_t g = 0; g < cs::kGroups; ++g) {
    const uint32_t count = 1 + (g % 3);
    m.w[kManager + 612 + 28 * g + 16] = count;
    // each ring has its own array and its own buffers, so a ring flip changes every answer
    for (uint32_t ring = 0; ring < 2; ++ring) {
      const uint32_t array = 0x50000000u + ring * 0x01000000u + g * 0x100;
      m.w[kManager + 4 * (153 + 7 * g + 5 + ring)] = array;
      for (uint32_t j = 0; j < count; ++j) m.w[array + 4 * j] = 0x70000000u + ring * 0x01000000u + g * 0x1000 + j * 0x10;
    }
  }
  return m;
}
static uint32_t BufferOf(uint32_t g, uint32_t j, uint32_t ring = 0) { return 0x70000000u + ring * 0x01000000u + g * 0x1000 + j * 0x10; }

// The policy under test: the shipped functions, optionally with a planted fault.
static void Report(bool drew, uint64_t kept, bool cards, int64_t now) {
  if (Plant("ignore_kept")) kept = 0;
  cs::g_gate.Report(drew, kept, cards, now);
}
static bool Proven(int64_t now) {
  if (Plant("ignore_stale")) now = cs::g_gate.last_ms.load();
  if (Plant("no_clean_wait")) return cs::g_gate.last_ms.load() != 0;
  return cs::g_gate.Proven(now);
}
static bool Drop(cs::DrawDecider& d, bool block, bool card, bool scene_crowd_open, int64_t now) {
  if (Plant("scene_crowd_open")) scene_crowd_open = false;
  if (Plant("drop_trigger")) return d.Drop(block, card, true, scene_crowd_open, now) || (block && Proven(now));
  if (Plant("card_without_ours")) return card ? Proven(now) : d.Drop(block, card, true, scene_crowd_open, now);
  return d.Drop(block, card, true, scene_crowd_open, now);
}
static bool Skip(const Mem& m, uint32_t buffer, int64_t now) {
  if (!Plant("stale_map")) cs::InvalidateMap();  // (the game: the sub_82509CB8 hook, once per manager update)
  if (Plant("wrong_ring")) {
    Mem other = m;
    other.w[kManager + 548] = 1 - m(kManager + 548);  // looks at the read side's ring
    return cs::SkipSkin(other, kManager, buffer, true, now);
  }
  if (Plant("unknown_group_skips")) return Proven(now) && !cs::g_reads.RecentlyRead(cs::GroupOfBuffer(m, kManager, buffer) % cs::kGroups, now);
  if (Plant("ignore_reads")) return Proven(now) && cs::GroupOfBuffer(m, kManager, buffer) < cs::kGroups;
  return cs::SkipSkin(m, kManager, buffer, true, now);
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) g_plant = argv[i + 1];
  if (!g_plant.empty()) std::printf("planted fault: %s\n", g_plant.c_str());

  const int64_t t0 = 1000000;

  // 1. Nothing reported: the game's work stays.
  {
    cs::DrawDecider d;
    d.BeginRun();
    CHECK(!Drop(d, true, false, false, t0), "no report: a block draw must be recorded");
    CHECK(!Drop(d, false, true, false, t0), "no report: a card draw must be recorded");
    const Mem m = BuildManager(0);
    CHECK(!Skip(m, BufferOf(5, 0), t0), "no report: the skin loop must run");
  }

  // 2. Frames in which ours drew with nothing kept: the third makes it proven.
  Report(true, 0, true, t0);
  Report(true, 0, true, t0 + 16);
  CHECK(!Proven(t0 + 20), "two clean frames are not enough");
  {
    cs::DrawDecider d;
    d.BeginRun();
    CHECK(!Drop(d, true, false, false, t0 + 20), "not proven yet: block draw recorded");
  }
  Report(true, 0, true, t0 + 33);
  CHECK(Proven(t0 + 40), "three clean frames prove it");

  // 3. P2b: the first block draw of a run stays (the injection trigger), the rest go; every run has its own first; cards go when ours draws them.
  {
    cs::DrawDecider d;
    d.BeginRun();
    CHECK(!Drop(d, true, false, false, t0 + 40), "the first block draw of a run is the trigger and stays");
    CHECK(Drop(d, true, false, false, t0 + 40), "the second block draw is dropped");
    CHECK(Drop(d, true, false, false, t0 + 40), "the third block draw is dropped");
    CHECK(Drop(d, false, true, false, t0 + 40), "a card draw is dropped when ours draws cards");
    CHECK(!Drop(d, false, false, false, t0 + 40), "a draw that is neither block nor card is never dropped");
    d.BeginRun();
    CHECK(!Drop(d, true, false, false, t0 + 40), "a new run (a tile) keeps its own first block draw");
    CHECK(Drop(d, true, false, false, t0 + 40), "...and drops the rest");
    CHECK(!Drop(d, true, false, true, t0 + 40), "inside an open scene_crowd run nothing is dropped here (that mode owns the records)");
  }

  // 4. A cards-less hook (cards not ours): card draws stay.
  Report(true, 0, false, t0 + 50);
  {
    cs::DrawDecider d;
    d.BeginRun();
    CHECK(!Drop(d, false, true, false, t0 + 55), "ours does not draw cards: card draws stay");
    CHECK(!Drop(d, true, false, false, t0 + 55), "(trigger)");
    CHECK(Drop(d, true, false, false, t0 + 55), "blocks still go");
  }
  Report(true, 0, true, t0 + 60);

  // 5. A frame with a kept stream draw (a member ours could not draw): everything returns to the game, and needs three clean frames again.
  Report(true, 2, true, t0 + 70);
  CHECK(!Proven(t0 + 72), "a kept draw voids the proof at once");
  {
    cs::DrawDecider d;
    d.BeginRun();
    CHECK(!Drop(d, true, false, false, t0 + 72) && !Drop(d, true, false, false, t0 + 72), "kept draw: no block draw is dropped");
  }
  Report(true, 0, true, t0 + 80);
  Report(true, 0, true, t0 + 90);
  CHECK(!Proven(t0 + 91), "after a void, two clean frames are not enough");
  Report(true, 0, true, t0 + 100);
  CHECK(Proven(t0 + 101), "...three are");

  // 6. A frame ours did not draw (a menu, a load screen): void.
  Report(false, 0, true, t0 + 110);
  CHECK(!Proven(t0 + 111), "a frame without ours voids the proof");
  Report(true, 0, true, t0 + 120);
  Report(true, 0, true, t0 + 130);
  Report(true, 0, true, t0 + 140);

  // 7. Silence: the render thread stopped reporting.
  CHECK(Proven(t0 + 150), "proven right after the last report");
  CHECK(!Proven(t0 + 140 + cs::kStaleMs + 1), "silence for the stale time voids the proof");

  // 8. P2c: the group lookup and the skip decision.
  Report(true, 0, true, t0 + 1000);
  Report(true, 0, true, t0 + 1010);
  Report(true, 0, true, t0 + 1020);
  CHECK(cs::g_gate.proven_frames.load() >= 1, "proven frames are counted");
  for (uint32_t ring = 0; ring < 2; ++ring) {
    const Mem m = BuildManager(ring);
    CHECK(cs::GroupOfBuffer(m, kManager, BufferOf(0, 0, ring)) == 0, "group 0 found");
    CHECK(cs::GroupOfBuffer(m, kManager, BufferOf(37, 1, ring)) == 37, "group 37 model 1 found");
    CHECK(cs::GroupOfBuffer(m, kManager, BufferOf(59, 1, ring)) == 59, "group 59 model 1 found");
    CHECK(cs::GroupOfBuffer(m, kManager, 0x7FFFFFF0u) == cs::kGroups, "an unknown buffer is in no group");
    CHECK(cs::GroupOfBuffer(m, kManager, 0) == cs::kGroups, "a null buffer is in no group");
    CHECK(cs::GroupOfBuffer(m, kManager, BufferOf(5, 0, 1 - ring)) == cs::kGroups, "the OTHER ring's buffer is not this ring's (the write ring is [manager+548])");
  }
  {
    const Mem m = BuildManager(0);
    const int64_t now = t0 + 1030;
    CHECK(Skip(m, BufferOf(7, 0), now), "an unread group's skin loop is skipped");
    CHECK(!Skip(m, 0x7FFFFFF0u, now), "an unknown buffer's skin loop runs");
    cs::g_reads.NoteRead(8, now - 10);
    CHECK(!Skip(m, BufferOf(8, 0), now), "a group another draw read lately is still skinned");
    CHECK(Skip(m, BufferOf(9, 0), now), "its neighbour is not");
    Report(true, 0, true, now + 2500);  // (keep the proof alive; the read is now older than kGroupReadMs)
    Report(true, 0, true, now + 2501);
    Report(true, 0, true, now + 2502);
    CHECK(Skip(m, BufferOf(8, 0), now + 2503), "a group not read for the read window is skipped again");
    CHECK(!cs::SkipSkin(m, kManager, BufferOf(9, 0), false, now + 2503), "switch off: the loop runs");
    // the map is built once per update, and rebuilt after the update hook invalidates it: a ring flip changes every buffer
    {
      const Mem m0 = BuildManager(0), m1 = BuildManager(1);
      const int64_t t = now + 2600;
      Report(true, 0, true, t);
      CHECK(Skip(m0, BufferOf(11, 0, 0), t), "ring 0: group 11 skipped");
      CHECK(Skip(m1, BufferOf(11, 0, 1), t), "after the flip and the invalidation: the ring 1 buffer is found (a map kept from ring 0 would not know it)");
      CHECK(!Skip(m1, BufferOf(11, 0, 0), t), "...and the ring 0 buffer no longer is");
    }
    // the census: the skin loops counted, the last skip remembered for crowd_ours.cpp's pose comparison
    {
      const uint64_t total = cs::g_loops_total.load(), skipped = cs::g_loops_skipped.load();
      // (lane 295 SF1: the previous report is 400 ms old, so the proof is void and ONE report no longer re-proves it: a pause / arena load starts the count again, three clean frames)
      Report(true, 0, true, now + 2990);
      CHECK(!Proven(now + 2991), "a report after a 400 ms gap does not re-prove at once");
      Report(true, 0, true, now + 2995);
      Report(true, 0, true, now + 3000);
      const bool ok = Skip(m, BufferOf(12, 0), now + 3001);
      CHECK(ok && cs::g_loops_total.load() == total + 1 && cs::g_loops_skipped.load() == skipped + 1, "a skipped loop is counted as skipped and as a loop");
      CHECK(cs::SkippingNow(now + 3002), "skipping now is visible to the pose comparison");
      CHECK(!cs::SkippingNow(now + 3001 + cs::kStaleMs + 1), "...and not after the stale time");
      if (Plant("ignore_census")) cs::g_loops_skipped.store(skipped);
      CHECK(cs::g_loops_skipped.load() == skipped + 1, "(census kept)");
    }
    // the gate's void causes
    {
      const int64_t t = now + 4000;
      Report(true, 0, true, t), Report(true, 0, true, t + 1), Report(true, 0, true, t + 2);  // (the first one follows a long silence: counted before the baseline below)
      const uint64_t k0 = cs::g_gate.void_kept.load(), n0 = cs::g_gate.void_not_drawn.load(), s0 = cs::g_gate.void_stale.load();
      Report(true, 2, true, t + 3);
      CHECK(cs::g_gate.void_kept.load() == k0 + 1, "a kept draw voiding a proof is counted");
      Report(true, 0, true, t + 4), Report(true, 0, true, t + 5), Report(true, 0, true, t + 6);
      Report(false, 0, true, t + 7);
      CHECK(cs::g_gate.void_not_drawn.load() == n0 + 1, "a frame without ours voiding a proof is counted");
      Report(true, 0, true, t + 8), Report(true, 0, true, t + 9), Report(true, 0, true, t + 10);
      Report(true, 0, true, t + 10 + cs::kStaleMs + 5);
      CHECK(cs::g_gate.void_stale.load() == s0 + 1, "silence voiding a proof is counted");
    }
    // an unproven gate (kept draw) stops the skipping
    Report(true, 3, true, now + 2504);
    CHECK(!Skip(m, BufferOf(9, 0), now + 2505), "a kept draw: the skin loops run again");
    cs::g_reads.NoteRead(cs::kGroups, now);  // out of range: ignored, no crash
    CHECK(cs::g_reads.RecentlyRead(cs::kGroups + 1, now), "an out-of-range group counts as read");
  }

  if (g_fail) {
    std::printf("crowd_skip_test: %d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("crowd_skip_test: all checks passed\n");
  return 0;
}
