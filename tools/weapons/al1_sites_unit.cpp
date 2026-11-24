// al1_sites_unit: the offline proof of S108 lane AL1's wired sites (docs\research\s108\AL1_plan.md rows 1-5). 0 game runs, no SDK, no game memory, no game files.
//   al1_sites_unit [--plant silent]
// Each site runs the SHIPPED code in the production order and must print exactly one named `[rdr][PROBLEM] draw_shortfall:` line for a planted shortfall and none for clean data (lines captured
// through rdr::SetLogSink, printed as `LINE <text>`). `--plant silent` = every report call skipped (the old code: PoseMesh / the loader / the front end said nothing): every site check must fail.
// Checks (exit 1 when any failed):
//   site.pose_rows_weapon  MakeWeapon's hook -> the model call -> TakeNext -> Plan -> BindModel -> RowsNeeded -> ReportWeaponRows -> PoseCall: 659 rows none; the tracker's RingCap96 twin (96 rows)
//                          one line naming `event prop 2591 entry 4`, asked 658, given 96 (and PoseCall leaves the row-657 vertex at its file position: the fault is real)
//   site.pose_rows_ring    BindMesh -> RowsNeeded -> ReportRingRows -> PoseMesh on a 70-node ring piece: 70 rows none; 40 rows of a fallen-back call one line `ring key 973 (arena 5)`, `asked key 980`
//   site.row_bound         a 1025-node event model through OnModelCall -> ReportRefused: one `row_bound` line; a 659-node call none
//   site.model_part        NoteDropped (the five writers' line) -> ReportLoadedModel: a dropped slot one `model_part` line; a texture not found = one plain note, no PROBLEM; a clean model nothing
//   site.texture_pac       the real LoadCommonTexDirs on a folder without pac\ch\chCommon.pac -> CommonTexError -> NoteDropped(texture_pac) on two wrestler models -> ReportLoadedModel: ONE line (the pac)
//   site.texture_upload    TextureRefusal -> ReportUploadRefusal: a 6 x 6 BC1 and a BC1 with half its mip 0 one line each; a whole 8 x 8 BC1 none (and no refusal)
//   site.cap               70 dropped parts more: the run's PROBLEM lines stop at 64 named + 1 limit line
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>   // (asset_load.h's pac / stage_media code needs it; the build passes -I<sdk stb>)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/problem.h"
#include "../../src/renderer_scene/model_load_report.h"
#include "../../src/renderer_scene/pose_rows_report.h"
#include "../../src/renderer_scene/upload_check.h"
#include "../../src/renderer_scene/weapon_draw_core.h"
#include "../../src/scene_live/weapon_rows_core.h"
#include "../../src/scene_live/weapon_rows_report.h"

namespace fs = std::filesystem;
using namespace rdr_scene;
using namespace rdr_scene::live::weaponrows;
namespace W = rdr_scene::weapons;

