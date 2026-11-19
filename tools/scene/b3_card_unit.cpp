// S103 B3c: the C++ unit for lane B3's card path and table rules. It runs the REAL code (no Python redo): the record row -> SceneFrame fields (card_gain.h FromRows, the function record_frame.cpp calls),
// the front end's gain rule (card_gain.h Effective / Gain, the functions FrontEnd::SetCardFactor / ApplyLmRow call), and the arena table's parser and Compute (src/assets/arena_lm.h) on a synthetic leaf.
//   b3_card_unit.exe [--plant factor_all_models|row_ignored|gain_double]      exit 0 = all pass; each plant must exit 1.
// Checks: (1) a row of 0.85 -> the card model's lm_gain bits == float(2.5f * 0.85f), non-card models stay 2.5; (2) no row / flags 0 / a row of 0 -> unscaled; (3) the gain G * 0.01f over G = 0..2000 equals the
// single-precision reference bit for bit (and the sweep tells f32 from double); (4) parser offsets (count at +20, array at 8 + Be(+32)), a zero record (finite span, gain 0), an id out of range (arena 4's record, counted).
// Compile only, no game.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/arena_lm.h"
#include "../../src/renderer_scene/card_gain.h"
#include "../../src/renderer_scene/front_end.h"

namespace al = assets::arena_lm;
namespace cg = rdr_scene::card_gain;
using rdr_scene::SceneFrame;

static int g_fail = 0;
static std::string g_plant;
static uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
static void Check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }

// The frame a record with these rows builds (the same call record_frame.cpp makes), then the gain of a card / non-card model as the front end resolves it.
static void Frame(const std::vector<rdr_scene::StageLookRow>& rows, float base, float& card_gain, float& other_gain) {
  SceneFrame f;
  cg::FromRows(rows, f.card_factor_valid, f.card_factor);
  if (g_plant == "row_ignored") f.card_factor_valid = false;   // plant: the row is read but dropped
  const float eff = cg::Effective(f.card_factor_valid, f.card_factor);
  card_gain = cg::Gain(base, true, eff);
  other_gain = cg::Gain(base, g_plant == "factor_all_models", eff);   // plant: the factor applied to every model
}

static rdr_scene::StageLookRow Row(float factor, uint32_t flags) {
  rdr_scene::StageLookRow r;
  r.kind = rdr_scene::kSlCardFactor, r.flags = flags, r.dir[0] = factor;
  return r;
}

