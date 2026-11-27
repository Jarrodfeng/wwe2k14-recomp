// arena_props_test: the offline proof of S111 ARENAFIX (docs\research\s111\ARENAFIX.md). 0 game runs, no SDK; reads the player's files only.
//   arena_props_test --game DIR [--plant fixed32 | all_props | loader_fixed | desk67 | old_desk | desk_slot0 | gate_off | unsigned_counts]
// Runs the SHIPPED parsers (src\assets\arena_props_table.h) on built bytes, and the SHIPPED loader read (src\assets\asset_load.h load_detail::ArenaPropsPlan) on the player's
// pac\misc01_Start.pac p0/2 and pac\bg\bgEtc.pac p10/45010. Checks (a planted twin must fail exactly its named checks; exit 1 when any check failed):
//   ap.parse      built bytes: record byte 28 by arena id (200..249 -> 200, a miss 0, a short leaf -1); the arena block's group ids (the key's reference followed, the key
//                 chunk's own records not read, a negative reference = nothing); a cut table and a table without its 'ASH ' block refused; the plan (the desk / top
//                 by any id sub_8258CE20 maps to them: 6 / 106 / 105, 7 / 107 / 104)
//   ap.gate       S111 PLACECOUNT0, built bytes: block 1's count 0 / -1 / -32768 or its byte 0 = 2: nothing placed, the table read (the loader's gate);   <- gate_off
//                 count 1, byte 0 = 1: the arena block's 6 / 32  (gate_off = load_detail::PlacementGateOffPlant, the parse without the gate)
//   ap.signed     S111 PLACESIGNED, built bytes: a count of -1 (0xFFFF) on a SubH chunk's records (both SubH blocks), the 'ASH ' block or an 'ASH ' head:   <- unsigned_counts
//                 none, the table read, arena 5's group 6 / 32 placed  (unsigned_counts = load_detail::UnsignedCountsPlant, the counts read unsigned)
//   ap.variant    the player's files: arenas 2, 36, 41, 50 -> steps 35; 28, 35, 40 -> 29; 0, 4, 23, 56, 200, 249 -> 32                              <- fixed32
//   ap.presence   the player's files: 21, 22 -> desk and top, no steps; 0, 4, 200 -> steps, desk and top; 66, 78, 80 -> nothing                      <- all_props
//   ap.load       the SHIPPED LoadArena on the player's files: bg21 has the desk and no steps; bg28 / bg36 / bg04 carry steps entry 29 / 35 / 32       <- loader_fixed
//                 (loader_fixed = load_detail::AlwaysPropsPlant, the loader's old fixed choice: steps 32 and the desk on every ring arena)
//   ap.desk       S111 DESKMODEL: the desk / top entries by the record's bytes 64 / 65 (DeskKey / TopKey; built bytes and the player's files: 28 / 42 -> 106 / 107,
//                 43 / 45 -> 105 / 107, 46 / 51 -> 6 / 107, 0 / 21 / 52 / 200 -> 6 / 7)                                                              <- desk67
//   ap.deskload   S111 DESKSKIN: the SHIPPED LoadArena: bg04 / bg28 / bg43 / bg46 load those entries and every desk / top VERTEX sits at its own palette slot's node row,
//                 against the file's vertices (AddProp into an empty model) and rows written here (desk bone01 (0, 0, -775.4), other desk nodes (0, 0, -760), top
//                 (0, -48.25, -760)); bg04 desk 6 mesh 0's 113 plate vertices (bone04) at -760                                                  <- old_desk, desk_slot0
//                 (old_desk = load_detail::OldDeskPlant, the loader's old 6 / 7 in every arena; desk_slot0 = load_detail::DeskSlot0Plant, each mesh whole on slot 0)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

