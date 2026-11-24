// Quads2DRig (Session 103, lane I1; docs\renderer\REMNANT_MAP.md section 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The OFFLINE proof rig of the 2D record (fview --quads-2d-rows, with --ui-ours --ui-quads / --movie-ours). On a captured frame the STREAM's own draws are the truth: for every T_Rect / movie draw
// UiQuads / MovieOurs build a Quad2D row from the draw's numbers (Truth), pass it through the record's own format (the section-58 writer and parser, via g_roundtrip) and the planted twins
// (Make), and draw it with the drawer's pure function (quads2d_blocks.h BlocksOf) instead of reading the stream's constants again. The stream's own 2D pass is the reference the picture is
// compared with. What the stream keeps (target, depth, viewport, the blend and alpha-test STATE) is cross-checked against the row's copy (CheckState): the live drawer sets them from the row.
//
// Header-only, no link dependency (UiQuads / MovieOurs are linked into the game, rplay and fview): the record round trip is a function pointer fview sets. Single render thread.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#include "../renderer_adapter/adapter_frame.h"
#include "rows_quad2d.h"   // S106 G4-2
#include "../renderer_adapter/draw_replace_hook.h"   // S103 SC: ReplaceOut::OwnDraw (the row's scissor)
#include "quads2d_blocks.h"
#include "quads2d_core.h"

