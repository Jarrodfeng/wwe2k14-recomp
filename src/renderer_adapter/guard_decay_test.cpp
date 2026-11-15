// S102 lane K1: synthetic tests for the decaying state guard (wwe2k14_rdr_guard_decay) and the flat guard walk (wwe2k14_rdr_skip_lean) of lasting_prune.h. No GPU, no capture.
//   clang++ -std=c++20 -I src src\renderer_adapter\guard_decay_test.cpp -o guard_decay_test.exe
//   guard_decay_test.exe [--plant-fast | --plant-lean]   (--plant-lean: the flat walk forgets copy writes)
//     (--plant-fast: the guard window is 1 frame: a lasting copy is lost; the test must report FAIL)
// Cases:
//   S1  the match: an image read once before its copy (frame 0), then moved per-frame copies into it: the old guard keeps them lasting for ever; decay lets the image rule take them after the window,
//       and not before it.
//   S2  an image that carries state read before its copy EVERY frame (exposure): stays lasting under decay at any frame.
//   S3  the same read every 4th frame (an effect that steps on alternate frames): still lasting under a 16 frame window (the plant's window of 1 loses it).
//   S4  a tile built atlas (a different tile each frame, no overlap) never becomes per-frame, decay or not.
//   S5  the flat guard walk equals the map walk: seeds of every frame, the state set and its last-seen frames, over a seeded random stream of 4000 frames (with ids past kLeanMax).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "lasting_prune.h"

using namespace rdr_adapter;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Syn {
  std::vector<Op> ops;
  std::vector<Draw> draws;
  std::vector<rdr::CopyOp> copies;
  std::vector<TextureSlot> slots;
  void DrawOp(std::vector<uint32_t> tex) {
    Draw d;
    d.texture_first = uint32_t(slots.size());
    d.texture_count = uint32_t(tex.size());
    for (uint32_t t : tex) {
      TextureSlot s;
      s.image = t;
      slots.push_back(s);
    }
    draws.push_back(d);
    ops.push_back({OpKind::Draw, uint32_t(draws.size() - 1)});
  }
  void CopyOp(uint32_t src, uint32_t dst, int32_t x, int32_t y, uint32_t w = 256, uint32_t h = 256) {
    rdr::CopyOp c;
    c.source.image = rdr::ImageId(src);
    c.dest.image = rdr::ImageId(dst);
    c.dest_rect = {x, y, w, h};
    copies.push_back(c);
    ops.push_back({OpKind::Copy, uint32_t(copies.size() - 1)});
  }
  LoadedFrame Frame() const {
    LoadedFrame f;
    f.ops = {ops.data(), uint32_t(ops.size())};
    f.draws = {draws.data(), uint32_t(draws.size())};
    f.copies = {copies.data(), uint32_t(copies.size())};
    f.texture_slots = {slots.data(), uint32_t(slots.size())};
    return f;
  }
};

// One handled frame through the live order (Note, then Classify when skipped); returns the first copy's seed.
static bool Handle(lasting::CopyStreaks& st, const Syn& s, std::vector<uint8_t>* seed_out = nullptr) {
  LoadedFrame f = s.Frame();
  st.Note(f);
  std::vector<uint8_t> seed;
  st.Classify(f, false, seed, [](const rdr::CopyOp&) {});
  if (seed_out) *seed_out = seed;
  for (size_t i = 0; i < seed.size(); ++i)
    if (s.ops[i].kind == OpKind::Copy && seed[i]) return true;
  return false;
}

static uint32_t g_window = 16;
static bool g_plant_lean = false, g_plant_noclear = false;
static uint32_t g_rule = 0;   // --plant-oldrule: the time-based draft rule (TEST ONLY)

// Frame n of a per-frame copy into image 20 whose rectangle moves by a few pixels (overlap far above half); `read_before` = a draw samples 20 before the copy.
static Syn MovingFrame(int n, bool read_before) {
  Syn s;
  if (read_before) s.DrawOp({20});
  s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4);
  s.DrawOp({20});   // after the copy: not a state read
  return s;
}

static lasting::CopyStreaks Make(bool decay, bool lean) {
  lasting::CopyStreaks st;
  st.guard_decay = decay;
  st.guard_window = g_window;
  st.guard_rule = g_rule;
  st.lean = lean;
  return st;
}