namespace AP = assets::arena_props;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* why) {
  std::printf("%s  %-14s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) g_fail = 1;
}

static bool g_fixed32 = false;    // planted: the old rule, steps 32 on every arena
static bool g_all_props = false;  // planted: the old rule, steps and desk on every ring arena
static bool g_desk67 = false;     // planted: the old rule, desk 6 and top 7 on every arena
static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void Put16(std::vector<uint8_t>& b, int v) { b.push_back(uint8_t(v & 255)), b.push_back(uint8_t((v >> 8) & 255)); }
static void Tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }
static void Head(std::vector<uint8_t>& b, int count) { b.push_back(0x01), b.push_back(0xCC), Put16(b, count); }
static void Sub(std::vector<uint8_t>& b, int key, int n, int ref) { Tag(b, "SubH"), Put16(b, key), Put16(b, n), Put16(b, ref), Put16(b, 0); }
static void Rec(std::vector<uint8_t>& b, int id) {   // a 36-byte 'Data' record (the player's file: the id at byte 31, bytes 32..35 zero)
  Tag(b, "Data");
  b.push_back(36), b.push_back(0), b.push_back(0), b.push_back(0);
  for (int i = 8; i < 31; ++i) b.push_back(0);
  b.push_back(uint8_t(id));
  for (int i = 32; i < 36; ++i) b.push_back(0);
}
static void Ash(std::vector<uint8_t>& b, int id, int n) {   // an 'ASH ' head and its n 8-byte entries (the loader sub_8258F698 skips over them)
  Tag(b, "ASH "), Put16(b, id), Put16(b, n);
  for (int j = 0; j < n; ++j) Tag(b, "ADat"), b.push_back(uint8_t(j)), b.push_back(4), b.push_back(0xCC), b.push_back(0xCC);
}

static std::vector<uint8_t> BuiltStg() {   // three records: 5 -> byte 28 = 1, 200 -> 2, 7 -> 0; bytes 64 / 65: 5 -> 1 / 1, 200 -> 2 / 0, 7 -> 0 / 0
  std::vector<uint8_t> b(12, 0);
  b[4] = 3;
  const int rows[3][4] = {{5, 1, 1, 1}, {200, 2, 2, 0}, {7, 0, 0, 0}};
  for (const auto& r : rows) {
    std::vector<uint8_t> rec(108, 0);
    rec[4] = uint8_t(r[0]), rec[28] = uint8_t(r[1]), rec[64] = uint8_t(r[2]), rec[65] = uint8_t(r[3]);
    b.insert(b.end(), rec.begin(), rec.end());
  }
  return b;
}

// Block 1 (a decoy key 5 -> 1000), the 'ASH ' block, block 3 = the arena block: 5 -> 1001 [6, 32] (the key chunk carries its own record 99, which the game does not read, as
// the player's block 1 keys do), 200 -> 1002 [7], 9 -> 1003 (no group), 8 -> -1 (nothing), 3 -> 1004 [106, 107, 35] (the player's unkeyed group 1001).
static std::vector<uint8_t> BuiltSubh(bool with_ash = true) {
  std::vector<uint8_t> b;
  Head(b, 1), Sub(b, 5, 0, 1000);
  if (with_ash) Head(b, 2), Ash(b, 0, 2), Ash(b, 1, 1);
  Head(b, 8), Sub(b, 5, 1, 1001), Rec(b, 99), Sub(b, 200, 0, 1002), Sub(b, 9, 0, 1003), Sub(b, 8, 0, -1), Sub(b, 3, 0, 1004);
  Sub(b, 1001, 2, -1), Rec(b, 6), Rec(b, 32);
  Sub(b, 1002, 1, -1), Rec(b, 7);
  Sub(b, 1004, 3, -1), Rec(b, 106), Rec(b, 107), Rec(b, 35);
  return b;
}

// S111 PLACECOUNT0: block 1's head as given (u8 byte0, u8 0xCC, u16 count1; count1 chunks 7 -> 1000 when count1 > 0), the 'ASH ' block, then an arena block 5 -> 1001 [6, 32]:
// whether the loader sub_8258F698 builds any of it rests on block 1's head alone.
static std::vector<uint8_t> GateSubh(int count1, int byte0) {
  std::vector<uint8_t> b;
  b.push_back(uint8_t(byte0)), b.push_back(0xCC), Put16(b, count1);
  for (int c = 0; c < count1; ++c) Sub(b, 7, 0, 1000);
  Head(b, 1), Ash(b, 0, 1);
  Head(b, 2), Sub(b, 5, 0, 1001), Sub(b, 1001, 2, -1), Rec(b, 6), Rec(b, 32);
  return b;
}