namespace rdr_scene::q2d::rig {

inline bool g_on = false;   // fview --quads-2d-rows

// Planted twins: each must be CAUGHT (the picture moves, or a counter the check reads rises).
struct Faults {
  bool drop_rect = false;       // a rect row lost on the way (the fade is not drawn)
  bool swap_vertices = false;   // corners 1 and 2 of a rect exchanged (a bow-tie)
  bool wrong_blend = false;     // the row's blend class is not the draw's
  bool stale_plane = false;     // a movie row carrying the PREVIOUS claimed row's plane images
  bool lost_constants = false;  // the colour constant never read (the row has no c1)
  bool drop_movie = false;      // a movie row lost on the way (the video quad is not drawn)
  bool wrong_uv = false;        // S103 I2: a sprite row's uv rectangle shrunk to its first quarter (every picture zoomed)
  bool drop_hsl = false;        // S103 I2: a hue-shifted sprite row loses its hue-shift constant (the plain program draws)
  bool wrong_picture = false;   // S103 I2: a joined sprite key is answered with ANOTHER joined key's picture
  bool lost_blend = false;      // S103 I2: the row's blend class is lost (blend unknown: the drawer refuses it, the stream's draw stays)
  bool glyph_uv_shift = false;     // S103 I3: a glyph row's uv rectangle moved by a third of its width (the neighbouring glyph is drawn)
  bool glyph_wrong_sheet = false;  // S103 I3: a glyph row's sheet key is answered with ANOTHER joined key's picture
  bool glyph_lost_colour = false;  // S103 I3: a glyph row's colour is lost (opaque white: the outline / shadow passes and the tints are gone)
  bool glyph_drop = false;         // S103 I3: every fifth glyph row is lost on the way (a dropped glyph)
  bool glyph_stale_regs = false;   // S103 I3 review: the register uploads the shadow sees carry the PREVIOUS frame's stamp (every glyph row is incomplete: refused, counted, the stream's draw stays)
  bool proj_identity = false;      // S103 P3c: a matrix-placed row's matrix is ignored (the identity is used: the positions stay in pixel units, off the clip volume)
  bool proj_transposed = false;    // S103 P3c: a matrix-placed row's matrix is transposed (rows for columns)
  bool c28_ignored = false;        // S103 P3c: a matrix-placed row is placed by the pixel-unit rule (c28 not looked at)
  bool scissor_ignored = false;    // S103 SC: every row's scissor is dropped (scissor off: the menu lists are drawn unclipped)
  bool scissor_y_flipped = false;  // S103 SC: every row's scissor box is measured from the bottom edge (720 - y - h)
  bool scissor_prev_row = false;   // S103 SC: every row takes the PREVIOUS row's scissor (the box of the draw before)
  bool strip_order = false;        // S103 P3b: a strip-site row (rect 0x82BC484C, sprite 0x82BC8BEC) is taken in the draw's Z order, not the perimeter order (a bow-tie / a twisted picture)
  static bool Parse(const std::string& list, Faults& out, std::string& error) {
    out = Faults{};
    size_t at = 0;
    while (at < list.size()) {
      size_t end = list.find(',', at);
      if (end == std::string::npos) end = list.size();
      const std::string n = list.substr(at, end - at);
      at = end + 1;
      if (n.empty()) continue;
      if (n == "drop_rect") out.drop_rect = true;
      else if (n == "swap_vertices") out.swap_vertices = true;
      else if (n == "wrong_blend") out.wrong_blend = true;
      else if (n == "stale_plane") out.stale_plane = true;
      else if (n == "lost_constants") out.lost_constants = true;
      else if (n == "drop_movie") out.drop_movie = true;
      else if (n == "wrong_uv") out.wrong_uv = true;
      else if (n == "drop_hsl") out.drop_hsl = true;
      else if (n == "wrong_picture") out.wrong_picture = true;
      else if (n == "lost_blend") out.lost_blend = true;
      else if (n == "glyph_uv_shift") out.glyph_uv_shift = true;
      else if (n == "glyph_wrong_sheet") out.glyph_wrong_sheet = true;
      else if (n == "glyph_lost_colour") out.glyph_lost_colour = true;
      else if (n == "glyph_drop") out.glyph_drop = true;
      else if (n == "glyph_stale_regs") out.glyph_stale_regs = true;
      else if (n == "strip_order") out.strip_order = true;
      else if (n == "scissor_ignored") out.scissor_ignored = true;
      else if (n == "scissor_y_flipped") out.scissor_y_flipped = true;
      else if (n == "scissor_prev_row") out.scissor_prev_row = true;
      else if (n == "proj_identity") out.proj_identity = true;
      else if (n == "proj_transposed") out.proj_transposed = true;
      else if (n == "c28_ignored") out.c28_ignored = true;
      else {
        error = "unknown --quads-2d-fault '" + n + "' (drop_rect, swap_vertices, wrong_blend, stale_plane, lost_constants, drop_movie, wrong_uv, drop_hsl, wrong_picture, lost_blend, glyph_uv_shift, glyph_wrong_sheet, glyph_lost_colour, glyph_drop, glyph_stale_regs, strip_order, proj_identity, proj_transposed, c28_ignored, scissor_ignored, scissor_y_flipped, scissor_prev_row)";
        return false;
      }
    }
    return true;
  }
};
inline Faults g_faults;

// fview installs the record's own round trip (SerializeRecord + ParseRecord of a one-row table). False + `error`: the section was refused.
inline std::function<bool(Quad2D&, std::string&)> g_roundtrip;

struct Stats {
  uint64_t rows[kQkCount] = {};                 // rows made, by kind
  uint64_t drawn = 0, refused[uint8_t(Refuse::Count)] = {};
  uint64_t roundtrip_refused = 0;               // the section parser dropped the row
  uint64_t blend_mismatch = 0, alpha_mismatch = 0;   // the row's state is not the stream draw's
  uint64_t blend_seen[kQkCount][kQbCount] = {};      // the stream's blend class by kind (the census REMNANT_MAP U6 asks for)
  uint64_t alpha_test_on = 0;                   // rows whose draw has the alpha test on
  uint64_t degenerate = 0;                      // drop plants applied
  uint64_t stale = 0;                           // stale_plane applied
  // S103 I2 (sprites)
  uint64_t hsl_rows = 0, hsl_dropped = 0;       // rows with a hue shift; the drop_hsl plant applied
  uint64_t sprites_drawn = 0;                   // sprite rows that passed every drawer rule
  uint64_t uv_planted = 0;                      // wrong_uv applied
  uint64_t pic_file = 0, pic_stream = 0;        // sprite rows whose picture came from the key table (UiOurs' join) / stayed the stream's (no join: the guest-object source in the live drawer, unit-tested)
  uint64_t pic_key_changed = 0;                 // a known key arrived with a different image than its first answer
  uint64_t pic_wrong = 0;                       // wrong_picture applied
  uint64_t pic_state_bad = 0;                   // the key table's answer is not the image the draw's own join gave (a key-table bug; 0 with no plant)
  uint64_t hsl_state_bad = 0;                   // the row's hue-shift presence differs from the stream draw's program (hsl program vs not)
  uint64_t blend_unknown = 0;                   // rows refused by the blend rule (the drawer cannot draw an unknown blend)
  // S103 I3 (glyphs)
  uint64_t glyph_taken = 0, glyph_drawn = 0;     // glyph stream draws the hook took over (a row was attempted) / rows that passed every drawer rule
  uint64_t glyph_pic_file = 0, glyph_pic_stream = 0, glyph_pic_wrong = 0, glyph_pic_state_bad = 0;   // like the sprites': the key table's answer / the stream's own picture / the wrong_sheet plant / the key table is not the join
  uint64_t glyph_uv_planted = 0, glyph_colour_planted = 0, glyph_dropped = 0;   // plants applied
  uint64_t glyph_topology[8] = {};              // the stream's primitive topology (Vulkan enum) of the glyph draws taken
  uint64_t glyph_shadow_rows = 0, glyph_shadow_mismatch = 0, glyph_shadow_incomplete = 0;   // S103 I3 review: glyph rows built through the TAP path (register shadow + op-19 core over a fake guest memory holding the stream's numbers); rows that differ from the stream's own numbers; rows the shadow left incomplete
  uint64_t strip_rect_rows = 0, strip_m_rows = 0, strip_drawn = 0, strip_planted = 0;   // S103 P3b: rows of the two strip sites (rect 0x82BC484C / sprite 0x82BC8BEC), those that passed every drawer rule, the strip_order plant applied
  uint64_t proj_rows[kQkCount] = {}, proj_drawn[kQkCount] = {}, proj_planted = 0, strip_tap_rows = 0, strip_tap_mismatch = 0, proj_tap_rows = 0;   // S103 P3c: matrix-placed rows made / drawn by kind, the three matrix plants applied, strip rows built through the tap path (and those that differ from the stream's numbers), glyph rows whose matrix came through the tap
  uint64_t tg_frame = 0, tg_offscreen = 0, tg_by_size = 0, tg_unknown = 0;   // S103 SC: rows by target class (review F3: the SURFACE the stream draw renders into: the frame-buffer role image = the default back buffer; a capture with no role table falls back to the image's size, counted in tg_by_size; no readable target = tg_unknown, the row has no target state)
  uint64_t sc_rows = 0, sc_clipping = 0, sc_planted = 0;   // S103 SC: rows made with a scissor (all, from the stream draw's own window scissor), those whose box is smaller than 1280x720 (a clipped list / panel), the scissor plants applied
  uint64_t glyph_zero_alpha_color = 0;          // glyph rows whose colour alpha is 0 (the draw is invisible)
};
inline Stats g_stats;
// The alpha tests the stream draws carry, by (kind, function, reference): the census REMNANT_MAP U6 asks for (the live tap cannot read them; a per-kind default needs this table).
inline std::map<std::tuple<int, int, float>, uint64_t> g_alpha_tests;
inline uint32_t g_seq = 0;
inline std::map<std::pair<int, float>, uint64_t> g_glyph_alpha;   // S103 I3: the glyph draws' alpha tests by (function, reference)
inline bool g_last_row_glyph = false;   // S103 I3: the last Make was a glyph row (the picture step reads it)
inline std::map<std::pair<int, float>, uint64_t> g_sprite_alpha;   // S103 I2: the sprite draws' alpha tests by (function, reference)
// S103 I2: the key table of the rig = what UiOurs' join said for each sprite key this replay (key -> the renderer image of the file picture), and a census of the sprite-site fragment programs by name (draws, first guest frame):
// the REMNANT_MAP U8 question "which sprites carry a hue shift" answered from the stream.
inline std::map<uint32_t, uint32_t> g_pictures;
inline uint32_t g_last_sprite_key = 0;   // the key of the last sprite row Build made (after the record's round trip): the key the picture is chosen by
inline std::map<std::string, std::pair<uint64_t, uint64_t>> g_programs;
// The image the row draws for `key` whose join answered `joined`: the key table's entry (the row selects its picture by KEY, not by the draw's slot). The wrong_picture plant answers another key's.
inline uint32_t PictureFor(uint32_t key, uint32_t joined, bool& planted) {
  planted = false;
  const auto ins = g_pictures.try_emplace(key, joined);   // F3: the first answer for a key stays; a known key that arrives with ANOTHER image is counted
  if (!ins.second && ins.first->second != joined) ++g_stats.pic_key_changed;
  if (g_faults.wrong_picture)
    for (const auto& kv : g_pictures)
      if (kv.first != key && kv.second != joined) return planted = true, kv.second;
  return g_pictures[key];
}

// S103 I3: the same key table for a GLYPH row's sheet; the glyph_wrong_sheet plant answers another key's picture.
inline uint32_t PictureForGlyph(uint32_t key, uint32_t joined, bool& planted) {
  planted = false;
  const auto ins = g_pictures.try_emplace(key, joined);
  if (!ins.second && ins.first->second != joined) ++g_stats.pic_key_changed;
  if (g_faults.glyph_wrong_sheet)
    for (const auto& kv : g_pictures)
      if (kv.first != key && kv.second != joined) return planted = true, kv.second;
  return g_pictures[key];
}

// The sprite census line (a separate line: the I1 line's format is fixed): rows, hue-shifted rows, where the pictures came from, the plants applied, and the fragment programs the sprite site draws (U8).
inline std::string ReportSprites() {
  char line[600];
  const Stats& s = g_stats;
  std::snprintf(line, sizeof(line),
                "[fview] [rdr][2d] sprites: rows %llu, drawn %llu, hue-shifted %llu, drawn pictures: from the key table %llu, the stream's own %llu; state: hsl %llu blend-unknown %llu pic %llu; plants applied: uv %llu hsl-dropped %llu pic-wrong %llu; sprite-site programs:",
                (unsigned long long)s.rows[kQkSprite], (unsigned long long)s.sprites_drawn, (unsigned long long)s.hsl_rows, (unsigned long long)s.pic_file, (unsigned long long)s.pic_stream, (unsigned long long)s.hsl_state_bad, (unsigned long long)s.blend_unknown,
                (unsigned long long)s.pic_state_bad, (unsigned long long)s.uv_planted, (unsigned long long)s.hsl_dropped, (unsigned long long)s.pic_wrong);
  std::string out = line;
  char bl[200];
  std::snprintf(bl, sizeof(bl), " stream blend: off %llu alpha %llu add %llu other %llu;", (unsigned long long)s.blend_seen[kQkSprite][kQbOff], (unsigned long long)s.blend_seen[kQkSprite][kQbAlpha], (unsigned long long)s.blend_seen[kQkSprite][kQbAdd],
                (unsigned long long)s.blend_seen[kQkSprite][kQbOther]);
  out += bl;
  out += " alpha tests (function@reference: draws):";
  for (const auto& kv : g_sprite_alpha) {
    char b[80];
    std::snprintf(b, sizeof(b), " %d@%g: %llu;", kv.first.first, double(kv.first.second), (unsigned long long)kv.second);
    out += b;
  }
  for (const auto& kv : g_programs) {
    char b[160];
    std::snprintf(b, sizeof(b), " %s %llu (first frame %llu);", kv.first.c_str(), (unsigned long long)kv.second.first, (unsigned long long)kv.second.second);
    out += b;
  }
  return out;
}
// S103 I3: the glyph census line (a separate line: the I1 / I2 lines' formats are fixed).
inline std::string ReportGlyphs() {
  char line[900];
  const Stats& s = g_stats;
  std::snprintf(line, sizeof(line),
                "[fview] [rdr][2d] glyphs: taken %llu, rows %llu, drawn %llu, refused-by-prim %llu, pictures: from the key table %llu, the stream's own %llu; state: pic %llu; plants applied: uv %llu colour %llu dropped %llu sheet %llu; shadow: rows %llu mismatch %llu incomplete %llu; "
                "stream blend: off %llu alpha %llu add %llu other %llu; topology (vulkan 3 list / 4 strip / 5 fan): 3 %llu 4 %llu 5 %llu other %llu; alpha tests (function@reference: draws):",
                (unsigned long long)s.glyph_taken, (unsigned long long)s.rows[kQkGlyph], (unsigned long long)s.glyph_drawn, (unsigned long long)s.refused[uint8_t(Refuse::Prim)], (unsigned long long)s.glyph_pic_file, (unsigned long long)s.glyph_pic_stream,
                (unsigned long long)s.glyph_pic_state_bad, (unsigned long long)s.glyph_uv_planted, (unsigned long long)s.glyph_colour_planted, (unsigned long long)s.glyph_dropped, (unsigned long long)s.glyph_pic_wrong, (unsigned long long)s.glyph_shadow_rows, (unsigned long long)s.glyph_shadow_mismatch, (unsigned long long)s.glyph_shadow_incomplete,
                (unsigned long long)s.blend_seen[kQkGlyph][kQbOff], (unsigned long long)s.blend_seen[kQkGlyph][kQbAlpha], (unsigned long long)s.blend_seen[kQkGlyph][kQbAdd], (unsigned long long)s.blend_seen[kQkGlyph][kQbOther],
                (unsigned long long)s.glyph_topology[3], (unsigned long long)s.glyph_topology[4], (unsigned long long)s.glyph_topology[5], (unsigned long long)(s.glyph_topology[0] + s.glyph_topology[1] + s.glyph_topology[2] + s.glyph_topology[6] + s.glyph_topology[7]));
  std::string out = line;
  for (const auto& kv : g_glyph_alpha) {
    char b[80];
    std::snprintf(b, sizeof(b), " %d@%g: %llu;", kv.first.first, double(kv.first.second), (unsigned long long)kv.second);
    out += b;
  }
  return out;
}
// S103 P3b: the menu leaves' strip rows (the flat T_Rect of sub_82BC4450 at 0x82BC484C, the plain T_Sprite (technique 0x8206AA0C) of sub_82BC8180 at 0x82BC8BEC).
inline std::string ReportP3b() {
  char line[300];
  const Stats& s = g_stats;
  std::snprintf(line, sizeof(line), "[fview] [rdr][2d] strips: rect rows %llu, sprite-M rows %llu, drawn %llu; plants applied: strip-order %llu", (unsigned long long)s.strip_rect_rows, (unsigned long long)s.strip_m_rows, (unsigned long long)s.strip_drawn, (unsigned long long)s.strip_planted);
  return line;
}
// S103 P3c: the matrix-placed rows (c28 != 0: the menu's text, M sprites and rects), by kind.
inline std::string ReportP3c() {
  char line[420];
  const Stats& s = g_stats;
  std::snprintf(line, sizeof(line), "[fview] [rdr][2d] matrix: rows rect %llu sprite %llu glyph %llu, drawn rect %llu sprite %llu glyph %llu; tap: strip rows %llu (differ %llu), glyph matrices through the register file %llu; plants applied: %llu",
                (unsigned long long)s.proj_rows[kQkRect], (unsigned long long)s.proj_rows[kQkSprite], (unsigned long long)s.proj_rows[kQkGlyph], (unsigned long long)s.proj_drawn[kQkRect], (unsigned long long)s.proj_drawn[kQkSprite],
                (unsigned long long)s.proj_drawn[kQkGlyph], (unsigned long long)s.strip_tap_rows, (unsigned long long)s.strip_tap_mismatch, (unsigned long long)s.proj_tap_rows, (unsigned long long)s.proj_planted);
  return line;
}
inline std::string Report() {
  char line[700];
  const Stats& s = g_stats;
  uint64_t ref = 0;
  for (int i = 1; i < int(Refuse::Count); ++i) ref += s.refused[i];
  std::snprintf(line, sizeof(line),
                "[fview] [rdr][2d] rows: rect %llu, movie %llu, drawn %llu, refused %llu (layout %llu, kind %llu, verts %llu, constants %llu, projection %llu, finite %llu, not-rect %llu, blend %llu, planes %llu, variant %llu, picture %llu), "
                "section-refused %llu, state mismatch: blend %llu alpha %llu; stream blend: rect off %llu alpha %llu add %llu other %llu, movie off %llu alpha %llu add %llu other %llu; alpha test on %llu; plants applied: dropped %llu stale %llu",
                (unsigned long long)s.rows[kQkRect], (unsigned long long)s.rows[kQkYuvAlpha], (unsigned long long)s.drawn, (unsigned long long)ref, (unsigned long long)s.refused[1], (unsigned long long)s.refused[2],
                (unsigned long long)s.refused[3], (unsigned long long)s.refused[4], (unsigned long long)s.refused[5], (unsigned long long)s.refused[6], (unsigned long long)s.refused[7],
                (unsigned long long)s.refused[8], (unsigned long long)s.refused[9], (unsigned long long)s.refused[10], (unsigned long long)s.refused[11], (unsigned long long)s.roundtrip_refused, (unsigned long long)s.blend_mismatch, (unsigned long long)s.alpha_mismatch,
                (unsigned long long)s.blend_seen[kQkRect][kQbOff], (unsigned long long)s.blend_seen[kQkRect][kQbAlpha], (unsigned long long)s.blend_seen[kQkRect][kQbAdd], (unsigned long long)s.blend_seen[kQkRect][kQbOther],
                (unsigned long long)s.blend_seen[kQkYuvAlpha][kQbOff], (unsigned long long)s.blend_seen[kQkYuvAlpha][kQbAlpha], (unsigned long long)s.blend_seen[kQkYuvAlpha][kQbAdd],
                (unsigned long long)s.blend_seen[kQkYuvAlpha][kQbOther], (unsigned long long)s.alpha_test_on, (unsigned long long)s.degenerate, (unsigned long long)s.stale);
  std::string out = line;
  out += "; alpha tests (kind function@reference: draws):";
  for (const auto& kv : g_alpha_tests) {
    char b[80];
    std::snprintf(b, sizeof(b), " %s %d@%g: %llu;", std::get<0>(kv.first) == kQkRect ? "rect" : "movie", std::get<1>(kv.first), double(std::get<2>(kv.first)), (unsigned long long)kv.second);
    out += b;
  }
  return out;
}

// S103 SC review F3: the colour target SURFACE of a stream draw (what the live tap reads at the op-45 holder: its size word, and whether it is the default back buffer). In a capture frame: the last SetTargets before the draw names the
// image; the image's size is the surface's; the default back buffer is the image the frame declares FrameBuffer (names.h SurfaceRole: "the screen-size pair the post chain, the HUD, text, particles and the movie draw into")
// or Present. A frame with no role table at all (older captures) falls back to the image being 1280x720 and is counted (tg_by_size): still the surface's size, never the viewport's. A 1280x720 target that is NOT the default (the
// scene colour) reads Offscreen.
struct RigTarget {
  bool known = false;
  uint32_t w = 0, h = 0;
  bool is_default = false;
};
// The pure half (tools\scene\sc_unit.cpp tests it): an image of size w x h and the frame's role table -> the surface's class. A 1280x720 image the roles do not name FrameBuffer / Present is NOT the default back buffer.
inline RigTarget TargetOfImage(uint32_t image, uint32_t w, uint32_t h, const rdr_adapter::SurfaceRoleEntry* roles, uint32_t n_roles) {
  RigTarget out;
  if (!image || !w || !h) return out;
  out.known = true, out.w = w, out.h = h;
  if (n_roles) {
    for (uint32_t i = 0; i < n_roles; ++i)
      if (roles[i].image == image) out.is_default = roles[i].role == rdr_adapter::SurfaceRole::FrameBuffer || roles[i].role == rdr_adapter::SurfaceRole::Present;
    if (Q2D_PLANT(47)) out.is_default = w == 1280u && h == 720u;   // (plant final_by_size: the rig judges by size too)
  } else out.is_default = w == 1280u && h == 720u, ++g_stats.tg_by_size;
  return out;
}
inline RigTarget TargetOf(const rdr_adapter::ReplaceIn& in) {
  RigTarget out;
  if (!in.frame || !in.image_size) return out;
  const rdr_adapter::LoadedFrame& fr = *in.frame;
  uint32_t image = 0;
  for (uint32_t k = 0; k < fr.ops.count; ++k) {
    const rdr_adapter::Op& op = fr.ops[k];
    if (op.kind == rdr_adapter::OpKind::SetTargets) {
      const rdr::TargetsOp& t = fr.targets[op.index];
      image = t.color_count ? t.color[0].image : 0;
    } else if (op.kind == rdr_adapter::OpKind::Draw && op.index == in.draw_index) break;
  }
  uint32_t w = 0, h = 0;
  if (!image || !in.image_size(image, w, h)) return out;
  return TargetOfImage(image, w, h, fr.surface_roles.count ? &fr.surface_roles[0] : nullptr, fr.surface_roles.count);
}

// The TRUTH row of a stream draw: the four vertices (x y z [u v]) as the stream carries them, the three constants, the blend and the alpha test of the draw's fixed state.
inline Quad2D Truth(uint8_t kind, uint32_t site, uint32_t object, uint64_t guest_frame, const float v[4][5], const float pos_scale[4], const float color[4], float use_proj, const rdr_adapter::DrawFixed& fixed,
                    uint32_t tex_key = 0, const float* hsl = nullptr, const float* channel = nullptr, uint8_t prim = 0, const float* proj = nullptr, const rdr::Rect* stream_scissor = nullptr, const float* stream_viewport = nullptr, const RigTarget* stream_target = nullptr) {   // S103 SC scope add: `stream_viewport` = the stream draw's viewport x y w h (the rig's target size / class: the viewport of a draw covers its surface [U: a page drawn with a smaller viewport reads as a smaller target])
     // S103 SC: `stream_scissor` = the stream draw's window scissor (viewport ∩ game scissor ∩ surface, 1x guest pixels): the row's scissor in the rig   // S103 P3c: `proj` = c29..c32 of a matrix-placed draw (16 floats)   // S103 I2: a sprite's picture key and (hue-shifted program) its hsl triple; S103 I3: a glyph's channel selector (c4) and primitive word
  Quad2D q;
  q.kind = kind;
  q.site = site;
  q.object = object;
  q.frame = uint32_t(guest_frame);
  q.seq = ++g_seq;
  q.nverts = 4;
  q.prim = prim;   // S103 P3b: the strip sites (rect 0x82BC484C, sprite 0x82BC8BEC) carry the op-19 primitive word 6 (the other rect / sprite rows 0 here)
  q.stride_bytes = uint8_t(StrideOfKind(kind));
  const uint32_t fl = FloatsOfKind(kind);
  for (int i = 0; i < 4; ++i)
    for (uint32_t c = 0; c < fl; ++c) q.verts[i][c] = v[i][c];
  std::memcpy(q.pos_scale, pos_scale, 16);
  std::memcpy(q.color, color, 16);
  q.use_proj = use_proj;
  if (kind == kQkSprite) {
    q.tex_key[0] = tex_key;
    q.have |= kQhPlanes;
    if (hsl) std::memcpy(q.hsl, hsl, 12), q.have |= kQhHsl, ++g_stats.hsl_rows;
  }
  if (kind == kQkGlyph) {   // S103 I3: the sheet's key, the channel selector, the primitive word
    q.tex_key[0] = tex_key;
    q.have |= kQhPlanes;
    q.prim = prim;
    if (channel) std::memcpy(q.hsl, channel, 16), q.have |= kQhChannel;
    if (color[3] == 0.0f) ++g_stats.glyph_zero_alpha_color;
  }
  q.have |= kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest;
  if (stream_viewport && stream_target && stream_target->known) {   // S103 SC (scope add): the row's target class and viewport. Review F3: the class and size are the SURFACE's (the live tap reads *(RT0) + 36 and the default back buffer's pointer), never the viewport's
    for (int i = 0; i < 4; ++i) q.viewport[i] = int32_t(stream_viewport[i]);
    q.target_size[0] = stream_target->w, q.target_size[1] = stream_target->h;
    q.pass = stream_target->is_default ? kQpFinal : kQpOffscreen;
    q.have |= kQhTarget;
    ++(q.pass == kQpFinal ? g_stats.tg_frame : g_stats.tg_offscreen);
  } else if (stream_viewport) ++g_stats.tg_unknown;   // (no readable target: the row stays a hole, RowProblem refuses it)
  if (stream_scissor) {   // S103 SC (layout 4): the row's scissor is the stream draw's own window scissor
    q.have |= kQhScissor, q.scissor_on = 1;
    q.scissor[0] = stream_scissor->x, q.scissor[1] = stream_scissor->y, q.scissor[2] = int32_t(stream_scissor->width), q.scissor[3] = int32_t(stream_scissor->height);
    ++g_stats.sc_rows;
    if (q.scissor[0] > 0 || q.scissor[1] > 0 || q.scissor[2] < 1280 || q.scissor[3] < 720) ++g_stats.sc_clipping;
  }
  if (proj) std::memcpy(q.proj, proj, sizeof(q.proj)), q.have |= kQhMatrix, ++g_stats.proj_rows[kind];
  q.blend = BlendClass(fixed.state.blend[0].enable, fixed.state.blend[0].src_color, fixed.state.blend[0].dst_color, fixed.state.blend[0].color_op);
  q.alpha_func = fixed.alpha_test ? fixed.alpha_func : 7;
  q.alpha_ref = fixed.alpha_test ? fixed.alpha_ref : 0.0f;
  ++g_stats.rows[kind];
  ++g_stats.blend_seen[kind][q.blend];
  if (fixed.alpha_test && kind == kQkGlyph) ++g_glyph_alpha[{int(fixed.alpha_func), fixed.alpha_ref}];
  if (fixed.alpha_test && kind != kQkSprite && kind != kQkGlyph) ++g_stats.alpha_test_on, ++g_alpha_tests[{int(kind), int(fixed.alpha_func), fixed.alpha_ref}];
  if (fixed.alpha_test && kind == kQkSprite) ++g_sprite_alpha[{int(fixed.alpha_func), fixed.alpha_ref}];
  return q;
}

// S103 SC: copies the scissor and target state of the truth row onto a row the tap core assembled from a fake guest memory (which carries no state object / recorder history).
inline void CarryDeviceState(const Quad2D& truth, Quad2D& q) {
  q.have = (q.have & ~uint32_t(kQhScissor | kQhTarget)) | (truth.have & (kQhScissor | kQhTarget));
  q.scissor_on = truth.scissor_on, q.pass = truth.pass;
  std::memcpy(q.scissor, truth.scissor, sizeof(q.scissor)), std::memcpy(q.viewport, truth.viewport, sizeof(q.viewport)), std::memcpy(q.target_size, truth.target_size, sizeof(q.target_size));
}
// S103 I3 review: a glyph row built THROUGH THE TAP PATH. The stream's own numbers for this draw (the Truth row: vertices, c0 / c1 / c4 / c28, the sheet key) are written into a fake guest memory exactly as the game would hold them
// (big-endian words; the key at object + 416; four register uploads), and the production core (quads2d_core.h: OnOp21Shadow for each upload, then the glyph scope's OnOp19) assembles the row. That row, not the Truth row, is what is
// drawn; it is compared with the Truth row and a difference is counted (glyph_shadow_mismatch). What the tap cannot read (blend, alpha test) is carried over from the Truth row.
struct TapMem {
  std::map<uint32_t, uint32_t> w;
  void PutF(uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    w[a] = v;
  }
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    for (uint32_t i = 0; i < n; ++i) {
      auto it = w.find(a + 4 * i);
      if (it == w.end()) return false;
      out[i] = it->second;
    }
    return true;
  }
};
inline bool BuildGlyphViaTap(const Quad2D& truth, Quad2D& out) {
  constexpr uint32_t kObj = 0x40001000u, kVerts = 0x40002000u, kPay = 0x40003000u;
  TapMem m;
  m.w[kObj + kGlyphKeyOffset] = truth.tex_key[0];
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) m.PutF(kVerts + 20u * i + 4u * c, truth.verts[i][c]);
  for (int c = 0; c < 4; ++c) m.PutF(kPay + 4u * c, truth.pos_scale[c]), m.PutF(kPay + 16u + 4u * c, truth.color[c]), m.PutF(kPay + 32u + 4u * c, truth.hsl[c]), m.PutF(kPay + 48u + 4u * c, c == 0 ? truth.use_proj : 0.f);
  for (int c = 0; c < 16; ++c) m.PutF(kPay + 64u + 4u * c, truth.proj[c]);   // S103 P3c: c29..c32 in one upload, as sub_82BC2A30 makes it
  Store store;
  Counters counters;
  Shadow sh;
  Scope scope;
  const uint32_t frame = truth.frame, stamp = g_faults.glyph_stale_regs ? frame - 1 : frame;
  OnOp21Shadow(sh, m, 0, 1, kPay, counters, stamp);
  OnOp21Shadow(sh, m, 1, 1, kPay + 16, counters, stamp);
  OnOp21Shadow(sh, m, kSelectorReg, 1, kPay + 32, counters, stamp);
  OnOp21Shadow(sh, m, 28, 1, kPay + 48, counters, stamp);
  if (truth.have & kQhMatrix) OnOp21Shadow(sh, m, kMatrixReg, kMatrixRegs, kPay + 64, counters, stamp);
  OpenScope(scope, kQkGlyph, truth.object ? truth.object : kObj);
  scope.object = kObj;   // (the key is read at kObj + 416)
  OnOp19(scope, m, store, frame, truth.site, truth.prim, 4, 20, kVerts, counters, &sh);
  CloseScope(scope, store, frame, counters);
  std::vector<Quad2D> rows;
  store.Drain(frame, rows);
  if (rows.size() != 1) return false;
  Quad2D q = rows[0];
  ++g_stats.glyph_shadow_rows;
  constexpr uint32_t kNeed = kQhPosScale | kQhColour | kQhProj | kQhChannel;
  if ((q.have & kNeed) != kNeed) ++g_stats.glyph_shadow_incomplete;
  else {
    bool same = q.kind == truth.kind && q.site == truth.site && q.prim == truth.prim && q.tex_key[0] == truth.tex_key[0] && q.use_proj == truth.use_proj && q.pos_scale[0] == truth.pos_scale[0] && q.pos_scale[1] == truth.pos_scale[1];
    for (int i = 0; i < 4; ++i) same = same && q.color[i] == truth.color[i] && q.hsl[i] == truth.hsl[i];
    if (truth.have & kQhMatrix) {   // S103 P3c: the matrix came through the tap's register file, unchanged
      ++g_stats.proj_tap_rows;
      same = same && (q.have & kQhMatrix) && std::memcmp(q.proj, truth.proj, sizeof(q.proj)) == 0;
    }
    for (int v = 0; v < 4; ++v)
      for (int c = 0; c < 5; ++c) same = same && q.verts[v][c] == truth.verts[v][c];
    if (!same) ++g_stats.glyph_shadow_mismatch;
  }
  q.seq = truth.seq, q.object = truth.object, q.blend = truth.blend, q.alpha_func = truth.alpha_func, q.alpha_ref = truth.alpha_ref;
  q.have |= truth.have & (kQhBlend | kQhAlphaTest);
  CarryDeviceState(truth, q);   // S103 SC: the scissor / target / viewport the stream draw has (the fake memory above holds no state object or recorder history: the live tap fills these from the thread's recorders, proven in sc_unit)
  out = q;
  return true;
}

