// caster_rows_test: the offline proof of lane 202's Caster D3 phase 1 pieces E0-E2 (Session 101; docs\renderer\SKIP_GAME_WORK_MAP.md 17.3 / 17.7). 0 game runs, no SDK, no game memory.
//   caster_rows_test --sites tools\caster\caster_call_sites.tsv [--plant NAME] [--write-record PATH]
// It runs the SHIPPED code: the pure rules (src\scene_live\caster_core.h), the pass-entry tracker (caster_hook.h) against a fake guest memory, the contributor's Emit (caster_contrib.h) and the record file
// code (record_io: kScCasterCalls / kScCasterStates round trip, an older record, an unknown layout, a bad state index).
// THE ORACLE is the listings table (tools\caster\caster_call_sites.tsv: the three methods, the collapse flag's word, the world's shape), never the stream's values: the fake guest is BUILT from it
// (the camera record R rewritten by the "game" before each pass entry, the palettes of the wrestlers rewritten every frame with junk past the node count, the collapse word set around map A), the game's
// frames are replayed in LIVE ORDER (a frame's entries, then the frame boundary's Emit with the present's guest frame f + 1; the consumer keeps only the newest 8 records and joins by (hook stamp, pass,
// object, ordinal)), and every expected value is computed here from the values the "game" wrote at that entry by a formula of this file (the palette hash by this file's own FNV, not HashPalette).
// Plants (each must make a NAMED check fail; exit code 1 when any check failed):
//   late_record        the consumer sees record R_f only with stream frame f + 2 (a record one present late)                          -> "contrib.live_order_join"
//   offset0            the +1 frame offset lost (the stamp is the stream's frame, not the hook's)                                   -> "contrib.live_order_join"
//   endless_retry      an entry whose palette was unreadable is retried at every later entry                                          -> "hook.unreadable_once"
//   dropped_frame_failure  calls of an already closed frame counted as failures that latch                                           -> "hook.old_calls_not_failures"
//   ordinal_merge      map A and map B (and plane, rim) keyed alike: one counter per object                                          -> "hook.ordinals"
//   stale_slots        the palette hash covers slots past the part's own node count                                                  -> "hook.palette_hash"
//   camera_prev_frame  the camera state is a cache that is not invalidated at the frame boundary                                    -> "hook.camera_per_entry"
//   late_read          R is read when the NEXT entry arrives, not at the entry                                                        -> "hook.camera_per_entry"
//   map_a_key_ignored  the collapse flag is not read                                                                                 -> "hook.collapse"
//   node_shift         (lane 217) the per-node hashes are stored one node early                                                      -> "hook.node_located"
//   plane_r5_palette   (lane 226) a plane entry takes r5 as the palette pointer (the plane pass never reads r5)                          -> "hook.plane_mats"
//   plane_pos_dropped  (lane 226 follow-up, S1) the plane entry's c7 snapshot is not stored in the extra                                 -> "hook.plane_pos"
//   plane_mats_shift   (lane 226) the plane matrices are stored one node early                                                         -> "hook.plane_mats"
//   product_slot0      (lane 221) the camera product is read for slot 0, not the thread's slot                                       -> "hook.extra_per_entry"
//   product_prev_frame (lane 221) the first entry's product of an earlier frame is reused (a cache never invalidated)                  -> "hook.extra_per_entry"
//   pose_row_off       (lane 221) the map-A collapse point is row 2 of the pose matrix, not row 3                                       -> "hook.pose_point"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/caster_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::live::casters;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;

static_assert(kScCasterNodes == 24 && kScCasterExtras == 25 && kScCasterMats == 26 && kScCount == 28 && sizeof(Mat4) == 64 && sizeof(CasterExtra) == 96 && sizeof(uint32_t) == 4, "lane 217: section 24 (the per-node hash pool); lane 221: section 25 (CasterExtra, 96 bytes; the Python reader pins the same)");
static_assert(kScCasterCalls == 22 && kScCasterStates == 23 && sizeof(CasterCall) == 64 && sizeof(LightState) == 1232, "sections 22 / 23; the Python reader pins the same sizes");
static_assert(!kCasterSkipBuilt, "lane 202 builds no skipping");

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- a fake guest: 4-byte words (host values) in a hash map ----
struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;
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
  // raw BIG-endian bytes as the real ReadGuest returns them (DecodeRecord byte-swaps)
  bool ReadBytes(uint32_t a, void* dst, size_t n) const {
    auto* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      uint32_t w = it == words.end() ? 0u : it->second;   // an unwritten word of the record is 0
      if (it == words.end() && !Mapped(a)) return false;
      w = __builtin_bswap32(w);
      std::memcpy(d + i, &w, 4);
    }
    return true;
  }
  static bool Mapped(uint32_t a) { return a >= 0x41000000u && a < 0x41010000u; }   // the record region (kArr..kArr+64K) is mapped, unwritten words read 0
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
};

// ---- the oracle's own hash (FNV-1a 64 over the float bytes, written here, not HashPalette) ----
static uint64_t Fnv(const std::vector<float>& f) {
  uint64_t h = 1469598103934665603ull;
  for (float x : f) {
    uint32_t b;
    std::memcpy(&b, &x, 4);
    for (int k = 0; k < 4; ++k) h = (h ^ ((b >> (8 * k)) & 0xFF)) * 1099511628211ull;
  }
  return h ? h : 1;
}

constexpr uint32_t kG = 0x40000000, kArr = 0x41000000, kTls = 0x42000000, kBlk = 0x43000000, kR13 = 0x44000000, kSlot = 1;

enum CamKind : uint32_t { kCamProps = 1, kCamMapA, kCamMapB, kCamPlane, kCamRim };
static float CamV(uint32_t kind, uint32_t frame, uint32_t i) { return float((frame % 500u) * 17u + kind * 131u + i) * 0.5f; }   // exact in float
static void SetCamera(Guest& g, CamKind kind, uint32_t frame) {
  const uint32_t r = kArr + kSlot * 2208;
  for (uint32_t i = 0; i < 48; ++i) g.PutFloat(r + 912 + 4 * i, CamV(kind, frame, i));
  for (uint32_t i = 0; i < 15; ++i) g.PutFloat(r + 1104 + 4 * i, CamV(kind, frame, 100 + i));
  for (uint32_t i = 0; i < 16; ++i) g.PutFloat(r + 2144 + 4 * i, CamV(kind, frame, 200 + i));
  for (uint32_t i = 0; i < 224; ++i) g.PutFloat(r + 4 * i, CamV(kind, frame, 300 + i));   // the light block too (the key covers R+0..1167)
}
static bool CamMatches(const LightState& s, CamKind kind, uint32_t frame) {
  for (uint32_t m = 0; m < 3; ++m)
    for (uint32_t i = 0; i < 16; ++i)
      if (s.shadow_m[m][i] != CamV(kind, frame, m * 16 + i)) return false;
  for (uint32_t i = 0; i < 15; ++i)
    if (s.shadow_f[i] != CamV(kind, frame, 100 + i)) return false;
  for (uint32_t b = 0; b < 2; ++b)
    for (uint32_t i = 0; i < 8; ++i)
      if (s.shadow_x[b][i] != CamV(kind, frame, 200 + b * 8 + i)) return false;
  return true;
}

