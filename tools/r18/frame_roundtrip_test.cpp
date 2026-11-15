// S102 lane R18-B1: unit test for frame_roundtrip.h (no GPU, no capture): the walk's op calls (the same calls binder.h makes) and the identity question of a restore.
//   clang++ -std=c++20 -I src\renderer_adapter tools\r18\frame_roundtrip_test.cpp -o frame_roundtrip_test.exe
//   frame_roundtrip_test.exe                  every case must pass
//   frame_roundtrip_test.exe plant_NAME       the planted fault NAME (frame_roundtrip.h Fault): the test must FAIL
//     NAME: identity_after_write hidden_as_proof wrong_depth no_pair
#include <chrono>
#include <cstdio>
#include <cstring>

#include "frame_roundtrip.h"

using namespace rdr_adapter::frame_roundtrip;
namespace fc = rdr_adapter::frame_cuts;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// Bryan's numbers: frame buffer 8 (colour) / 9 (depth); scene resolve textures 2 (colour) / 5 (depth); snapshots 1289 / 1290.
static fc::Verdict Restore(uint32_t csrc, uint32_t cdst, uint32_t dsrc, uint32_t ddst) {
  fc::Verdict v;
  v.kind = fc::Verdict::Restore, v.colour_src = csrc, v.colour_dst = cdst, v.depth_src = dsrc, v.depth_dst = ddst;
  return v;
}
static const uint32_t C8[1] = {8};

// The walk of Bryan's entrance, op by op: R1 (2 / 5 -> 8 / 9), snapshots, the hidden draws, the colour snapshot 8 -> 2, the blit (left out), then R2 (2 / 5 -> 8 / 9).
struct Opts {
  bool hidden_writes_depth = false, hidden_writes_colour_after_copy = false, blit_counted = false, copy_issued = true, upload = false, foreign_depth = false, r1_kept_as_unknown = false, usable = true;
};
static bool Walk(RoundTrip& t, const Opts& o, Why& why) {
  t.BeginFrame(o.usable);
  if (o.upload) t.MarkDirty(5);
  t.OpSetTargets(C8, 1, 9);
  t.OpDraw(true);                                   // R1: the restore draw (its state writes depth: depth test + write)
  if (!o.r1_kept_as_unknown) {
    Why w1;
    CHECK(!t.RestoreIdentity(Restore(2, 8, 5, 9), w1));   // nothing is equal yet: the first restore is never the identity
    t.NoteRestore(Restore(2, 8, 5, 9));
  }
  t.OpCopy(1289), t.CopyDone(8, 1289, true, true);   // the snapshots
  t.OpCopy(1290), t.CopyDone(9, 1290, true, true);
  t.OpSetTargets(C8, 1, 9);
  for (int i = 0; i < 11; ++i) t.OpDraw(o.hidden_writes_depth);   // the hidden model_part draws: colour targets always, the depth when the state can
  if (o.foreign_depth) t.Foreign(9);
  t.OpCopy(2), t.CopyDone(8, 2, o.copy_issued, true);   // 8 -> 2: the colour of the frame buffer
  if (o.hidden_writes_colour_after_copy) t.OpDraw(false);
  t.OpSetTargets(C8, 1, 0);
  t.OpDraw(false);                                    // the blit
  if (!o.blit_counted) t.CancelDraw();                // (left out: it wrote nothing)
  t.OpSetTargets(C8, 1, 9);
  t.OpDraw(true);                                     // R2
  return t.RestoreIdentity(Restore(2, 8, 5, 9), why);
}