// S103 P3c: a STRIP-site row (the flat T_Rect of sub_82BC4450 at 0x82BC484C, the plain T_Sprite (technique 0x8206AA0C) of sub_82BC8180 at 0x82BC8BEC) built through the tap path as the leaf makes it: the scope's op-21 uploads (c0, c1, c28 and, for a
// matrix-placed draw, c29..c32 in one upload, sub_82BC2A30), then the op-19 (the vertices, the primitive word, the stride). The row that comes out is compared with the truth row; a difference is counted.
inline bool BuildStripViaTap(const Quad2D& truth, Quad2D& out) {
  constexpr uint32_t kObj = 0x40001000u, kVerts = 0x40002000u, kPay = 0x40003000u;
  const bool rect = truth.kind == kQkRect;
  const uint32_t stride = rect ? 12u : 20u;
  TapMem m;
  if (!rect) m.w[kObj + kGlyphKeyOffset] = truth.tex_key[0];
  for (int i = 0; i < 4; ++i)
    for (uint32_t c = 0; c < stride / 4; ++c) m.PutF(kVerts + stride * i + 4u * c, truth.verts[i][c]);
  for (int c = 0; c < 4; ++c) m.PutF(kPay + 4u * c, truth.pos_scale[c]), m.PutF(kPay + 16u + 4u * c, truth.color[c]), m.PutF(kPay + 32u + 4u * c, c == 0 ? truth.use_proj : 0.f);
  for (int c = 0; c < 16; ++c) m.PutF(kPay + 48u + 4u * c, truth.proj[c]);
  Store store;
  Counters counters;
  Scope scope;
  const uint32_t frame = truth.frame;
  OpenScope(scope, rect ? kQkRect : kQkGlyph, kObj);   // (the variant leaf's scope is a glyph-kind scope: OnOp19 pairs site 0x82BC8BEC with it)
  OnOp21(scope, m, 0, 1, kPay, counters);
  OnOp21(scope, m, 1, 1, kPay + 16, counters);
  OnOp21(scope, m, 28, 1, kPay + 32, counters);
  if (truth.have & kQhMatrix) OnOp21(scope, m, kMatrixReg, kMatrixRegs, kPay + 48, counters);
  OnOp19(scope, m, store, frame, truth.site, truth.prim, 4, stride, kVerts, counters);
  CloseScope(scope, store, frame, counters);
  std::vector<Quad2D> rows;
  store.Drain(frame, rows);
  if (rows.size() != 1) return false;
  Quad2D q = rows[0];
  ++g_stats.strip_tap_rows;
  bool same = q.kind == truth.kind && q.site == truth.site && q.prim == truth.prim && q.use_proj == truth.use_proj && q.tex_key[0] == truth.tex_key[0] && ((q.have & kQhMatrix) != 0) == ((truth.have & kQhMatrix) != 0);
  for (int i = 0; i < 4; ++i) same = same && q.color[i] == truth.color[i];
  same = same && q.pos_scale[0] == truth.pos_scale[0] && q.pos_scale[1] == truth.pos_scale[1] && std::memcmp(q.proj, truth.proj, sizeof(q.proj)) == 0;
  for (int v = 0; v < 4; ++v)
    for (uint32_t c = 0; c < stride / 4; ++c) same = same && q.verts[v][c] == truth.verts[v][c];
  if (!same) ++g_stats.strip_tap_mismatch;
  q.seq = truth.seq, q.object = truth.object, q.blend = truth.blend, q.alpha_func = truth.alpha_func, q.alpha_ref = truth.alpha_ref;
  q.have |= truth.have & (kQhBlend | kQhAlphaTest);
  CarryDeviceState(truth, q);   // S103 SC: the scissor / target / viewport the stream draw has (the fake memory above holds no state object or recorder history: the live tap fills these from the thread's recorders, proven in sc_unit)
  out = q;
  return true;
}

