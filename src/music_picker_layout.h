// wwe2k14 - USER PLAYLIST music picker, VIEW side, part 1 (lane 292): the layout, the hit tests and the input mapping.
// Pure C++ (no ImGui, no game, no Windows / Android headers): the host test (tools\music\picker_view_test.cpp) builds this
// header with src\music_picker_view.h alone, and the ImGui view (src\music_picker_imgui.h) is a thin drawer over it.
//
// One unit `u` = height / 720 (PC 1080p: 1.5, 1440p: 2.0, a 2400 x 1080 phone: 1.5). Everything is laid out from u, the
// surface size and a SafeArea (display cutout / rounded corners / title safe), so one code path serves the PC window, a
// 21:9 monitor and a 20:9 phone in landscape.
//
// Devices (Poll() in music_picker_view.h):
//   pointer (mouse or touch, one pointer): press on a row's thumbnail -> kTogglePlay(row); press on the rest of a row ->
//     kTapRow(row) (choose); press on the footer's CANCEL -> kCancel; moving more than the slop before release turns the
//     press into a drag-scroll (no tap); release after a drag flings (kinetic scroll); the wheel scrolls.
//   keyboard: Up / Down / PageUp / PageDown / Home / End, Enter -> kSelect, Esc -> kCancel, Space or P -> kTogglePlay.
//   controller (XInput bits): D-pad / left stick up-down (with repeat), LB / RB -> pages, A -> kSelect, B / Back -> kCancel,
//     X -> kTogglePlay.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "music_picker_view.h"

namespace wwe2k14::music_picker {

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
  bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// Pixels kept clear at each edge of the surface (display cutout, rounded corners, TV / title safe).
struct SafeArea {
  float l = 0, t = 0, r = 0, b = 0;
};

// What the host knows about the display's unusable edges is clamped here, so a wrong number can never leave the panel without
// room: each edge at most a quarter of its dimension, both opposite edges together at most 60 %.
inline SafeArea ClampSafeArea(SafeArea s, float W, float H) {
  auto one = [](float v, float dim) { return std::isfinite(v) ? std::clamp(v, 0.0f, 0.25f * dim) : 0.0f; };
  s.l = one(s.l, W), s.r = one(s.r, W), s.t = one(s.t, H), s.b = one(s.b, H);
  if (s.l + s.r > 0.6f * W) s.l = s.r = 0.3f * W;
  if (s.t + s.b > 0.6f * H) s.t = s.b = 0.3f * H;
  return s;
}

// The heuristic used when the host gives none (the game's Java side does not report cutouts to native code): phones keep clear of
// the rounded corners and a side cutout (a 20:9 phone in landscape has its camera hole on one short edge); a PC window keeps the
// 2.5 % title safe area.
inline SafeArea DefaultSafeArea(float W, float H, bool touch) {
  if (touch) return ClampSafeArea({0.045f * W, 0.04f * H, 0.045f * W, 0.04f * H}, W, H);
  return ClampSafeArea({0.025f * W, 0.025f * H, 0.025f * W, 0.025f * H}, W, H);
}

struct Layout {
  float W = 0, H = 0;
  float u = 1;            // scale unit
  bool touch = false;
  SafeArea safe;
  Rect panel, header, list, footer;
  Rect cancel;            // footer button (CANCEL)
  float row_h = 0;        // one row's pitch
  float thumb = 0;        // thumbnail side
  float pad = 0;          // inner padding
  int rows_visible = 0;   // whole rows that fit in `list`
  float max_scroll = 0;   // content height - list height (>= 0)
  int n = 0;              // rows

