// fan_tassel_unit: the offline proof of lane FAN (Session 108; docs\research\s108\FAN_plan.md). 0 game runs, no SDK, no game memory.
//   fan_tassel_unit selftest [--plant ring_cap96 | event_rows_append]
//   fan_tassel_unit real --game DIR --dlc DIR [--plant ring_cap96 | bind_u8]    (read-only: the player's own files; Fandango's prop 2591 is in the DLC folder's evtobj_dlc04.pac)
// It runs the SHIPPED code in the production order: MakeWeapon's hook (Tracker::EventMake) -> the model call (Tracker::OnModelCall: the rows copied AT the call from a fake guest palette) -> the closed frame
// (TakeNext) -> the drawer's plan (weapons::Plan) -> the pose (weapons::BindModel + weapons::PoseCall, ring_pose.h PoseMesh). Each vertex is compared with the GAME's rule computed straight from the guest
// matrices: the draw reads node v's 4x4 at palette + 64 (v - 1) for every palette slot, no cap (sub_82C04DF0 0x82C0A038-0x82C0A04C), so a vertex = sum_k w_k * (v x G_node_k) in file space, i.e.
// v_viewer x MirrorY(G) in the loader's mirrored space.
// Checks (exit 1 when any failed; each planted twin must fail exactly its named checks: tools\weapons\fan_tassel_selftest.py):
//   tassel.all_rows             a 659-node event model's call carries all 659 rows; vertices on rows 0 / 95 / 96 / 657 and a 50 + 600 blend land where the game puts them  <- ring_cap96
//   tassel.over_bound_refused   a count above kWeaponMaxNodes (1025) is refused (refused = over bound, 0 rows, census capped 1) and the plan draws nothing for it            <- ring_cap96
//   tassel.pool_full_whole      a call that does not fit the frame's pool is refused WHOLE (refused = pool full, no row) and the earlier calls keep every row
//   tassel.one_copy_a_frame     an own submit replacing an earlier non-own row of the same event model rewrites its rows in place: the frame's pool holds ONE copy (659 rows, not 1318)  <- event_rows_append
//   tassel.real_vertices        (real) prop 2591 entry 4 from the player's files: every one of its vertices equals the game's rule (from the stored mesh's own slot -> node map, not the bind); none keeps its file position  <- ring_cap96, bind_u8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>   // (asset_load.h's pac / stage_media code needs it; the build passes -I<sdk stb>)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/dlc_catalog.h"
#include "../../src/dlc_overlays.h"
#include "../../src/renderer_scene/rows_ring.h"
#include "../../src/renderer_scene/weapon_draw_core.h"   // BindModel, PoseCall (+ weapon_plan.h: Plan, View)
#include "../../src/renderer_scene/wrestler_src.h"
#include "../../src/scene_live/weapon_rows_core.h"

namespace fs = std::filesystem;
using namespace rdr_scene;
using namespace rdr_scene::live::weaponrows;
namespace W = rdr_scene::weapons;

static int g_fail = 0;
static bool g_bind_u8 = false;   // review: --plant bind_u8 (real only): the drawer's bind narrows every row number to 8 bits
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-26s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// The guest: words as host values (the tracker's reads go through these two functions, as in the other weapon units).
struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  void PutFloat(uint32_t a, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
  void MakeModel(uint32_t obj, uint32_t nodes) {   // a CSkinModel: vtable 0x8206AF90 whose slot 2 is sub_82BD3AB8, [this+296] != 0, the count in [this+300] (the listing's rule)
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, 1.0f);
  }
};