// The state the stream keeps for this draw against the row's copy: what the live drawer will set from the row. Counted, never fixed up.
inline void CheckState(const Quad2D& row, const rdr_adapter::DrawFixed& fixed) {
  const uint8_t b = BlendClass(fixed.state.blend[0].enable, fixed.state.blend[0].src_color, fixed.state.blend[0].dst_color, fixed.state.blend[0].color_op);
  if ((row.have & kQhBlend) && row.blend != b) ++g_stats.blend_mismatch;
  const uint8_t func = fixed.alpha_test ? fixed.alpha_func : 7;
  if ((row.have & kQhAlphaTest) && (row.alpha_func != func || (func != 7 && row.alpha_ref != fixed.alpha_ref))) ++g_stats.alpha_mismatch;
}

// S103 SC: the scissor the row's own draw takes (what the live drawer's ScissorOf gives at 1x): the row's box when its scissor is on, else a box over any target (a row with no scissor is not clipped at all: the proof that the clip comes from the ROW).
inline void ApplyScissor(const Quad2D& row, rdr_adapter::ReplaceOut::OwnDraw& own) {
  own.scissor_set = true;
  if (row.scissor_on && (row.have & kQhScissor)) std::memcpy(own.scissor, row.scissor, sizeof(own.scissor));
  else own.scissor[0] = own.scissor[1] = 0, own.scissor[2] = own.scissor[3] = 16384;
}
inline std::string ReportSC() {
  char line[420];
  const Stats& s = g_stats;
  std::snprintf(line, sizeof(line), "[fview] [rdr][2d] scissor: rows with a scissor %llu, clipping (smaller than 1280x720) %llu; target: frame %llu offscreen %llu; plants applied: %llu; target class by surface role: unknown %llu, by size (no role table) %llu", (unsigned long long)s.sc_rows, (unsigned long long)s.sc_clipping, (unsigned long long)s.tg_frame, (unsigned long long)s.tg_offscreen, (unsigned long long)s.sc_planted, (unsigned long long)s.tg_unknown, (unsigned long long)s.tg_by_size);
  return line;
}

