// See ui_text_ours.h. Session 101, lane 89 (UT).
#include "ui_text_ours.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui_text_fonts.h"

namespace rdr_scene {
namespace {
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr double kGuardPx = 0.005;     // every quad edge of a segment must agree with the stream's within this (the lane's bar)
constexpr double kUvSlack = 0.01;      // sheet pixels: a uv rect equals the record's
constexpr uint32_t kVkRg32 = 103, kVkRgb32 = 106, kVkRgba32 = 109;   // VK_FORMAT_R32G32_SFLOAT / R32G32B32_SFLOAT / R32G32B32A32_SFLOAT
constexpr uint32_t kFirstIcon = 0xE000u;

// One glyph draw's four vertices, parsed from the stream's bytes (a strip: TL, TR, BL, BR).
struct StreamQuad {
  bool ok = false;
  float x[4] = {}, y[4] = {}, u[4] = {}, v[4] = {};
  uint32_t stride = 0, pos_off = 0, uv_off = 0;
  size_t bytes_at = 0;   // where the draw's vertex bytes start in the caller's `bytes` (appended by ParseQuad)
};

bool ReadFloat(const std::vector<uint8_t>& raw, size_t at, float& v) {
  if (at + 4 > raw.size()) return false;
  std::memcpy(&v, raw.data() + at, 4);
  return std::isfinite(v);
}

// The stream draw -> its quad. Fails (ok false) for anything but 4 inline vertices with a float position and a float uv. The draw's vertex bytes are appended to `bytes`
// (q.bytes_at) on success.
void ParseQuad(const rdr_adapter::ReplaceIn& in, uint32_t draw_index, std::vector<uint8_t>& raw, std::vector<uint8_t>& bytes, StreamQuad& q) {
  q = {};
  const rdr_adapter::LoadedFrame& f = *in.frame;
  if (draw_index >= f.draws.count) return;
  const rdr_adapter::Draw& d = f.draws[draw_index];
  if (d.vertex_count != 4 || d.index_bytes != 0 || d.layout == 0xFFFFFFFFu || d.layout >= f.layouts.count || !in.read_geometry) return;
  const rdr_adapter::VertexLayout& layout = f.layouts[d.layout];
  if (layout.host_stride == 0 || d.vertex_bytes != 4 * layout.host_stride || layout.element_first + layout.element_count > f.elements.count) return;
  bool have_pos = false, have_uv = false;
  for (uint32_t e = 0; e < layout.element_count; ++e) {
    const rdr_adapter::VertexElement& el = f.elements[layout.element_first + e];
    if (!have_pos && (el.vk_format == kVkRgb32 || el.vk_format == kVkRgba32)) q.pos_off = el.host_offset, have_pos = true;
    else if (!have_uv && el.vk_format == kVkRg32) q.uv_off = el.host_offset, have_uv = true;
  }
  if (!have_pos || !have_uv || !in.read_geometry(d.vertex_offset, d.vertex_bytes, raw) || raw.size() < d.vertex_bytes) return;
  q.stride = layout.host_stride;
  for (int k = 0; k < 4; ++k) {
    const size_t base = size_t(k) * q.stride;
    if (!ReadFloat(raw, base + q.pos_off, q.x[k]) || !ReadFloat(raw, base + q.pos_off + 4, q.y[k]) || !ReadFloat(raw, base + q.uv_off, q.u[k]) ||
        !ReadFloat(raw, base + q.uv_off + 4, q.v[k]))
      return;
  }
  // an axis-aligned rectangle in strip order, uv the same way
  const float tol = 1e-3f;
  q.ok = std::fabs(q.y[0] - q.y[1]) < tol && std::fabs(q.y[2] - q.y[3]) < tol && std::fabs(q.x[0] - q.x[2]) < tol && std::fabs(q.x[1] - q.x[3]) < tol &&
         std::fabs(q.v[0] - q.v[1]) < 1e-6f && std::fabs(q.v[2] - q.v[3]) < 1e-6f && std::fabs(q.u[0] - q.u[2]) < 1e-6f && std::fabs(q.u[1] - q.u[3]) < 1e-6f;
  if (q.ok) q.bytes_at = bytes.size(), bytes.insert(bytes.end(), raw.begin(), raw.begin() + d.vertex_bytes);
}

// Does the stream quad's uv rect equal the record of `code` in `font`?
bool UvMatches(const UiFont& font, uint32_t code, const StreamQuad& q) {
  const auto it = font.glyphs.find(code);
  if (it == font.glyphs.end() || font.sheet.width == 0 || font.sheet.height == 0) return false;
  const UiGlyph& g = it->second;
  const double sw = font.sheet.width, sh = font.sheet.height;
  return std::fabs(q.u[0] * sw - g.x) < kUvSlack && std::fabs(q.u[1] * sw - (g.x + g.w)) < kUvSlack && std::fabs(q.v[0] * sh - g.y) < kUvSlack &&
         std::fabs(q.v[2] * sh - (g.y + g.h)) < kUvSlack;
}

}  // namespace

bool UiTextFaults::Parse(const std::string& name_in, UiTextFaults& out) {
  out = {};
  std::string name = name_in;
  if (name.rfind("unchecked_", 0) == 0) out.unchecked = true, name = name.substr(10);
  if (name.empty()) return !out.unchecked;
  if (name == "advance_c2") out.advance_c2 = true;
  else if (name == "kern_off") out.kern_off = true;
  else if (name == "wrong_font") out.wrong_font = true;
  else if (name == "run_shift") out.run_shift = true;
  else return false;
  return true;
}

bool UiTextDecode(const std::string& s, std::vector<uint32_t>& out) {
  out.clear();
  for (size_t i = 0; i < s.size();) {
    const uint8_t b = uint8_t(s[i]);
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    if (b < 0x80) cp = b;
    else if ((b & 0xE0) == 0xC0 && i + 1 < s.size() && (uint8_t(s[i + 1]) & 0xC0) == 0x80) cp = ((b & 0x1Fu) << 6) | (uint8_t(s[i + 1]) & 0x3Fu), len = 2;
    else if ((b & 0xF0) == 0xE0 && i + 2 < s.size() && (uint8_t(s[i + 1]) & 0xC0) == 0x80 && (uint8_t(s[i + 2]) & 0xC0) == 0x80)
      cp = ((b & 0x0Fu) << 12) | ((uint8_t(s[i + 1]) & 0x3Fu) << 6) | (uint8_t(s[i + 2]) & 0x3Fu), len = 3;
    else if ((b & 0xF8) == 0xF0 && i + 3 < s.size() && (uint8_t(s[i + 1]) & 0xC0) == 0x80 && (uint8_t(s[i + 2]) & 0xC0) == 0x80 && (uint8_t(s[i + 3]) & 0xC0) == 0x80)
      cp = ((b & 0x07u) << 18) | ((uint8_t(s[i + 1]) & 0x3Fu) << 12) | ((uint8_t(s[i + 2]) & 0x3Fu) << 6) | (uint8_t(s[i + 3]) & 0x3Fu), len = 4;
    out.push_back(cp);
    i += len;
  }
  return !out.empty();
}

void UiTextUnitsOf(const UiFont& font, const std::vector<uint32_t>& cps, std::vector<UiTextUnit>& out) {
  out.clear();
  for (uint32_t cp : cps) {
    if (cp < 0x20 || cp == 0x7F) continue;       // draws nothing [C: sub_82BD06B8 0x82BD0974]
    if (cp == 0x2019) cp = 0x27;                  // U+2019 is drawn as U+0027
    UiTextUnit u;
    if (cp >= kFirstIcon) {
      u.code = cp, u.icon = true;                 // the icon sheet: not in the font's table, not laid out here
    } else if (font.glyphs.count(cp)) {
      u.code = cp;
    } else if (font.glyphs.count(0x2A)) {
      u.code = 0x2A;                              // a code the font lacks is drawn as '*'
    } else {
      continue;                                   // ... and nothing if '*' is missing too
    }
    out.push_back(u);
  }
}

void UiTextLayout(const UiFont& metrics, const std::vector<UiTextUnit>& units, size_t first, size_t count, float left, float scale, float hscale, const UiTextFaults& faults,
                  std::vector<UiTextQuad>& out) {
  out.clear();
  double pen = left;
  const double trim = faults.advance_c2 ? 2.0 : double(metrics.advance_trim);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t code = units[first + i].code;
    const auto it = metrics.glyphs.find(code);
    const double w = it == metrics.glyphs.end() ? 0.0 : double(it->second.w);
    double kern = 0;
    if (!faults.kern_off && i + 1 < count) kern = double(metrics.Kern(code, units[first + i + 1].code));
    const double shift = faults.run_shift ? 0.5 : 0.0;
    UiTextQuad q;
    q.x0 = float(pen + shift);
    q.x1 = float(pen + w * double(scale) * double(hscale) + shift);
    out.push_back(q);
    pen += (w - trim - kern) * double(scale) * double(hscale);
  }
}

