// crr_unit: the offline proof of S103 lane CRR: a created arena (ids 200-249) draws its ring from the game's fixed ALT keys (docs\research\s103\CRK.md; src\renderer_scene\ring_keys.h, asset_load.h PlaceRing / load lists,
// ring_low_keys.h, ring_rows_core.h kClassTable, ring_recompute_core.h alt posts).
//   crr_unit [--game DIR] [--plant NAME]
// Part A (no game files): the pure rules: KeyOf / GroupKeys / Need / RankOfKey for the alt ring against the CRK table, the low -> high twins, the game's G+68..+112 class entries, our alt post rows (8 calls, angle indices).
// Part B (--game DIR, the player's own pac\car + pac\bg\bg04.pac): (1) re-find the bug: the keys the OLD loader would take from the merged pac are {952, 980}; (2) every alt piece loads and has its twin's node set (bg04);
// (3) the alt PlaceRing output equals the corner table; the four cover models share one geometry.
// A plant is a FAULT in the thing under test; each must make a NAMED check fail (exit 1 when any check failed):
//   old_cover_corners   the expected at-rest cover per corner is the OLD table (1071, 1072, 1073, 1070 by placement angle; the call corner r25 is not the placement corner)  -> "place.covers_by_corner"
//   normal_cover_pose   the alt cover pose uses the normal angle / record side (CoverNodes with the alt flag off)                                       -> "rows.alt_cover_pose"
//   logo_as_cover       a post logo (1080) stands in for a cover in the shape check               -> "shape.covers_share_geometry"
//   post_loop1_corner0  the alt post loop 1 starts at corner 0 (angle indices 0, 1)               -> "rows.alt_posts_angles"
//   skirt_swap          the alt skirt keys are swapped (1065 for 973's)                           -> "keys.alt_table"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer_scene/rows_ring.h"
#include "../../src/renderer_scene/ring_keys.h"
#include "../../src/scene_live/ring_recompute_core.h"
#include "../../src/scene_live/ring_rows_core.h"

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

namespace rk = rdr_scene::rf::c::ringkeys;
namespace rr = rdr_scene::live::ringrecompute;
namespace rows = rdr_scene::live::ringrows;
using rdr_scene::RingRows;

static RingRows Row(uint8_t klass, uint32_t call, uint16_t pad2 = 0) {
  RingRows e;
  e.klass = klass, e.call = e.keyed = call, e.pad2 = pad2;
  return e;
}
static std::string Keys(const std::vector<int>& v) {
  std::string s;
  for (int k : v) s += (s.empty() ? "" : ",") + std::to_string(k);
  return s;
}
static int SlotOfPart(const std::string& part) {
  int k = 0;
  return std::sscanf(part.c_str(), "slot %d", &k) == 1 && part == "slot " + std::to_string(k) ? k : -1;
}
static bool Near(const assets::Matrix& a, const assets::Matrix& b, float eps = 1e-3f) {
  for (int i = 0; i < 16; ++i)
    if (std::fabs(a[size_t(i)] - b[size_t(i)]) > eps) return false;
  return true;
}

