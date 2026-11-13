// S101 lane 120: drives src/event_bus_rumble.h (the Rumble + move branch of the event bus's MatchTick) against synthetic guest words.
// Built and run by tools/bus/rumble_selftest.py (clang++ -std=c++17 -I src -DHDR=\"...\" tools/bus/rumble_test.cpp). No game, no SDK.
//   rumble_test SCENARIO   prints one line a bus event: {"f": F, "kind": K, <fields>}
// scenarios: rumble (type 23, G+80=38, G+81=2, counter 0 -> 1 -> 3, [0x835E1928] 0 -> 1, a clock that flaps), type0 (the same words, type 0),
//            cap (an entrant never built), resync (a counter that jumps > 40, one that drops), move / moveall (Brock's finisher, a stale
//            recycled character, a repeat with no clear in between), twinorder (the last slot's character has the LOWER participant index)
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include HDR

using namespace wwe2k14::events::rumble;

struct Fake {
  std::map<uint32_t, uint8_t> b;
  uint8_t r8(uint32_t a) const { auto i = b.find(a); return i == b.end() ? 0 : i->second; }
  uint16_t r16(uint32_t a) const { return uint16_t(r8(a) << 8 | r8(a + 1)); }
  uint32_t r32(uint32_t a) const { return uint32_t(r16(a)) << 16 | r16(a + 2); }
  void w8(uint32_t a, uint8_t v) { b[a] = v; }
  void w16(uint32_t a, uint16_t v) { b[a] = uint8_t(v >> 8); b[a + 1] = uint8_t(v); }
  void w32(uint32_t a, uint32_t v) { w16(a, uint16_t(v >> 16)); w16(a + 2, uint16_t(v)); }
  void Desc(uint32_t e, uint32_t id) { w32(0x51000000u + 724 + 3580 * e + 56, id); }
  // a character in `slot` at address c, participant index e (whose description entry holds id)
  void Char(uint32_t slot, uint32_t c, uint32_t e, uint32_t id, uint8_t st = 0) {
    w32(kSlotsTable + 4 * slot, c);
    w32(c + 1220, slot);
    w32(c + 3756, e);
    w8(c + 1020, st);
    Desc(e, id);
  }
};

static void Line(uint64_t f, const char* kind, const std::string& fields) {
  std::printf("{\"f\": %llu, \"kind\": \"%s\", %s}\n", (unsigned long long)f, kind, fields.c_str());
}

int main(int argc, char** argv) {
  const std::string sc = argc > 1 ? argv[1] : "rumble";
  Fake m;
  m.w32(kMatchDescWord, 0x51000000u);
  if (sc.rfind("move", 0) == 0) {
    MoveState ms;
    const bool all = sc == "moveall";
    auto tick = [&](uint64_t f) { MoveTick(ms, m, all, [&](const std::string& s) { Line(f, "move", s); }); };
    const uint32_t a = 0x50000000u, b = 0x50100000u;
    m.Char(0, a, 0, 343);  // Brock Lesnar
    m.w16(a + 2486, 0xFFFF);
    tick(9);  // first sight: remembered, nothing emitted
    m.w16(a + 2486, 3);
    m.w16(a + 2488, 100);  // a normal move
    tick(10);
    m.w16(a + 2486, 17);
    m.w16(a + 2488, 50413);  // a finisher
    tick(11);
    tick(12);  // unchanged: nothing
    m.w16(a + 2486, 17);
    m.w16(a + 2488, 50413);  // written again with no clear the poll could see (the logger clears and rewrites in one call): LIMIT, nothing
    tick(13);
    m.Char(0, b, 0, 343);  // the slot's character is replaced by a recycled one holding a STALE finisher
    m.w16(b + 2486, 18);
    m.w16(b + 2488, 777);
    tick(14);  // first sight of the new pointer: remembered, no line
    m.w16(b + 2486, 17);
    m.w16(b + 2488, 50413);  // its own finisher, equal to the OLD character's last key: a line only if the key was reseeded on the pointer change
    tick(15);
    return 0;
  }
  m.w8(kG, sc == "type0" ? 0 : 23);
  m.w8(kG + 80, 38);
  m.w8(kG + 81, 2);
  m.Desc(5, 77), m.Desc(6, 88), m.Desc(7, 99), m.Desc(9, 55), m.Desc(10, 56);
  m.Char(0, 0x50200000u, 0, 11);  // the two starters
  m.Char(1, 0x50210000u, 1, 12);
  State st;
  auto tick = [&](uint64_t f, bool clock) { Tick(st, m, f, clock, [&](const std::string& s) { Line(f, "match", s); }); };
  tick(50, false);  // before the bell: nothing
  tick(100, true);  // clock_started: the starters
  m.Char(3, 0x50000000u, 5, 77);
  m.w32(kLastElimSlot, 3);
  m.w32(kElimCount, 1);
  m.w8(kEligFlags + 5, 1);
  m.w8(kElimCredit + 2, 1);
  tick(101, true);  // elimination 1
  m.w32(kElimCount, 3);
  m.w32(kLastElimSlot, 4);
  if (sc == "twinorder") {
    m.Char(4, 0x50030000u, 7, 99);  // the last slot's character has the HIGHER index here; the other victim (6) is the lower one
  } else {
    m.Char(4, 0x50030000u, 6, 88);  // the last slot's character (participant 6) has the LOWER index; the other victim is 7
  }
  m.w8(kEligFlags + 6, 1);
  m.w8(kEligFlags + 7, 1);
  tick(102, true);  // eliminations 2 + 3 in one frame
  m.w32(kEnteredCount, 1);
  m.w32(kSpawnSlot, 3);
  m.w8(0x50000000u + 1020, 5);  // slot 3 still holds the eliminated body
  tick(103, true);  // entered n=3
  tick(104, true);
  if (sc == "cap") {
    tick(103 + kEnteredIdCapFrames, true);
    return 0;
  }
  m.w8(0x50000000u + 1020, 0);  // the new character is built
  tick(105, true);
  tick(106, false);  // the clock flaps with no seq change (events_6541): nothing, and the starters are not emitted again
  tick(107, true);
  if (sc == "resync") {
    m.w32(kElimCount, 60);
    tick(200, true);  // a jump of more than 40: a resync line, no eliminations
    m.w32(kElimCount, 61);
    m.w32(kLastElimSlot, 4);
    m.w8(kEligFlags + 9, 1);
    tick(201, true);  // lines resume
    m.w32(kElimCount, 1);
    tick(202, true);  // the counter dropped: a new match, silent
    m.w32(kElimCount, 2);
    m.w8(kEligFlags + 10, 1);
    tick(203, true);  // lines resume
    m.w32(kEnteredCount, 100);
    tick(300, true);  // the entered counter jumps by more than 40
    m.w32(kEnteredCount, 101);
    tick(301, true);
    m.w32(kSpawnSlot, 0xFFFFFFFFu);  // a spawn slot word of -1 is read as signed
    m.w32(kEnteredCount, 102);
    tick(302, true);
  }
  return 0;
}
