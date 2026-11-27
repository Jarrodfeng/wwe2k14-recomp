// body-scale: none (checks the scene's clear colour, builds no wrestler picture)  [S103 RGF: body_scale_feed_lint.py]
// clrb_unit (S103 CLRB, docs\agents\s103\CLR.md 3): a LIVE record clears the scene to the GAME'S colour ([[0x83929A68] + 240 / 244 / 248 / 252] = R, G, B, A, 0..255), black when the game gives none; a VIEWER record keeps the slate.
//   clrb_unit [--plant NAME]
// Production order, the real code at every step: a fake big-endian guest (the clear block pointer at 0x83929A68, the four words) -> the LATCH contributor (InputsContributor::Latch reads them in Take) -> Emit into a SceneTables ->
// the RECORD (SerializeRecord / ParseRecord: what the stream-free build writes and reads) -> the clear pick record_frame.cpp BuildFrame calls (clear_pick::Pick over the PARSED record's stage_look and header.source).
// The oracle is hand-written from the listing (sub_82C224A0 0x82C224F0-0x82C225FC: R = +240, G = +244, B = +248, A = +252, each & 0xFF; sub_825C7F48 black, sub_8233E678 arena 77 = white), never from the code's tables.
// BuildFrame itself is not linked (it pulls the model cache); the unit asserts record_frame.cpp's call text instead (clrb.build_frame_calls_pick). Exit 1 on a failure.
// A plant is a fault in the thing under test; each must make a NAMED check fail:
//   slate_live    a live record keeps the viewer's slate (the old behaviour, record_frame.cpp)  -> "clrb.no_row_is_black"
//   row_ignored   the pick drops the row (always black)                                              -> "clrb.white_is_white"
//   word_shift    the latch reads +244 .. +256 (one word late)                                       -> "clrb.white_is_white"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/clear_pick.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_stage_look.h"
#include "../../src/scene_live/inputs_contrib.h"

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace si = scene_inputs;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = si::Swap32(it->second);
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g;
static bool ReadThunk(uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }
static int g_mode = 2;

static constexpr uint32_t kBlockPtrAddr = 0x83929A68u, kBlock = 0x40500000u;   // the object (a made-up heap address); the pointer is the game's global

static void BuildGuest(bool pointer, uint32_t r, uint32_t gr, uint32_t b, uint32_t a) {
  g = Guest{};
  g.Put(0x8376383Cu, 850);   // the card factor word (a neighbouring row on the same latch: it must not disturb the clear row)
  if (pointer) g.Put(kBlockPtrAddr, kBlock);
  const uint32_t base = Plant("word_shift") ? kBlock + 244 : kBlock + 240;   // plant: the word offsets are shifted one word
  g.Put(base, r), g.Put(base + 4, gr), g.Put(base + 8, b), g.Put(base + 12, a);
  if (Plant("word_shift")) g.Put(kBlock + 240, 0u);   // so the first word still reads (a shift, not a failed read)
  g.Put(kBlock + 256 + 0, 0u);
}

static void ResetStamps() {
  for (int b = 0; b < 32; ++b) rdr_own::S(rdr_own::Pass::Scene3D).input_frame[b].store(0xFFFFFFFFFFFF0000ull);
  for (int b = 0; b < si::kInputCount; ++b) si::g_counters.stamps[b].store(0), si::g_counters.last[b].store(0);
}

struct Run {
  SceneTables emitted, parsed;
  std::string census;
  bool parse_ok = false;
  size_t clear_rows = 0;
};
// the latch -> Emit -> record -> parse of the current guest, as the game's frame goes; `source` = who wrote the record (live_scene.cpp sets Live)
static Run Go(uint64_t frame, Source source) {
  Run r;
  ResetStamps();
  live::inputs::InputsContributor c;
  c.SetReader(ReadThunk);
  FrameInfo fi;
  fi.guest_frame = frame;
  c.Latch(fi);
  Counters cn;
  c.Emit(fi, r.emitted, cn);
  c.Census(r.census);
  r.emitted.header.source = uint8_t(source);
  std::vector<uint8_t> by = SerializeRecord(r.emitted);
  std::string er;
  r.parse_ok = ParseRecord(by.data(), by.size(), r.parsed, er);
  for (const StageLookRow& row : r.parsed.stage_look) r.clear_rows += row.kind == kSlClearColor;
  return r;
}

