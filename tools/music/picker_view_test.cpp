// Lane 292: host test of the music picker view's layout, hit tests and input mapping (src\music_picker_layout.h). No ImGui, no game.
//   clang++ -std=c++20 -Isrc tools\music\picker_view_test.cpp -o out\l292\picker_view_test.exe   (build_picker_shots.ps1 does this)
#include <cstdio>
#include <cstdlib>

#include "music_picker_layout.h"
#include "music_picker_ui.h"

using namespace wwe2k14::music_picker;

static int g_fail = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    if (!(c)) {                                                           \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);                    \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

// An Interaction whose Pad() works out the edges from the previous call, the way FeedPad does between two reads (the real pad feed is
// tested in TestOpenRace). Pad(held, ly, dt, edges) with explicit edges is the base class's.
struct PI : Interaction {
  uint16_t prev = 0;
  void Pad(uint16_t b, int16_t ly, float dt) {
    Interaction::Pad(b, ly, dt, uint16_t(b & ~prev));
    prev = b;
  }
  void Pad(uint16_t b, int16_t ly, float dt, uint16_t extra) {
    Interaction::Pad(b, ly, dt, uint16_t((b & ~prev) | extra));
    prev = b;
  }
};

struct Sizes {
  float W, H;
  bool touch;
};

PointerSample Move(float x, float y, bool down = false) {
  PointerSample p;
  p.x = x, p.y = y, p.down = down;
  return p;
}

// One full tap (press frame, release frame) at (x, y).
std::vector<PickerInput> Tap(Interaction& in, const Layout& L, int cursor, float x, float y, float rx = -1, float ry = -1) {
  PointerSample d = Move(x, y, true);
  d.pressed = true;
  in.Frame(L, cursor, d, 1.0f / 60);
  PointerSample u = Move(rx < 0 ? x : rx, ry < 0 ? y : ry, false);
  u.released = true;
  in.Frame(L, cursor, u, 1.0f / 60);
  return in.Take();
}