struct Model {
  uint32_t addr = 0, pal = 0, count = 0;   // count 0 = a prop (WantCount 0)
};
static float PalV(uint32_t frame, uint32_t w, uint32_t node, uint32_t e) { return float((frame % 400u) * 19u + w * 211u + node * 13u + e) * 0.25f; }
static void WritePalette(Guest& g, const Model& m, uint32_t frame, bool junk_past = true) {
  for (uint32_t n = 0; n < m.count; ++n)
    for (uint32_t e = 0; e < 16; ++e) g.PutFloat(m.pal + 64 * n + 4 * e, PalV(frame, m.addr & 0xFF, n, e));
  if (junk_past)
    for (uint32_t n = m.count; n < 24; ++n)
      for (uint32_t e = 0; e < 16; ++e) g.PutFloat(m.pal + 64 * n + 4 * e, -1000.f - float(frame % 100u) - float(n));   // slots past the part's own count: the previous draw's rows
}
static uint64_t ExpectHash(const Model& m, uint32_t frame) {
  std::vector<float> f;
  for (uint32_t n = 0; n < m.count; ++n)
    for (uint32_t e = 0; e < 16; ++e) f.push_back(PalV(frame, m.addr & 0xFF, n, e));
  return Fnv(f);
}

struct Expect {
  uint8_t pass;
  uint32_t frame, object, ordinal, key, r5, n_nodes, flags;
  uint8_t collapse;
  CamKind cam;
  uint64_t hash;
  uint32_t map_a[3];
  bool unreadable;
};

struct Site {   // the table's rows we use
  std::string kind, a, b, c, d;
};
static std::vector<std::vector<std::string>> LoadTable(const std::string& path) {
  std::vector<std::vector<std::string>> v;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) c.push_back(cell);
    v.push_back(c);
  }
  return v;
}