// ---- the hook -------------------------------------------------------------------------------------------------------------------------------------------------
UiTextOurs::UiTextOurs(const UiTextOptions& options) : options_(options), root_(options.game_root) {}

std::string UiTextOurs::error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return error_;
}

UiTextOurs::~UiTextOurs() {
  if (load_thread_.joinable()) load_thread_.join();
}

void UiTextOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  if (root_.empty()) root_ = root;
}

bool UiTextOurs::Claims(const rdr_adapter::DrawOrigin& origin) const {
  return origin.klass == rdr_adapter::DrawClass::Text && origin.op == 19 && (origin.call_site == kGlyphSite || origin.call_site == kGlyphSiteM);
}

void UiTextOurs::LoadFonts() {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  std::string err;
  UiFont loaded[3];
  const bool ok = UiTextLoadFonts(root, loaded, err);
  std::lock_guard<std::mutex> lock(mu_);
  if (ok) {
    for (int i = 0; i < 3; ++i) fonts_[i] = std::move(loaded[i]);
    loaded_ = true;
  } else {
    error_ = err;
    load_failed_ = true;
    if (options_.log) options_.log("[rdr][ui_text] the menu fonts could not be read, the game's text stays the stream's: " + err);   // once: LoadFonts runs once
  }
}

bool UiTextOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  ++census_.frames;
  if (!(frame.header.flags & rdr_adapter::kFrameSixteenPairsHighFirst)) return ++census_.old_format_frames, false;   // review F2
  if (!loaded_ && !load_failed_) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (root_.empty()) return ++census_.not_ready_frames, false;   // no game folder known yet
    }
    if (!load_started_.exchange(true)) {
      if (options_.live) load_thread_ = std::thread([this] { LoadFonts(); });
      else LoadFonts();
    }
  }
  if (!loaded_) return ++census_.not_ready_frames, false;
  // The frame's runs: consecutive glyph draws with one string and one object (the capture's TextRun table is sorted by draw).
  groups_.clear();
  group_of_draw_.assign(frame.draws.count, -1);
  plan_.clear();
  vertices_.clear();
  for (uint32_t i = 0; i < frame.text_runs.count; ++i) {
    const rdr_adapter::TextRun& r = frame.text_runs[i];
    if (r.draw >= frame.draws.count) continue;
    const rdr_adapter::DrawOrigin* o = frame.OriginOf(r.draw);
    if (!o) continue;
    bool extend = false;
    if (!groups_.empty()) {
      const Group& g = groups_.back();
      const rdr_adapter::DrawOrigin* first = frame.OriginOf(g.first_draw);
      extend = first && g.string == r.string && first->object == o->object && r.draw == g.first_draw + g.draws;
    }
    if (extend) {
      ++groups_.back().draws;
    } else {
      Group g;
      g.first_draw = r.draw, g.draws = 1, g.string = r.string, g.flags = r.flags;
      groups_.push_back(g);
    }
    group_of_draw_[r.draw] = int32_t(groups_.size() - 1);
  }
  uint32_t at = 0;
  for (Group& g : groups_) g.plan = at, at += g.draws;
  plan_.assign(at, Plan{});
  return true;
}