// Node i's 4x4 as the game keeps it (row vectors, translation at [12..14]): a turn about y and x plus a place far from the origin, different for every node.
static void NodeMatrix(uint32_t i, float g[16]) {
  const float a = 0.011f * float(i), b = 0.0037f * float(i);
  const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
  // R = Ry(a) * Rx(b) (row-vector form)
  const float ry[9] = {ca, 0, -sa, 0, 1, 0, sa, 0, ca};
  const float rx[9] = {1, 0, 0, 0, cb, sb, 0, -sb, cb};
  float r[9];
  for (int p = 0; p < 3; ++p)
    for (int q = 0; q < 3; ++q) r[3 * p + q] = ry[3 * p + 0] * rx[0 + q] + ry[3 * p + 1] * rx[3 + q] + ry[3 * p + 2] * rx[6 + q];
  for (int p = 0; p < 3; ++p) {
    for (int q = 0; q < 3; ++q) g[4 * p + q] = r[3 * p + q];
    g[4 * p + 3] = 0.f;
  }
  g[12] = 1000.f + 3.f * float(i), g[13] = 500.f + 0.5f * float(i), g[14] = -2000.f - 2.f * float(i), g[15] = 1.f;
}
static void WritePalette(Guest& gs, uint32_t buf, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    float g[16];
    NodeMatrix(i, g);
    for (uint32_t j = 0; j < 16; ++j) gs.PutFloat(buf + 64 * i + 4 * j, g[j]);
  }
}
// The game's rule for one vertex in the loader's (Y-mirrored) space: sum_k w_k * v x MirrorY(G_k) / sum w (row vectors).
static bool Expected(const assets::Vertex& v, float out[3]) {
  float wsum = 0.f;
  out[0] = out[1] = out[2] = 0.f;
  for (int k = 0; k < 4; ++k) {
    if (!(v.weights[k] > 0.f)) continue;
    float g[16];
    NodeMatrix(v.bones[k], g);
    assets::Matrix m{};
    for (int j = 0; j < 16; ++j) m[size_t(j)] = g[j];
    m = assets::MirrorY(m);
    for (int c = 0; c < 3; ++c) out[c] += v.weights[k] * (v.pos[0] * m[size_t(c)] + v.pos[1] * m[size_t(4 + c)] + v.pos[2] * m[size_t(8 + c)] + m[size_t(12 + c)]);
    wsum += v.weights[k];
  }
  if (!(wsum > 0.f)) return false;
  for (int c = 0; c < 3; ++c) out[c] /= wsum;
  return true;
}

// Review: the same rule from the STORED mesh's own file map (the loader's raw skin pairs: slot -> palette_nodes[slot] = the file's palette word - 1; a mesh with no pairs = rigid_node), never from the
// bind's output: a bind that maps a vertex to a wrong row must not move the reference with it. Vertex i of stored mesh `me`.
static bool ExpectedRaw(const assets::Mesh& me, size_t i, float out[3]) {
  assets::Vertex v = me.vertices[i];
  for (int k = 0; k < 4; ++k) v.bones[k] = 0, v.weights[k] = 0.f;
  if (me.src_skin_pairs == 0) {
    if (me.rigid_node < 0) return false;
    v.bones[0] = uint16_t(me.rigid_node), v.weights[0] = 1.f;
    return Expected(v, out);
  }
  if (me.src_skin_slots.size() < 4 * (i + 1) || me.src_skin_weights.size() < 4 * (i + 1)) return false;
  int nb = 0;
  for (int k = 0; k < 4; ++k) {
    const uint32_t slot = me.src_skin_slots[4 * i + size_t(k)];
    const float w = me.src_skin_weights[4 * i + size_t(k)];
    if (slot == 255 || slot >= me.palette_nodes.size() || me.palette_nodes[slot] == 0xFFFF || !(w > 0.f)) continue;
    v.bones[nb] = me.palette_nodes[slot], v.weights[nb] = w, ++nb;
  }
  return nb > 0 && Expected(v, out);
}

struct Rig {
  Guest g;
  Tracker t;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame, bool own = true) {
    return t.OnModelCall(model, r5, frame, own, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); });
  }
  bool Take(FrameBuffer& fb) {   // the first closed frame with calls
    bool got = t.TakeNext(fb);
    while (got && fb.calls.empty()) got = t.TakeNext(fb);
    return got;
  }
};
static W::View ViewOf(const FrameBuffer& fb) {
  W::View v;
  v.calls = fb.calls.data(), v.n_calls = fb.calls.size(), v.nodes = reinterpret_cast<const float*>(fb.nodes.data()), v.n_nodes = fb.nodes.size();
  return v;
}

