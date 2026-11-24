// Host unit for src\replay_fx_skip_core.h (S110 TABLEFIRE): synthetic guest memory, no game. 0 game runs.
//   replay_fx_skip_unit.exe            all cases; exit 1 on a failure, each failure line starts with "FAIL <name>"
// The planted twins are made by tools\scene\replay_fx_skip_check.py, which compiles a text-edited copy of the core against this same unit.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "replay_fx_skip_core.h"

using namespace wwe2k14::replay_fx_skip;

namespace {

int g_fails = 0;
void Check(bool ok, const char* name, const char* what) {
  if (!ok) {
    ++g_fails;
    std::printf("FAIL %s: %s\n", name, what);
  }
}

struct FakeMem final : Mem {
  std::vector<uint8_t> b = std::vector<uint8_t>(0x10000, 0);   // guest 0x10000 .. 0x1FFFF
  static constexpr uint32_t kBase = 0x10000;
  bool In(uint32_t a, uint32_t n) const { return a >= kBase && a - kBase + n <= b.size(); }
  uint8_t U8(uint32_t a) const override { return In(a, 1) ? b[a - kBase] : 0; }
  uint16_t U16(uint32_t a) const override { return In(a, 2) ? uint16_t(b[a - kBase] << 8 | b[a - kBase + 1]) : 0; }
  uint32_t U32(uint32_t a) const override {
    if (!In(a, 4)) return 0;
    return uint32_t(b[a - kBase]) << 24 | uint32_t(b[a - kBase + 1]) << 16 | uint32_t(b[a - kBase + 2]) << 8 | b[a - kBase + 3];
  }
  void P8(uint32_t a, uint8_t v) { b[a - kBase] = v; }
  void P16(uint32_t a, uint16_t v) { P8(a, uint8_t(v >> 8)); P8(a + 1, uint8_t(v)); }
  void P32(uint32_t a, uint32_t v) { P16(a, uint16_t(v >> 16)); P16(a + 2, uint16_t(v)); }
};

constexpr uint32_t kTrack = 0x10100, kEvH = 0x10200, kFtH = 0x10210, kEvData = 0x11000, kFtData = 0x12000;
constexpr uint32_t kEvOff = 0x40, kFtOff = 0x20, kCap = 8;

struct Track {
  FakeMem m;
  uint32_t nev = 0;
  Track() {
    m.P32(kTrack + kOffCap, kCap);
    m.P32(kTrack + kOffBusy, 0);
    m.P32(kTrack + kOffEvBase, kEvH);
    m.P32(kTrack + kOffEvOff, kEvOff);
    m.P32(kTrack + kOffFtBase, kFtH);
    m.P32(kTrack + kOffFtOff, kFtOff);
    m.P32(kEvH, kEvData);
    m.P32(kFtH, kFtData);
    for (uint32_t f = 0; f < kCap; ++f) m.P16(kFtData + kFtOff + 2 * f, 0xFFFF);
  }
  // a full ring (+36 = kCap frames held): +32 is the NEXT write slot (sub_82707FF0 begins frame +32, then advances it), so the newest
  // recorded frame is +32 - 1 and slot +32 itself is the OLDEST frame (+28 == +32 once the ring is full)
  void Window(uint32_t last, uint32_t newest) {
    m.P32(kTrack + kOffLast, last);
    m.P32(kTrack + kOffNext, (newest + 1) % kCap);
  }
  // append an event to frame f's chain (at the end); type 3 = start, 5 = stop. +16 is the "life" word: 1 on a stop, 99 on a start
  void Ev(uint32_t f, uint8_t type, uint8_t ch, uint8_t id, uint32_t w4, uint8_t b2) {
    const uint32_t i = nev++;
    const uint32_t ev = kEvData + kEvOff + 24 * i;
    m.P8(ev + 0, ch);
    m.P8(ev + 1, id);
    m.P8(ev + 2, b2);
    m.P32(ev + 4, w4);
    m.P16(ev + 14, 0xFFFF);
    m.P32(ev + 16, type == 5 ? 1 : 99);
    m.P8(ev + 21, type);
    const uint32_t slot = kFtData + kFtOff + 2 * f;
    uint32_t cur = m.U16(slot);
    if (cur == 0xFFFF) {
      m.P16(slot, uint16_t(i));
      return;
    }
    for (;;) {
      const uint32_t nx = m.U16(kEvData + kEvOff + 24 * cur + 14);
      if (nx == 0xFFFF) break;
      cur = nx;
    }
    m.P16(kEvData + kEvOff + 24 * cur + 14, uint16_t(i));
  }
  // the fixture: a start at f0 (the looping fire), a stop at f2 (already sent by the sampler when last = f2), a start then a stop at f5
  void Fixture() {
    Ev(0, 3, 0, 10, 0x1111, 7);
    Ev(2, 5, 4, 44, 0x2222, 1);
    Ev(5, 3, 1, 11, 0x3333, 2);
    Ev(5, 5, 0, 10, 0xABCD, 9);
  }
};

}  // namespace

