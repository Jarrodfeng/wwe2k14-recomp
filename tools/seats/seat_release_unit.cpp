// S110 SEATREL (moved here from seats_proto_unit.cpp by SEAT3 lane G-B, which deleted the seat-swap prototype): the unit of src/seat_release_core.h, with
// synthetic guest memory and no game.
//   seat_release_unit.exe     every check must PASS (exit 0)
// Planted twins: tools/seats/seat_release_selftest.py edits a COPY of the header and requires this test to fail on it.
// Every payload word is a made-up pattern (never copied from a game file).
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "seat_release_core.h"

static int g_pass = 0, g_fail = 0;
static void Check(bool ok, const char* name) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
  (ok ? g_pass : g_fail)++;
}

int main() {
  namespace sr = wwe2k14::seat_release;
  std::map<uint32_t, uint32_t> m;
  const uint32_t mgr = 0x80100000, arr = 0x80200000;
  m[sr::kSlotMgrPtr] = mgr, m[mgr + 8] = arr;
  auto desc = [&](int i, uint32_t key, uint32_t loaded) { m[arr + 192 * uint32_t(i) + 52] = loaded, m[arr + 192 * uint32_t(i) + 28] = key; };
  for (int i = 0; i < 12; ++i) desc(i, 0xFFFFFFFFu, 0);
  desc(0, 39702, 1);   // ring slot of 397, attire 0
  desc(1, 39712, 1);   // attire 1
  desc(2, 39802, 1);   // ANOTHER id (398): never released
  desc(3, 39702, 0);   // 397's key but not loaded: nothing to release
  desc(4, 39699, 1);   // 396's last key: another id
  desc(5, 39700, 1);   // 397's first key
  desc(6, 39800, 1);   // 398's first key: another id
  desc(9, 39704, 1);   // entrance model of 397 (descriptors 9..11)
  desc(11, 39799, 1);  // the last descriptor, 397's last key
  auto rd = [&](uint32_t a, uint32_t& v) {
    auto it = m.find(a);
    if (it == m.end()) return false;
    v = it->second;
    return true;
  };
  const std::vector<int> got = sr::SlotsToRelease(rd, 397);
  Check(got == std::vector<int>({0, 1, 5, 9, 11}), "SlotsToRelease(397): loaded descriptors of 397 only, slots 0-11, every attire / kind digit");
  Check(sr::SlotsToRelease(rd, 398) == std::vector<int>({2, 6}), "SlotsToRelease(398): 398's own two, not 397's (a key of another id is NOT released: planted)");
  Check(sr::SlotsToRelease(rd, 400).empty(), "SlotsToRelease: an id nothing holds: none");
  m.erase(sr::kSlotMgrPtr);
  Check(sr::SlotsToRelease(rd, 397).empty(), "SlotsToRelease: no slot manager yet: none");

  // the sequence gate (a release frees the model of the slot: never while a match is loaded)
  Check(sr::SequenceRefusal(75).empty() && sr::SequenceRefusal(80).empty() && sr::SequenceRefusal(81).empty(), "SequenceRefusal: the front-end SmartMenu sequences 75 / 80 / 81 allow a seat");
  Check(sr::SequenceRefusal(9).find("a match is loaded") != std::string::npos, "SequenceRefusal: in game (9: load, entrances, the match, pause, replay, victory) refuses (planted)");
  Check(!sr::SequenceRefusal(76).empty() && !sr::SequenceRefusal(77).empty() && !sr::SequenceRefusal(78).empty(), "SequenceRefusal: the creation suite (76 CAS, 77 Threads, 78 PaintTool) refuses (planted)");
  Check(!sr::SequenceRefusal(0).empty() && !sr::SequenceRefusal(3).empty() && !sr::SequenceRefusal(82).empty(), "SequenceRefusal: no sequence (0), the title (3), the Universe next match (82) refuse");

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
