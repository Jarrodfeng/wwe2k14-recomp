// car_rope_unit: the offline proof of S111 lane CAARING (src\renderer_scene\car_rope_colours.h): a created arena's twelve rope colours, read from a fake guest ring table, carried as kFxCarRope rows and
// handed to the rope instances of our recompute. 0 game files, no SDK.
//   car_rope_unit [--plant NAME]
// A plant is a FAULT in the thing under test; each must make a NAMED check fail (exit 1 when any check failed):
//   white        RopeColourOf gives no colour (the draw before this lane: every rope white)   -> "apply.recomputed_rope_coloured"
//   zero_is_one  a word <= 0 reads as 1.0 instead of the game's 0.0                           -> "read.zero_and_negative_are_zero"
//   grid_only    only the grid recompute's rows (object 0) take a colour (the landed rule)  -> "apply.rids_rows_coloured"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/car_rope_colours.h"

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-38s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main(int argc, char** argv) {
  using namespace rdr_scene;
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc) plant = argv[++i];
  car_rope::g_rope_plant = plant == "white" ? 1 : plant == "zero_is_one" ? 2 : plant == "grid_only" ? 3 : 0;
  if (!plant.empty() && car_rope::g_rope_plant == 0) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }

  constexpr uint32_t kG = 0x40001000u;
  std::map<uint32_t, uint8_t> mem;
  auto W = [&](uint32_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) mem[at + uint32_t(i)] = uint8_t(v >> (24 - 8 * i));
  };
  auto rd = [&](uint32_t at, void* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      auto it = mem.find(at + uint32_t(i));
      if (it == mem.end()) return false;
      static_cast<uint8_t*>(d)[i] = it->second;
    }
    return true;
  };
  // the ring table: G, then twelve records {R, G, B, 255} at G + 128 + 16 i (i = 3 side + height)
  W(car_rope::kRingTableGlobal, kG);
  for (uint32_t i = 0; i < car_rope::kColours; ++i) {
    const uint32_t at = kG + car_rope::kColourOff + car_rope::kColourStride * i;
    W(at, 20 * i + 15), W(at + 4, 255 - 10 * i), W(at + 8, i), W(at + 12, 255);
  }
  W(kG + car_rope::kColourOff + 16 * 5 + 8, 0xFFFFFFFBu);   // record 5's blue = -5: not > 0 -> 0.0

  // ---------------------------------------------------------------- read
  car_rope::Colours c;
  const bool read = car_rope::ReadRopeColours(rd, c);
  Check(read && c.valid, "read.twelve_records", "G = [0x836F4C74], records at G + 128 + 16 i");
  Check(Near(c.rgb[0][0], 15.f / 255.f) && Near(c.rgb[0][1], 1.f) && Near(c.rgb[11][0], 235.f / 255.f) && Near(c.rgb[11][1], 145.f / 255.f) && Near(c.rgb[11][2], 11.f / 255.f),
        "read.values_over_255", "each word v > 0 -> v / 255 (sub_822A7F48)");
  Check(Near(c.rgb[0][2], 0.f) && Near(c.rgb[5][2], 0.f), "read.zero_and_negative_are_zero", "a word <= 0 -> 0.0 (0x8218B8CC)");
  {
    std::map<uint32_t, uint8_t> keep = mem;
    W(car_rope::kRingTableGlobal, 0);
    car_rope::Colours z;
    Check(!car_rope::ReadRopeColours(rd, z) && !z.valid, "read.null_table_no_colours", "G null: no rows (the ropes keep the file's ambient)");
    mem = keep;
    mem.erase(kG + car_rope::kColourOff + 16 * 11 + 4);
    Check(!car_rope::ReadRopeColours(rd, z) && !z.valid, "read.unreadable_no_colours", "a word unreadable: no rows (fail closed)");
    mem = keep;
  }

  // ---------------------------------------------------------------- rows
  std::vector<StageFxRow> rows;
  car_rope::ToRows(c, rows);
  Check(rows.size() == car_rope::kColours && rows[3].kind == kFxCarRope && rows[3].slot == 3, "rows.twelve_rows", "slot = the record index");
  car_rope::Colours back;
  Check(car_rope::FromRows(rows, back) && std::memcmp(back.rgb, c.rgb, sizeof c.rgb) == 0, "rows.roundtrip", "the rows carry the twelve colours");
  std::vector<StageFxRow> part(rows.begin(), rows.begin() + 11);
  car_rope::Colours partial;
  Check(!car_rope::FromRows(part, partial), "rows.partial_set_not_valid", "eleven rows: no colour at all");
  std::vector<StageFxRow> none;
  car_rope::ToRows(car_rope::Colours(), none);
  Check(none.empty(), "rows.unread_writes_nothing", "an unread set writes no rows");

  // ---------------------------------------------------------------- apply
  RingRows e;
  e.klass = kRcRopeHi, e.call = 7, e.flags = kRrRecomputed, e.object = 0;
  float rgb[3] = {9, 9, 9};
  const bool hi = car_rope::RopeColourOf(e, true, back, rgb);
  Check(hi && Near(rgb[0], c.rgb[7][0]) && Near(rgb[1], c.rgb[7][1]) && Near(rgb[2], c.rgb[7][2]), "apply.recomputed_rope_coloured", "our rope row (call = 3 side + height) takes record `call`");
  e.klass = kRcRopeLo;
  Check(car_rope::RopeColourOf(e, true, c, rgb) && Near(rgb[0], c.rgb[7][0]), "apply.low_rope_too", "the game colours the low LOD model as well");
  e.klass = kRcRopeHi;
  Check(!car_rope::RopeColourOf(e, false, c, rgb), "apply.normal_arena_untouched", "not a created arena: the file's ambient");
  RingRows g = e;
  g.flags = 0;
  Check(!car_rope::RopeColourOf(g, true, c, rgb), "apply.game_rows_untouched", "the game's own rows: call is not the record index");
  RingRows r = e;
  r.object = 0x82001234u;
  Check(car_rope::RopeColourOf(r, true, c, rgb) && Near(rgb[0], c.rgb[7][0]) && Near(rgb[2], c.rgb[7][2]), "apply.rids_rows_coloured",
        "a RIDS slot copy of a ring in motion (object = the slot's model, call = slot - 33 = 3 side + height, ring_rids_pose.h) takes record `call`: the game colours it before +0x678");
  RingRows cv = e;
  cv.klass = kRcCoverHi;
  Check(!car_rope::RopeColourOf(cv, true, c, rgb), "apply.other_class_untouched", "only the ropes take a colour");
  RingRows big = e;
  big.call = 12;
  Check(!car_rope::RopeColourOf(big, true, c, rgb), "apply.call_past_twelve_refused", "a call past the twelve records");
  Check(!car_rope::RopeColourOf(e, true, car_rope::Colours(), rgb), "apply.no_set_no_colour", "no rows read: the file's ambient");

  std::printf("%d checks, %d failed%s%s\n", g_checks, g_fail, plant.empty() ? "" : ", plant ", plant.c_str());
  return g_fail ? 1 : 0;
}