int main(int argc, char** argv) {
  std::string game;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) g_plant = argv[++i];
    else if (a == "--game" && i + 1 < argc) game = argv[++i];
  }
  const float kPi = 3.14159265f;

  // ---------------------------------------------------------------- A1. the alt key table (CRK section 1 / 2)
  {
    // the expected keys, written from the CRK tables (not from the code under test)
    int want_skirt_even = 1065, want_skirt_odd = 1064;   // G[332..344] = 1065, 1064, 1065, 1064 vs the normal 974, 973, 974, 973
    if (Plant("skirt_swap")) want_skirt_even = 973;
    bool ok = true;
    std::string why;
    const auto expect = [&](const RingRows& e, bool ours, int key, const char* what) {
      const int got = rk::KeyOf(e, ours, true);
      if (got != key) ok = false, why += std::string(what) + " " + std::to_string(got) + " != " + std::to_string(key) + "; ";
    };
    expect(Row(rdr_scene::kRcRopeHi, 0), true, 1021, "rope hi");
    expect(Row(rdr_scene::kRcRopeLo, 0), true, 1041, "rope lo");
    // by the call corner r25 (sub_82740550 for 0..2, sub_827403E8 for 3; r25 poses at angle {pi/2, pi, -pi/2, 0}: the corner at angle p * pi/2 holds 1070 + p)
    const int cover_hi[4] = {1071, 1072, 1073, 1070}, cover_lo[4] = {1075, 1076, 1077, 1074};
    for (uint32_t c = 0; c < 12; ++c) {   // ours: call = 3 * corner + height
      expect(Row(rdr_scene::kRcCoverHi, c), true, cover_hi[c / 3], "ours cover hi");
      expect(Row(rdr_scene::kRcCoverLo, c), true, cover_lo[c / 3], "ours cover lo");
    }
    const int post_hi[8] = {1050, 1050, 1051, 1051, 1080, 1080, 1081, 1081}, post_lo[8] = {1052, 1052, 1053, 1053, 1082, 1082, 1083, 1083};
    for (uint32_t c = 0; c < 8; ++c) {
      expect(Row(rdr_scene::kRcPostHi, c), true, post_hi[c], "ours post hi");
      expect(Row(rdr_scene::kRcPostLo, c), true, post_lo[c], "ours post lo");
    }
    expect(Row(rdr_scene::kRcTurnbuckle, 5), true, 952, "turnbuckle");
    expect(Row(rdr_scene::kRcSkirt, 0, 0), true, want_skirt_even, "skirt 0");
    expect(Row(rdr_scene::kRcSkirt, 0, 1), true, want_skirt_odd, "skirt 1");
    expect(Row(rdr_scene::kRcSkirt, 0, 0), false, 0, "game skirt rows are not keyed");
    // the game's own rows: the base slots G+36 / 40 / 48 / 52 hold the corner-3 cover and the loop-1 post; the G+68..+112 slots carry their key in pad2
    expect(Row(rdr_scene::kRcCoverHi, 2), false, 1070, "game G+36");
    expect(Row(rdr_scene::kRcCoverLo, 2), false, 1074, "game G+40");
    expect(Row(rdr_scene::kRcPostHi, 1), false, 1050, "game G+48");
    expect(Row(rdr_scene::kRcPostLo, 1), false, 1052, "game G+52");
    for (const rows::ClassEntry& ce : rows::kClassTable) {
      if (ce.key == 0) continue;
      expect(Row(ce.cls, 1, ce.key), false, int(ce.key), "game alt slot");
    }
    Check(ok, "keys.alt_table", ok ? "every class / call of the alt ring maps to its CRK key (ours: covers by call / 3, 8 posts; the game's rows: base slots + pad2)" : why);
    // the normal ring is untouched
    bool normal = rk::KeyOf(Row(rdr_scene::kRcCoverHi, 7), true, false) == 964 && rk::KeyOf(Row(rdr_scene::kRcPostHi, 3, 1075), false, false) == 951 && rk::KeyOf(Row(rdr_scene::kRcRopeLo, 0), true, false) == 980 &&
                  rk::KeyOf(Row(rdr_scene::kRcSkirt, 0, 1), true, false) == 973 && rk::KeyOf(Row(rdr_scene::kRcSkirt, 0, 0), true, false) == 974 && rk::Need(3, false) == 4 && rk::Need(3, true) == 8;
    Check(normal, "keys.normal_unchanged", "a normal arena keeps 951-982 and 4 posts; pad2 never names a key there");
    // every key the alt rule can produce belongs to its group's key list and has a rank / the low twins pair up
    std::set<int> produced;
    for (uint8_t k : {rdr_scene::kRcRopeHi, rdr_scene::kRcRopeLo, rdr_scene::kRcCoverHi, rdr_scene::kRcCoverLo, rdr_scene::kRcTurnbuckle, rdr_scene::kRcPostHi, rdr_scene::kRcPostLo, rdr_scene::kRcSkirt})
      for (uint32_t c = 0; c < 12; ++c)
        for (uint16_t p2 : {uint16_t(0), uint16_t(1)}) {
          const int key = rk::KeyOf(Row(k, c, p2), true, true);
          if (key) produced.insert(key);
        }
    bool in_group = true;
    std::string miss;
    for (int key : produced) {
      bool found = false;
      for (int g = 0; g < 5; ++g)
        for (int gk : rk::GroupKeys(g, true)) found = found || gk == key;
      if (!found) in_group = false, miss += std::to_string(key) + " ";
      if (rk::RankOfKey(key).tok == nullptr) in_group = false, miss += "rank:" + std::to_string(key) + " ";
    }
    const bool twins = assets::RingHighTwin(1041) == 1021 && assets::RingHighTwin(1052) == 1050 && assets::RingHighTwin(1053) == 1051 && assets::RingHighTwin(1074) == 1070 && assets::RingHighTwin(1075) == 1071 &&
                       assets::RingHighTwin(1076) == 1072 && assets::RingHighTwin(1077) == 1073 && assets::RingHighTwin(1082) == 1080 && assets::RingHighTwin(1083) == 1081 && assets::RingHighTwin(1070) == 0;
    Check(in_group && twins && rk::GroupKeys(3, true).size() == 8 && rk::GroupKeys(1, true).size() == 8, "keys.groups_and_twins", in_group ? "every produced key is in its group's list and ranked; the low -> high twins pair up" : "missing: " + miss);
    // the game's G+68..+112 entries (CRK section 1: listing 0x824EB6DC-0x824EBD58)
    const struct { uint32_t off; uint8_t cls; uint16_t key; } want[12] = {{68, rdr_scene::kRcCoverHi, 1071}, {72, rdr_scene::kRcCoverHi, 1072}, {76, rdr_scene::kRcCoverHi, 1073}, {80, rdr_scene::kRcCoverLo, 1075}, {84, rdr_scene::kRcCoverLo, 1076},
                                                                          {88, rdr_scene::kRcCoverLo, 1077}, {92, rdr_scene::kRcPostHi, 1051}, {96, rdr_scene::kRcPostHi, 1080}, {100, rdr_scene::kRcPostHi, 1081}, {104, rdr_scene::kRcPostLo, 1053},
                                                                          {108, rdr_scene::kRcPostLo, 1082}, {112, rdr_scene::kRcPostLo, 1083}};
    bool table = rows::kClassTableCount == 25;
    for (const auto& w : want) {
      bool found = false;
      for (const rows::ClassEntry& ce : rows::kClassTable) found = found || (ce.offset == w.off && ce.cls == w.cls && ce.key == w.key);
      table = table && found;
    }
    // a fake G: a model pointer in G+96 is a post_hi with key 1080; a pointer in G+20 is a rope with no alt key; a 0 pointer is nothing
    std::map<uint32_t, uint32_t> mem;
    mem[rows::kTableGlobal] = 0x40000;
    for (const rows::ClassEntry& ce : rows::kClassTable) mem[0x40000 + ce.offset] = 0;
    mem[0x40000 + 96] = 0x70000, mem[0x40000 + 20] = 0x71000;
    rows::RingTable tb;
    const bool rt = rows::ReadRingTable([&](uint32_t a, uint32_t& v) { auto it = mem.find(a); v = it == mem.end() ? 0 : it->second; return true; }, tb);
    table = table && rt && tb.ClassOf(0x70000) == rdr_scene::kRcPostHi && tb.AltKeyOf(0x70000) == 1080 && tb.ClassOf(0x71000) == rdr_scene::kRcRopeHi && tb.AltKeyOf(0x71000) == 0 && tb.ClassOf(0) == rdr_scene::kRcNone && tb.AltKeyOf(0) == 0;
    Check(table, "rows.class_table_alt", "kClassTable holds the 12 G+68..+112 slots with their keys (25 entries); a pointer found there reads back its class and key");
  }

  // ---------------------------------------------------------------- A2. our alt post rows (CRK section 2: 8 calls; angle indices {1,2}, {0,-1}, {1,2}, {0,-1})
  {
    rr::PostInputs alt;
    alt.ok = true, alt.alt = true;
    alt.models[0].model = 1, alt.models[0].n = 3, alt.models[0].palette = 0x1000;
    alt.step = kPi * 0.5f, alt.pi = kPi, alt.neg_pi = -kPi, alt.two_pi = 2.f * kPi;
    rr::PostInputs norm = alt;
    norm.alt = false;
    std::vector<RingRows> a_rows, n_rows;
    std::vector<rdr_scene::RingNodeRows> a_pool, n_pool;
    rr::RingCensus cn;
    for (uint32_t i = 0; i < rr::PostCalls(alt); ++i) rr::AppendPost(alt, 0, i, 7, a_rows, a_pool, cn);
    for (uint32_t i = 0; i < rr::PostCalls(norm); ++i) rr::AppendPost(norm, 0, i, 7, n_rows, n_pool, cn);
    // the expected normal-ring row of angle index k (k in 0..3 are the stored ones; -1 = 3 after the wrap)
    int idx_want[8] = {1, 2, 0, -1, 1, 2, 0, -1};
    if (Plant("post_loop1_corner0")) idx_want[0] = 0, idx_want[1] = 1;
    bool ok = a_rows.size() == 8 && n_rows.size() == 4 && rr::PostCalls(alt) == 8 && rr::PostCalls(norm) == 4;
    std::string why = ok ? "" : "row counts " + std::to_string(a_rows.size()) + " / " + std::to_string(n_rows.size());
    for (size_t c = 0; ok && c < 8; ++c) {
      const int nk = (idx_want[c] + 4) % 4;
      const RingRows& ar = a_rows[c];
      const RingRows& nr = n_rows[size_t(nk)];
      bool same = ar.n_nodes == 3 && ar.klass == rdr_scene::kRcPostHi && ar.call == c;
      for (uint32_t n = 0; same && n < 3; ++n)
        for (int f = 0; f < 12; ++f) same = same && std::fabs(a_pool[ar.first_node + n].rows[f] - n_pool[nr.first_node + n].rows[f]) < 1e-5f;
      if (!same) ok = false, why += "call " + std::to_string(c) + " is not the rotation of index " + std::to_string(idx_want[c]) + "; ";
    }
    Check(ok, "rows.alt_posts_angles", ok ? "8 alt post rows = RotY of angle indices 1, 2, 0, -1, 1, 2, 0, -1 (the normal 4 rows are indices 0..3); the 4-row normal set is unchanged" : why);
    rr::WholeRing w;
    w.ropes = 12, w.turnbuckles = 12, w.covers = 12, w.posts = 8, w.posts_expected = 8, w.skirts = 3, w.skirts_expected = 3;
    rr::RingCensus c2;
    const bool clean8 = rr::WholeRingClean(w, c2);
    w.posts_expected = 4;
    const bool unclean = !rr::WholeRingClean(w, c2);
    Check(clean8 && unclean, "rows.whole_ring_posts", "a created arena's whole ring is clean with 8 posts, not with the normal 4 expected");
    {   // the alt cover pose (CRR fix, sub_825CE668 0x825CE848-0x825CE89C): call corner r25 -> angle {pi/2, pi, -pi/2, 0}, record side {2, 3, 0, 1}; one descriptor of side 0 (the normal pose reads side (0 + 1) % 4 = 1)
      rr::CvInputs cv;
      cv.ok = true, cv.alt = !Plant("normal_cover_pose");
      cv.models[0].model = 1, cv.models[0].n = 3;
      cv.models[0].inv.assign(48, 0.f), cv.models[0].pal.assign(48, 0.f);
      for (int n = 0; n < 3; ++n) cv.models[0].inv[size_t(n) * 16] = cv.models[0].inv[size_t(n) * 16 + 5] = cv.models[0].inv[size_t(n) * 16 + 10] = cv.models[0].inv[size_t(n) * 16 + 15] = 1.f;
      cv.ang_step = kPi * 0.5f, cv.ang_base = 0.f, cv.pi = kPi, cv.neg_pi = -kPi, cv.two_pi = 2.f * kPi;
      rr::Inputs grid;
      grid.layer[0].resize(size_t(rr::kRecCount) * 3);
      for (size_t i = 0; i < grid.layer[0].size(); ++i) grid.layer[0][i] = float(i / 3) + 0.25f * float(i % 3);   // record i = (i, i + .25, i + .5)
      const float ang[4] = {kPi * 0.5f, kPi, -kPi * 0.5f, 0.f};
      const int side[4] = {2, 3, 0, 1};
      bool ok = true;
      std::string why;
      for (uint32_t r25 = 0; r25 < 4; ++r25) {
        cv.count[r25] = 1;
        cv.desc[r25] = {rr::CvDesc{1, 0, 0}};
        std::vector<float> nodes;
        float alpha = 0.f;
        rr::RingCensus c3;
        const bool got = rr::CoverNodes(cv, grid, 0, r25, 0, nodes, alpha, c3);
        const uint32_t ri = 24u * uint32_t(side[r25]);
        const float t[3] = {grid.layer[0][ri * 3], grid.layer[0][ri * 3 + 1], grid.layer[0][ri * 3 + 2]};
        float w[16];
        rr::TbWorld(ang[r25], t, w);
        bool same = got && nodes.size() == 48;
        for (int k = 0; same && k < 16; ++k) same = std::fabs(nodes[16 + size_t(k)] - w[k]) < 1e-4f;
        if (!same) ok = false, why += "corner " + std::to_string(r25) + " is not angle " + std::to_string(ang[r25]) + " at record side " + std::to_string(side[r25]) + "; ";
      }
      Check(ok, "rows.alt_cover_pose", ok ? "alt covers: r25 0..3 pose at angle pi/2, pi, -pi/2, 0 on grid record side 2, 3, 0, 1 (the descriptor's own side is replaced)" : why);
    }
  }

  // ---------------------------------------------------------------- B. the player's own files
  if (!game.empty()) {
    launcher::GameFiles files;
    files.SetRoot(game);
    assets::load_detail::RingLowKeys() = true;   // the live arena load's switch (live_assets.cpp)
    assets::car::Sel sel;
    sel.valid = true, sel.arena = 200, sel.caen = 0, sel.casd = 0, sel.carl = 0, sel.bg = 5;
    assets::car::Clear();
    assets::Model m;
    std::string err;
    const bool ok = assets::LoadCustomArena(files, 200, sel, m, err);
    Check(ok, "real.loads_created_arena", ok ? m.label : err);
    assets::Model twin;
    std::string err2;
    const bool tok = assets::LoadArena(files, 4, twin, err2);
    Check(tok, "real.loads_bg04", tok ? twin.label : err2);
    if (ok && tok) {
      std::map<int, std::vector<size_t>> by_key, tw_key;   // ring key -> mesh indices
      for (size_t i = 0; i < m.meshes.size(); ++i) {
        const int k = SlotOfPart(m.meshes[i].part);
        if (k >= 951) by_key[k].push_back(i);
      }
      for (size_t i = 0; i < twin.meshes.size(); ++i) {
        const int k = SlotOfPart(twin.meshes[i].part);
        if (k >= 951) tw_key[k].push_back(i);
      }
      // 1. the bug: what the OLD loader took from the merged pac (keys 951 952 953 960 964 973 974 + the low 980 981 982) = {952, 980}
      std::string e3;
      assets::car::Publish(sel);
      const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(200), e3);
      std::vector<int> old_keys;
      if (p)
        for (int k : {951, 952, 953, 960, 964, 973, 974, 980, 981, 982})
          if (assets::pac::Find(p->pachs[0], uint32_t(k))) old_keys.push_back(k);
      assets::car::Clear();
      Check(p && Keys(old_keys) == "952,980", "bug.old_keys_are_952_980", p ? "the keys the old loader would draw from the merged pac: " + Keys(old_keys) : e3);
      // 2. every alt piece is loaded, a ring piece by the rule rf_c_core.cpp PartOf uses
      std::vector<int> alt_all;
      for (int k : assets::kAltRingHighKeys) alt_all.push_back(k);
      for (const assets::RingLowKey& lk : assets::kRingLowKeys)
        if (assets::IsAltRingKey(lk.low)) alt_all.push_back(lk.low);
      std::sort(alt_all.begin(), alt_all.end());
      std::vector<int> missing, extra;
      for (int k : alt_all)
        if (!by_key.count(k)) missing.push_back(k);
      for (const auto& kv : by_key)
        if (assets::IsAltRingKey(kv.first) && !std::binary_search(alt_all.begin(), alt_all.end(), kv.first)) extra.push_back(kv.first);
      const bool has_shared = by_key.count(952) && by_key.count(980);
      Check(missing.empty() && extra.empty() && has_shared && alt_all.size() == 21, "bug.alt_keys_loaded", missing.empty() ? std::to_string(alt_all.size()) + " alt keys + 952 + 980 loaded, none outside the table" : "missing " + Keys(missing) + " extra " + Keys(extra));
      bool no_normal = !by_key.count(951) && !by_key.count(960) && !by_key.count(964) && !by_key.count(973) && !by_key.count(974);
      Check(no_normal, "bug.no_normal_keys_in_car", "the merged pac holds none of 951 / 960 / 964 / 973 / 974 (they are the normal ring's)");

      // 3. node counts: each alt piece's skeleton (the JBOY node table: posts 3, covers 3, canvas 20, ropes 27, skirts 66) has the node count of its bg04 twin; the cover models share one geometry
      const auto be32 = [](const uint8_t* q) { return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | uint32_t(q[3]); };
      const auto node_names = [&](const assets::pac::Pac* pc, int key) {
        std::vector<std::string> names;
        const assets::pac::Ent* e = pc ? assets::pac::Find(pc->pachs[0], uint32_t(key)) : nullptr;
        if (!e) return names;
        const assets::Bytes blob = assets::pac::Decode(pc->bytes.data() + e->off, e->size);
        if (blob.size() < 8 + 0x28) return names;
        const uint8_t* body = blob.data() + 8;
        const uint32_t nn = be32(body + 0x18), nt = be32(body + 0x20);
        if (nn > 4096 || size_t(nt) + size_t(nn) * 80 > blob.size() - 8) return names;
        for (uint32_t i = 0; i < nn; ++i) names.emplace_back(reinterpret_cast<const char*>(body + nt + 80ull * i), strnlen(reinterpret_cast<const char*>(body + nt + 80ull * i), 16));
        return names;
      };
      assets::car::Publish(sel);
      std::string e5, e6;
      // (OpenCached keeps ONE pac: read every alt key's node count before the bg04 pac replaces it)
      std::map<int, size_t> alt_nodes, twin_nodes;
      const assets::pac::Pac* car_pac = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(200), e5);
      for (int k = 1017; k <= 1083; ++k) alt_nodes[k] = node_names(car_pac, k).size();
      assets::car::Clear();
      const assets::pac::Pac* bg04_pac = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(4), e6);
      for (int k : {951, 953, 960, 964, 973, 974, 980, 981, 982}) twin_nodes[k] = node_names(bg04_pac, k).size();
      struct Pair { int alt, normal; };
      std::vector<Pair> pairs = {{1050, 951}, {1051, 951}, {1052, 981}, {1053, 981}, {1070, 964}, {1071, 964}, {1072, 964}, {1073, 964}, {1074, 982}, {1075, 982}, {1076, 982}, {1077, 982}, {1017, 953}, {1021, 960}, {1041, 980}, {1064, 973}, {1065, 974}};
      bool same = car_pac && bg04_pac;
      std::string why = same ? "" : "a pac did not open; ";
      for (const Pair& pr : pairs) {
        if (!same) break;
        if (alt_nodes[pr.alt] == 0 || alt_nodes[pr.alt] != twin_nodes[pr.normal]) same = false, why += std::to_string(pr.alt) + " has " + std::to_string(alt_nodes[pr.alt]) + " nodes, twin " + std::to_string(pr.normal) + " " + std::to_string(twin_nodes[pr.normal]) + "; ";
      }
      Check(same, "shape.alt_nodes_match_bg04_twins", same ? std::to_string(pairs.size()) + " alt pieces have the node count of their bg04 twin (posts 3, covers 3, canvas 20, ropes 27, skirts 66)" : why);
      std::printf("  info: node counts: 1050 %zu, 1080 %zu, 1081 %zu, 1051 %zu, 1070 %zu, 1017 %zu, 1021 %zu, 1064 %zu\n", alt_nodes[1050], alt_nodes[1080], alt_nodes[1081], alt_nodes[1051], alt_nodes[1070], alt_nodes[1017], alt_nodes[1021], alt_nodes[1064]);
      // the alt post models all carry the 3 nodes our post rows are cut for (ring_recompute_core.h PostNodes: n from G+48's model)
      bool post3 = true;
      for (int k : {1050, 1051, 1052, 1053, 1080, 1081, 1082, 1083}) post3 = post3 && alt_nodes[k] == 3;
      Check(post3, "shape.alt_posts_three_nodes", "every alt post / logo model has 3 nodes: one 3-row set per call poses any of them");
      const auto verts = [](const assets::Model& mm, const std::vector<size_t>& idx) {
        std::vector<size_t> v;
        for (size_t i : idx) v.push_back(mm.meshes[i].vertices.size());
        return v;
      };
      // the four cover models are one shape (same vertex counts per mesh and the same positions: they differ by texture only, CRK 5)
      std::vector<int> covers = {1070, 1071, 1072, 1073};
      if (Plant("logo_as_cover")) covers[1] = 1080;
      bool geo = true;
      std::string gwhy;
      for (int k : covers) {
        if (!by_key.count(k) || !by_key.count(1070) || verts(m, by_key[k]) != verts(m, by_key[1070])) { geo = false, gwhy += std::to_string(k) + " differs in vertex counts; "; continue; }
        for (size_t j = 0; j < by_key[k].size(); ++j) {
          const assets::Mesh& a = m.meshes[by_key[k][j]];
          const assets::Mesh& b = m.meshes[by_key[1070][j]];
          for (size_t v = 0; v < a.vertices.size(); ++v)
            for (int c = 0; c < 3; ++c)
              if (std::fabs(a.vertices[v].pos[c] - b.vertices[v].pos[c]) > 1e-3f) { geo = false, gwhy += std::to_string(k) + " differs in positions; "; v = a.vertices.size(); j = by_key[k].size(); break; }
        }
      }
      Check(geo, "shape.covers_share_geometry", geo ? "covers 1070-1073 share vertex counts and positions (the rows of one corner pose any of them)" : gwhy);

      // 4. the alt PlaceRing output = the corner table (CRK section 4 item 3)
      const auto placement = [&](int key) -> const assets::Placement* {
        if (!by_key.count(key)) return nullptr;
        const int pi = m.meshes[by_key[key][0]].placement;
        return pi >= 0 && size_t(pi) < m.placements.size() ? &m.placements[size_t(pi)] : nullptr;
      };
      using namespace assets::load_detail;
      const auto rot = [&](int idx) { return RotYFile(float(idx) * kPi * 0.5f); };
      const auto expect_post = [&](int key, std::vector<int> idxs) {
        const assets::Placement* pl = placement(key);
        if (!pl || pl->transforms.size() != idxs.size()) return false;
        for (size_t i = 0; i < idxs.size(); ++i)
          if (!Near(pl->transforms[i], assets::MirrorY(rot(idxs[i])))) return false;
        return true;
      };
      bool posts = expect_post(1050, {1, 2}) && expect_post(1051, {0, -1}) && expect_post(1081, {0, -1}) && expect_post(1080, {1, 2});
      Check(posts, "place.posts", "1050 at RotY(1, 2 * pi/2) (loop 1); 1051 + 1081 at corners 0, 3 (0, -pi/2); 1080 at corners 1, 2");
      int cover_want[4] = {1070, 1071, 1072, 1073};   // by placement angle p * pi/2 (CRR fix: the alt pose puts call corner r25 at {pi/2, pi, -pi/2, 0})
      if (Plant("old_cover_corners")) cover_want[0] = 1071, cover_want[1] = 1072, cover_want[2] = 1073, cover_want[3] = 1070;
      const float cover_y[3] = {-153.844f, -195.879f, -237.844f};
      bool covers_ok = true;
      std::string cwhy;
      for (int corner = 0; corner < 4; ++corner) {
        const assets::Placement* pl = placement(cover_want[corner]);
        bool this_ok = pl && pl->transforms.size() == 3;
        for (int h = 0; this_ok && h < 3; ++h) this_ok = Near(pl->transforms[size_t(h)], assets::MirrorY(assets::Mul(TransFile(-2.931f, cover_y[h], -2.928f), rot(corner))));
        if (!this_ok) covers_ok = false, cwhy += "corner " + std::to_string(corner) + " is not model " + std::to_string(cover_want[corner]) + "'s placement; ";
      }
      Check(covers_ok, "place.covers_by_corner", covers_ok ? "3 heights per corner; the corner at angle p * pi/2 holds 1070 + p (the 964 shift, RotY(corner * pi/2))" : cwhy);
      const assets::Placement* rope = placement(1021);
      const assets::Placement* sk_a = placement(1064);
      const assets::Placement* sk_b = placement(1065);
      const assets::Placement* tb = placement(952);
      Check(rope && rope->transforms.size() == 12 && sk_a && sk_a->transforms.size() == 2 && sk_b && sk_b->transforms.size() == 2 && tb && tb->transforms.size() == 12, "place.ropes_skirts_buckles",
            "12 ropes (1021), 12 turnbuckles (952), skirts 1064 / 1065 two each (973's / 974's placements)");
      bool no_canvas_place = !placement(1017);
      Check(no_canvas_place, "place.canvas_as_stored", "the canvas 1017 has no placement (as stored, like 953)");
    }
  }
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
