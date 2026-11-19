// The offline AssetSource of record_frame.h (Session 103, lane R; moved out of tools\scene\rsc_draw.cpp): assets by pac + entry from the player's own files with our
// loaders (the viewer's loader seam, assets::crowd_body). Used by rsc_draw and frame_rig. Header only; the tool links loader_seam_game.cpp + crowd_body.cpp.
//   "pac\bg\bgNN.pac" + "arena" (or "slot K")  -> the whole arena NN        "pac\ch\chNNN.pac" + variant -> wrestler NNN attire V
//   "pac\evt\evtobj.pac" + N                   -> prop N                    "pac\bg\bgNN.pac" + 20000..20199 / "card:TT" -> a crowd person (stored pose)
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/assets/crowd_body.h"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer_scene/record_frame.h"

namespace rdr_scene::offline {

class Assets : public rf::AssetSource {
 public:
  explicit Assets(int keep_src_kind = 0, int drop_src_kind = 0) : keep_kind_(keep_src_kind), drop_kind_(drop_src_kind) {}   // S103 GRF: drop_src_kind > 0 removes the meshes of that src_kind from EVERY model, the arena's too (3 = the crowd baked into a whole-arena model)
  //  // keep_src_kind > 0: keep only meshes of Mesh::src_kind K (3 = the crowd) in non-arena, non-crowd models
  bool Load(const std::string& pac, const std::string& entry, uint32_t variant, assets::Model& m, std::string& err) override {
    std::string norm = pac;
    std::replace(norm.begin(), norm.end(), '/', '\\');
    int id = 0;
    int arena = 0, num = 0, type = 0;
    const bool bg = std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &arena) == 1;
    // S101 lane27: a crowd person: arena pac + entry 20000 + 40 * LOD + type (only LOD 0, the near body, is loaded: the viewer has no other), or "card:TT" (our renderer
    // draws the type's body for the 360's run-time impostor; C2-1).
    if (bg && (std::sscanf(entry.c_str(), "card:%d", &type) == 1 || (std::sscanf(entry.c_str(), "%d", &num) == 1 && num >= 20000 && num < 20200))) {
      if (entry.rfind("card:", 0) != 0) {
        const int lod = (num - 20000) / 40;
        type = (num - 20000) % 40;
        if (lod > 2 || type > 19) { err = "crowd body " + entry + ": not a person body (20000 + 40 * LOD(0..2) + type(0..19))"; return false; }
        if (lod > 0) std::fprintf(stderr, "[rsc_draw] crowd body %s: LOD %d drawn as the near body of type %d\n", entry.c_str(), lod, type);
      }
      if (type < 0 || type > 19) { err = "crowd body type out of range: " + entry; return false; }
      return assets::crowd_body::LoadBody(arena, type, m, err);
    }
    bool ok = false;
    bool filter = true;
    if (bg) { ok = ViewerLoad("arena", arena, 0, m, err); filter = entry != "arena"; }  // (the contributor's arena, loaded by rf_arena.cpp, is never filtered)
    else if (std::sscanf(norm.c_str(), "pac\\ch\\ch%d.pac", &id) == 1) {
      // S103 R: entry "model_key:K" (lane B's wrestler contributor): the model PACH whose EMD name is K, else the same attire's kind 2 (ring) model (live_assets.cpp's rule);
      // `variant` is NOT an attire for such a record. Any other entry: variant = the attire.
      int key = 0, ord = int(variant);
      if (std::sscanf(entry.c_str(), "model_key:%d", &key) == 1) {
        ord = ViewerOrdinalOfModelKey(id, key);
        if (ord < 0) ord = ViewerOrdinalOfModelKey(id, key - key % 10 + 2);
        if (ord < 0) { err = "no model PACH named " + std::to_string(key) + " (or its kind 2) in " + pac; return false; }
      }
      ok = ViewerLoad("wrestler", id, ord, m, err);
    }
    else if (norm == "pac\\evt\\evtobj.pac" && std::sscanf(entry.c_str(), "%d", &id) == 1) ok = ViewerLoad("prop", id, 0, m, err);
    else { err = "no loader for pac \"" + pac + "\" entry \"" + entry + "\""; return false; }
    if (ok && filter && keep_kind_ > 0) {  // S101 lane27: the filter (a picture of one source kind only)
      std::vector<assets::Mesh> kept;
      for (assets::Mesh& me : m.meshes) if (me.src_kind == keep_kind_) kept.push_back(std::move(me));
      m.meshes = std::move(kept);
    }
    if (ok && drop_kind_ > 0) {   // S103 GRF: the arena selftest compares the contributor (no crowd) with the viewer's whole-arena record (crowd inside the model) without the crowd
      std::vector<assets::Mesh> kept;
      for (assets::Mesh& me : m.meshes) if (me.src_kind != drop_kind_) kept.push_back(std::move(me));
      m.meshes = std::move(kept);
    }
    return ok;
  }
 private:
  int keep_kind_;
  int drop_kind_;
};

}  // namespace rdr_scene::offline
