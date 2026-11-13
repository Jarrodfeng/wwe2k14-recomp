// wwe2k14 - USER PLAYLIST music picker, VIEW side, part 2 (lane 292): the ImGui view. Header-only so the offline picture tool
// (tools\music\picker_shots.cpp) and the game build compile the same code; the game's factory is src\music_picker_ui.cpp.
//
// Look (taken from the launchers, src\launcher\launcher_main.cpp / launcher_theme.h, Jarrod 2026-11-13 "take inspirations visually
// from the launcher"): near-black panel, the red gradient header with the slanted white edge, the selected row as a slanted red bar,
// slanted red buttons with a white rim, dim grey text turning white on the selected row, the game's own menu face (font000 of
// pac\menu\fontHD.pac read from the player's game files at run time, with a drop shadow; the fallback is ImGui's font).
// Each row: a square thumbnail on the left (the song's album art, or a generic vinyl record drawn as a vector) with a PLAY / STOP
// button over it, then the name and artist. Nothing is drawn from game assets except that font.
//
// Must run inside an ImGui frame (ImGuiDialog::OnDraw); draws to the foreground draw list at io.DisplaySize.
#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "music_picker_assets.h"
#include "music_picker_layout.h"
#include "music_picker_ui.h"
#include "music_picker_view.h"

namespace wwe2k14::music_picker {

namespace pal {  // the launcher's colours (launcher_theme.h: kBg, kRed*; launcher_main.cpp: kMenuRed, the grey text steps)
constexpr ImU32 kBg = IM_COL32(17, 18, 21, 255), kBgLift = IM_COL32(27, 28, 33, 255), kDot = IM_COL32(34, 36, 41, 255);
constexpr ImU32 kRedTop = IM_COL32(178, 19, 26, 255), kRedBottom = IM_COL32(120, 11, 17, 255), kMenuRed = IM_COL32(214, 22, 30, 255);
constexpr ImU32 kRedHotTop = IM_COL32(205, 30, 38, 255), kRedHotBottom = IM_COL32(118, 12, 18, 255);
constexpr ImU32 kRedDownTop = IM_COL32(112, 12, 18, 255), kRedDownBottom = IM_COL32(64, 6, 10, 255);
constexpr ImU32 kTextDim = IM_COL32(141, 142, 147, 255), kTextHover = IM_COL32(205, 206, 211, 255), kWhite = IM_COL32(255, 255, 255, 255);
}  // namespace pal

#if defined(__ANDROID__)
inline constexpr bool kTouchDefault = true;
#else
inline constexpr bool kTouchDefault = false;
#endif

class ImGuiPickerView final : public PickerView {
 public:
  explicit ImGuiPickerView(ViewHost host) : host_(std::move(host)) {}

  void OnOpen() override {
    inter_.Reset();
    anim_ = 0;
    kb_armed_ = false;  // an Enter held from opening the picker (past its auto-repeat) is not a choice
    ResetPadFeed();
  }
  // GAME thread (the pad gate in src\music_picker.h): recorded in atomics, read in Draw on the UI thread.
  void OnPadState(const PadState& s) override { FeedPad(s.buttons, s.ly); }
  void OnClose() override {}  // (the covers stay in the process cache: the overlay layer keeps every uploaded texture until exit)
  // `id` identifies the cover (the host derives it from the file's path, size and mtime), so a cover seen before is not decoded or uploaded
  // again at the next open (review S4). The cache holds at most kMaxArt covers per process; past that rows keep the record.
  void SetArtRgba(uint32_t id, int w, int h, const std::vector<uint8_t>& rgba) override {
    auto& art = Art();
    if (!host_.upload || id == 0 || art.count(id) || art.size() >= kMaxArt || w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4) return;
    art[id] = host_.upload(w, h, rgba.data());  // (decoded on a worker: the UI thread only uploads)
  }
  void SetArt(uint32_t id, const std::vector<uint8_t>& encoded) override {
    auto& art = Art();
    if (!host_.upload || id == 0 || art.count(id) || art.size() >= kMaxArt) return;
    Rgba img;
    GpuImage g;
    if (DecodeCover(encoded.data(), encoded.size(), 160, img)) g = host_.upload(img.w, img.h, img.px.data());
    art[id] = std::move(g);  // an empty GpuImage = "no art": the row keeps the record
  }
  std::vector<PickerInput> Poll() override { return inter_.Take(); }

