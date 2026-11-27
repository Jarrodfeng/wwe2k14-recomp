// S111 MODBELT unit (docs\research\s111\MODBELT.md): an entrance prop id that a pack's evtobj_dlcPP.pac carries AND the disc's evtobj.pac carries resolves to the
// PACK's PACH (the game's pac list: a pack's nodes at the head, the disc's at the tail, the highest pack id first; asset_load.h PropPachInPacks), so the stored model is
// the one MakeWeapon built and the draw call's node rows cover every vertex. 0 game runs, no game files: the fixture is a temp tree of synthetic pacs (our own bytes)
// under out\modbelt_unit_fixture. The pack's model has the modded belt's features (body word +0 = 0x10 with the mesh table at 0x48, a second root node first, an S3
// tangent stream); the disc's is a stock-like belt with MORE nodes than the pack's.
//   modbelt_unit.exe                       the fixture checks + the in-process twin (the old disc-first order leaves vertices past the call's rows)
//   modbelt_unit.exe --plant disc_first    the old order for the whole run: must exit 1 and FAIL mod_pack_shadows_disc
// Build and run: pwsh -NoProfile -File tools\assets\build_modbelt_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/assets/entrance.h"
#include "../../src/pac_lookup_route.h"   // S111 READANSWER: the hook's own route of a prop hit (read or probe)
#include "../../src/renderer_scene/ring_pose.h"

namespace fs = std::filesystem;
using assets::Bytes;

namespace {

int g_fail = 0;
void Check(const char* name, bool ok, const std::string& what) {
  std::printf("%s  %s: %s\n", ok ? "PASS" : "FAIL", name, what.c_str());
  if (!ok) ++g_fail;
}

void PutBe32(Bytes& b, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + size_t(i)] = uint8_t(v >> (24 - 8 * i));
}
void PutBeF(Bytes& b, size_t o, float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  PutBe32(b, o, u);
}
void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }

// One synthetic JBOY: one yProp_BumpMap mesh of N vertices, one skin pair a vertex (slot v % palette size, weight 1), palette = 1-based node numbers.
struct Spec {
  std::vector<const char*> nodes;
  std::vector<int> parents;
  uint32_t word0 = 0, mesh_tab = 0x40;
  std::vector<uint32_t> pal;
  bool s3 = false;
  uint32_t N = 4;
};
Bytes MakeJboy(const Spec& s) {
  const uint32_t nn = uint32_t(s.nodes.size()), N = s.N;
  const uint32_t node_tab = s.mesh_tab + 0xB8, tex_tab = node_tab + 80 * nn, s0 = tex_tab, s0d = s0 + 4, s1 = s0d + 28 * N, s2 = s1 + 8 * N, s3 = s2 + 8 * N,
                 ib = s3 + (s.s3 ? 12 * N : 0), idx = ib + 12, body_len = idx + 2 * N + 2;
  Bytes b(8 + size_t(body_len), 0);
  std::memcpy(b.data(), "JBOY", 4);
  PutBe32(b, 4, body_len);
  const size_t B = 8;
  PutBe32(b, B + 0x00, s.word0);
  PutBe32(b, B + 0x10, 1), PutBe32(b, B + 0x14, s.mesh_tab), PutBe32(b, B + 0x18, nn), PutBe32(b, B + 0x1C, 0), PutBe32(b, B + 0x20, node_tab), PutBe32(b, B + 0x24, tex_tab);
  for (uint32_t i = 0; i < nn; ++i) {
    const size_t o = B + node_tab + 80 * size_t(i);
    std::memcpy(&b[o], s.nodes[i], std::strlen(s.nodes[i]));
    PutBeF(b, o + 20, 5.f * float(i));
    PutBe32(b, o + 48, uint32_t(s.parents[i]));
  }
  const size_t rec = B + s.mesh_tab;
  PutBe32(b, rec, N), PutBe32(b, rec + 8, uint32_t(s.pal.size()));
  for (size_t k = 0; k < s.pal.size(); ++k) PutBe32(b, rec + 0x0C + 4 * k, s.pal[k]);
  PutBe32(b, rec + 0x5C, 1);
  PutBe32(b, rec + 0x68, s0), PutBe32(b, rec + 0x6C, s1), PutBe32(b, rec + 0x70, s2), PutBe32(b, rec + 0x74, s3), PutBe32(b, rec + 0x9C, ib);
  std::memcpy(&b[rec + 0x7C], "yProp_BumpMap", 13);
  for (uint32_t v = 0; v < N; ++v) {
    const size_t p0 = B + s0d + 28 * size_t(v);
    PutBeF(b, p0, float(v)), PutBeF(b, p0 + 4, 2.f * float(v)), PutBeF(b, p0 + 8, 3.f * float(v));
    PutBeF(b, p0 + 12, 0.f), PutBeF(b, p0 + 16, 1.f), PutBeF(b, p0 + 20, 0.f);
    PutBe32(b, p0 + 24, 0xFFFFFFFFu);
    const uint32_t slot = v % uint32_t(s.pal.size());
    std::memcpy(&b[B + s1 + 8 * size_t(v)], &slot, 4);   // LE slot
    PutBeF(b, B + s1 + 8 * size_t(v) + 4, 1.f);
    PutBeF(b, B + s2 + 8 * size_t(v), 0.25f * float(v)), PutBeF(b, B + s2 + 8 * size_t(v) + 4, 0.5f);
    if (s.s3) PutBeF(b, B + s3 + 12 * size_t(v) + 8, 1.f);
  }
  b[B + ib] = 6;   // LE 6: triangle strip
  PutBe32(b, B + ib + 4, N), PutBe32(b, B + ib + 8, idx);
  for (uint32_t v = 0; v < N; ++v) b[B + idx + 2 * size_t(v)] = uint8_t(v >> 8), b[B + idx + 2 * size_t(v) + 1] = uint8_t(v);
  return b;
}