static std::mutex g_mu;
static std::vector<std::string> g_lines;
static void Sink(const char* line) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_lines.emplace_back(line);
  std::printf("LINE %s\n", line);
}
static size_t Mark() { return g_lines.size(); }
static std::vector<std::string> Since(size_t a, const char* needle) {
  std::vector<std::string> out;
  for (size_t i = a; i < g_lines.size(); ++i)
    if (g_lines[i].find(needle) != std::string::npos) out.push_back(g_lines[i]);
  return out;
}
static int g_fail = 0;
static bool g_silent = false;
static void Check(bool ok, const char* name, const std::string& why) {
  std::printf("%s  %-22s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  std::fflush(stdout);
  if (!ok) ++g_fail;
}

// ---- the guest of the weapon tracker (fan_tassel_unit.cpp's rule: words as host values) ----
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
  void MakeModel(uint32_t obj, uint32_t nodes) {   // a CSkinModel whose NodeCount answers `nodes` (vtable 0x8206AF90, slot 2 = sub_82BD3AB8, [this+296] != 0, [this+300])
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, 1.0f);
  }
  void Palette(uint32_t buf, uint32_t n) {   // node i = a translation by (1000 + i, 0, 0)
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < 16; ++j) PutFloat(buf + 64 * i + 4 * j, j % 5 == 0 ? 1.f : j == 12 ? 1000.f + float(i) : 0.f);
  }
};
struct Rig {
  Guest g;
  Tracker t;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame) {
    return t.OnModelCall(model, r5, frame, true, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); });
  }
  bool Take(FrameBuffer& fb) {
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

// A stored arena-layout event mesh (the loader's form: skin pairs + the slot -> node map): 4 vertices on nodes 0 / 95 / 96 / 657.
static assets::Model TasselModel() {
  assets::Model m;
  assets::Mesh me;
  me.palette_nodes = {0, 95, 96, 657};
  me.src_skin_pairs = 1;
  for (uint8_t s = 0; s < 4; ++s) {
    assets::Vertex v{};
    v.pos[0] = 1.f, v.pos[1] = 2.f, v.pos[2] = 3.f;
    me.vertices.push_back(v);
    for (int k = 0; k < 4; ++k) me.src_skin_slots.push_back(k == 0 ? s : uint8_t(255)), me.src_skin_weights.push_back(k == 0 ? 1.f : 0.f);
  }
  m.meshes.push_back(me);
  return m;
}

// rf_props.cpp ApplyWeaponRows' order for one frame: plan -> bind (+ RowsNeeded, once) -> the report -> the pose. Returns the lines and whether the row-657 vertex kept its file position.
static size_t WeaponFrame(Plant plant, bool& unposed657) {
  const uint32_t kModel = 0xA0001000, kBuf = 0xB0000000;
  Rig r;
  r.t.plant = plant;
  r.g.MakeModel(kModel, 659);
  r.g.Palette(kBuf, 659);
  r.t.EventMake(kModel, 2591, 4);
  r.Call(kModel, kBuf, 300);
  r.Call(0xA0009000, kBuf, 301);   // the next frame closes frame 300
  FrameBuffer fb;
  const size_t a = Mark();
  unposed657 = false;
  if (!r.Take(fb)) return 0;
  const W::View view = ViewOf(fb);
  W::Census cen;
  const std::vector<W::Item> items = W::Plan(view, W::Plants(), cen);
  if (items.size() != 1) return 0;
  const W::Item& it = items[0];
  const W::WeaponCall& w = view.calls[it.call];
  assets::Model bound;
  W::BindModel(TasselModel(), bound);
  const uint32_t need = rf::c::RowsNeeded(bound);
  const std::string pool = "weapon:ev" + std::to_string(it.id) + "/" + std::to_string(it.entry);
  if (!g_silent) W::ReportWeaponRows(it, w, -1, pool, need);
  std::vector<assets::Matrix> pal;
  assets::Model out = bound;
  W::PoseCall(bound, view, w, W::Plants(), pal, out);
  const assets::Vertex& s = bound.meshes[0].vertices[3];
  const assets::Vertex& d = out.meshes[0].vertices[3];
  unposed657 = s.pos[0] == d.pos[0] && s.pos[1] == d.pos[1] && s.pos[2] == d.pos[2];
  return Since(a, "draw_shortfall:").size();
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "silent") == 0) g_silent = true;
  rdr::SetLogSink(Sink);
  // ---- site.pose_rows_weapon ----
  {
    bool unposed_clean = true, unposed_cut = false;
    const size_t clean = WeaponFrame(Plant::None, unposed_clean);
    const size_t a = Mark();
    const size_t cut = WeaponFrame(Plant::RingCap96, unposed_cut);
    const std::vector<std::string> l = Since(a, "draw_shortfall: pose_rows: event prop 2591 entry 4: asked 658, given 96");
    Check(clean == 0 && !unposed_clean && cut == 1 && l.size() == 1 && unposed_cut, "site.pose_rows_weapon",
          "659 rows: " + std::to_string(clean) + " line(s); 96 rows: " + std::to_string(cut) + " line(s), named " + std::to_string(l.size()) + ", row-657 vertex unposed " + (unposed_cut ? "yes" : "no"));
  }
  // ---- site.pose_rows_ring ----
  {
    assets::Model piece;
    assets::Mesh me;
    me.palette_nodes = {0, 30, 69};
    me.src_skin_pairs = 1;
    for (uint8_t s = 0; s < 3; ++s) {
      assets::Vertex v{};
      v.pos[0] = 4.f;
      me.vertices.push_back(v);
      for (int k = 0; k < 4; ++k) me.src_skin_slots.push_back(k == 0 ? s : uint8_t(255)), me.src_skin_weights.push_back(k == 0 ? 1.f : 0.f);
    }
    const bool bound = rf::c::BindMesh(me);   // rf_c_core.cpp's bind, then RowsNeeded (RingPiece::rows_needed)
    piece.meshes.push_back(me);
    const uint32_t need = rf::c::RowsNeeded(piece);
    std::vector<assets::Matrix> pal(70, assets::Identity());
    for (size_t k = 0; k < pal.size(); ++k) pal[k][12] = 100.f + float(k);
    const size_t a = Mark();
    if (!g_silent) rf::c::ReportRingRows(973, 973, 5, need, 70);
    const size_t clean = Since(a, "draw_shortfall:").size();
    const size_t b = Mark();
    if (!g_silent) rf::c::ReportRingRows(973, 980, 5, need, 40);
    assets::Mesh out = piece.meshes[0];
    rf::c::PoseMesh(piece.meshes[0], pal.data(), 40, out);
    const bool left = out.vertices[2].pos[0] == piece.meshes[0].vertices[2].pos[0];
    const std::vector<std::string> l = Since(b, "draw_shortfall: pose_rows: ring key 973 (arena 5): asked 70, given 40: asked key 980");
    Check(bound && need == 70 && clean == 0 && Since(b, "draw_shortfall:").size() == 1 && l.size() == 1 && left, "site.pose_rows_ring",
          "need " + std::to_string(need) + "; 70 rows: " + std::to_string(clean) + " line(s); 40 rows: named " + std::to_string(l.size()) + ", node-69 vertex left " + (left ? "yes" : "no"));
  }
  // ---- site.row_bound ----
  {
    Rig r;
    r.g.MakeModel(0xA0002000, kWeaponMaxNodes + 1);
    r.g.Palette(0xB1000000, kWeaponMaxNodes + 1);
    r.g.MakeModel(0xA0003000, 659);
    r.g.Palette(0xB2000000, 659);
    r.t.EventMake(0xA0002000, 70, 0);
    r.t.EventMake(0xA0003000, 2591, 4);
    const size_t a = Mark();
    const CallResult ok = r.Call(0xA0003000, 0xB2000000, 300);
    if (!g_silent) ReportRefused(ok, 0xA0003000);
    const size_t clean = Since(a, "draw_shortfall:").size();
    const size_t b = Mark();
    const CallResult over = r.Call(0xA0002000, 0xB1000000, 300);
    if (!g_silent) ReportRefused(over, 0xA0002000);
    const std::vector<std::string> l = Since(b, "draw_shortfall: row_bound: event prop 70 entry 0: asked 1025, given 0: model 0xA0002000");
    Check(clean == 0 && over.refused == kRefusedOverBound && l.size() == 1 && Since(b, "draw_shortfall:").size() == 1, "site.row_bound",
          "659: " + std::to_string(clean) + " line(s); 1025: refused " + std::to_string(over.refused) + ", named " + std::to_string(l.size()));
  }
  // ---- site.model_part ----
  {
    assets::Model clean_m;
    clean_m.warnings.push_back("mesh has normals that are not unit length");
    assets::Model m;
    assets::NoteDropped(m, "slot 973", "slot 973: node table out of range");
    m.warnings.push_back("texture 'wrinkles_n' not found in any directory");
    const size_t a = Mark();
    if (!g_silent) ReportLoadedModel("arena 5", clean_m);
    const size_t clean = g_lines.size() - a;
    const size_t b = Mark();
    if (!g_silent) ReportLoadedModel("arena 5", m);
    const std::vector<std::string> l = Since(b, "draw_shortfall: model_part: arena 5: slot 973: asked 1, given 0: slot 973: node table out of range");
    const std::vector<std::string> note = Since(b, "[wwe2k14][model_load] arena 5: texture 'wrinkles_n' not found");
    const bool warn_kept = !m.warnings.empty() && m.warnings[0] == "slot 973 failed: slot 973: node table out of range";
    Check(clean == 0 && l.size() == 1 && note.size() == 1 && Since(b, "[rdr][PROBLEM]").size() == 1 && warn_kept, "site.model_part",
          "clean: " + std::to_string(clean) + " line(s); dropped slot: named " + std::to_string(l.size()) + ", texture note " + std::to_string(note.size()) + ", PROBLEM lines " +
              std::to_string(Since(b, "[rdr][PROBLEM]").size()) + ", warning text kept " + (warn_kept ? "yes" : "no"));
  }
  // ---- site.texture_pac: the real LoadCommonTexDirs on a game folder that has no pac\ch\chCommon.pac ----
  {
    const fs::path root = fs::temp_directory_path() / "al1_sites_unit_root";
    fs::create_directories(root / "pac" / "ch");
    launcher::GameFiles files;
    files.SetRoot(root);
    assets::load_detail::LoadCommonTexDirs(files);
    const std::string why = assets::load_detail::CommonTexError();
    assets::Model w1, w2;
    for (assets::Model* w : {&w1, &w2})   // LoadWrestlerPac's line (asset_load.h): every wrestler loaded after the failed open
      if (!why.empty()) assets::NoteDropped(*w, "pac\\ch\\chCommon.pac (the shared wrestler textures)", why, "texture_pac");
    const size_t a = Mark();
    if (!g_silent) ReportLoadedModel("pac\\ch\\ch394.pac model_key:39400 variant 0", w1), ReportLoadedModel("pac\\ch\\ch385.pac model_key:38500 variant 0", w2);
    const std::vector<std::string> l = Since(a, "draw_shortfall: texture_pac: pac\\ch\\chCommon.pac (the shared wrestler textures): asked 1, given 0: first seen loading pac\\ch\\ch394.pac");
    Check(!why.empty() && l.size() == 1 && Since(a, "draw_shortfall:").size() == 1, "site.texture_pac", "open error '" + why + "'; lines for two wrestlers " + std::to_string(Since(a, "draw_shortfall:").size()) + " (want 1)");
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  // ---- site.texture_upload ----
  {
    assets::Texture good, odd, shortt;
    good.name = "good", good.format = assets::TexFormat::kBC1, good.width = good.height = 8, good.mips = 1, good.data.assign(32, 0);
    odd = good, odd.name = "odd", odd.width = odd.height = 6;
    shortt = good, shortt.name = "short", shortt.data.assign(16, 0);
    upload::TexturePlan p;
    const char* r_good = upload::TextureRefusal(good, p);
    const char* r_odd = upload::TextureRefusal(odd, p);
    const char* r_short = upload::TextureRefusal(shortt, p);
    const size_t a = Mark();
    for (const auto& pr : {std::make_pair(&good, r_good), std::make_pair(&odd, r_odd), std::make_pair(&shortt, r_short)})   // AddModelImpl's order: a refusal with a reason is reported
      if (pr.second && !g_silent) upload::ReportUploadRefusal("texture_upload", "arena 5 ring piece 973", "texture '" + pr.first->name + "'", pr.second);
    const std::vector<std::string> l1 = Since(a, "draw_shortfall: texture_upload: arena 5 ring piece 973: texture 'odd': asked 1, given 0: block-compressed size not a multiple of 4");
    const std::vector<std::string> l2 = Since(a, "draw_shortfall: texture_upload: arena 5 ring piece 973: texture 'short': asked 1, given 0: data shorter than mip 0");
    Check(!r_good && r_odd && r_short && l1.size() == 1 && l2.size() == 1 && Since(a, "draw_shortfall:").size() == 2, "site.texture_upload",
          std::string("good refused: ") + (r_good ? r_good : "no") + "; odd named " + std::to_string(l1.size()) + ", short named " + std::to_string(l2.size()));
  }
  // ---- site.cap ----
  {
    for (int i = 0; i < 70; ++i) {
      assets::Model m;
      assets::NoteDropped(m, "slot " + std::to_string(i), "flood");
      if (!g_silent) ReportLoadedModel("arena 99", m);
    }
    const size_t problem = Since(0, "[rdr][PROBLEM]").size(), limit = Since(0, "draw_shortfall: limit:").size();
    Check(problem == 65 && limit == 1, "site.cap", "PROBLEM lines in the run " + std::to_string(problem) + ", limit lines " + std::to_string(limit) + " (want 65 = 64 named + 1 limit)");
  }
  std::printf("%s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
