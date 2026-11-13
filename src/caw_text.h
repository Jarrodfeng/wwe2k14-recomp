// wwe2k14 - the Create a Superstar editor's drawn state, read from a frame's text runs (S101 lane 118, N2 / L-6, docs/research/s101/N2_EDITOR_DRIVER.md).
// No game, no SDK, no Windows includes (the offline test tools/n2/caw_text_test.cpp links this header against a capture; the game exe reaches it through
// src/caw_live.h and one call in src/renderer_adapter/live.cpp's frame path).
//
// ReadCawState(frame) -> State: what the editor shows, from the draws the frame already holds:
//   seq / seq76   "screen seq=76" in the widget string of the text draws (names.h section 64): the game's sequence id, no memory peek
//   title         the glyph run at (140, 58)                                    [the drop shadow is at (138, 56): another place, never read]
//   rows          the glyph runs at x 144, y = 118 + 24 k, indexed by k           [a row sliding into place (a page change, 5 frames) is off the grid: not read]
//   hot           the one row whose text colour is red: the draw's VERTEX-shader constant bank, vec4 register 1 (r >= 0.15, g and b <= 0.25 r)
//   tab           the red one of the four 16 px sprite dots at x 362 + 16 k, y 66 (class sprite; VS bank register 1 r >= 0.6, g and b <= 0.25 r)
// The same rules, field for field, as tools/n2/caw_read.py (the gate compares them on every frame of the kept walks).
// The run strings exist only while labels are on (origin::g_labels_wanted): a frame without them reads has_text = false, "no text", never a guess.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "renderer_adapter/capture_reader.h"