static void S1() {   // the match
  for (int decay = 0; decay < 2; ++decay) {
    lasting::CopyStreaks st = Make(decay, false);
    int lasting_at[64] = {};
    for (int n = 0; n < 64; ++n) lasting_at[n] = Handle(st, MovingFrame(n, n == 0));
    // frame 0 is a state read; streaks need 3 frames: lasting for frames 0..2 either way.
    CHECK(lasting_at[0] && lasting_at[1] && lasting_at[2]);
    // inside the window the guard still holds (frames 3..10, window 16)
    for (int n = 3; n <= 10; ++n) CHECK(lasting_at[n]);
    if (!decay) {
      for (int n = 20; n < 64; ++n) CHECK(lasting_at[n]);   // insert-only: lasting for ever (the R16 finding)
      CHECK(st.guard_decayed == 0);
    } else {
      for (int n = int(g_window) + 3; n < 64; ++n) CHECK(!lasting_at[n]);   // decayed: per-frame by the image rule
      CHECK(st.guard_decayed > 0);
    }
  }
}

static void S2() {   // read before its copy every frame
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 120; ++n) CHECK(Handle(st, MovingFrame(n, true)));   // never decays
}

static void S3() {   // read before its copy every 4th frame
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 120; ++n) {
    const bool lasting_now = Handle(st, MovingFrame(n, n % 4 == 0));
    if (n >= 4) CHECK(lasting_now);   // window 16 keeps it; --plant-fast (window 1) loses it on the frames between the reads
  }
}

static void S4() {   // a tile built atlas
  for (int decay = 0; decay < 2; ++decay) {
    lasting::CopyStreaks st = Make(decay, false);
    for (int n = 0; n < 100; ++n) {
      Syn s;
      if (n == 0) s.DrawOp({40});   // read once before its copy: a state image
      s.CopyOp(10, 40, (n % 8) * 64, (n / 8 % 8) * 64, 64, 64);
      CHECK(Handle(st, s));
    }
  }
}

static void S5() {   // flat walk == map walk
  std::mt19937 rng(12345);
  lasting::CopyStreaks a = Make(true, false), b = Make(true, true);
  b.plant_lean_no_write = g_plant_lean;
  const uint32_t ids[] = {3, 7, 20, 21, 22, 40, 41, 300, 5000, 1048575, 1048576, 1048600, 2000000};   // ids at and past kLeanMax take the map path
  for (int n = 0; n < 4000; ++n) {
    Syn s;
    const int ops = int(rng() % 12);
    for (int k = 0; k < ops; ++k) {
      if (rng() % 3 == 0) {
        s.CopyOp(ids[rng() % 13], ids[rng() % 13], int32_t(rng() % 3) * 4, int32_t(rng() % 3) * 4);
      } else {
        std::vector<uint32_t> tex;
        for (uint32_t t = 0, c = rng() % 4; t < c; ++t) tex.push_back(ids[rng() % 13]);
        s.DrawOp(tex);
      }
    }
    std::vector<uint8_t> sa, sb;
    const bool la = Handle(a, s, &sa), lb = Handle(b, s, &sb);
    CHECK(la == lb);
    CHECK(sa == sb);
    CHECK(a.state_last == b.state_last);   // the last frame each image was seen as state, after every frame
    if (la != lb || sa != sb || a.state_last != b.state_last) break;
  }
  CHECK(a.state_images == b.state_images);
  CHECK(a.state_last == b.state_last);
  CHECK(a.by_image_guarded == b.by_image_guarded && a.by_image_left == b.by_image_left && a.guard_decayed == b.guard_decayed);
  CHECK(!a.state_images.empty());
}

// One handled frame in the live order with the ledger: odd frames are skipped (Classify: the left-out copies become owed), even frames are drawn (NoteDrawn: a write clears, a stale read counts).
static bool Handle2(lasting::CopyStreaks& st, const Syn& s, int n) {
  LoadedFrame f = s.Frame();
  st.Note(f);
  if (n % 2 == 0) { st.NoteDrawn(f); return false; }
  std::vector<uint8_t> seed;
  st.Classify(f, false, seed, [](const rdr::CopyOp&) {});
  for (size_t i = 0; i < seed.size(); ++i)
    if (s.ops[i].kind == OpKind::Copy && seed[i]) return true;
  return false;
}

