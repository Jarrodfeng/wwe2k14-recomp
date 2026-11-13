// The game's text drawn by us, in place inside the game's frame (Session 101, lane 89 = UT, P2b; docs\renderer\UI_TEXT_OURS.md, the design:
// docs\renderer\UI_PROMOTION_MAP.md sections 6 and 7; the seam: src\renderer_adapter\draw_replace_hook.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// UiTextOurs is a DrawReplaceHook ("ui_text") for the glyph draws (class Text, op 19 at 0x82BC3784 / 0x82BC8BEC). For each text run of the frame (the run string the
// capture / live ring carries per glyph draw, names.h section 69) it lays the run out with OUR pen rule from the font tables of the player's fontHD.pac:
//     quad width   = w x scale x hscale
//     next left    = this left + (w - trim - kern(this, next)) x scale x hscale     (trim 2 / 2 / 7 for font000 / 001 / 002)
//   the run's parameters are read from the run's FIRST glyph draw of each segment: the pen start (its left), the scale (its height / 40) and the horizontal factor
//   hscale (its width / (w x scale); 1 for almost every quad, 0.928 / 0.95 for the narrower ones [C: measured on the editor zip, docs/formats/ui_font.md; no rule says which
//   quads, so it is taken from the draw and counted]). A segment is a run of units between icon glyphs (the icon sheet's quads are not laid out: they stay the stream's).
// and replaces the glyph draws' vertex bytes (x of the four corners and the uv rect) through ReplaceOut::inline_vertices; y, z, layout, state, texture and order stay the
// stream's. EVERY quad of a segment must agree with the stream's within 0.005 px (and its uv rect with the record's) or the whole segment stays the stream's (counted by
// reason): the layout is checked against the game's own draw before it is used, so a wrong pen rule keeps the game's text and shows in the census.
//
// The glyph TEXTURE is not touched: an accepted glyph draw is not offered to the UI textures' hook (ui_ours), a refused one is (KeepWhy::NotOurs).
//
// No game or SDK includes. GPU-free: the unit test (tools\ui\ui_text_test.cpp) links this file. Threads: SetGameRoot from any thread; every other call on the render
// thread; the fonts load on a worker (live) or on the calling thread (fview / rplay / the test).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../renderer_adapter/draw_replace_hook.h"
#include "ui_assets.h"

namespace rdr_scene {

// Planted faults (--replace-fault NAME; `unchecked_NAME` = the same fault with the per-segment guard OFF, so the planted layout is drawn and the picture moves).
struct UiTextFaults {
  bool advance_c2 = false;   // R, a real past bug: the pen's trim 2 for every font (font002's is 7), the first pen rule of the map
  bool kern_off = false;     // kerning ignored
  bool wrong_font = false;   // the next font's tables (metrics) for the run
  bool run_shift = false;    // every quad 0.5 px to the right of the layout
  bool unchecked = false;    // the guard counts a disagreement but accepts the segment
  static bool Parse(const std::string& name, UiTextFaults& out);
};

// ---- the layout (pure functions, no allocation beyond the outputs; the unit test calls the same ones) ------------------------------------------------------------
// One drawn unit of a run: the code that is drawn (after the game's mapping) and whether it is an icon glyph (>= 0xE000: the icon sheet, not laid out).
struct UiTextUnit {
  uint32_t code = 0;
  bool icon = false;
};
// UTF-8 -> code points (invalid bytes become U+FFFD; a 4-byte sequence is one code point). Returns false for an empty string.
bool UiTextDecode(const std::string& utf8, std::vector<uint32_t>& out);
// sub_82BD06B8's per-unit rules [C: UI_PROMOTION_MAP.md 1.3]: units < 0x20 and 0x7F draw nothing, U+2019 is drawn as U+0027, a code the font lacks is drawn as '*'
// (nothing if '*' is missing too), codes >= 0xE000 are icon glyphs.
void UiTextUnitsOf(const UiFont& font, const std::vector<uint32_t>& code_points, std::vector<UiTextUnit>& out);

struct UiTextQuad {
  float x0 = 0, x1 = 0;   // left and right edge (the stream's units: the game's 1280 x 720 root)
};
// Lays out units [first, first + count) (no icons among them) from the pen start `left` at `scale` (= the quad height / the font's cell) and `hscale`. The pen rule above.
// `metrics` supplies the glyph widths, the trim and the kerning pairs.
void UiTextLayout(const UiFont& metrics, const std::vector<UiTextUnit>& units, size_t first, size_t count, float left, float scale, float hscale, const UiTextFaults& faults,
                  std::vector<UiTextQuad>& out);

// ---- the hook ------------------------------------------------------------------------------------------------------------------------------------------------
struct UiTextOptions {
  std::string game_root;
  UiTextFaults faults;
  std::function<void(const std::string&)> log;   // where a font load failure is reported once (the factory sets rdr::Log; null: nowhere)
  bool live = false;   // true: the fonts load on a worker and frames before they are ready keep the stream's text; false: on the calling thread
};

// Counters (per hook; the report prints them). Glyph draws are counted once each; runs once each.
struct UiTextCensus {
  uint64_t frames = 0, not_ready_frames = 0;
  uint64_t old_format_frames = 0;      // frames without kFrameSixteenPairsHighFirst (captures from before the reader's pair fix): their vertex bytes are not the layout this hook writes; the stream's text stays
  uint64_t claimed = 0;                // glyph draws offered to Replace
  uint64_t ours = 0;                   // glyph draws replaced
  uint64_t kept_icon = 0;              // an icon glyph (icon sheet): the stream's quad, offered on to the UI textures
  uint64_t kept_norun = 0;             // no run string for the draw (labels off, an unnamed run, over the table's cap)
  uint64_t kept_truncated = 0;         // the run string was cut at 512 units
  uint64_t kept_count = 0;             // the group's draws are not a whole number of the run's units
  uint64_t kept_font = 0;              // no font's glyph records agree with the stream's uv rects
  uint64_t kept_geometry = 0;          // not 4 axis-aligned vertices / the geometry could not be read
  uint64_t kept_layout = 0;            // the segment's layout disagrees with the stream's quads by more than 0.05 px
  uint64_t segments = 0, narrow_segments = 0, runs = 0, runs_ours = 0, icon_runs = 0;
  uint64_t fonts_ambiguous = 0;        // more than one font agreed with the uv rects (the first is used)
  double max_dx = 0;                   // the largest quad edge difference of an accepted segment (guard: 0.05)
  double ms = 0;
};

class UiTextOurs final : public rdr_adapter::DrawReplaceHook {
 public:
  explicit UiTextOurs(const UiTextOptions& options);
  ~UiTextOurs() override;
  UiTextOurs(const UiTextOurs&) = delete;
  UiTextOurs& operator=(const UiTextOurs&) = delete;