// A plain EPAC (the pack pac layout: not XCTD): the directory at 0x800 (the first section's name, then 12-byte rows from 0x80C: a section row opens a section),
// one PACH per row at the next 2 KB sector after 0x4000, each holding one entry (id 0 = the model).
struct Row {
  const char* section;
  const char* id;
  const Bytes* model;
};
void WriteEpac(const fs::path& p, const std::vector<Row>& rows) {
  Bytes f(0x4000, 0);
  std::memcpy(f.data(), "EPAC", 4);
  PutLe32(f, 12, 7);
  std::string cur = rows.empty() ? std::string("SOBJ") : std::string(rows[0].section);
  std::memcpy(f.data() + 0x800, cur.c_str(), 4);
  size_t o = 0x80C;
  for (const Row& r : rows) {
    if (cur != r.section) {
      cur = r.section;
      std::memcpy(f.data() + o, cur.c_str(), 4);
      PutLe32(f, o + 4, 9);
      o += 12;
    }
    const size_t at = f.size();
    Bytes pach(20 + r.model->size(), 0);
    std::memcpy(pach.data(), "PACH", 4);
    PutLe32(pach, 4, 1), PutLe32(pach, 8, 0), PutLe32(pach, 12, 0), PutLe32(pach, 16, uint32_t(r.model->size()));
    std::memcpy(pach.data() + 20, r.model->data(), r.model->size());
    pach.resize((pach.size() + 0x7FF) & ~size_t(0x7FF), 0);
    f.insert(f.end(), pach.begin(), pach.end());
    std::memcpy(f.data() + o, r.id, 4);
    PutLe32(f, o + 4, uint32_t((at - 0x4000) / 0x800)), PutLe32(f, o + 8, uint32_t(pach.size() / 256));
    o += 12;
  }
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

// S111 MODBELT review: an SPMT PACH's entry 0 = a texture directory (dds.h TexSet) holding ONE texture `Mask1`: a w x w BGRA4 DDS (the width tells the pack's mask from the disc's).
Bytes MakeMaskDir(uint32_t w) {
  const size_t dds = 128 + size_t(w) * w * 2;
  Bytes b(0x30 + dds, 0);
  PutLe32(b, 0, 1);
  std::memcpy(&b[0x10], "Mask1", 5);
  std::memcpy(&b[0x10 + 16], "dds", 3);
  PutLe32(b, 0x10 + 20, uint32_t(dds)), PutLe32(b, 0x10 + 24, 0x30);
  const size_t d = 0x30;
  std::memcpy(&b[d], "DDS ", 4);
  PutLe32(b, d + 4, 124), PutLe32(b, d + 12, w), PutLe32(b, d + 16, w);
  PutLe32(b, d + 88, 16), PutLe32(b, d + 92, 0x0F00), PutLe32(b, d + 96, 0x00F0), PutLe32(b, d + 100, 0x000F), PutLe32(b, d + 104, 0xF000);
  return b;
}

// S111 MODBELT review: an Entrance pac as entrance.h EvTable reads it: 16-byte rows from 0x80C (8-character bank name, LE32 sector), each bank's EV table ("0FOP", its name at +0x22) at 0x4000 + sector x 0x800.
void WriteEntrance(const fs::path& p, const std::vector<std::pair<int, std::string>>& banks) {
  Bytes f(0x4000 + 0x800 * banks.size(), 0);
  std::memcpy(f.data(), "EPK8", 4);
  for (size_t i = 0; i < banks.size(); ++i) {
    char nm[16];
    std::snprintf(nm, sizeof nm, "%08d", banks[i].first);
    std::memcpy(&f[0x80C + 16 * i], nm, 8);
    PutLe32(f, 0x80C + 16 * i + 8, uint32_t(i)), PutLe32(f, 0x80C + 16 * i + 12, 8);
    const size_t t = 0x4000 + 0x800 * i;
    std::memcpy(&f[t], "0FOP", 4);
    std::memcpy(&f[t + 0x22], banks[i].second.c_str(), std::min<size_t>(banks[i].second.size(), 30));
  }
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

// The rows a stored model's vertices ask for, bound the way the drawer binds it (weapon_draw_core.h BindModel -> ring_pose.h BindMesh / RowsNeeded).
uint32_t RowsAsked(const assets::Model& m) {
  assets::Model bound = m;
  for (assets::Mesh& me : bound.meshes) rdr_scene::rf::c::BindMesh(me);
  return rdr_scene::rf::c::RowsNeeded(bound);
}

std::string Num(long long v) { return std::to_string(v); }

int Run(bool plant) {
  assets::load_detail::PropDiscFirstPlant() = plant;
  const fs::path base = fs::absolute("out/modbelt_unit_fixture");
  std::error_code ec;
  fs::remove_all(base, ec);

  Spec stock;   // a stock-like belt: 6 nodes, one root, vertices on every node
  stock.nodes = {"root", "bone01", "bone02", "bone03", "bone04", "bone05"};
  stock.parents = {-1, 0, 1, 2, 3, 4};
  stock.pal = {1, 2, 3, 4, 5, 6};
  stock.N = 6;
  Spec mod;     // the modded belt's features: body word +0 = 0x10, mesh table at 0x48, a second root node first, an S3 tangent stream, FEWER nodes
  mod.nodes = {"vector", "root", "bone01", "bone02"};
  mod.parents = {-1, -1, 1, 2};
  mod.word0 = 0x10, mod.mesh_tab = 0x48, mod.s3 = true;
  mod.pal = {2, 3, 4};
  mod.N = 4;
  const uint32_t game_rows = uint32_t(mod.nodes.size());   // the rows the game's call carries for the pack's model: its node count (sub_82BDB460 +B8: model+300 = header +24)
  const Bytes A = MakeJboy(stock), Bm = MakeJboy(mod);
  const Bytes mask_disc = MakeMaskDir(4), mask_pack = MakeMaskDir(8);
  // S111 MODBELT review: + 2700 (disc BOBJ, pack 5 SOBJ: the game's read asks /BOBJ/ over the WHOLE list before any /SOBJ/), 2800 (pack 4 in both sections: BOBJ is read), the SPMT masks 2718 (disc and pack 5) / 2719 (disc only)
  WriteEpac(base / "game" / "pac" / "evt" / "evtobj.pac", {{"BOBJ", "2004", &A}, {"BOBJ", "3004", &A}, {"BOBJ", "2009", &A}, {"BOBJ", "2700", &A}, {"SPMT", "2718", &mask_disc}, {"SPMT", "2719", &mask_disc}});
  WriteEpac(base / "dlc" / "pack4" / "pac" / "evt" / "evtobj_dlc04.pac", {{"SOBJ", "2800", &A}, {"BOBJ", "5004", &A}, {"BOBJ", "2800", &Bm}});
  WriteEpac(base / "dlc" / "pack5" / "pac" / "evt" / "evtobj_dlc05.pac", {{"SOBJ", "2600", &Bm}, {"SOBJ", "2700", &Bm}, {"BOBJ", "2004", &Bm}, {"BOBJ", "3004", &Bm}, {"BOBJ", "5004", &Bm}, {"SPMT", "2718", &mask_pack}});
  WriteEntrance(base / "game" / "pac" / "evt" / "Entrance00.pac", {{416, "DISC_416"}, {417, "DISC_417"}});
  WriteEntrance(base / "dlc" / "pack4" / "pac" / "evt" / "Entrance_dlc04.pac", {{416, "PACK4_416"}});
  WriteEntrance(base / "dlc" / "pack5" / "pac" / "evt" / "Entrance_dlc05.pac", {{416, "PACK5_416"}});

  launcher::GameFiles files;
  files.SetRoot(base / "game");
  files.SetOverlays({{base / "dlc" / "pack4", "_dlc04", "dlc", nullptr}, {base / "dlc" / "pack5", "_dlc05", "dlc", nullptr}});   // ASCENDING (the viewer's order): the rule must not depend on it

  const int k = assets::dlc_files::kPropPackStride;
  const int p2004 = assets::PropPachEvt(files, 2004);
  Check("mod_pack_shadows_disc", p2004 == 5 * k + 2, "prop 2004 (disc BOBJ row AND pack 5's) -> PACH " + Num(p2004) + ", want pack 5's PACH 2 = " + Num(5 * k + 2));
  {
    assets::Model m;
    std::string err;
    const bool ok = p2004 >= 0 && assets::LoadPropEntry(files, p2004, 0, m, err);
    const uint32_t need = ok ? RowsAsked(m) : 0;
    Check("rows_cover_every_vertex", ok && need >= 1 && need <= game_rows, "the stored model asks rows " + Num(need) + " of the call's " + Num(game_rows) + (ok ? "" : " (load failed: " + err + ")"));
    bool shape = ok && m.meshes.size() == 1 && m.meshes[0].vertices.size() == 4 && m.meshes[0].palette_nodes == std::vector<uint16_t>{1, 2, 3} && m.meshes[0].rigid_node == 1;
    if (shape) {
      const assets::Vertex& v2 = m.meshes[0].vertices[2];
      shape = v2.pos[0] == 2.f && v2.pos[1] == -4.f && v2.pos[2] == 6.f && v2.tangent[2] == 1.f;   // (FinishModel mirrors y)
    }
    Check("mod_features_read", shape, "header word 0x10 + mesh table 0x48, a second root first (palette 2,3,4 -> nodes 1,2,3), the S3 tangent: read as the game's build reads them");
  }
  const int p5004 = assets::PropPachEvt(files, 5004);
  Check("highest_pack_wins", p5004 == 5 * k + 4, "prop 5004 (packs 4 and 5) -> PACH " + Num(p5004) + ", want pack 5's PACH 4 = " + Num(5 * k + 4));
  const int p2009 = assets::PropPachEvt(files, 2009);
  Check("disc_only_id_stays_disc", p2009 == 2, "prop 2009 (disc only) -> PACH " + Num(p2009) + ", want the disc's ordinal 2");
  const int p2600 = assets::PropPachEvt(files, 2600);
  Check("pack_only_id", p2600 == 5 * k, "prop 2600 (pack 5 SOBJ only) -> PACH " + Num(p2600) + ", want " + Num(5 * k));
  const int v3004 = assets::PropPach(files, 3004);
  Check("viewer_rule_same", v3004 == 5 * k + 3, "PropPach (the viewer's) 3004 -> PACH " + Num(v3004) + ", want pack 5's PACH 3 = " + Num(5 * k + 3));
  // S111 MODBELT review: the game's read (sub_825AE720) asks /BOBJ/<id> through the WHOLE pac list before /SOBJ/<id>: a disc BOBJ row beats a pack's SOBJ row of the same id
  const int p2700 = assets::PropPachEvt(files, 2700), v2700 = assets::PropPach(files, 2700);
  Check("bobj_anywhere_beats_sobj", p2700 == 3 && v2700 == 3, "prop 2700 (disc BOBJ PACH 3, pack 5 SOBJ) -> PACH " + Num(p2700) + " (viewer " + Num(v2700) + "), want the disc's BOBJ ordinal 3");
  int other = -2;
  const int p2800 = assets::PropPachEvt(files, 2800, &other);
  Check("both_sections_bobj_wins", p2800 == 4 * k + 2 && other == 4 * k, "prop 2800 (pack 4 SOBJ PACH 0 and BOBJ PACH 2) -> PACH " + Num(p2800) + ", other " + Num(other) + ", want BOBJ " + Num(4 * k + 2) + ", other the SOBJ " + Num(4 * k));
  // S111 MODBELT review: the recolour mask /SPMT/<id> (sub_825AE6D0) is found head-first too: pack 5's 2718 mask shadows the disc's; 2719 (disc only) stays the disc's
  for (const auto& [id, want, want_w] : {std::tuple<uint32_t, int, uint32_t>{2718, 5 * k + 5, 8}, std::tuple<uint32_t, int, uint32_t>{2719, 5, 4}}) {
    const int ord = assets::PropMaskPachEvt(files, id);
    assets::Texture t;
    std::string err;
    const bool ok = ord >= 0 && assets::LoadPropMask(files, ord, t, err);
    Check(id == 2718 ? "mask_pack_shadows_disc" : "mask_disc_only_id", ord == want && ok && t.width == want_w && t.format == assets::TexFormat::kB4G4R4A4,
          "SPMT " + Num(id) + " -> PACH " + Num(ord) + " (want " + Num(want) + "), Mask1 " + (ok ? Num(t.width) + " wide (want " + Num(want_w) + ")" : "not loaded: " + err));
  }
  // S111 MODBELT review: an entrance bank /EVP/<id> (sub_8231BEC8 -> pac_path_resolve) is found head-first: the highest pack's bank shadows the disc's
  const std::string n416 = assets::entrance::EvName(files, 416), n417 = assets::entrance::EvName(files, 417);
  Check("entrance_pack_shadows_disc", n416 == "PACK5_416" && n417 == "DISC_417", "bank 416 (disc, pack 4, pack 5) -> '" + n416 + "' (want PACK5_416), bank 417 (disc only) -> '" + n417 + "'");

  if (!plant) {   // S111 PACANSWER (pac_path_answer.h): the game's own pac-list answer wins over our derived order; no answer (or one we cannot use) = the derived order
    namespace ppa = assets::pac_path_answer;
    ppa::Clear();
    auto dir = [](const char* s) { return ppa::IsPropDir(reinterpret_cast<const unsigned char*>(s)); };
    const std::string disc = "D:\\PAC\\EVT\\EVTOBJ.PAC", pack5 = "dlc1:\\pac\\evt\\evtobj_dlc05.pac";
    const bool refused = !ppa::Note("/BOBJ/20045", disc) && !ppa::Note("/EMD/00039712", disc) && !ppa::Note("/BOBJ/5004", "DLC1:\\PAC\\EVT\\EVTOBJ_DL") && !ppa::Note("/BOBJ/5004", "DLC1:\\PAC\\EVT\\EVTOBJ_DLC05");
    Check("answer_parse", dir("BOBJ/2004") && dir("SOBJ/2700") && dir("SPMT/2718") && !dir("BOB") && !dir("SOBX/1") && !dir("SP") && !dir("EMD/00039712") && refused,
          "the hook's first bytes take BOBJ/ SOBJ/ SPMT/ only; a 5-digit id, a model path and a node path seen mid-copy are refused (nothing stored)");
    const int none = assets::PropPachEvt(files, 2004);
    ppa::Note("/BOBJ/2004", disc);   // the game's walk took the disc's 2004 (our derived order says pack 5's)
    const int g2004 = assets::PropPachEvt(files, 2004);
    Check("answer_wins_over_derived", none == 5 * k + 2 && g2004 == 0, "prop 2004: no answer -> PACH " + Num(none) + " (want the derived " + Num(5 * k + 2) + "), the game's /BOBJ/ answer = the disc -> PACH " + Num(g2004) + " (want 0)");
    ppa::Note("/SOBJ/2700", pack5);   // the rule with a SOBJ answer and no BOBJ answer; the derived order says the disc's BOBJ 3 (lower-case node name: any case). (In the game sub_8258C590 +7C probes /BOBJ/ first, so with the disc's BOBJ row the table would hold /BOBJ/2700 too: PACANSWER review)
    const int g2700 = assets::PropPachEvt(files, 2700);
    Check("sobj_answer_used", g2700 == 5 * k + 1, "prop 2700: the game's /SOBJ/ answer = pack 5 -> PACH " + Num(g2700) + " (want " + Num(5 * k + 1) + ")");
    ppa::Note("SPMT/2718", disc);   // (no leading '/') the game took the disc's mask; the derived order says pack 5's
    const int g2718 = assets::PropMaskPachEvt(files, 2718);
    Check("mask_answer_wins", g2718 == 4, "SPMT 2718: the game's answer = the disc -> PACH " + Num(g2718) + " (want the disc's ordinal 4; derived " + Num(5 * k + 5) + ")");
    ppa::Note("/BOBJ/3004", "DLC0:\\PAC\\EVT\\EVTOBJ_MOD.PAC");   // a pac name we do not map: said once, the derived order
    const int g3004 = assets::PropPachEvt(files, 3004);
    Check("unmapped_answer_derived", g3004 == 5 * k + 3, "prop 3004: an unmapped answer -> PACH " + Num(g3004) + " (want the derived " + Num(5 * k + 3) + ")");
    // PACANSWER review: the title-update pass (sub_82C52818) names its node "UPDATE:\" + the disc's def name: an UPDATE: evtobj ends in EVTOBJ.PAC but is not the disc's file we read
    ppa::Note("/BOBJ/3004", "UPDATE:\\PAC\\EVT\\EVTOBJ.PAC");
    const int u3004 = assets::PropPachEvt(files, 3004);
    Check("update_node_not_disc", u3004 == 5 * k + 3, "prop 3004: an UPDATE: node's answer -> PACH " + Num(u3004) + " (want the derived " + Num(5 * k + 3) + ", never the disc's BOBJ ordinal 1)");
    const int g5004 = assets::PropPachEvt(files, 5004);   // the torn notes above stored nothing
    Check("no_answer_derived", g5004 == 5 * k + 4, "prop 5004: no answer -> PACH " + Num(g5004) + " (want the derived " + Num(5 * k + 4) + ")");
    ppa::Clear();
    const int back = assets::PropPachEvt(files, 2004);
    Check("cleared_derived", back == 5 * k + 2, "prop 2004 after the table is cleared -> PACH " + Num(back) + " (want the derived " + Num(5 * k + 2) + ")");
    // S111 READANSWER (docs\research\s111\READANSWER.md): the answer of the game's READ wins over its probes'. The hits go through the hook's own route (pac_lookup_route.h
    // NotePropHit); a read is a hit inside the pac request's reader scope (model_file_note::ReaderScope, set around sub_82C37E48 in load_probe.h), a probe any hit outside it.
    {
      namespace route = wwe2k14::pac_lookup_route;
      using rdr_scene::live::model_file_note::ReaderScope;
      const std::string pack4 = "DLC0:\\PAC\\EVT\\EVTOBJ_DLC04.PAC";
      auto in_request = [](const char* path, const std::string& node) {   // a walk made inside a pac request (sub_82C37E48 -> sub_82C365A0 -> sub_82C51D20)
        const ReaderScope scope(0x40001000u);
        return route::NotePropHit(path, node);
      };
      // a recoloured prop (sub_8258C590 record +16 set): +7C probes /BOBJ/<id>, then sub_83391F18 READS /SOBJ/<id>. 2700: the disc's BOBJ row (PACH 3), pack 5's SOBJ row
      ppa::Clear();
      const bool probed = route::NotePropHit("/BOBJ/2700", disc), read_ok = in_request("/SOBJ/2700", pack5);
      const int r2700 = assets::PropPachEvt(files, 2700);
      Check("recoloured_read_sobj_wins", probed && read_ok && r2700 == 5 * k + 1,
            "prop 2700 recoloured: probe /BOBJ/ = the disc, read /SOBJ/ = pack 5 -> PACH " + Num(r2700) + " (want pack 5's SOBJ " + Num(5 * k + 1) + ", not the disc's BOBJ 3)");
      // the twin: the same hits with no request scope (the PACANSWER rule: every hit alike) take the probe's BOBJ
      ppa::Clear();
      route::NotePropHit("/BOBJ/2700", disc);
      route::NotePropHit("/SOBJ/2700", pack5);
      const int t2700 = assets::PropPachEvt(files, 2700);
      Check("twin_probe_only_bobj", t2700 == 3, "prop 2700, the same hits outside any request -> PACH " + Num(t2700) + " (want the disc's BOBJ 3: no read seen, the probes' order)");
      // the same pack in both sections: 2800 recoloured reads pack 4's SOBJ PACH 0, not its BOBJ PACH 2
      ppa::Clear();
      route::NotePropHit("/SOBJ/2800", pack4);   // sub_825AE5D8 (the SOBJ-then-BOBJ existence test)
      route::NotePropHit("/BOBJ/2800", pack4);   // sub_8258C590 +7C
      in_request("/SOBJ/2800", pack4);           // sub_83391F18 -> sub_82C37FF0 -> sub_82C37E48
      const int r2800 = assets::PropPachEvt(files, 2800);
      Check("recoloured_same_pack_sobj", r2800 == 4 * k, "prop 2800 recoloured (pack 4 SOBJ PACH 0 and BOBJ PACH 2) -> PACH " + Num(r2800) + " (want the SOBJ " + Num(4 * k) + ")");
      // a plain prop: the probes, then sub_825AE720 READS /BOBJ/<id> (it resolved); a later probe outside the scope changes nothing
      ppa::Clear();
      route::NotePropHit("/SOBJ/2800", pack4);
      route::NotePropHit("/BOBJ/2800", pack4);
      route::NotePropHit("/BOBJ/2800", pack4);   // sub_825AE720 +74
      in_request("/BOBJ/2800", pack4);           // sub_825AE720 +A8 -> sub_82C37FF0 -> sub_82C37E48
      route::NotePropHit("/SOBJ/2800", pack4);
      const int p2800b = assets::PropPachEvt(files, 2800);
      Check("plain_read_keeps_bobj", p2800b == 4 * k + 2 && rdr_scene::live::model_file_note::CurrentReader() == 0,
            "prop 2800 plain: read /BOBJ/ -> PACH " + Num(p2800b) + " (want the BOBJ " + Num(4 * k + 2) + "); the scope closed with the request");
      // READANSWER review: a read whose answer we cannot use (here an unmapped pac name) keeps the READ section: the game drew from the section it read, so our derived order for that
      // section (pack 5's SOBJ 2700), never the probe's BOBJ answer (the disc's 3) nor the BOBJ-first derived order
      ppa::Clear();
      route::NotePropHit("/BOBJ/2700", disc);                          // sub_8258C590 +7C
      in_request("/SOBJ/2700", "DLC0:\\PAC\\EVT\\EVTOBJ_MOD.PAC");     // sub_83391F18's read: a node whose pac name we do not map
      const int u2700 = assets::PropPachEvt(files, 2700);
      Check("unusable_read_keeps_section", u2700 == 5 * k + 1,
            "prop 2700 recoloured, the read's /SOBJ/ answer unmapped -> PACH " + Num(u2700) + " (want the derived SOBJ " + Num(5 * k + 1) + ", not the probe's BOBJ 3)");
      // S111 FOLLOWUPS3 (READANSWER review F2): one id, two participants, two reads. B plain (sub_825AE720 reads /BOBJ/: it resolved), then A recoloured (sub_83391F18 reads /SOBJ/ only,
      // the LAST read of the id). Each built model takes its own reader's section: the recoloured reader's read never becomes the plain readers' section (NotePropHit's reader class).
      auto in_request_as = [](const char* path, const std::string& node, bool recoloured) {
        const ReaderScope scope(0x40002000u);
        return route::NotePropHit(path, node, recoloured);
      };
      ppa::Clear();
      route::NotePropHit("/BOBJ/2700", disc);          // B: sub_8258C590 +7C
      in_request_as("/BOBJ/2700", disc, false);        // B: sub_825AE720's read
      route::NotePropHit("/BOBJ/2700", disc);          // A: sub_8258C590 +7C
      in_request_as("/SOBJ/2700", pack5, true);        // A: sub_83391F18's read
      const int plain2700 = assets::PropPachEvt(files, 2700), recol2700 = assets::PropPachEvt(files, 2700, nullptr, true);
      Check("two_readers_two_models", plain2700 == 3 && recol2700 == 5 * k + 1,
            "prop 2700: plain model -> PACH " + Num(plain2700) + " (want the disc's BOBJ 3), recoloured model -> PACH " + Num(recol2700) + " (want pack 5's SOBJ " + Num(5 * k + 1) + ")");
      // the twin: the recoloured read noted as any read (the READANSWER rule: one read section per id, the last) moves the plain participant's model to the SOBJ row
      ppa::Clear();
      route::NotePropHit("/BOBJ/2700", disc);
      in_request_as("/BOBJ/2700", disc, false);
      route::NotePropHit("/BOBJ/2700", disc);
      in_request_as("/SOBJ/2700", pack5, false);
      const int twin2700 = assets::PropPachEvt(files, 2700);
      Check("twin_last_read_per_id", twin2700 == 5 * k + 1, "the old rule: the plain model -> PACH " + Num(twin2700) + " (the SOBJ " + Num(5 * k + 1) + ": two_readers_two_models tells the rules apart)");
      // a recoloured model whose read is not seen (only the probe's BOBJ answer): still SOBJ, the recoloured reader's only path; the plain model keeps the probe's BOBJ
      ppa::Clear();
      route::NotePropHit("/BOBJ/2700", disc);
      const int norec = assets::PropPachEvt(files, 2700, nullptr, true), noplain = assets::PropPachEvt(files, 2700);
      Check("recoloured_without_read_sobj", norec == 5 * k + 1 && noplain == 3, "recoloured, no read seen -> PACH " + Num(norec) + " (want " + Num(5 * k + 1) + "); plain -> " + Num(noplain) + " (want 3)");
      ppa::Clear();
    }
  }
  if (!plant) {   // the in-process twin: the old disc-first order picks the disc's belt, whose vertices ask more rows than the pack model's call carries
    assets::load_detail::PropDiscFirstPlant() = true;
    const int t = assets::PropPachEvt(files, 2004);
    assets::Model m;
    std::string err;
    const bool ok = t >= 0 && assets::LoadPropEntry(files, t, 0, m, err);
    const uint32_t need = ok ? RowsAsked(m) : 0;
    assets::load_detail::PropDiscFirstPlant() = false;
    Check("twin_disc_first_caught", t == 0 && ok && need > game_rows,
          "PLANT (the old order): 2004 -> PACH " + Num(t) + " (the disc's), its vertices ask rows " + Num(need) + " > the call's " + Num(game_rows) + ": the spike");
  }
  fs::remove_all(base, ec);
  std::printf("%s (%d failed)\n", g_fail ? "modbelt_unit FAILED" : "modbelt_unit passed", g_fail);
  return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc >= 3 && !std::strcmp(argv[1], "--plant") && !std::strcmp(argv[2], "disc_first");
  if (argc >= 2 && !plant) {
    std::fprintf(stderr, "usage: modbelt_unit [--plant disc_first]\n");
    return 2;
  }
  return Run(plant);
}