static void S1b() {   // S1 under the ledger, window 16: the decay happens, no drawn frame reads an image a skipped frame left out before its copy
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 120; ++n) Handle2(st, MovingFrame(n, n == 0), n);
  CHECK(st.guard_decayed > 0);
  CHECK(st.stale_read == 0);
}

// F2: the reader (a read before the copy) is on frames 0-39 and 70-79, absent 40-69; the copy is made every frame. The positive-evidence rule keeps every copy (an absent reader is no evidence);
// the time-based draft rule (--plant-oldrule, window 1 with --plant-fast) drops them and the reader's return reads stale content (the ledger counts it).
static void S6() {
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 100; ++n) {
    const bool reader = n < 40 || (n >= 70 && n < 80);
    Syn s;
    if (reader) s.DrawOp({20});
    s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4);
    const bool lasting_now = Handle2(st, s, n);
    if (n % 2 == 1 && n >= 3) CHECK(lasting_now);   // every copy kept
  }
  CHECK(st.stale_read == 0);
}

// F4: the off path pays for no decay bookkeeping (state_last stays empty); the off-to-on edge seeds from state_images at the current frame, so the guard is not lost at the flip.
static void S7() {
  lasting::CopyStreaks st = Make(false, false);
  for (int n = 0; n < 100; ++n) Handle(st, MovingFrame(n, n == 0));
  CHECK(st.state_last.empty());
  CHECK(!st.state_images.empty());
  st.guard_decay = true;
  for (int n = 100; n < 106; ++n) CHECK(Handle(st, MovingFrame(n, false)));   // seeded at the flip: still held for the window
  CHECK(!st.state_last.empty());
}

// F3 / F5: the gap histogram (reads before the copy every 4th frame: gap 4 = bucket 2) and the off-path would-decay counter (S1 shape with decay OFF: the counter sees what the switch would take).
static void S8() {
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 60; ++n) Handle(st, MovingFrame(n, n % 4 == 0));
  CHECK(st.gap_hist[2] >= 10);
  lasting::CopyStreaks off = Make(false, false);
  for (int n = 0; n < 80; ++n) Handle(off, MovingFrame(n, n == 0));
  CHECK(off.would_decay > 0 && off.guard_decayed == 0);
  lasting::CopyStreaks on = Make(true, false);
  for (int n = 0; n < 80; ++n) Handle(on, MovingFrame(n, n == 0));
  CHECK(on.guard_decayed > 0 && on.would_decay == 0);
}

// F1: one instance flips the flat walk on and off every 37 frames and must equal the map-only instance after EVERY frame (ids past kLeanMax go through the map in both).
static void S5b() {
  std::mt19937 rng(777);
  lasting::CopyStreaks a = Make(true, false), b = Make(true, false);
  b.plant_lean_no_clear = g_plant_noclear;
  const uint32_t ids[] = {3, 7, 20, 21, 40, 300, 1048575, 1048576, 1048600, 2000000};
  for (int n = 0; n < 4000; ++n) {
    b.lean = (n / 37) % 2 == 1;
    Syn s;
    const int ops = int(rng() % 12);
    for (int k = 0; k < ops; ++k) {
      if (rng() % 3 == 0) {
        s.CopyOp(ids[rng() % 10], ids[rng() % 10], int32_t(rng() % 3) * 4, int32_t(rng() % 3) * 4);
      } else {
        std::vector<uint32_t> tex;
        for (uint32_t t = 0, c = rng() % 4; t < c; ++t) tex.push_back(ids[rng() % 10]);
        s.DrawOp(tex);
      }
    }
    std::vector<uint8_t> sa, sb;
    const bool la = Handle(a, s, &sa), lb = Handle(b, s, &sb);
    CHECK(la == lb && sa == sb);
    CHECK(a.state_images == b.state_images && a.state_last == b.state_last);
    if (la != lb || sa != sb || a.state_last != b.state_last || a.state_images != b.state_images) break;
  }
  CHECK(a.gap_hist[0] == b.gap_hist[0] && a.gap_hist[3] == b.gap_hist[3]);
}