// Poses `bound` from the plan's one event item of the frame and compares every vertex with the game's rule. Returns the vertices off (> tol) and the ones left at their file position.
struct PoseResult { size_t items = 0, vertices = 0, off = 0, unposed = 0; float worst = 0.f; uint32_t rows = 0; };
static PoseResult PoseAndCompare(const assets::Model& bound, const FrameBuffer& fb, uint32_t want_k, const assets::Model* stored = nullptr) {   // stored: the reference from its file map (ExpectedRaw), else from `bound`
  PoseResult r;
  const W::View view = ViewOf(fb);
  W::Census cen;
  const std::vector<W::Item> items = W::Plan(view, W::Plants(), cen);
  r.items = items.size();
  if (items.size() != 1 || !items[0].event || items[0].entry != want_k) return r;
  const W::WeaponCall& w = view.calls[items[0].call];
  r.rows = w.node_count;
  std::vector<assets::Matrix> pal;
  assets::Model out = bound;
  W::PoseCall(bound, view, w, W::Plants(), pal, out);
  for (size_t mi = 0; mi < bound.meshes.size(); ++mi)
    for (size_t vi = 0; vi < bound.meshes[mi].vertices.size(); ++vi) {
      const assets::Vertex& s = bound.meshes[mi].vertices[vi];
      const assets::Vertex& d = out.meshes[mi].vertices[vi];
      float e[3];
      if (stored ? !ExpectedRaw(stored->meshes[mi], vi, e) : !Expected(s, e)) continue;
      ++r.vertices;
      float err = 0.f;
      for (int c = 0; c < 3; ++c) err = std::max(err, std::fabs(d.pos[c] - e[c]));
      r.worst = std::max(r.worst, err);
      if (err > 1e-2f) ++r.off;
      if (d.pos[0] == s.pos[0] && d.pos[1] == s.pos[1] && d.pos[2] == s.pos[2]) ++r.unposed;
    }
  return r;
}