// S111 PLACESIGNED: counts of -1 (0xFFFF) where the loader sub_8258F698 reads a signed halfword and takes 0 or less as none, building on: block 1's chunk 7 -> 1000
// with record count -1; the 'ASH ' block's own count -1 (`ash_neg`) or one head with entry count -1; the arena block 5 -> 1001 [6, 32] whose key chunk 5 has record
// count -1, and 9 -> 1003 whose group chunk 1003 has record count -1 (the key found, nothing placed).
static std::vector<uint8_t> SignedSubh(bool ash_neg) {
  std::vector<uint8_t> b;
  Head(b, 1), Sub(b, 7, -1, 1000);
  if (ash_neg) Head(b, -1);
  else Head(b, 1), Ash(b, 0, -1);
  Head(b, 4), Sub(b, 5, -1, 1001), Sub(b, 1001, 2, -1), Rec(b, 6), Rec(b, 32), Sub(b, 9, 0, 1003), Sub(b, 1003, -1, -1);
  return b;
}

int main(int argc, char** argv) {
  std::string game;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) game = argv[++i];
    else if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc) {
      ++i;
      g_fixed32 = std::strcmp(argv[i], "fixed32") == 0;
      g_all_props = std::strcmp(argv[i], "all_props") == 0;
      if (std::strcmp(argv[i], "loader_fixed") == 0) assets::load_detail::AlwaysPropsPlant() = true;
      g_desk67 = std::strcmp(argv[i], "desk67") == 0;
      if (std::strcmp(argv[i], "old_desk") == 0) assets::load_detail::OldDeskPlant() = true;
      if (std::strcmp(argv[i], "desk_slot0") == 0) assets::load_detail::DeskSlot0Plant() = true;
      if (std::strcmp(argv[i], "gate_off") == 0) assets::load_detail::PlacementGateOffPlant() = true;
      if (std::strcmp(argv[i], "unsigned_counts") == 0) assets::load_detail::UnsignedCountsPlant() = true;
    }
  }

  const std::vector<uint8_t> stg = BuiltStg(), sh = BuiltSubh();
  bool parse = AP::RecordByte28(stg, 5) == 1 && AP::RecordByte28(stg, 230) == 2 && AP::RecordByte28(stg, 7) == 0 && AP::RecordByte28(stg, 9) == 0 &&
               AP::RecordByte28(std::vector<uint8_t>(8, 0), 5) == -1;
  std::vector<int> ids;
  bool found = false;
  parse = parse && AP::PlacementIds(sh, 5, ids, found) && found && ids == std::vector<int>{6, 32};
  parse = parse && AP::PlacementIds(sh, 249, ids, found) && found && ids == std::vector<int>{7};
  parse = parse && AP::PlacementIds(sh, 9, ids, found) && found && ids.empty();
  parse = parse && AP::PlacementIds(sh, 8, ids, found) && found && ids.empty();
  parse = parse && AP::PlacementIds(sh, 4, ids, found) && !found && ids.empty();
  const std::vector<uint8_t> cut(sh.begin(), sh.end() - 1);
  parse = parse && !AP::PlacementIds(cut, 5, ids, found);
  parse = parse && !AP::PlacementIds(BuiltSubh(false), 5, ids, found);   // the blocks are read in the loader's order: no 'ASH ' block = not the table
  const AP::Plan p5 = AP::PlanOf(stg, sh, 5), p9 = AP::PlanOf(stg, sh, 9), bad = AP::PlanOf(std::vector<uint8_t>(8, 0), sh, 5);
  parse = parse && p5.read && p5.steps && p5.desk && !p5.top && p5.steps_key == 29u;
  parse = parse && p9.read && !p9.steps && !p9.desk && !p9.top && p9.steps_key == 32u;
  parse = parse && !bad.read && bad.steps && bad.desk && bad.top && bad.steps_key == 32u;
  const AP::Plan p3 = AP::PlanOf(stg, sh, 3);   // 106 / 107 are the desk / top slot too (sub_8258CE20 maps 6 / 106 / 105 and 7 / 107 / 104 through the same tables)
  parse = parse && p3.read && p3.steps && p3.desk && p3.top && p3.steps_key == 32u;
  Check(parse, "ap.parse", "built bytes: byte 28 by id (200..249 -> 200), the arena block's group ids (by the key's reference), a cut or ASH-less table refused, the plan");

  // S111 PLACECOUNT0: the loader sub_8258F698 builds no block when block 1's count (a signed halfword) is <= 0 or its byte 0 is not 1, and the fresh table (sub_8258C878:
  // counts 0, block pointers null) holds no key for sub_8258CA78: the arena places nothing, the table read. The control (count 1, byte 0 = 1) places 6 / 32.
  bool gate = AP::PlacementIds(GateSubh(1, 1), 5, ids, found) && found && ids == std::vector<int>{6, 32};
  for (const auto& h : {std::pair<int, int>{0, 1}, {-1, 1}, {-32768, 1}, {1, 2}})
    gate = gate && AP::PlacementIds(GateSubh(h.first, h.second), 5, ids, found) && !found && ids.empty();
  const AP::Plan g0 = AP::PlanOf(stg, GateSubh(0, 1), 5);
  gate = gate && g0.read && !g0.steps && !g0.desk && !g0.top;
  Check(gate, "ap.gate", "built bytes: block 1's count 0 / -1 / -32768 or byte 0 = 2: nothing placed, the table read (the loader's gate); count 1, byte 0 = 1: 6 / 32");

  // S111 PLACESIGNED: the loader sub_8258F698 reads every count after its gate as a signed halfword (extsh) and takes 0 or less as none, building on (+0x268 / +0x608
  // a chunk's records, +0x3D4 / +0x3DC the 'ASH ' block, +0x40C an 'ASH ' head's entries): -1 there leaves the arena's group as it was.
  bool sgn = true;
  for (bool ash_neg : {true, false}) {
    const std::vector<uint8_t> s = SignedSubh(ash_neg);
    sgn = sgn && AP::PlacementIds(s, 5, ids, found) && found && ids == std::vector<int>{6, 32};
    sgn = sgn && AP::PlacementIds(s, 9, ids, found) && found && ids.empty();
  }
  Check(sgn, "ap.signed", "built bytes: a -1 count on a SubH chunk's records, the 'ASH ' block or an 'ASH ' head: none, the table read, arena 5's group 6 / 32 placed");

  launcher::GameFiles files;
  if (!game.empty()) files.SetRoot(game);
  const auto real = [&](int id) {
    AP::Plan p = assets::load_detail::ArenaPropsPlan(files, id);
    if (g_fixed32) p.steps_key = 32;
    if (g_all_props) p.steps = p.desk = p.top = true;
    if (g_desk67) p.desk_key = 6, p.top_key = 7;
    return p;
  };
  bool variant = !game.empty();
  for (int id : {2, 36, 41, 50}) variant = variant && real(id).read && real(id).steps_key == 35u;
  for (int id : {28, 35, 40}) variant = variant && real(id).read && real(id).steps_key == 29u;
  for (int id : {0, 4, 23, 56, 200, 249}) variant = variant && real(id).read && real(id).steps_key == 32u;
  Check(variant, "ap.variant", "the player's files: 2 / 36 / 41 / 50 -> 35, 28 / 35 / 40 -> 29, 0 / 4 / 23 / 56 / 200 / 249 -> 32");

  bool presence = !game.empty();
  for (int id : {21, 22}) {
    const AP::Plan p = real(id);
    presence = presence && p.read && !p.steps && p.desk && p.top;
  }
  for (int id : {0, 4, 200}) {
    const AP::Plan p = real(id);
    presence = presence && p.read && p.steps && p.desk && p.top;
  }
  for (int id : {66, 78, 80}) {
    const AP::Plan p = real(id);
    presence = presence && p.read && !p.steps && !p.desk && !p.top;
  }
  Check(presence, "ap.presence", "the player's files: 21 / 22 desk + top only, 0 / 4 / 200 all three, 66 / 78 / 80 none");

  // The shipped loader end to end: LoadArena's props by its plan (the mesh's src_key = the bgEtc PACH 9 entry AddProp read).
  const auto props_of = [&](int id, std::vector<int>& steps_keys, int& desk) {
    assets::Model m;
    std::string err;
    steps_keys.clear(), desk = 0;
    if (game.empty() || !assets::LoadArena(files, id, m, err)) return false;
    for (const assets::Mesh& me : m.meshes) {
      if (me.part.rfind("ring steps", 0) == 0) steps_keys.push_back(me.src_key);
      else if (me.part.rfind("announce desk", 0) == 0) ++desk;
    }
    return true;
  };
  bool load = !game.empty();
  std::vector<int> sk;
  int desk = 0;
  load = load && props_of(21, sk, desk) && sk.empty() && desk > 0;
  for (const auto& want : {std::pair<int, int>{28, 29}, {36, 35}, {4, 32}}) {
    load = load && props_of(want.first, sk, desk) && !sk.empty() && desk > 0;
    for (int k : sk) load = load && k == want.second;
  }
  Check(load, "ap.load", "the shipped LoadArena: bg21 desk and no steps; bg28 / bg36 / bg04 steps entry 29 / 35 / 32");

  // S111 DESKMODEL: the desk / top entries by the record's bytes 64 / 65 (sub_825DE388 / sub_825DE3C0).
  bool deskk = AP::DeskKey(0) == 6u && AP::DeskKey(1) == 106u && AP::DeskKey(2) == 105u && AP::DeskKey(3) == 6u && AP::TopKey(0) == 7u && AP::TopKey(1) == 107u &&
               AP::TopKey(2) == 104u && AP::TopKey(-1) == 7u;
  const AP::Plan d5 = AP::PlanOf(stg, sh, 5), d230 = AP::PlanOf(stg, sh, 230), d9 = AP::PlanOf(stg, sh, 9);
  deskk = deskk && d5.read && d5.desk_key == 106u && d5.top_key == 107u && d230.read && d230.desk_key == 105u && d230.top_key == 7u && d9.read && d9.desk_key == 6u &&
          d9.top_key == 7u && !game.empty();
  for (const auto& w : {std::array<int, 3>{28, 106, 107}, {42, 106, 107}, {43, 105, 107}, {45, 105, 107}, {46, 6, 107}, {51, 6, 107}, {0, 6, 7}, {21, 6, 7}, {52, 6, 7}, {200, 6, 7}}) {
    const AP::Plan p = real(w[0]);
    deskk = deskk && p.read && p.desk_key == uint32_t(w[1]) && p.top_key == uint32_t(w[2]);
  }
  Check(deskk, "ap.desk", "desk / top entries by bytes 64 / 65: 28 / 42 -> 106 / 107, 43 / 45 -> 105 / 107, 46 / 51 -> 6 / 107, 0 / 21 / 52 / 200 -> 6 / 7");

  // S111 DESKSKIN: the shipped LoadArena's desk / top VERTICES against the file's own (AddProp into an empty model: file space, unmoved, the same mesh order). The game draws each
  // vertex with the row of its own palette slot's node (sub_82C04DF0 0x82C0A038..0x82C0A098); the rows, in file space and written out here (not read from DeskMeshAt): desk
  // bone01 (0, 0, -775.4), every other desk node (0, 0, -760), every top node (0, -48.25, -760). World = the shipped vertex (viewer space) + its placement's translation; expected
  // = the file vertex + its node's row, Y negated (the viewer's mirror). Every desk / top vertex in the player's files carries one pair: its slot = pair 0's.
  struct DeskCount {
    size_t meshes = 0, bone01 = 0, plate0 = 0;   // plate0: the desk's mesh 0, vertices on bone04
  };
  const auto split = [](const std::string& s) {
    std::vector<std::string> out;
    size_t b = 0;
    for (size_t e = s.find(';'); e != std::string::npos; b = e + 1, e = s.find(';', b)) out.push_back(s.substr(b, e - b));
    out.push_back(s.substr(b));
    return out;
  };
  const auto skin_ok = [&](int id, uint32_t desk_key, uint32_t top_key, DeskCount& c) {
    assets::Model m;
    std::string err;
    c = DeskCount{};
    if (game.empty() || !assets::LoadArena(files, id, m, err)) return false;
    bool ok = true;
    for (int part = 0; part < 2 && ok; ++part) {
      const bool top = part == 1;
      const uint32_t key = top ? top_key : desk_key;
      assets::Model ref;
      const std::vector<std::vector<assets::Matrix>> one = {{assets::Identity()}};
      const size_t nref = assets::load_detail::AddProp(files, key, {}, "reference", ref, false, &one);
      std::vector<const assets::Mesh*> got;
      for (const assets::Mesh& me : m.meshes)
        if (me.part.rfind("announce desk", 0) == 0 && (me.part.rfind("announce desk top", 0) == 0) == top) got.push_back(&me);
      ok = nref > 0 && got.size() == nref;
      for (size_t k = 0; ok && k < nref; ++k) {
        const assets::Mesh& g = *got[k];
        const assets::Mesh& r = ref.meshes[k];
        ok = uint32_t(g.src_key) == key && g.vertices.size() == r.vertices.size() && g.placement >= 0 && size_t(g.placement) < m.placements.size() &&
             m.placements[size_t(g.placement)].transforms.size() == 1;
        if (!ok) break;
        const auto& t = m.placements[size_t(g.placement)].transforms[0];
        const std::vector<std::string> names = split(r.palette);
        const bool pairs = r.src_skin_slots.size() == 4 * r.vertices.size();
        for (size_t i = 0; ok && i < r.vertices.size(); ++i) {
          const size_t slot = pairs && r.src_skin_slots[4 * i] != 255 ? r.src_skin_slots[4 * i] : 0;
          const std::string node = slot < names.size() ? names[slot] : std::string();
          const float ry = top ? -48.25f : 0.f, rz = top || node != "bone01" ? -760.f : -775.4f;
          const float* p = g.vertices[i].pos;
          const float* q = r.vertices[i].pos;
          ok = !node.empty() && Near(p[0] + t[12], q[0]) && Near(p[1] + t[13], -(q[1] + ry)) && Near(p[2] + t[14], q[2] + rz);
          if (!top) c.bone01 += node == "bone01" ? 1 : 0, c.plate0 += k == 0 && node == "bone04" ? 1 : 0;
        }
      }
      c.meshes += nref;
    }
    return ok;
  };
  bool deskload = !game.empty();
  DeskCount dc;
  for (const auto& w : {std::array<int, 3>{4, 6, 7}, {28, 106, 107}, {43, 105, 107}, {46, 6, 107}})
    deskload = deskload && skin_ok(w[0], uint32_t(w[1]), uint32_t(w[2]), dc) && dc.bone01 > 0;
  deskload = deskload && skin_ok(4, 6, 7, dc) && dc.meshes == 6 && dc.plate0 == 113;   // bg04 desk 6 mesh 0: its 113 plate vertices (bone04) at the place, not slot 0's -775.4
  Check(deskload, "ap.deskload", "the shipped LoadArena: bg04 6 / 7, bg28 106 / 107, bg43 105 / 107, bg46 6 / 107, each vertex at its own slot's node row; bg04 plate at -760");

  std::printf("%s\n", g_fail ? "arena_props_test: FAILED" : "arena_props_test: all checks passed");
  return g_fail;
}