int main() {
  {
    Track t;
    t.Fixture();
    t.Window(2, 5);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.size() == 1, "skip.one_record", "last = f2, newest = f5 must give exactly the f5 stop (not f2's, which was already sent)");
    if (w.stops.size() == 1) {
      Check(w.stops[0].b0 == 0 && w.stops[0].b1 == 10 && w.stops[0].w4 == 0xABCD && w.stops[0].w8 == 9, "skip.fields", "the record is (ch 0, id 10, +4 0xABCD, +8 9)");
    }
    Check(w.frames == 3 && w.refused == 0 && w.ran, "skip.census", "3 frames walked (f3, f4, f5), not refused, ran");
  }
  {
    Track t;
    t.Fixture();
    t.Window(5, 5);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && !w.ran, "playedout.none", "played out (last == newest) sends nothing");
  }
  {
    Track t;
    t.Fixture();
    t.Window(0xFFFFFFFFu, 5);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && !w.ran, "nosample.none", "no sample taken (last = -1) sends nothing");
  }
  {
    Track t;
    t.Fixture();
    t.Window(2, 5);
    t.m.P32(kTrack + kOffBusy, 1);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty(), "busy.none", "+104 set sends nothing");
  }
  {
    Track t;   // a window that wraps: last = f6, newest = f1: frames 7, 0, 1; a stop at f7 and one at f1, none at f0
    t.Ev(7, 5, 2, 20, 0x77, 3);
    t.Ev(0, 3, 0, 10, 0x1111, 7);
    t.Ev(1, 5, 3, 30, 0x88, 4);
    t.Window(6, 1);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.size() == 2 && w.frames == 3, "wrap.walked", "a wrapped window walks f7, f0, f1 and gives 2 stops");
    if (w.stops.size() == 2) Check(w.stops[0].b1 == 20 && w.stops[1].b1 == 30, "wrap.order", "the stops come in frame order across the wrap");
  }
  {
    Track t;   // the chain of one frame holds a start before the stop: the walk follows the chain
    t.Ev(4, 3, 1, 1, 1, 1);
    t.Ev(4, 5, 1, 2, 2, 2);
    t.Ev(4, 3, 1, 3, 3, 3);
    t.Ev(4, 5, 1, 4, 4, 4);
    t.Window(3, 4);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.size() == 2 && w.stops[0].b1 == 2 && w.stops[1].b1 == 4, "chain.both", "a chain of start, stop, start, stop gives its two stops in order");
  }
  {
    Track t;   // a garbage chain: an event that points at itself
    t.Ev(4, 5, 1, 2, 2, 2);
    t.m.P16(kEvData + kEvOff + 14, 0);
    t.Window(3, 4);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && w.refused == 1, "garbage.loop", "a chain that loops is refused, nothing sent");
  }
  {
    Track t;
    t.Fixture();
    t.Window(2, 5);
    t.m.P32(kTrack + kOffEvBase, 0x20);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && w.refused == 1, "garbage.ptr", "an event store pointer below 0x10000 is refused");
  }
  {
    Track t;
    t.Fixture();
    t.Window(2, 5);
    t.m.P32(kTrack + kOffNext, kCap + 1);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && w.refused == 1, "garbage.newest", "a next-write slot past the frames held is refused");
  }
  {
    Track t;   // played out on a full ring: last = the newest frame (f5), +32 = f6 = the oldest frame, which holds an old stop: nothing may be sent
    t.Fixture();
    t.Ev(6, 5, 0, 10, 0x0DD, 1);
    t.Window(5, 5);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && !w.ran, "playedout.next_slot", "+32 is the next write slot: the played-out walk is empty and never reads the oldest frame's stops");
  }
  {
    Track t;   // a ring not yet full: +36 = frames held = 6, +32 = 6 (next write); last = f2 must walk f3..f5 and send f5's stop, not refuse
    t.Fixture();
    t.m.P32(kTrack + kOffCap, 6);
    t.m.P32(kTrack + kOffLast, 2);
    t.m.P32(kTrack + kOffNext, 6);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.size() == 1 && w.frames == 3 && w.refused == 0, "notfull.walk", "a ring not yet full (+32 == +36) walks to frame +36 - 1");
  }
  {
    Track t;   // early in a match: 3 frames held, one frame with 5 events (more than the frames held, within the 64-event pool): not refused
    t.m.P32(kTrack + kOffCap, 3);
    t.Ev(2, 3, 1, 1, 1, 1);
    t.Ev(2, 3, 1, 2, 2, 2);
    t.Ev(2, 3, 1, 3, 3, 3);
    t.Ev(2, 3, 1, 4, 4, 4);
    t.Ev(2, 5, 1, 5, 5, 5);
    t.m.P32(kTrack + kOffLast, 1);
    t.m.P32(kTrack + kOffNext, 3);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.size() == 1 && w.refused == 0, "chain.pool_bound", "a chain is bounded by the 64-event pool, not by the frames held");
  }
  {
    Track t;   // an event index past the 64-event pool is garbage
    t.m.P16(kFtData + kFtOff + 2 * 4, 64);
    t.Window(3, 4);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.empty() && w.refused == 1, "garbage.index", "an event index past the 64-event pool is refused");
  }
  {
    Track t;   // the played-out path (no skip seen): nothing is sent, even with stops in the window
    t.Fixture();
    t.Window(2, 5);
    const Walk w = SkippedStops(t.m, kTrack, false);
    Check(w.stops.empty() && !w.ran, "gate.clear", "no skip seen: the hook does nothing, whatever the window holds");
  }
  {
    SkipGate g;   // the skip signal: set only when the skip function left the controller in state 4, taken once at the end
    g.OnBegin();
    g.OnSkip(4);
    Check(g.TakeAtEnd(), "gate.set", "a skip (controller state 4 after sub_826071F8) is seen at the end");
    Check(!g.TakeAtEnd(), "gate.taken_once", "the end takes the flag: a second end sees no skip");
  }
  {
    SkipGate g;
    g.OnSkip(3);
    Check(!g.TakeAtEnd(), "gate.not_skipped", "sub_826071F8 that did not reach state 4 (its own refusal) is no skip");
  }
  {
    SkipGate g;   // a skip whose end never ran (scene left mid-fade): the next replay's begin clears it, so its played-out end sends nothing
    g.OnSkip(4);
    g.OnBegin();
    Check(!g.TakeAtEnd(), "gate.begin_clears", "begin clears a stale skip flag");
  }
  {
    Track t;   // a STOP then a later START of the same (channel, id) in the window: the STOP is sent, nothing is started
    t.Ev(3, 5, 0, 10, 0xABCD, 9);
    t.Ev(4, 3, 0, 10, 0xABCD, 9);
    t.Window(2, 5);
    const Walk w = SkippedStops(t.m, kTrack, true);
    Check(w.stops.size() == 1 && w.stops[0].b1 == 10, "restart.stop_sent", "a STOP followed by a START of the same key: the STOP is still sent, the START is not");
  }
  if (g_fails == 0) std::printf("OK replay_fx_skip_unit\n");
  return g_fails ? 1 : 0;
}