  Rect RowRect(int i, float scroll) const {
    const float y = list.y0 + float(i) * row_h - scroll;
    return {list.x0, y, list.x1, y + row_h};
  }
  Rect ThumbRect(int i, float scroll) const {
    const Rect r = RowRect(i, scroll);
    const float side = thumb, y = r.y0 + (row_h - side) * 0.5f;
    return {r.x0 + pad, y, r.x0 + pad + side, y + side};
  }
};

inline Layout ComputeLayout(float W, float H, bool touch, SafeArea safe, int rows) {
  Layout L;
  L.W = W, L.H = H, L.touch = touch, L.n = std::max(0, rows);
  L.u = std::clamp(H / 720.0f, 0.4f, 6.0f);
  const float u = L.u;
  L.safe = ClampSafeArea(safe, W, H);
  // The panel: centred in the safe area, at most 1000 u wide (a 21:9 monitor / a long phone get side margins, not stretched rows)
  // and at most 640 u tall (the 360's Guide is a panel too, not a full page).
  const float aw = W - L.safe.l - L.safe.r, ah = H - L.safe.t - L.safe.b;
  const float pw = std::min(aw, 1000.0f * u), ph = std::min(ah, touch ? 700.0f * u : 640.0f * u);
  const float px0 = L.safe.l + (aw - pw) * 0.5f, py0 = L.safe.t + (ah - ph) * 0.5f;
  L.panel = {px0, py0, px0 + pw, py0 + ph};
  L.pad = 14.0f * u;
  // Rows: 56 u on PC; on touch at least 48 dp-ish. A phone's u is 1.5 at 1080 px and a 1080 px phone is ~ 2.6 density, so 74 u =
  // 111 px = 43 dp ... the thumbnail (the play target) is the full row less padding: ~ 100 px = 38 dp, plus the row body.
  L.row_h = (touch ? 78.0f : 60.0f) * u;
  L.thumb = L.row_h - 2.0f * (touch ? 8.0f : 7.0f) * u;
  const float head_h = (touch ? 64.0f : 58.0f) * u, foot_h = (touch ? 76.0f : 56.0f) * u;
  L.header = {L.panel.x0, L.panel.y0, L.panel.x1, L.panel.y0 + head_h};
  L.footer = {L.panel.x0, L.panel.y1 - foot_h, L.panel.x1, L.panel.y1};
  L.list = {L.panel.x0, L.header.y1 + 6.0f * u, L.panel.x1, L.footer.y0 - 6.0f * u};
  L.rows_visible = std::max(0, int(std::floor(L.list.h() / L.row_h)));
  L.max_scroll = std::max(0.0f, float(L.n) * L.row_h - L.list.h());
  // CANCEL: a slanted button at the footer's right end; on touch big enough to hit (>= 56 u tall, 180 u wide).
  const float bh = foot_h - (touch ? 14.0f : 12.0f) * u, bw = (touch ? 190.0f : 168.0f) * u;
  L.cancel = {L.footer.x1 - L.pad - bw, L.footer.y0 + (foot_h - bh) * 0.5f, L.footer.x1 - L.pad, L.footer.y0 + (foot_h - bh) * 0.5f + bh};
  return L;
}

enum class HitKind : uint8_t { kNone, kPanel, kCancel, kThumb, kRow };
struct Hit {
  HitKind kind = HitKind::kNone;
  int row = -1;
};

// What is under (x, y) with the list scrolled by `scroll` px. Rows only count inside the list area (a row half scrolled under the
// header does not catch taps that landed on the header).
inline Hit HitTest(const Layout& L, float scroll, float x, float y) {
  if (!L.panel.Contains(x, y)) return {};
  if (L.cancel.Contains(x, y)) return {HitKind::kCancel, -1};
  if (L.list.Contains(x, y)) {
    const float rel = y - L.list.y0 + scroll;
    const int row = int(std::floor(rel / L.row_h));
    if (rel >= 0 && row >= 0 && row < L.n) {
      // The play target is the thumbnail square, grown by half the padding all round so a thumb tap does not need to be exact.
      Rect t = L.ThumbRect(row, scroll);
      const float g = 0.5f * L.pad;
      t = {t.x0 - g, t.y0 - g, t.x1 + g, t.y1 + g};
      return {t.Contains(x, y) ? HitKind::kThumb : HitKind::kRow, row};
    }
  }
  return {HitKind::kPanel, -1};
}

// ---- input mapping --------------------------------------------------------------------------------------------------------

// One pointer sample per UI frame (the ImGui view fills it from the mouse / touch state).
struct PointerSample {
  float x = -1, y = -1;
  bool down = false;       // held now
  bool pressed = false;    // went down since the last sample
  bool released = false;   // went up since the last sample
  float wheel_rows = 0;    // wheel clicks (+ = later rows), already in row units
};

// XInput button bits (the same as photo_mode.h's comments).
namespace pad {
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kBack = 0x0020, kLb = 0x0100, kRb = 0x0200, kA = 0x1000, kB = 0x2000,
                   kX = 0x4000;
}

enum class Key : uint8_t { kUp, kDown, kPageUp, kPageDown, kEnter, kEscape, kSpace, kP };

class Interaction {
 public:
  // Per UI frame: lays out, moves the scroll and turns the pointer into inputs. `cursor` is the model's cursor (an outside cursor
  // move scrolls the list to show it, unless the player is dragging).
  void Frame(const Layout& L, int cursor, const PointerSample& p, float dt) {
    layout_ = L;
    max_scroll_ = L.max_scroll;
    clock_ += dt;
    if (p.pressed) last_pointer_t_ = clock_;
    const bool follow = scroll_cursor_pending_;  // last frame's scroll moved the cursor: do not scroll back to it
    scroll_cursor_pending_ = false;
    if (cursor != last_cursor_) {
      if (!dragging_ && !flinging_ && !follow) EnsureVisible(L, cursor);
      last_cursor_ = cursor;
    }
    hover_ = HitTest(L, scroll_, p.x, p.y);
    if (p.pressed) PointerDown(p);
    if (down_ && p.down) PointerMove(p, dt);
    if (down_ && p.released) PointerUp(p);
    if (!p.down && !p.released && down_) PointerCancel();
    if (p.wheel_rows != 0 && !down_) Scroll(L, p.wheel_rows * L.row_h);
    Fling(dt);
    ClampScroll();
    Flush();
  }

