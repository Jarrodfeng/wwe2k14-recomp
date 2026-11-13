// Session 101 lane 89 (UT): the unit test of our UI text (src\renderer_scene\ui_text_ours.*, the SHIPPED hook the game and fview run) on the player's font files and a capture
// that carries the run strings (names.h section 69). No GPU, no game, no SDK.
//
//   ui_text_test.exe capture --game DIR --capture DIR --kind menu|editor [--step N] [--fault NAME]
//
// Every frame of the capture goes through the hook the way the binder drives it (FrameReady, then Replace for every glyph draw it claims, with the stream draw's vertex bytes
// readable). For each replaced draw the stream's own bytes are compared with the bytes the hook supplies, by code written HERE (not the hook's): the four corners' x within 0.005 px,
// y / z and every other byte identical, the uv rect within 1e-6 of the stream's. Lines are `PASS|FAIL name detail`; exit 1 on any FAIL, 2 when an input cannot be read.
// A planted fault (--fault advance_c2|kern_off|wrong_font|run_shift) makes the guard refuse (the `kept` check FAILs); `unchecked_NAME` draws the planted layout (the `quads` check FAILs).
// tools\ui\ui_text_check.py runs every plant and demands both.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/ui_text_ours.h"

using namespace rdr_scene;
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;

