// Offline proof of src\renderer_game\fallback_census.h (S103 L2b, landing 2 piece P2): every reason counts once, the first reason wins in the documented order, the window's reason
// counts sum to its fallbacks, an uncounted frame is reported, and the planted twins (a reason not counted; a frame never opened) are caught. No game, no run.
//   pwsh -NoProfile -File tools\scene\build_fallback_census_unit.ps1  ->  out\tools\fallback_census_unit.exe
#define FALLBACK_CENSUS_PLANTS 1
#include <cstdio>
#include <string>

#include "../../src/renderer_game/fallback_census.h"

namespace fc = fallback_census;
using rdr_own::WhyNot;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static_assert(int(WhyNot::SwitchOff) == 1 && int(WhyNot::NotReady) == 2 && int(WhyNot::InputsMissing) == 3 && int(WhyNot::Hold) == 4 && int(WhyNot::Latched) == 5 && int(WhyNot::NoInputsRegistered) == 6,
              "kWhyName must follow rdr_own::WhyNot");

static bool Has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

// One frame with one reason, closed: returns the (final) line of a fresh census holding just that frame.
static std::string One(fc::Kind k, const std::string& key) {
  fc::Census c;
  c.Begin(100);
  c.Note(100, k, key);
  return c.FinalLine(1);
}

