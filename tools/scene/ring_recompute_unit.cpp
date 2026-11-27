// ring_recompute_unit: the offline proof of S103 lane C3's ring rope-row recompute (docs\renderer\STAGE_LIVE_WRITERS.md section 3). 0 game runs, no SDK, no game memory.
//   ring_recompute_unit [--plant NAME]
// It runs the SHIPPED code (src\scene_live\ring_recompute_core.h + ring_recompute_contrib.h) against a FAKE GUEST built from the offsets the listing of sub_825CF2F8 gives (core header: X + 96 / 100 / 104,
// descriptors X + 10692 + 12 (128 h + j), counts X + 29124 + 4 h, buffer X + 29184 + 8192 h, G2 = [0x83768A00], layers [G2 + 64 + 4 r] -> +440 -> 96 records x 80 bytes).
// THE ORACLE is worked by hand from the listing's rules (single numbers below, no code shared with the shipped rule): the rest point of descriptor (a, b) on rope r is
// (288 - 24 b, y_r, -288) turned by a quarter turns; the record is i = 24 a + b (0 when >= 96); even sides write x - P.x, odd sides write z - P.z.
// A plant is a FAULT in the fake guest; each must make a NAMED check fail (exit 1 when any check failed):
//   grid_stride     the guest lays the grid records 76 bytes apart (the game: 80)             -> "ring.grid_values"
//   desc_swapped    the descriptor holds b at +4 and a at +8                                    -> "ring.descriptors"
//   side_stride     the rest buffers of the sides sit 4096 apart (the game: 8192)               -> "ring.rest_pose"
//   counts_swapped  the counts of sides 0 and 1 are exchanged                                   -> "ring.counts"
//   layer_swapped   the layer pointers of ropes 0 and 1 are exchanged                           -> "ring.rope_height"
//   table_stride    the table elements are 16 bytes apart (the game: 12)                        -> "ring.table"
//   mode_ignored    the latch ignores --wwe2k14_rdr_scene = 0                                   -> "mode.off_reads_nothing"
//   lose_flag       the round trip drops kRrRecomputed                                          -> "record.roundtrip"
// S103 C4 (covers, posts, skirts, bit 6); the oracle numbers are worked by hand from the listing of sub_825CE668 and the numbers of run 16 (inv.t = (284.997, -0.0001, 285)):
//   cv_desc_stride  the cover descriptors sit 16 bytes apart (the game: 12)                     -> "ring.cv_values"
//   cv_ang_base     the corner angle base is pi/2 (the image word 0x8201D4D0 is 3 pi/2)         -> "ring.cv_values"
//   cv_side_plus    the descriptor's side is 1 more (the loop adds 1 itself)                     -> "ring.cv_values"
//   cv_ovr_off      the third-layer override vector sits 8 bytes off                             -> "ring.cv_override"
//   cv_alpha_off    the corner fades sit 4 bytes off                                              -> "ring.cv_alpha"
//   post_step_off   the post angle step word reads 0 (the image word is pi/2)                    -> "ring.post_values"
//   skirt_pal_off   the skirt palette pointer is 64 bytes off                                     -> "ring.skirt_values"
//   cv_desc_low     the cover descriptors sit 65536 bytes low (C4 read 62056 for lis 1 | ori 62056)    -> "ring.cv_values"   (S103 C6)
//   skirt_glob_wrong the camera record pointer sits at 0x83763AA8 (C5 typed 0x8376 for lis -31889)  -> "ring.skirt_visible" (S103 C6)
//   whole_no_coversthe cover model is gone from the guest of the clean ring (no stamp may be made)  -> "ring.whole_stamp"
//   rids_fade_off   the corner fades sit 4 bytes off, read by the RIDS (ring break) rows          -> "ring.rids_fade"   (S111 TBPAD review)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_ring.h"
#include "../../src/scene_live/ring_recompute_contrib.h"

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace rr = rdr_scene::live::ringrecompute;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static bool Near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }
static int g_mode = 2;
static uint64_t g_stamp_calls = 0, g_stamp_frame = 0;
static void StampSink(uint64_t record_frame) { ++g_stamp_calls, g_stamp_frame = record_frame; }

struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); words[a] = b; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = rr::Swap32(it->second);
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g_guest;
static uint64_t g_reads_scoped = 0, g_reads_unscoped = 0;   // S108 RDS review: reads made inside / outside a pass scope (safe_guest_pages.h PassLines; the game's ReadGuest serves the scoped ones from the pass's line copy)
static bool ReadThunk(uint32_t a, void* dst, size_t n) { ++(wwe2k14::safe_pages::PassLines::Current() ? g_reads_scoped : g_reads_unscoped); return g_guest.Read(a, dst, n); }

static constexpr uint32_t kX = 0x40100000, kG = 0x40010000, kG2 = 0x40020000, kGate = 0x40030000, kY = 0x40180000;   // S111 RINGLOD: kY = the ring-side object (its own block)
static uint32_t LayerObj(uint32_t r) { return 0x40040000 + r * 0x1000; }
static uint32_t LayerBuf(uint32_t r) { return 0x40100000 + 0x100000 + r * 0x4000; }   // 96 * 80 = 7680 bytes each

struct D { int32_t n, a, b; };
// Side h's descriptors (the hand-worked scenario; the same list for the three ropes, as the game's).
static const std::vector<D> kDesc[4] = {{{5, 0, 3}, {0, 2, 0}, {3, 4, 0}}, {{2, 1, 6}}, {{1, 3, 12}}, {}};

// ---- S103 C4: the cover / post / skirt pieces of the fake guest, at the LISTING literals (never the shipped constants): counts X + 31932 * 4 + 4 r25 (0x825CE7E8), descriptors X + 127584 + 12 d (n), +4 (side), +8 (c)
// (S103 C6: r16 = (lis 1) | 62056 = 0x1F268 [0x825CE708 / 0x825CE724]; r29 = X + 36 r25 + r16 and -8 / -4 / 0 of it, 0x825CE81C-0x825CE82C, 0x825CE8D8, 0x825CE940), corner fades X + 4 (2342 + r25) (0x825CEB54), override flag X + 4 (2364 + r25) (0x825CEA54), vector X + 16 (587 + r25) (0x825CEA68).
static constexpr uint32_t kCm = 0x40700000, kCmVt = 0x40710000, kCmPal = 0x40720000, kCmInv = 0x40730000, kPm = 0x40740000, kPmVt = 0x40750000, kPmPal = 0x40760000, kSkBase = 0x40800000;
static void BuildPieces() {
  const uint32_t X = kX;
  g_guest.Put(0x8376AB34u, 0);   // the arena id word (sub_827D7578 + 64): not the alt ring
  g_guest.Put(0x8201D4D0u, Plant("cv_ang_base") ? 0x3FC90FDBu : 0x4096CBE4u);   // 3 pi/2
  // S105 C4 / S111 RINGLOD: the post / cover far test's T[0] = the ring object's words X + 0x1F314 / X + 0x1F320 (sub_825CD8D0 +0x1F4 / +0x200; the callers pass them in r3: LISTING literals); the plant
  // moves them: the far-test check must then fail. The image word they are filled from is a decoy here (1e9): a reader of it finds nothing far.
  g_guest.PutF(X + 0x1F314u, Plant("lod_dist") ? 1.0e9f : 3000.f), g_guest.PutF(X + 0x1F320u, Plant("lod_dist") ? 1.0e9f : 3000.f);
  g_guest.PutF(0x82012B14u, 1.0e9f);
  g_guest.Put(0x82000ADCu, 0x3FC90FDBu);   // pi/2
  g_guest.Put(0x82118560u, 0x40490FDBu);   // pi
  g_guest.Put(0x82000A94u, 0xC0490FDBu);   // -pi
  g_guest.Put(0x82000A98u, 0x40C90FDBu);   // 2 pi
  // the cover model: 3 nodes, inverse binds: node 2 translation (284.997, -0.0001, 285) = run 16's constant; palette identity
  g_guest.Put(kG + 36, kCm);
  g_guest.Put(kG + 40, 0);
  // S111 RINGLOD: a created arena's own cover slots (sub_82740550: [G + 4 (r5 + 17)] near, [G + 4 (r5 + 20)] far, corners 0..2; LISTING literals). The game's G holds them in every arena (a normal ring never
  // reads them): the near ones name the same cover model here, the far ones are empty.
  for (uint32_t c = 0; c < 3; ++c) g_guest.Put(kG + 68 + 4 * c, kCm), g_guest.Put(kG + 80 + 4 * c, 0);
  g_guest.Put(kCm, kCmVt);
  g_guest.Put(kCmVt + 8, 0x82BD3AB8u);
  g_guest.Put(kCm + 296, 1);
  g_guest.Put(kCm + 300, 3);
  g_guest.Put(kCm + 200, kCmPal);
  g_guest.Put(kCm + 224, kCmInv);
  for (uint32_t k = 0; k < 3; ++k)
    for (uint32_t w = 0; w < 16; ++w) {
      const float id = (w % 5 == 0) ? 1.f : 0.f;
      g_guest.PutF(kCmPal + 64 * k + 4 * w, id);
      float v = id;
      if (k == 2 && w == 12) v = 284.997f;
      if (k == 2 && w == 13) v = -0.0001f;
      if (k == 2 && w == 14) v = 285.f;
      g_guest.PutF(kCmInv + 64 * k + 4 * w, v);
    }
  for (uint32_t c = 0; c < 4; ++c) {
    g_guest.Put(X + 31932u * 4u + 4 * c, 1);   // one descriptor per corner
    const uint32_t d = X + ((0x10000u | 62056u) - 8u) + (Plant("cv_desc_stride") ? 16u : 12u) * (3 * c) - (Plant("cv_desc_low") ? 0x10000u : 0u);   // S103 C6: the LISTING literal (lis 1 | ori 62056 = 127592, n at -8)
    g_guest.Put(d, 2);                                                  // n: the cover's third node
    g_guest.Put(d + 4, c + (Plant("cv_side_plus") ? 1u : 0u));          // side: the init stores r4 = the corner
    g_guest.Put(d + 8, 0);                                              // c
    g_guest.PutF(X + 4u * (2342 + c) + ((Plant("cv_alpha_off") || Plant("rids_fade_off")) ? 4u : 0u), 1.f);   // fade
    g_guest.Put(X + 4u * (2364 + c), c == 2 ? 1u : 0u);                  // override only on corner 2
    const uint32_t v = X + 16u * (587 + c) + (Plant("cv_ovr_off") ? 8u : 0u);
    g_guest.PutF(v, 7.f), g_guest.PutF(v + 4, 8.f), g_guest.PutF(v + 8, 9.f), g_guest.PutF(v + 12, 1.f);
  }
  g_guest.PutF(X + 4u * (2342 + 1) + ((Plant("cv_alpha_off") || Plant("rids_fade_off")) ? 4u : 0u), 0.5f);   // corner 1 third layer: half faded
  g_guest.PutF(X + 4u * (2342 + 3) + ((Plant("cv_alpha_off") || Plant("rids_fade_off")) ? 4u : 0u), 0.f);    // corner 3 third layer: hidden (not posed, not submitted)
  // posts: [G + 48] 3 nodes, [G + 52] = 0 (no low post)
  g_guest.Put(kG + 48, kPm);
  g_guest.Put(kG + 52, 0);
  for (uint32_t h = 0; h < 3; ++h) g_guest.Put(kG + 92 + 4 * h, kPm), g_guest.Put(kG + 104 + 4 * h, 0);   // S111 RINGLOD: loop 2's own post slots (sub_82740480: [G + 4 (r5 + 23)] / [G + 4 (r5 + 26)])
  g_guest.Put(kPm, kPmVt);
  g_guest.Put(kPmVt + 8, 0x82BD3AB8u);
  g_guest.Put(kPm + 296, 1);
  g_guest.Put(kPm + 300, 3);
  g_guest.Put(kPm + 200, kPmPal);
  for (uint32_t w = 0; w < 48; ++w) g_guest.PutF(kPmPal + 4 * w, (w % 5 == 0) ? 1.f : 0.f);   // 3 nodes of identity (the post palette as it stands; the rows come from the angle alone)
  if (Plant("post_step_off")) g_guest.Put(0x82000ADCu, 0);
  // skirts: G + 332 + 4 s, 4 nodes each, palette = a recognisable pattern (node k, float w = 100 s + k + w / 100)
  for (uint32_t s = 0; s < 4; ++s) {
    const uint32_t m = kSkBase + 0x10000u * s, vt = m + 0x4000, pal = m + 0x8000;
    g_guest.Put(kG + 332 + 4 * s, m);
    g_guest.Put(m, vt);
    g_guest.Put(vt + 8, 0x82BD3AB8u);
    g_guest.Put(m + 296, 1);
    g_guest.Put(m + 300, 4);
    g_guest.Put(m + 200, Plant("skirt_pal_off") ? pal + 64 : pal);
    for (uint32_t k = 0; k < 4; ++k)
      for (uint32_t w = 0; w < 16; ++w) g_guest.PutF(pal + 64 * k + 4 * w, 100.f * float(s) + float(k) + float(w) / 100.f);
  }
  // S103 C5: the skirt visibility inputs at the LISTING literals (sub_82609060): P = [0x83763AA8] (+16 != 0, +24 = the camera array, +32 = the index), cam = [P + 24] + 272 [P + 32], the floats at cam + 144 (a) and + 152 (b),
  // the threshold float at 0x8218B8CC (0.0), the draw word at 0x83763928, the side table at 0x82020A94 (0 1 2 3), the state words at X + 1248 + 4 i
  const uint32_t P = 0x40900000, arr = 0x40910000;
  g_guest.Put(Plant("skirt_glob_wrong") ? 0x83763AA8u : 0x836F3AA8u, P);   // S103 C6: the LISTING literal (sub_82609060 0x82609090: lis r11,-31889 = 0x836F0000, lwz 15016)
  g_guest.Put(P + 16, 1), g_guest.Put(P + 24, arr), g_guest.Put(P + 32, 0);
  g_guest.PutF(arr + 144, 0.f);                                                // a
  g_guest.PutF(arr + 152, Plant("skirt_quad_wrong") ? 0.f : -5.f);            // b: |b| > |a|, b < 0 -> quadrant 0 -> side 2 faces away
  if (Plant("skirt_quad_wrong")) g_guest.PutF(arr + 144, -5.f);                // a: quadrant 1 -> side 3 faces away (the wrong skirt hidden)
  g_guest.PutF(rr::kQuadThreshold, 0.f);
  g_guest.Put(0x836F3928u, 2);   // S103 C6: the LISTING literal (lis r23,-31889, lwz 14632). S111 RINGLOD: it names side 2, the hidden one: the POSE keeps side 2 for it, the DRAW does not (ring.skirt_visible wants 0xB)
  // S111 RINGLOD: the state words are the ring-SIDE object's (Y + 1248 + 4 i: sub_82609290 +0x118); X's words at the same offset hold 1 (a decoy: a reader of X hides every skirt). Plant skirt_side_obj: the
  // noted object is X (the old reading) -> ring.skirt_visible.
  for (uint32_t i = 0; i < 4; ++i) g_guest.Put(rr::kSkirtSideTable + 4 * i, i), g_guest.Put(kY + rr::kSkirtStateOff + 4 * i, 0), g_guest.Put(X + rr::kSkirtStateOff + 4 * i, 1);
  rr::RingSideSlot().store(Plant("skirt_side_obj") ? X : kY);
}