int main(int argc, char** argv) {
  if (argc > 1) {
    const char* name = argv[1];
    if (std::strncmp(name, "plant_", 6) == 0) name += 6;
    if (!ParseFault(name, g_fault)) { std::printf("unknown plant %s\n", argv[1]); return 2; }
  }
  Why why = Why::Taken;
  // 1. the entrance: R2 is the identity (colour 2 is a whole copy of 8 nothing wrote since, depth 9 = 5 from R1 with no depth write since)
  {
    RoundTrip t; Opts o;
    CHECK(Walk(t, o, why));
  }
  // 2. a hidden draw whose state can write depth between R1 and R2 (the 0-pixel draws are NOT trusted): the depth pair is stale (hidden_as_proof / wrong_depth plant this to identity)
  {
    RoundTrip t; Opts o; o.hidden_writes_depth = true;
    CHECK(!Walk(t, o, why) && why == Why::DepthStale);
  }
  // 3. a draw into 8 after the copy and before R2: the colour pair is stale (identity_after_write plants this to identity)
  {
    RoundTrip t; Opts o; o.hidden_writes_colour_after_copy = true;
    CHECK(!Walk(t, o, why) && why == Why::ColourStale);
  }
  // 4. the blit counted as a write although it was left out: R2 would be declined (the cancel keeps the pair valid)
  {
    RoundTrip t; Opts o; o.blit_counted = true;
    CHECK(!Walk(t, o, why) && why == Why::ColourStale);
  }
  // 5. the copy 8 -> 2 was not issued (aliased / dropped by another plan): no pair for the colour
  {
    RoundTrip t; Opts o; o.copy_issued = false;
    CHECK(!Walk(t, o, why) && why == Why::ColourStale);   // (R1's pair for 2 / 8 exists but is stale: 8 was drawn on since)
  }
  // 6. a CPU upload into the depth source this frame: stale
  {
    RoundTrip t; Opts o; o.upload = true;
    CHECK(!Walk(t, o, why));
  }
  // 7. another plan writes the depth target in between (Foreign): stale
  {
    RoundTrip t; Opts o; o.foreign_depth = true;
    CHECK(!Walk(t, o, why) && why == Why::DepthStale);
  }
  // 8. R1 not known to be a restore (its static rules declined: no pair for the depth): no depth pair
  {
    RoundTrip t; Opts o; o.r1_kept_as_unknown = true;
    CHECK(!Walk(t, o, why) && why == Why::NoDepthPair);
  }
  // 9. an unusable frame declines whatever it holds
  {
    RoundTrip t; Opts o; o.usable = false;
    CHECK(!Walk(t, o, why) && why == Why::Unusable);
  }
  // 10. a draw before any target switch: the targets are unknown, the frame is unusable from there
  {
    RoundTrip t;
    t.BeginFrame(true);
    t.OpDraw(true);
    CHECK(!t.usable);
  }
  // 11. the pair is symmetric and a clear / copy into either image makes it stale
  {
    RoundTrip t;
    t.BeginFrame(true);
    t.OpSetTargets(C8, 1, 9);
    t.OpCopy(2), t.CopyDone(8, 2, true, true);
    t.OpClear();
    t.Flush();
    CHECK(t.Equal(2, 8) == RoundTrip::State::Stale && t.Equal(8, 2) == RoundTrip::State::Stale);
    t.OpCopy(2), t.CopyDone(8, 2, true, true);
    CHECK(t.Equal(2, 8) == RoundTrip::State::Ok && t.Equal(8, 2) == RoundTrip::State::Ok && t.Equal(8, 8) == RoundTrip::State::No);
    t.OpCopy(8);
    t.Flush();
    CHECK(t.Equal(8, 2) == RoundTrip::State::Stale);
  }
  // 12. a copy that is not whole is no pair
  {
    RoundTrip t;
    t.BeginFrame(true);
    t.OpSetTargets(C8, 1, 9);
    t.OpCopy(2), t.CopyDone(8, 2, true, false);
    CHECK(t.Equal(2, 8) == RoundTrip::State::No);
  }
  // 13. a kept restore (G11's rules passed, not the identity) makes its destinations equal the sources even though the draw's own writes are pending at the time
  {
    RoundTrip t;
    t.BeginFrame(true);
    t.OpSetTargets(C8, 1, 9);
    t.OpDraw(true);
    t.NoteRestore(Restore(2, 8, 5, 9));
    t.OpDraw(false);                                  // the next op: the writes of the restore are applied now, the pair stays valid
    t.Flush();
    CHECK(t.Equal(2, 8) == RoundTrip::State::Stale);  // (this next draw writes 8 again: stale)
    RoundTrip u;
    u.BeginFrame(true);
    u.OpSetTargets(C8, 1, 9);
    u.OpDraw(true);
    u.NoteRestore(Restore(2, 8, 5, 9));
    u.OpCopy(77);                                     // an unrelated op
    CHECK(u.Equal(2, 8) == RoundTrip::State::Ok && u.Equal(5, 9) == RoundTrip::State::Ok);
  }
  // 14. the census reads
  {
    RoundTrip t; Opts o;
    CHECK(Walk(t, o, why));
    t.CountIdentityRestore(921600, true);
    CHECK(t.census.identity_restore == 1 && t.census.passes_dropped == 1 && t.census.copies_dropped == 2 && t.census.frames_taken == 1);
    const std::string r = t.Report(1);
    CHECK(r.find("identity 1") != std::string::npos && r.find("passes dropped 1") != std::string::npos);
  }
  // 15. the span: R1 makes F == S, the snapshot is accepted while neither moved; a write to S (or F) before it declines; the first restore unknown declines; closure keeps S == snapshot source after F is written
  {
    for (int variant = 0; variant < 6; ++variant) {
      RoundTrip t;
      t.BeginFrame(variant != 5);
      const uint32_t S = 1194, SD = 1195;
      t.OpSetTargets(C8, 1, 9);
      t.OpDraw(true);
      if (variant != 3) t.NoteRestore(Restore(S, 8, SD, 9));   // 3: the first restore was not G11-static
      if (variant == 1) t.Foreign(S);                            // a write to the scene target before the snapshot
      if (variant == 2) t.OpDraw(true), t.OpCopy(9), t.Flush();  // the frame buffer's depth written (a copy into 9)
      t.Flush();
      Span s;
      const SpanWhy why = t.AcceptSpan(s, 8, S, 9, SD);
      if (variant == 0 || variant == 4) CHECK(why == SpanWhy::Taken && s.state == 1);
      if (variant == 1) CHECK(why == SpanWhy::ColourStale);
      if (variant == 2) CHECK(why == SpanWhy::DepthStale || why == SpanWhy::ColourStale);
      if (variant == 3) CHECK(why == SpanWhy::NoColourPair);
      if (variant == 5) CHECK(why == SpanWhy::Unusable);
    }
    // closure: a copy F -> X after the restore makes S == X; F written by the hidden draws afterwards keeps S == X
    RoundTrip t;
    t.BeginFrame(true);
    t.OpSetTargets(C8, 1, 9);
    t.OpDraw(true);
    t.NoteRestore(Restore(1194, 8, 1195, 9));
    t.OpCopy(1289), t.CopyDone(8, 1289, true, true);
    t.OpDraw(false);   // the hidden draw writes 8
    t.Flush();
    CHECK(t.Equal(1194, 1289) == RoundTrip::State::Ok && t.Equal(8, 1289) == RoundTrip::State::Stale);
  }
  // 16. review F2, the ledger: a span took X / Y; the NEXT frame's stream samples X with no copy into it first: one violation and X is banned; a frame that copies into it first is clean
  {
    Ledger l;
    l.Taken(1289, 1290);
    CHECK(!l.Read(77));
    l.Written(1289);                       // frame with the snapshot copy: written before its read
    CHECK(!l.Read(1289) && !l.Banned(1289));
    CHECK(l.Read(1290) && l.Banned(1290));  // a read with no copy into it first (a pause / photo frame): violation + ban
    CHECK(!l.Read(1290));                   // counted once
  }
  // 17. review F3 / F1 tripwire: a draw the record cannot vouch for (marker, thin, owned) counts as a depth writer: OpDraw(true) between the first restore and the snapshot makes the depth pair stale; and the pair
  // that held at the first snapshot is gone at the second when something wrote F's depth in between
  {
    RoundTrip t;
    t.BeginFrame(true);
    t.OpSetTargets(C8, 1, 9);
    t.OpDraw(true);
    t.NoteRestore(Restore(1194, 8, 1195, 9));
    t.OpCopy(1289), t.CopyDone(8, 1289, true, true);
    Span s;
    CHECK(t.AcceptSpan(s, 8, 1194, 9, 1195) == SpanWhy::Taken);
    t.OpDraw(true);                         // a draw with depth write on between the two snapshots (Bryan's order: colour copy 1601, depth copy 1602)
    t.OpCopy(1290);
    CHECK(!t.SpanStillEqual(8, 1194, 9, 1195));
  }
  // 18. review F6: the restore is a pass under the stream and two copies under G11's restore switch, never both; the snapshots are booked as they are dropped
  {
    RoundTrip a, b;
    Span s; s.w = 1280, s.h = 720;
    a.CountSpanTaken(s, false), b.CountSpanTaken(s, true);
    CHECK(a.census.passes_dropped == 1 && a.census.copies_dropped == 0 && b.census.passes_dropped == 0 && b.census.copies_dropped == 2 && a.census.clears_dropped == 1);
  }
  // 19. cost of the per-op calls (the render thread's share): 2000 ops of a frame (draws, with a target switch every 8) through the two trackers' call pattern; printed, bounded
  {
    RoundTrip t;
    t.BeginFrame(true);
    const auto t0 = std::chrono::steady_clock::now();
    const int frames = 200, ops = 2000;
    for (int f = 0; f < frames; ++f) {
      t.BeginFrame(true);
      for (int i = 0; i < ops; ++i) {
        if (i % 8 == 0) { const uint32_t c[1] = {uint32_t(100 + i % 40)}; t.OpSetTargets(c, 1, 9); }
        else if (i % 50 == 7) t.OpCopy(uint32_t(300 + i % 20)), t.CopyDone(8, uint32_t(300 + i % 20), true, true);
        else t.OpDraw((i & 3) == 0);
      }
    }
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / double(frames * ops);
    std::printf("cost: %.1f ns an op (the RoundTrip calls of one walk op)\n", ns);
    CHECK(ns < 400.0);
  }
  if (g_fail) std::printf("%d FAILED\n", g_fail);
  else std::printf("all passed\n");
  return g_fail ? 1 : 0;
}