int main(int argc, char** argv) {
  std::string plant, sites_path = "tools/caster/caster_call_sites.tsv", write_record;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--sites" && i + 1 < argc) sites_path = argv[++i];
    else if (a == "--write-record" && i + 1 < argc) write_record = argv[++i];
  }
  Plant tp = plant == "endless_retry" ? Plant::EndlessRetry : plant == "dropped_frame_failure" ? Plant::CountFailureIsHole : plant == "offset0" ? Plant::Offset0
             : plant == "ordinal_merge" ? Plant::OrdinalMerge : plant == "stale_slots" ? Plant::StaleSlots : plant == "camera_prev_frame" ? Plant::CameraPrevFrame
             : plant == "late_read" ? Plant::LateRead : plant == "map_a_key_ignored" ? Plant::MapAKeyIgnored : plant == "node_shift" ? Plant::NodeShift
             : plant == "plane_pos_dropped" ? Plant::PlanePosDropped : plant == "plane_r5_palette" ? Plant::PlaneR5Palette : plant == "plane_mats_shift" ? Plant::PlaneMatsShift
             : plant == "product_slot0" ? Plant::ProductSlot0 : plant == "product_prev_frame" ? Plant::ProductPrevFrame : plant == "pose_row_off" ? Plant::PoseRowOff : Plant::None;
  const auto table = LoadTable(sites_path);
  if (table.empty()) {
    std::printf("FAIL  no rows read from %s\n", sites_path.c_str());
    return 1;
  }
  // ---- the table's world: passes, the collapse word, the shapes ----
  uint32_t fn_of[kCpCount] = {};
  uint32_t collapse_table = 0;
  uint32_t shape_wr = 0, shape_wr_obj = 0, shape_props = 0, shape_props_obj = 0, shape_plane_obj = 0, shape_rim_obj = 0;
  for (const auto& r : table) {
    if (r[0] == "pass" && r.size() > 2) {
      const uint8_t p = r[1] == "map" ? kCpMap : r[1] == "plane" ? kCpPlane : r[1] == "rim" ? kCpRim : 0;
      fn_of[p] = uint32_t(std::stoul(r[2], nullptr, 16));
    } else if (r[0] == "collapse" && r.size() > 2) {
      collapse_table = uint32_t(std::stoul(r[2], nullptr, 16));
    } else if (r[0] == "shape" && r.size() > 3) {
      if (r[1] == "map_wrestler") shape_wr = uint32_t(std::stoul(r[2])), shape_wr_obj = uint32_t(std::stoul(r[3]));
      if (r[1] == "map_props") shape_props = uint32_t(std::stoul(r[2])), shape_props_obj = uint32_t(std::stoul(r[3]));
      if (r[1] == "plane") shape_plane_obj = uint32_t(std::stoul(r[3]));
      if (r[1] == "rim") shape_rim_obj = uint32_t(std::stoul(r[3]));
    }
  }

  // ---- the pure rules ----
  {
    bool ok = true;
    for (const PassInfo& p : kPasses)
      if (fn_of[p.pass] != p.fn) ok = false;
    Check(ok && collapse_table == kCollapseTable && PassOfSite(0x82C04634) == kCpMap && PassOfSite(0x82BF4D38) == kCpPlane && PassOfSite(0x82BFBD58) == kCpRim && PassOfSite(0x82C0A950) == kCpNone,
          "table.matches_core", "the three methods and the collapse table address in the core equal the listings table; the pass sites map to their passes");
    Check(shape_wr == 2 && shape_wr_obj == 4 && shape_props == 1 && shape_props_obj == 2 && shape_plane_obj == 1 && shape_rim_obj == 4, "table.shape", "world: 4 wrestlers x (A, B), 2 props, 1 plane object, 4 rim entries");
  }

  // ---- the guest world built from the table ----
  Guest g;
  g.words[0x8392DDA4] = kG;
  g.words[kG + 4] = kArr;
  g.words[kG + 208] = 0x44800000, g.words[kG + 212] = 0x44000000;   // 1024.0f x 512.0f
  g.words[kR13] = kTls;
  g.words[kTls + 1048] = kBlk;
  g.words[kBlk + 32] = kSlot;
  g.words[kArr & ~0xFFFu] = 1;   // page markers (the record region is mapped)
  g.words[kArr] = 0;
  std::vector<Model> wr, props, plane_obj;
  std::map<uint32_t, uint32_t> want;
  for (uint32_t i = 0; i < shape_wr_obj; ++i) {
    Model m;
    m.addr = 0x50000100u + i * 0x1000u, m.pal = 0x60000000u + i * 0x10000u, m.count = 5 + 2 * i;
    g.words[m.addr + 200] = m.pal;
    for (uint32_t k = 0; k < 3; ++k) g.words[m.addr + 408 + 4 * k] = 0x1000u * (i + 1) + k;
    want[m.addr] = m.count;
    wr.push_back(m);
  }
  for (uint32_t i = 0; i < shape_props_obj; ++i) {
    Model m;
    m.addr = 0x50100100u + i * 0x1000u, m.pal = 0, m.count = 0;
    g.words[m.addr + 200] = 0x61000000u + i * 0x1000u;
    for (uint32_t k = 0; k < 3; ++k) g.words[m.addr + 408 + 4 * k] = 0x2000u * (i + 1) + k;
    props.push_back(m);
  }
  for (uint32_t i = 0; i < shape_plane_obj; ++i) {
    Model m;
    m.addr = 0x50200100u + i * 0x1000u;
    g.words[m.addr + 200] = 0x62000000u;
    m.pal = 0x62000000u, m.count = kPlaneNodeWindow;   // lane 226 (E5): a plane entry reads the first kPlaneNodeWindow nodes of [model+200] (r5 is not its palette)
    plane_obj.push_back(m);
  }
  auto rw = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
  auto rb = [&](uint32_t a, void* d, size_t n) { return g.ReadBytes(a, d, n); };
  auto rm = [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); };
  auto wc = [&](uint32_t m) -> uint32_t { auto it = want.find(m); return it == want.end() ? 0u : it->second; };

  // ---- the pure/census rules ----
  {
    EntryR e;
    SetCamera(g, kCamRim, 7);
    g.words[collapse_table + 4 * kSlot] = 1;
    uint32_t flag = 0;
    const bool a = ReadEntryR(rw, rb, kR13, e) == RStatus::Ok && e.slot == kSlot && !e.null_block && e.have_map && ReadCollapse(rw, e.slot, flag) && flag == 1;
    g.words[kBlk + 32] = 99;
    const bool bad = ReadEntryR(rw, rb, kR13, e) == RStatus::BadSlot;
    g.words[kBlk + 32] = kSlot;
    g.words[kTls + 1048] = 0;
    const bool nb = ReadEntryR(rw, rb, kR13, e) == RStatus::Ok && e.slot == 0 && e.null_block;
    g.words[kTls + 1048] = kBlk;
    g.words[collapse_table + 4 * kSlot] = 0;
    Check(a && bad && nb, "pure.entry_read", "R read through [[r13]+1048]+32 (slot 1), the collapse word at table + 4 * slot, a slot >= 16 refused, a null block = slot 0");
  }

  // ---- the oracle run, in live order ----
  CasterContributor C;
  C.tracker.plant = tp;
  const uint32_t kFrames = 60, kFirst = 7000;
  std::vector<Expect> expect_all;                 // every entry the "game" made, in order
  std::vector<std::shared_ptr<SceneTables>> records;
  uint64_t join_ok = 0, join_missing = 0;
  uint64_t counts_wrong = 0, ord_wrong = 0, cam_wrong = 0, col_wrong = 0, hash_wrong = 0, flags_wrong = 0, mapa_wrong = 0, calls_total = 0;
  uint64_t exp_cam_eq = 0, exp_cam_differ = 0, exp_pal_equal = 0;
  const bool late_record = plant == "late_record";
  const uint32_t unreadable_frame = kFirst + 23;
  Model* unreadable_model = &wr[3];
  uint32_t unreadable_expected = 0;
  std::map<uint32_t, std::vector<Expect>> by_frame;
  for (uint32_t f = kFirst; f < kFirst + kFrames; ++f) {
    const bool unr = f == unreadable_frame;
    std::vector<Expect> ex;
    std::map<uint64_t, uint32_t> ord;   // (pass, object) -> entries so far this frame (the oracle's own count)
    std::vector<uint64_t> colour_cams;
    for (const Model& m : wr) WritePalette(g, m, f);
    for (const Model& m : plane_obj) WritePalette(g, m, f, false);
    if (unr) for (uint32_t n = 0; n < 24; ++n) for (uint32_t e = 0; e < 16; ++e) g.words.erase(unreadable_model->pal + 64 * n + 4 * e);   // this frame the palette cannot be read
    const bool a_on = (f % 10) < 8;   // fewer than 3 participants: map A exists
    const auto enter = [&](uint8_t pass, const Model& m, uint32_t key, bool r5_explicit, CamKind cam, bool collapse, bool foreign_r5 = false) {
      SetCamera(g, cam, f);
      g.words[collapse_table + 4 * kSlot] = collapse ? 1u : 0u;
      uint32_t r5 = r5_explicit ? m.pal : 0u;
      if (foreign_r5) {   // another caller's array (not [model+200]) holding the same rows: still what the pass reads
        r5 = m.pal + 0x8000u;
        Model alias = m;
        alias.pal = r5;
        WritePalette(g, alias, f);
        if (unr && m.pal == unreadable_model->pal) for (uint32_t n = 0; n < 24; ++n) for (uint32_t e2 = 0; e2 < 16; ++e2) g.words.erase(r5 + 64 * n + 4 * e2);
      }
      Expect e{};
      e.pass = pass, e.frame = f, e.object = m.addr, e.key = key, e.r5 = r5, e.cam = cam, e.collapse = collapse ? 1 : 0;
      e.ordinal = ord[(uint64_t(pass) << 32) | m.addr]++;
      uint32_t fl = 0;
      if (r5 == 0 && pass != kCpPlane) fl |= kCcR5Zero;   // (lane 226: the plane pass has no palette argument: its r5 is never classified)
      if (foreign_r5) fl |= kCcForeignR5;
      e.n_nodes = m.count;
      e.unreadable = m.count != 0 && m.pal == unreadable_model->pal && unr;
      if (m.count == 0) fl |= kCcNoPalette;
      if (e.unreadable) fl |= kCcUnreadable, e.n_nodes = 0, ++unreadable_expected;
      e.flags = fl;
      e.hash = (m.count != 0 && !e.unreadable) ? ExpectHash(m, f) : 0;
      if (e.hash) e.flags |= pass == kCpPlane ? kCcMats : kCcNodeHashes;   // lane 217: a call with a palette hash carries its per-node hashes; lane 226: a plane call its matrices
      if (pass == kCpMap) for (int k = 0; k < 3; ++k) { uint32_t w = 0; g.ReadWord(m.addr + 408 + 4 * k, w); e.map_a[k] = w; }
      else e.map_a[0] = e.map_a[1] = e.map_a[2] = 0;
      ex.push_back(e);
      C.tracker.OnPassEntry(pass, m.addr, key, r5, kR13, f, rw, rb, rm, wc);
    };
    // the order of a frame (17.5): the props map, map A, map B, the plane mask, the rim map; the colour pass follows
    for (const Model& m : props) enter(kCpMap, m, 0x11, true, kCamProps, false);
    if (a_on) for (const Model& m : wr) enter(kCpMap, m, 0x22, true, kCamMapA, true);
    for (const Model& m : wr) enter(kCpMap, m, 0x22, true, kCamMapB, false);
    for (const Model& m : plane_obj) enter(kCpPlane, m, 0x33, false, kCamPlane, false);
    for (size_t i = 0; i < wr.size(); ++i) enter(kCpRim, wr[i], 0x44, false, kCamRim, false, /*foreign_r5=*/i == 1 && f % 3 == 0);
    // a late call of an older frame (another thread; the sink skipped that frame): counted as old, never a failure
    if (f == kFirst + 20) {
      C.tracker.OnPassEntry(kCpRim, wr[0].addr, 0x44, 0, kR13, f - 3, rw, rb, rm, wc);
      C.tracker.OnPassEntry(kCpMap, wr[0].addr, 0x22, wr[0].pal, kR13, f - 3, rw, rb, rm, wc);
    }
    // the frame boundary: the present's guest frame is f + 1; the colour calls' LightRefs / states and the wrestlers' bones are in the record before this contributor runs
    FrameInfo fi;
    fi.guest_frame = f + 1;
    auto t = std::make_shared<SceneTables>();
    t->header.source = uint8_t(Source::Live);
    t->header.guest_frame = f + 1;
    // colour states: the rim camera and the props camera (the cameras the colour pass is built to share with these entries in this world)
    for (CamKind k : {kCamRim, kCamProps}) {
      LightState s{};
      for (uint32_t m = 0; m < 3; ++m) for (uint32_t i = 0; i < 16; ++i) s.shadow_m[m][i] = CamV(k, f, m * 16 + i);
      t->light_states.push_back(s);
      t->light_refs.push_back(LightRef{wr[0].addr, uint32_t(t->light_refs.size()), uint32_t(t->light_states.size() - 1), f});
    }
    // the wrestlers' bones at this record: the colour pass's copy of the palette (the same frame's rows)
    for (const Model& m : wr) {
      Object o;
      o.bones_first = uint32_t(t->bones.size()), o.bones_count = m.count;
      for (uint32_t n = 0; n < m.count; ++n) {
        Mat4 b;
        for (uint32_t e = 0; e < 16; ++e) b.m[e] = PalV(f, m.addr & 0xFF, n, e);
        t->bones.push_back(b);
      }
      t->objects.push_back(o);
      t->engine_objects.push_back(m.addr);
    }
    Counters cn;
    C.Emit(fi, *t, cn);
    records.push_back(t);
    if (records.size() > 8) records.erase(records.begin());
    const size_t avail = late_record ? records.size() - 1 : records.size();
    // expected census numbers (the oracle's own): camera equal to a colour-call state = the rim and props entries; the rest differ; the palette equals the record's bones for every readable wrestler entry
    for (const Expect& e : ex) {
      (e.cam == kCamRim || e.cam == kCamProps) ? ++exp_cam_eq : ++exp_cam_differ;
      if (e.hash && e.pass != kCpPlane) ++exp_pal_equal;   // (a plane call's matrices are not a wrestler's bones: the census skips it)
    }
    // stream frame f + 1 joins the record that closed hook frame f
    for (const Expect& e : ex) {
      ++calls_total;
      const CasterCall* hit = nullptr;
      const SceneTables* rec = nullptr;
      for (size_t ri = 0; ri < avail; ++ri)
        for (const CasterCall& c : records[ri]->caster_calls)
          if (c.frame == e.frame && c.pass == e.pass && c.object == e.object && c.ordinal == e.ordinal) hit = &c, rec = records[ri].get();
      if (!hit) { ++join_missing; continue; }
      ++join_ok;
      if (hit->key != e.key || hit->r5 != e.r5 || hit->n_nodes != e.n_nodes) ++ord_wrong;
      if (hit->flags != e.flags) ++flags_wrong;
      if (hit->pal_hash != e.hash) ++hash_wrong;
      if (hit->collapse != e.collapse) ++col_wrong;
      if (e.pass == kCpMap && std::memcmp(hit->map_a, e.map_a, 12) != 0) ++mapa_wrong;
      if (e.pass != kCpMap && (hit->map_a[0] || hit->map_a[1] || hit->map_a[2])) ++mapa_wrong;
      if (hit->state == kCasterNoState || hit->state >= rec->caster_states.size() || !CamMatches(rec->caster_states[hit->state], e.cam, e.frame)) ++cam_wrong;
    }
    // counts per pass for this frame, from the record the hook produced (all records are checked through the join above; this one is per-pass counts of the NEWEST record)
    uint32_t cnt[kCpCount] = {};
    for (const CasterCall& c : records.back()->caster_calls) if (c.frame == f) ++cnt[c.pass];
    uint32_t want_cnt[kCpCount] = {};
    for (const Expect& e : ex) ++want_cnt[e.pass];
    for (uint32_t p = 1; p < kCpCount; ++p) if (cnt[p] != want_cnt[p]) ++counts_wrong;
    if (!late_record && cnt[kCpMap] + cnt[kCpRim] + cnt[kCpPlane] == 0 && !ex.empty()) ++counts_wrong;   // a frame with entries must have a record
    by_frame[f] = ex;
    expect_all.insert(expect_all.end(), ex.begin(), ex.end());
  }
  (void)ord_wrong;
  const Stats st = C.tracker.GetStats();
  std::string census;
  C.Census(census);
  std::printf("INFO  %s\n", census.c_str());

  // ---- the checks ----
  Check(join_missing == 0 && join_ok == calls_total && calls_total > 0, "contrib.live_order_join", "joined " + std::to_string(join_ok) + " of " + std::to_string(calls_total) + " entries by (hook stamp, pass, object, ordinal) in live order, missing " + std::to_string(join_missing));
  Check(counts_wrong == 0 && st.recorded > 0, "hook.counts_per_pass", "entries per pass per frame equal the oracle's (wrong frames " + std::to_string(counts_wrong) + ")");
  {
    // the ordinals: every (pass, object) of every expected frame has ordinals 0..k-1 exactly in call order, map A = 0 and B = 1 for a wrestler, one counter per pass
    uint64_t bad = 0;
    std::map<std::tuple<uint32_t, uint8_t, uint32_t>, std::set<uint32_t>> seen;
    for (const auto& rec : records)
      for (const CasterCall& c : rec->caster_calls) seen[{c.frame, c.pass, c.object}].insert(c.ordinal);
    for (const auto& kv : seen) {
      const uint32_t k = uint32_t(kv.second.size());
      for (uint32_t i = 0; i < k; ++i) if (!kv.second.count(i)) ++bad;
    }
    for (const Expect& e : expect_all) {
      if (e.frame < kFirst + kFrames - 8) continue;   // only the last 8 frames' records are kept
      const uint32_t f = e.frame;
      const bool a_on = (f % 10) < 8;
      bool is_wr = false;
      for (const Model& m : wr) if (m.addr == e.object) is_wr = true;
      if (e.pass == kCpMap && is_wr) { const uint32_t want_ord = a_on ? (e.collapse ? 0u : 1u) : 0u; if (e.ordinal != want_ord) ++bad; }
    }
    Check(bad == 0 && !seen.empty(), "hook.ordinals", "per (frame, pass, object) the ordinals are 0..k-1, a wrestler's map A is 0 and map B 1 (B alone is 0 when a third wrestler removes A); bad " + std::to_string(bad));
  }
  Check(cam_wrong == 0 && join_ok > 0, "hook.camera_per_entry", "the camera state of every joined entry equals the camera the game wrote just before THAT entry (shadow matrices, ShadowPrm floats, the two 32-byte blocks); wrong " + std::to_string(cam_wrong));
  Check(col_wrong == 0 && join_ok > 0, "hook.collapse", "the collapse flag is 1 on map A entries and 0 elsewhere; wrong " + std::to_string(col_wrong));
  Check(hash_wrong == 0 && join_ok > 0, "hook.palette_hash", "the palette hash equals this file's FNV over the part's own node count (the junk past it excluded); wrong " + std::to_string(hash_wrong));
  Check(flags_wrong == 0 && mapa_wrong == 0 && join_ok > 0, "hook.flags_and_map_a_words", "r5 zero / foreign / no-palette / unreadable flags and [model+408..416] equal the oracle's; wrong flags " + std::to_string(flags_wrong) + ", wrong map_a " + std::to_string(mapa_wrong));
  Check(st.pal_unreadable == unreadable_expected && st.retries == 0 && unreadable_expected > 0, "hook.unreadable_once", "unreadable palettes recorded once each (" + std::to_string(st.pal_unreadable) + " of " + std::to_string(unreadable_expected) + "), retries " + std::to_string(st.retries));
  Check(st.old_calls == 2 && st.latched == 0, "hook.old_calls_not_failures", "2 calls of an older frame counted as old_calls, latched " + std::to_string(st.latched) + " (a dropped frame is not a failure)");
  Check(st.late == 0 && st.overflow == 0 && st.state_overflow == 0 && st.unreadable == 0 && st.bad_slot == 0, "hook.nothing_refused", "late " + std::to_string(st.late) + ", overflow " + std::to_string(st.overflow) + ", unreadable R " + std::to_string(st.unreadable));
  {
    const Cross x = C.Crossed();
    Check(x.cam_eq_colour == exp_cam_eq && x.cam_differ == exp_cam_differ && x.pal_equal == exp_pal_equal && x.pal_differ == 0 && x.pal_not_in_record == 0 && x.calls == calls_total,
          "contrib.cross_check", "census: camera equal to a colour state " + std::to_string(x.cam_eq_colour) + " of expected " + std::to_string(exp_cam_eq) + ", differ " + std::to_string(x.cam_differ) + " of " + std::to_string(exp_cam_differ) + ", palette equal to the record's bones " + std::to_string(x.pal_equal) + " of " + std::to_string(exp_pal_equal));
  }

  // ---- the null-edit self-test (LESSONS S101): the unedited oracle vs the record = 0 differences; one flipped bit of one expected float = exactly 1 ----
  {
    auto diffs = [&](bool flip) {
      uint64_t d = 0;
      bool flipped = false;
      for (const auto& rec : records)
        for (const CasterCall& c : rec->caster_calls) {
          if (c.frame < kFirst + kFrames - 8 || c.state == kCasterNoState) continue;
          LightState expect{};
          const Expect* ex = nullptr;
          for (const Expect& e : by_frame[c.frame]) if (e.pass == c.pass && e.object == c.object && e.ordinal == c.ordinal) ex = &e;
          if (!ex) { ++d; continue; }
          for (uint32_t m = 0; m < 3; ++m) for (uint32_t i = 0; i < 16; ++i) expect.shadow_m[m][i] = CamV(ex->cam, ex->frame, m * 16 + i);
          if (flip && !flipped) { uint32_t b; std::memcpy(&b, &expect.shadow_m[1][3], 4); b ^= 0x00400000u; std::memcpy(&expect.shadow_m[1][3], &b, 4); flipped = true; }
          if (std::memcmp(expect.shadow_m, rec->caster_states[c.state].shadow_m, sizeof expect.shadow_m) != 0) ++d;
        }
      return d;
    };
    const uint64_t d0 = diffs(false), d1 = diffs(true);
    Check(d0 == 0 && d1 == 1, "oracle.null_edit", "unedited oracle vs record: " + std::to_string(d0) + " differences (0 expected); one flipped bit of one expected float: " + std::to_string(d1) + " (exactly 1 expected)");
  }

  // ---- lane 217: the per-node hash locates a difference between the entry's palette and the record's bones ----
  // A 100-node skeleton (above the 96 cap). The "game" writes its palette at every entry; the record's bones of header H hold the SAME rows except nodes 60 and 71 (+1.0 in one element) and node 98 (past the
  // cap: must never be compared). Calls stamped S are compared with the bones of the record whose HEADER is S (17.8b), which Emit stashed one record earlier. Expected: nodes 60 and 71 differ in every compared
  // call, no other node, the first record's calls have no bones (counted), the control run (identical bones) differs nowhere.
  {
    const uint32_t kN = 100, kFirstN = 9000, kNFrames = 12;
    Model nm;
    nm.addr = 0x50300100u, nm.pal = 0x63000000u, nm.count = kN;
    g.words[nm.addr + 200] = nm.pal;
    auto wcn = [&](uint32_t m) -> uint32_t { return m == nm.addr ? kN : 0u; };
    uint64_t located[2] = {}, exact_ok = 0;
    Cross xs[2];
    for (int control = 0; control < 2; ++control) {
      CasterContributor NC;
      NC.tracker.plant = tp;
      for (uint32_t f = kFirstN; f < kFirstN + kNFrames; ++f) {
        WritePalette(g, nm, f, false);
        SetCamera(g, kCamMapB, f);
        NC.tracker.OnPassEntry(kCpMap, nm.addr, 0x22, nm.pal, kR13, f, rw, rb, rm, wcn);
        SetCamera(g, kCamRim, f);
        NC.tracker.OnPassEntry(kCpRim, nm.addr, 0x44, 0, kR13, f, rw, rb, rm, wcn);
        FrameInfo fi;
        fi.guest_frame = f + 1;
        SceneTables t;
        t.header.source = uint8_t(Source::Live);
        t.header.guest_frame = f + 1;
        Object o;
        o.bones_first = 0, o.bones_count = kN;
        for (uint32_t n = 0; n < kN; ++n) {
          Mat4 b;
          for (uint32_t e = 0; e < 16; ++e) b.m[e] = PalV(f + 1, nm.addr & 0xFF, n, e);
          if (!control && (n == 60 || n == 71 || n == 98)) b.m[3] += 1.0f;
          t.bones.push_back(b);
        }
        t.objects.push_back(o);
        t.engine_objects.push_back(nm.addr);
        Counters cn;
        NC.Emit(fi, t, cn);
        // hold the bones of header f + 1 under "the palette the entry will read at stamp f + 1" = PalV(f + 1): the next iteration writes exactly that
      }
      xs[control] = NC.Crossed();
    }
    const Cross& x = xs[0];
    uint64_t others = 0;
    for (uint32_t k = 0; k < kCasterMaxNodes; ++k)
      if (k != 60 && k != 71 && x.node_differ[k]) others += x.node_differ[k];
    located[0] = x.node_differ[60], located[1] = x.node_differ[71];
    exact_ok = x.node_calls == 2 * kNFrames && x.node_no_bones == 2 && x.node_calls_differ == 2 * (kNFrames - 1) && x.node_calls_equal == 0 && located[0] == 2 * (kNFrames - 1) &&
               located[1] == 2 * (kNFrames - 1) && others == 0 && x.node_first_differ_min == 60 && x.node_short == 0 && x.node_not_in_record == 0;
    Check(exact_ok, "hook.node_located", "a planted difference in nodes 60 and 71 (and a decoy at 98 past the 96 cap) is located exactly: node 60 differs in " + std::to_string(located[0]) + " of " + std::to_string(x.node_calls - x.node_no_bones) +
                                             " compared calls, node 71 in " + std::to_string(located[1]) + ", other nodes " + std::to_string(others) + ", first differing node " + std::to_string(x.node_first_differ_min) + ", calls without bones " + std::to_string(x.node_no_bones) +
                                             "; blocks {" + NodeBlocksText(x) + "}");
    uint64_t any_control = 0;
    for (uint32_t k = 0; k < kCasterMaxNodes; ++k) any_control += xs[1].node_differ[k];
    Check(any_control == 0 && xs[1].node_calls_equal == 2 * (kNFrames - 1) && xs[1].node_calls_differ == 0, "hook.node_control_equal", "identical bones: every compared call equal (" + std::to_string(xs[1].node_calls_equal) + "), no node differs");
    // the pairing (17.8b): the record that carries the call (header stamp + 1) is NOT the bones' record. Bones are stashed under their own header; a call whose stamp has no stash is counted, never compared with another.
    Check(kBonesHeaderForStamp == 0, "contrib.node_pairing", "the bones of a call stamped S are those of the record whose header is S (run 7138: g-1 for stream frame g)");
  }

  // ---- lane 221 (SKIP_GAME_WORK_MAP.md 17.10): the CasterExtra reads: the thread slot's current-camera product, RimLightDepth (G+300 / G+304) and the map-A collapse point (row 3 of the pose matrix [model+408]) ----
  // The "game" rewrites, every frame: the product table for the thread's slot AND for slot 0 (different values: a read of the wrong slot is wrong), G+300 / G+304, the pose matrices. A map entry with the collapse
  // word set reads the pose point; a rim entry (and a map entry without the word) never does. Every call's extra must be the values of ITS frame.
  {
    constexpr uint32_t kXFirst = 9500, kXFrames = 8, kPoseBase = 0x66000000u;
    const auto PV = [](uint32_t slot, uint32_t f, uint32_t i) { return float(slot * 1000u + (f % 300u) * 7u + i) * 0.25f; };
    const auto DV = [](uint32_t f, uint32_t k) { return 400.0f + float(f % 100u) * 0.5f + float(k) * 100.0f; };
    const auto TV = [](uint32_t f, uint32_t i) { return -20.0f + float(f % 50u) + float(i) * 0.125f; };
    const uint32_t xm = 0x50400100u;
    g.words[xm + 192] = kPoseBase, g.words[xm + 408] = 3, g.words[xm + 412] = 57, g.words[xm + 416] = 83;
    auto wcx = [](uint32_t) -> uint32_t { return 0u; };
    CasterContributor XC;
    XC.tracker.plant = tp;
    uint64_t calls = 0, bad_prod = 0, bad_depth = 0, bad_pose = 0, pose_calls = 0, unexpected_pose = 0, bad_flags = 0;
    for (uint32_t f = kXFirst; f < kXFirst + kXFrames + 1; ++f) {
      for (uint32_t slot : {0u, kSlot})
        for (uint32_t i = 0; i < 16; ++i) g.PutFloat(kCameraProductBase + 64u * slot + 4u * i, PV(slot, f, i));
      g.PutFloat(kG + 300, DV(f, 0)), g.PutFloat(kG + 304, DV(f, 1));
      for (uint32_t i = 0; i < 16; ++i) g.PutFloat(kPoseBase + 64u * 3u + 4u * i, i >= 12 ? TV(f, i) : 0.f);
      SetCamera(g, kCamMapA, f);
      g.words[collapse_table + 4 * kSlot] = 1;
      XC.tracker.OnPassEntry(kCpMap, xm, 0x22, 0, kR13, f, rw, rb, rm, wcx);
      g.words[collapse_table + 4 * kSlot] = 0;
      SetCamera(g, kCamMapB, f);
      XC.tracker.OnPassEntry(kCpMap, xm, 0x22, 0, kR13, f, rw, rb, rm, wcx);
      SetCamera(g, kCamRim, f);
      XC.tracker.OnPassEntry(kCpRim, xm, 0x44, 0, kR13, f, rw, rb, rm, wcx);
      FrameInfo fi;
      fi.guest_frame = f + 1;
      SceneTables t;
      t.header.source = uint8_t(Source::Live);
      t.header.guest_frame = f + 1;
      Counters cn;
      XC.Emit(fi, t, cn);
      if (t.caster_extras.size() != t.caster_calls.size()) ++bad_flags;
      for (size_t i = 0; i < t.caster_calls.size() && i < t.caster_extras.size(); ++i) {
        const CasterCall& c = t.caster_calls[i];
        const CasterExtra& e = t.caster_extras[i];
        ++calls;
        const uint32_t cf = c.frame;
        bool pr = (e.flags & kCxProduct) != 0, dp = (e.flags & kCxDepth) != 0;
        for (uint32_t k = 0; k < 16 && pr; ++k) pr = e.product[k] == PV(kSlot, cf, k);
        bad_prod += !pr;
        bad_depth += !(dp && e.depth[0] == DV(cf, 0) && e.depth[1] == DV(cf, 1));
        if (c.pass == kCpMap && c.collapse) {
          ++pose_calls;
          bool po = (e.flags & kCxPose) != 0;
          for (uint32_t k = 0; k < 4 && po; ++k) po = e.pose_t[k] == TV(cf, 12 + k);
          bad_pose += !po;
        } else {
          unexpected_pose += (e.flags & kCxPose) != 0;
        }
      }
    }
    Check(calls == 3 * (kXFrames + 1) && bad_prod == 0 && bad_depth == 0 && bad_flags == 0, "hook.extra_per_entry", std::to_string(calls) + " calls: the camera product is the thread slot's, of the call's own frame, in " + std::to_string(calls - bad_prod) + "; G+300 / G+304 equal in " + std::to_string(calls - bad_depth) + "; extras parallel to the calls");
    Check(pose_calls == kXFrames + 1 && bad_pose == 0 && unexpected_pose == 0, "hook.pose_point", std::to_string(pose_calls) + " map-A calls read the pose matrix's row 3 (bad " + std::to_string(bad_pose) + "); " + std::to_string(unexpected_pose) + " other calls read a pose (0 expected)");
    // the record code of the section + the rows rule
    SceneTables rt;
    rt.header.guest_frame = 4;
    for (uint32_t i = 0; i < 3; ++i) {
      CasterCall c;
      c.pass = uint8_t(1 + i), c.object = 0x50400000u + i, c.frame = 4, c.ordinal = i;
      rt.caster_calls.push_back(c);
      CasterExtra e;
      e.flags = kCxProduct | kCxDepth | (i == 0 ? kCxPose : 0u);
      for (uint32_t k = 0; k < 16; ++k) e.product[k] = PV(1, 4, k);
      e.depth[0] = 11.5f, e.depth[1] = 22.5f;
      for (uint32_t k = 0; k < 4; ++k) e.pose_t[k] = float(k) + 0.5f;
      rt.caster_extras.push_back(e);
    }
    const std::vector<uint8_t> xa = SerializeRecord(rt);
    SceneTables xback;
    std::string xerr;
    const bool xparsed = ParseRecord(xa.data(), xa.size(), xback, xerr);
    Check(xparsed && xback.caster_extras.size() == 3 && std::memcmp(xback.caster_extras.data(), rt.caster_extras.data(), 3 * sizeof(CasterExtra)) == 0 && xa == SerializeRecord(xback) && std::string(SectionName(kScCasterExtras)) == "CasterExtra",
          "record.extras_round_trip", "write -> read -> write byte-identical; 3 extras, section name");
    SceneTables xbad = rt;
    xbad.caster_extras.pop_back();   // not parallel to the 3 calls
    const std::vector<uint8_t> xb = SerializeRecord(xbad);
    SceneTables xbback;
    Check(ParseRecord(xb.data(), xb.size(), xbback, xerr) && xbback.caster_calls.empty() && xbback.caster_extras.empty() && xbback.caster_errors == 1, "record.bad_extras_drops_caster_only", "extras that are not parallel to the calls drop the caster tables only");
    // BuildCasterRows + ApplyCasterExtra: the rim takes the product (c56 + i = COLUMN i), c54 = c59, c162 = (depth0, depth1, 0, 0); a map keeps R's camera
    LightState ls{};
    for (uint32_t i = 0; i < 16; ++i) ls.shadow_m[0][i] = 1000.f + float(i);
    CasterRows rows = BuildCasterRows(ls);
    CasterRows rim = rows, mp = rows;
    ApplyCasterExtra(rim, rt.caster_extras[2], true);
    ApplyCasterExtra(mp, rt.caster_extras[0], false);
    bool cols = rim.rim_cam_valid && rim.rim_light_depth_valid && rim.rim_light_depth[0] == 11.5f && rim.rim_light_depth[1] == 22.5f && rim.rim_light_depth[2] == 0.f;
    for (int a = 0; a < 4 && cols; ++a)
      for (int k = 0; k < 4; ++k) cols = cols && rim.view_proj[a][k] == rt.caster_extras[2].product[k * 4 + a];
    cols = cols && std::memcmp(rim.view_depth, rim.view_proj[3], 16) == 0 && !mp.rim_cam_valid && std::memcmp(mp.view_proj, rows.view_proj, sizeof rows.view_proj) == 0;
    Check(cols, "rows.rim_camera", "the rim's c56 + i = column i of the product, c54 = c59, c162 = (G+300, G+304, 0, 0); a map pass keeps R's camera");
  }

  // ---- lane 226 (SKIP_GAME_WORK_MAP.md 17.11, E5): a PLANE entry's palette is [model+200] (r5 is junk: run 7149 left 0x2b6, 0x33a, .. in it), its matrices go to kScCasterMats ----
  {
    constexpr uint32_t kPFirst = 9700, kPFrames = 6;
    const uint32_t pm = 0x50500100u, pal = 0x67000000u;
    g.words[pm + 200] = pal;
    const auto MV = [](uint32_t f, uint32_t node, uint32_t i) { return float(node * 100u + (f % 50u) * 3u + i) * 0.5f; };
    auto wcp = [](uint32_t) -> uint32_t { return 0u; };   // the plane proxies are in no wrestler table
    CasterContributor PC;
    PC.tracker.plant = tp;
    uint64_t calls = 0, bad_mats = 0, bad_flags = 0, with_mats = 0, bad_pos = 0, pos_calls = 0, stray_pos = 0;
    const auto PP = [](uint32_t f, uint32_t call, uint32_t k) { return float(f % 90u) * 0.5f + float(call) * 4.f + float(k); };
    for (uint32_t f = kPFirst; f < kPFirst + kPFrames + 1; ++f) {
      for (uint32_t node = 0; node < kPlaneNodeWindow; ++node)
        for (uint32_t i = 0; i < 16; ++i) g.PutFloat(pal + 64u * node + 4u * i, MV(f, node, i));
      SetCamera(g, kCamPlane, f);
      for (uint32_t call = 0; call < 2; ++call) {
        const float pos[4] = {PP(f, call, 0), PP(f, call, 1), PP(f, call, 2), PP(f, call, 3)};
        PC.tracker.OnPassEntry(kCpPlane, pm, 0x33, 0x2b6u + 0x84u * call + f, kR13, f, rw, rb, rm, wcp, pos);
      }
      PC.tracker.OnPassEntry(kCpRim, pm, 0x44, 0, kR13, f, rw, rb, rm, wcp, nullptr);
      FrameInfo fi;
      fi.guest_frame = f + 1;
      SceneTables t;
      t.header.source = uint8_t(Source::Live);
      t.header.guest_frame = f + 1;
      Counters cn;
      PC.Emit(fi, t, cn);
      for (size_t ci = 0; ci < t.caster_calls.size(); ++ci) {
        const CasterCall& c = t.caster_calls[ci];
        const CasterExtra& ex = t.caster_extras[ci];
        if (c.pass != kCpPlane) { stray_pos += (ex.flags & kCxPlanePos) != 0; continue; }
        ++pos_calls;
        bool pok = (ex.flags & kCxPlanePos) != 0;
        for (uint32_t k = 0; k < 4 && pok; ++k) pok = ex.pose_t[k] == PP(c.frame, c.ordinal, k);
        bad_pos += !pok;
        ++calls;
        bad_flags += (c.flags & (kCcR5Zero | kCcForeignR5)) != 0 || (c.flags & kCcNodeHashes) != 0;
        bool ok = (c.flags & kCcMats) != 0 && c.n_nodes == kPlaneNodeWindow && uint64_t(c.node_first) + c.n_nodes <= t.caster_mats.size();
        for (uint32_t node = 0; node < kPlaneNodeWindow && ok; ++node)
          for (uint32_t i = 0; i < 16 && ok; ++i) ok = t.caster_mats[c.node_first + node].m[i] == MV(c.frame, node, i);
        with_mats += (c.flags & kCcMats) != 0;
        bad_mats += !ok;
      }
      t.caster_errors = 0;
    }
    Check(calls == 2 * (kPFrames + 1) && bad_mats == 0 && bad_flags == 0 && with_mats == calls, "hook.plane_mats",
          std::to_string(calls) + " plane calls: the matrices are [model+200]'s, of the call's own frame, node by node, in " + std::to_string(calls - bad_mats) + " (r5 never the palette; neither r5-zero nor foreign flagged: " + std::to_string(bad_flags) + " flagged)");
    Check(pos_calls == calls && bad_pos == 0 && stray_pos == 0, "hook.plane_pos", std::to_string(pos_calls) + " plane calls: the c7 snapshot handed to the entry is in the call's own extra (kCxPlanePos) in " + std::to_string(pos_calls - bad_pos) + "; a rim entry never carries one (" + std::to_string(stray_pos) + " stray)");
    SceneTables mt;
    mt.header.guest_frame = 6;
    for (uint32_t i = 0; i < 2; ++i) {
      CasterCall c;
      c.pass = kCpPlane, c.object = 0x50500100u, c.frame = 6, c.ordinal = i, c.n_nodes = 3, c.flags = kCcMats, c.node_first = 3 * i;
      mt.caster_calls.push_back(c);
    }
    for (uint32_t k = 0; k < 6; ++k) {
      Mat4 m{};
      for (uint32_t i = 0; i < 16; ++i) m.m[i] = float(k * 16 + i);
      mt.caster_mats.push_back(m);
    }
    const std::vector<uint8_t> ma = SerializeRecord(mt);
    SceneTables mback;
    std::string merr;
    const bool mparsed = ParseRecord(ma.data(), ma.size(), mback, merr);
    Check(mparsed && mback.caster_mats.size() == 6 && std::memcmp(mback.caster_mats.data(), mt.caster_mats.data(), 6 * sizeof(Mat4)) == 0 && ma == SerializeRecord(mback) && std::string(SectionName(kScCasterMats)) == "CasterMat" && mback.caster_calls.size() == 2,
          "record.mats_round_trip", "write -> read -> write byte-identical; 6 matrices, section 26 name");
    SceneTables mbad = mt;
    mbad.caster_calls[1].node_first = 5;   // 5 + 3 > 6: past the pool
    const std::vector<uint8_t> mb = SerializeRecord(mbad);
    SceneTables mbback;
    Check(ParseRecord(mb.data(), mb.size(), mbback, merr) && mbback.caster_calls.empty() && mbback.caster_mats.empty() && mbback.caster_errors == 1, "record.bad_mats_drops_caster_only", "a matrix range past the pool drops the caster tables only");
  }

  // ---- the record code ----
  {
    SceneTables t;
    t.header.guest_frame = 9;
    t.caster_states.resize(2);
    for (uint32_t i = 0; i < 16; ++i) t.caster_states[1].shadow_m[1][i] = float(i) * 0.25f;
    for (uint32_t i = 0; i < 5; ++i) {
      CasterCall c;
      c.pass = uint8_t(1 + i % 3), c.object = 0x50000000u + i, c.frame = 9, c.ordinal = i, c.state = i % 2, c.key = 7 + i, c.palette = 0x60000000u, c.n_nodes = uint16_t(i), c.pal_hash = 0x1234567800000000ull + i;
      c.map_a[1] = i;
      t.caster_calls.push_back(c);
    }
    {   // lane 217: per-node hashes: calls 1..4 carry i hashes each (flag + node_first), call 0 none
      uint32_t first = 0;
      for (uint32_t i = 1; i < 5; ++i) {
        t.caster_calls[i].flags |= kCcNodeHashes, t.caster_calls[i].node_first = first;
        for (uint32_t k = 0; k < i; ++k) t.caster_nodes.push_back(0xA0000000u + 16 * i + k);
        first += i;
      }
    }
    const std::vector<uint8_t> a = SerializeRecord(t);
    SceneTables back;
    std::string err;
    const bool parsed = ParseRecord(a.data(), a.size(), back, err);
    Check(parsed && back.caster_calls.size() == 5 && back.caster_states.size() == 2 && a == SerializeRecord(back) && std::memcmp(back.caster_calls.data(), t.caster_calls.data(), 5 * sizeof(CasterCall)) == 0 &&
              back.caster_errors == 0 && std::string(SectionName(kScCasterCalls)) == "CasterCall", "record.round_trip_bytes", "write -> read -> write byte-identical; 5 calls, 2 states");
    Check(parsed && back.caster_nodes == t.caster_nodes && back.caster_nodes.size() == 10 && std::string(SectionName(kScCasterNodes)) == "CasterNode", "record.node_round_trip", "the per-node hash pool (10 entries) survives write -> read -> write");
    {
      SceneTables badn = t;
      badn.caster_calls[4].node_first = 8;   // 8 + 4 nodes past the 10 pool entries
      const std::vector<uint8_t> bn = SerializeRecord(badn);
      SceneTables bnback;
      Check(ParseRecord(bn.data(), bn.size(), bnback, err) && bnback.caster_calls.empty() && bnback.caster_nodes.empty() && bnback.caster_errors == 1, "record.bad_node_index_drops_caster_only", "a node-hash index outside the pool drops the caster tables only (counted)");
    }
    SceneTables old;
    old.header.guest_frame = 5;
    const std::vector<uint8_t> o = SerializeRecord(old);
    SceneTables oback;
    Check(ParseRecord(o.data(), o.size(), oback, err) && oback.caster_calls.empty() && oback.caster_errors == 0, "record.older_record_opens", "a record without the sections opens, no caster table");
    SceneTables bad = t;
    bad.caster_calls[2].state = 77;   // outside the two states
    const std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables bback;
    Check(ParseRecord(bb.data(), bb.size(), bback, err) && bback.caster_calls.empty() && bback.caster_states.empty() && bback.caster_errors == 1, "record.bad_state_drops_caster_only", "a state index outside the table drops the caster tables only (counted), the rest of the record is read");
    SceneTables bad2 = t;
    bad2.caster_calls[0].layout = 9;
    const std::vector<uint8_t> b2 = SerializeRecord(bad2);
    SceneTables b2back;
    Check(ParseRecord(b2.data(), b2.size(), b2back, err) && b2back.caster_calls.empty() && b2back.caster_errors == 1, "record.unknown_layout_drops_caster_only", "an unknown layout drops the caster tables only");
    std::string verr;
    Check(!live::Validate(bad, verr) && verr.find("CasterCall") != std::string::npos && live::Validate(t, verr), "record.validate", "the live writer refuses a bad caster table and accepts a good one");
    if (!write_record.empty() && !records.empty()) {
      const SceneTables* pick = records.back().get();   // the newest record that holds a map A entry (collapse 1): the sample shows all three passes, a collapse flag and palette hashes
      for (size_t i = records.size(); i-- > 0;) {
        bool has_a = false;
        for (const CasterCall& c : records[i]->caster_calls) has_a = has_a || c.collapse == 1;
        if (has_a) { pick = records[i].get(); break; }
      }
      const std::vector<uint8_t> bytes = SerializeRecord(*pick);
      std::ofstream(write_record, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
  }

  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
