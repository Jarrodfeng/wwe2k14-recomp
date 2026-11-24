// The skipped-replay stops (S110 TABLEFIRE; docs\research\s110\TABLEFIRE.md, "The smallest faithful fix"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, pure: no game, SDK or engine includes (the hook and the offline unit run THIS function).
//
// What the game does: the replay's particle track (PRTI) samples one frame at a time and sends each type-5 STOP event of a frame it reaches
// (0x8271B5F4-0x8271B61C -> sub_82253950). A skipped replay ends before the frames after the last sampled one, so those stops are never sent and
// the looping effect the replay re-lit keeps burning. On a SKIPPED replay only (SkipGate: the controller's skip function sub_826071F8 left it in
// state 4), SkippedStops returns the stops of the frames after the last sampled one up to the newest recorded one, in the order the sampler would
// have sent them: each was a stop of the live match, so "off" is the live truth whatever the clip ends are. A played-out replay is not touched.
// It NEVER starts an effect: a STOP followed in the window by a START of the same (channel, id) is still sent (begin already stopped the live
// instance and the replay never re-lit it), and the START is not.
//
// Layout (the note, claims 9 and 10; the window re-derived in review from the ring base class):
//   track +28 oldest slot, +32 the NEXT write slot, +36 frames held (the modulus the sampler and recorder use), +104 busy (the sampler issues events only at 0),
//   +132 / +136 event store (ptr to ptr, offset), +148 / +152 frame table (ptr to ptr, offset), +168 last sampled frame (-1 = none).
//   The window: sub_82707FF0 begins frame [+32] (vslot 15 = sub_8271B000, which sets +160 = that slot) and THEN advances +32 (sub_82707CE0:
//   +32 = (+32 + 1) mod +24, +36 = min(+36 + 1, +24)). So the newest recorded frame is (+32 - 1) mod +36; once full, slot +32 is the oldest
//   frame (+28 == +32); before that +32 == +36. The recorder's own window walk (sub_8271C078 +188) stops where (f + 1) mod +36 == +32 mod +36.
//   frame table: u16 at [[+148]] + [+152] + 2 f = first event index of frame f (0xFFFF = none).
//   events: 24 bytes at [[+132]] + [+136] + 24 i, a pool of 64 (sub_8271BE70 clears 1536 bytes); u8 +0 channel, u8 +1 id, u8 +2, u32 +4,
//   u16 +14 next event index (0xFFFF ends), u8 +21 type (5 = STOP).
// A value out of range (frames held 0 or over 0x10000, last not below it, +32 above it, a pointer below 0x10000 or past 0xC0000000, an event index
// past the pool, a chain longer than the pool) refuses the whole walk: nothing is sent, refused = 1.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace wwe2k14::replay_fx_skip {

// Guest memory as the walk sees it: big-endian values at guest addresses.
struct Mem {
  virtual ~Mem() = default;
  virtual uint8_t U8(uint32_t a) const = 0;
  virtual uint16_t U16(uint32_t a) const = 0;
  virtual uint32_t U32(uint32_t a) const = 0;
};

inline constexpr uint32_t kOffNext = 32, kOffCap = 36, kOffBusy = 104, kOffEvBase = 132, kOffEvOff = 136, kOffFtBase = 148, kOffFtOff = 152,
                          kOffLast = 168;
inline constexpr uint32_t kEvSize = 24, kEvType = 21, kTypeStop = 5, kNone16 = 0xFFFFu, kNone32 = 0xFFFFFFFFu, kMaxCap = 0x10000u, kPool = 64;

// The 20-byte record sub_82253950 takes (0x8271B5F4..+270): byte 0 = ev+0, byte 1 = ev+1, u32 +4 = ev+4, u32 +8 = ev+2 (u8), +12 = 0, +16 = 0.
struct StopRec {
  uint8_t b0 = 0, b1 = 0;
  uint32_t w4 = 0, w8 = 0;
};

struct Walk {
  std::vector<StopRec> stops;
  uint32_t frames = 0;    // frames walked
  uint32_t refused = 0;   // 1 = an [L] value or a pointer was out of range: nothing sent
  bool ran = false;       // the walk was attempted (a skipped replay): the census line is written
};

inline bool PtrOk(uint32_t p) { return p >= 0x10000u && p < 0xC0000000u; }

// The skip signal. sub_826071F8 (called only from the state-3 play handler sub_82607778 at 0x82607884) sets [ctrl+212] = 1 and [ctrl+48] = 4
// (the fade-out state, whose handler sub_826078A0 ends the replay through sub_8250FE90 -> slot 8) unless its own check refuses; so state 4 after it
// = a skip. Begin (sub_8271A630, slot 7) clears it, so a skip whose end never ran cannot leak into a later played-out replay; the end takes it.
inline constexpr uint32_t kCtrlState = 48, kStateSkipFade = 4;
struct SkipGate {
  std::atomic<bool> skipped{false};
  void OnBegin() { skipped.store(false); }
  void OnSkip(uint32_t state_after) {
    if (state_after == kStateSkipFade) skipped.store(true);
  }
  bool TakeAtEnd() { return skipped.exchange(false); }
};

inline Walk SkippedStops(const Mem& m, uint32_t track, bool skipped) {
  Walk w;
  if (!skipped) return w;                             // played out (or no skip seen): the original game, untouched
  if (!PtrOk(track)) return w;
  const uint32_t last = m.U32(track + kOffLast);
  if (last == kNone32) return w;                      // no sample was taken
  if (m.U32(track + kOffBusy) != 0) return w;         // the sampler issues events only when +104 == 0
  const uint32_t cap = m.U32(track + kOffCap), next = m.U32(track + kOffNext);
  if (cap == 0 || cap > kMaxCap || last >= cap || next > cap) { w.ran = true; w.refused = 1; return w; }
  const uint32_t newest = (next % cap + cap - 1) % cap;   // the newest recorded frame: +32 is the next write slot
  if (last == newest) return w;                       // played out to the newest frame: nothing left to send
  w.ran = true;
  const uint32_t evb_p = m.U32(track + kOffEvBase), ftb_p = m.U32(track + kOffFtBase);
  if (!PtrOk(evb_p) || !PtrOk(ftb_p)) { w.refused = 1; return w; }
  const uint32_t evb = m.U32(evb_p), ftb = m.U32(ftb_p);
  if (!PtrOk(evb) || !PtrOk(ftb)) { w.refused = 1; return w; }
  const uint32_t events = evb + m.U32(track + kOffEvOff), table = ftb + m.U32(track + kOffFtOff);
  if (!PtrOk(events) || !PtrOk(table)) { w.refused = 1; return w; }
  uint32_t f = (last + 1) % cap;
  for (;;) {
    ++w.frames;
    uint32_t i = m.U16(table + 2 * f);
    uint32_t steps = 0;
    while (i != kNone16) {
      if (++steps > kPool || i >= kPool) { w.stops.clear(); w.refused = 1; return w; }   // past the 64-event pool or longer than it: a loop or garbage
      const uint32_t ev = events + kEvSize * i;
      if (!PtrOk(ev)) { w.stops.clear(); w.refused = 1; return w; }
      if (m.U8(ev + kEvType) == kTypeStop) {
        StopRec r;
        r.b0 = m.U8(ev + 0);
        r.b1 = m.U8(ev + 1);
        r.w4 = m.U32(ev + 4);
        r.w8 = m.U8(ev + 2);
        w.stops.push_back(r);
      }
      i = m.U16(ev + 14);
    }
    if (f == newest) break;
    f = (f + 1) % cap;
  }
  return w;
}

}  // namespace wwe2k14::replay_fx_skip