  void SetGameRoot(const std::string& root);

  // ---- DrawReplaceHook ----
  const char* Name() const override { return "ui_text"; }
  bool Claims(const rdr_adapter::DrawOrigin& origin) const override;
  bool FrameReady(const rdr_adapter::LoadedFrame& frame) override;
  bool Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why) override;

  const UiTextCensus& census() const { return census_; }
  std::string Report() const;
  std::string error() const;   // fonts that could not be read (a copy, under the lock: the worker sets it)

  // The glyph call sites (names.h / UI_PROMOTION_MAP.md 1.1) the hook claims.
  static constexpr uint32_t kGlyphSite = 0x82BC3784u, kGlyphSiteM = 0x82BC8BECu;

 private:
  enum class Fate : uint8_t { Ours, Icon, NoRun, Truncated, Count, Font, Geometry, Layout };
  struct Group {
    uint32_t first_draw = 0, draws = 0;   // consecutive glyph draws of one run (same string, same object)
    uint32_t string = 0;                  // the TextRun's string name
    uint32_t flags = 0;
    bool decided = false;
    uint32_t plan = 0;                    // offset of the first draw's entry in plan_ (one entry per draw of the group)
  };
  struct Plan {
    Fate fate = Fate::NoRun;
    uint32_t vertex_at = 0;               // offset in vertices_ (ours only)
  };
  void LoadFonts();
  void Decide(const rdr_adapter::ReplaceIn& in, Group& group);
  void SetFate(Group& group, uint32_t from, uint32_t count, Fate fate);

  UiTextOptions options_;
  UiTextCensus census_;
  mutable std::mutex mu_;                 // guards root_, error_ and the loaded flag
  std::string root_, error_;
  UiFont fonts_[3];
  std::atomic<bool> loaded_{false}, load_started_{false}, load_failed_{false};
  std::thread load_thread_;
  // per frame
  std::vector<Group> groups_;
  std::vector<int32_t> group_of_draw_;    // draw index -> group, -1
  std::vector<Plan> plan_;
  std::vector<uint8_t> vertices_;         // the replacement vertex bytes of the frame's accepted draws
  // scratch
  std::vector<uint32_t> cps_;
  std::vector<UiTextUnit> units_;
  std::vector<UiTextQuad> quads_;
  std::vector<uint8_t> raw_, stream_bytes_;
};

}  // namespace rdr_scene