  // Keyboard (edge events from the overlay's key state): the mapped input, queued in order.
  void OnKey(Key k) { Push(false, k); }

  // The pad for this frame. `held` = the buttons down now and `stick_y` the left stick (XInput, up = positive): they drive only the
  // REPEAT of D-pad / stick up-down and LB / RB. `edges` = the buttons that went down since the last call, measured by the pad feed
  // (music_picker_ui.h FeedPad) against its own baseline: A / B / Back / X act ONLY on edges, so a button already held when the picker
  // opened (the A that opened it) is never a press, whatever order the game thread and the UI thread ran in. A D-pad press shorter
  // than a frame (an edge with nothing held now) moves once.
  void Pad(uint16_t held, int16_t stick_y, float dt, uint16_t edges) {
    int dir = 0;  // -1 up, +1 down (the list's own direction)
    if (held & pad::kDpadUp) dir = -1;
    if (held & pad::kDpadDown) dir = +1;
    if (!dir) dir = stick_y > 16000 ? -1 : stick_y < -16000 ? +1 : 0;  // XInput's thumb Y is up-positive
    int page = (held & pad::kLb) ? -1 : (held & pad::kRb) ? +1 : 0;
    if (!pad_primed_) {
      // The first call after opening: a direction already held then starts its repeat delay from here (no immediate move).
      pad_primed_ = true;
      // (a direction whose press is one of this call's edges was pressed AFTER the baseline: it moves now, no 0.4 s wait)
      const bool dir_edge = (dir < 0 && (edges & pad::kDpadUp)) || (dir > 0 && (edges & pad::kDpadDown));
      const bool page_edge = (page < 0 && (edges & pad::kLb)) || (page > 0 && (edges & pad::kRb));
      repeat_dir_ = dir_edge ? 0 : dir, repeat_t_ = 0.40f, repeat_page_ = page_edge ? 0 : page, repeat_pt_ = 0.45f;
    }
    if (edges & pad::kA) Push(true, Key::kEnter);
    if (edges & (pad::kB | pad::kBack)) Push(true, Key::kEscape);
    if (edges & pad::kX) Push(true, Key::kSpace);
    if (!dir) {
      if (edges & pad::kDpadUp) Push(true, Key::kUp);
      else if (edges & pad::kDpadDown) Push(true, Key::kDown);
    }
    if (!page) {
      if (edges & pad::kLb) Push(true, Key::kPageUp);
      else if (edges & pad::kRb) Push(true, Key::kPageDown);
    }
    Repeat(dir, repeat_dir_, repeat_t_, dt, 0.40f, 0.09f, [&] { Push(true, dir < 0 ? Key::kUp : Key::kDown); });
    Repeat(page, repeat_page_, repeat_pt_, dt, 0.45f, 0.18f, [&] { Push(true, page < 0 ? Key::kPageUp : Key::kPageDown); });
  }