  void Draw(const PickerModel& m, const FrameInfo& f) override {
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (!(W >= 64) || !(H >= 64)) return;
    const float dt = std::clamp(f.dt, 0.0f, 0.1f);
    anim_ += dt;
    EnsureFont();

    // ---- input -> inter_ ------------------------------------------------------------------------------------------------------
    PointerSample ps;
    const bool valid = io.MousePos.x > -1e5f && io.MousePos.y > -1e5f;
    ps.x = valid ? io.MousePos.x : -1, ps.y = valid ? io.MousePos.y : -1;
    ps.down = io.MouseDown[0], ps.pressed = ImGui::IsMouseClicked(0, false), ps.released = ImGui::IsMouseReleased(0);
    ps.wheel_rows = -io.MouseWheel * 2.0f;
    if (ps.pressed) device_ = (kTouchDefault || f.touch) ? Device::kTouch : Device::kMouse;
    if (f.touch) device_ = Device::kTouch;
    if (f.controller && device_ == Device::kMouse) device_ = Device::kPad;
    touch_ = kTouchDefault || device_ == Device::kTouch;

    const int n = m.empty ? 0 : int(m.songs.size());
    const SafeArea sa = host_.safe_area ? host_.safe_area(W, H) : DefaultSafeArea(W, H, touch_);
    const Layout L = ComputeLayout(W, H, touch_, sa, n);
    inter_.Frame(L, m.cursor, ps, dt);
    if (ImGui::IsMouseClicked(1, false) || ImGui::IsMouseClicked(2, false)) inter_.NotePointer();
    bool kb = false;
    // Keyboard actions count only after every action key has been seen up once since the picker opened.
    if (!kb_armed_ && !ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) && !ImGui::IsKeyDown(ImGuiKey_Escape) &&
        !ImGui::IsKeyDown(ImGuiKey_Space) && !ImGui::IsKeyDown(ImGuiKey_P))
      kb_armed_ = true;
    auto key = [&](ImGuiKey k, Key out, bool repeat, bool action) {
      if (action && !kb_armed_) return;
      if (ImGui::IsKeyPressed(k, repeat)) inter_.OnKey(out), kb = true;
    };
    key(ImGuiKey_UpArrow, Key::kUp, true, false);
    key(ImGuiKey_DownArrow, Key::kDown, true, false);
    key(ImGuiKey_PageUp, Key::kPageUp, true, false);
    key(ImGuiKey_PageDown, Key::kPageDown, true, false);
    key(ImGuiKey_Enter, Key::kEnter, false, true);
    key(ImGuiKey_KeypadEnter, Key::kEnter, false, true);
    key(ImGuiKey_Escape, Key::kEscape, false, true);
    key(ImGuiKey_Space, Key::kSpace, false, true);
    key(ImGuiKey_P, Key::kP, false, true);
    // The pad is read only once the game thread has delivered its first state after the picker opened (FeedPad's baseline): before that
    // there is no baseline to measure presses against, and the A that opened the picker is still down.
    const uint16_t pad_buttons = uint16_t(g_pad_buttons.load(std::memory_order_relaxed));
    if (g_pad_primed.load(std::memory_order_acquire)) {
      inter_.Pad(pad_buttons, int16_t(g_pad_stick_y.load(std::memory_order_relaxed)), dt, uint16_t(g_pad_edges.exchange(0, std::memory_order_relaxed)));
      if (pad_buttons) device_ = Device::kPad;
    }
    if (kb) device_ = Device::kKeyboard;

    Paint(m, L);
  }

 private:
  enum class Device { kMouse, kKeyboard, kPad, kTouch };