static void BuildGuest(bool alt, bool gate_open = true, bool g20 = true, bool mg = false) {
  g_guest.words.clear();
  g_guest.Put(rr::kRingGlobal, kG);
  g_guest.Put(kG + 20, g20 ? 0x40500000u : 0u);
  g_guest.Put(kX + rr::kGateOff, gate_open ? 1u : 0u);
  g_guest.Put(kX + rr::kAltOff, alt ? 1u : 0u);
  g_guest.Put(rr::kModelGate, mg ? kGate : 0u);
  g_guest.Put(kGate + 368, mg ? 1u : 0u);
  g_guest.Put(rr::kOverrideGate, 0);
  // counts + descriptors
  for (uint32_t h = 0; h < 4; ++h) {
    const uint32_t ch = Plant("counts_swapped") ? (h == 0 ? 1 : h == 1 ? 0 : h) : h;
    g_guest.Put(kX + rr::kCountsOff + 4 * h, uint32_t(kDesc[ch].size()));
    for (size_t j = 0; j < kDesc[h].size(); ++j) {
      const uint32_t a = kX + rr::kEntriesOff + 12 * (128 * h + uint32_t(j));
      g_guest.Put(a, uint32_t(kDesc[h][j].n));
      g_guest.Put(a + 4, uint32_t(Plant("desc_swapped") ? kDesc[h][j].b : kDesc[h][j].a));
      g_guest.Put(a + 8, uint32_t(Plant("desc_swapped") ? kDesc[h][j].a : kDesc[h][j].b));
    }
  }
  // the grid: layer r, record i = (i, 10 (r + 1), -i); a decoy in each record's unused words
  g_guest.Put(rr::kGridGlobal, kG2);
  const uint32_t stride = Plant("grid_stride") ? 76u : 80u;
  for (uint32_t r = 0; r < 3; ++r) {
    const uint32_t lr = Plant("layer_swapped") ? (r == 0 ? 1 : r == 1 ? 0 : r) : r;
    g_guest.Put(kG2 + rr::kLayerOff + 4 * r, LayerObj(lr));
    g_guest.Put(LayerObj(r) + rr::kLayerBuf, LayerBuf(r));
    for (uint32_t i = 0; i < 96; ++i) {
      const uint32_t rec = LayerBuf(r) + stride * i;
      g_guest.PutF(rec, float(i));
      g_guest.PutF(rec + 4, 10.f * float(r + 1));
      g_guest.PutF(rec + 8, -float(i));
      for (uint32_t w = 12; w < 80; w += 4) g_guest.PutF(LayerBuf(r) + stride * i + w, 99.f);
    }
  }
  // the table (alt mode): element m = (m, 2, 3)
  const uint32_t tstride = Plant("table_stride") ? 16u : 12u;
  for (uint32_t m = 0; m < 12 * 64; ++m) {
    g_guest.PutF(kX + rr::kTableOff + tstride * m, float(m));
    g_guest.PutF(kX + rr::kTableOff + tstride * m + 4, 2.f);
    g_guest.PutF(kX + rr::kTableOff + tstride * m + 8, 3.f);
  }
  // the turnbuckle model T = [G + 44]: 4 palette nodes (node count by the rule: slot 2 of its vtable = 0x82BD3AB8, [T + 296] != 0 -> [T + 300]); palette identity; inverse binds: node 1 translation
  // (331.9335, 0, 320) = the constant run 16's rows give, node 2 (100, 0, 50) (our own value, hand oracle); the constants the loop reads from the image
  {
    constexpr uint32_t kT = 0x40620000, kVt = 0x40630000, kPal = 0x40600000, kInv = 0x40610000;
    g_guest.Put(kG + 44, kT);
    g_guest.Put(kT, kVt);
    g_guest.Put(kVt + 8, 0x82BD3AB8u);
    g_guest.Put(kT + 296, 1);
    g_guest.Put(kT + 300, 4);
    g_guest.Put(kT + 200, kPal);
    g_guest.Put(kT + (Plant("tb_inv_off") ? 228 : 224), kInv);
    for (uint32_t k = 0; k < 4; ++k)
      for (uint32_t w = 0; w < 16; ++w) {
        const float id = (w % 5 == 0) ? 1.f : 0.f;
        g_guest.PutF(kPal + 64 * k + 4 * w, id);
        float v = id;
        if (w == 12 && k == 1) v = 331.9335f;
        if (w == 14 && k == 1) v = 320.f;
        if (w == 12 && k == 2) v = 100.f;
        if (w == 14 && k == 2) v = 50.f;
        g_guest.PutF(kInv + 64 * k + 4 * w, v);
      }
    // the index words at the LISTING literals (r22 = (lis r9,1) | 62256 = 0x1F330, +4: 0x825CE2E8 / 0x825CE314 / 0x825CE474), NOT the shipped constants: a wrong constant must fail here (review 1)
    g_guest.Put(kX + (0x10000u | 62256u), Plant("tb_idx_swapped") ? 2u : 1u);
    g_guest.Put(kX + (0x10000u | 62256u) + 4u, Plant("tb_idx_swapped") ? 1u : 2u);
    const float vecs[16] = {-318.5f, -120.f, -318.5f, 1.f, -318.5f, -120.f, 318.5f, 1.f, 318.5f, -120.f, 318.5f, 1.f, 318.5f, -120.f, -318.5f, 1.f};
    for (uint32_t i = 0; i < 16; ++i) g_guest.PutF(rr::kTbVecTable + 4 * i, vecs[i]);
    const float angs[4] = {-0.7853981852531433f, 0.7853981852531433f, 2.356194496154785f, -2.356194496154785f};
    for (uint32_t i = 0; i < 4; ++i) g_guest.PutF(rr::kTbAngleTable + 4 * i, angs[Plant("tb_ang_off") ? (i + 1) % 4 : i]);
    g_guest.PutF(rr::kTbStep, 42.f);
    g_guest.PutF(rr::kTbHeight0, -154.f);
  }
  // the rest buffers: side h row k = a recognisable pose: m[0] = 0.25 + h, m[5] = 0.5 + k / 100, m[10] = 1, m[15] = 1, translation 7 7 7
  for (uint32_t h = 0; h < 4; ++h)
    for (uint32_t k = 0; k < 8; ++k) {
      const uint32_t row = kX + rr::kBufOff + (Plant("side_stride") ? 4096u : 8192u) * h + 64 * k;
      for (uint32_t w = 0; w < 16; ++w) g_guest.PutF(row + 4 * w, 0.f);
      g_guest.PutF(row + 0, 0.25f + float(h));
      g_guest.PutF(row + 20, 0.5f + float(k) / 100.f);
      g_guest.PutF(row + 40, 1.f);
      g_guest.PutF(row + 60, 1.f);
      g_guest.PutF(row + 48, 7.f), g_guest.PutF(row + 52, 7.f), g_guest.PutF(row + 56, 7.f);
    }
  BuildPieces();
}