  // A press of any mouse button, wherever it landed: the host's mouse filter turns clicks into pad buttons for the game, and those
  // must not also drive the picker for a moment.
  void NotePointer() { last_pointer_t_ = clock_; }

  std::vector<PickerInput> Take() {
    Flush();  // inputs pushed after this frame's Frame() (keyboard, pad) are handed over now, not one frame later
    std::vector<PickerInput> v;
    v.swap(taken_);
    return v;
  }

  void Reset() { *this = Interaction(); }

  float scroll() const { return scroll_; }
  bool dragging() const { return dragging_; }
  bool scrolling() const { return dragging_ || flinging_ || std::fabs(vel_) > 1.0f; }
  Hit hover() const { return hover_; }
  // The row / button held down right now (for the pressed look), none when no press is on a target.
  Hit pressed() const { return down_ && !dragging_ ? press_hit_ : Hit{}; }

  // Scroll so that `row` is wholly inside the list (nearest edge).
  void EnsureVisible(const Layout& L, int row) {
    if (row < 0 || row >= L.n) return;
    const float top = float(row) * L.row_h, bot = top + L.row_h;
    if (top < scroll_) scroll_ = top;
    else if (bot > scroll_ + L.list.h()) scroll_ = bot - L.list.h();
    ClampScroll();
    vel_ = 0;
  }