static void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v); }

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  float c, o;
  // (1) the card model's gain = the table's gain x the row
  Frame({Row(0.85f, 1)}, 2.5f, c, o);
  Check(Bits(c) == Bits(float(2.5f * 0.85f)), "card model: lm_gain bits == float(2.5f * 0.85f)");
  Check(Bits(o) == Bits(2.5f), "non-card model: lm_gain stays 2.5");
  // (2) no row, flags 0, a row of 0: the gain is unscaled (the game applies the factor only when > 0)
  Frame({}, 2.5f, c, o);
  Check(Bits(c) == Bits(2.5f) && Bits(o) == Bits(2.5f), "no row: unscaled");
  Frame({Row(0.85f, 0)}, 2.5f, c, o);
  Check(Bits(c) == Bits(2.5f), "row with flags 0 (word not read): unscaled");
  Frame({Row(0.f, 1)}, 2.5f, c, o);
  Check(Bits(c) == Bits(2.5f) && Bits(o) == Bits(2.5f), "row of 0: unscaled");
  // (3) G * 0.01f over 0..2000 against a single-precision reference; the sweep must tell f32 from double
  int differs = 0, bad = 0;
  for (int g = 0; g <= 2000; ++g) {
    al::Table t;
    t.rec.assign(5, al::Record{5600, 5750, 250});
    t.rec[2] = {4000, 4100, g == 0 ? 1 : g};
    if (g == 0) { t.rec[2].g = 0; }
    const al::Param p = al::Compute(&t, 2);
    volatile float a = float(g), k = 0.01f;
    const float ref = a * k;
    const float dbl = float(double(g) * 0.01);
    differs += Bits(ref) != Bits(dbl);
    const float got = g_plant == "gain_double" ? dbl : p.v[2];   // plant: the old double product
    bad += Bits(got) != Bits(ref);
  }
  Check(bad == 0, "G * 0.01f over G = 0..2000 equals the f32 reference bit for bit");
  Check(differs > 0, "the sweep can tell f32 from double (some G differ)");
  // (4) a synthetic leaf: count at +20 (the +12 word is a decoy), array at 8 + Be(+32), pointers from the EDAT base (leaf + 8)
  std::vector<uint8_t> leaf(36 + 4 * 5 + 5 * 16, 0);
  std::memcpy(&leaf[8], "EDAT", 4);
  Put32(leaf, 12, 99), Put32(leaf, 20, 5), Put32(leaf, 32, 28);   // arr = 8 + 28 = 36
  const int32_t recs[5][3] = {{1, 1, 1}, {0, 0, 0}, {4000, 4100, 100}, {1, 1, 1}, {5600, 5750, 250}};
  for (int i = 0; i < 5; ++i) {
    const size_t at = 36 + 20 + 16 * size_t(i);
    Put32(leaf, 36 + 4 * size_t(i), uint32_t(at - 8));   // pointer = offset from the EDAT base
    Put32(leaf, at, 1), Put32(leaf, at + 4, uint32_t(recs[i][0])), Put32(leaf, at + 8, uint32_t(recs[i][1])), Put32(leaf, at + 12, uint32_t(recs[i][2]));
  }
  al::Table t;
  Check(al::ParseLeaf(leaf.data(), leaf.size(), t) && t.rec.size() == 5, "parser: count = Be(+20), array at 8 + Be(+32)");
  Check(t.rec.size() == 5 && t.rec[2].x == 4000 && t.rec[2].z == 4100 && t.rec[2].g == 100 && t.rec[4].g == 250, "parser: record = leaf + 8 + pointer");
  std::vector<uint8_t> cut(leaf.begin(), leaf.begin() + 60);
  al::Table tc;
  Check(!al::ParseLeaf(cut.data(), cut.size(), tc), "parser: a truncated leaf is refused (bounds-checked)");
  Put32(leaf, 32, 0x7FFFFFFF);
  Check(!al::ParseLeaf(leaf.data(), leaf.size(), tc), "parser: an array offset past the leaf is refused");
  const uint64_t z0 = al::g_zero.load(), c0 = al::g_clamped.load();
  const al::Param pz = al::Compute(&t, 1);
  Check(pz.zero && pz.v[2] == 0.f && pz.v[0] > 0.f && pz.v[0] < 1e30f && pz.v[1] > 0.f && pz.v[1] < 1e30f && !pz.clamped, "zero record: finite span, gain 0 (not arena 4's record)");
  Check(al::g_zero.load() == z0 + 1, "zero record is counted (g_zero)");
  const al::Param po = al::Compute(&t, 150), pn = al::Compute(&t, 7), pr = al::Compute(&t, 220);
  Check(po.clamped && Bits(po.v[2]) == Bits(250.f * 0.01f) && Bits(po.v[0]) == Bits(1.0f / 5600.f), "id >= 100: arena 4's record [L stand-in], counted");
  Check(pn.clamped && pr.clamped && al::g_clamped.load() == c0 + 3, "id >= count and the 200..249 remap (99): counted");
  const al::Param pk = al::Compute(&t, 2);
  Check(!pk.clamped && !pk.zero && Bits(pk.v[0]) == Bits(1.0f / 4000.f) && Bits(pk.v[1]) == Bits(1.0f / 4100.f) && Bits(pk.v[2]) == Bits(100.f * 0.01f) && pk.v[3] == 1.f, "a normal record: (1 / X, 1 / Z, G * 0.01f, 1)");
  // the counters have a reader: the census line of the unit prints them
  std::printf("census: clamped %llu no_table %llu zero %llu\n", (unsigned long long)al::g_clamped.load(), (unsigned long long)al::g_no_table.load(), (unsigned long long)al::g_zero.load());
  std::printf("%d FAIL%s\n", g_fail, g_plant.empty() ? "" : ("  (plant: " + g_plant + ")").c_str());
  return g_fail ? 1 : 0;
}