static bool g_plant_blind = false;
// S102 K2 (stale reads with the decay): an image read before its copy only every 40th handled frame (a state value consumed now and then), per-frame reads after the copy in between. The faulty rule judged it by the 16
// frames of per-frame reads and dropped the copy in the skipped frames before the next carry read: every carry read found the image owed (stale). The cadence rule learns the gap from the second carry read and holds
// 2 x gap: at most ONE stale read (the first cycle: the cadence is unknown until it has been seen twice), 0 after. --plant-blind (cadence_blind) restores the faulty rule: stale reads on every cycle.
static void S9() {
  for (int blind = 0; blind < 2; ++blind) {
    lasting::CopyStreaks st = Make(true, false);
    st.cadence_blind = blind || g_plant_blind;
    for (int n = 0; n < 400; ++n) Handle2(st, MovingFrame(n, n % 40 == 0), n);
    if (blind) CHECK(st.stale_read >= 5);          // the faulty rule: every cycle
    else CHECK(st.stale_read <= 1);                // the fix: the first cycle only
    std::printf("cadence 40, window 16, %s: %llu stale reads in 400 handled frames\n", blind ? "blind rule" : "cadence rule", (unsigned long long)st.stale_read);
  }
}

// S102 K2 review (Opus, planted tests t.cpp / re.cpp / re2.cpp / fp.cpp / di.cpp turned into cases). Every scenario is the live order with the ledger: odd handled frames are skipped, even ones drawn.
template <class P>
static uint64_t CarryStale(int N, P carry, bool blind) {
  lasting::CopyStreaks st = Make(true, false);
  st.cadence_blind = blind;
  for (int n = 0; n < N; ++n) Handle2(st, MovingFrame(n, carry(n)), n);
  return st.stale_read;
}

// F2: jittered cadences defeat "twice the LAST gap". Monotone gap + the pin on the first stale read: at most ONE stale read in every scenario; the blind rule (the faulty one) reads more.
static void S10() {   // alternating gaps 10 / 50
  const auto c = [](int n) { int m = n % 60; return m == 0 || m == 10; };
  const uint64_t fix = CarryStale(1200, c, false), bl = CarryStale(1200, c, true);
  std::printf("alternating 10/50: fix %llu, blind %llu\n", (unsigned long long)fix, (unsigned long long)bl);
  CHECK(fix <= 1 && bl >= 5);
}
static void S11() {   // growing gaps 20, 50, 130, 330
  const auto c = [](int n) { return n == 0 || n == 20 || n == 70 || n == 200 || n == 530; };
  const uint64_t fix = CarryStale(600, c, false), bl = CarryStale(600, c, true);
  std::printf("growing gaps 20,50,130,330: fix %llu, blind %llu\n", (unsigned long long)fix, (unsigned long long)bl);
  CHECK(fix <= 1 && bl >= 3);
}
static void S12() {   // jitter 20 then 50, repeating
  const auto c = [](int n) { int m = n % 70; return m == 0 || m == 20; };
  const uint64_t fix = CarryStale(1400, c, false), bl = CarryStale(1400, c, true);
  std::printf("jitter 20/50: fix %llu, blind %llu\n", (unsigned long long)fix, (unsigned long long)bl);
  CHECK(fix <= 1 && bl >= 5);
}