void UiTextOurs::SetFate(Group& group, uint32_t from, uint32_t count, Fate fate) {
  for (uint32_t k = from; k < from + count && k < group.draws; ++k) {
    plan_[group.plan + k].fate = fate;
    switch (fate) {
      case Fate::Ours: ++census_.ours; break;
      case Fate::Icon: ++census_.kept_icon; break;
      case Fate::NoRun: ++census_.kept_norun; break;
      case Fate::Truncated: ++census_.kept_truncated; break;
      case Fate::Count: ++census_.kept_count; break;
      case Fate::Font: ++census_.kept_font; break;
      case Fate::Geometry: ++census_.kept_geometry; break;
      case Fate::Layout: ++census_.kept_layout; break;
    }
  }
}

void UiTextOurs::Decide(const rdr_adapter::ReplaceIn& in, Group& group) {
  const auto t0 = Clock::now();
  group.decided = true;
  ++census_.runs;
  const auto finish = [&](Fate fate) {
    SetFate(group, 0, group.draws, fate);
    census_.ms += MsSince(t0);
  };
  const rdr_adapter::LoadedFrame& frame = *in.frame;
  const char* text = group.string ? frame.String(group.string) : "";
  if (!text[0]) return finish(Fate::NoRun);
  if (group.flags & rdr_adapter::kRunTruncated) return finish(Fate::Truncated);
  UiTextDecode(text, cps_);
  // The stream's quads of the whole group (their vertex bytes are kept in stream_bytes_).
  std::vector<StreamQuad> quads(group.draws);
  stream_bytes_.clear();
  bool geometry_ok = true;
  for (uint32_t k = 0; k < group.draws; ++k) {
    ParseQuad(in, group.first_draw + k, raw_, stream_bytes_, quads[k]);
    geometry_ok = geometry_ok && quads[k].ok;
  }
  // Which font: the one whose records agree with every non-icon quad's uv rect and whose unit count divides the group.
  int font_index = -1;
  bool count_ok_somewhere = false;
  for (int f = 0; f < 3 && font_index < 0; ++f) {
    UiTextUnitsOf(fonts_[f], cps_, units_);
    if (units_.empty() || group.draws % units_.size() != 0) continue;
    count_ok_somewhere = true;
    if (!geometry_ok) continue;
    bool all = true;
    for (uint32_t k = 0; k < group.draws && all; ++k) {
      const UiTextUnit& u = units_[k % units_.size()];
      if (!u.icon) all = UvMatches(fonts_[f], u.code, quads[k]);
    }
    if (all) font_index = f;
  }
  if (!count_ok_somewhere) {
    UiTextUnitsOf(fonts_[0], cps_, units_);
    return finish(units_.empty() ? Fate::NoRun : Fate::Count);   // (a string with nothing to draw is not a count fault)
  }
  if (!geometry_ok) return finish(Fate::Geometry);
  if (font_index < 0) return finish(Fate::Font);
  for (int f = font_index + 1; f < 3; ++f) {   // a second font that also agrees makes the choice ambiguous: counted, the first is used
    std::vector<UiTextUnit> other;
    UiTextUnitsOf(fonts_[f], cps_, other);
    if (other.empty() || group.draws % other.size() != 0) continue;
    bool all = true, tested = false;   // (a run of icons alone fits every font: nothing was tested)
    for (uint32_t k = 0; k < group.draws && all; ++k)
      if (!other[k % other.size()].icon) all = UvMatches(fonts_[f], other[k % other.size()].code, quads[k]), tested = true;
    if (all && tested) ++census_.fonts_ambiguous;
  }
  const UiFont& font = fonts_[font_index];
  const UiFont& metrics = options_.faults.wrong_font ? fonts_[(font_index + 1) % 3] : font;
  UiTextUnitsOf(font, cps_, units_);
  const size_t n = units_.size();
  bool any_icon = false;
  for (const UiTextUnit& u : units_) any_icon = any_icon || u.icon;
  census_.icon_runs += any_icon;
  bool any_ours = false;
  const double cell = font.cell > 0 ? font.cell : 40.0;
  const double sw = font.sheet.width, sh = font.sheet.height;
  for (uint32_t rep = 0; rep < group.draws / n; ++rep) {
    for (size_t s = 0; s < n;) {
      if (units_[s].icon) {
        SetFate(group, uint32_t(rep * n + s), 1, Fate::Icon);
        ++s;
        continue;
      }
      size_t e = s;
      while (e < n && !units_[e].icon) ++e;   // the segment [s, e) of this repetition
      ++census_.segments;
      const uint32_t d0 = uint32_t(rep * n + s);   // its first draw, as an index into the group
      const StreamQuad& a = quads[d0];
      // The segment's parameters come from its first glyph draw: the pen start, the scale, the horizontal factor.
      const double height = double(a.y[2]) - double(a.y[0]);
      const float scale = float(height / cell);
      const auto g0 = metrics.glyphs.find(units_[s].code);
      Fate fate = Fate::Layout;
      if (scale > 0.05f && scale < 8.0f && g0 != metrics.glyphs.end() && g0->second.w > 0) {
        float hscale = float((double(a.x[1]) - double(a.x[0])) / (double(g0->second.w) * double(scale)));
        if (std::fabs(hscale - 1.0f) < 2e-3f) hscale = 1.0f;
        if (hscale > 0.4f && hscale <= 1.0f) {   // (a quad is never wider than w x scale: 0.4 is below anything measured)
          UiTextLayout(metrics, units_, s, e - s, a.x[0], scale, hscale, options_.faults, quads_);
          double worst = 0;
          for (size_t i = 0; i < e - s; ++i) {
            const StreamQuad& q = quads[d0 + i];
            worst = std::fmax(worst, std::fmax(std::fabs(double(quads_[i].x0) - double(q.x[0])), std::fabs(double(quads_[i].x1) - double(q.x[1]))));
          }
          if (worst <= kGuardPx || options_.faults.unchecked) {
            fate = Fate::Ours;
            if (worst <= kGuardPx) census_.max_dx = std::fmax(census_.max_dx, worst);
            if (hscale < 1.0f) ++census_.narrow_segments;
          }
        }
      }
      if (fate == Fate::Ours) {
        // The replacement vertices: the stream's own bytes with the corners' x and the uv rect (from the record) rewritten; y, z and the rest stay the stream's.
        for (size_t i = 0; i < e - s; ++i) {
          const StreamQuad& q = quads[d0 + i];
          const size_t at = vertices_.size();
          vertices_.insert(vertices_.end(), stream_bytes_.begin() + q.bytes_at, stream_bytes_.begin() + q.bytes_at + 4 * size_t(q.stride));
          const UiGlyph& gl = font.glyphs.at(units_[s + i].code);
          const float u0 = float(double(gl.x) / sw), u1 = float(double(gl.x + gl.w) / sw), v0 = float(double(gl.y) / sh), v1 = float(double(gl.y + gl.h) / sh);
          const float xs[4] = {quads_[i].x0, quads_[i].x1, quads_[i].x0, quads_[i].x1};
          const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
          for (int c = 0; c < 4; ++c) {
            uint8_t* vtx = vertices_.data() + at + size_t(c) * q.stride;
            std::memcpy(vtx + q.pos_off, &xs[c], 4);
            std::memcpy(vtx + q.uv_off, &us[c], 4);
            std::memcpy(vtx + q.uv_off + 4, &vs[c], 4);
          }
          plan_[group.plan + d0 + i].vertex_at = uint32_t(at);
        }
        any_ours = true;
      }
      SetFate(group, d0, uint32_t(e - s), fate);
      s = e;
    }
  }
  census_.runs_ours += any_ours;
  census_.ms += MsSince(t0);
}