namespace {

int g_fail = 0;
void Line(bool ok, const char* name, const std::string& detail) {
  std::printf("%s  %-12s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok) g_fail = 1;
}
std::string Num(unsigned long long v) { return std::to_string(v); }
std::string Flt(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.5f", v);
  return b;
}

struct Args {
  std::string game, capture, kind = "menu", fault;
  uint32_t step = 1;
};

bool Parse(int argc, char** argv, Args& a) {
  if (argc < 2 || std::string(argv[1]) != "capture") return false;
  for (int i = 2; i < argc; ++i) {
    const std::string s = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (s == "--game") a.game = next();
    else if (s == "--capture") a.capture = next();
    else if (s == "--kind") a.kind = next();
    else if (s == "--fault") a.fault = next();
    else if (s == "--step") a.step = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else return false;
  }
  return !a.game.empty() && !a.capture.empty() && a.step >= 1;
}

// The test's own reading of a 4-vertex draw: stride, the position / uv element offsets, the floats.
struct Raw {
  bool ok = false;
  uint32_t stride = 0, pos = 0, uv = 0;
  std::vector<uint8_t> bytes;
  float Get(int vertex, uint32_t off) const {
    float v;
    std::memcpy(&v, bytes.data() + size_t(vertex) * stride + off, 4);
    return v;
  }
};
Raw ReadRaw(const CaptureReader& cap, const LoadedFrame& frame, uint32_t draw) {
  Raw r;
  const rdr_adapter::Draw& d = frame.draws[draw];
  if (d.layout >= frame.layouts.count || d.vertex_count != 4) return r;
  const rdr_adapter::VertexLayout& l = frame.layouts[d.layout];
  bool p = false, u = false;
  for (uint32_t e = 0; e < l.element_count; ++e) {
    const rdr_adapter::VertexElement& el = frame.elements[l.element_first + e];
    if (!p && (el.vk_format == 106 || el.vk_format == 109)) r.pos = el.host_offset, p = true;
    else if (!u && el.vk_format == 103) r.uv = el.host_offset, u = true;
  }
  r.stride = l.host_stride;
  r.ok = p && u && d.vertex_bytes == 4 * r.stride && cap.ReadGeometry(d.vertex_offset, d.vertex_bytes, r.bytes);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!Parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: ui_text_test capture --game DIR --capture DIR --kind menu|editor [--step N] [--fault advance_c2|kern_off|wrong_font|run_shift|unchecked_NAME]\n");
    return 2;
  }
  UiTextOptions options;
  options.game_root = a.game;
  if (!UiTextFaults::Parse(a.fault, options.faults)) {
    std::fprintf(stderr, "unknown fault %s\n", a.fault.c_str());
    return 2;
  }
  CaptureReader capture;
  if (!capture.Open(a.capture)) {
    std::fprintf(stderr, "cannot open the capture %s\n", a.capture.c_str());
    return 2;
  }
  UiTextOurs hook(options);
  LoadedFrame frame;
  uint64_t frames = 0, claimed = 0, replaced = 0, runs_seen = 0, bad_flags = 0, bad_other = 0, bad_x = 0, bad_uv = 0, uv_exact = 0, no_runs_frames = 0;
  double max_dx = 0, max_du = 0;
  for (uint32_t f = 0; f < capture.frame_count(); ++f) {
    if (f % a.step != 0 && f + 1 != capture.frame_count()) continue;
    if (!capture.LoadFrame(f, frame)) {
      std::fprintf(stderr, "cannot read frame %u\n", f);
      return 2;
    }
    ++frames;
    runs_seen += frame.text_runs.count;
    no_runs_frames += frame.text_runs.count == 0;
    if (!hook.FrameReady(frame)) {
      std::fprintf(stderr, "the hook is not ready: %s\n", hook.error().c_str());
      return 2;
    }
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const rdr_adapter::DrawOrigin* origin = frame.OriginOf(i);
      if (!origin || !hook.Claims(*origin)) continue;
      ++claimed;
      rdr_adapter::ReplaceIn in;
      in.frame = &frame;
      in.draw = &frame.draws[i];
      in.draw_index = i;
      in.origin = origin;
      in.read_geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
      rdr_adapter::ReplaceOut out;
      rdr_adapter::KeepWhy why = rdr_adapter::KeepWhy::NotOurs;
      if (!hook.Replace(in, out, why)) continue;
      ++replaced;
      const Raw s = ReadRaw(capture, frame, i);
      if (!out.keep_stream_geometry || !s.ok || out.inline_vertices == nullptr || out.inline_vertex_bytes != frame.draws[i].vertex_bytes) {
        ++bad_flags;
        continue;
      }
      Raw o = s;
      std::memcpy(o.bytes.data(), out.inline_vertices, out.inline_vertex_bytes);
      bool x_ok = true, uv_ok = true, uv_same = true;
      for (int v = 0; v < 4; ++v) {
        const double dx = std::fabs(double(o.Get(v, s.pos)) - double(s.Get(v, s.pos)));
        max_dx = std::fmax(max_dx, dx);
        x_ok = x_ok && dx <= 0.005;
        for (uint32_t k = 0; k < 2; ++k) {
          const double du = std::fabs(double(o.Get(v, s.uv + 4 * k)) - double(s.Get(v, s.uv + 4 * k)));
          max_du = std::fmax(max_du, du);
          uv_ok = uv_ok && du <= 1e-6;
          uv_same = uv_same && o.Get(v, s.uv + 4 * k) == s.Get(v, s.uv + 4 * k);
        }
      }
      bad_x += !x_ok, bad_uv += !uv_ok, uv_exact += uv_same;
      // every other byte (y, z, whatever else the layout carries) is the stream's
      bool rest = true;
      for (int v = 0; v < 4 && rest; ++v)
        for (uint32_t b = 0; b < s.stride && rest; ++b) {
          const bool in_x = b >= s.pos && b < s.pos + 4, in_uv = b >= s.uv && b < s.uv + 8;
          if (!in_x && !in_uv && o.bytes[size_t(v) * s.stride + b] != s.bytes[size_t(v) * s.stride + b]) rest = false;
        }
      bad_other += !rest;
    }
  }
  const UiTextCensus& c = hook.census();
  std::printf("      frames %llu, run strings %llu; %s\n", (unsigned long long)frames, (unsigned long long)runs_seen, hook.Report().c_str());
  Line(runs_seen > 0 && no_runs_frames < frames, "runs", Num(runs_seen) + " run strings in " + Num(frames) + " frames (" + Num(no_runs_frames) + " frames without any: a capture that has no section 69 fails here)");
  Line(claimed > 0 && claimed == c.claimed, "claimed", Num(claimed) + " glyph draws claimed (hook census " + Num(c.claimed) + ")");
  Line(replaced > 0 && replaced == c.ours && bad_flags == 0, "replaced", Num(replaced) + " glyph draws replaced (hook census " + Num(c.ours) + "), seam flags wrong on " + Num(bad_flags));
  Line(replaced > 0 && bad_x == 0 && bad_uv == 0 && bad_other == 0, "quads",
       Num(replaced) + " replaced quads against the stream's bytes: corner x differs by more than 0.005 px on " + Num(bad_x) + " (largest " + Flt(max_dx) + " px), uv rect on " + Num(bad_uv) +
           " (largest " + Flt(max_du) + ", bit-equal on " + Num(uv_exact) + "), other bytes on " + Num(bad_other));
  const uint64_t not_laid_out = c.kept_icon + c.kept_norun;
  const uint64_t refused = c.kept_truncated + c.kept_count + c.kept_font + c.kept_geometry + c.kept_layout;
  Line(refused == 0, "kept", "kept only for the listed reasons: icon glyphs " + Num(c.kept_icon) + ", no run string " + Num(c.kept_norun) + "; refused (bar 0): truncated " + Num(c.kept_truncated) + ", count " +
       Num(c.kept_count) + ", font " + Num(c.kept_font) + ", geometry " + Num(c.kept_geometry) + ", layout " + Num(c.kept_layout));
  Line(c.claimed == c.ours + not_laid_out + refused, "census", "claimed " + Num(c.claimed) + " = ours " + Num(c.ours) + " + icon / no run " + Num(not_laid_out) + " + refused " + Num(refused));
  Line(a.kind != "editor" || c.narrow_segments > 0, "narrow",
       "segments " + Num(c.segments) + ", with a narrower quad factor " + Num(c.narrow_segments) + " (handled: the factor comes from the segment's first glyph draw, every quad of the segment still checked); the editor zip must show some");
  Line(c.fonts_ambiguous == 0, "fonts", "runs whose uv rects fit more than one font: " + Num(c.fonts_ambiguous));
  return g_fail;
}