// F1 / b: a copy RUN ENDING ON A SKIPPED FRAME. Image 20 is read before its copy once (frame 0) (`prior_read`), moving per-frame copies 0..59 with reads after them, the last copy (59, odd) is on a skipped frame, the copies stop, frames 60..199
// read it with no copy. Decay off: 0 stale (the insert-only guard kept 59's copy). Decay on: ONE stale read (counted once per owed episode: before, every later drawn frame counted, 70 times), shape = run end, path = the
// decayed guard; the image is pinned afterwards. Without the prior read (re2: never a state image) the exact-rect / image rule owes it in ALL arms: one stale read, run end, decay off included (the shipped behaviour). The copy can not be
// recovered (the skipped frame's own render was dropped): the image stays one frame older than the game's, in a picture no copy updates any more.
static uint64_t RunEnd(bool decay, bool prior_read, uint64_t* run_end, uint64_t* guard_path) {
  lasting::CopyStreaks st = Make(decay, false);
  for (int n = 0; n < 200; ++n) {
    Syn s;
    if (n == 0 && prior_read) s.DrawOp({20});
    if (n < 60) s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4);
    s.DrawOp({20});
    Handle2(st, s, n);
  }
  if (run_end) *run_end = st.stale_run_end;
  if (guard_path) *guard_path = st.stale_guard();
  return st.stale_read;
}
static void S13() {
  uint64_t re_off = 0, g_off = 0, re_on = 0, g_on = 0, re2 = 0, g2 = 0, re2on = 0, g2on = 0;
  const uint64_t off = RunEnd(false, true, &re_off, &g_off), on = RunEnd(true, true, &re_on, &g_on), off2 = RunEnd(false, false, &re2, &g2), on2 = RunEnd(true, false, &re2on, &g2on);
  std::printf("run end (read once at 0): decay off %llu, decay on %llu (run end %llu, by the decayed guard %llu); never read before its copy: decay off %llu, on %llu\n",
              (unsigned long long)off, (unsigned long long)on, (unsigned long long)re_on, (unsigned long long)g_on, (unsigned long long)off2, (unsigned long long)on2);
  CHECK(off == 0);
  CHECK(on == 1 && re_on == 1 && g_on == 1);
  CHECK(off2 == 1 && re2 == 1 && on2 == 1 && re2on == 1);
}

// F3: a skipped frame that KEEPS the copy owes nothing. Reads before the copy on frames 0-9 and 61+ (the guard decays in between), frame 61 (skipped) keeps its copy again: drawn frame 62 reads content frame 61 made: 0 stale.
static void S14() {   // frames 50..61 are all skipped, frame 62 is drawn
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 80; ++n) {
    const Syn s = MovingFrame(n, n < 10 || n >= 61);
    LoadedFrame f = s.Frame();
    st.Note(f);
    if (!(n >= 50 && n <= 61) && n % 2 == 0) { st.NoteDrawn(f); continue; }   // drawn: even frames outside the skipped run
    if (!(n >= 50 && n <= 61) && n % 2 == 1) { st.NoteDrawn(f); continue; }   // (outside the run every frame is drawn)
    std::vector<uint8_t> seed;
    st.Classify(f, false, seed, [](const rdr::CopyOp&) {});
  }
  std::printf("skipped frame keeps the copy: stale %llu (must be 0)\n", (unsigned long long)st.stale_read);
  CHECK(st.stale_read == 0);
}

// F4: a T3 draw-instead frame (Classify ran, the frame was drawn): nothing was left out, so nothing is owed. Live defers the owed set (defer_owed) and commits it only on the really-skipped branch; the plant (no defer) counts 1.
static void S15() {
  for (int defer = 1; defer >= 0; --defer) {
    lasting::CopyStreaks st;
    st.defer_owed = defer != 0;
    for (int n = 0; n < 20; ++n) {
      Syn s;
      s.DrawOp({20});
      s.CopyOp(10, 20, 0, 0);
      LoadedFrame f = s.Frame();
      st.Note(f);
      if (n == 10) { std::vector<uint8_t> seed; st.Classify(f, false, seed, [](const rdr::CopyOp&) {}); }   // wanted a skip, drawn instead: no CommitOwed
      st.NoteDrawn(f);
    }
    std::printf("draw-instead frame, defer_owed %d: stale %llu\n", defer, (unsigned long long)st.stale_read);
    if (defer) CHECK(st.stale_read == 0); else CHECK(st.stale_read == 1);
  }
}

// F1: an image read in a frame with NO copy into it (consumed, not rebuilt) never decays. Moving copies into 20 every frame, except every 60th frame (n % 60 == 0): that frame reads 20 and has no copy. Pinned at the first such frame:
// nothing decays, 0 stale; without the pin (the text plant removes it) the guard decays between the reads and the next reading frame finds the image owed.
static void S16() {
  lasting::CopyStreaks st = Make(true, false);
  for (int n = 0; n < 600; ++n) {
    Syn s;
    if (n % 60 == 0) s.DrawOp({20});
    else { s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4); s.DrawOp({20}); }
    Handle2(st, s, n);
  }
  std::printf("read with no copy into the image every 60th frame: stale %llu, copies let go %llu (must be 0 / 0)\n", (unsigned long long)st.stale_read, (unsigned long long)st.guard_decayed);
  CHECK(st.stale_read == 0 && st.guard_decayed == 0);
}

