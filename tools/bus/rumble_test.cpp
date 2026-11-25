// S101 lane 120: drives src/event_bus_rumble.h (the Rumble + move branch of the event bus's MatchTick) against synthetic guest words.
// Built and run by tools/bus/rumble_selftest.py (clang++ -std=c++17 -I src -DHDR=\"...\" tools/bus/rumble_test.cpp). No game, no SDK.
//   rumble_test SCENARIO   prints one line a bus event: {"f": F, "kind": K, <fields>}
// scenarios: rumble (type 23, G+80=38, G+81=2, counter 0 -> 1 -> 3, [0x835E1928] 0 -> 1, a clock that flaps), type0 (the same words, type 0),
//            cap (an entrant never built), resync (a counter that jumps > 40, one that drops), move / moveall (S110 BUSMOVE fix 2: move-log
//            calls and motion-start latches: a normal move and its continuation, the grapple-start offer loop and the real pick after it, an
//            offer latched by the next motion, run 002's Yes Lock logged twice before its one latch, the same finisher again after a motion
//            of nothing, a new character, a non-ring slot), twinorder (the last slot's character has the LOWER participant index)
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
    // one call of the move log sub_825A14A8 (slot, waza, move) as the game makes it: the pending log char+2486 / +2488 written;
    // offer = made inside the grapple-start offer loop sub_826BB6F0
    auto log = [&](uint32_t slot, uint32_t waza, uint32_t mv, bool offer) {
      const uint32_t c = m.r32(kSlotsTable + 4 * slot);
      m.w16(c + 2486, uint16_t(waza));
      m.w16(c + 2488, uint16_t(mv));
      MoveLog(ms, m, slot, offer);
    };
    // one motion start sub_823CA4B0 on character c ([c+24] == 4): the log latched into +2492 / +2494 and cleared, then the rule
    auto latch = [&](uint64_t f, uint32_t c) {
      m.w16(c + kMoveNowWaza, m.r16(c + 2486));
      m.w16(c + kMoveNowMove, m.r16(c + 2488));
      m.w16(c + 2486, 0xFFFF);
      m.w16(c + 2488, 0);
      MoveLatch(ms, m, c, all, [&](const std::string& s) { Line(f, "move", s); });
    };
    const uint32_t a = 0x50000000u, b = 0x50100000u, c6 = 0x50200000u;
    m.Char(0, a, 0, 343);  // Brock Lesnar
    m.w16(a + 2486, 0xFFFF);  // nothing logged
    log(0, 3, 100, false);  // a normal move, its motion: a line only with all
    latch(10, a);
    log(0, 3, 999, false);  // a re-log of the move it is in (r4 = [char+2492], r5 the act now playing), its next motion: nothing
    latch(11, a);
    latch(12, a);  // a motion of no logged move
    log(0, 72, 7000, false);  // the grapple (sub_826BB818 +1B4 logs 72), its motion: a line only with all
    latch(20, a);
    log(0, 14, 700, true);  // the offer loop right after it: slots 14..18 in one pass, slot 18 last
    log(0, 17, 50413, true);
    log(0, 18, 42060, true);
    log(0, 18, 42060, false);  // the real pick of slot 18, the same key the offer left, then its motion: a finisher line (f31)
    latch(31, a);
    log(0, 18, 42061, false);  // re-logged while in it, the next motion: nothing
    latch(32, a);
    log(0, 18, 42060, true);  // an offer left pending and latched by the next motion (no pick between): nothing
    latch(36, a);
    log(0, 18, 42060, false);  // the real pick of the same key after that: a line (f38)
    latch(38, a);
    latch(39, a);  // a motion of nothing
    log(0, 17, 58010, false);  // run 002 f13927: the chooser's pick of the Yes Lock (sub_826056E0 +7F0)
    log(0, 17, 58010, false);  // run 002 f13937: its executor logs the same record again (sub_82602DE0 +1994) ...
    latch(13937, a);           // ... then the hold's motion starts: ONE line
    latch(13940, a);  // a motion of nothing
    log(0, 17, 58010, false);  // the same finisher again: a second line
    latch(40000, a);
    m.Char(0, b, 0, 343);  // the slot's character replaced by a recycled object
    log(0, 17, 58010, false);  // its first play, the same waza as the other character's last: a line
    latch(50000, b);
    m.Char(6, c6, 1, 77);  // a character in slot 6 (not a ring slot)
    log(6, 17, 50413, false);
    latch(60000, c6);  // nothing
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