bool UiTextOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) {
  ++census_.claimed;
  const uint32_t d = in.draw_index;
  if (d >= group_of_draw_.size() || group_of_draw_[d] < 0) {
    ++census_.kept_norun;
    return why = rdr_adapter::KeepWhy::NotOurs, false;
  }
  Group& g = groups_[size_t(group_of_draw_[d])];
  if (!g.decided) Decide(in, g);
  const Plan& p = plan_[g.plan + (d - g.first_draw)];
  if (p.fate != Fate::Ours) return why = rdr_adapter::KeepWhy::NotOurs, false;   // the next hook that claims the draw (the UI textures) may take it
  out.keep_stream_geometry = true;
  out.inline_vertices = vertices_.data() + p.vertex_at;
  out.inline_vertex_bytes = in.draw->vertex_bytes;
  return true;
}

std::string UiTextOurs::Report() const {
  const UiTextCensus& c = census_;
  const std::string err = error();
  char line[640];
  std::snprintf(line, sizeof line,
                "glyph draws claimed %llu: ours %llu, kept (icon %llu, no run %llu, truncated %llu, count %llu, font %llu, geometry %llu, layout %llu); runs %llu (ours %llu, with icons %llu), segments %llu (narrower quads %llu), "
                "fonts ambiguous %llu, max quad edge difference %.4f px, ms %.2f%s%s",
                (unsigned long long)c.claimed, (unsigned long long)c.ours, (unsigned long long)c.kept_icon, (unsigned long long)c.kept_norun, (unsigned long long)c.kept_truncated,
                (unsigned long long)c.kept_count, (unsigned long long)c.kept_font, (unsigned long long)c.kept_geometry, (unsigned long long)c.kept_layout, (unsigned long long)c.runs,
                (unsigned long long)c.runs_ours, (unsigned long long)c.icon_runs, (unsigned long long)c.segments, (unsigned long long)c.narrow_segments,
                (unsigned long long)c.fonts_ambiguous, c.max_dx, c.ms, err.empty() ? "" : ", ERRORS: ", err.c_str());
  return line;
}

}  // namespace rdr_scene