// ---- round 2 (Opus, scratchpad k2r2: gap.cpp, di2.cpp, pin.cpp) ----
static bool g_plant_peek = false;   // --plant-peek: the run-end recovery ignores the next frame's copy table (keeps nothing)

// item 1: the gap split. The blind cadence rule on cadence 40: every stale read happens at a carry read 40 frames after the last one: ALL in the >16 bucket (Note() had moved `last`, the split read gap 0 for every one of them).
static void S17() {
  lasting::CopyStreaks st = Make(true, false);
  st.cadence_blind = true;
  for (int n = 0; n < 400; ++n) Handle2(st, MovingFrame(n, n % 40 == 0), n);
  std::printf("blind cadence 40: stale %llu, gap<=16 %llu, gap>16 %llu (all must be >16)\n", (unsigned long long)st.stale_read, (unsigned long long)st.stale_gap_le16, (unsigned long long)st.stale_gap_gt16);
  CHECK(st.stale_read >= 5 && st.stale_gap_gt16 == st.stale_read && st.stale_gap_le16 == 0);
}

// item 3: the kept copy clears the debt only when the frame is really skipped. Frame 9 is skipped (copy left out, committed), frame 10 Classify KEEPS the copy but T3 draws it instead (no CommitOwed): frame 10 reads frame 8's content: 1 stale.
static void S18() {
  for (int keep = 0; keep < 2; ++keep) {
    lasting::CopyStreaks st;
    st.defer_owed = true;
    for (int n = 0; n < 20; ++n) {
      Syn s;
      s.DrawOp({20});
      s.CopyOp(10, 20, 0, 0);
      LoadedFrame f = s.Frame();
      st.Note(f);
      if (n == 9) { std::vector<uint8_t> seed; st.Classify(f, false, seed, [](const rdr::CopyOp&) {}); st.CommitOwed(); continue; }
      if (n == 10) { std::vector<uint8_t> seed; st.Classify(f, keep != 0, seed, [](const rdr::CopyOp&) {}); }
      st.NoteDrawn(f);
    }
    std::printf("draw-instead frame 10 Classify %s the copy: stale %llu (must be 1)\n", keep ? "KEEPS" : "leaves out", (unsigned long long)st.stale_read);
    CHECK(st.stale_read == 1);
  }
}

// item 4: a pin and the monotone gap EXPIRE. A shadow-shaped image (read once at 0, moving copies and reads after them every frame) lets its copies go: variant A 1000 of ~1000 skipped frames alone; B one camera-cut frame
// (a read with no copy: pinned) and C a 600-frame pause (reads before the copy resume at 700: stale, pinned) must let go again after pin_expire clean frames. Without the expiry (the text plant pin_expire = huge) 0 let go.
static uint64_t LetGoAfter1000(int variant, uint64_t* stale_out) {
  lasting::CopyStreaks st = Make(true, false);
  uint64_t let0 = 0;
  for (int n = 0; n < 3000; ++n) {
    Syn s;
    const bool pause = variant == 2 && n >= 100 && n < 700;
    if (variant == 1 && n == 100) s.DrawOp({20});
    else if (!pause) { if (n == 0 || (variant == 2 && n == 700)) s.DrawOp({20}); s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4); s.DrawOp({20}); }
    if (n == 1000) let0 = st.guard_decayed;
    Handle2(st, s, n);
  }
  if (stale_out) *stale_out = st.stale_read;
  return st.guard_decayed - let0;
}
static void S19() {
  uint64_t sa = 0, sb = 0, sc = 0;
  const uint64_t a = LetGoAfter1000(0, &sa), b = LetGoAfter1000(1, &sb), c = LetGoAfter1000(2, &sc);
  std::printf("shadow-shaped: copies let go frames 1000..2999: A %llu, B (one camera-cut frame) %llu, C (600-frame pause) %llu; stale %llu / %llu / %llu\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)sa, (unsigned long long)sb, (unsigned long long)sc);
  CHECK(a >= 900 && b >= 900 && c >= 600);
  CHECK(sa == 0 && sb <= 1 && sc <= 1);
}

// item 4 (second half): the monotone gap tested on its own (pins ignored): alternating gaps 10/50 give at most ONE stale read; the last-interval-only plant gives many.
static void S20() {
  lasting::CopyStreaks st = Make(true, false);
  st.pin_blind = true;
  for (int n = 0; n < 1200; ++n) { const int m = n % 60; Handle2(st, MovingFrame(n, m == 0 || m == 10), n); }
  std::printf("monotone gap alone (pins ignored), alternating 10/50: stale %llu (must be <= 1)\n", (unsigned long long)st.stale_read);
  CHECK(st.stale_read <= 1);
}

// COUNTING: the episode count AND the per-read count AND the open gauge. A run end that is never written again: 1 episode, one stale read per later drawn frame, 1 image still open at the end.
static void S21() {
  lasting::CopyStreaks st = Make(false, false);
  for (int n = 0; n < 200; ++n) {
    Syn s;
    if (n < 60) s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4);
    s.DrawOp({20});
    Handle2(st, s, n);
  }
  std::printf("run end: episodes %llu, stale frames %llu, open now %zu\n", (unsigned long long)st.stale_read, (unsigned long long)st.stale_frames, st.StaleOpen());
  CHECK(st.stale_read == 1 && st.stale_frames >= 60 && st.StaleOpen() == 1);
}

