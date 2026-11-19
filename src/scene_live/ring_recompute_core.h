// The ring ropes' node rows, recomputed from the rope grid: the pure rules (S103 lane C3; docs\renderer\STAGE_LIVE_WRITERS.md section 3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only; NO game, SDK or renderer-adapter includes: the contributor (ring_recompute_contrib.h) and the offline unit (tools\scene\ring_recompute_unit.cpp) run THESE functions.
// Marks: [C] read in a listing (cited), [L] a reading, [U] unknown (a run names it).
//
// WHY. Under OURS the ring's colour callback (sub_825D0438, which calls sub_825D0340 and sub_825CF2F8) is skipped (SCENE_SKIPS.md row 5), and every writer of the ropes' node rows is inside it. The rows
// the packer reads for a rope draw are the ROPE CONTEXT BUFFER X + 29184 + 8192 h (h = side 0..3): 128 matrices of 64 bytes, of which sub_825CF2F8 rewrites only the TRANSLATION (m[12..14]) of the nodes
// its descriptors name; the rest of every matrix is the ring's static pose. So the recompute is: the buffer as it stands (rest rotation) + our translations.
//
// THE LOOP [C: listing sub_825CF2F8 0x825CF5EC-0x825CFC48; the fill is 0x825CF654-0x825CF8DC]. For h = 0..3 (r24; the global at 0x8362230C + 4 h is r30, unused here), rope r = 0..2 (r25),
//   count = int32 [X + 29124 + 4 h]  (r21, 0x825CF640-0x825CF64C), the descriptor j (r30 = X + 12 * (891 + 128 h + j)): { n = [d+0] (the buffer row; r29 = 128 h is added: row = n + 128 h),
//   a = [d+4] (the side index), b = [d+8] (the segment) },  j = 0 .. count - 1; the same descriptors for the three ropes.
//   dest = X + 29232 + 64 * (n + 128 h) = the translation row of buffer row n of side h  (29232 = 29184 + 48).
//   P(a, b, r) = sub_826C81D8(out, G2, a, r, b) [C 0x826C81D8-0x826C8268]: out = sub_8276DF28(grid layer r, a, b) with out.y := -154 / -196 / -238 for r = 0 / 1 / 2 (the floats 0x8201D38C /
//     0x8201E43C / 0x8201E440). sub_8276DF28 [C 0x8276DF28-0x8276DFE8]: n2 = (vtable slot 0 of the layer = sub_824FCCD8 = `li r3,24`; image word 0x8201B8EC) / 2 = 12;
//     out = (288 - (b * 288) / n2, 0, -288, 1) (floats 0x8200376C = 288.0, 0x82003774 = -288.0, single precision in that order: fmuls, fdivs, fsubs), then sub_824D6D10 turns x / z by a quarter turns
//     [C 0x824D6D10-0x824D6D78]: a = +-2: (-x, -z); a = 1 or -3: (z, -x); a = 3 or -1: (-z, x); anything else: unchanged.
//   GRID MODE ([X + 100] == 0): the record is the grid layer r's: i = 24 a + b, set to 0 when i >= 96 (signed compare, 0x825CF6B4-0x825CF6C0), the 80-byte record at
//     [[G2 + 64 + 4 r] + 440] + 80 i (G2 = the word at 0x83768A00; x y z at +0 +4 +8; 0x825CF698-0x825CF6D0). (The layer is [G2 + 4 (r + 16)]: the same word.)
//   TABLE MODE ([X + 100] != 0): the source is X + 104 + 12 m, m = 64 (3 h + r) + j (r27 = (r19 + r25 - 33) << 6 with r19 = 33 + 3 h; 0x825CF7A0-0x825CF834): x at +0, y at +4, z at +8 of that element.
//   EVEN sides (h = 0, 2: `r24 == 0` or r17 == [sp+80] + 8, 0x825CF6D8-0x825CF6EC): x := src.x - P.x, y := src.y, z := src.z.   ODD sides (h = 1, 3): x := src.x, y := src.y, z := src.z - P.z.
// NOT REPLICATED (counted, [U] until a mode-1 record with rows is compared): the two conditional overrides of rows 2 and 26 (0x825CF8E0-0x825CF96C: written only when the word 0x83767C38 is non-zero and
// sub_82594F70 returns an object; from that object's floats at +48 / +52 / +112 / +116 or +52 / +56 / +116 / +120); the rope LOD (sub_827406A8: a rope whose classifier returns 0 is not filled; "dead in
// practice", ring.md section 3); the view-distance fade of the material block (not rows).
// GATES [C]: X + 96 != 0 (0x825CF30C), the word [0x83767B74] is null or its +368 != 1 (0x825CF338-0x825CF350, the model task's own gate), [0x836F4C74] = G non-null and [G + 20] != 0 (0x825CF354-0x825CF370;
// the rope model exists), the camera slot (sub_82BA7748 non-null). The first three are read here (counted); the camera slot is the walk's own and not an input of the rows.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../renderer_scene/ring_keys.h"   // S103 CRR: the alt ring's post call -> angle table (shared with the key rule)
#include "../renderer_scene/scene_record.h"
#include "arena_world.h"   // RowsFromMatrix

