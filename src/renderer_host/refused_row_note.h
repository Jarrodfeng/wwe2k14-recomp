// S105 A4 piece 3 (docs\research\s105\A4.md "Not settled"): a log-only note naming the FIRST refused / dropped 2D row of a frame, once per mode entry, and any full-screen opaque 2D row before it.
// Answers, in the main session's one check run: which row is refused (site, pass, kind, target, rect, blend, row index) and whether an opaque full-screen row lies under it (why Create a Finisher's preview half
// is black). Pure functions over the record's rows: no renderer, no game. Called by SceneHost::DrawRows (scene_host.cpp) when a frame dropped a row; the host logs the line with `Log`.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "../renderer_scene/scene_record.h"

namespace wwe2k14::scene_host::refused_row {

#ifdef RDR_REFUSED_ROW_PLANTS
// Test only (tools\scene\refused_row_note_unit.cpp --plant N): 1 = the gate never re-arms, 2 = the gate notes every dropped frame, 3 = no row counts as full-screen opaque, 4 = ServeDecision serves any pass, 5 = ServeDecision serves a redundant opaque row.
inline int g_plant = 0;
#define RR_PLANT(n) (::wwe2k14::scene_host::refused_row::g_plant == (n))
#else
#define RR_PLANT(n) false
#endif

// Once per mode entry AND per distinct (site, why) (review F8: the first dropped row may not be the picture row): Note() says yes the first time a (site, why) pair is seen since the last quiet spell;
// Frame(dropped) is called once per frame with rows, and kQuietFrames consecutive frames with no dropped row forget what was noted (a menu / the next mode between).
struct Gate {
  static constexpr uint32_t kQuietFrames = 30;
  static constexpr uint32_t kMaxKeys = 12;
  uint32_t quiet = 0, n = 0;
  uint64_t seen[kMaxKeys] = {};
  void Frame(bool dropped) {
    if (dropped) {
      quiet = 0;
      return;
    }
    if (++quiet >= kQuietFrames && !RR_PLANT(1)) n = 0;
  }
  bool Note(uint32_t site, const char* why) {
    if (RR_PLANT(2)) return true;
    uint64_t h = 1469598103934665603ull;   // FNV-1a over the reason text
    for (const char* p = why ? why : "?"; *p; ++p) h = (h ^ uint8_t(*p)) * 1099511628211ull;
    const uint64_t key = h ^ (uint64_t(site) << 17);
    for (uint32_t i = 0; i < n; ++i)
      if (seen[i] == key) return false;
    if (n >= kMaxKeys) return false;   // (a flood of distinct pairs: the first twelve are enough)
    seen[n++] = key;
    return true;
  }
};

// The row's rect in the guest's 1280x720 pixels: the corner box of its vertices (x y of verts[0..3]).
struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
inline Rect RectOf(const rdr_scene::Quad2D& r) {
  Rect b{r.verts[0][0], r.verts[0][1], r.verts[0][0], r.verts[0][1]};
  for (int i = 1; i < 4; ++i) {
    if (r.verts[i][0] < b.x0) b.x0 = r.verts[i][0];
    if (r.verts[i][1] < b.y0) b.y0 = r.verts[i][1];
    if (r.verts[i][0] > b.x1) b.x1 = r.verts[i][0];
    if (r.verts[i][1] > b.y1) b.y1 = r.verts[i][1];
  }
  return b;
}
// A row that covers the whole 1280x720 frame (>= 98 % each way) with the blend off = it hides what is under it.
inline bool FullScreenOpaque(const rdr_scene::Quad2D& r) {
  const Rect b = RectOf(r);
  return !RR_PLANT(3) && r.blend == rdr_scene::kQbOff && (b.x1 - b.x0) >= 1280.f * 0.98f && (b.y1 - b.y0) >= 720.f * 0.98f;
}

// S105 A4 review F6: may the scene-copy sprite row `row` be answered with our own 3D picture? Only a FINAL-pass row (a row on F: the sprite of path B, after the scene is resolved to the frame); a scene-colour / offscreen /
// unknown-pass row is never served (an in-scene row is drawn into `out` itself; an unknown pass may be a page the game drew offscreen). A full-screen OPAQUE sprite with no FINAL row drawn before it in the record only puts
// the scene back on a frame that already holds it (F holds `out`): skipped and counted, not a redundant full-screen quad. `have_picture` = the host presents a 3D picture this frame.
enum class Serve : uint8_t { Yes, NotFinal, NoPicture, Redundant };
inline Serve ServeDecision(const rdr_scene::Quad2D* rows, size_t n, const rdr_scene::Quad2D& row, bool have_picture) {
  if (row.pass != rdr_scene::kQpFinal && !RR_PLANT(4)) return Serve::NotFinal;
  if (!have_picture) return Serve::NoPicture;
  if (FullScreenOpaque(row) && !RR_PLANT(5)) {
    bool final_before = false;
    for (size_t i = 0; i < n && !final_before; ++i)
      final_before = rows[i].seq < row.seq && rows[i].pass == rdr_scene::kQpFinal;
    if (!final_before) return Serve::Redundant;
  }
  return Serve::Yes;
}

// The note: `rows[idx]` is the first refused / dropped row (`why` = the drawer's reason name, e.g. "picture" or "pass_offscreen"). Rows before it that are full-screen and opaque are listed by index with their site and pass.
inline std::string Line(const rdr_scene::Quad2D* rows, size_t n, size_t idx, const char* why, uint64_t frame) {
  if (!rows || idx >= n) return {};
  const rdr_scene::Quad2D& r = rows[idx];
  const Rect b = RectOf(r);
  char head[420];
  std::snprintf(head, sizeof head,
                "[host][2d] first refused row at record %llu (once per mode entry): index %zu of %zu, why %s, site 0x%08X, pass %u, kind %u, target %ux%u, rect %.0f,%.0f %.0fx%.0f, blend %u, state 0x%02X, tex_key 0x%08X",
                (unsigned long long)frame, idx, n, why ? why : "?", r.site, unsigned(r.pass), unsigned(r.kind), r.target_size[0], r.target_size[1], b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, unsigned(r.blend), unsigned(r.state), r.tex_key[0]);
  std::string s = head;
  std::string under;
  size_t listed = 0;
  for (size_t i = 0; i < idx; ++i) {
    if (!FullScreenOpaque(rows[i])) continue;
    if (++listed > 4) continue;   // (a count says the rest)
    char e[80];
    std::snprintf(e, sizeof e, "%s%zu (site 0x%08X pass %u)", under.empty() ? "" : ", ", i, rows[i].site, unsigned(rows[i].pass));
    under += e;
  }
  s += listed ? "; full-screen opaque rows before it: " + std::to_string(listed) + " [" + under + "]" : "; no full-screen opaque row before it";
  return s;
}

}  // namespace wwe2k14::scene_host::refused_row