namespace wwe2k14::caw {

// The planted twins' knobs only (tools/n2/caw_text_test.cpp --plant NAME); the shipped defaults are Options{}.
struct Options {
  bool ps_bank = false;     // twin: the pixel-shader bank read instead of the vertex-shader bank
  float min_r = 0.05f;      // twin: 0.3f (the first rule, lost the highlight at the pulse's low; the review's floor: the pulse goes to 0.165 on the kept walk)
  float ratio = 0.25f;
  bool white = false;       // twin: every colour reads as 1, 1, 1
  bool strip_runs = false;  // twin: the frame read as if section 69 were absent
};

struct State {
  uint64_t guest = 0;
  bool has_text = false;
  int seq = -1;
  bool seq76 = false;
  std::string title;
  std::vector<std::string> rows;   // indexed by k; "" where row k is not drawn
  std::vector<float> row_r, row_g, row_b;   // each row's text colour (VS bank register 1), finite-clamped: the driver tells grey (disabled) and amber (ACCEPT) rows by them
  bool complete = false;           // rows k = 0..n-1 with no gap, n >= 1
  int hot = -1;                    // the one hot row; -1 none, -2 more than one
  int hot_n = 0;
  float hot_r = 0;
  int tab = -1;                    // the red dot 0-3; -1 none, -2 more than one
  int dots = 0;                    // how many of the four dots are drawn
  bool flash = false;              // set by Flash(): the hot row changed or its red jumped by > 0.1 since the state before
};

using ReadGeometry = std::function<bool(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out)>;

inline bool IsHot(const float* c, const Options& o) { return c[0] >= o.min_r && c[1] <= o.ratio * c[0] && c[2] <= o.ratio * c[0]; }
inline bool IsRedDot(const float* c) { return c[0] >= 0.6f && c[1] <= 0.25f * c[0] && c[2] <= 0.25f * c[0]; }
inline bool Near(float a, float b, float e = 0.6f) { return std::fabs(a - b) <= e; }
inline float Fin(float v) { return std::isfinite(v) ? v : 0.0f; }

// The hot row changed, or its red jumped up by more than 0.1, between two states (a move shows as one frame at r 0.92).
inline bool Flash(const State& prev, const State& cur) {
  return cur.hot >= 0 && prev.hot >= 0 && (cur.hot != prev.hot || cur.hot_r - prev.hot_r > 0.1f);
}

inline State ReadCawState(const rdr_adapter::LoadedFrame& frame, const ReadGeometry& read_geometry, const Options& opt = Options{}) {
  using namespace rdr_adapter;
  State out;
  out.guest = frame.header.guest_frame;
  if (opt.strip_runs || frame.text_runs.count == 0 || frame.strings.count == 0) return out;
  out.has_text = true;
  auto colour = [&](const Draw& d, float* c) {
    if (opt.white) { c[0] = c[1] = c[2] = c[3] = 1.0f; return; }
    const uint32_t bank = opt.ps_bank ? d.ps_bank : d.vs_bank;
    if (bank >= frame.constant_banks.count) { c[0] = c[1] = c[2] = c[3] = 0.0f; return; }
    std::memcpy(c, &frame.constant_banks[bank].dwords[4], 16);   // vec4 register 1
  };
  auto position = [&](const Draw& d, float& x, float& y) {
    std::vector<uint8_t> v;
    if (d.layout == 0xFFFFFFFFu || !d.vertex_count || !read_geometry || !read_geometry(d.vertex_offset, 8, v) || v.size() < 8) return false;
    std::memcpy(&x, v.data(), 4);
    std::memcpy(&y, v.data() + 4, 4);
    return true;
  };
  struct Row { std::string text; float c[4]; };
  std::vector<Row> rows;      // by k
  std::vector<bool> have;
  bool title_set = false;
  bool offgrid = false;   // a run at x 144 in the list's y range that is not on a row slot (a row sliding in)
  std::vector<int> seqs;
  for (const TextRun& r : frame.text_runs) {
    if (r.draw >= frame.draws.count) continue;
    const Draw& d = frame.draws[r.draw];
    float x, y;
    if (!position(d, x, y)) continue;
    x = std::round(x * 10.0f) / 10.0f, y = std::round(y * 10.0f) / 10.0f;   // the reference rounds to 0.1 as well
    const std::string text = frame.String(r.string);
    if (const DrawOrigin* o = frame.OriginOf(r.draw); o && o->widget) {
      const char* w = frame.String(o->widget);
      if (std::strncmp(w, "screen seq=", 11) == 0 && w[11] >= '0' && w[11] <= '9') seqs.push_back(std::atoi(w + 11));
    }
    if (Near(x, 140.0f) && Near(y, 58.0f) && !title_set) { out.title = text, title_set = true; }
    if (Near(x, 144.0f) && y >= 118.0f - 0.5f && y <= 580.0f) {
      const int k = int(std::lround((y - 118.0f) / 24.0f));
      if (!(k >= 0 && std::fabs(y - (118.0f + 24.0f * k)) <= 0.6f)) offgrid = true;
      if (k >= 0 && std::fabs(y - (118.0f + 24.0f * k)) <= 0.6f) {
        if (size_t(k) >= rows.size()) rows.resize(size_t(k) + 1), have.resize(size_t(k) + 1, false);
        if (!have[size_t(k)]) {
          have[size_t(k)] = true;
          rows[size_t(k)].text = text;
          colour(d, rows[size_t(k)].c);
        }
      }
    }
  }
  if (!seqs.empty()) {
    out.seq = seqs[0];
    out.seq76 = std::all_of(seqs.begin(), seqs.end(), [](int v) { return v == 76; });
  }
  out.rows.resize(rows.size());
  out.row_r.assign(rows.size(), 0.0f), out.row_g.assign(rows.size(), 0.0f), out.row_b.assign(rows.size(), 0.0f);
  out.complete = !rows.empty() && !offgrid;
  std::vector<int> hots;
  for (size_t k = 0; k < rows.size(); ++k) {
    if (!have[k]) { out.complete = false; continue; }
    out.rows[k] = rows[k].text;
    out.row_r[k] = Fin(rows[k].c[0]), out.row_g[k] = Fin(rows[k].c[1]), out.row_b[k] = Fin(rows[k].c[2]);
    if (IsHot(rows[k].c, opt)) hots.push_back(int(k));
  }
  out.hot_n = int(hots.size());
  out.hot = hots.size() == 1 ? hots[0] : (hots.empty() ? -1 : -2);
  out.hot_r = hots.size() == 1 ? rows[size_t(hots[0])].c[0] : 0.0f;
  // the tab dots: sprite-class draws, first quad at y 64-68, x 362 + 16 k
  bool dot_seen[4] = {false, false, false, false};
  float dot_c[4][4] = {};
  for (uint32_t i = 0; i < frame.draws.count && i < frame.origins.count; ++i) {
    if (frame.origins[i].klass != DrawClass::Sprite) continue;
    const Draw& d = frame.draws[i];
    if (d.layout == 0xFFFFFFFFu || !d.vertex_count || !d.texture_count) continue;
    float x, y;
    if (!position(d, x, y)) continue;
    const int k = int(std::lround((x - 362.0f) / 16.0f));
    if (k < 0 || k > 3 || !Near(x, 362.0f + 16.0f * k) || y < 64.0f || y > 68.0f) continue;
    if (dot_seen[k]) continue;
    dot_seen[k] = true;
    colour(d, dot_c[k]);
  }
  int red = 0, red_k = -1;
  for (int k = 0; k < 4; ++k) {
    if (!dot_seen[k]) continue;
    ++out.dots;
    if (IsRedDot(dot_c[k])) ++red, red_k = k;
  }
  out.tab = red == 1 ? red_k : (red ? -2 : -1);
  return out;
}

// ---- the text form: one line a state, tab separated, UTF-8. Both readers print it; the gate compares the lines. ---------------------------------
inline std::string Line(const State& s) {
  char head[160];
  std::snprintf(head, sizeof(head), "%llu\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%d\t%d\t", (unsigned long long)s.guest, s.has_text ? 1 : 0, s.seq, s.seq76 ? 1 : 0,
                s.complete ? 1 : 0, s.hot, s.hot_n, s.tab, s.hot_r, s.dots, int(s.rows.size()));
  std::string out = head;
  out += s.title;
  for (size_t k = 0; k < s.rows.size(); ++k) {
    char r[64];
    std::snprintf(r, sizeof(r), "\t%.3f,%.3f,%.3f\t", s.row_r[k], s.row_g[k], s.row_b[k]);
    out += r;
    out += s.rows[k];
  }
  return out;
}

inline void JsonString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += char(c);   // UTF-8 bytes pass through
  }
  out += '"';
}