// THE RECOVERY: the skipped frame that ends a copy run keeps its copy when the NEXT frame (queued) has no copy into the image. Frame 59 (skipped) is the last copy; frame 60 has none. With KeepRunEnds: 0 stale, image written; without
// (decay off and on, with and without a prior read): 1 episode and one stale read per later drawn frame. --plant-peek (the next table ignored) brings it back.
static uint64_t RunEndRecovery(bool decay, bool prior_read, bool recover, uint64_t* kept_out, uint64_t* frames_out) {
  lasting::CopyStreaks st = Make(decay, false);
  st.defer_owed = true;
  std::vector<Syn> frames;
  for (int n = 0; n < 200; ++n) {
    Syn s;
    if (n == 0 && prior_read) s.DrawOp({20});
    if (n < 60) s.CopyOp(10, 20, 120 + (n % 5) * 4, 64 + (n % 3) * 4);
    s.DrawOp({20});
    frames.push_back(s);
  }
  for (int n = 0; n < 200; ++n) {
    LoadedFrame f = frames[n].Frame();
    st.Note(f);
    if (n % 2 == 0) { st.NoteDrawn(f); continue; }
    std::vector<uint8_t> seed;
    st.Classify(f, false, seed, [](const rdr::CopyOp&) {});
    if (recover) {
      std::vector<rdr::CopyOp> next;
      if (n + 1 < 200 && !g_plant_peek) next = frames[n + 1].copies;
      else next = frames[n].copies;   // the peek ignored / no next frame: every destination counts as written next
      st.KeepRunEnds(f, seed, next);
    }
    st.CommitOwed();
  }
  if (kept_out) *kept_out = st.run_end_kept;
  if (frames_out) *frames_out = st.stale_frames;
  return st.stale_read;
}
static void S22() {
  uint64_t kept = 0, fr = 0, kept2 = 0, fr2 = 0, k3 = 0, f3 = 0;
  const uint64_t with_on = RunEndRecovery(true, true, true, &kept, &fr), without_on = RunEndRecovery(true, true, false, nullptr, &fr2);
  const uint64_t with_off = RunEndRecovery(false, false, true, &kept2, nullptr), without_off = RunEndRecovery(false, false, false, nullptr, &f3);
  (void)k3;
  std::printf("run end recovery: decay on %llu stale (frames %llu) against %llu (frames %llu) without; never read before its copy, decay off %llu against %llu without; copies kept %llu / %llu\n",
              (unsigned long long)with_on, (unsigned long long)fr, (unsigned long long)without_on, (unsigned long long)fr2, (unsigned long long)with_off, (unsigned long long)without_off, (unsigned long long)kept, (unsigned long long)kept2);
  CHECK(with_on == 0 && fr == 0 && kept >= 1);
  CHECK(without_on == 1 && fr2 >= 60);
  CHECK(with_off == 0 && without_off == 1 && f3 >= 60 && kept2 >= 1);
}

