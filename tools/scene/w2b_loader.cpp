// S103 W2B: the loader's piece-node tags on the player's own files (docs\agents\s103\W2.md section 6, W2_ADV.md R1). Reads game files only, writes nothing.
//   w2b_loader.exe --game DIR WRESTLER:ATTIRE ...   -> per attire: main_bones, extras, per-piece extra counts, canonical-order check, forced names that also exist in the body
//   w2b_loader.exe --game DIR --expect 166:0=62 278:0=44 322:1=8 ...   -> exit 1 unless each attire's EXTRA count equals the number (the W2.md table: Khali 62, McIntyre 44, Del Rio 8)
// Build: tools\scene\build_w2b_loader.ps1 (needs the game folder, so it builds only from the main checkout).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

int main(int argc, char** argv) {
  std::string game;
  std::vector<std::pair<std::pair<int, int>, int>> todo;   // (wrestler, attire), expected extras (-1 = none given)
  bool expect = false, compare = false, old_nodes = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--expect") expect = true;
    else if (a == "--old-nodes") old_nodes = true, assets::load_detail::SpringNodes() = false;   // the switch off: the old by-name merge; --expect then compares the TOTAL bone count (the session branch's: ch109/1 155, Khali 171, McIntyre 153, Del Rio 117, from tools\assets\asset_dump)
    else if (a == "--compare") compare = true;   // every attire of every character: the game's key list against the old filter; exit 1 when any attire LOSES meshes
    else {
      int w = 0, at = 0, n = -1;
      if (std::sscanf(a.c_str(), "%d:%d=%d", &w, &at, &n) >= 2) todo.push_back({{w, at}, n});
    }
  }
  if (!game.empty() && compare) {
    launcher::GameFiles cf;
    cf.SetRoot(game);
    int lost = 0, grew = 0, same = 0, keys28 = 0, keys29 = 0;
    for (int w = 1; w < 500; ++w)
      for (int at = 0; at < 12; ++at) {
        assets::Model mo, mn;
        std::string e1, e2;
        assets::load_detail::OldEntryFilterPlant() = true;
        const bool o = assets::LoadWrestler(cf, w, at, mo, e1);
        assets::load_detail::OldEntryFilterPlant() = false;
        const bool n = assets::LoadWrestler(cf, w, at, mn, e2);
        if (!o && !n) continue;
        if (o != n || mn.meshes.size() < mo.meshes.size()) {
          std::printf("LOST wrestler %d attire %d: old %zu meshes (%d), new %zu (%d)\n", w, at, mo.meshes.size(), int(o), mn.meshes.size(), int(n));
          ++lost;
        } else if (mn.meshes.size() > mo.meshes.size()) {
          ++grew;
          int k28 = 0, k29 = 0;
          for (const assets::Mesh& me : mn.meshes) k28 += me.src_kind == 2 && me.src_key == 28, k29 += me.src_kind == 2 && me.src_key == 29;
          keys28 += k28, keys29 += k29;
          std::printf("grew wrestler %d attire %d: %zu -> %zu meshes (src_key 28: %d, 29: %d, extras %zu)\n", w, at, mo.meshes.size(), mn.meshes.size(), k28, k29, mn.bones.size() - mn.main_bones);
        } else {
          ++same;
        }
      }
    std::printf("compare: %d attires grew, %d unchanged, %d LOST (src_key 28 meshes %d, 29 meshes %d)\n", grew, same, lost, keys28, keys29);
    if (grew != 11 || keys28 != 4 || keys29 != 8) std::printf("compare: EXPECTED 11 attires / 4 + 8 meshes of entries 28 / 29 (docs\\agents\\s103\\ENT.md census on the shipped files)\n");
    return lost || grew != 11 || keys28 != 4 || keys29 != 8 ? 1 : 0;
  }
  if (game.empty() || todo.empty()) {
    std::fprintf(stderr, "usage: w2b_loader --game DIR [--expect] WRESTLER:ATTIRE[=EXTRAS]...\n");
    return 2;
  }
  launcher::GameFiles files;
  files.SetRoot(game);
  int rc = 0;
  for (const auto& t : todo) {
    assets::Model m;
    std::string err;
    if (!assets::LoadWrestler(files, t.first.first, t.first.second, m, err)) {
      std::printf("wrestler %d attire %d FAILED %s\n", t.first.first, t.first.second, err.c_str());
      rc = 1;
      continue;
    }
    const size_t extras = m.bones.size() - m.main_bones;
    std::map<int, int> per_piece;
    bool canon = true;
    std::set<std::string> body_names;
    for (size_t i = 0; i < m.main_bones; ++i) body_names.insert(m.bones[i].name);
    int shared_forced = 0;
    for (size_t i = m.main_bones; i < m.bones.size(); ++i) {
      ++per_piece[m.bones[i].piece_id];
      if (i > m.main_bones && !(m.bones[i].piece_id > m.bones[i - 1].piece_id || (m.bones[i].piece_id == m.bones[i - 1].piece_id && m.bones[i].piece_node > m.bones[i - 1].piece_node))) canon = false;
      if (body_names.count(m.bones[i].name)) ++shared_forced;
    }
    std::printf("wrestler %d attire %d: main %u, extras %zu (", t.first.first, t.first.second, m.main_bones, extras);
    for (const auto& kv : per_piece) std::printf(" piece %d: %d", kv.first, kv.second);
    std::printf(" ), canonical order %s, extras whose name the body also has %d\n", canon ? "yes" : "NO", shared_forced);
    int with_corr = 0;
    for (size_t i = m.main_bones; i < m.bones.size(); ++i) with_corr += m.bones[i].has_corr;
    std::printf("  extras with a non-identity corr (premultiplied by corr^-1 at draw, review F): %d\n", with_corr);
    int k28 = 0, k29 = 0;
    for (const assets::Mesh& me : m.meshes) k28 += me.src_kind == 2 && me.src_key == 28, k29 += me.src_kind == 2 && me.src_key == 29;
    std::printf("  meshes %zu (src_key 28: %d, 29: %d)\n", m.meshes.size(), k28, k29);
    if (!canon && !old_nodes) rc = 1;
    if (expect && t.second >= 0 && int(old_nodes ? m.bones.size() : extras) != t.second) {
      std::printf("  EXPECTED %d %s\n", t.second, old_nodes ? "bones in all" : "extras");
      rc = 1;
    }
  }
  return rc;
}