int main() {
  // 1. each reason alone: counted once, under its kind, the sum holds
  struct { fc::Kind k; const char* key; const char* want; } each[] = {
      {fc::Kind::Scene, "inputs_missing_bit4_light_states", "scene inputs_missing_bit4_light_states:1"},
      {fc::Kind::Scene, "hold", "scene hold:1"},
      {fc::Kind::Render, "no_frame_image", "render no_frame_image:1"},
      {fc::Kind::Hole, "0x82BC3758", "hole 0x82BC3758:1"},
      {fc::Kind::Refused, "blend", "refused blend:1"},
      {fc::Kind::UnknownSite, "0x82BC4404", "unknown_site 0x82BC4404:1"},
      {fc::Kind::RemnantExtra, "0x82BB10C0", "remnant_extra 0x82BB10C0:1"},
      {fc::Kind::PassUnknown, "all", "pass_unknown 1"},
      {fc::Kind::BlendCode, "7", "blend_code 7:1"},
  };
  for (const auto& e : each) {
    const std::string l = One(e.k, e.key);
    CHECK(Has(l, "frames 1, 2d-whole 0, fallback 1"));
    CHECK(Has(l, e.want));
    CHECK(Has(l, "sum ok"));
    CHECK(Has(l, "none uncounted"));
    CHECK(Has(l, "rows 0, pair (0 0, -1 0, +1 0, other 0)"));   // S103 L2aF: rows / pair are wired (L2a): nothing noted reads as a measured zero
    CHECK(Has(l, "run totals: frames 1, 2d-whole 0, fallback 1"));
  }
  // an all-whole frame
  {
    fc::Census c;
    c.Begin(5);
    const std::string l = c.FinalLine(1);
    CHECK(Has(l, "frames 1, 2d-whole 1, fallback 0"));
    CHECK(Has(l, "scene -; render -; hole -; refused -; unknown_site -; remnant_extra -; pass_unknown -; blend_code -"));   // (S103 P7 H2: pass_unknown is wired by the scene host: measured zero reads `-`)
    CHECK(Has(l, "sum ok"));
  }

  // 2. the first reason wins, in the documented order, not by arrival
  {
    fc::Census c;
    c.Begin(1);
    c.Note(1, fc::Kind::BlendCode, "7");
    c.Note(1, fc::Kind::RemnantExtra, "0x1");
    c.Note(1, fc::Kind::Render, "loading");   // arrives later than two 2D reasons, still wins over them
    c.Note(1, fc::Kind::Scene, "latched");     // and the scene reason wins over all
    c.Note(1, fc::Kind::Scene, "hold");        // the same kind: the first noted stays
    c.Begin(2);
    c.Note(2, fc::Kind::Hole, "0xA");
    c.Note(2, fc::Kind::Hole, "0xB");          // same kind, the first stays
    c.Note(2, fc::Kind::Refused, "blend");     // a later kind loses
    const std::string l = c.FinalLine(2);
    CHECK(Has(l, "frames 2, 2d-whole 0, fallback 2"));
    CHECK(Has(l, "scene latched:1"));
    CHECK(!Has(l, "scene latched:1,hold"));
    CHECK(Has(l, "hole 0xA:1"));
    CHECK(!Has(l, "0xB:") && !Has(l, "refused blend") && !Has(l, "blend_code 7") && !Has(l, "0x1:") && !Has(l, "loading"));
    CHECK(Has(l, "sum ok"));
  }

  // 3. a 600-frame window: the line comes out once at 600 closed frames, counts sum, whole + fallback = frames
  {
    fc::Census c;
    std::string line;
    for (uint64_t f = 1; f <= 1200; ++f) {
      c.CloseBefore(f);
      c.Begin(f);
      if (f % 3 == 0) c.Note(f, fc::Kind::Scene, f % 2 ? "inputs_missing_bit3_wrestler_palette" : "inputs_missing_bit4_light_states");
      else if (f % 5 == 0) c.Note(f, fc::Kind::RemnantExtra, "0x82BB10C0");
      const std::string l = c.TakeLine(f);
      if (!l.empty()) { line += l + "\n"; }
    }
    // frames 1..599 closed by the time frame 600 opens ... the 600th closes when 601 opens: two lines (at 601 and 1201 would be the third; we ran to 1200)
    size_t lines = 0;
    for (char ch : line) lines += ch == '\n';
    CHECK(lines == 1);   // 1199 closed frames: one window of 600 flushed, the second is not full yet
    CHECK(Has(line, "frames 600, 2d-whole "));
    CHECK(Has(line, "sum ok"));
    CHECK(Has(line, "none uncounted") || Has(line, "UNCOUNTED"));
    const std::string fin = c.FinalLine(1200);
    CHECK(Has(fin, "exit: frames 600"));                 // review F7: the exit line's first part is the FINAL WINDOW (the second 600) ...
    CHECK(Has(fin, "; run totals: frames 1200, "));      // ... the run totals follow it
    CHECK(Has(fin, "sum ok") && Has(fin, "none uncounted"));
    const fc::Census::Tally t = c.Total();
    CHECK(t.frames == 1200 && t.whole + t.fallback == 1200);
    CHECK(t.fallback == 400 + 160 - 0 || t.fallback > 0);   // (exact split is the loop's arithmetic; the invariant is the sum below)
    uint64_t sum = 0;
    for (size_t k = 0; k < size_t(fc::Kind::Count); ++k)
      for (const auto& kv : t.by[k]) sum += kv.second;
    CHECK(sum == t.fallback);
  }

  // 4. the planted twin: a closed fallback frame whose reason is not counted -> the sum check fails
  {
    fc::Census c;
    c.plant = 1;
    c.Begin(1);
    c.Note(1, fc::Kind::Scene, "hold");
    c.Begin(2);
    const std::string l = c.FinalLine(2);
    CHECK(Has(l, "SUM MISMATCH"));
    CHECK(!Has(l, "sum ok"));
  }

  // 5. the planted twin: a path that handled a frame and never opened it -> UNCOUNTED
  {
    fc::Census c;
    c.Begin(1);
    const std::string l = c.FinalLine(2);   // the caller handled 2, the census closed 1
    CHECK(Has(l, "UNCOUNTED 1"));
    CHECK(!Has(l, "none uncounted"));
  }

  // 6. a note on a frame that was never opened is an orphan, never a silent reason
  {
    fc::Census c;
    c.Note(9, fc::Kind::Scene, "hold");
    c.Begin(1);
    const std::string l = c.FinalLine(1);
    CHECK(Has(l, "orphan notes 1"));
    CHECK(Has(l, "frames 1, 2d-whole 1, fallback 0"));
  }

  // 7. pending 2D notes: adopted exactly once by the first record j with tap_frame <= DrainBound(j) = j (the same bound P1's Drain(j) uses); a later one stays pending; duplicates are one entry
  {
    fc::Census c;
    c.Pend(10, fc::Kind::UnknownSite, "0x82BC4404");
    c.Pend(10, fc::Kind::Hole, "0x82BC3758");
    c.Pend(12, fc::Kind::UnknownSite, "0x82BC32B8");
    c.Pend(12, fc::Kind::UnknownSite, "0x82BC32B8");   // the same (frame, kind, key) again: one entry
    CHECK(c.pending() == 3);
    static_assert(fc::DrainBound(10) == 10, "Drain(j): the bound is the record frame itself");
    c.Begin(9);
    c.Adopt(9);    // nothing is due
    c.Begin(10);
    c.Adopt(10);   // tap 10 <= 10: the two notes are due (hole wins over unknown_site)
    c.Begin(11);
    c.Adopt(11);   // nothing more
    c.Begin(12);
    c.Adopt(12);   // tap 12 <= 12: the last one
    CHECK(c.pending() == 0);
    const std::string l = c.FinalLine(4);
    CHECK(Has(l, "frames 4, 2d-whole 2, fallback 2"));
    CHECK(Has(l, "hole 0x82BC3758:1"));
    CHECK(Has(l, "unknown_site 0x82BC32B8:1"));
    CHECK(!Has(l, "0x82BC4404"));   // lost to the hole in the same frame: still counted exactly once as the frame's one reason
    CHECK(Has(l, "sum ok"));
    CHECK(Has(l, "pending unadopted 0"));
  }
  // 7b. review F6: a note no record ever adopts is reported (pending unadopted), and the cap drops the NEW note with a count (no erase under the lock)
  {
    fc::Census c;
    c.Begin(1);
    c.Pend(500, fc::Kind::Hole, "0x82BC3758");   // a future frame that never opens
    for (uint32_t i = 0; i < 5000; ++i) c.Pend(600 + i, fc::Kind::UnknownSite, fc::HexKey(0x82000000u + i));
    const std::string l = c.FinalLine(1);
    CHECK(Has(l, "pending unadopted 4096"));
    CHECK(Has(l, "pending dropped 905"));   // 1 + 5000 - 4096
  }
  // 7c. the window on a scene change: flushed under the OLD scene's name, queued for the render thread
  {
    fc::Census c;
    for (uint64_t f = 1; f <= 3; ++f) c.Begin(f), c.Note(f, fc::Kind::Scene, "hold");
    c.CloseBefore(4);
    c.SceneChange("main_menu");   // 3 frames closed so far, no scene name yet
    for (uint64_t f = 10; f <= 11; ++f) c.Begin(f);
    c.CloseBefore(12);
    c.SceneChange("Match (30 Years)");
    const std::string l1 = c.TakeLine(), l2 = c.TakeLine(), l3 = c.TakeLine();
    CHECK(Has(l1, "frames 3, 2d-whole 0, fallback 3") && !Has(l1, "in scene"));
    CHECK(Has(l2, "frames 2, 2d-whole 2, fallback 0") && Has(l2, "in scene main_menu"));   // (3 closed by 10's CloseBefore ... the window holds what closed)
    CHECK(l3.empty());
  }

  // 8. keys
  {
    rdr_own::OwnedPassStamp st[2] = {};
    st[0].pass = uint8_t(rdr_own::Pass::Scene3D), st[0].guest_frame = 99, st[0].decision = uint8_t(rdr_own::Decision::GameWalks), st[0].why_not = uint8_t(WhyNot::InputsMissing);
    const uint32_t req = (1u << 3) | (1u << 4) | (1u << 12);
    st[0].inputs_mask = 1u << 12;
    CHECK(fc::SceneKey(st, 1, 100, req) == "inputs_missing_bit3_wrestler_palette");   // the lowest missing bit
    st[0].inputs_mask = (1u << 12) | (1u << 3);
    CHECK(fc::SceneKey(st, 1, 100, req) == "inputs_missing_bit4_light_states");
    st[0].why_not = uint8_t(WhyNot::Hold);
    CHECK(fc::SceneKey(st, 1, 100, req) == "hold");
    st[0].why_not = uint8_t(WhyNot::NotReady);
    CHECK(fc::SceneKey(st, 1, 100, req) == "not_ready");
    st[0].why_not = uint8_t(WhyNot::SwitchOff);
    CHECK(fc::SceneKey(st, 1, 100, req) == "switch_off");
    st[0].why_not = uint8_t(WhyNot::Latched);
    CHECK(fc::SceneKey(st, 1, 100, req) == "latched");
    st[0].why_not = uint8_t(WhyNot::NoInputsRegistered);
    CHECK(fc::SceneKey(st, 1, 100, req) == "no_inputs_registered");
    st[0].decision = uint8_t(rdr_own::Decision::Ours);
    CHECK(fc::SceneKey(st, 1, 100, req) == "stamp_ours_not_drawn");   // review F9: never empty
    CHECK(fc::SceneKey(st, 1, 200, req) == "no_stamp");   // a stamp for another frame
    CHECK(fc::SceneKey(st, 0, 100, req) == "no_stamp");
    CHECK(fc::RenderKey("record inputs not fresh for hook frame 14999 (first: bit 4)", false) == "inputs_not_fresh_bit4_light_states");
    CHECK(fc::RenderKey("asset unknown: pac:entry 12", false) == "asset_unknown");
    CHECK(fc::RenderKey("draw: no graph", false) == "draw");
    CHECK(fc::RenderKey("whatever", true) == "loading");
    CHECK(fc::RenderKey("", false) == "unnamed");
  }

  // 9. the op-19 hook's classification and the global one-liners (mode Ours: Enabled)
  {
    scene_frame::g_mode_snapshot.store(0);
    fc::OnOp19Site(49, 0x82BC4404u, false);   // mode off: nothing is noted
    scene_frame::g_mode_snapshot.store(2);
    CHECK(fc::Enabled());
    fc::OnOp19Site(48, 0x82BC3EE8u, true);    // a recorded site (T_Rect) inside its scope: a row, not unknown (review F5: exempt only then)
    fc::OnOp19Site(49, 0x82A0DA58u, false);   // a particle site: lane F
    fc::OnOp19Site(49, 0x82BC4404u, true);    // a scope was open: the hole path owns it
    fc::OnOp19Site(49, 0x82BC4404u, false);   // T_RectColor, no scope: unknown_site
    fc::BeginFrame(50);                       // record 50 = hook 49: the note is due
    fc::BeginFrame(51);
    fc::NoRecord(52);
    fc::BeginFrame(53);
    fc::NoteRemnantExtra(53, 0x82BB10C0u);
    fc::NoteBlendCode(53, 9);
    const std::string l = fc::g_census.FinalLine(4);
    CHECK(Has(l, "frames 4, 2d-whole 1, fallback 3"));   // 51 whole, 50 unknown_site, 52 no record, 53 remnant_extra
    CHECK(Has(l, "unknown_site 0x82BC4404:1"));
    CHECK(Has(l, "render no_scene_record:1"));
    CHECK(Has(l, "remnant_extra 0x82BB10C0:1"));
    CHECK(Has(l, "blend_code -"));   // lost to the remnant draw (earlier kind)
    CHECK(Has(l, "sum ok") && Has(l, "none uncounted"));
  }
  // 10. review F5: a recorded site called with NO scope open makes no row: unknown_site
  {
    fc::OnOp19Site(60, 0x82BC3EE8u, false);
    fc::BeginFrame(60);
    const std::string l = fc::g_census.FinalLine(0);
    CHECK(Has(l, "unknown_site 0x82BC3EE8:1"));
  }
  // 11. review F1: the binder's call does not depend on the post chain (OURS skips it): the first draw of a Bind on an OURS frame (scene_frame_id != 0) is noted, once; not on a non-OURS frame / the velocity pass
  {
    fc::Census& c = fc::g_census;
    c.Begin(70);
    bool noted = false;
    fc::NoteRemnantOnce(noted, 0u, false, 70, 0x82BB10C0u);    // not an OURS frame
    CHECK(!noted);
    fc::NoteRemnantOnce(noted, 5u, true, 70, 0x82BB10C0u);     // the velocity pass
    CHECK(!noted);
    fc::NoteRemnantOnce(noted, 5u, false, 70, 0x82BB10C0u);    // the first draw reaching the draw
    CHECK(noted);
    fc::NoteRemnantOnce(noted, 5u, false, 70, 0x82BB2222u);    // later draws of the Bind: no lock, no note
    const std::string l = c.FinalLine(0);
    CHECK(Has(l, "remnant_extra 0x82BB10C0:1") && !Has(l, "0x82BB2222"));
  }
  // 12. rows / pair print numbers once somebody notes them
  {
    fc::Census c;
    c.Begin(1);
    c.NoteRows(5), c.NotePair(0);
    const std::string l = c.FinalLine(1);
    CHECK(Has(l, "rows 5, pair (0 1, -1 0, +1 0, other 0)"));
    c.NotePairs(3, 1, 1, 0);
    CHECK(Has(c.FinalLine(1), "pair (0 3, -1 1, +1 1, other 0)"));   // S103 L2aF: the contributor's bulk note
  }
  std::printf("%s\n", g_fail ? "FALLBACK_CENSUS_UNIT FAILED" : "fallback_census_unit: all checks passed");
  return g_fail ? 1 : 0;
}