static size_t RopeEntries(const std::vector<RingRows>& v) {
  size_t n = 0;
  for (const RingRows& r : v) n += r.klass == kRcRopeHi;
  return n;
}
static uint32_t LayerObjFor(uint32_t r) { return LayerObj(r); }
static const RingRows* EntryOf(const std::vector<RingRows>& v, uint32_t call) {
  for (const RingRows& r : v)
    if (r.call == call) return &r;
  return nullptr;
}
// the translation of buffer row `row` of an entry (the rows are VS c76..c78: row k = (m[k], m[4+k], m[8+k], m[12+k]) -> translation = rows[3], rows[7], rows[11])
static bool Trans(const std::vector<RingNodeRows>& pool, const RingRows& e, uint32_t row, float t[3]) {
  if (row >= e.n_nodes) return false;
  const RingNodeRows& n = pool[e.first_node + row];
  t[0] = n.rows[3], t[1] = n.rows[7], t[2] = n.rows[11];
  return true;
}
static bool T3(const std::vector<RingNodeRows>& pool, const RingRows* e, uint32_t row, float x, float y, float z, std::string* why = nullptr) {
  float t[3];
  if (!e || !Trans(pool, *e, row, t)) { if (why) *why += " missing row " + std::to_string(row); return false; }
  const bool ok = Near(t[0], x) && Near(t[1], y) && Near(t[2], z);
  if (!ok && why) *why += " row " + std::to_string(row) + " got (" + std::to_string(t[0]) + ", " + std::to_string(t[1]) + ", " + std::to_string(t[2]) + ") want (" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")";
  return ok;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  scene_frame::g_mode_cvar.store(&g_mode);

  // ------------------------------------------------------------ 1. grid mode, hand-worked values (rope 1: y_r = -196, grid y = 20)
  BuildGuest(false);
  rr::Inputs in;
  rr::RingCensus cn;
  rr::ReadInputs(ReadThunk, kX, in, cn);
  std::vector<RingRows> rows;
  std::vector<RingNodeRows> pool;
  for (uint32_t h = 0; h < 4; ++h)
    for (uint32_t r = 0; r < 3; ++r) rr::AppendRope(in, h, r, 41, rows, pool, cn);
  {
    rr::TbInputs tbi;
    rr::ReadTurnbuckleInputs(ReadThunk, kX, in, tbi, cn);
    for (uint32_t c = 0; c < 4; ++c)
      for (uint32_t h = 0; h < 3; ++h) rr::AppendTurnbuckle(tbi, c, h, 41, rows, pool, cn);
  }
  {
    Check(in.gates_ok && RopeEntries(rows) == 9 && rows.size() == 21, "ring.counts", "gates " + std::to_string(int(in.gates_ok)) + " entries " + std::to_string(rows.size()) + " (3 sides x 3 ropes = 9; side 3 has no descriptors)");
  }
  {
    // rope 1 (call = 3 h + r): side 0: (n 5: a 0 b 3 -> P (216, -196, -288), record 3 = (3, 20, -3) -> (-213, 20, -3)); (n 0: a 2 b 0 -> P turned (-288, 288), record 48 = (48, 20, -48) -> (336, 20, -48))
    //                          side 0 n 3: a 4 b 0 -> i = 96 -> record 0 = (0, 20, 0); P (288, -288) not turned -> (0 - 288, 20, 0)
    std::string why;
    const RingRows* e = EntryOf(rows, 1);
    bool ok = e && T3(pool, e, 5, -213.f, 20.f, -3.f, &why) && T3(pool, e, 0, 336.f, 20.f, -48.f, &why) && T3(pool, e, 3, -288.f, 20.f, 0.f, &why);
    Check(ok, "ring.grid_values", why);
  }
  {
    // side 1 (odd): a 1 b 6 rope 1: P base (144, -288) turned a=1 -> (z, -x) = (-288, -144); record 30 = (30, 20, -30): x stays 30, z = -30 - (-144) = 114
    // side 2 (even): a 3 b 12: base (0, -288) turned a=3 -> (-z, x) = (288, 0); record 84 = (84, 20, -84): x = 84 - 288 = -204, z stays -84
    std::string why;
    const bool ok = T3(pool, EntryOf(rows, 3 * 1 + 1), 2, 30.f, 20.f, 114.f, &why) && T3(pool, EntryOf(rows, 3 * 2 + 1), 1, -204.f, 20.f, -84.f, &why);
    Check(ok, "ring.descriptors", why);
  }
  {
    // the three heights: the same descriptor on ropes 0 / 2 reads the other layer and the other y constant: side 0 n 5, rope 0 -> record 3 of layer 0 = (3, 10, -3), P (216, -154, -288) -> (-213, 10, -3);
    //                                                                                                      rope 2 -> (3, 30, -3) from layer 2 -> (-213, 30, -3)
    std::string why;
    const bool ok = T3(pool, EntryOf(rows, 0), 5, -213.f, 10.f, -3.f, &why) && T3(pool, EntryOf(rows, 2), 5, -213.f, 30.f, -3.f, &why);
    Check(ok, "ring.rope_height", why);
  }
  {
    // the rest pose: a node the descriptors do not name keeps the buffer's translation (7 7 7) and every node keeps its rotation (m[0] = 0.25 + h, m[5] = 0.5 + k / 100)
    const RingRows* e = EntryOf(rows, 1);
    const RingRows* e1 = EntryOf(rows, 3);
    bool ok = e && e1 && e->n_nodes == 6 && e1->n_nodes == 3;
    if (ok) {
      const RingNodeRows& n4 = pool[e->first_node + 4];   // row 4 of side 0: not named
      const RingNodeRows& n5 = pool[e->first_node + 5];   // row 5 of side 0: named; rotation kept
      const RingNodeRows& s2 = pool[e1->first_node + 1];  // row 1 of side 1: not named
      ok = Near(n4.rows[3], 7.f) && Near(n4.rows[7], 7.f) && Near(n4.rows[11], 7.f) && Near(n4.rows[0], 0.25f) && Near(n4.rows[5], 0.54f) && Near(n5.rows[0], 0.25f) && Near(n5.rows[5], 0.55f) &&
           Near(s2.rows[0], 1.25f) && Near(s2.rows[3], 7.f);
    }
    Check(ok, "ring.rest_pose", e ? ("n_nodes " + std::to_string(e->n_nodes)) : "no entry");
  }
  {
    const RingRows* e = EntryOf(rows, 4);
    Check(e && (e->flags & kRrRecomputed) && e->r5 == kX + rr::kBufOff + 8192 * 1 && e->frame == 41 && e->call == e->keyed, "ring.entry_fields",
          e ? ("r5 " + std::to_string(e->r5) + " flags " + std::to_string(e->flags)) : "no entry");
  }

  // ------------------------------------------------------------ 1b. the turnbuckles (step 2): 12 instances, hand oracle = the numbers of run 16's rows (corner 0 height 0: node 1 (-310.0618, -154, 142.4866);
  // corner 1 height 1: (142.4866, -196, 310.0618)); node 2 = the grid record (24 (c + 1)) + (100, 0, 50) * R by hand: corner 0 height 0 = (24, 10, -24) + (35.3553, 0, 106.0660)
  {
    std::string why;
    uint32_t n = 0;
    for (const RingRows& r : rows) n += r.klass == kRcTurnbuckle;
    const RingRows* t0 = nullptr;
    const RingRows* t4 = nullptr;
    for (const RingRows& r : rows)
      if (r.klass == kRcTurnbuckle && r.call == 0) t0 = &r;
      else if (r.klass == kRcTurnbuckle && r.call == 4) t4 = &r;
    bool ok = n == 12 && t0 && t4 && (t0->flags & kRrRecomputed) && t0->n_nodes == 4 && t0->r5 == 0x40600000u;
    if (ok) {
      ok = T3(pool, t0, 1, -310.0618f, -154.f, 142.4866f, &why) && T3(pool, t4, 1, 142.4866f, -196.f, 310.0618f, &why) && T3(pool, t0, 2, 59.3553f, 10.f, 82.0660f, &why) && T3(pool, t0, 0, 0.f, 0.f, 0.f, &why) &&
           T3(pool, t0, 3, 0.f, 0.f, 0.f, &why);
      const float* w = pool[t0->first_node + 1].rows;   // node 1 rotation as run 16's rows: columns (cos, 0, sin a) = (0.7071, 0, -0.7071)
      ok = ok && Near(w[0], 0.70710678f, 1e-5f) && Near(w[2], -0.70710678f, 1e-5f) && Near(w[10], 0.70710678f, 1e-5f);
    }
    Check(ok, "ring.tb_values", why + " entries " + std::to_string(n));
  }

  // ------------------------------------------------------------ 1c. the turnbuckles' own gates (review 2-5): independent of the rope gates, RIDS playback, node count < 3, index words -1
  {
    auto tbrows = [&](rr::RingCensus& c) {
      rr::Inputs i2;
      rr::ReadInputs(ReadThunk, kX, i2, c);
      rr::TbInputs t2;
      rr::ReadTurnbuckleInputs(ReadThunk, kX, i2, t2, c);
      std::vector<RingRows> rr2;
      std::vector<RingNodeRows> pp2;
      for (uint32_t cc = 0; cc < 4; ++cc)
        for (uint32_t hh = 0; hh < 3; ++hh) rr::AppendTurnbuckle(t2, cc, hh, 1, rr2, pp2, c);
      return rr2.size() == 12;
    };
    rr::RingCensus c1, c2, c3, c4, c5;
    BuildGuest(false);
    g_guest.Put(kX + rr::kGateOff, 0);                    // the ROPE gate [X + 96] closed: the turnbuckles still read
    const bool a = tbrows(c1);
    BuildGuest(false);
    g_guest.Put(rr::kRidsByteWord, 0x00000100u);          // the byte at 0x837632FA = 1: RIDS playback
    const bool b = !tbrows(c2);
    BuildGuest(false);
    g_guest.Put(0x40620000u + 300, 2);                    // [T + 300] = 2 nodes: not posed
    const bool c = !tbrows(c3);
    BuildGuest(false);
    g_guest.Put(kX + (0x10000u | 62256u), 0xFFFFFFFFu);   // idxA = -1
    const bool d = !tbrows(c4);
    BuildGuest(false);
    g_guest.words.erase(LayerObjFor(0) + rr::kLayerBuf);  // the grid layer unreadable: tb_unreadable, not the rope counter
    const bool e = tbrows(c5) == false;
    Check(a && b && c && d && e && c1.gate_closed == 1 && c2.rope_rids == 1 && c2.tb_rids == 1 && c3.tb_nopose == 1 && c4.tb_gate == 1 && c5.tb_unreadable == 4 && c5.unreadable == 0, "ring.tb_gates",
          "a" + std::to_string(a) + std::to_string(b) + std::to_string(c) + std::to_string(d) + std::to_string(e) + " rids " + std::to_string(c2.tb_rids) + " nopose " + std::to_string(c3.tb_nopose) + " gate " + std::to_string(c4.tb_gate) +
              " unreadable " + std::to_string(c5.tb_unreadable) + "/" + std::to_string(c5.unreadable));
    BuildGuest(false);
  }


  // ------------------------------------------------------------ 1d. S103 C4: the covers (sub_825CE668), hand-worked from the listing and run 16's inv.t; layer r record i = (i, 10 (r + 1), -i)
  BuildGuest(false);
  {
    rr::Inputs ic;
    rr::RingCensus cc;
    rr::ReadInputs(ReadThunk, kX, ic, cc);
    rr::CvInputs cv;
    rr::ReadCoverInputs(ReadThunk, kX, ic, cv, cc);
    std::vector<RingRows> cr;
    std::vector<RingNodeRows> cp;
    for (uint32_t c = 0; c < 4; ++c)
      for (uint32_t l = 0; l < 3; ++l) rr::AppendCover(cv, ic, 0, c, l, 77, cr, cp, cc);
    {
      // call 0: corner 0 layer 0: side (0 + 1) % 4 = 1, c = 0 -> record 24 = (24, 10, -24); angle 0 -> identity: t = (24 + 284.997, 10 - 0.0001, -24 + 285)
      // call 4: corner 1 layer 1: side 2 -> record 48 = (48, 20, -48); angle pi/2: inv.t * R = (285, -0.0001, -284.997): t = (333, 19.9999, -332.997)
      // call 8: corner 2 layer 2: override flag set -> translation (7, 8, 9); angle pi: inv.t * R = (-284.997, -0.0001, -285): t = (-277.997, 7.9999, -276)
      std::string why;
      const bool ok = cv.ok && T3(cp, EntryOf(cr, 0), 2, 308.997f, 9.9999f, 261.f, &why) && T3(cp, EntryOf(cr, 4), 2, 333.f, 19.9999f, -332.997f, &why) && T3(cp, EntryOf(cr, 5), 2, 333.f, 29.9999f, -332.997f, &why);
      Check(ok, "ring.cv_values", why + " entries " + std::to_string(cr.size()));
    }
    {
      std::string why;
      const bool ok = T3(cp, EntryOf(cr, 8), 2, -277.997f, 7.9999f, -276.f, &why);
      Check(ok, "ring.cv_override", why);
    }
    {
      // corner 1 layer 2 (call 5) is faded to 0.5: posed, alpha 0.5; corner 3 layer 2 (call 11) fades to 0: no entry (the game neither poses nor submits), counted hidden
      const RingRows* e5 = EntryOf(cr, 5);
      Check(e5 && Near(e5->alpha, 0.5f) && EntryOf(cr, 11) == nullptr && cr.size() == 11 && cc.cv_hidden == 1 && cc.cv_rows == 11, "ring.cv_alpha", "entries " + std::to_string(cr.size()) + " hidden " + std::to_string(cc.cv_hidden));
    }
    {
      const RingRows* e = EntryOf(cr, 0);
      bool ok = e && e->klass == kRcCoverHi && e->object == kCm && e->r5 == kCmPal && e->n_nodes == 3 && (e->flags & kRrRecomputed) && e->frame == 77 && e->call == e->keyed && Near(e->alpha, 1.f);
      std::string why;
      ok = ok && T3(cp, e, 0, 0.f, 0.f, 0.f, &why) && T3(cp, e, 1, 0.f, 0.f, 0.f, &why);   // nodes 0 / 1 are not posed: the palette as it stands (identity)
      Check(ok, "ring.cv_entry", why);
    }
    {   // the gates: RIDS playback, table mode, the model gate: no cover rows, each counted on its own counter. S103 CRR: the alt ring (arena id 220) is NOT a gate any more: the game poses its covers by the same rule
        // (sub_825CE668: 0x825CE738-0x825CE7DC picks the model per corner, 0x825CE848-0x825CE89C replaces the angle and the record side per call corner; CoverNodes follows with cv.alt), so the read goes on and counts cv_alt_ring; its posts read as the 8-call alt set (CRK section 2)
      auto covers = [&](rr::RingCensus& c) {
        rr::Inputs i2;
        rr::ReadInputs(ReadThunk, kX, i2, c);
        rr::CvInputs v2;
        rr::ReadCoverInputs(ReadThunk, kX, i2, v2, c);
        return v2.ok;
      };
      rr::RingCensus a, b, c3, d, e;
      BuildGuest(false);
      g_guest.Put(0x8376AB34u, 220);
      const bool alt = covers(a);
    {   // S111 RINGLOD: a created arena's corner 1 poses ITS slot [G + 72] (sub_825CE668 +0x124 -> sub_82740550 r5 = 1): empty = no cover rows at corner 1 (the r27 == 0 skip, +0x17C); corners 0 / 2 pose their
        // own slots and corner 3 [G + 36] (sub_827403E8). The reading before RINGLOD posed [G + 36] at every corner: 3 rows at corner 1.
      g_guest.Put(kG + 72, 0);
      rr::RingCensus k;
      rr::Inputs ki;
      rr::ReadInputs(ReadThunk, kX, ki, k);
      rr::CvInputs kv;
      rr::ReadCoverInputs(ReadThunk, kX, ki, kv, k);
      std::vector<RingRows> kr;
      std::vector<RingNodeRows> kp;
      size_t per[4] = {};
      for (uint32_t c = 0; c < 4; ++c) {
        const size_t before = kr.size();
        for (uint32_t l = 0; l < 3; ++l) rr::AppendCover(kv, ki, 0, c, l, 77, kr, kp, k);
        per[c] = kr.size() - before;
      }
      const RingRows* k0 = EntryOf(kr, 0);
      Check(kv.ok && kv.alt && per[0] == 3 && per[1] == 0 && per[2] == 3 && per[3] == 2 && k0 && k0->object == kCm, "ring.cv_alt_slots",
            "rows per corner " + std::to_string(per[0]) + " " + std::to_string(per[1]) + " " + std::to_string(per[2]) + " " + std::to_string(per[3]));
      g_guest.Put(kG + 72, kCm);
    }
      rr::RingCensus ap;
      rr::Inputs ai;
      rr::ReadInputs(ReadThunk, kX, ai, ap);
      rr::PostInputs apo;
      rr::ReadPostInputs(ReadThunk, kX, ai, apo, ap);
      const bool alt_posts = apo.ok && apo.alt && rr::PostCalls(apo) == 8 && ap.post_alt_ring == 1;
      BuildGuest(false);
      g_guest.Put(rr::kRidsByteWord, 0x00000100u);
      const bool rids = !covers(b);
      BuildGuest(true);
      const bool table = !covers(c3);
      BuildGuest(false, true, true, true);
      const bool gate = !covers(d);
      BuildGuest(false);
      g_guest.Put(0x40620000u, 0);   // placeholder write: the turnbuckle vtable word is unused here
      const bool fine = covers(e);
      Check(alt && alt_posts && rids && table && gate && fine && a.cv_alt_ring == 1 && b.cv_rids == 1 && c3.cv_table_mode == 1 && d.cv_gate == 1, "ring.cv_gates",
            "alt " + std::to_string(a.cv_alt_ring) + " rids " + std::to_string(b.cv_rids) + " table " + std::to_string(c3.cv_table_mode) + " gate " + std::to_string(d.cv_gate));
      BuildGuest(false);
    }
  }

  // ------------------------------------------------------------ 1e. posts: RotY(i * pi/2) in every node, zero translation (run 16's rows); skirts: the live palette as it stands
  BuildGuest(false);
  {
    rr::Inputs ip;
    rr::RingCensus cp2;
    rr::ReadInputs(ReadThunk, kX, ip, cp2);
    rr::PostInputs po;
    rr::ReadPostInputs(ReadThunk, kX, ip, po, cp2);
    std::vector<RingRows> pr;
    std::vector<RingNodeRows> pp;
    for (uint32_t i = 0; i < 4; ++i) rr::AppendPost(po, 0, i, 55, pr, pp, cp2);
    bool ok = po.ok && pr.size() == 4 && cp2.post_rows == 4;
    // call 1: angle pi/2: c = 0, s = 1: rows[0] = m0 = 0, rows[2] = m8 = 1, rows[8] = m2 = -1; call 2: angle pi: rows[0] = -1; call 3: 3 pi/2 wraps to -pi/2: rows[2] = -1; call 0: identity
    std::string why;
    auto rot = [&](uint32_t call, uint32_t node, float r0, float r2, float r8) {
      const RingRows* e = EntryOf(pr, call);
      if (!e || e->n_nodes != 3 || e->r5 != kPmPal || e->klass != kRcPostHi) { why += " bad entry " + std::to_string(call); return false; }
      const float* w = pp[e->first_node + node].rows;
      const bool g = Near(w[0], r0, 1e-5f) && Near(w[2], r2, 1e-5f) && Near(w[8], r8, 1e-5f) && Near(w[3], 0.f) && Near(w[7], 0.f) && Near(w[11], 0.f) && Near(w[5], 1.f);
      if (!g) why += " call " + std::to_string(call) + " node " + std::to_string(node) + " rows " + std::to_string(w[0]) + " " + std::to_string(w[2]) + " " + std::to_string(w[8]);
      return g;
    };
    ok = ok && rot(0, 0, 1.f, 0.f, 0.f) && rot(1, 0, 0.f, 1.f, -1.f) && rot(1, 2, 0.f, 1.f, -1.f) && rot(2, 1, -1.f, 0.f, 0.f) && rot(3, 2, 0.f, -1.f, 1.f);
    Check(ok, "ring.post_values", why);
    rr::ModelPal sk[rr::kSkirtCount];
    rr::RingCensus cs;
    rr::ReadSkirts(ReadThunk, 0xFu, sk, cs);
    std::vector<RingRows> sr;
    std::vector<RingNodeRows> sp;
    for (uint32_t s = 0; s < rr::kSkirtCount; ++s) rr::AppendSkirt(sk[s], s, 55, sr, sp, cs);
    // skirt s node k float w = 100 s + k + w / 100: m0 -> rows[0], translation m12..m14 -> rows[3], rows[7], rows[11]
    bool sok = sr.size() == 4 && cs.skirt_rows == 4;
    for (size_t i = 0; sok && i < sr.size(); ++i) {
      const float s = float(i);
      const RingRows& e = sr[i];
      const float* w = sp[e.first_node + 3].rows;   // node 3
      sok = e.klass == kRcSkirt && e.n_nodes == 4 && e.r5 == kSkBase + 0x10000u * uint32_t(i) + 0x8000u && Near(w[0], 100.f * s + 3.f, 1e-3f) && Near(w[3], 100.f * s + 3.f + 0.12f, 1e-3f) && Near(w[7], 100.f * s + 3.f + 0.13f, 1e-3f) &&
            Near(w[11], 100.f * s + 3.f + 0.14f, 1e-3f);
    }
    Check(sok, "ring.skirt_values", "entries " + std::to_string(sr.size()));
  }

  // ------------------------------------------------------------ 1f. the whole-ring verdict (input bit 6): stamps only when every piece has its rows for the record
  {
    auto run = [&](bool override_gate, bool alt, bool drop_covers, bool drop_skirts, uint64_t frame, uint64_t& calls, uint64_t& at) {
      BuildGuest(alt);
      g_guest.Put(kX + rr::kCountsOff + 4 * 3, 1);                                    // side 3 gets one descriptor: 12 ropes
      g_guest.Put(kX + rr::kEntriesOff + 12 * (128 * 3), 1), g_guest.Put(kX + rr::kEntriesOff + 12 * (128 * 3) + 4, 0), g_guest.Put(kX + rr::kEntriesOff + 12 * (128 * 3) + 8, 0);
      if (override_gate) g_guest.Put(rr::kOverrideGate, 1);
      if (drop_covers || Plant("whole_no_covers")) g_guest.Put(kG + 36, 0);
      if (drop_skirts)
        for (uint32_t s = 0; s < 4; ++s) g_guest.Put(kG + 332 + 4 * s, 0);
      g_stamp_calls = 0, g_stamp_frame = 0;
      rr::RingRecomputeContributor con;
      con.SetReader(ReadThunk);
      con.SetStampSink(&StampSink);
      con.NoteRing(kX, frame);
      FrameInfo f;
      f.guest_frame = frame;
      con.Latch(f);
      SceneTables t;
      Counters c;
      con.Emit(f, t, c);
      const rr::RingCensus st = con.Stats();
      calls = st.whole_clean, at = g_stamp_calls;   // S103 C5: the verdict is counted; the stamp itself is HELD (kStampRingRows = false): at must stay 0
      return t.ring_rows.size();
    };
    uint64_t c1, f1, c2, f2, c3, f3, c4, f4, c5, f5, c6, f6;
    const size_t n1 = run(false, false, false, false, 900, c1, f1);   // clean: 12 + 12 + 11 covers (+ 1 hidden) + 4 posts + 3 skirts (the one facing away is not posed) = 42 rows
    run(true, false, false, false, 901, c2, f2);                      // override gate set: rows 2 / 26 not replicated -> no stamp
    run(false, false, true, false, 902, c3, f3);                      // no cover model
    run(false, false, false, true, 903, c4, f4);                      // no skirt model
    run(false, true, false, false, 904, c5, f5);                      // table mode
    run(false, false, false, false, 905, c6, f6);                     // clean again, a later frame
    const uint64_t on = rr::kStampRingRows ? 1 : 0;   // S103 main: bit 6 released: a clean frame stamps once, an unclean one never
    Check(c1 == 1 && c2 == 0 && c3 == 0 && c4 == 0 && c5 == 0 && c6 == 1 && n1 == 42 && f1 == on && f2 == 0 && f3 == 0 && f4 == 0 && f5 == 0 && f6 == on && g_stamp_calls == on, "ring.whole_stamp",
          "clean " + std::to_string(c1) + " stamps " + std::to_string(f1) + " rows " + std::to_string(n1) + " override " + std::to_string(c2) + " nocover " + std::to_string(c3) + " noskirt " + std::to_string(c4) + " table " + std::to_string(c5) + " again " + std::to_string(c6));
    BuildGuest(false);
  }

  // ------------------------------------------------------------ 1g. S103 C5: the skirts the game poses (visibility), one LOD model per cover / post, no count for an absent model
  {
    // (a) the pure rule: the quadrant function, the state word, the draw word, RIDS, the early exit
    rr::SkirtVisIn v;
    v.ok = true, v.quad = 0, v.draw_word = 1;
    for (uint32_t i = 0; i < 4; ++i) v.side[i] = i;
    bool ok = rr::QuadrantOf(0.f, -5.f, 0.f) == 0 && rr::QuadrantOf(-5.f, 0.f, 0.f) == 1 && rr::QuadrantOf(0.f, 5.f, 0.f) == 2 && rr::QuadrantOf(5.f, 0.f, 0.f) == 3;
    std::string why;
    ok = ok && rr::SkirtVisibleMask(v) == 0xBu;   // quadrant 0 hides side 2
    v.quad = 1, v.draw_word = 0, ok = ok && rr::SkirtVisibleMask(v) == 0x7u;   // hides side 3
    v.quad = 2, v.draw_word = 1, ok = ok && rr::SkirtVisibleMask(v) == 0xEu;   // hides side 0
    v.quad = 3, v.draw_word = 0, ok = ok && rr::SkirtVisibleMask(v) == 0xDu;   // hides side 1


    v.quad = 0, v.state[1] = 1, ok = ok && rr::SkirtVisibleMask(v) == 0x9u;   // state 1: not posed
    v.state[1] = 0, v.draw_word = 2, ok = ok && rr::SkirtVisibleMask(v) == 0xFu;   // the draw word names side 2: posed although it faces away
    v.draw_word = 1, v.rids = true, ok = ok && rr::SkirtVisibleMask(v) == 0xFu;   // RIDS playback: all
    v.rids = false, v.early_exit = true, ok = ok && rr::SkirtVisibleMask(v) == 0u;
    Check(ok, "ring.skirt_rule", "pure rule");
    {   // S111 RINGLOD: the DRAW's rule (sub_82609290): no draw-word exception; P / [P + 16] zero = nothing drawn unless RIDS; the state word and the quadrant as the pose
      rr::SkirtVisIn d;
      d.ok = true, d.quad = 0, d.draw_word = 2;
      for (uint32_t i = 0; i < 4; ++i) d.side[i] = i;
      bool dk = rr::SkirtDrawnMask(d) == 0xBu && rr::SkirtVisibleMask(d) == 0xFu;   // the draw word names the hidden side 2: posed, not drawn
      d.state[3] = 1, dk = dk && rr::SkirtDrawnMask(d) == 0x3u;
      d.state[3] = 0, d.cam_ok = false, dk = dk && rr::SkirtDrawnMask(d) == 0u;
      d.rids = true, dk = dk && rr::SkirtDrawnMask(d) == 0xFu;
      d.rids = false, d.cam_ok = true, d.early_exit = true, dk = dk && rr::SkirtDrawnMask(d) == 0u;
      Check(dk, "ring.skirt_draw_rule", "the draw's rule");
    }
    // (b) through the contributor: the guest above (quadrant 0, side 2 faces away): exactly the skirts of sides 0, 1, 3 are emitted, and the verdict wants exactly 3
    BuildGuest(false);
    rr::RingRecomputeContributor con;
    con.SetReader(ReadThunk);
    con.NoteRing(kX, 700);
    FrameInfo f;
    f.guest_frame = 700;
    g_reads_scoped = g_reads_unscoped = 0;
    con.Latch(f);
    Check(g_reads_scoped > 0 && g_reads_unscoped == 0 && !wwe2k14::safe_pages::PassLines::Current(), "rds.ring_take_pass_scoped",
          "reads in the pass scope " + std::to_string((unsigned long long)g_reads_scoped) + ", outside it " + std::to_string((unsigned long long)g_reads_unscoped));   // S108 RDS review: the take is one pass (its line copy dies with it)
    SceneTables t;
    Counters c;
    con.Emit(f, t, c);
    uint32_t sides = 0, n_sk = 0;
    for (const RingRows& r : t.ring_rows)
      if (r.klass == kRcSkirt) sides |= 1u << r.pad2, ++n_sk;
    const rr::RingCensus st = con.Stats();
    Check(sides == 0xBu && n_sk == 3 && st.skirt_vis_unreadable == 0, "ring.skirt_visible", "sides " + std::to_string(sides) + " count " + std::to_string(n_sk));
    // (c) one model per cover / post: the guest names a LOW cover [G + 40] and a LOW post [G + 52]; neither is read or emitted; they are counted
    BuildGuest(false);
    g_guest.Put(kG + 40, kCm), g_guest.Put(kG + 52, kPm);
    rr::RingRecomputeContributor con2;
    con2.SetReader(ReadThunk);
    con2.NoteRing(kX, 701);
    FrameInfo f2;
    f2.guest_frame = 701;
    con2.Latch(f2);
    SceneTables t2;
    Counters c2;
    con2.Emit(f2, t2, c2);
    if (Plant("lod_both")) {   // planted: a contributor that emits both LODs would add the low covers: they must be caught
      rr::Inputs ip;
      rr::RingCensus cx;
      rr::CvInputs cv;
      rr::ReadInputs(ReadThunk, kX, ip, cx);
      rr::ReadCoverInputs(ReadThunk, kX, ip, cv, cx);
      cv.models[1] = cv.models[0];
      std::vector<RingNodeRows> pp;
      rr::AppendCover(cv, ip, 1, 0, 0, 701, t2.ring_rows, pp, cx);
    }
    uint32_t lo = 0, hi_cov = 0, hi_post = 0;
    for (const RingRows& r : t2.ring_rows) {
      if (r.klass == kRcCoverLo || r.klass == kRcPostLo) ++lo;
      if (r.klass == kRcCoverHi) ++hi_cov;
      if (r.klass == kRcPostHi) ++hi_post;
    }
    const rr::RingCensus s2 = con2.Stats();
    Check(lo == 0 && hi_cov == 11 && hi_post == 4 && s2.cv_lod_both >= 1 && s2.post_lod_both >= 1 && s2.cv_unreadable == 0 && s2.post_unreadable == 0, "ring.lod_single",
          "low " + std::to_string(lo) + " covers " + std::to_string(hi_cov) + " posts " + std::to_string(hi_post) + " cv_unreadable " + std::to_string(s2.cv_unreadable));
    // (c2) S105 C4: the game's post / cover LOD (sub_82740350 / sub_827403E8 -> sub_8273FF58): LOW when the eye is more than T[0] = [0x82012B14] (3000) from the corner point turned by the corner. The same guest (a low cover and
    // a low post present), the eye at (0, -196, 2800): corners 0 and 3 (z = -288 / -313 after the turn) are 3101 / 3128 away = low; corners 1 and 2 are 2528 / 2487 = high. No view = every corner high (above).
    {
      rr::RingRecomputeContributor con3;
      con3.SetReader(ReadThunk);
      rr::RopeView vw;
      vw.has = true, vw.eye[0] = 0.f, vw.eye[1] = -196.f, vw.eye[2] = 2800.f, vw.fwd[2] = -1.f;
      con3.NoteRing(kX, 703, &vw);
      FrameInfo f3;
      f3.guest_frame = 703;
      con3.Latch(f3);
      SceneTables t3;
      Counters c3x;
      con3.Emit(f3, t3, c3x);
      uint32_t cov_lo = 0, cov_hi = 0, post_lo = 0, post_hi = 0;
      bool corner_ok = true;
      for (const RingRows& r : t3.ring_rows) {
        const bool is_cov = r.klass == kRcCoverHi || r.klass == kRcCoverLo, is_post = r.klass == kRcPostHi || r.klass == kRcPostLo;
        if (!is_cov && !is_post) continue;
        const bool low = r.klass == kRcCoverLo || r.klass == kRcPostLo;
        const uint32_t corner = is_cov ? r.call / 3 : r.call;
        const bool want_low = corner == 0 || corner == 3;
        corner_ok = corner_ok && (low == want_low);
        (is_cov ? (low ? cov_lo : cov_hi) : (low ? post_lo : post_hi)) += 1;
      }
      const rr::RingCensus s3 = con3.Stats();
      // 11 cover rows as above (12 less the hidden one); low = corners 0 / 3, high = corners 1 / 2; never both models of one corner (the sum is unchanged)
      Check(corner_ok && cov_lo + cov_hi == 11 && cov_lo >= 3 && cov_hi >= 3 && post_lo == 2 && post_hi == 2 && s3.cv_low == cov_lo && s3.post_low == 2 && s3.lod_dist_unreadable == 0, "ring.lod_far_test",
            "cover lo/hi " + std::to_string(cov_lo) + "/" + std::to_string(cov_hi) + " post lo/hi " + std::to_string(post_lo) + "/" + std::to_string(post_hi) + " corner_ok " + std::to_string(corner_ok));
      // the threshold is the game's word, not a literal: a different value in the ring object's two words moves the split (1e9 = nothing is far)
      g_guest.PutF(kX + 0x1F314u, 1.0e9f), g_guest.PutF(kX + 0x1F320u, 1.0e9f);
      rr::RingRecomputeContributor con4;
      con4.SetReader(ReadThunk);
      con4.NoteRing(kX, 704, &vw);
      FrameInfo f4;
      f4.guest_frame = 704;
      con4.Latch(f4);
      SceneTables t4;
      Counters c4x;
      con4.Emit(f4, t4, c4x);
      uint32_t lo4 = 0;
      for (const RingRows& r : t4.ring_rows)
        if (r.klass == kRcCoverLo || r.klass == kRcPostLo) ++lo4;
      Check(lo4 == 0, "ring.lod_threshold_read", "low rows with T = 1e9: " + std::to_string(lo4));
      g_guest.PutF(kX + 0x1F314u, 3000.f), g_guest.PutF(kX + 0x1F320u, 3000.f);
    }
    // (d) an absent model is not a failure: no low cover and a hidden skirt count nothing; a skirt that IS visible and unreadable counts once
    Check(s2.skirt_unreadable == 0 && st.cv_unreadable == 0, "ring.absent_not_counted", "skirt_unreadable " + std::to_string(s2.skirt_unreadable) + " cv_unreadable " + std::to_string(st.cv_unreadable));
    BuildGuest(false);
    g_guest.Put(kSkBase + 0x8000u * 0 + 200, 0x7F000000u);   // model 0 (visible): its palette pointer is unmapped
    rr::ModelPal sk3[rr::kSkirtCount];
    rr::RingCensus c3;
    rr::ReadSkirts(ReadThunk, 0xBu, sk3, c3);
    std::vector<RingRows> r3;
    std::vector<RingNodeRows> p3;
    for (uint32_t s = 0; s < rr::kSkirtCount; ++s) rr::AppendSkirt(sk3[s], s, 702, r3, p3, c3);
    Check(c3.skirt_unreadable == 1 && r3.size() == 2, "ring.skirt_fail_once", "unreadable " + std::to_string(c3.skirt_unreadable) + " rows " + std::to_string(r3.size()));
    BuildGuest(false);
  }

  // ------------------------------------------------------------ 2. table mode (X + 100 != 0)
  BuildGuest(true);
  {
    rr::Inputs ia;
    rr::RingCensus ca;
    rr::ReadInputs(ReadThunk, kX, ia, ca);
    std::vector<RingRows> ra;
    std::vector<RingNodeRows> pa;
    for (uint32_t h = 0; h < 4; ++h)
      for (uint32_t r = 0; r < 3; ++r) rr::AppendRope(ia, h, r, 42, ra, pa, ca);
    // side 0 rope 1 j 0: m = 64 * (3 * 0 + 1) + 0 = 64: src (64, 2, 3), P (216, -196, -288), even -> (64 - 216, 2, 3) = (-152, 2, 3)
    // side 1 rope 2 j 0: m = 64 * (3 + 2) = 320: src (320, 2, 3), P base (144, -288) turned a=1 -> z = -144: odd -> (320, 2, 3 + 144) = (320, 2, 147)
    std::string why;
    const bool ok = ia.alt && T3(pa, EntryOf(ra, 1), 5, -152.f, 2.f, 3.f, &why) && T3(pa, EntryOf(ra, 3 * 1 + 2), 2, 320.f, 2.f, 147.f, &why) && ca.alt_frames == 1;
    Check(ok, "ring.table", why);
  }

  // ------------------------------------------------------------ 3. the gates: X + 96 = 0, no rope model, the model task's gate: no rows, counted
  {
    rr::RingCensus c2;
    rr::Inputs i2;
    BuildGuest(false, false);
    rr::ReadInputs(ReadThunk, kX, i2, c2);
    const bool a = !i2.gates_ok;
    BuildGuest(false, true, false);
    rr::ReadInputs(ReadThunk, kX, i2, c2);
    const bool b = !i2.gates_ok;
    BuildGuest(false, true, true, true);
    rr::ReadInputs(ReadThunk, kX, i2, c2);
    const bool c = !i2.gates_ok;
    rr::ReadInputs(ReadThunk, 0, i2, c2);   // no ring object yet
    Check(a && b && c && c2.gate_closed == 2 && c2.no_ring == 2, "ring.gates_counted", "gate_closed " + std::to_string(c2.gate_closed) + " no_ring " + std::to_string(c2.no_ring));
  }

  // ------------------------------------------------------------ 4. the contributor: latch / emit, the stamp, the mode gate
  BuildGuest(false);
  {
    rr::RingRecomputeContributor con;
    con.SetReader(ReadThunk);
    con.NoteRing(kX, Plant("clock_early") ? 299 : 300);   // the ring callback reads g_guest_frame one ahead of the walks (run 14): it notes the record's own frame
    FrameInfo f;
    f.guest_frame = 300;
    const bool latched = con.Latch(f);
    SceneTables t;
    t.ring_nodes.resize(5);   // a pool that already holds rows: first_node must be rebased
    Counters c;
    con.Emit(f, t, c);
    bool ok = latched && t.ring_rows.size() == 39 && c.objects == 39;
    for (const RingRows& r : t.ring_rows) ok = ok && r.first_node >= 5 && uint64_t(r.first_node) + r.n_nodes <= t.ring_nodes.size() && r.frame == 300;   // the stamp is the callback frame = the record frame (run 16)
    Check(ok, "contrib.latch_emit", "entries " + std::to_string(t.ring_rows.size()) + " pool " + std::to_string(t.ring_nodes.size()));
    {   // F3: an X noted in an earlier frame (the ring task is not running now) gives no rows, counted
      rr::RingRecomputeContributor stale;
      stale.SetReader(ReadThunk);
      stale.NoteRing(kX, Plant("stale_x") ? 300 : 100);
      FrameInfo fs;
      fs.guest_frame = 300;
      stale.Latch(fs);
      SceneTables ts;
      Counters cs;
      stale.Emit(fs, ts, cs);
      Check(ts.ring_rows.empty() && stale.Stats().no_call == 1 && stale.Stats().delta[7] == 1 && stale.Stats().noted_calls == 1, "ring.noted_expires", "entries " + std::to_string(ts.ring_rows.size()) + " no_call " + std::to_string(stale.Stats().no_call));
    }
    {   // the clock: a note equal to the record frame is accepted; one frame early or late is not (run 14: the callback notes record frame = hook frame + 1)
      size_t got[3] = {};
      for (int k = 0; k < 3; ++k) {
        rr::RingRecomputeContributor cc;
        cc.SetReader(ReadThunk);
        cc.NoteRing(kX, uint64_t(300 + k - 1));   // note 299 / 300 / 301 against record 300
        FrameInfo fk;
        fk.guest_frame = 300;
        cc.Latch(fk);
        SceneTables tk;
        Counters ck;
        cc.Emit(fk, tk, ck);
        got[k] = tk.ring_rows.size();
      }
      Check(got[0] == 0 && got[1] == 39 && got[2] == 0, "ring.clock_plus_one", "rows for notes 299 / 300 / 301: " + std::to_string(got[0]) + " " + std::to_string(got[1]) + " " + std::to_string(got[2]));
    }
    {   // S110 RINGBREAK: the ring in motion posing (bit 0 of 0x837632FA): rows from the per-slot palettes the hook copied right after each pose call (ring_rids_pose.h); the canvas row
      namespace ri = rdr_scene::live::ringrids;
      constexpr uint32_t kRopeM = 0x40500000, kTbM = 0x40620000;
      const uint32_t cmodel[4] = {kPm, kTbM, kCm, kRopeM};   // posts 5..8, turnbuckles 9..20, covers 21..32, ropes 33..44 (the slot switch's models: G+48, G+44, G+36, G+20)
      const uint32_t cnodes[4] = {3, 4, 3, 5};
      const uint32_t cbase[4] = {5, 9, 21, 33}, clast[4] = {8, 20, 32, 44};
      const uint8_t cklass[4] = {kRcPostHi, kRcTurnbuckle, kRcCoverHi, kRcRopeHi};
      auto cls_of = [](uint32_t k) { return k < 9 ? 0u : k < 21 ? 1u : k < 33 ? 2u : 3u; };
      auto mkslot = [&](uint32_t k, uint32_t src) {   // slot k holding the palette of pose `src` (per slot: translation (100 src + node, 0.5 + node / 100, -src))
        ri::Slot s;
        s.k = k, s.model = cmodel[cls_of(k)], s.n = cnodes[cls_of(k)], s.palette = 0x41000000u + 0x1000u * k;
        s.pal.assign(size_t(s.n) * 16, 0.f);
        for (uint32_t j = 0; j < s.n; ++j) {
          float* m = &s.pal[size_t(j) * 16];
          m[0] = m[5] = m[10] = m[15] = 1.f;
          m[12] = 100.f * float(src) + float(j), m[13] = 0.5f + float(j) / 100.f, m[14] = -float(src);
        }
        return s;
      };
      auto publish = [&]() {
        ri::Set set;
        set.frame = 300;
        for (uint32_t k = 5; k <= 44; ++k) {
          uint32_t src = k;
          if (Plant("rids_latch_read")) src = clast[cls_of(k)];   // the shared model read once at the latch: every instance of a class holds the LAST instance's pose
          ri::Slot s = mkslot(k, src);
          if (Plant("rids_table")) {                              // the RIDS recorder table: the ropes are not copied there, and rows of two slots can overlap
            if (cls_of(k) == 3) s.n = 0, s.pal.clear();
            if (k == 6) s = mkslot(6, 5);
          }
          set.slots.push_back(std::move(s));
        }
        ri::The().Publish(std::move(set));
      };
      auto latch = [&](rr::RingRecomputeContributor& c, SceneTables& tt) {
        c.SetReader(ReadThunk);
        c.SetStampSink(StampSink);
        c.NoteRing(kX, 300);
        FrameInfo fr;
        fr.guest_frame = 300;
        c.Latch(fr);
        Counters cc2;
        c.Emit(fr, tt, cc2);
      };
      auto find = [](const SceneTables& tt, uint8_t klass, uint32_t call) -> const RingRows* {
        for (const RingRows& r : tt.ring_rows)
          if (r.klass == klass && r.call == call) return &r;
        return nullptr;
      };
      BuildGuest(false);
      g_guest.Put(rr::kRidsByteWord, 0x00000100u);
      publish();
      const uint64_t stamps0 = g_stamp_calls;
      rr::RingRecomputeContributor rc;
      SceneTables tr;
      latch(rc, tr);
      {
        uint32_t n[4] = {};
        bool ok = true;
        std::string why;
        for (uint32_t c = 0; c < 4; ++c)
          for (uint32_t call = 0; call < (c == 0 ? 4u : 12u); ++call) {
            if (c == 2 && call == 11) continue;   // S111 TBPAD review: corner 3's pad (the guest's fade 0) has no row: ring.rids_fade
            const RingRows* e = find(tr, cklass[c], call);
            if (!e) { ok = false, why += " missing " + std::to_string(c) + "/" + std::to_string(call); continue; }
            ++n[c];
            const uint32_t k = cbase[c] + call;
            ok = ok && (e->flags & kRrRecomputed) && e->n_nodes == cnodes[c] && e->object == cmodel[c] && e->r5 == 0x41000000u + 0x1000u * k && e->frame == 300;
            ok = ok && T3(tr.ring_nodes, e, 0, 100.f * float(k), 0.5f, -float(k), &why) && T3(tr.ring_nodes, e, cnodes[c] - 1, 100.f * float(k) + float(cnodes[c] - 1), 0.5f + float(cnodes[c] - 1) / 100.f, -float(k), &why);
          }
        Check(ok && n[0] == 4 && n[1] == 12 && n[2] == 11 && n[3] == 12 && rc.Stats().rids_rows == 39 && rc.Stats().rids_refused == 0, "ring.rids_values", why + " rows " + std::to_string(n[0]) + "/" + std::to_string(n[1]) + "/" + std::to_string(n[2]) + "/" + std::to_string(n[3]));
      }
      {   // S111 TBPAD review: sub_825CE668 tests a corner's pad fade after the RIDS pose too (the pose call at 0x825CE90C, then the test 0x825CEB38-0x825CEB64: no submit when <= 0). The guest's corner 3 fade is 0:
          // no row for call 11 (counted hidden, the other calls keep their index); corner 1's is 0.5: call 5's alpha; the lower layers (calls 4, 10) keep alpha 1
        const RingRows* e11 = find(tr, kRcCoverHi, 11);
        const RingRows* e5 = find(tr, kRcCoverHi, 5);
        const RingRows* e4 = find(tr, kRcCoverHi, 4);
        const RingRows* e10 = find(tr, kRcCoverHi, 10);
        Check(!e11 && e5 && Near(e5->alpha, 0.5f) && e4 && Near(e4->alpha, 1.f) && e10 && Near(e10->alpha, 1.f) && rc.Stats().cv_hidden == 1, "ring.rids_fade",
              std::string("call 11 ") + (e11 ? "drawn" : "hidden") + ", call 5 alpha " + (e5 ? std::to_string(e5->alpha) : std::string("none")) + ", hidden " + std::to_string(rc.Stats().cv_hidden));
      }
      Check(rc.Stats().whole_clean == 1 && g_stamp_calls == stamps0 + 1 && rc.Stats().whole_unclean == 0, "ring.rids_clean", "clean " + std::to_string(rc.Stats().whole_clean) + " unclean " + std::to_string(rc.Stats().whole_unclean) + " stamps " + std::to_string(g_stamp_calls - stamps0));
      {   // refusals: a model below 0x10000, a node count above the class's, and a slot past 44 give no row and are counted; the rest stay
        ri::Set set;
        set.frame = 300;
        for (uint32_t k = 5; k <= 44; ++k) set.slots.push_back(mkslot(k, k));
        set.slots[0].model = 0x8000;                                    // slot 5: not a model
        set.slots[1].n = 200, set.slots[1].pal.assign(200u * 16u, 0.f); // slot 6: a post with 200 nodes
        set.refused = 1;                                                // a slot 45 that still named a model
        ri::The().Publish(std::move(set));
        rr::RingRecomputeContributor rf;
        SceneTables tf;
        latch(rf, tf);
        size_t posts = 0;
        for (const RingRows& r : tf.ring_rows) posts += r.klass == kRcPostHi;
        Check(posts == 2 && rf.Stats().rids_refused == 3 && rf.Stats().rids_rows == 37 && rf.Stats().whole_clean == 0 && rf.Stats().unclean_post == 1, "ring.rids_refused", "posts " + std::to_string(posts) + " refused " + std::to_string(rf.Stats().rids_refused));   // 37: 40 slots - 2 refused posts - corner 3's hidden pad
      }
      {   // S111 CAARING review: a RIDS rope row's call is the game's record index, slot - 33 = 3 side + height (sub_825CF2F8 +0x80C poses slot r19 + r25, r19 = 33 + 3 side), not its order among the
          // rope slots that read: with the first rope slot refused, the other eleven keep calls 1..11 (each rope's colour record, car_rope_colours.h RopeColourOf)
        ri::Set set;
        set.frame = 300;
        for (uint32_t k = 5; k <= 44; ++k) set.slots.push_back(mkslot(k, k));
        set.slots[33 - 5].model = 0x8000;   // slot 33 (side 0, height 0): not a model
        ri::The().Publish(std::move(set));
        rr::RingRecomputeContributor rx;
        SceneTables tx;
        latch(rx, tx);
        bool ok = find(tx, kRcRopeHi, 0) == nullptr;
        std::string why = ok ? "" : " call 0 present";
        for (uint32_t call = 1; call < 12; ++call) {
          const RingRows* e = find(tx, kRcRopeHi, call);
          if (!e || e->r5 != 0x41000000u + 0x1000u * (33u + call)) ok = false, why += " call " + std::to_string(call) + (e ? " at another slot" : " missing");
        }
        Check(ok, "ring.rids_rope_record_index", why.empty() ? "calls 1..11 at slots 34..44" : why);
      }
      {   // no copies published for this frame: no rows, counted, never clean
        ri::Set none;
        none.frame = 299;
        ri::The().Publish(std::move(none));
        rr::RingRecomputeContributor rn;
        SceneTables tn;
        latch(rn, tn);
        Check(rn.Stats().rids_rows == 0 && rn.Stats().rids_no_set == 1 && rn.Stats().whole_clean == 0, "ring.rids_no_set", "no_set " + std::to_string(rn.Stats().rids_no_set));
      }
      {   // the canvas: [G + 56]'s palette as it stands, one row, in a normal frame (bit clear) too
        constexpr uint32_t kCv = 0x40A00000, kCvVt = 0x40A10000, kCvPal = 0x40A20000;
        BuildGuest(false);
        if (!Plant("canvas_rest")) {   // the canvas left at rest = the game's model is not read: no row
          g_guest.Put(kG + 56, kCv);
          g_guest.Put(kCv, kCvVt);
          g_guest.Put(kCvVt + 8, 0x82BD3AB8u);
          g_guest.Put(kCv + 296, 1);
          g_guest.Put(kCv + 300, 2);
          g_guest.Put(kCv + 200, kCvPal);
          for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t w = 0; w < 16; ++w) g_guest.PutF(kCvPal + 64 * k + 4 * w, w == 12 ? 321.f + float(k) : w == 13 ? 4.f : w == 14 ? 5.f : (w % 5 == 0 ? 1.f : 0.f));
        }
        rr::RingRecomputeContributor rcv;
        SceneTables tcv;
        latch(rcv, tcv);
        size_t canv = 0;
        for (const RingRows& r : tcv.ring_rows) canv += r.klass == kRcCanvas;
        const RingRows* e = find(tcv, kRcCanvas, 0);
        std::string why;
        const bool ok = canv == 1 && e && e->n_nodes == 2 && e->object == kCv && e->r5 == kCvPal && (e->flags & kRrRecomputed) && T3(tcv.ring_nodes, e, 1, 322.f, 4.f, 5.f, &why) && tcv.ring_rows.size() == 40 && rcv.Stats().canvas_rows == 1;
        Check(ok, "ring.canvas_row", why + " canvas rows " + std::to_string(canv) + " total " + std::to_string(tcv.ring_rows.size()));
      }
      BuildGuest(false);
    }
    g_mode = Plant("mode_ignored") ? 2 : 0;
    rr::RingRecomputeContributor off;
    off.SetReader(ReadThunk);
    off.NoteRing(kX, 600);
    FrameInfo f2;
    f2.guest_frame = 600;
    const bool l2 = off.Latch(f2);
    SceneTables t2;
    Counters c2;
    off.Emit(f2, t2, c2);
    FrameInfo f3;
    f3.guest_frame = 601;
    off.Emit(f3, t2, c2);
    Check(l2 && t2.ring_rows.empty() && off.Stats().frames == 0, "mode.off_reads_nothing", "entries " + std::to_string(t2.ring_rows.size()) + " frames " + std::to_string(off.Stats().frames));
    g_mode = 2;
  }

  // ------------------------------------------------------------ 4b. the rope LOD rule (S103 RLB; RLD.md / RLV.md). THE ORACLE is worked by hand from the geometry, no shipped code: the rope ends (+-288, y, -288) with y = -154 - 42 r,
  // turned by the side: side 0 runs along X at z = -288, side 1 along Z at x = -288, side 2 along X at z = +288, side 3 along Z at x = +288 (every segment spans +-288). LOW = the eye farther than 500 from the segment,
  // or the forward axis within 20 degrees of the rope's line. Rope index = 3 h + r.
  {
    rr::LodParams lp;   // the game's T = {500, 20 degrees, on}
    if (Plant("lod_dist50")) lp.dist = 50.f;           // planted: a distance threshold of 50
    if (Plant("lod_no_angle")) lp.angle_on = false;    // planted: the angle test dropped
    if (Plant("lod_nan_low")) lp.nan_low = true;       // planted: the pre-review NaN rule (a NaN angle was LOW)
    auto view = [](float ex, float ey, float ez, float fx, float fy, float fz) {
      rr::RopeView v;
      v.has = true, v.eye[0] = ex, v.eye[1] = ey, v.eye[2] = ez, v.fwd[0] = fx, v.fwd[1] = fy, v.fwd[2] = fz;
      return v;
    };
    auto mask = [&](const rr::RopeView& v) {   // 12 chars, side-major: 'L' low, 'h' high
      std::string m;
      for (uint32_t h = 0; h < 4; ++h)
        for (uint32_t r = 0; r < 3; ++r) m += rr::RopeIsLow(v, h, r, lp) ? 'L' : 'h';
      return m;
    };
    const float k25 = 0.42261826f, c25 = 0.90630779f, k15 = 0.25881905f, c15 = 0.96592583f;   // sin / cos of 25 and 15 degrees
    // (1) eye far (3000 from every segment), any forward: all 12 low
    const std::string mfar = mask(view(0, -196, -3000, 0, 0, 1));
    Check(mfar == "LLLLLLLLLLLL", "ring.lod_distance", "far eye: " + mfar);
    // (1b) the 500 edge: forward straight up (90 degrees to every rope, the angle test silent). Eye (0, -196, -700): side 0 is 412 away (414 for the ropes 42 above / below): high; sides 1 and 3 are 502.7 / 504 away
    // from their end points: low; side 2 988: low.   Eye (0, -196, -690): sides 1 and 3 are 494.5 / 496.3 away: high.
    const std::string m700 = mask(view(0, -196, -700, 0, 1, 0)), m690 = mask(view(0, -196, -690, 0, 1, 0));
    Check(m700 == "hhhLLLLLLLLL" && m690 == "hhhhhhLLLhhh", "ring.lod_distance", "700: " + m700 + " 690: " + m690);
    // (2) eye at the ring's centre: every segment is 288 away (distance silent). Forward along +Z: the ropes of sides 1 and 3 run along Z (angle 0 / 180): low; sides 0 and 2 run along X (90): high.
    const std::string mz = mask(view(0, -196, 0, 0, 0, 1)), mx = mask(view(0, -196, 0, 1, 0, 0)), m25 = mask(view(0, -196, 0, k25, 0, c25)), m15 = mask(view(0, -196, 0, k15, 0, c15)), mneg = mask(view(0, -196, 0, 0, 0, -1));
    Check(mz == "hhhLLLhhhLLL" && mneg == mz && mx == "LLLhhhLLLhhh" && m25 == "hhhhhhhhhhhh" && m15 == "hhhLLLhhhLLL", "ring.lod_angle", "+Z " + mz + " -Z " + mneg + " +X " + mx + " 25deg " + m25 + " 15deg " + m15);
    // (3) no camera: all 12 high
    rr::RopeView none;
    Check(mask(none) == "hhhhhhhhhhhh", "ring.lod_nocam", mask(none));
    // the class that reaches the rows: AppendRope through the Inputs' view, the clean guest's 9 ropes (side 3 has no descriptors)
    {
      BuildGuest(false);
      rr::Inputs in;
      rr::RingCensus cx;
      rr::ReadInputs(ReadThunk, kX, in, cx);
      in.lod = lp;
      const rr::RopeView views[3] = {view(0, -196, -3000, 0, 0, 1), view(0, -196, 0, 0, 0, 1), rr::RopeView{}};
      uint32_t lo[3] = {}, hi[3] = {};
      for (int k = 0; k < 3; ++k) {
        in.view = views[k];
        std::vector<RingRows> rw;
        std::vector<RingNodeRows> pl;
        for (uint32_t h = 0; h < 4; ++h)
          for (uint32_t r = 0; r < 3; ++r) rr::AppendRope(in, h, r, 300, rw, pl, cx);
        for (const RingRows& e : rw) (e.klass == kRcRopeLo ? lo[k] : hi[k]) += 1;
      }
      Check(lo[0] == 9 && hi[0] == 0 && lo[1] == 3 && hi[1] == 6 && lo[2] == 0 && hi[2] == 9, "ring.lod_class", "far " + std::to_string(lo[0]) + "/" + std::to_string(hi[0]) + " centre +Z " + std::to_string(lo[1]) + "/" + std::to_string(hi[1]) + " none " + std::to_string(lo[2]) + "/" + std::to_string(hi[2]));
    }
    // the contributor end to end: the hook's view reaches the emitted rows (same frame) and the census counts our classes (the game/ours compare is offline: PairLod below)
    {
      BuildGuest(false);
      rr::RingRecomputeContributor con;
      con.SetReader(ReadThunk);
      const rr::RopeView v = view(0, -196, -3000, 0, 0, 1);
      con.NoteRing(kX, 300, &v);
      FrameInfo f;
      f.guest_frame = 300;
      con.Latch(f);
      SceneTables t;
      Counters cc;
      con.Emit(f, t, cc);
      uint32_t lo_rows = 0;
      for (const RingRows& r : t.ring_rows)
        if ((r.flags & kRrRecomputed) && r.klass == kRcRopeLo) ++lo_rows;
      Check(lo_rows == 9 && con.Stats().lod_lo == 9 && con.Stats().lod_hi == 0, "ring.lod_census", "low rows " + std::to_string(lo_rows) + " lod_lo " + std::to_string(con.Stats().lod_lo));
    }
    // NaN (review 3): sub_827405F8 returns HIGH for a NaN angle (fcmpu + blt, then fcmpu + bge: unordered takes bge), and sub_82324380 zeroes a non-finite vector (dot 0 -> pi/2 -> HIGH): NaN / inf forward = HIGH
    {
      const float nan = std::nanf(""), inf = INFINITY;
      const std::string mn = mask(view(0, -196, 0, nan, 0, 1)), mi = mask(view(0, -196, 0, inf, 0, 1)), m0 = mask(view(0, -196, 0, 0, 0, 0));
      Check(mn == "hhhhhhhhhhhh" && mi == mn && m0 == mn, "ring.lod_nan", "NaN fwd " + mn + " inf fwd " + mi + " zero fwd " + m0);
    }
    // PairLod on the real clock (review 1): ours of record j (3 ropes per side, height 0..2, r5 = side buffer) against the game's rope rows of record j + 1 (stamped j). The game's frame is MIXED: on side 1 the
    // ropes are hi, lo, hi; its `call` is a per-model ordinal (960 and 980 count apart: the lo rope is call 0 of its model, the hi ropes are calls 0 and 1), so call == call would pair the wrong ropes.
    {
      std::vector<RingRows> ours, game;
      const uint32_t r5s[2] = {0x1000, 0x2000};
      const uint8_t oc[2][3] = {{kRcRopeHi, kRcRopeHi, kRcRopeHi}, {kRcRopeHi, kRcRopeLo, kRcRopeHi}};   // ours: side 0 all hi, side 1 hi lo hi
      for (uint32_t h = 0; h < 2; ++h)
        for (uint32_t r = 0; r < 3; ++r) {
          RingRows e;
          e.klass = oc[h][r], e.flags = kRrRecomputed, e.frame = 300, e.call = e.keyed = 3 * h + r, e.r5 = r5s[h];
          ours.push_back(e);
        }
      const uint8_t gc[2][3] = {{kRcRopeHi, kRcRopeHi, kRcRopeHi}, {kRcRopeHi, kRcRopeLo, kRcRopeHi}};   // the game's: the same classes, in record order
      const uint32_t gcall[2][3] = {{0, 1, 2}, {0, 0, 1}};   // per-model ordinals: side 1 = hi 0, lo 0, hi 1
      for (uint32_t h = 0; h < 2; ++h)
        for (uint32_t r = 0; r < 3; ++r) {
          RingRows e;
          e.klass = gc[h][r], e.frame = 300, e.call = gcall[h][r], e.keyed = gcall[h][r], e.r5 = r5s[h];
          game.push_back(e);
        }
      std::vector<const RingRows*> gp, op;
      for (const RingRows& e : game) gp.push_back(&e);
      for (const RingRows& e : ours) op.push_back(&e);
      const rr::LodPairing good = rr::PairLod(gp, op, Plant("lod_pair_call"));
      game[4].klass = kRcRopeHi;   // the game's lo rope becomes hi: exactly one pair must differ
      const rr::LodPairing one = rr::PairLod(gp, op, Plant("lod_pair_call"));
      Check(good.eq == 6 && good.diff == 0 && one.eq == 5 && one.diff == 1 && one.unpaired == 0, "ring.lod_pairs", "eq/diff " + std::to_string(good.eq) + "/" + std::to_string(good.diff) + " then " + std::to_string(one.eq) + "/" + std::to_string(one.diff));
    }
  }

  // ------------------------------------------------------------ 5. the record round trip (the entries survive the file the offline applier reads)
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.ring_rows = rows;
    t.ring_nodes = pool;
    if (Plant("lose_flag"))
      for (RingRows& r : t.ring_rows) r.flags = 0;
    const std::string path = "ring_recompute_unit_roundtrip.rsc";
    std::string err;
    SceneTables back;
    bool ok = WriteRecord(path, t, err) && ReadRecord(path, back, err);
    ok = ok && back.ring_rows.size() == rows.size() && back.ring_nodes.size() == pool.size();
    for (size_t i = 0; ok && i < rows.size(); ++i) ok = std::memcmp(&back.ring_rows[i], &rows[i], sizeof(RingRows)) == 0;
    if (ok && !pool.empty()) ok = std::memcmp(back.ring_nodes.data(), pool.data(), pool.size() * sizeof(RingNodeRows)) == 0;
    Check(ok, "record.roundtrip", err);
    std::remove(path.c_str());
  }
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