// Truth row -> the blocks the drawer builds from it, through the plants and the record's round trip. Refuse::None: `out` is the draw's constants. `degenerate` is set when a drop plant removed the
// row: the caller draws a quad of no area (nothing appears, in any blend state).
inline Refuse Make(Quad2D& row, const rdr_adapter::DrawFixed& fixed, Blocks& out, bool& degenerate) {   // `row` comes back as the record's round trip made it (the planes are read from it)
  const Faults& f = g_faults;
  degenerate = false;
  if (f.wrong_blend) row.blend = row.blend == kQbAlpha ? kQbAdd : kQbAlpha;
  if (f.lost_constants) row.have &= ~uint32_t(kQhColour);
  if (f.lost_blend) row.blend = kQbUnknown, row.have &= ~uint32_t(kQhBlend);
  if (row.kind == kQkSprite && f.wrong_uv) {
    for (int i = 1; i < 4; ++i)
      for (int c = 3; c < 5; ++c) row.verts[i][c] = row.verts[0][c] + 0.25f * (row.verts[i][c] - row.verts[0][c]);
    ++g_stats.uv_planted;
  }
  if (row.use_proj != 0.0f && (row.have & kQhMatrix)) {   // S103 P3c plants on a matrix-placed row
    if (f.proj_identity) std::memset(row.proj, 0, sizeof(row.proj)), row.proj[0] = row.proj[5] = row.proj[10] = row.proj[15] = 1.0f, ++g_stats.proj_planted;
    if (f.proj_transposed) {
      for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b) std::swap(row.proj[4 * a + b], row.proj[4 * b + a]);
      ++g_stats.proj_planted;
    }
    if (f.c28_ignored) row.use_proj = 0.0f, ++g_stats.proj_planted;
  }
  if ((row.have & kQhScissor) && (f.scissor_ignored || f.scissor_y_flipped || f.scissor_prev_row)) {   // S103 SC plants on the row's scissor
    static int32_t prev[4] = {0, 0, 1280, 720};
    int32_t now[4] = {row.scissor[0], row.scissor[1], row.scissor[2], row.scissor[3]};
    if (f.scissor_y_flipped && row.scissor_on) row.scissor[1] = 720 - row.scissor[1] - row.scissor[3], ++g_stats.sc_planted;
    if (f.scissor_prev_row && row.scissor_on) std::memcpy(row.scissor, prev, sizeof(prev)), ++g_stats.sc_planted;
    if (f.scissor_ignored) std::memset(row.scissor, 0, sizeof(row.scissor)), row.scissor_on = 0, ++g_stats.sc_planted;
    std::memcpy(prev, now, sizeof(prev));
  }
  g_last_row_glyph = row.kind == kQkGlyph;
  const bool strip_site = (row.kind == kQkRect && row.site == kSiteRectGrad) || (row.kind == kQkSprite && row.site == kSiteGlyphM);
  if (strip_site) {
    ++(row.kind == kQkRect ? g_stats.strip_rect_rows : g_stats.strip_m_rows);
    if (f.strip_order && row.prim == kGlyphPrimStrip) row.prim = 0, ++g_stats.strip_planted;   // (plant strip_order: the strip's Z order is taken as a perimeter)
  }
  bool glyph_drop = false;
  if (row.kind == kQkGlyph) {   // S103 I3 plants
    if (f.glyph_uv_shift) {
      const float du = (row.verts[1][3] - row.verts[0][3]) / 3.0f;   // (vertex 1 = the right edge of a strip's first row)
      for (int i = 0; i < 4; ++i) row.verts[i][3] += du;
      ++g_stats.glyph_uv_planted;
    }
    if (f.glyph_lost_colour) row.color[0] = row.color[1] = row.color[2] = row.color[3] = 1.0f, ++g_stats.glyph_colour_planted;
    if (f.glyph_drop && row.seq % 5 == 0) glyph_drop = true;
  }
  if (row.kind == kQkSprite && f.drop_hsl && (row.have & kQhHsl)) row.have &= ~uint32_t(kQhHsl), std::memset(row.hsl, 0, sizeof(row.hsl)), ++g_stats.hsl_dropped;
  if (f.swap_vertices && row.kind == kQkRect) {
    for (int c = 0; c < 5; ++c) std::swap(row.verts[1][c], row.verts[2][c]);
  }
  if (g_roundtrip) {
    std::string err;
    if (!g_roundtrip(row, err)) return ++g_stats.roundtrip_refused, Refuse::Layout;
  }
  CheckState(row, fixed);
  const Refuse r = BlocksOf(row, out);
  if (r != Refuse::None) return ++g_stats.refused[uint8_t(r)], r;
  {   // the drawer cannot draw an unknown blend (Append's rule): the stream's draw stays
    rdr::BlendState bs;
    if (!(row.have & kQhBlend) || !BlendOf(row.blend, bs)) return ++g_stats.refused[uint8_t(Refuse::Blend)], ++g_stats.blend_unknown, Refuse::Blend;
  }
  if (row.kind == kQkSprite && ((row.have & kQhHsl) != 0) != (out.program == Program::SpriteHsl)) ++g_stats.hsl_state_bad;
  if (glyph_drop) {   // S103 I3: a degenerate quad (the corners collapse onto corner 0)
    ++g_stats.glyph_dropped;
    degenerate = true;
    for (int i = 1; i < 4; ++i)
      for (int c = 0; c < 3; ++c) out.vs[4 + 4 * i + c] = out.vs[4 + c];
  }
  if ((row.kind == kQkRect && f.drop_rect) || (row.kind == kQkYuvAlpha && f.drop_movie)) {
    ++g_stats.degenerate;
    degenerate = true;
    const int base = 4;   // the rect block: Pos[i] at vs[4 + 4 i]; the movie block holds the rectangle itself (x0 y0 x1 y1 at vs[4..7])
    if (row.kind == kQkRect) {
      for (int i = 1; i < 4; ++i)
        for (int c = 0; c < 3; ++c) out.vs[base + 4 * i + c] = out.vs[base + c];
    } else {
      out.vs[6] = out.vs[4], out.vs[7] = out.vs[5];   // x1 = x0, y1 = y0
    }
  }
  if (row.use_proj != 0.0f && (row.have & kQhMatrix)) ++g_stats.proj_drawn[row.kind];   // (S103 P3c)
  if (strip_site) ++g_stats.strip_drawn;   // (S103 P3b: also counted in the kind's own drawn counter below)
  if (row.kind == kQkGlyph) ++g_stats.glyph_drawn;   // (S103 I3)
  else if (row.kind == kQkSprite) ++g_stats.sprites_drawn;   // (S103 I2: the I1 line's `drawn` stays rect + movie)
  else ++g_stats.drawn;
  return Refuse::None;
}

}  // namespace rdr_scene::q2d::rig