// ---- round 3 (reviewer's pt\rk.cpp, pt\rect.cpp) ----
// F1: the run-end keep clears the debt only when the frame is really skipped. Frame 9 skipped (copy into 20 left out, CommitOwed); frame 10 skip wanted, run_end_keep keeps the copy (frame 11 has none), T3 draws frame 10 instead (no CommitOwed):
// frame 10 reads frame 8's content: stale must be 1 with or without the keep (it was 0 with it).
static void S23() {
  for (int keep = 0; keep < 2; ++keep) {
    lasting::CopyStreaks st;
    st.defer_owed = true;
    for (int n = 0; n < 20; ++n) {
      Syn s;
      s.DrawOp({20});
      if (n <= 10) s.CopyOp(10, 20, 0, 0);
      LoadedFrame f = s.Frame();
      st.Note(f);
      if (n == 9) { std::vector<uint8_t> seed; st.Classify(f, false, seed, [](const rdr::CopyOp&) {}); st.CommitOwed(); continue; }
      if (n == 10) {
        std::vector<uint8_t> seed;
        st.Classify(f, false, seed, [](const rdr::CopyOp&) {});
        if (keep) { std::vector<rdr::CopyOp> next; st.KeepRunEnds(f, seed, next); }
      }
      st.NoteDrawn(f);
    }
    std::printf("run_end_keep %s, frame 10 drawn instead: stale %llu (must be 1)\n", keep ? "ON " : "off", (unsigned long long)st.stale_read);
    CHECK(st.stale_read == 1);
  }
}

// F2: the next-frame test and the debt are per RECTANGLE. Image 20 (an atlas) has rect x=0 (frames 0..9, the run ends on skipped frame 9) and rect x=64 (every frame). Skip the odd frames. The x=0 copy of frame 9 must be kept (1) and
// nothing stays owed.
static void S24() {
  lasting::CopyStreaks st;
  st.defer_owed = true;
  uint64_t kept = 0;
  std::vector<Syn> syn(30);
  for (int n = 0; n < 30; ++n) { syn[n].DrawOp({20}); if (n <= 9) syn[n].CopyOp(10, 20, 0, 0, 32, 32); syn[n].CopyOp(11, 20, 64, 0, 32, 32); }
  for (int n = 0; n < 30; ++n) {
    LoadedFrame f = syn[n].Frame();
    st.Note(f);
    if (n & 1) {
      std::vector<uint8_t> seed;
      st.Classify(f, false, seed, [](const rdr::CopyOp&) {});
      const std::vector<rdr::CopyOp>& next = syn[n + 1 < 30 ? n + 1 : n].copies;
      kept += st.KeepRunEnds(f, seed, next).size();
      st.CommitOwed();
      continue;
    }
    st.NoteDrawn(f);
  }
  std::printf("rect x=0 run ends on skipped frame 9 (rect x=64 goes on): kept %llu (want 1), stale %llu, owed now %zu\n", (unsigned long long)kept, (unsigned long long)st.stale_read, st.StaleOpen());
  CHECK(kept == 1);   // (the ledger itself is image-level: rect 64 keeps the image owed on every skipped frame)
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant-fast")) g_window = 1;
    else if (!std::strcmp(argv[i], "--plant-lean")) g_plant_lean = true;
    else if (!std::strcmp(argv[i], "--plant-noclear")) g_plant_noclear = true;
    else if (!std::strcmp(argv[i], "--plant-oldrule")) g_rule = 1, g_window = 1;
    else if (!std::strcmp(argv[i], "--plant-blind")) g_plant_blind = true;
    else if (!std::strcmp(argv[i], "--plant-peek")) g_plant_peek = true;
  S1();
  S2();
  S3();
  S4();
  S5();
  S1b();
  S6();
  S7();
  S8();
  S9();
  S10();
  S11();
  S12();
  S13();
  S14();
  S15();
  S16();
  S17();
  S18();
  S19();
  S20();
  S21();
  S22();
  S23();
  S24();
  S5b();
  std::printf("%s (window %u)\n", g_fail ? "FAILED" : "all passed", g_window);
  return g_fail ? 1 : 0;
}