// the pick record_frame.cpp makes, over the PARSED record (`on` = the switch)
static void PickOf(const Run& r, bool on, float out[4], bool* fed = nullptr) {
  const bool live = r.parsed.header.source == uint8_t(Source::Live) && !Plant("slate_live");
  std::vector<StageLookRow> rows = r.parsed.stage_look;
  if (Plant("row_ignored")) rows.clear();
  clear_pick::Pick(rows, live, on, out, fed);
}
static bool Is(const float c[4], float r, float gr, float b, float a) {
  return std::fabs(c[0] - r) < 1e-6f && std::fabs(c[1] - gr) < 1e-6f && std::fabs(c[2] - b) < 1e-6f && std::fabs(c[3] - a) < 1e-6f;
}
static std::string Str(const float c[4]) {
  char b[96];
  std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f, %.3f)", c[0], c[1], c[2], c[3]);
  return b;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  scene_frame::g_mode_cvar.store(&g_mode);
  scene_frame::SetReady(true);
  float c[4];
  bool fed = false;
  Check(!game_clear::On(), "clrb.header_default_off", "the header default keeps the slate (the stream build, the rigs, the tools)");
  game_clear::g_on.store(true);   // what ApplyStartSettings does in the game (start_settings.cpp)

  // A. the capture state: (0, 0, 0, 255) -> black
  BuildGuest(true, 0, 0, 0, 255);
  Run a = Go(300, Source::Live);
  Check(a.parse_ok, "clrb.record_roundtrip", "latch -> Emit -> record -> parse");
  Check(a.clear_rows == 1, "clrb.one_row", std::to_string(a.clear_rows) + " kSlClearColor rows");
  PickOf(a, true, c, &fed);
  Check(Is(c, 0, 0, 0, 1) && fed, "clrb.black_row_is_black", Str(c) + (fed ? " fed" : " unfed"));
  Check(a.census.find("[clear_colour read 1, unfed 0, last_rgba 255]") != std::string::npos, "clrb.census_fed", "");

  // B. arena 77 in a story sequence (sub_8233E678): white
  BuildGuest(true, 255, 255, 255, 255);
  Run w = Go(301, Source::Live);
  PickOf(w, true, c, &fed);
  Check(Is(c, 1, 1, 1, 1) && fed, "clrb.white_is_white", Str(c));

  // C. arena 79: green; the four words map R, G, B in order (+240 = R, +244 = G, +248 = B)
  BuildGuest(true, 10, 255, 51, 255);
  Run gr = Go(302, Source::Live);
  PickOf(gr, true, c);
  Check(Is(c, 10.f / 255.f, 1, 51.f / 255.f, 1), "clrb.channel_order", Str(c));

  // D. the game's high bytes are masked (& 0xFF): a stray high byte must not leak
  BuildGuest(true, 0xAB000000u | 255u, 0, 0, 255);
  Run hb = Go(303, Source::Live);
  PickOf(hb, true, c);
  Check(Is(c, 1, 0, 0, 1), "clrb.words_masked", Str(c));

  // E. the menu-exit sequence writes alpha 0 (sub_83328D40): the first build forces alpha 1
  BuildGuest(true, 0, 0, 0, 0);
  Run tr = Go(304, Source::Live);
  PickOf(tr, true, c);
  Check(Is(c, 0, 0, 0, 1), "clrb.alpha_forced_one", Str(c));

  // F. no row on a LIVE record (the pointer unread): black, never the slate; one unfed latch counted
  BuildGuest(false, 255, 255, 255, 255);
  Run nr = Go(305, Source::Live);
  PickOf(nr, true, c, &fed);
  Check(nr.clear_rows == 0 && Is(c, 0, 0, 0, 1) && !fed, "clrb.no_row_is_black", Str(c));
  Check(nr.census.find("[clear_colour read 0, unfed 1,") != std::string::npos, "clrb.census_unfed", "");

  // G. a pointer that reads but a block word that does not: the whole row is refused (never a half-read colour)
  BuildGuest(true, 255, 255, 255, 255);
  g.words.erase(kBlock + 252);
  Run hr = Go(306, Source::Live);
  PickOf(hr, true, c);
  Check(hr.clear_rows == 0 && Is(c, 0, 0, 0, 1), "clrb.half_read_refused", std::to_string(hr.clear_rows) + " rows " + Str(c));

  // H. a VIEWER record keeps the slate, with or without a row
  BuildGuest(true, 255, 255, 255, 255);
  Run vw = Go(307, Source::Viewer);
  PickOf(vw, true, c);
  Check(Is(c, 0.12f, 0.14f, 0.18f, 1.f), "clrb.viewer_keeps_slate", Str(c));

  // I. the switch off: a live record keeps the slate too (the old behaviour)
  PickOf(w, false, c);
  Check(Is(c, 0.12f, 0.14f, 0.18f, 1.f), "clrb.switch_off_slate", Str(c));

  // J. the row's flags bit 0 gates it: a row without the read bit is ignored
  {
    std::vector<StageLookRow> rows(1);
    rows[0].kind = kSlClearColor, rows[0].flags = 0, rows[0].col[0] = 1.f;
    clear_pick::Pick(rows, true, true, c);
    Check(Is(c, 0, 0, 0, 1), "clrb.unread_row_ignored", Str(c));
  }

  // J2. an Emit with no latch of its frame never reads the colour: no row, and the census does not count it (S111 HEADGUARD: the off-thread fallback take is gone; such an Emit counts stale and returns)
  {
    BuildGuest(true, 255, 255, 255, 255);
    live::inputs::InputsContributor ic;
    ic.SetReader(ReadThunk);
    FrameInfo fi;
    fi.guest_frame = 308;
    ResetStamps();
    SceneTables t;
    Counters cn;
    ic.Emit(fi, t, cn);   // no Latch for this frame: Emit reads nothing
    size_t rows = 0;
    for (const StageLookRow& r : t.stage_look) rows += r.kind == kSlClearColor;
    std::string cen;
    ic.Census(cen);
    Check(rows == 0 && cen.find("[clear_colour read 0, unfed 0,") != std::string::npos, "clrb.fallback_take_no_row", std::to_string(rows) + " rows");
  }

  // K. record_frame.cpp's BuildFrame calls the pick with the record's rows, source and the switch (the 0.12 / 0.14 / 0.18 slate is gone from it)
  {
    std::ifstream f("src/renderer_scene/record_frame.cpp");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    Check(!s.empty() && s.find("clear_pick::Pick(t.stage_look, live, game_clear::On(), out.clear)") != std::string::npos && s.find("out.clear[0] = 0.12f") == std::string::npos, "clrb.build_frame_calls_pick",
          s.empty() ? "run from the repo root" : "");
  }

  std::printf("%s: %d checks, %d failed (plant '%s')\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail, g_plant.c_str());
  return g_fail ? 1 : 0;
}