namespace rdr_scene::live::ringrecompute {

inline constexpr uint32_t kRingGlobal = 0x836F4C74;      // G
inline constexpr uint32_t kGridGlobal = 0x83768A00;      // G2
inline constexpr uint32_t kModelGate = 0x83767B74;       // [word] -> +368 == 1 stops the rope pass
inline constexpr uint32_t kOverrideGate = 0x83767C38;    // non-zero: rows 2 / 26 may be overridden by another system (not replicated)
inline constexpr uint32_t kGateOff = 96, kAltOff = 100, kTableOff = 104, kEntriesOff = 10692, kCountsOff = 29124, kBufOff = 29184;
inline constexpr uint32_t kBufStride = 8192, kRowStride = 64, kRows = 128, kSides = 4, kRopes = 3;
inline constexpr uint32_t kLayerOff = 64, kLayerBuf = 440, kRecStride = 80, kRecCount = 96, kPerSide = 24, kMaxDesc = 128, kTableElems = 64;
inline constexpr float kSpan = 288.f, kSegHalf = 12.f;   // n / 2 of sub_8276DF28 (slot 0 of the layer returns 24)
inline constexpr float kRopeY[3] = {-154.f, -196.f, -238.f};

struct Desc {
  int32_t n = 0, a = 0, b = 0;
};

// THE ROPE LOD RULE (S103 RLB; docs/research/s103/RLD.md [C], RLV.md [C]). sub_827406A8 (side, height) -> sub_827405F8: the rope's ends A = (288, y, -288), B = (-288, y, -288), y = -154 - 42 height, both turned by `side`
// quarter turns (sub_824D6D10); LOW (class 1, the 980 model) when T0 < the eye's distance to the segment A-B (sub_8273FFF8), or (T2 != 0) when the angle between the view's forward axis and B - A (sub_827402A0)
// is < T1 or > pi - T1; else HIGH (960). No camera (sub_82BA7748 = 0, 0x82740624) = HIGH. T = {500.0, 0.34906587 rad, 1} (RLD rows 1-4, set once by sub_825CD8D0). Eye = camera +592, forward = camera +608 (world;
// ring-local == world, RLV row 12). Single precision in the listings' order [f32]. The angle's acos is the game's VMX polynomial (sub_823A59A8), ours std::acosf: [L] the same within its error, which only matters
// at an angle within ~1e-4 rad of the 20 degree edge.
struct RopeView {
  bool has = false;      // false = sub_82BA7748 returned 0: every rope HIGH
  float eye[3] = {};
  float fwd[3] = {};
};
struct LodParams {
  float dist = 500.0f;            // [f32] T[0] = [0x8200E834]
  float angle = 0.34906587f;      // [f32] T[1] = [0x8201FAC0] (20 degrees)
  bool angle_on = true;           // T[2] != 0
  bool nan_low = false;           // unit-only plant: the pre-review NaN rule
  float pi = 3.1415927f;          // [f32] [0x82118560]
};

// FUSED OPS (S103 RLB review 4): the listings use fmadds / fmsubs, so the sums below are std::fma in the listing's order. The game's rsqrt-estimate normalize (sub_82324380, vrsqrtefp128 + one
// refinement) is sqrt + divide here: the same value to ~1e-7, which only matters at the 20 degree edge.
// sub_82C327F0: sqrt(fma(z, z, fma(x, x, y*y))) [f32].
inline float Len3(const float v[3]) { return std::sqrt(std::fma(v[2], v[2], std::fma(v[0], v[0], v[1] * v[1]))); }

// sub_824D6D10: x / z turned by `a` quarter turns (the rule BasePoint uses).
inline void TurnQuarter(int32_t a, float& x, float& z) {
  const float ox = x, oz = z;
  switch (a) {
    case 2: case -2: x = -ox, z = -oz; break;
    case 1: case -3: x = oz, z = -ox; break;
    case 3: case -1: x = -oz, z = ox; break;
    default: break;
  }
}

// sub_8273FFF8: the distance from P to the segment A-B. Past the A end (d.pa < 0) |P - A|, past the B end ((A-B).(P-B) < 0) |P - B|, else |d x pa| / |d| with d = B - A, pa = P - A (0 when either length is 0) [f32].
// The cross is the listing's order (0x825 +0x248-+0x258, fmsubs: first * second - third * fourth): x = d.y*pa.z - d.z*pa.y, y = d.z*pa.x - pa.z*d.x, z = pa.y*d.x - d.y*pa.x.
inline float PointToSegment(const float p[3], const float a[3], const float b[3]) {
  const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const float pa[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
  const float dot_a = std::fma(d[1], pa[1], std::fma(d[2], pa[2], d[0] * pa[0]));   // f8@+90
  if (dot_a < 0.0f) return Len3(pa);
  const float ab[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  const float pb[3] = {p[0] - b[0], p[1] - b[1], p[2] - b[2]};
  const float dot_b = std::fma(ab[0], pb[0], std::fma(ab[2], pb[2], ab[1] * pb[1]));   // f2@+14C
  if (dot_b < 0.0f) return Len3(pb);
  const float cross[3] = {std::fma(d[1], pa[2], -(d[2] * pa[1])), std::fma(d[2], pa[0], -(pa[2] * d[0])), std::fma(pa[1], d[0], -(d[1] * pa[0]))};
  const float cl = Len3(cross), dl = Len3(d);
  if (cl == 0.0f || dl == 0.0f) return 0.0f;
  return cl / dl;
}

// sub_82324380 + sub_827402A0: the angle between `fwd` and (B - A), both normalized; a zero or non-finite vector becomes zero (the game's select on a zero / NaN / inf squared length), then acos of
// the dot fma(uy, vy, fma(vx, ux, uz*vz)) [f32]. acos is std::acos (the game's VMX polynomial sub_823A59A8 [L]).
inline void NormalizeGame(float v[3]) {
  const float l2 = std::fma(v[2], v[2], std::fma(v[0], v[0], v[1] * v[1]));
  if (!(l2 > 0.0f) || !std::isfinite(l2)) { v[0] = v[1] = v[2] = 0.0f; return; }
  const float l = std::sqrt(l2);
  v[0] /= l, v[1] /= l, v[2] /= l;
}
inline float AngleBetween(const float fwd[3], const float a[3], const float b[3]) {
  float u[3] = {fwd[0], fwd[1], fwd[2]}, v[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  NormalizeGame(u);
  NormalizeGame(v);
  float c = std::fma(u[1], v[1], std::fma(v[0], u[0], u[2] * v[2]));
  if (c > 1.0f) c = 1.0f;
  if (c < -1.0f) c = -1.0f;
  return std::acos(c);
}

// sub_827406A8 + sub_827405F8 for rope (side h, height r): true = LOW (class 1).
// The game's tests (0x82740650-0x82740690): LOW when `ang < T1` (fcmpu + blt), else HIGH when `pi - T1 >= ang`'s complement fails: (pi - T1) < ang -> LOW. A NaN angle is unordered: both tests are not taken -> HIGH.
inline bool RopeIsLow(const RopeView& v, uint32_t h, uint32_t r, const LodParams& t = LodParams{}) {
  if (!v.has) return false;   // 0x82740624: no camera
  const float y = -(float(r) * 42.0f - -154.0f);
  float a[3] = {288.0f, y, -288.0f}, b[3] = {-288.0f, y, -288.0f};
  TurnQuarter(int32_t(h), a[0], a[2]);
  TurnQuarter(int32_t(h), b[0], b[2]);
  if (t.dist < PointToSegment(v.eye, a, b)) return true;
  if (!t.angle_on) return false;
  const float ang = AngleBetween(v.fwd, a, b);
  if (t.nan_low) return !std::isfinite(v.fwd[0] + v.fwd[1] + v.fwd[2]) || ang < t.angle || (t.pi - t.angle) < ang;   // unit-only plant: the rule before the review (a NaN angle was LOW)
  return ang < t.angle || (t.pi - t.angle) < ang;
}

// ring_lod game/ours (S103 RLB review 1): our rope classes of record j against the game's own rope rows stamped j, which arrive in record j + 1 (the callback reads the frame one ahead: Take's clock comment).
// The game's `call` is a per-model ordinal (960 and 980 count apart), so it cannot pair ropes in a mixed frame: pair by (r5, rank within r5 in record order); ours are written side by side, height 0..2.
// `game` = the game's rope rows (no kRrRecomputed) of the record, `ours` = our rope rows of the record before. Offline (tools\scene\ring_compare.cpp); the live contributor sees neither.
struct LodPairing {
  uint64_t eq = 0, diff = 0, unpaired = 0;
};
inline LodPairing PairLod(const std::vector<const RingRows*>& game, const std::vector<const RingRows*>& ours, bool by_call = false, void (*each)(const RingRows&, const RingRows&) = nullptr) {
  LodPairing out;
  for (size_t gi = 0; gi < game.size(); ++gi) {
    const RingRows& g = *game[gi];
    size_t rank = 0;
    for (size_t k = 0; k < gi; ++k) rank += game[k]->r5 == g.r5;
    const RingRows* match = nullptr;
    size_t seen = 0;
    for (const RingRows* o : ours) {
      if (by_call ? (o->call == g.call && o->r5 == g.r5) : (o->r5 == g.r5 && seen++ == rank)) { match = o; break; }
    }
    if (!match) { ++out.unpaired; continue; }
    ((match->klass == g.klass) ? out.eq : out.diff)++;
    if (each) each(g, *match);
  }
  return out;
}

// One ring's inputs as the game keeps them.
struct Inputs {
  uint32_t x = 0;
  bool gates_ok = false, alt = false, override_gate = false, grid_read = false;
  int32_t count[kSides] = {};
  std::vector<Desc> desc[kSides];
  std::vector<float> layer[kRopes];   // 96 records x (x, y, z); empty = unreadable
  std::vector<float> table;           // alt mode: 12 * 64 elements x (x, y, z); empty = unreadable
  std::vector<float> rest[kSides];    // the buffer's matrices as they stand: 16 floats per row, rows 0 .. max row + 1 (host order)
  RopeView view;                      // S103 RLB: the draw thread's camera at the ring hook (the rope LOD rule's eye and forward)
  LodParams lod;                      // the game's T block (defaults = the game's; a unit test plants a fault here)
};

struct RingCensus {
  uint64_t frames = 0, rows = 0, ropes = 0, no_ring = 0, gate_closed = 0, unreadable = 0, bad_count = 0, bad_index = 0, override_gate_set = 0, alt_frames = 0, nodes_total = 0, no_call = 0, noted_calls = 0, tb_rows = 0, tb_bad_index = 0, tb_table_mode = 0, tb_unreadable = 0, tb_gate = 0, tb_rids = 0, tb_nopose = 0, rope_rids = 0;
  // S103 C4: covers, posts, skirts and the whole-ring verdict (bit 6)
  uint64_t cv_rows = 0, cv_bad = 0, cv_unreadable = 0, cv_gate = 0, cv_table_mode = 0, cv_alt_ring = 0, cv_hidden = 0, cv_lod_both = 0, cv_rids = 0, post_rows = 0, post_unreadable = 0, post_lod_both = 0, post_alt_ring = 0, skirt_rows = 0, skirt_unreadable = 0, skirt_vis_unreadable = 0, skirt_vis_none = 0;
  uint64_t whole_clean = 0, whole_unclean = 0, unclean_rope = 0, unclean_tb = 0, unclean_cover = 0, unclean_post = 0, unclean_skirt = 0, unclean_override = 0;
  uint64_t lod_lo = 0, lod_hi = 0, lod_nocam = 0;   // S103 RLB: our rope LOD class counts (the game/ours compare is offline: PairLod, tools\\scene\\ring_compare.cpp)
  uint64_t delta[9] = {};   // noted hook frame - the latched hook frame: -3..+3 in 0..6, other 7, never noted 8 (the clock question of run 13)
};

// P(a, b, r): the rope's rest point on the ring edge (sub_826C81D8). Single precision in the game's operation order.
inline void BasePoint(int32_t a, int32_t b, uint32_t r, float out[3]) {
  const float f0 = kSpan;
  const float t = (float(b) * f0) / kSegHalf;
  float x = f0 - t, z = -kSpan;
  float nx = x, nz = z;
  switch (a) {
    case 2: case -2: nx = -x, nz = -z; break;
    case 1: case -3: nx = z, nz = -x; break;
    case 3: case -1: nx = -z, nz = x; break;
    default: break;
  }
  out[0] = nx;
  out[1] = kRopeY[r < 3 ? r : 2];   // sub_826C81D8: 0 -> -154, 1 -> -196, else -238
  out[2] = nz;
}

// The translation of one descriptor of rope r on side h; false = no source (bad index / unreadable), counted by the caller.
inline bool NodeTranslation(const Inputs& in, uint32_t h, uint32_t r, uint32_t j, float t[3], RingCensus& cn) {
  const Desc& d = in.desc[h][j];
  float p[3];
  BasePoint(d.a, d.b, r, p);
  const float* src = nullptr;
  float tmp[3];
  if (!in.alt) {
    if (in.layer[r].size() < size_t(kRecCount) * 3) { ++cn.unreadable; return false; }
    int64_t i = int64_t(24) * d.a + d.b;
    if (i < 0) { ++cn.bad_index; return false; }       // the game would index before the buffer
    if (i >= int64_t(kRecCount)) i = 0;                 // 0x825CF6B4-0x825CF6C0
    src = &in.layer[r][size_t(i) * 3];
  } else {
    const size_t m = size_t(kTableElems) * (3 * h + r) + j;
    if (in.table.size() < (m + 1) * 3) { ++cn.unreadable; return false; }
    tmp[0] = in.table[m * 3], tmp[1] = in.table[m * 3 + 1], tmp[2] = in.table[m * 3 + 2];
    src = tmp;
  }
  if ((h & 1u) == 0) t[0] = src[0] - p[0], t[1] = src[1], t[2] = src[2];   // even sides: x relative
  else t[0] = src[0], t[1] = src[1], t[2] = src[2] - p[2];                 // odd sides: z relative
  return true;
}

// One rope's palette (side h, rope r): the buffer's rest matrices with the descriptors' translations written. `nodes` gets (max row + 1) matrices of 16 floats. Returns the node count (0 = nothing).
inline uint32_t RopeNodes(const Inputs& in, uint32_t h, uint32_t r, std::vector<float>& nodes, RingCensus& cn) {
  nodes.clear();
  if (!in.gates_ok) { ++cn.gate_closed; return 0; }
  const int32_t count = in.count[h];
  if (count <= 0 || size_t(count) > in.desc[h].size()) { if (count != 0) ++cn.bad_count; return 0; }
  uint32_t rows = 0;
  for (int32_t j = 0; j < count; ++j) {
    const int32_t n = in.desc[h][size_t(j)].n;
    if (n < 0 || uint32_t(n) >= kRows) { ++cn.bad_count; return 0; }
    rows = std::max(rows, uint32_t(n) + 1);
  }
  nodes.assign(size_t(rows) * 16, 0.f);
  for (uint32_t k = 0; k < rows; ++k) {   // the static pose; identity where the buffer was not readable
    if (in.rest[h].size() >= size_t(k + 1) * 16) std::memcpy(&nodes[size_t(k) * 16], &in.rest[h][size_t(k) * 16], 64);
    else nodes[size_t(k) * 16 + 0] = nodes[size_t(k) * 16 + 5] = nodes[size_t(k) * 16 + 10] = nodes[size_t(k) * 16 + 15] = 1.f;
  }
  for (int32_t j = 0; j < count; ++j) {
    float t[3];
    if (!NodeTranslation(in, h, r, uint32_t(j), t, cn)) continue;
    float* m = &nodes[size_t(in.desc[h][size_t(j)].n) * 16];
    m[12] = t[0], m[13] = t[1], m[14] = t[2];
  }
  ++cn.ropes;
  cn.nodes_total += rows;
  return rows;
}

// The RingRows entry for rope (h, r) and its node rows (12 floats a node, pool order = buffer row). `call` = 3 h + r.
inline void AppendRope(const Inputs& in, uint32_t h, uint32_t r, uint32_t frame, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  std::vector<float> nodes;
  const uint32_t n = RopeNodes(in, h, r, nodes, cn);
  if (n == 0) return;
  RingRows e;
  const bool low = RopeIsLow(in.view, h, r, in.lod);   // S103 RLB: the game's rule (RLD / RLV); the low model is Resolve's to find (it draws 960 while 980 is not loaded)
  e.klass = low ? kRcRopeLo : kRcRopeHi;
  if (low) ++cn.lod_lo; else ++cn.lod_hi;
  if (!in.view.has) ++cn.lod_nocam;
  e.object = 0;
  e.frame = frame;
  e.call = e.keyed = 3 * h + r;
  e.r5 = in.x + kBufOff + kBufStride * h;
  e.first_node = uint32_t(pool.size());
  e.n_nodes = uint16_t(std::min<uint32_t>(n, kRingMaxNodes));
  e.flags = kRrRecomputed | (n > kRingMaxNodes ? kRrCapped : 0u);
  for (uint32_t k = 0; k < e.n_nodes; ++k) {
    RingNodeRows nr;
    arenaworld::RowsFromMatrix(&nodes[size_t(k) * 16], nr.rows);
    pool.push_back(nr);
  }
  rows.push_back(e);
  ++cn.rows;
}

// ======================================================================================================================================================================================================
// TURNBUCKLES (S103 lane C3 step 2): sub_825CE250, 12 instances (4 corners c x 3 heights h) of ONE model T = [G + 44], posed from the same rope grid.
// [C listing 0x825CE324-0x825CE57C + run 16's records (class turnbuckle, call = 3 c + h, 4 palette nodes, r5 = [T + 200])]:
//   world A (node idxA = [X + 127792]):  Y rotation R(a_c) = row0 (cos a, 0, -sin a), row2 (sin a, 0, cos a) [row-vector form, checked against the rows: sub_82C32DE0], a_c = the float at 0x8201FA60 + 4 c;
//        translation (V_c.x, y_h, V_c.z), V_c = the vec at 0x8201FA20 + 16 c, y_h = f30 - h * f31 = -154 - 42 h (floats 0x8201D38C, 0x82003504; 0x825CE540 fnmsubs).
//   world B (node idxB = [X + 127796]):  the same rotation, translation = the grid record i = 24 (c + 1) of layer h (the loop counter r21 starts at 24 and steps 24; i >= 96 -> 0, the signed test), x y z at +0 +4 +8.
//   The palette the packer reads is [T + 200] after the vtable +84 update: node k = inverse bind [T + 224 + 64 k] x world k (the rule of stage_fx_core.h SkinOf); nodes the loop does not write keep the
//   palette as it stands (identity in run 16: nodes 0 and 3).
//   [C run 16, 12 of 12 instances, to 1e-4]: node 1's skin translation = A's translation + inv.t * R with ONE constant inv.t = (331.9335, 0, 320.0000): the formula, the angle and vec tables, the height step.
//   [U] the node indices idxA / idxB are read from X + 127792 / 127796 (assumed 1 and 2 by the dump); the inverse binds, the palette rows and idx are read live (a mode-1 run's compare settles them).
// Table mode ([X + 100] != 0: the matrices come from RIDS, sub_82595C50) is NOT replicated: counted `tb_table_mode`, no rows.
// ======================================================================================================================================================================================================
inline constexpr uint32_t kTbModelOff = 44, kPaletteOff = 200, kInvBindOff = 224, kTbIdxA = 127792, kTbIdxB = 127796;
inline constexpr uint32_t kTbVecTable = 0x8201FA20, kTbAngleTable = 0x8201FA60, kTbStep = 0x82003504, kTbHeight0 = 0x8201D38C;
inline constexpr uint32_t kRidsByteWord = 0x837632F8;   // the word holding the byte 0x837632FA: bit 0 = RIDS playback (the ropes / turnbuckles go straight to submit, no posing: 0x825CE338-0x825CE358, 0x825CFAD8)
inline constexpr uint32_t kTbCorners = 4, kTbHeights = 3, kTbMaxNodes = 8;

struct TbInputs {
  bool ok = false;
  uint32_t model = 0, palette = 0;
  uint32_t n = 0;                 // palette nodes
  int32_t idxA = -1, idxB = -1;
  float vec[kTbCorners][3] = {};
  float ang[kTbCorners] = {};
  float step = 0.f, h0 = 0.f;
  std::vector<float> inv, pal;    // n * 16 floats each (host order)
  float rec[kTbHeights][kTbCorners][3] = {};
  bool rec_ok[kTbHeights] = {};
};

// skin = inverse bind x world (row-vector: out row i = sum_k inv[i][k] * world.row(k); stage_fx_core.h SkinOf).
inline void TbSkin(const float world[16], const float inv[16], float out[16]) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += inv[i * 4 + k] * world[k * 4 + j];
      out[i * 4 + j] = s;
    }
}
inline void TbWorld(float a, const float t[3], float m[16]) {
  const float c = std::cos(a), s = std::sin(a);
  const float v[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, t[0], t[1], t[2], 1};
  std::memcpy(m, v, sizeof v);
}

// The palette nodes of instance (corner c, height h): `nodes` = n * 16 floats. false = no row (counted by the caller).
inline bool TurnbuckleNodes(const TbInputs& in, uint32_t c, uint32_t h, std::vector<float>& nodes, RingCensus& cn) {
  if (!in.ok || in.n == 0 || c >= kTbCorners || h >= kTbHeights) return false;
  if (in.idxA < 0 || in.idxB < 0 || uint32_t(in.idxA) >= in.n || uint32_t(in.idxB) >= in.n) { ++cn.tb_bad_index; return false; }
  if (!in.rec_ok[h]) { ++cn.tb_unreadable; return false; }
  nodes = in.pal;
  float ta[3] = {in.vec[c][0], in.h0 - float(h) * in.step, in.vec[c][2]};   // fnmsubs: f30 - (float)h * f31
  float tbv[3] = {in.rec[h][c][0], in.rec[h][c][1], in.rec[h][c][2]};
  float wa[16], wb[16];
  TbWorld(in.ang[c], ta, wa);
  TbWorld(in.ang[c], tbv, wb);
  TbSkin(wa, &in.inv[size_t(in.idxA) * 16], &nodes[size_t(in.idxA) * 16]);
  TbSkin(wb, &in.inv[size_t(in.idxB) * 16], &nodes[size_t(in.idxB) * 16]);
  return true;
}
// The grid record index of corner c: 24 (c + 1), 0 when >= 96.
inline uint32_t TbRecordIndex(uint32_t c) {
  const uint32_t i = 24 * (c + 1);
  return i >= kRecCount ? 0u : i;
}

inline void AppendTurnbuckle(const TbInputs& in, uint32_t c, uint32_t h, uint32_t frame, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  std::vector<float> nodes;
  if (!TurnbuckleNodes(in, c, h, nodes, cn)) return;
  RingRows e;
  e.klass = kRcTurnbuckle;
  e.object = in.model;
  e.frame = frame;
  e.call = e.keyed = 3 * c + h;
  e.r5 = in.palette;
  e.first_node = uint32_t(pool.size());
  e.n_nodes = uint16_t(std::min<uint32_t>(in.n, kRingMaxNodes));
  e.flags = kRrRecomputed;
  for (uint32_t k = 0; k < e.n_nodes; ++k) {
    RingNodeRows nr;
    arenaworld::RowsFromMatrix(&nodes[size_t(k) * 16], nr.rows);
    pool.push_back(nr);
  }
  rows.push_back(e);
  ++cn.tb_rows;
}

// ======================================================================================================================================================================================================
// COVERS, POSTS, SKIRTS (S103 lane C4). Every address below is from a listing (tools\analysis\list_function.py), cited.
//
// COVERS = sub_825CE668 (called by sub_825D0340 at 0x825D0410, r4 = the task arg = 0 in the colour pass, 0x825D0534). 4 corners r25 x 3 heights r28 = 12 instances of ONE model (the cover LOD, below),
// call = 3 r25 + r28 (the game's RingRows order). [C listing 0x825CE668-0x825CECE0; fit against run 16 in tools\scene\ring_cover_fit_check.py]
//   model pick (0x825CE738-0x825CE7E0): alt-ring flag sub_822A7EB0 = (arena id in 200..249, the word at 0x8376AB34 = [0x8376AAF4 + 64], sub_827D7578 / sub_822AA2F8 0x822AA2F8-0x822AA30C); normal ring:
//     sub_827403E8(X + 0x1F320, r25) = [G + 36] when [G + 40] == 0, else G[9 + far] (a view-distance test, the cover LOD: far needs the view position; [U]). The alt ring (82740550) is NOT replicated.
//   per instance (grid mode [X + 100] == 0, 0x825CE928-0x825CE99C): the descriptors d = 3 r25 + k, k < count = int32 [X + 127728 + 4 r25] (0x825CE7E8-0x825CE7F4: r14 = (r25 + 31932) << 2): word n at
//     X + 127584 + 12 d, side s at +127588, c at +127592 (S103 C6: r16 = (lis 1) | 62056 = 0x1F268 = 127592, r29 = X + 36 r25 + 127592 and -8 / -4 / 0 of it, 0x825CE714-0x825CE724 + 0x825CE824; C4 had read 62056 alone and so read 65536 bytes too low; the init that fills them is sub_825CD8D0 0x825CDF50-0x825CDFB8: side := r4, c := c mod 24).
//     angle f31 = fmadds((float)((r25 + 1) % 4), f29, f26), once wrapped (> f28: - f25; < f23: + f25): 0x825CE8A0-0x825CE8D4; f29 = pi/2 [0x82000ADC], f26 = 3 pi/2 [0x8201D4D0], f28 = pi [0x82118560], f23 = -pi
//     [0x82000A94], f25 = 2 pi [0x82000A98] (image words). The matrix at row n of [model + 180] gets sub_82C32DE0(angle): a Y rotation (m0 = cos, m2 = -sin, m8 = sin, m10 = cos, m5 = m15 = 1, the rest 0), the angle wrapped
//     again inside (0x82C32DFC-0x82C32E48), then the TRANSLATION 16 bytes = grid record i = 24 ((s + 1) % 4) + c of layer r28 (i >= 96 -> 0, the signed idiom 0x825CE944-0x825CE984), x y z at +0 +4 +8.
//     Third layer (r28 == 2): if the word X + 9456 + 4 r25 (0x825CEA54-0x825CEA64) is non-zero the translation is the 16 bytes at X + 9392 + 16 r25 instead (0x825CEA68-0x825CEA84).
//   then 0x825CEB38-0x825CEB90: alpha f0 = 1.0 (r4 == 0) [0x8218B8C0]; r28 == 2: f0 *= float X[2342 + r25] (X + 9368 + 4 r25); f0 <= 0.0 [0x8218B8CC]: the instance is NOT posed and NOT submitted (0x825CEB64-0x825CEB68);
//     else [model + 252] = min(f0, 1). Grid mode: vtable +84 (the pose: skin = inverse bind x world, ring.md 2.1) then +52 (the submit, which is what the game's RingRows copy at the call).
//   Run 16 (tools\scene\ring_cover_fit_check.py): cover node 2's skin translation = grid record + inv.t * R(angle) with ONE constant inv.t (284.997, -0.0001, 285.0) to 1e-4 for all 12 instances, record side
//     (r25 + 1) % 4, c = 0, angle 0 / pi/2 / pi / -pi/2 for r25 = 0..3; nodes 0 / 1 identity.
// NOT replicated, counted: table mode ([X + 100] != 0: RIDS copy), RIDS playback (byte 0x837632FA bit 0), the alt ring, the cover LOD's far test (both models' entries are given when [G + 40] != 0: cv_lod_both).
// ======================================================================================================================================================================================================
inline constexpr uint32_t kCvCountOff = 127728, kCvDescOff = 127584, kCvDescStride = 12, kCvAlphaOff = 9368, kCvOvrFlagOff = 9456, kCvOvrVecOff = 9392, kCvModelHi = 36, kCvModelLo = 40;
inline constexpr uint32_t kCvAngBase = 0x8201D4D0, kCvAngStep = 0x82000ADC, kCvPi = 0x82118560, kCvNegPi = 0x82000A94, kCvTwoPi = 0x82000A98;
inline constexpr uint32_t kArenaIdWord = 0x8376AB34;   // [0x8376AAF4 + 64] (sub_827D7578 + 64): the arena id; 200..249 = the alt ring (sub_822AA2F8)
inline constexpr uint32_t kCvCorners = 4, kCvLayers = 3, kCvMaxDesc = 3, kCvModels = 2;

struct CvDesc {
  int32_t n = 0, side = 0, c = 0;
};
struct ModelPal {   // one model's palette and inverse binds as the latch read them
  uint32_t model = 0, palette = 0, n = 0;
  std::vector<float> inv, pal;   // n * 16 floats each (host order)
};
struct CvInputs {
  bool ok = false;
  ModelPal models[kCvModels];   // [0] = [G + 36] (high), [1] = [G + 40] (low; model == 0 when absent)
  int32_t count[kCvCorners] = {};
  std::vector<CvDesc> desc[kCvCorners];
  float alpha_mul[kCvCorners] = {1.f, 1.f, 1.f, 1.f};
  bool ovr[kCvCorners] = {};
  bool alt = false;   // S103 CRR fix: a created arena (id 200..249): sub_825CE668 0x825CE848-0x825CE89C replaces the angle and the record side per call corner r25 (kAltCoverAngle / kAltCoverSide)
  float ovr_vec[kCvCorners][3] = {};
  float ang_base = 0.f, ang_step = 0.f, pi = 0.f, neg_pi = 0.f, two_pi = 0.f;
};

// sub_82C32DE0's own angle wrap (0x82C32DFC-0x82C32E48), single precision.
inline float WrapAngle32DE0(float a, float pi, float neg_pi, float two_pi) {
  if (a > pi) {
    a = a - two_pi;
    if (!(a >= neg_pi)) a = neg_pi;
  } else if (!(a >= neg_pi)) {
    a = a + two_pi;
    if (!(a <= pi)) a = pi;
  }
  return a;
}
// The corner's rotation angle before sub_82C32DE0 (0x825CE8A0-0x825CE8D4).
// S103 CRR fix [C, listing sub_825CE668 0x825CE848-0x825CE89C: sub_822A7EB0 true -> r25 = 0: f29 / side 2; 1: f28 / side 3; 2: f24 / side r18 = 0; 3: f27 / side 1]: the alt ring poses its covers with the angle
// {pi/2 (0x82000ADC), pi (0x82118560), -pi/2 (0x82004D50), 0 (0x8218B8CC)} and the record side {2, 3, 0, 1} per call corner r25 (no wrap); alpha X[2342 + r25] and the override stay on r25.
// The corner at angle p * pi/2 therefore holds model 1070 + p (far 1074 + p) (ring_keys.h kAltCoverHi).
inline constexpr int32_t kAltCoverSide[4] = {2, 3, 0, 1};
inline float AltCoverAngle(const CvInputs& in, uint32_t r25) {
  const float a[4] = {in.ang_step, in.pi, -in.ang_step, 0.f};
  return a[r25 & 3u];
}
inline float CoverAngle(const CvInputs& in, uint32_t r25) {
  const uint32_t r20 = (r25 + 1) % 4;
  float a = std::fmaf(float(r20), in.ang_step, in.ang_base);   // fmadds
  if (a > in.pi) a = a - in.two_pi;
  else if (a < in.neg_pi) a = a + in.two_pi;
  return a;
}
// The nodes (n * 16 floats) of cover instance (r25, r28) of model `mi`; false = no instance (hidden / unreadable / bad), counted. `alpha` = [model + 252].
inline bool CoverNodes(const CvInputs& in, const Inputs& grid, uint32_t mi, uint32_t r25, uint32_t r28, std::vector<float>& nodes, float& alpha, RingCensus& cn) {
  if (!in.ok || mi >= kCvModels || r25 >= kCvCorners || r28 >= kCvLayers) return false;
  const ModelPal& m = in.models[mi];
  if (!m.model) return false;   // S103 C5: the model is simply absent (no low cover, or ReadCoverInputs did not read it): no rows, NOT a read failure
  if (m.n == 0 || m.inv.size() < size_t(m.n) * 16 || m.pal.size() < size_t(m.n) * 16) { ++cn.cv_unreadable; return false; }
  float f0 = 1.f;
  if (r28 == 2) f0 = in.alpha_mul[r25] * f0;
  if (!(f0 > 0.f)) { ++cn.cv_hidden; return false; }   // 0x825CEB64: not posed, not submitted
  alpha = f0 > 1.f ? 1.f : f0;
  const int32_t count = in.count[r25];
  if (count < 0 || count > int32_t(kCvMaxDesc) || size_t(count) > in.desc[r25].size()) { ++cn.cv_bad; return false; }
  if (count > 0 && grid.layer[r28].size() < size_t(kRecCount) * 3) { ++cn.cv_unreadable; return false; }
  nodes = m.pal;
  const float ang = WrapAngle32DE0(in.alt ? AltCoverAngle(in, r25) : CoverAngle(in, r25), in.pi, in.neg_pi, in.two_pi);   // (the alt angles are in range: the wrap is the identity)
  for (int32_t k = 0; k < count; ++k) {
    const CvDesc& d = in.desc[r25][size_t(k)];
    if (d.n < 0 || uint32_t(d.n) >= m.n) { ++cn.cv_bad; return false; }
    const int32_t s1 = in.alt ? kAltCoverSide[r25] : (d.side + 1) % 4;   // srawi / addze: truncating division (alt: r30 = 2, 3, 0, 1 by r25, 0x825CE870-0x825CE89C)
    const int64_t i = int64_t(24) * s1 + d.c;
    if (i < 0) { ++cn.cv_bad; return false; }
    const size_t ri = i >= int64_t(kRecCount) ? 0u : size_t(i);
    float t[3] = {grid.layer[r28][ri * 3], grid.layer[r28][ri * 3 + 1], grid.layer[r28][ri * 3 + 2]};
    if (r28 == 2 && in.ovr[r25]) t[0] = in.ovr_vec[r25][0], t[1] = in.ovr_vec[r25][1], t[2] = in.ovr_vec[r25][2];
    float w[16];
    TbWorld(ang, t, w);
    TbSkin(w, &m.inv[size_t(d.n) * 16], &nodes[size_t(d.n) * 16]);
  }
  return true;
}
inline void AppendCover(const CvInputs& in, const Inputs& grid, uint32_t mi, uint32_t r25, uint32_t r28, uint32_t frame, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  std::vector<float> nodes;
  float alpha = 1.f;
  if (!CoverNodes(in, grid, mi, r25, r28, nodes, alpha, cn)) return;
  const ModelPal& m = in.models[mi];
  RingRows e;
  e.klass = mi == 0 ? kRcCoverHi : kRcCoverLo;
  e.object = m.model;
  e.frame = frame;
  e.call = e.keyed = 3 * r25 + r28;
  e.r5 = m.palette;
  e.first_node = uint32_t(pool.size());
  e.n_nodes = uint16_t(std::min<uint32_t>(m.n, kRingMaxNodes));
  e.flags = kRrRecomputed;
  e.alpha = alpha;
  for (uint32_t k = 0; k < e.n_nodes; ++k) {
    RingNodeRows nr;
    arenaworld::RowsFromMatrix(&nodes[size_t(k) * 16], nr.rows);
    pool.push_back(nr);
  }
  rows.push_back(e);
  ++cn.cv_rows;
}

// ======================================================================================================================================================================================================
// POSTS = sub_825CCFE0 loop 1 (i = 0..3), model [G + 48] (high) or [G + 52] (low, far from the view; the test is the same view-distance rule as the covers, [U]).
// [C listing: docs\systems\ring.md section 2 (0x825CD088-0x825CD230): sub_82C32DE0(model + 32, i * [0x82000ADC]) writes the Y rotation into the model's ROOT matrix, vtable +84 poses]. The palette of every node
// is then that rotation (run 16: 4 calls a frame, 3 nodes, all three rows equal RotY(i pi/2), zero translation, for all 383 records; tools\scene\ring_post_static_check.py compares them with this rule bit for bit).
// So the post rows have NO input from the game: the angle table alone. Counted not replicated: table mode (X + 100: RIDS copy), RIDS playback, the alt ring (2 posts).
// ======================================================================================================================================================================================================
inline constexpr uint32_t kPostModelHi = 48, kPostModelLo = 52, kPostCorners = 4, kPostModels = 2;
struct PostInputs {
  bool ok = false;
  bool alt = false;               // S103 CRR: a created arena (id 200..249): 8 calls, angle indices ringkeys::kAltPostAngle (CRK section 2), every post model has the same 3-node skeleton [C files, CRK K2]
  ModelPal models[kPostModels];   // palette / inverse bind are not needed (the rotation is the whole row); n only
  float step = 0.f, pi = 0.f, neg_pi = 0.f, two_pi = 0.f;
};
// The posts one frame carries: 4 (i = angle index 0..3) or, in a created arena, 8 (call i -> angle index kAltPostAngle[i]: loop 1 corners {1, 2}; loop 2 h 0 {0, -1}, h 1 {1, 2}, h 2 {0, -1}; [L] the exact float chain after 0x825CD3A0).
inline uint32_t PostCalls(const PostInputs& in) { return in.alt ? ::rdr_scene::rf::c::ringkeys::kAltPostCalls : kPostCorners; }
inline void PostNodes(const PostInputs& in, uint32_t i, uint32_t n, std::vector<float>& nodes) {
  const int32_t idx = in.alt ? ::rdr_scene::rf::c::ringkeys::kAltPostAngle[i & 7u] : int32_t(i);
  const float ang = WrapAngle32DE0(float(idx) * in.step, in.pi, in.neg_pi, in.two_pi);   // fmuls
  const float zero[3] = {0.f, 0.f, 0.f};
  float w[16];
  TbWorld(ang, zero, w);
  nodes.assign(size_t(n) * 16, 0.f);
  for (uint32_t k = 0; k < n; ++k) std::memcpy(&nodes[size_t(k) * 16], w, sizeof w);
}
inline void AppendPost(const PostInputs& in, uint32_t mi, uint32_t i, uint32_t frame, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  if (!in.ok || mi >= kPostModels || i >= PostCalls(in) || !in.models[mi].model || in.models[mi].n == 0) return;
  const ModelPal& m = in.models[mi];
  std::vector<float> nodes;
  PostNodes(in, i, m.n, nodes);
  RingRows e;
  e.klass = mi == 0 ? kRcPostHi : kRcPostLo;
  e.object = m.model;
  e.frame = frame;
  e.call = e.keyed = i;
  e.r5 = m.palette;
  e.first_node = uint32_t(pool.size());
  e.n_nodes = uint16_t(std::min<uint32_t>(m.n, kRingMaxNodes));
  e.flags = kRrRecomputed;
  e.alpha = 1.f;
  for (uint32_t k = 0; k < e.n_nodes; ++k) {
    RingNodeRows nr;
    arenaworld::RowsFromMatrix(&nodes[size_t(k) * 16], nr.rows);
    pool.push_back(nr);
  }
  rows.push_back(e);
  ++cn.post_rows;
}

// ======================================================================================================================================================================================================
// SKIRTS: the four 66-node skirt skeletons G[332..344] MOVE (run 16: the rows differ record to record; p50 0.2 / p90 1.8 units, ring.md B4). Their pose is NOT inside the skipped ring callback: sub_82609670 (update side)
// calls the skirt sim sub_8274EC60 and then sub_82609060 (vtable +84 over the sim's local matrices, 0x8260925C), so the pose runs under OURS and [skirt + 200] holds it at the latch: the skirt rows are READ,
// not recomputed (ring.md section 5). One entry per model (call = keyed = 0, like the game's skirt calls, which are one per object); the sides the game does not draw this frame are given too (the consumer
// decides by the visible-side rule, [U]).
// ======================================================================================================================================================================================================
inline constexpr uint32_t kSkirtFirst = 332, kSkirtCount = 4, kSkirtMaxNodes = 96;
// S103 C5: WHICH skirts the game poses and draws (sub_82609060 0x82609150-0x826091C0, read from the listing; ring.md section 5). The loop runs i = 0..3: the state word w = [X + 1248 + 4 i], the side s = the word at
// 0x82020A94 + 4 i (a constant table), and the model is G[332 + 4 s]. A skirt is skipped when w == 1; otherwise, with RIDS playback off, it is posed when (the word at 0x836F3928 & 3, S103 C6: was typed 0x8376...) == s, else only when s is not the
// side facing away from the camera: quad = sub_82534768(cam + 144) (cam = [P + 24] + 272 * [P + 32], P = [0x836F3AA8] (S103 C6: lis -31889 = 0x836F, C5 had typed 0x8376), the floats at +0 and +8), hidden side = {2, 3, 0, 1}[quad] (0x826091A4-0x826091B8). The game
// leaves the function at once (nothing posed) when P, [P + 16] or any of the four models is 0. [X is the pointer the caller passes in r3 of sub_82609060: taken to be the ring object [U]; the quadrant read at the latch,
// not at the call [U].]
inline constexpr uint32_t kSkirtStateOff = 1248, kSkirtSideTable = 0x82020A94u, kSkirtViewGlobal = 0x836F3AA8u, kSkirtDrawWord = 0x836F3928u, kQuadThreshold = 0x8218B8CCu;
inline constexpr uint32_t kSkirtHiddenSide[4] = {2, 3, 0, 1};
// sub_82534768 (0x82534768-0x825347D0): the dominant axis of (a = [r3 + 0], b = [r3 + 8]) as 0..3; thr = the float at 0x8218B8CC.
inline uint32_t QuadrantOf(float a, float b, float thr) {
  const float fb = b < thr ? -b : b, fa = a < thr ? -a : a;
  if (fb > fa) return b > thr ? 2u : 0u;
  return a > thr ? 3u : 1u;
}
struct SkirtVisIn {
  bool ok = false;           // false = an input was unreadable (counted): no verdict
  bool early_exit = false;   // the game's function returns before the loop: nothing is posed
  bool rids = false;
  uint32_t state[4] = {};    // [X + 1248 + 4 i]
  uint32_t side[4] = {};     // 0x82020A94 + 4 i
  uint32_t draw_word = 0;    // the word at 0x836F3928
  uint32_t quad = 99;        // QuadrantOf(...); > 3 = no hidden side
};
// The bit s of the result is set when the model of side s is posed this frame.
inline uint32_t SkirtVisibleMask(const SkirtVisIn& v) {
  if (!v.ok || v.early_exit) return 0;
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (v.state[i] == 1) continue;
    const uint32_t s = v.side[i];
    if (s >= 4) continue;
    if (!v.rids && (v.draw_word & 3u) != s && v.quad <= 3 && kSkirtHiddenSide[v.quad] == s) continue;
    mask |= 1u << s;
  }
  return mask;
}
inline uint32_t PopCount4(uint32_t m) { return (m & 1u) + ((m >> 1) & 1u) + ((m >> 2) & 1u) + ((m >> 3) & 1u); }

// A model that is absent has no entry and is not counted (ReadSkirts counts a real read failure once, as an unreadable model).
inline void AppendSkirt(const ModelPal& m, uint32_t idx, uint32_t frame, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  if (!m.model) return;   // S103 C5: not read / not visible / absent: ReadSkirts already counted a real failure
  if (m.n == 0 || m.pal.size() < size_t(m.n) * 16 || m.n > kSkirtMaxNodes) { ++cn.skirt_unreadable; return; }
  RingRows e;
  e.klass = kRcSkirt;
  e.object = m.model;
  e.frame = frame;
  e.call = e.keyed = 0;
  e.pad2 = uint16_t(idx);   // the skirt's index in G[332 + 4 idx] (the applier names its model by it: 974, 973, 974, 973); the game's own skirt entries leave it 0
  e.r5 = m.palette;
  e.first_node = uint32_t(pool.size());
  e.n_nodes = uint16_t(m.n);
  e.flags = kRrRecomputed;
  e.alpha = 1.f;
  for (uint32_t k = 0; k < m.n; ++k) {
    RingNodeRows nr;
    arenaworld::RowsFromMatrix(&m.pal[size_t(k) * 16], nr.rows);
    pool.push_back(nr);
  }
  rows.push_back(e);
  ++cn.skirt_rows;
}

// ======================================================================================================================================================================================================
// THE WHOLE RING (input bit 6, ring_rows): clean = every piece the colour callback writes has its rows for THIS record: 12 ropes, 12 turnbuckles, 12 covers, 4 posts, 3-4 skirts, and none of the not-replicated
// paths was taken (override gate, table mode, RIDS, alt ring). The canvas is not posed by anything under OURS and never changes (run 16, tools\scene\ring_post_static_check.py): it is drawn at rest and has no rows.
// ======================================================================================================================================================================================================
struct WholeRing {
  uint32_t ropes = 0, turnbuckles = 0, covers = 0, covers_hidden = 0, posts = 0, skirts = 0;
  uint32_t posts_expected = 4;   // S103 CRR: a created arena draws 8 post calls (CRK section 2)
  int32_t skirts_expected = -1;   // S103 C5: the skirts the game poses this frame (SkirtVisibleMask); -1 = unknown (unreadable): never clean   // covers_hidden: instances the game does not pose (alpha <= 0): no rows by the game either
  bool override_gate = false;
};
inline constexpr uint32_t kWholeRopes = 12, kWholeTurnbuckles = 12, kWholeCovers = 12, kWholePosts = 4;
inline bool WholeRingClean(const WholeRing& w, RingCensus& cn) {
  bool ok = true;
  if (w.ropes != kWholeRopes) ++cn.unclean_rope, ok = false;
  if (w.turnbuckles != kWholeTurnbuckles) ++cn.unclean_tb, ok = false;
  if (w.covers + w.covers_hidden != kWholeCovers) ++cn.unclean_cover, ok = false;
  if (w.posts != w.posts_expected) ++cn.unclean_post, ok = false;
  if (w.skirts_expected <= 0 || w.skirts != uint32_t(w.skirts_expected)) ++cn.unclean_skirt, ok = false;   // S103 C5: EXACTLY the visible skirts (was >= 3)
  if (w.override_gate) ++cn.unclean_override, ok = false;   // rows 2 / 26 of every rope may be overridden: not replicated
  ++(ok ? cn.whole_clean : cn.whole_unclean);
  return ok;
}

}  // namespace rdr_scene::live::ringrecompute