static int SelfTest(Plant plant) {
  const uint32_t kTassel = 0xA0001000, kBuf = 0xB0000000, kHuge = 0xA0002000, kHugeBuf = 0xB1000000;
  const uint64_t F = 300;
  // ---- tassel.all_rows ----
  {
    Rig r;
    r.t.plant = plant;
    r.g.MakeModel(kTassel, 659);   // Fandango's tassel model: 659 nodes (docs\formats\evtobj_pac.md: entry 4 skins 638 of 659)
    WritePalette(r.g, kBuf, 659);
    r.t.EventMake(kTassel, 2591, 4);
    const CallResult c = r.Call(kTassel, kBuf, F);
    r.Call(0xA0009000, kBuf, F + 1);   // the next frame closes F
    FrameBuffer fb;
    const bool got = r.Take(fb);
    assets::Model bound;
    assets::Mesh me;
    const uint16_t nodes[5][2] = {{0, 0}, {95, 0}, {96, 0}, {657, 0}, {50, 600}};
    for (const auto& nd : nodes) {
      assets::Vertex v{};
      v.pos[0] = 1.5f, v.pos[1] = -7.f, v.pos[2] = 3.25f;
      v.bones[0] = nd[0], v.weights[0] = nd[1] ? 0.5f : 1.f;
      if (nd[1]) v.bones[1] = nd[1], v.weights[1] = 0.5f;
      me.vertices.push_back(v);
    }
    bound.meshes.push_back(me);
    const PoseResult p = got ? PoseAndCompare(bound, fb, 4) : PoseResult{};
    const Stats s = r.t.GetStats();   // review: the census names the largest call (rows, prop id, entry): the game check reads `largest call 659 rows (event prop 2591 k 4)`
    const bool census_ok = s.max_rows == 659 && s.max_rows_id == 2591 && s.max_rows_k == 4 && s.max_rows_event;
    Check(got && c.recorded && c.event && p.items == 1 && p.rows == 659 && p.vertices == 5 && p.off == 0 && census_ok, "tassel.all_rows",
          "rows " + std::to_string(p.rows) + " of 659, vertices off " + std::to_string(p.off) + " of " + std::to_string(p.vertices) + " (worst " + std::to_string(p.worst) + "), census largest " +
              std::to_string(s.max_rows) + " id " + std::to_string(s.max_rows_id) + " k " + std::to_string(s.max_rows_k));
  }
  // ---- tassel.over_bound_refused ----
  {
    Rig r;
    r.t.plant = plant;
    r.g.MakeModel(kHuge, kWeaponMaxNodes + 1);
    WritePalette(r.g, kHugeBuf, kWeaponMaxNodes + 1);
    r.t.EventMake(kHuge, 2591, 4);
    const CallResult c = r.Call(kHuge, kHugeBuf, F);
    r.Call(0xA0009000, kBuf, F + 1);
    FrameBuffer fb;
    const bool got = r.Take(fb);
    W::Census cen;
    const size_t items = got ? W::Plan(ViewOf(fb), W::Plants(), cen).size() : 99;
    const Stats s = r.t.GetStats();
    Check(c.refused == kRefusedOverBound && c.n_nodes == 0 && s.capped == 1 && items == 0 && got && fb.nodes.empty(), "tassel.over_bound_refused",
          "count 1025: refused " + std::to_string(c.refused) + ", rows " + std::to_string(c.n_nodes) + ", capped " + std::to_string(s.capped) + ", items " + std::to_string(items));
  }
  // ---- tassel.pool_full_whole: 5 calls of 1000 rows (4096 pool): the 5th is refused whole, the first four keep all their rows ----
  {
    Rig r;
    r.t.plant = plant;
    std::vector<CallResult> cs;
    for (uint32_t i = 0; i < 5; ++i) {
      const uint32_t m = 0xA1000000 + 0x1000 * i;
      r.g.MakeModel(m, 1000);
      r.t.EventMake(m, 2600 + int(i), 0);
    }
    WritePalette(r.g, kHugeBuf, 1000);
    for (uint32_t i = 0; i < 5; ++i) cs.push_back(r.Call(0xA1000000 + 0x1000 * i, kHugeBuf, F));
    r.Call(0xA0009000, kBuf, F + 1);
    FrameBuffer fb;
    const bool got = r.Take(fb);
    bool whole = got && fb.calls.size() == 4 && fb.nodes.size() == 4000;
    for (size_t i = 0; whole && i < fb.calls.size(); ++i) whole = fb.calls[i].node_count == 1000;
    const Stats s = r.t.GetStats();
    Check(whole && cs[4].refused == kRefusedPoolFull && !cs[4].recorded && cs[4].pool_used == 4000 && s.overflow == 1, "tassel.pool_full_whole",
          "calls " + std::to_string(got ? fb.calls.size() : 0) + ", rows " + std::to_string(got ? fb.nodes.size() : 0) + ", 5th refused " + std::to_string(cs[4].refused) + " (pool " + std::to_string(cs[4].pool_used) + "), overflow " + std::to_string(s.overflow));
  }
  // ---- tassel.one_copy_a_frame: a non-own pass, then the own submit of the same event model ----
  {
    Rig r;
    r.t.plant = plant;
    r.g.MakeModel(kTassel, 659);
    WritePalette(r.g, kBuf, 659);
    r.t.EventMake(kTassel, 2591, 4);
    const CallResult a = r.Call(kTassel, kBuf, F, /*own=*/false);
    const CallResult b = r.Call(kTassel, kBuf, F, /*own=*/true);
    r.Call(0xA0009000, kBuf, F + 1);
    FrameBuffer fb;
    const bool got = r.Take(fb);
    const Stats s = r.t.GetStats();
    const bool rows_ok = got && fb.calls.size() == 1 && fb.calls[0].node_count == 659 && fb.calls[0].node_first + 659 <= fb.nodes.size();
    Check(a.recorded && b.recorded && rows_ok && fb.nodes.size() == 659 && s.event_replaced == 1 && s.event_rows_in_place == 1, "tassel.one_copy_a_frame",
          "pool rows " + std::to_string(got ? fb.nodes.size() : 0) + " (one copy = 659), calls " + std::to_string(got ? fb.calls.size() : 0) + ", in place " + std::to_string(s.event_rows_in_place));
  }
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  fan_tassel_unit selftest\n");
  return g_fail ? 1 : 0;
}