  // ---- text ---------------------------------------------------------------------------------------------------------------------
  static uint32_t NextCp(const char*& p) {
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3Fu >> extra);
    for (int i = 0; i < extra && (static_cast<unsigned char>(*p) & 0xC0) == 0x80; ++i) cp = cp << 6 | (*p++ & 0x3F);
    return cp;
  }
  static std::string Upper(const std::string& s) {
    std::string o = s;
    for (char& c : o)
      if (c >= 'a' && c <= 'z') c = char(c - 32);
    return o;
  }
  bool Covered(const char* s) const {
    for (const char* p = s; *p;) {
      const uint32_t cp = NextCp(p);
      if (cp != ' ' && !font_.glyphs.count(cp)) return false;
    }
    return true;
  }
  bool UseGameFont(const char* s) const { return font_tex_.id && Covered(s); }
  float TextW(const char* s, float px) const {
    if (UseGameFont(s)) {
      const float k = px / font_.cell;
      float w = 0;
      for (const char* p = s; *p;) {
        const auto it = font_.glyphs.find(NextCp(p));
        w += (it != font_.glyphs.end() ? it->second.w : font_.cell * 0.33f) * k;
      }
      return w;
    }
    return ImGui::GetFont()->CalcTextSizeA(px, 1e9f, 0.0f, s).x;
  }
  void TextRaw(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* s) const {
    if (!UseGameFont(s)) {
      dl->AddText(ImGui::GetFont(), px, pos, col, s);
      return;
    }
    const float k = px / font_.cell, iw = 1.0f / float(font_.sheet.w), ih = 1.0f / float(font_.sheet.h);
    const ImTextureRef tex(static_cast<ImTextureID>(font_tex_.id));
    float x = std::floor(pos.x);
    const float y = std::floor(pos.y);
    for (const char* p = s; *p;) {
      const auto it = font_.glyphs.find(NextCp(p));
      if (it == font_.glyphs.end()) {
        x += font_.cell * 0.33f * k;
        continue;
      }
      const MenuFont::Glyph& g = it->second;
      dl->AddImage(tex, ImVec2(x, y), ImVec2(x + g.w * k, y + g.h * k), ImVec2((g.x + 0.5f) * iw, (g.y + 0.5f) * ih),
                   ImVec2((g.x + g.w - 0.5f) * iw, (g.y + g.h - 0.5f) * ih), col);
      x += g.w * k;
    }
  }
  // With the game's drop shadow (the launcher's MenuText).
  void Text(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* s) const {
    const float o = std::max(1.0f, px * 0.06f);
    TextRaw(dl, ImVec2(pos.x + o, pos.y + o), px, IM_COL32(0, 0, 0, 190), s);
    TextRaw(dl, pos, px, col, s);
  }
  // `s` cut with "..." so that it fits max_w.
  std::string Fit(const std::string& s, float px, float max_w) const {
    if (TextW(s.c_str(), px) <= max_w) return s;
    std::vector<size_t> cuts;  // byte offsets of codepoint starts
    for (size_t i = 0; i < s.size(); ++i)
      if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) cuts.push_back(i);
    for (size_t k = cuts.size(); k-- > 0;) {
      const std::string t = s.substr(0, cuts[k]) + "...";
      if (TextW(t.c_str(), px) <= max_w) return t;
    }
    return "...";
  }

  // ---- shapes -------------------------------------------------------------------------------------------------------------------
  static void GradQuad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 top, ImU32 bottom) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6, 4);
    const ImDrawIdx i = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteIdx(i), dl->PrimWriteIdx(ImDrawIdx(i + 1)), dl->PrimWriteIdx(ImDrawIdx(i + 2));
    dl->PrimWriteIdx(i), dl->PrimWriteIdx(ImDrawIdx(i + 2)), dl->PrimWriteIdx(ImDrawIdx(i + 3));
    dl->PrimWriteVtx(p0, uv, top), dl->PrimWriteVtx(p1, uv, top), dl->PrimWriteVtx(p2, uv, bottom), dl->PrimWriteVtx(p3, uv, bottom);
  }
  // The launcher's slanted button: red gradient, white rim, the label in the menu face (SlantButton).
  void SlantButton(ImDrawList* dl, const Rect& r, const char* label, bool hover, bool held, float u) const {
    const float s = r.h() * 0.25f;
    const ImVec2 q[4] = {{r.x0 + s, r.y0}, {r.x1, r.y0}, {r.x1 - s, r.y1}, {r.x0, r.y1}};
    ImU32 top = pal::kRedTop, bottom = pal::kRedBottom;
    if (held) top = pal::kRedDownTop, bottom = pal::kRedDownBottom;
    else if (hover) top = pal::kRedHotTop, bottom = pal::kRedHotBottom;
    GradQuad(dl, q[0], q[1], q[2], q[3], top, bottom);
    dl->AddPolyline(q, 4, pal::kWhite, ImDrawFlags_Closed, std::max(1.5f, 2.2f * u));
    const float px = r.h() * 0.42f;
    const std::string t = Fit(label, px, r.w() - 2 * s);
    Text(dl, ImVec2(r.x0 + (r.w() - TextW(t.c_str(), px)) * 0.5f + s * 0.1f, r.y0 + (r.h() - px) * 0.5f), px, pal::kWhite, t.c_str());
  }

  // The generic record: a vinyl disc with grooves, a sheen that turns while the row plays, a red label. Drawn in `r` (a square).
  void Record(ImDrawList* dl, const Rect& r, bool playing, float u) const {
    const float side = r.w();
    const ImVec2 c((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f);
    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), IM_COL32(31, 32, 38, 255), 4.0f * u);
    const float R = side * 0.46f;
    dl->AddCircleFilled(c, R, IM_COL32(9, 9, 11, 255), 48);
    for (float f : {0.93f, 0.84f, 0.75f, 0.66f, 0.57f}) dl->AddCircle(c, R * f, IM_COL32(255, 255, 255, 16), 48, 1.0f);
    const float spin = playing ? anim_ * 1.6f : 0.6f;  // radians; a still record has its sheen at rest
    for (int k = 0; k < 2; ++k) {                      // two opposite sheen wedges
      const float a0 = spin + float(k) * 3.14159265f, w = 0.5f;
      ImVec2 pts[10];
      pts[0] = c;
      for (int i = 0; i < 9; ++i) {
        const float a = a0 - w + 2 * w * float(i) / 8.0f;
        pts[i + 1] = ImVec2(c.x + std::cos(a) * R * 0.97f, c.y + std::sin(a) * R * 0.97f);
      }
      dl->AddConvexPolyFilled(pts, 10, IM_COL32(255, 255, 255, 20));
    }
    dl->AddCircleFilled(c, R * 0.36f, pal::kMenuRed, 36);
    dl->AddCircle(c, R * 0.36f, IM_COL32(255, 255, 255, 110), 36, std::max(1.0f, 1.2f * u));
    dl->AddCircle(c, R * 0.25f, IM_COL32(0, 0, 0, 60), 36, std::max(1.0f, 1.0f * u));
    dl->AddCircleFilled(c, R * 0.065f, IM_COL32(17, 18, 21, 255), 16);
    dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), IM_COL32(255, 255, 255, 28), 4.0f * u, 0, 1.0f);
  }

  // PLAY / STOP over the thumbnail. Playing: red disc, a white stop square and an arc that circles the button.
  void PlayButton(ImDrawList* dl, const Rect& t, bool playing, bool hover, bool held, float u) const {
    const ImVec2 c((t.x0 + t.x1) * 0.5f, (t.y0 + t.y1) * 0.5f);
    float r = t.w() * 0.31f;
    if (hover) r *= 1.07f;
    if (held) r *= 0.93f;
    dl->AddCircleFilled(c, r, playing ? IM_COL32(214, 22, 30, 238) : IM_COL32(0, 0, 0, hover ? 190 : 160), 40);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, hover || playing ? 255 : 215), 40, std::max(1.5f, 2.0f * u));
    if (playing) {
      const float h = r * 0.40f;
      dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), pal::kWhite, 2.0f * u);
      const float a = anim_ * 4.0f;
      dl->PathArcTo(c, r + 4.0f * u, a, a + 1.7f, 20);
      dl->PathStroke(IM_COL32(255, 255, 255, 230), 0, std::max(2.0f, 2.6f * u));
    } else {
      const ImVec2 a(c.x - r * 0.28f, c.y - r * 0.44f), b(c.x - r * 0.28f, c.y + r * 0.44f), d(c.x + r * 0.48f, c.y);
      dl->AddTriangleFilled(a, b, d, pal::kWhite);
    }
  }

  void EqBars(ImDrawList* dl, float x, float y_mid, float h, float u) const {
    for (int i = 0; i < 4; ++i) {
      const float k = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(anim_ * (5.0f + 1.3f * float(i)) + float(i) * 1.7f));
      const float bw = 4.0f * u, gap = 3.0f * u, bx = x + float(i) * (bw + gap), bh = h * k;
      dl->AddRectFilled(ImVec2(bx, y_mid + h * 0.5f - bh), ImVec2(bx + bw, y_mid + h * 0.5f), pal::kWhite, 1.0f * u);
    }
  }

  // A key / button badge for the hint line.
  float HintItem(ImDrawList* dl, float x, const Rect& foot, const char* badge, ImU32 badge_col, const char* label, float px, float u,
                 bool draw) const {
    const float cy = (foot.y0 + foot.y1) * 0.5f, br = px * 0.78f;
    float bw = br * 2;
    const bool pill = std::strlen(badge) > 1;
    if (pill) bw = TextW(badge, px * 0.7f) + px * 0.9f;
    const float gap = px * 0.45f, tw = TextW(label, px);
    if (draw) {
      if (pill) {
        dl->AddRectFilled(ImVec2(x, cy - br), ImVec2(x + bw, cy + br), badge_col, br * 0.5f);
        dl->AddRect(ImVec2(x, cy - br), ImVec2(x + bw, cy + br), IM_COL32(255, 255, 255, 150), br * 0.5f, 0, std::max(1.0f, 1.2f * u));
        TextRaw(dl, ImVec2(x + (bw - TextW(badge, px * 0.7f)) * 0.5f, cy - px * 0.35f), px * 0.7f, pal::kWhite, badge);
      } else {
        dl->AddCircleFilled(ImVec2(x + br, cy), br, badge_col, 28);
        dl->AddCircle(ImVec2(x + br, cy), br, IM_COL32(255, 255, 255, 190), 28, std::max(1.0f, 1.3f * u));
        TextRaw(dl, ImVec2(x + br - TextW(badge, px * 0.8f) * 0.5f, cy - px * 0.4f), px * 0.8f, pal::kWhite, badge);
      }
      Text(dl, ImVec2(x + bw + gap, cy - px * 0.5f), px, pal::kTextHover, label);
    }
    return bw + gap + tw;
  }

  // ---- the picture --------------------------------------------------------------------------------------------------------------
  void Paint(const PickerModel& m, const Layout& L) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = L.u;
    const Rect& P = L.panel;
    const int n = L.n;
    const float scroll = inter_.scroll();
    playing_any_ = false;  // set again by the playing row's paint below (the header's record spins with it)

    // Dim the menu underneath; the panel's shadow; the body.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(L.W, L.H), IM_COL32(0, 0, 0, 160));
    for (int i = 4; i >= 1; --i)
      dl->AddRectFilled(ImVec2(P.x0 - 5.0f * i * u, P.y0 - 3.0f * i * u), ImVec2(P.x1 + 5.0f * i * u, P.y1 + 7.0f * i * u),
                        IM_COL32(0, 0, 0, 26), 8.0f * u);
    GradQuad(dl, ImVec2(P.x0, P.y0), ImVec2(P.x1, P.y0), ImVec2(P.x1, P.y1), ImVec2(P.x0, P.y1), pal::kBgLift, pal::kBg);
    dl->AddRect(ImVec2(P.x0, P.y0), ImVec2(P.x1, P.y1), IM_COL32(255, 255, 255, 38), 0, 0, std::max(1.0f, 1.2f * u));

    // Rows (clipped to the list).
    dl->PushClipRect(ImVec2(L.list.x0, L.list.y0), ImVec2(L.list.x1, L.list.y1), true);
    if (n == 0) PaintEmpty(dl, m, L);
    const Hit hover = inter_.hover(), held = inter_.pressed();
    const int first = std::max(0, int(std::floor(scroll / L.row_h))), last = std::min(n - 1, int(std::floor((scroll + L.list.h()) / L.row_h)));
    for (int i = first; i <= last && n > 0; ++i) PaintRow(dl, m, L, i, scroll, hover, held);
    // Soft fades at the list's ends when it scrolls.
    if (L.max_scroll > 0) {
      const float fh = 14.0f * u;
      if (scroll > 1) dl->AddRectFilledMultiColor(ImVec2(L.list.x0, L.list.y0), ImVec2(L.list.x1, L.list.y0 + fh), IM_COL32(20, 21, 25, 235), IM_COL32(20, 21, 25, 235), IM_COL32(20, 21, 25, 0), IM_COL32(20, 21, 25, 0));
      if (scroll < L.max_scroll - 1) dl->AddRectFilledMultiColor(ImVec2(L.list.x0, L.list.y1 - fh), ImVec2(L.list.x1, L.list.y1), IM_COL32(18, 19, 22, 0), IM_COL32(18, 19, 22, 0), IM_COL32(18, 19, 22, 235), IM_COL32(18, 19, 22, 235));
      const float tx = L.list.x1 - 7.0f * u, ty0 = L.list.y0 + 4.0f * u, th = L.list.h() - 8.0f * u;
      const float thumb_h = std::max(24.0f * u, th * L.list.h() / (L.list.h() + L.max_scroll));
      const float ty = ty0 + (th - thumb_h) * (scroll / L.max_scroll);
      dl->AddRectFilled(ImVec2(tx, ty0), ImVec2(tx + 4.0f * u, ty0 + th), IM_COL32(255, 255, 255, 22), 2.0f * u);
      dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + 4.0f * u, ty + thumb_h), inter_.scrolling() ? pal::kTextHover : IM_COL32(205, 206, 211, 120), 2.0f * u);
    }
    // A model message under the list (not the empty-library text): a strip at the list's foot.
    if (n > 0 && !m.message.empty()) {
      const float px = 15.0f * u, h = px * 2.0f;
      dl->AddRectFilled(ImVec2(L.list.x0 + L.pad, L.list.y1 - h - 4.0f * u), ImVec2(L.list.x1 - L.pad, L.list.y1 - 4.0f * u), IM_COL32(20, 21, 25, 240), 4.0f * u);
      const std::string t = Fit(Upper(m.message), px, L.list.w() - 4 * L.pad);
      Text(dl, ImVec2(L.list.x0 + 2 * L.pad, L.list.y1 - h * 0.5f - 4.0f * u - px * 0.5f), px, IM_COL32(255, 205, 90, 255), t.c_str());
    }
    dl->PopClipRect();

    PaintHeader(dl, m, L);
    PaintFooter(dl, m, L);
  }

  void PaintHeader(ImDrawList* dl, const PickerModel& m, const Layout& L) const {
    const float u = L.u;
    const Rect& h = L.header;
    const float slant = 24.0f * u;
    const ImVec2 q[4] = {{h.x0, h.y0}, {h.x1, h.y0}, {h.x1 - slant, h.y1}, {h.x0, h.y1}};
    GradQuad(dl, q[0], q[1], q[2], q[3], pal::kRedTop, pal::kRedBottom);
    dl->AddLine(ImVec2(h.x1, h.y0), ImVec2(h.x1 - slant, h.y1), IM_COL32(255, 255, 255, 170), std::max(1.5f, 1.6f * u));
    // The record mark (our own vector), then the title.
    const float side = h.h() * 0.66f, pad = (h.h() - side) * 0.5f;
    Record(dl, Rect{h.x0 + L.pad, h.y0 + pad, h.x0 + L.pad + side, h.y0 + pad + side}, inter_.scrolling() ? false : playing_any_, u);
    const float tpx = h.h() * 0.46f, tx = h.x0 + L.pad + side + L.pad;
    const float sub_w = m.subtitle.empty() ? 0.0f : std::min(h.w() * 0.42f, TextW(Upper(m.subtitle).c_str(), h.h() * 0.24f));
    const std::string title = Fit(Upper(m.title.empty() ? "MUSIC LIBRARY" : m.title), tpx, h.w() - (tx - h.x0) - slant - sub_w - 2 * L.pad);
    Text(dl, ImVec2(tx, h.y0 + (h.h() - tpx) * 0.5f), tpx, pal::kWhite, title.c_str());
    if (!m.subtitle.empty()) {
      const float spx = h.h() * 0.24f;
      const std::string sub = Fit(Upper(m.subtitle), spx, h.w() * 0.42f);
      TextRaw(dl, ImVec2(h.x1 - slant - L.pad - TextW(sub.c_str(), spx), h.y0 + (h.h() - spx) * 0.5f), spx, IM_COL32(255, 255, 255, 215), sub.c_str());
    }
  }

  void PaintFooter(ImDrawList* dl, const PickerModel& m, const Layout& L) const {
    const float u = L.u;
    const Rect& f = L.footer;
    GradQuad(dl, ImVec2(f.x0, f.y0), ImVec2(f.x1, f.y0), ImVec2(f.x1, f.y1), ImVec2(f.x0, f.y1), IM_COL32(11, 11, 14, 255), IM_COL32(21, 22, 26, 255));
    dl->AddLine(ImVec2(f.x0, f.y0), ImVec2(f.x1, f.y0), IM_COL32(255, 255, 255, 34), std::max(1.0f, 1.2f * u));
    const Hit hv = inter_.hover(), hd = inter_.pressed();
    SlantButton(dl, L.cancel, "CANCEL", hv.kind == HitKind::kCancel, hd.kind == HitKind::kCancel, u);
    struct Item {
      const char* badge;
      ImU32 col;
      const char* label;
    };
    std::vector<Item> items;
    const ImU32 green = IM_COL32(52, 150, 62, 255), red = IM_COL32(190, 24, 32, 255), blue = IM_COL32(40, 100, 200, 255), grey = IM_COL32(70, 72, 80, 255);
    if (touch_) {
      items = {{"PLAY", IM_COL32(0, 0, 0, 170), "TAP TO LISTEN"}, {"TAP", grey, "A SONG TO CHOOSE IT"}};
    } else if (device_ == Device::kPad) {
      items = {{"A", green, "SELECT"}, {"X", blue, "PLAY / STOP"}, {"B", red, "CANCEL"}, {"LB RB", grey, "PAGE"}};
    } else {
      items = {{"ENTER", grey, "SELECT"}, {"SPACE", grey, "PLAY / STOP"}, {"ESC", grey, "CANCEL"}, {"UP DOWN", grey, "MOVE"}};
    }
    const float px = std::min(f.h() * 0.30f, 20.0f * u * (touch_ ? 1.0f : 1.0f));
    const float room = (L.cancel.x0 - f.x0) - 2 * L.pad;
    size_t cnt = items.size();
    auto total = [&](size_t k) {
      float w = 0;
      for (size_t i = 0; i < k; ++i) w += HintItem(dl, 0, f, items[i].badge, items[i].col, items[i].label, px, u, false) + (i ? px * 1.4f : 0);
      return w;
    };
    while (cnt > 1 && total(cnt) > room) --cnt;
    float x = f.x0 + L.pad;
    for (size_t i = 0; i < cnt; ++i) {
      x += HintItem(dl, x, f, items[i].badge, items[i].col, items[i].label, px, u, true) + px * 1.4f;
    }
    (void)m;
  }

  void PaintEmpty(ImDrawList* dl, const PickerModel& m, const Layout& L) const {
    const float u = L.u;
    const float side = std::min(L.list.h() * 0.38f, 150.0f * u);
    const float cx = (L.list.x0 + L.list.x1) * 0.5f, cy = L.list.y0 + L.list.h() * 0.40f;
    Record(dl, Rect{cx - side * 0.5f, cy - side * 0.5f, cx + side * 0.5f, cy + side * 0.5f}, false, u);
    std::string msg = m.message.empty() ? "NO SONGS YET. ADD MP3 FILES IN THE LAUNCHER: MODS > MUSIC." : Upper(m.message);
    // Two lines at most, broken at a space near the middle when it does not fit.
    const float px = 20.0f * u, maxw = L.list.w() - 4 * L.pad;
    std::vector<std::string> lines;
    if (TextW(msg.c_str(), px) <= maxw) lines.push_back(msg);
    else {
      size_t best = std::string::npos;
      for (size_t i = 0; i < msg.size(); ++i)
        if (msg[i] == ' ' && TextW(msg.substr(0, i).c_str(), px) <= maxw) best = i;
      if (best == std::string::npos) lines.push_back(Fit(msg, px, maxw));
      else {
        lines.push_back(msg.substr(0, best));
        lines.push_back(Fit(msg.substr(best + 1), px, maxw));
      }
    }
    float y = cy + side * 0.5f + 16.0f * u;
    for (const std::string& ln : lines) {
      Text(dl, ImVec2(cx - TextW(ln.c_str(), px) * 0.5f, y), px, pal::kTextHover, ln.c_str());
      y += px * 1.35f;
    }
  }

  void PaintRow(ImDrawList* dl, const PickerModel& m, const Layout& L, int i, float scroll, Hit hover, Hit held) {
    const float u = L.u;
    const Rect r = L.RowRect(i, scroll), th = L.ThumbRect(i, scroll);
    const Song& song = m.songs[size_t(i)];
    const bool cursor = i == m.cursor && !touch_, saved = i == m.selected, playing = i == m.playing;
    if (playing) playing_any_ = true;
    const bool row_hover = hover.row == i && (hover.kind == HitKind::kRow || hover.kind == HitKind::kThumb) && !inter_.scrolling();
    const bool row_held = held.row == i && held.kind == HitKind::kRow;
    const bool thumb_hover = row_hover && hover.kind == HitKind::kThumb, thumb_held = held.row == i && held.kind == HitKind::kThumb;
    const float s = r.h() * 0.2f, inset = 3.0f * u, xl = r.x0 + L.pad * 0.4f, xr = r.x1 - L.pad * 0.9f;

    // Row background: the launcher's slanted bar.
    if (cursor || saved || row_hover || row_held) {
      const ImVec2 q[4] = {{xl + s, r.y0 + inset}, {xr, r.y0 + inset}, {xr - s, r.y1 - inset}, {xl, r.y1 - inset}};
      if (cursor) {
        GradQuad(dl, q[0], q[1], q[2], q[3], row_hover ? pal::kRedHotTop : pal::kRedTop, row_hover ? pal::kRedHotBottom : pal::kRedBottom);
        dl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 150), ImDrawFlags_Closed, std::max(1.0f, 1.5f * u));
      } else if (saved) {
        GradQuad(dl, q[0], q[1], q[2], q[3], IM_COL32(178, 19, 26, 120), IM_COL32(120, 11, 17, 120));
        dl->AddPolyline(q, 4, IM_COL32(255, 255, 255, 60), ImDrawFlags_Closed, std::max(1.0f, 1.2f * u));
      } else {
        dl->AddConvexPolyFilled(q, 4, IM_COL32(255, 255, 255, row_held ? 30 : 14));
      }
    }
    if (!cursor && i + 1 < L.n) dl->AddLine(ImVec2(r.x0 + L.pad * 2, r.y1 - 0.5f), ImVec2(r.x1 - L.pad * 2, r.y1 - 0.5f), IM_COL32(255, 255, 255, 12), 1.0f);

    // Thumbnail: the album art, or the record; PLAY / STOP over it.
    const auto art = song.art_id ? Art().find(song.art_id) : Art().end();
    if (art != Art().end() && art->second.id) {
      const ImTextureRef tex(static_cast<ImTextureID>(art->second.id));
      dl->AddImageRounded(tex, ImVec2(th.x0, th.y0), ImVec2(th.x1, th.y1), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0f * u);
      dl->AddRectFilled(ImVec2(th.x0, th.y0), ImVec2(th.x1, th.y1), IM_COL32(0, 0, 0, playing ? 110 : 55), 4.0f * u);
      dl->AddRect(ImVec2(th.x0, th.y0), ImVec2(th.x1, th.y1), playing ? pal::kMenuRed : IM_COL32(255, 255, 255, 40), 4.0f * u, 0, std::max(1.0f, (playing ? 2.5f : 1.0f) * u));
    } else {
      Record(dl, th, playing, u);
      if (playing) dl->AddRect(ImVec2(th.x0, th.y0), ImVec2(th.x1, th.y1), pal::kMenuRed, 4.0f * u, 0, std::max(1.0f, 2.5f * u));
    }
    PlayButton(dl, th, playing, thumb_hover, thumb_held, u);

    // Right side: the equaliser while playing, the CURRENT tag on the saved choice.
    float right = xr - L.pad * 0.6f - s * 0.5f;
    if (saved) {
      const float tpx = r.h() * 0.17f, tw = TextW("CURRENT", tpx), pw = tw + tpx * 1.4f, ph = tpx * 1.9f;
      const float cy = (r.y0 + r.y1) * 0.5f;
      dl->AddRectFilled(ImVec2(right - pw, cy - ph * 0.5f), ImVec2(right, cy + ph * 0.5f), cursor ? IM_COL32(0, 0, 0, 150) : pal::kMenuRed, ph * 0.5f);
      TextRaw(dl, ImVec2(right - pw + tpx * 0.7f, cy - tpx * 0.5f), tpx, pal::kWhite, "CURRENT");
      right -= pw + L.pad * 0.6f;
    }
    if (playing) {
      const float eqw = 4 * 4.0f * u + 3 * 3.0f * u;
      EqBars(dl, right - eqw, (r.y0 + r.y1) * 0.5f, r.h() * 0.34f, u);
      right -= eqw + L.pad * 0.8f;
    }

    // Name and artist.
    const float tx = th.x1 + L.pad * 1.1f, room = std::max(10.0f * u, right - tx);
    const ImU32 name_col = cursor || playing ? pal::kWhite : (row_hover || saved) ? pal::kTextHover : pal::kTextDim;
    const float npx = r.h() * 0.30f, apx = r.h() * 0.20f;
    const std::string name = Fit(Upper(song.name), npx, room);
    if (song.artist.empty()) {
      Text(dl, ImVec2(tx, r.y0 + (r.h() - npx) * 0.5f), npx, name_col, name.c_str());
    } else {
      const float gap = r.h() * 0.06f, block = npx + gap + apx, y0 = r.y0 + (r.h() - block) * 0.5f;
      Text(dl, ImVec2(tx, y0), npx, name_col, name.c_str());
      const std::string artist = Fit(Upper(song.artist), apx, room);
      TextRaw(dl, ImVec2(tx, y0 + npx + gap), apx, cursor ? IM_COL32(255, 255, 255, 215) : IM_COL32(141, 142, 147, 255), artist.c_str());
    }
  }

  // ---- fonts / state --------------------------------------------------------------------------------------------------------------
  void EnsureFont() {
    if (font_tried_) return;
    font_tried_ = true;
    if (!host_.upload || !host_.read_font_pac) return;
    const std::vector<uint8_t> pac = host_.read_font_pac();
    if (pac.empty() || !ParseMenuFont(pac, font_)) {
      font_ = {};
      return;
    }
    font_tex_ = host_.upload(font_.sheet.w, font_.sheet.h, font_.sheet.px.data());
    font_.sheet.px = {};
    if (!font_tex_.id) font_ = {};
  }

  ViewHost host_;
  Interaction inter_;
  // The menu face is read and uploaded once per process (the overlay layer keeps every uploaded texture until exit).
  struct FontCache {
    MenuFont font;
    GpuImage tex;
    bool tried = false;
  };
  static FontCache& Fonts() {
    static FontCache c;
    return c;
  }
  MenuFont& font_ = Fonts().font;
  GpuImage& font_tex_ = Fonts().tex;
  bool& font_tried_ = Fonts().tried;
  static constexpr size_t kMaxArt = 160;
  static std::unordered_map<uint32_t, GpuImage>& Art() {
    static std::unordered_map<uint32_t, GpuImage> a;
    return a;
  }
  bool touch_ = kTouchDefault, playing_any_ = false, kb_armed_ = false;
  Device device_ = kTouchDefault ? Device::kTouch : Device::kMouse;
  float anim_ = 0;
};

}  // namespace wwe2k14::music_picker