Rect Center(const Rect& r) { return {(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, 0, 0}; }

void TestSafeArea() {
  SafeArea s = ClampSafeArea({-5, 99999, 1e30f, NAN}, 1000, 400);
  CHECK(s.l == 0 && s.t == 100 && s.r == 250 && s.b == 0);
  s = ClampSafeArea({240, 100, 240, 100}, 1000, 400);  // 480 of 1000 < 60 %: kept; 200 of 400 < 60 %
  CHECK(s.l == 240 && s.r == 240);
  s = ClampSafeArea({250, 90, 250, 90}, 400, 400);  // clamped to 100 each, 200 of 400 < 60 %
  CHECK(s.l == 100 && s.r == 100);
  s = ClampSafeArea({100, 100, 100, 100}, 100, 100);  // 25 each at most
  CHECK(s.l <= 25 && s.l + s.r <= 60);
  const SafeArea p = DefaultSafeArea(2400, 1080, true);
  CHECK(p.l >= 90 && p.l == p.r && p.t > 30);
}

void TestLayout() {
  const Sizes sizes[] = {{2400, 1080, true}, {1920, 1080, true}, {2340, 1080, true}, {1920, 1080, false}, {2560, 1440, false}, {2560, 1080, false}, {1280, 720, false}, {1024, 768, false}};
  for (const Sizes& s : sizes) {
    const SafeArea sa = DefaultSafeArea(s.W, s.H, s.touch);
    for (int n : {0, 1, 8, 40}) {
      const Layout L = ComputeLayout(s.W, s.H, s.touch, sa, n);
      // The panel sits inside the safe area and the screen.
      CHECK(L.panel.x0 >= sa.l - 0.01f && L.panel.x1 <= s.W - sa.r + 0.01f && L.panel.y0 >= sa.t - 0.01f && L.panel.y1 <= s.H - sa.b + 0.01f);
      // Header, list, footer stack without overlap, inside the panel.
      CHECK(L.header.y1 <= L.list.y0 && L.list.y1 <= L.footer.y0 && L.footer.y1 <= L.panel.y1 + 0.01f);
      CHECK(L.list.h() > 3 * L.row_h);  // at least three rows visible
      // CANCEL inside the footer.
      CHECK(L.cancel.x0 >= L.footer.x0 && L.cancel.x1 <= L.footer.x1 && L.cancel.y0 >= L.footer.y0 && L.cancel.y1 <= L.footer.y1);
      // Touch targets: a phone at 1080 px is ~2.6x density, 1 dp = ~2.6 px. Rows and the cancel button >= 48 dp (=125 px) would be
      // generous; the Material minimum is 48 dp for the smallest target: demand row >= 40 dp, thumbnail >= 36 dp, CANCEL >= 44 dp.
      if (s.touch) {
        const float dp = s.H / 411.0f;  // 1080 px tall landscape ~ 411 dp tall (a 2.63 density phone)
        CHECK(L.row_h >= 40 * dp * 0.9f && L.thumb >= 36 * dp * 0.7f && L.cancel.h() >= 44 * dp * 0.7f);
        CHECK(L.cancel.w() >= 100 * dp * 0.6f);
      }
      CHECK(L.panel.w() <= 1000 * L.u + 0.01f);  // ultra-wide: side margins, not stretched rows
      CHECK(L.max_scroll >= 0 && (n * L.row_h <= L.list.h() ? L.max_scroll == 0 : L.max_scroll > 0));
    }
  }
  // The same code for the phone and the PC: u follows the height.
  CHECK(std::fabs(ComputeLayout(2400, 1080, true, {}, 8).u - 1.5f) < 1e-4f);
  CHECK(std::fabs(ComputeLayout(2560, 1440, false, {}, 8).u - 2.0f) < 1e-4f);
  // A hostile safe area (the host reports nonsense) still leaves a usable panel.
  const Layout bad = ComputeLayout(2400, 1080, true, {5000, 5000, 5000, 5000}, 8);
  CHECK(bad.panel.w() > 600 && bad.panel.h() > 300 && bad.list.h() > 2 * bad.row_h);
}

void TestHit() {
  const Layout L = ComputeLayout(2400, 1080, true, DefaultSafeArea(2400, 1080, true), 40);
  const Rect t3 = L.ThumbRect(3, 0), r3 = L.RowRect(3, 0);
  CHECK(HitTest(L, 0, Center(t3).x0, Center(t3).y0).kind == HitKind::kThumb && HitTest(L, 0, Center(t3).x0, Center(t3).y0).row == 3);
  const Hit body = HitTest(L, 0, r3.x1 - 40, (r3.y0 + r3.y1) / 2);
  CHECK(body.kind == HitKind::kRow && body.row == 3);
  CHECK(HitTest(L, 0, (L.header.x0 + L.header.x1) / 2, (L.header.y0 + L.header.y1) / 2).kind == HitKind::kPanel);
  CHECK(HitTest(L, 0, Center(L.cancel).x0, Center(L.cancel).y0).kind == HitKind::kCancel);
  CHECK(HitTest(L, 0, 5, 5).kind == HitKind::kNone);
  // Scrolled by 2.5 rows: row 2 is half visible; what is under the list top is row 2, a point above the list is the header.
  const float sc = 2.5f * L.row_h;
  const Hit h = HitTest(L, sc, L.list.x1 - 40, L.list.y0 + 2);
  CHECK(h.kind == HitKind::kRow && h.row == 2);
  CHECK(HitTest(L, sc, L.list.x1 - 40, L.list.y0 - 4).kind == HitKind::kPanel);
  // Past the last row of a short list: the panel, not a row.
  const Layout S = ComputeLayout(2400, 1080, true, DefaultSafeArea(2400, 1080, true), 2);
  CHECK(HitTest(S, 0, S.list.x1 - 40, S.list.y0 + 2.5f * S.row_h).kind == HitKind::kPanel);
  // The play target is a bit bigger than the drawn thumbnail (half the padding all round).
  CHECK(HitTest(L, 0, t3.x0 - 0.3f * L.pad, (t3.y0 + t3.y1) / 2).kind == HitKind::kThumb);
  CHECK(HitTest(L, 0, t3.x1 + 0.9f * L.pad, (t3.y0 + t3.y1) / 2).kind == HitKind::kRow);
}

void TestTaps() {
  const Layout L = ComputeLayout(2400, 1080, true, DefaultSafeArea(2400, 1080, true), 40);
  PI in;
  in.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  // Tap on the play button of row 2 -> kTogglePlay(2); the cursor is not touched, nothing is chosen.
  Rect t = L.ThumbRect(2, 0);
  auto v = Tap(in, L, 0, Center(t).x0, Center(t).y0);
  CHECK(v.size() == 1 && v[0].kind == InputKind::kTogglePlay && v[0].row == 2);
  // Tap on the row body -> kTapRow(2) (the logic chooses it).
  const Rect r = L.RowRect(2, 0);
  v = Tap(in, L, 0, r.x1 - 60, (r.y0 + r.y1) / 2);
  CHECK(v.size() == 1 && v[0].kind == InputKind::kTapRow && v[0].row == 2);
  // CANCEL.
  v = Tap(in, L, 0, Center(L.cancel).x0, Center(L.cancel).y0);
  CHECK(v.size() == 1 && v[0].kind == InputKind::kCancel);
  // Press on a row, release on another thing -> no input (a slide-off cancels the tap) unless it turned into a drag.
  v = Tap(in, L, 0, r.x1 - 60, (r.y0 + r.y1) / 2, Center(L.cancel).x0, Center(L.cancel).y0);
  CHECK(v.empty());
  // Tap in the header or outside: nothing.
  v = Tap(in, L, 0, (L.header.x0 + L.header.x1) / 2, (L.header.y0 + L.header.y1) / 2);
  CHECK(v.empty());
  v = Tap(in, L, 0, 3, 3);
  CHECK(v.empty());
  // A tiny wobble (less than the slop) is still a tap.
  v = Tap(in, L, 0, Center(t).x0, Center(t).y0, Center(t).x0 + 4, Center(t).y0 + 6);
  CHECK(v.size() == 1 && v[0].kind == InputKind::kTogglePlay);
}

void TestDrag() {
  const Layout L = ComputeLayout(2400, 1080, true, DefaultSafeArea(2400, 1080, true), 40);
  PI in;
  in.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  const Rect r = L.RowRect(1, 0);
  const float x = r.x1 - 80;
  float y = (r.y0 + r.y1) / 2;
  PointerSample d = Move(x, y, true);
  d.pressed = true;
  in.Frame(L, 0, d, 1.0f / 60);
  // Drag the finger up by 3.4 rows over 20 frames (slowly: no fling at the release).
  const float total = 3.4f * L.row_h;
  int rows = 0;
  for (int i = 1; i <= 20; ++i) {
    in.Frame(L, 0, Move(x, y - total * float(i) / 20.0f, true), 1.0f / 60);
    for (auto e : in.Take()) {
      CHECK(e.kind == InputKind::kScroll && e.scroll_rows > 0);
      rows += e.scroll_rows;
    }
  }
  CHECK(in.dragging());
  CHECK(rows == 3);  // 3.4 rows crossed three row boundaries
  CHECK(std::fabs(in.scroll() - total) < total * 0.12f + 1);  // the view followed the finger (less the slop-free start)
  PointerSample u = Move(x, y - total, false);
  u.released = true;
  in.Frame(L, 3, u, 1.0f / 60);
  auto after = in.Take();
  for (auto e : after) CHECK(e.kind == InputKind::kScroll);  // never a tap after a drag
  CHECK(!in.dragging());
  // Drag down at the top: clamped at 0, no negative rows.
  PI top;
  top.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  PointerSample d2 = Move(x, y, true);
  d2.pressed = true;
  top.Frame(L, 0, d2, 1.0f / 60);
  for (int i = 1; i <= 10; ++i) top.Frame(L, 0, Move(x, y + 30.0f * float(i), true), 1.0f / 60);
  CHECK(top.scroll() == 0);
  for (auto e : top.Take()) CHECK(e.kind != InputKind::kScroll || e.scroll_rows == 0);
  // Drag to the bottom: clamped at max_scroll.
  PI bot;
  bot.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  PointerSample d3 = Move(x, L.list.y1 - 10, true);
  d3.pressed = true;
  bot.Frame(L, 0, d3, 1.0f / 60);
  for (int i = 1; i <= 800; ++i) bot.Frame(L, 0, Move(x, L.list.y1 - 10 - 30.0f * float(i) * 0.5f, true), 1.0f / 60);
  CHECK(std::fabs(bot.scroll() - L.max_scroll) < 0.01f);
}

void TestFling() {
  const Layout L = ComputeLayout(2400, 1080, true, DefaultSafeArea(2400, 1080, true), 40);
  PI in;
  in.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  const float x = L.list.x1 - 80;
  float y = L.list.y1 - 20;
  PointerSample d = Move(x, y, true);
  d.pressed = true;
  in.Frame(L, 0, d, 1.0f / 60);
  for (int i = 1; i <= 6; ++i) in.Frame(L, 0, Move(x, y - 60.0f * float(i), true), 1.0f / 60);  // 3600 px/s upward
  in.Take();
  PointerSample u = Move(x, y - 360, false);
  u.released = true;
  in.Frame(L, 0, u, 1.0f / 60);
  const float at_release = in.scroll();
  int more_rows = 0;
  for (int i = 0; i < 120; ++i) {
    in.Frame(L, 0, Move(-1, -1), 1.0f / 60);
    for (auto e : in.Take()) {
      CHECK(e.kind == InputKind::kScroll);
      more_rows += e.scroll_rows;
    }
  }
  CHECK(in.scroll() > at_release + 2 * L.row_h);  // it kept going
  CHECK(!in.scrolling());                         // and stopped
  CHECK(more_rows >= 2);
  // A slow release does not fling.
  PI slow;
  slow.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  slow.Frame(L, 0, d, 1.0f / 60);
  for (int i = 1; i <= 20; ++i) slow.Frame(L, 0, Move(x, y - 3.0f * float(i), true), 1.0f / 6);  // 18 px/s
  PointerSample u2 = Move(x, y - 60, false);
  u2.released = true;
  slow.Frame(L, 0, u2, 1.0f / 60);
  const float s0 = slow.scroll();
  for (int i = 0; i < 30; ++i) slow.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  CHECK(std::fabs(slow.scroll() - s0) < 0.01f);
}

void TestWheelAndCursor() {
  const Layout L = ComputeLayout(1920, 1080, false, DefaultSafeArea(1920, 1080, false), 40);
  PI in;
  PointerSample w = Move(L.list.x0 + 100, L.list.y0 + 100);
  w.wheel_rows = 2;
  in.Frame(L, 0, w, 1.0f / 60);
  CHECK(std::fabs(in.scroll() - 2 * L.row_h) < 0.01f);
  auto v = in.Take();
  CHECK(v.size() == 1 && v[0].kind == InputKind::kScroll && v[0].scroll_rows == 2);
  // A cursor moved by the logic (keyboard / pad) scrolls the view to show it, once; a cursor that follows a scroll does not scroll back.
  PI k;
  k.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  k.Frame(L, 30, Move(-1, -1), 1.0f / 60);
  CHECK(k.scroll() > 0);
  const float bottom_edge = 31 * L.row_h - L.list.h();
  CHECK(std::fabs(k.scroll() - bottom_edge) < 0.5f);  // row 30 sits at the list's foot
  k.Frame(L, 2, Move(-1, -1), 1.0f / 60);
  CHECK(std::fabs(k.scroll() - 2 * L.row_h) < 0.5f);  // scrolled up just enough to show row 2 at the top
  PI fol;
  fol.Frame(L, 5, Move(-1, -1), 1.0f / 60);
  PointerSample w2 = Move(L.list.x0 + 100, L.list.y0 + 100);
  w2.wheel_rows = 3;
  fol.Frame(L, 5, w2, 1.0f / 60);
  fol.Take();
  const float sc = fol.scroll();
  fol.Frame(L, 8, Move(-1, -1), 1.0f / 60);  // the logic moved the cursor 3 rows with the view
  CHECK(std::fabs(fol.scroll() - sc) < 0.01f);
  // ...and a later genuine cursor move does scroll again.
  fol.Frame(L, 20, Move(-1, -1), 1.0f / 60);
  CHECK(fol.scroll() > sc);
}

void TestKeysAndPad() {
  PI in;
  struct { Key k; InputKind want; } keys[] = {{Key::kUp, InputKind::kUp},       {Key::kDown, InputKind::kDown}, {Key::kPageUp, InputKind::kPageUp},
                                              {Key::kPageDown, InputKind::kPageDown}, {Key::kEnter, InputKind::kSelect}, {Key::kEscape, InputKind::kCancel},
                                              {Key::kSpace, InputKind::kTogglePlay}, {Key::kP, InputKind::kTogglePlay}};
  const Layout L = ComputeLayout(1920, 1080, false, {}, 8);
  for (auto& e : keys) {
    in.OnKey(e.k);
    in.Frame(L, 0, Move(-1, -1), 1.0f / 60);
    auto v = in.Take();
    CHECK(v.size() == 1 && v[0].kind == e.want);
    if (e.want == InputKind::kTogglePlay) CHECK(v[0].row == -1);  // the cursor row
  }
  // Pad: A / B / Back / X edges, once per press.
  PI p;
  p.Pad(0, 0, 1.0f / 60);  // (the first frame only primes the held state)
  p.Pad(pad::kA, 0, 1.0f / 60);
  p.Pad(pad::kA, 0, 1.0f / 60);  // held: no second input
  p.Pad(0, 0, 1.0f / 60);
  p.Pad(pad::kB, 0, 1.0f / 60);
  p.Pad(0, 0, 1.0f / 60);
  p.Pad(pad::kBack, 0, 1.0f / 60);
  p.Pad(0, 0, 1.0f / 60);
  p.Pad(pad::kX, 0, 1.0f / 60);
  p.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  auto v = p.Take();
  CHECK(v.size() == 4);
  CHECK(v.size() == 4 && v[0].kind == InputKind::kSelect && v[1].kind == InputKind::kCancel && v[2].kind == InputKind::kCancel && v[3].kind == InputKind::kTogglePlay);
  // D-pad down: first press at once, then nothing until 0.40 s, then every 0.09 s.
  PI r;
  r.Pad(0, 0, 1.0f / 60);
  int fired = 0;
  r.Pad(pad::kDpadDown, 0, 1.0f / 60);
  r.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  fired += int(r.Take().size());
  CHECK(fired == 1);
  for (int i = 0; i < 20; ++i) r.Pad(pad::kDpadDown, 0, 1.0f / 60);  // 0.33 s
  r.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  CHECK(r.Take().empty());
  for (int i = 0; i < 60; ++i) r.Pad(pad::kDpadDown, 0, 1.0f / 60);  // to 1.33 s
  r.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  const auto many = r.Take();
  CHECK(many.size() >= 9 && many.size() <= 12);  // (1.33 - 0.40) / 0.09 ~ 10
  for (auto e : many) CHECK(e.kind == InputKind::kDown);
  // The left stick (XInput: up positive) and LB / RB.
  PI s;
  s.Pad(0, 0, 1.0f / 60);
  s.Pad(0, 20000, 1.0f / 60);
  s.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  auto sv = s.Take();
  CHECK(sv.size() == 1 && sv[0].kind == InputKind::kUp);
  PI s2;
  s2.Pad(0, 0, 1.0f / 60);
  s2.Pad(0, -20000, 1.0f / 60);
  s2.Pad(0, 5000, 1.0f / 60);  // inside the dead zone: released
  s2.Pad(0, -20000, 1.0f / 60);
  s2.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  auto sv2 = s2.Take();
  CHECK(sv2.size() == 2 && sv2[0].kind == InputKind::kDown && sv2[1].kind == InputKind::kDown);
  PI b;
  b.Pad(0, 0, 1.0f / 60);
  b.Pad(pad::kRb, 0, 1.0f / 60);
  b.Pad(0, 0, 1.0f / 60);
  b.Pad(pad::kLb, 0, 1.0f / 60);
  b.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  auto bv = b.Take();
  CHECK(bv.size() == 2 && bv[0].kind == InputKind::kPageDown && bv[1].kind == InputKind::kPageUp);
}

void TestMouseOnPc() {
  // The same pointer path serves the mouse: a click on the thumbnail plays, a click on the body chooses, a small drag with the button
  // held scrolls (6 u slop on PC).
  const Layout L = ComputeLayout(1920, 1080, false, DefaultSafeArea(1920, 1080, false), 40);
  PI in;
  in.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  auto v = Tap(in, L, 0, Center(L.ThumbRect(1, 0)).x0, Center(L.ThumbRect(1, 0)).y0);
  CHECK(v.size() == 1 && v[0].kind == InputKind::kTogglePlay && v[0].row == 1);
  CHECK(L.cancel.w() > 100);
  // Hover reports what is under the mouse.
  in.Frame(L, 0, Move(Center(L.ThumbRect(4, 0)).x0, Center(L.ThumbRect(4, 0)).y0), 1.0f / 60);
  CHECK(in.hover().kind == HitKind::kThumb && in.hover().row == 4);
  // (a button held at open is FeedPad's baseline, not an edge: see TestOpenRace)
}

void TestSources() {
  const Layout L = ComputeLayout(1920, 1080, false, DefaultSafeArea(1920, 1080, false), 8);
  // A press shorter than a frame (down and up between two UI frames) still counts through extra_edges.
  PI a;
  a.Pad(0, 0, 1.0f / 60);
  a.Pad(0, 0, 1.0f / 60, pad::kA);
  a.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  auto v = a.Take();
  CHECK(v.size() == 1 && v[0].kind == InputKind::kSelect);
  // The same key seen by the keyboard path and through the game's pad (the SDK's keyboard driver) counts once, in either order.
  PI b;
  b.Pad(0, 0, 1.0f / 60);
  b.OnKey(Key::kSpace);
  b.Pad(pad::kX, 0, 1.0f / 60);
  b.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  v = b.Take();
  CHECK(v.size() == 1 && v[0].kind == InputKind::kTogglePlay);
  PI c;
  c.Pad(0, 0, 1.0f / 60);
  c.Pad(pad::kX, 0, 1.0f / 60);
  c.OnKey(Key::kP);
  c.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  v = c.Take();
  CHECK(v.size() == 1);
  // Two real presses 0.3 s apart are two inputs.
  PI d;
  d.Pad(0, 0, 1.0f / 60);
  d.Pad(pad::kX, 0, 1.0f / 60);
  d.Pad(0, 0, 1.0f / 60);
  for (int i = 0; i < 18; ++i) d.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  d.Pad(pad::kX, 0, 1.0f / 60);
  d.Frame(L, 0, Move(-1, -1), 1.0f / 60);
  CHECK(d.Take().size() == 2);
  // A mouse click that the host's mouse filter turned into pad A (and a right click into pad B) must not also drive the picker.
  PI m;
  m.Pad(0, 0, 1.0f / 60);
  PointerSample down = Move(10, 10, true);
  down.pressed = true;
  m.Frame(L, 0, down, 1.0f / 60);
  m.Pad(pad::kA, 0, 1.0f / 60);
  m.Pad(0, 0, 1.0f / 60);
  m.NotePointer();  // (a right / middle click: the view reports it)
  m.Pad(pad::kB, 0, 1.0f / 60);
  m.Frame(L, 0, Move(10, 10), 1.0f / 60);
  CHECK(m.Take().empty());
  for (int i = 0; i < 30; ++i) m.Frame(L, 0, Move(10, 10), 1.0f / 60);
  m.Pad(pad::kA, 0, 1.0f / 60);
  m.Frame(L, 0, Move(10, 10), 1.0f / 60);
  v = m.Take();
  CHECK(v.size() == 1 && v[0].kind == InputKind::kSelect);  // half a second later the pad counts again
}


// The reviewer's case (lane 292 review B1): the UI draws BEFORE the game thread's first pad read after the picker opened, while the A
// that opened it is still held. Mirrors ImGuiPickerView::Draw's pad step (Pad with the feed's edges; the real Draw also skips it until FeedPad is primed, which this mirror deliberately
// does not, so the Interaction alone must hold) and the pad gate's FeedPad, in the orders the two threads can run in.
void DrawPad(Interaction& in, const Layout& L) {
  PointerSample ps;
  in.Frame(L, 0, ps, 1.0f / 60);
  in.Pad(uint16_t(g_pad_buttons.load()), int16_t(g_pad_stick_y.load()), 1.0f / 60, uint16_t(g_pad_edges.exchange(0)));
}
// The re-review's harness (open_race2): the same pad step as the real Draw (skipped until FeedPad is primed).
void DrawPadReal(Interaction& in, const Layout& L) {
  PointerSample ps;
  in.Frame(L, 0, ps, 1.0f / 60);
  if (g_pad_primed.load()) in.Pad(uint16_t(g_pad_buttons.load()), int16_t(g_pad_stick_y.load()), 1.0f / 60, uint16_t(g_pad_edges.exchange(0)));
}
int CountKind(const std::vector<PickerInput>& v, InputKind k) {
  int n = 0;
  for (auto& i : v) n += i.kind == k;
  return n;
}
void TestOpenRace2() {
  const Layout L = ComputeLayout(1920, 1080, false, DefaultSafeArea(1920, 1080, false), 10);
  {  // 1: draw before the first pad read, A held then released: no choice
    Interaction in;
    in.Reset(), ResetPadFeed();
    std::vector<PickerInput> all;
    DrawPadReal(in, L);
    FeedPad(pad::kA, 0), DrawPadReal(in, L);
    FeedPad(pad::kA, 0), DrawPadReal(in, L);
    FeedPad(0, 0), DrawPadReal(in, L);
    for (auto& x : in.Take()) all.push_back(x);
    CHECK(CountKind(all, InputKind::kSelect) == 0);
  }
  {  // 4: an edge nobody drew (the last open ended by a force close), then a re-open with A held
    Interaction in;
    in.Reset(), ResetPadFeed();
    FeedPad(0, 0), FeedPad(pad::kA, 0);
    Interaction in2;
    in2.Reset(), ResetPadFeed();
    DrawPadReal(in2, L);
    FeedPad(pad::kA, 0), DrawPadReal(in2, L), DrawPadReal(in2, L);
    CHECK(CountKind(in2.Take(), InputKind::kSelect) == 0);
  }
  {  // 6: a D-pad press held across two frames moves once
    Interaction in;
    in.Reset(), ResetPadFeed();
    FeedPad(0, 0), DrawPadReal(in, L), in.Take();
    FeedPad(pad::kDpadDown, 0), DrawPadReal(in, L);
    FeedPad(pad::kDpadDown, 0), DrawPadReal(in, L);
    CHECK(CountKind(in.Take(), InputKind::kDown) == 1);
  }
  {  // 7: D-pad pressed after the baseline and still held at the first draw: moves at once (not after the 0.4 s repeat delay), once
    Interaction in;
    in.Reset(), ResetPadFeed();
    FeedPad(0, 0), FeedPad(pad::kDpadDown, 0);
    DrawPadReal(in, L), DrawPadReal(in, L);
    CHECK(CountKind(in.Take(), InputKind::kDown) == 1);
  }
  {  // 8: B held at open, released, pressed again = one cancel
    Interaction in;
    in.Reset(), ResetPadFeed();
    FeedPad(pad::kB, 0), DrawPadReal(in, L);
    FeedPad(0, 0), DrawPadReal(in, L);
    FeedPad(pad::kB, 0), DrawPadReal(in, L), DrawPadReal(in, L);
    CHECK(CountKind(in.Take(), InputKind::kCancel) == 1);
  }
}

// SF2: the closing press must not reach the game as keystrokes either.
void TestLatchKeystrokes() {
  g_latch_buttons.store(0);
  CHECK(!LatchBlocksKeystroke(0x1, 0x5800) && !LatchBlocksKeystroke(0x5, 0x5800));  // no latch: nothing is blocked
  CHECK(PadBitOfVk(0x5800) == pad::kA && PadBitOfVk(0x5801) == pad::kB && PadBitOfVk(0x5802) == pad::kX);
  CHECK(PadBitOfVk(0x5810) == pad::kDpadUp && PadBitOfVk(0x5811) == pad::kDpadDown && PadBitOfVk(0x0041) == 0);
  g_latch_buttons.store(pad::kA);  // A held at close
  CHECK(LatchBlocksKeystroke(0x1, 0x5800));         // its KEYDOWN
  CHECK(LatchBlocksKeystroke(0x1 | 0x4, 0x5800));   // its KEYDOWN | REPEAT
  CHECK(!LatchBlocksKeystroke(0x1, 0x5801));        // another button's KEYDOWN passes
  CHECK(LatchBlocksKeystroke(0x1 | 0x4, 0x5801));   // but any REPEAT is dropped while latched
  CHECK(!LatchBlocksKeystroke(0x1, 0x0041));        // a non-pad key
  g_latch_buttons.store(0);
}

void TestOpenRace() {
  const Layout L = ComputeLayout(1920, 1080, false, DefaultSafeArea(1920, 1080, false), 10);
  {  // draw first, then the game's first read (A held), then more draws: no input
    Interaction in;
    ResetPadFeed();
    DrawPad(in, L);
    CHECK(in.Take().empty());
    FeedPad(pad::kA, 0);
    DrawPad(in, L);
    DrawPad(in, L);
    CHECK(in.Take().empty());
    FeedPad(pad::kA, 0);  // still held on later reads
    DrawPad(in, L);
    CHECK(in.Take().empty());
    FeedPad(0, 0);  // released, then pressed: now it counts
    FeedPad(pad::kA, 0);
    DrawPad(in, L);
    auto v = in.Take();
    CHECK(v.size() == 1 && v[0].kind == InputKind::kSelect);
  }
  {  // the game reads first
    Interaction in;
    ResetPadFeed();
    FeedPad(pad::kA, 0);
    DrawPad(in, L);
    FeedPad(pad::kA, 0);
    DrawPad(in, L);
    DrawPad(in, L);
    CHECK(in.Take().empty());
  }
  {  // two presses between two draws (press, release, press): the second is not lost to the "held" state; both edges merge into one A
    Interaction in;
    ResetPadFeed();
    FeedPad(0, 0);
    DrawPad(in, L);
    FeedPad(pad::kA, 0), FeedPad(0, 0), FeedPad(pad::kA, 0);
    DrawPad(in, L);
    DrawPad(in, L);
    CHECK(in.Take().size() == 1);
  }
  {  // a D-pad press shorter than a frame moves once; a held direction at open does not move until its repeat delay
    Interaction in;
    ResetPadFeed();
    FeedPad(0, 0);
    DrawPad(in, L);
    FeedPad(pad::kDpadDown, 0), FeedPad(0, 0);
    DrawPad(in, L);
    auto v = in.Take();
    CHECK(v.size() == 1 && v[0].kind == InputKind::kDown);
    Interaction held;
    ResetPadFeed();
    FeedPad(pad::kDpadDown, 0);
    DrawPad(held, L);
    FeedPad(pad::kDpadDown, 0);
    DrawPad(held, L);
    CHECK(held.Take().empty());
  }
}

}  // namespace

int main() {
  TestSafeArea();
  TestLayout();
  TestHit();
  TestTaps();
  TestDrag();
  TestFling();
  TestWheelAndCursor();
  TestKeysAndPad();
  TestMouseOnPc();
  TestSources();
  TestOpenRace();
  TestOpenRace2();
  TestLatchKeystrokes();
  if (g_fail) {
    std::printf("picker_view_test: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("picker_view_test: all checks passed\n");
  return 0;
}