static int Real(const char* game_dir, const char* dlc_dir, Plant plant) {
  launcher::GameFiles files;
  files.SetRoot(fs::path(game_dir));
  uint64_t gen = ~0ull;
  std::vector<wwe2k14::dlc::Pack> packs = wwe2k14::dlc::Scan(fs::path(dlc_dir), true, false);
  std::vector<wwe2k14::dlc_overlays::Item> items;
  for (const auto& p : packs) {   // as dlc.h publishes: the ready folders, descending pack id
    if (p.status != wwe2k14::dlc::Status::kReady) continue;
    char sfx[16];
    std::snprintf(sfx, sizeof sfx, "_dlc%02d", p.pack_id);
    items.insert(items.begin(), {p.root, sfx, "dlc", p.pack_id, p.package});
  }
  wwe2k14::dlc_overlays::Publish(items);
  wrestler_src::ApplyOverlays(files, &gen);
  const int pach = assets::PropPach(files, 2591);
  assets::Model stored;
  std::string err;
  if (pach < 0 || !assets::LoadPropEntry(files, pach, 4, stored, err)) {
    Check(false, "tassel.real_vertices", "prop 2591 entry 4 did not load (PACH " + std::to_string(pach) + "): " + err);
    return 1;
  }
  assets::Model bound;
  const uint32_t unbound = W::BindModel(stored, bound);   // the drawer's own bind (rf_props.cpp ApplyWeaponRows)
  if (g_bind_u8)   // planted twin (review): the bind narrows a row number to 8 bits (row 300 -> 44): a reference read from `bound` would follow it
    for (assets::Mesh& me : bound.meshes)
      for (assets::Vertex& v : me.vertices)
        for (int k = 0; k < 4; ++k) v.bones[k] = uint16_t(uint8_t(v.bones[k]));
  uint32_t past95 = 0, max_node = 0;
  for (const assets::Mesh& me : bound.meshes) {
    bool any = false;
    for (const assets::Vertex& v : me.vertices)
      for (int k = 0; k < 4; ++k)
        if (v.weights[k] > 0.f) max_node = std::max<uint32_t>(max_node, v.bones[k]), any = any || v.bones[k] >= 96;
    past95 += any ? 1u : 0u;
  }
  std::printf("      prop 2591 -> PACH %d entry 4: %zu meshes (%u unbound), %u skin to rows >= 96, highest row %u\n", pach, bound.meshes.size(), unbound, past95, max_node);
  Rig r;
  r.t.plant = plant;
  const uint32_t kTassel = 0xA0001000, kBuf = 0xB0000000;
  r.g.MakeModel(kTassel, 659);
  WritePalette(r.g, kBuf, 659);
  r.t.EventMake(kTassel, 2591, 4);   // MakeWeapon's hook (0x8272E030): model, prop id, entry k
  r.Call(kTassel, kBuf, 500);
  r.Call(0xA0009000, kBuf, 501);
  FrameBuffer fb;
  const PoseResult p = r.Take(fb) ? PoseAndCompare(bound, fb, 4, &stored) : PoseResult{};
  Check(p.items == 1 && p.rows == 659 && p.vertices > 3000 && p.off == 0 && p.unposed == 0 && unbound == 0, "tassel.real_vertices",
        "rows " + std::to_string(p.rows) + ", vertices " + std::to_string(p.vertices) + ": off the game's rule " + std::to_string(p.off) + " (worst " + std::to_string(p.worst) + "), left at the file position " + std::to_string(p.unposed));
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  fan_tassel_unit real\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Plant plant = Plant::None;
  std::string game, dlc;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) {
      const std::string n = argv[++i];
      if (n == "ring_cap96") plant = Plant::RingCap96;
      else if (n == "event_rows_append") plant = Plant::EventRowsAppend;
      else if (n == "bind_u8") g_bind_u8 = true;
      else { std::fprintf(stderr, "unknown plant %s\n", n.c_str()); return 2; }
    } else if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--dlc" && i + 1 < argc) dlc = argv[++i];
  }
  if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return SelfTest(plant);
  if (argc >= 2 && !std::strcmp(argv[1], "real") && !game.empty() && !dlc.empty()) return Real(game.c_str(), dlc.c_str(), plant);
  std::fprintf(stderr, "usage: fan_tassel_unit selftest [--plant P] | real --game DIR --dlc DIR [--plant P]\n");
  return 2;
}