inline std::string Json(const State& s) {
  char b[200];
  std::string o = "{";
  std::snprintf(b, sizeof(b), "\"guest\":%llu,\"has_text\":%s,\"seq\":%d,\"seq76\":%s,\"complete\":%s,\"hot\":%d,\"hot_n\":%d,\"hot_r\":%.3f,\"tab\":%d,\"dots\":%d,\"flash\":%s,\"title\":",
                (unsigned long long)s.guest, s.has_text ? "true" : "false", s.seq, s.seq76 ? "true" : "false", s.complete ? "true" : "false", s.hot, s.hot_n,
                s.hot_r, s.tab, s.dots, s.flash ? "true" : "false");
  o += b;
  JsonString(o, s.title);
  o += ",\"rows\":[";
  for (size_t k = 0; k < s.rows.size(); ++k) {
    if (k) o += ',';
    JsonString(o, s.rows[k]);
  }
  o += "]";
  const std::vector<float>* cols[3] = {&s.row_r, &s.row_g, &s.row_b};
  const char* names[3] = {"row_r", "row_g", "row_b"};
  for (int c = 0; c < 3; ++c) {   // S101 lane 118 review: the driver needs each row's colour (grey / amber rows), which the first Json() left out
    o += ",\"" + std::string(names[c]) + "\":[";
    for (size_t k = 0; k < cols[c]->size(); ++k) {
      char b2[32];
      std::snprintf(b2, sizeof(b2), "%s%.3f", k ? "," : "", double((*cols[c])[k]));
      o += b2;
    }
    o += "]";
  }
  o += "}";
  return o;
}

// ---- the live store: the states the game's frame path read while the `!caw` verb is on (src/caw_live.h answers from it) --------------------------
inline std::atomic<bool> g_on{false};
inline std::atomic<int64_t> g_last_read_ms{0};   // steady-clock ms of the newest `on` / `state` / `since` (the auto-off clock)
inline void (*g_auto_off)() = nullptr;           // set by src/caw_live.h: the same as `!caw off` (the labels back to what they were)
constexpr int64_t kIdleMs = 30000;               // no read for this long: the verb switches itself off (a driver that died must not leave labels on)
inline int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline std::mutex g_mu;
inline std::deque<State> g_ring;       // newest last, at most kRing
inline uint64_t g_dropped = 0;         // states pushed out of the ring unread (the client polls too slowly)
inline uint64_t g_frames = 0;          // frames read since the verb went on
constexpr size_t kRing = 512;

// The live frame path calls this for each frame it parses while g_on is set (src/renderer_adapter/live.cpp, S101 lane 118).
inline void OnFrame(const rdr_adapter::LoadedFrame& frame, const ReadGeometry& read_geometry) {
  if (g_on.load(std::memory_order_relaxed) && NowMs() - g_last_read_ms.load(std::memory_order_relaxed) > kIdleMs) {
    if (g_auto_off) g_auto_off();   // S101 lane 118 review: 30 s without a read
    return;
  }
  State s = ReadCawState(frame, read_geometry);
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_on.load(std::memory_order_relaxed)) return;   // switched off while this frame was read: nothing enters a cleared ring
  if (!g_ring.empty()) s.flash = Flash(g_ring.back(), s);
  if (g_ring.size() >= kRing) g_ring.pop_front(), ++g_dropped;
  g_ring.push_back(std::move(s));
  ++g_frames;
}

// `!caw since GF [MAX]`: the states after guest frame GF, oldest first, as one JSON array on one line (the live channel's answer is "key: value" lines).
inline std::string Since(uint64_t gf, size_t max_states) {
  std::lock_guard<std::mutex> lock(g_mu);
  std::string out = "[";
  size_t n = 0;
  for (const State& s : g_ring) {
    if (s.guest <= gf) continue;
    if (n >= max_states) break;
    if (n) out += ',';
    out += Json(s);
    ++n;
  }
  return out + "]";
}

}  // namespace wwe2k14::caw