 private:
  void PointerDown(const PointerSample& p) {
    press_hit_ = HitTest(layout_, scroll_, p.x, p.y);
    down_ = true, dragging_ = false, flinging_ = false, vel_ = 0;
    press_x_ = last_x_ = p.x, press_y_ = last_y_ = p.y;
    drag_acc_ = 0;
    sample_t_ = 0;
  }
  void PointerMove(const PointerSample& p, float dt) {
    const float slop = (layout_.touch ? 14.0f : 6.0f) * layout_.u;
    if (!dragging_ && std::fabs(p.y - press_y_) > slop && layout_.max_scroll > 0 &&
        (press_hit_.kind == HitKind::kRow || press_hit_.kind == HitKind::kThumb || press_hit_.kind == HitKind::kPanel)) {
      dragging_ = true;
      last_y_ = press_y_;  // no jump: the drag starts where the finger first went down
    }
    if (dragging_) {
      const float dy = last_y_ - p.y;  // finger up -> later rows
      vel_ = 0.6f * vel_ + 0.4f * dy / std::clamp(dt, 1e-3f, 0.1f);
      Scroll(layout_, dy);
    }
    last_x_ = p.x, last_y_ = p.y;
  }
  void PointerUp(const PointerSample& p) {
    const bool was_drag = dragging_;
    down_ = false;
    dragging_ = false;
    if (was_drag) {
      flinging_ = std::fabs(vel_) > 300.0f * layout_.u;
      if (!flinging_) vel_ = 0;
      return;
    }
    // A tap: the press and the release must land on the same target.
    const Hit up = HitTest(layout_, scroll_, p.x, p.y);
    if (up.kind != press_hit_.kind || up.row != press_hit_.row) return;
    PickerInput in;
    switch (up.kind) {
      case HitKind::kCancel: in.kind = InputKind::kCancel; break;
      case HitKind::kThumb: in.kind = InputKind::kTogglePlay, in.row = up.row; break;
      case HitKind::kRow: in.kind = InputKind::kTapRow, in.row = up.row; break;
      default: return;
    }
    out_.push_back(in);
  }
  void PointerCancel() {
    down_ = false, dragging_ = false;
  }
  void Scroll(const Layout& L, float dy) {
    const float before = scroll_;
    scroll_ = std::clamp(scroll_ + dy, 0.0f, L.max_scroll);
    NoteRows(before, scroll_);
  }
  void Fling(float dt) {
    if (!flinging_) return;
    const float before = scroll_;
    scroll_ = std::clamp(scroll_ + vel_ * dt, 0.0f, max_scroll_);
    vel_ *= std::exp(-4.2f * dt);
    if (std::fabs(vel_) < 24.0f * layout_.u || scroll_ <= 0.0f || scroll_ >= max_scroll_) flinging_ = false, vel_ = 0;
    NoteRows(before, scroll_);
  }
  void ClampScroll() { scroll_ = std::clamp(scroll_, 0.0f, max_scroll_); }
  // Whole rows passed -> kScroll (the logic moves the cursor with the view). The row boundary is counted on the scroll position.
  void NoteRows(float before, float after) {
    const int a = int(std::floor(before / layout_.row_h)), b = int(std::floor(after / layout_.row_h));
    if (b != a) {
      PickerInput in;
      in.kind = InputKind::kScroll, in.scroll_rows = b - a;
      out_.push_back(in);
      scroll_cursor_pending_ = true;  // the cursor change this causes must not scroll the view back
    }
  }
  void Flush() {
    // merge consecutive kScroll of one frame
    std::vector<PickerInput> merged;
    for (const PickerInput& in : out_) {
      if (in.kind == InputKind::kScroll && !merged.empty() && merged.back().kind == InputKind::kScroll)
        merged.back().scroll_rows += in.scroll_rows;
      else
        merged.push_back(in);
    }
    for (PickerInput& in : merged)
      if (!(in.kind == InputKind::kScroll && in.scroll_rows == 0)) taken_.push_back(in);
    out_.clear();
  }
  // One mapped input. The pad's and the keyboard's streams can describe the same key press (the SDK's keyboard driver feeds the
  // game's pad, the picker reads the keyboard itself too), and the host's mouse filter turns clicks into pad buttons: a pad input
  // within 0.25 s of a pointer press is dropped, and a pad / keyboard pair of one kind within a short window counts once.
  void Push(bool from_pad, Key k) {
    PickerInput in;
    switch (k) {
      case Key::kUp: in.kind = InputKind::kUp; break;
      case Key::kDown: in.kind = InputKind::kDown; break;
      case Key::kPageUp: in.kind = InputKind::kPageUp; break;
      case Key::kPageDown: in.kind = InputKind::kPageDown; break;
      case Key::kEnter: in.kind = InputKind::kSelect; break;
      case Key::kEscape: in.kind = InputKind::kCancel; break;
      case Key::kSpace:
      case Key::kP: in.kind = InputKind::kTogglePlay, in.row = -1; break;
    }
    const int kind = int(in.kind) & 15;
    const bool repeating = in.kind == InputKind::kUp || in.kind == InputKind::kDown || in.kind == InputKind::kPageUp || in.kind == InputKind::kPageDown;
    const float window = repeating ? 0.07f : 0.15f;
    float* mine = from_pad ? pad_t_ : key_t_;
    const float* other = from_pad ? key_t_ : pad_t_;
    if (from_pad && clock_ - last_pointer_t_ < 0.25f) return;
    if (clock_ - other[kind] < window) return;
    mine[kind] = clock_;
    out_.push_back(in);
  }
  template <class F>
  static void Repeat(int dir, int& held, float& t, float dt, float first, float next, F fire) {
    if (!dir) {
      held = 0, t = 0;
      return;
    }
    if (dir != held) {
      held = dir, t = first;
      fire();
      return;
    }
    t -= dt;
    if (t <= 0) {
      t += next;
      fire();
    }
  }

  Layout layout_;
  float scroll_ = 0, vel_ = 0, max_scroll_ = 0;
  bool down_ = false, dragging_ = false, flinging_ = false, scroll_cursor_pending_ = false;
  Hit press_hit_, hover_;
  float press_x_ = 0, press_y_ = 0, last_x_ = 0, last_y_ = 0, drag_acc_ = 0, sample_t_ = 0;
  int last_cursor_ = -1;
  bool pad_primed_ = false;
  float clock_ = 0, last_pointer_t_ = -1e9f;
  float pad_t_[16] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
  float key_t_[16] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
  int repeat_dir_ = 0, repeat_page_ = 0;
  float repeat_t_ = 0, repeat_pt_ = 0;
  std::vector<PickerInput> out_, taken_;
};

}  // namespace wwe2k14::music_picker
