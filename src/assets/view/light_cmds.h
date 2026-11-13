// Session 98 (plan step 13, viewer side): the pipe / UI commands of the lighting editor (light_list, light_set, light_ab, light_reset,
// light_save, light_load, light_panel). The edit model is src\assets\lighting_edit.h; this file ties it to the viewer: the loaded
// arena's key, the renderer's per-set override (Renderer::SetSetEdit), the mod files. Header-only, included by viewer_main.cpp.
//
//   {"cmd":"light_list"}                           the sets of the loaded arena (or, in the entrance look, the entrance rig's) with their values
//   {"cmd":"light_set","set":121,"light":0,"col":[r,g,b],"yaw":D,"pitch":D}   one light (0-3): colour (0..N, may exceed 1) and / or direction
//   {"cmd":"light_set","set":121,"sky":[r,g,b],"ground":[r,g,b]}              sky / ground colours (can be combined with "light")
//   {"cmd":"light_set","set":121,"light":0,"intensity":2.0}                   scale the light's colour so its brightest channel is 2.0
//   {"cmd":"light_ab","on":true|false}             edited vs original (no "on" flips)
//   {"cmd":"light_reset"[,"set":N]}                one set or everything back to the file's values
//   {"cmd":"light_save","dir":"...\\lighting"[,"name":"..","wrestler":N,"cue":C]}   arena_NN.json (match look) or entrance_NNN.json (entrance look)
//   {"cmd":"light_load","file":"...\\arena_05.json"}
//   {"cmd":"light_panel","open":true|false}        the UI's LIGHTING window
// Entrance mode: "cue":C (sticky) picks the cue edited / listed / shown; light 0-4 (4 has no preview); a save writes every cue with edits,
// only the changed fields. A save over an existing file keeps the old one as <name>.bak. Edits are kept per arena for the session.
// "mode":"arena"|"entrance" overrides the automatic choice (entrance look on = entrance). Arena edits are key patches (see
// lighting_edit.h); entrance edits are static per-set values (the entrance_NNN.json phase-1 format, UNCONFIRMED in the game).
#pragma once

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../lighting_edit.h"
#include "../mods_lookup.h"
#include "../stage_key.h"
#include "json_lite.h"
#include "loader_seam.h"
#include "renderer.h"
#include "ui_light.h"

namespace view {

struct LightCtl {
  // S101 lane33: the viewer turns the camera-attached lights of an edited set (the edit replaces the whole set at the draw: renderer.cpp MakeDrawCB)
  std::function<void(assets::LightSet&)> post_eval;
  // arena mode
  // One editor per arena for the whole session (edits survive switching arenas; nothing is lost until the viewer closes).
  std::map<int, std::unique_ptr<assets::lighting_edit::Editor>> eds;
  assets::lighting_edit::Editor none_;  // (never loaded: what Ed() gives while no arena's key is current)
  assets::lighting_edit::Editor* cur_ed = nullptr;
  assets::lighting_edit::Editor& Ed() { return cur_ed ? *cur_ed : none_; }
  int key_arena = -1;      // the arena `Ed()` holds (-1 none)
  std::string key_err;
  // entrance mode: the loaded model's sets (what is drawn, entrance colours included) and the static edits. One entry per
  // (cue, set): only the fields the user changed are flagged (and written to entrance_NNN.json). Light 4 has no preview (the
  // viewer's shader has 4 lights) but is stored and saved. UNCONFIRMED in the game.
  struct EntEdit {
    assets::LightSet cur;
    bool col[5] = {}, dir[5] = {}, sky = false, ground = false;
    float col4[3] = {}, dir4[3] = {};
  };
  std::vector<assets::LightSet> model_sets;
  std::map<int, std::map<int, EntEdit>> ent_all;  // cue -> set id -> edit
  std::map<int, std::map<int, std::map<int, EntEdit>>> ent_stash;  // arena -> ent_all (kept across arena switches)
  int cue = 0;  // the cue whose entrance edits are shown / edited (sticky; "cue" argument of light_set / light_list / light_save)
  std::map<int, EntEdit>& EntCur() { return ent_all[cue]; }
  bool ab = true;
  int cur_arena = -1;

  static std::string F(double v, int d = 4) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
  }
  static std::string Err(const std::string& m) { return "{\"ok\":false,\"error\":\"" + jl::Esc(m) + "\"}"; }
  static std::string V3(const float* v) { return "[" + F(v[0]) + "," + F(v[1]) + "," + F(v[2]) + "]"; }

  static void DirToAngles(const float* d, double& yaw, double& pitch) {
    pitch = std::asin(std::max(-1.0, std::min(1.0, double(d[2])))) * 180.0 / 3.14159265358979;
    yaw = std::atan2(-double(d[1]), -double(d[0])) * 180.0 / 3.14159265358979;
  }
  static void AnglesToDir(double yaw, double pitch, float* d) {
    const double k = 3.14159265358979 / 180.0, cp = std::cos(pitch * k);
    d[0] = float(-cp * std::cos(yaw * k)), d[1] = float(-cp * std::sin(yaw * k)), d[2] = float(std::sin(pitch * k));
  }

  bool EntranceMode(const jl::Value& cmd, int stage_wrestler) const {
    const std::string m = cmd.str("mode", stage_wrestler >= 0 ? "entrance" : "arena");
    return m == "entrance";
  }

  // The arena's key (read once per arena).
  bool EnsureKey(int arena, std::string& err) {
    if (arena < 0) return err = "load an arena first", false;
    if (key_arena == arena && cur_ed && cur_ed->Loaded()) return true;
    if (auto it = eds.find(arena); it != eds.end()) return cur_ed = it->second.get(), key_arena = arena, true;
    char rel[64];
    std::snprintf(rel, sizeof rel, "pac\\bg\\bg%02d.pac", arena);
    std::vector<uint8_t> raw;
    assets::Bytes key;
    auto ne = std::make_unique<assets::lighting_edit::Editor>();
    if (!ViewerReadFile(rel, raw, err) || !assets::lighting_edit::ReadArenaKey(raw, key, err) || !ne->Load(key, err)) return key_arena = -1, cur_ed = nullptr, false;
    cur_ed = ne.get();
    eds[arena] = std::move(ne);
    key_arena = arena;
    return true;
  }

  // Called by DoLoad after a model is added to the renderer.
  void OnLoad(Renderer& r, bool is_arena, int id, bool add, const std::vector<assets::LightSet>& sets, int stage_wrestler) {
    if (!is_arena || add) return;
    model_sets.clear();
    for (const assets::LightSet& s : sets)  // (not the unlit -800 set, not a fallback copy: the game patches neither)
      if (s.id >= 0 && s.edit_id != -1) model_sets.push_back(s);
    if (id != cur_arena) {
      if (cur_arena >= 0) ent_stash[cur_arena] = std::move(ent_all);
      ent_all.clear();
      if (auto it = ent_stash.find(id); it != ent_stash.end()) ent_all = it->second;
      cur_arena = id;
      r.ClearSetEdits();
      if (auto it = eds.find(id); it != eds.end()) cur_ed = it->second.get(), key_arena = id;
      else cur_ed = nullptr, key_arena = -1;
      ab = true;
    }
    Apply(r, stage_wrestler >= 0);
  }

  // Push the edits to the renderer (or clear them while A is shown).
  void Apply(Renderer& r, bool entrance) {
    r.ClearSetEdits();
    if (!ab) return;
    const double t = assets::stage_key::Cfg().time;
    // (the entrance look shows the entrance rig's edits; the match look shows the arena key's: the key's values would replace the rig's colours)
    if (!entrance && Ed().Loaded() && key_arena == cur_arena && assets::stage_key::Cfg().enabled)
      for (int id : Ed().SetIds())
        if (Ed().Edited(id)) {
          assets::LightSet ls;
          if (Ed().Eval(id, t, ls)) {
            if (post_eval) post_eval(ls);  // S101 lane33
            r.SetSetEdit(id, &ls);
          }
        }
    if (entrance)
      for (auto& kv : EntCur()) r.SetSetEdit(kv.first, &kv.second.cur);
  }

  const assets::LightSet* ModelSet(int id) const {
    for (const assets::LightSet& s : model_sets)
      if (s.id == id) return &s;
    return nullptr;
  }

  std::string SetJson(const assets::LightSet& s, bool edited, bool used, double t, int arena_k, const EntEdit* ee = nullptr) {
    std::string o = "{\"id\":" + std::to_string(s.id) + ",\"name\":\"" + jl::Esc(assets::lighting_edit::SetName(s.id)) + "\",\"edited\":" +
                    (edited ? "true" : "false") + ",\"used\":" + (used ? "true" : "false") + ",\"lights\":[";
    for (int k = 0; k < 4; ++k) {
      double yaw = 0, pitch = 0;
      if (arena_k >= 0 && Ed().Angles(s.id, k, t, yaw, pitch)) {
      } else {
        DirToAngles(s.dir[k], yaw, pitch);
      }
      const bool has = arena_k < 0 || Ed().HasLight(s.id, k);
      o += std::string(k ? "," : "") + "{\"col\":" + V3(s.col[k]) + ",\"dir\":" + V3(s.dir[k]) + ",\"yaw\":" + F(yaw, 2) + ",\"pitch\":" + F(pitch, 2) +
           ",\"has\":" + (has ? "true" : "false") + "}";
    }
    if (arena_k < 0) {  // the entrance rig has a 5th light (no preview: the viewer's shader has 4)
      double yaw = 0, pitch = 0;
      const float zero[3] = {0, 0, 0};
      const float* c4 = ee ? ee->col4 : zero;
      const float* d4 = ee ? ee->dir4 : zero;
      DirToAngles(d4, yaw, pitch);
      o += ",{\"col\":" + V3(c4) + ",\"dir\":" + V3(d4) + ",\"yaw\":" + F(yaw, 2) + ",\"pitch\":" + F(pitch, 2) + ",\"has\":true,\"preview\":false}";
    }
    return o + "],\"sky\":" + V3(s.sky) + ",\"ground\":" + V3(s.ground) + "}";
  }

  std::string Handle(const std::string& c, const jl::Value& cmd, Renderer& r, int last_arena, int stage_wrestler) {
    const bool ent = EntranceMode(cmd, stage_wrestler);
    std::string err;
    if (c == "light_panel") {
      bool& open = ui::LightingPanelOpen();
      open = cmd.has("open") ? (cmd.get("open")->t == jl::Value::Bool ? cmd.get("open")->b : cmd.get("open")->n != 0) : !open;
      return std::string("{\"ok\":true,\"open\":") + (open ? "true" : "false") + "}";
    }
    if (last_arena < 0) return Err("load an arena first ({\"cmd\":\"load\",\"kind\":\"arena\",\"id\":5})");
    cur_arena = last_arena;
    const double t = assets::stage_key::Cfg().time;
    if (cmd.has("cue") && cmd.get("cue")->t == jl::Value::Num) cue = int(cmd.num("cue", 0));
    if (!ent && !EnsureKey(last_arena, err)) return Err(err);

    if (c == "light_list") {
      std::string o = "{\"ok\":true,\"arena\":" + std::to_string(last_arena) + ",\"mode\":\"" + (ent ? "entrance" : "arena") + "\",\"ab\":" + (ab ? "true" : "false") +
                      ",\"key_enabled\":" + (assets::stage_key::Cfg().enabled ? "true" : "false") + ",\"time\":" + F(t, 1);
      std::string sets;
      int edited_n = 0;
      if (ent) {
        o += ",\"cue\":" + std::to_string(cue) + ",\"unconfirmed\":true";
        for (const assets::LightSet& s : model_sets) {
          auto it = EntCur().find(s.id);
          const bool e = it != EntCur().end();
          if (e) ++edited_n;
          sets += (sets.empty() ? "" : ",") + SetJson(e ? it->second.cur : s, e, true, t, -1, e ? &it->second : nullptr);
        }
      } else {
        o += ",\"key_fnv\":\"" + assets::lighting_edit::Hex64(Ed().Id().fnv) + "\",\"key_size\":" + std::to_string(Ed().Id().size);
        for (int id : Ed().SetIds()) {
          assets::LightSet ls;
          if (!Ed().Eval(id, t, ls)) continue;
          const bool e = Ed().Edited(id);
          if (e) ++edited_n;
          sets += (sets.empty() ? "" : ",") + SetJson(ls, e, ModelSet(id) != nullptr, t, 0);
        }
        o += ",\"patches\":" + std::to_string(Ed().PatchCount());
      }
      return o + ",\"edited\":" + std::to_string(edited_n) + ",\"sets\":[" + sets + "]}";
    }

    if (c == "light_ab") {
      if (const jl::Value* on = cmd.get("on")) ab = on->t == jl::Value::Bool ? on->b : on->n != 0;
      else ab = !ab;
      Apply(r, ent);
      return std::string("{\"ok\":true,\"ab\":") + (ab ? "true" : "false") + "}";
    }

    if (c == "light_reset") {
      if (cmd.has("set")) {
        const int id = int(cmd.num("set", 0));
        if (ent) EntCur().erase(id);
        else Ed().ResetSet(id);
      } else {
        if (ent) EntCur().clear();
        else Ed().ResetAll();
      }
      Apply(r, ent);
      return "{\"ok\":true}";
    }

    if (c == "light_set") {
      if (!cmd.has("set")) return Err("missing set");
      const int id = int(cmd.num("set", 0));
      auto vec3 = [&](const char* k, float* o) {
        const jl::Value* v = cmd.get(k);
        if (!v) return false;
        if (v->t != jl::Value::Arr || v->a.size() < 3) return false;
        for (int i = 0; i < 3; ++i) o[i] = float(v->a[i].n);
        return true;
      };
      bool swing = false;
      const bool has_light = cmd.has("light");
      const int k = int(cmd.num("light", 0));
      if (has_light && (k < 0 || k > 4)) return Err("light must be 0..4 (4 has no preview: the viewer draws 4 lights)");
      float col[3] = {0, 0, 0}, sky[3], gnd[3];
      bool hc = has_light && vec3("col", col);
      const bool hs = vec3("sky", sky), hg = vec3("ground", gnd);
      const bool hi = has_light && cmd.has("intensity");
      const bool hy = has_light && (cmd.has("yaw") || cmd.has("pitch"));
      if (!hc && !hs && !hg && !hi && !hy) return Err("nothing to set (light + col / intensity / yaw / pitch, or sky / ground)");
      if (ent) {
        const assets::LightSet* base = ModelSet(id);
        if (!base) return Err("set " + std::to_string(id) + " is not in the loaded scene");
        auto& slot = EntCur();
        if (!slot.count(id)) slot[id].cur = *base;
        EntEdit& ee = slot[id];
        float* ccol = k < 4 ? ee.cur.col[k] : ee.col4;
        float* cdir = k < 4 ? ee.cur.dir[k] : ee.dir4;
        if (has_light) {
          if (hi) {
            float src[3] = {hc ? col[0] : ccol[0], hc ? col[1] : ccol[1], hc ? col[2] : ccol[2]};
            float m = std::max({src[0], src[1], src[2]});
            if (m <= 1e-6f) src[0] = src[1] = src[2] = m = 1.f;  // black: start from white
            const float want = float(cmd.num("intensity", 1)), sc = want / m;
            for (int i = 0; i < 3; ++i) col[i] = src[i] * sc;
            hc = true;
          }
          if (hc) {
            for (int i = 0; i < 3; ++i) ccol[i] = col[i];
            ee.col[k] = true;
          }
          if (hy) {
            double y, p;
            DirToAngles(cdir, y, p);
            AnglesToDir(cmd.num("yaw", y), cmd.num("pitch", p), cdir);
            ee.dir[k] = true;
          }
        }
        if (hs) std::memcpy(ee.cur.sky, sky, sizeof sky), ee.sky = true;
        if (hg) std::memcpy(ee.cur.ground, gnd, sizeof gnd), ee.ground = true;
      } else {
        assets::LightSet cur;
        if (!Ed().Eval(id, t, cur)) return Err("set " + std::to_string(id) + " is not in this arena's light key");
        if (has_light && k == 4 && hi) return Err("light 4: pass col (no intensity: the viewer does not evaluate it)");
        if (has_light) {
          if (hi) {
            float src[3] = {hc ? col[0] : cur.col[k][0], hc ? col[1] : cur.col[k][1], hc ? col[2] : cur.col[k][2]};
            float m = std::max({src[0], src[1], src[2]});
            if (m <= 1e-6f) src[0] = src[1] = src[2] = m = 1.f;  // black: start from white
            const float want = float(cmd.num("intensity", 1)), sc = want / m;
            for (int i = 0; i < 3; ++i) col[i] = src[i] * sc;
            hc = true;
          }
          if (hc && !Ed().SetColor(id, 10 + k, col, t, err)) return Err(err);
          if (hy) {
            double y, p;
            if (!Ed().Angles(id, k, t, y, p)) return Err("set " + std::to_string(id) + " has no light " + std::to_string(k));
            if (!Ed().SetDir(id, k, cmd.num("yaw", y), cmd.num("pitch", p), t, err)) return Err(err);
            swing = Ed().LastSwing();
          }
        }
        if (hs && !Ed().SetColor(id, 0, sky, t, err)) return Err(err);
        if (hg && !Ed().SetColor(id, 1, gnd, t, err)) return Err(err);
      }
      ab = true;
      Apply(r, ent);
      return "{\"ok\":true,\"patches\":" + std::to_string(Ed().PatchCount()) + (swing ? ",\"warn\":\"may swing: a key's angle passed +-180 degrees and was wrapped\"" : "") + "}";
    }

    if (c == "light_save") {
      std::string dir = cmd.str("dir");
      if (dir.empty()) dir = assets::mods_lookup::LightingDir();  // Session 98 step 14: <mods folder>\lighting (--mods / the mods command)
      if (dir.empty()) return Err("pass dir = the mods\\lighting folder to write into (or start the viewer with --mods)");
      namespace fs = std::filesystem;
      std::error_code ec;
      fs::create_directories(fs::u8path(dir), ec);
      char fn[64];
      std::string text;
      if (ent) {
        if (stage_wrestler < 0 && !cmd.has("wrestler")) return Err("entrance mod: show a wrestler's entrance look first (stage wrestler N) or pass wrestler");
        const int w = cmd.has("wrestler") ? int(cmd.num("wrestler", 0)) : stage_wrestler;
        std::snprintf(fn, sizeof fn, "entrance_%03d.json", w);
        // Every cue that has edits is written; only the fields the user changed (UNCONFIRMED in the game: phase 1, static per cue).
        text = "{\n \"format\": 1,\n \"kind\": \"entrance\",\n \"wrestler\": " + std::to_string(w) + ",\n \"cues\": [";
        int ncue = 0;
        for (auto& cv : ent_all) {
          std::string slots;
          int slot = 0;
          for (auto& kv : cv.second) {
            if (kv.first < 0) continue;  // (the viewer's unlit -800 set is not a game set)
            const EntEdit& e = kv.second;
            int last = -1;
            for (int k = 0; k < 5; ++k)
              if (e.col[k] || e.dir[k]) last = k;
            if (last < 0 && !e.sky && !e.ground) continue;
            std::string o = std::string(slot ? ",\n  " : "\n  ") + "{\"slot\": " + std::to_string(slot) + ", \"set\": " + std::to_string(kv.first);
            if (e.sky) o += ", \"sky\": " + V3(e.cur.sky);
            if (e.ground) o += ", \"ground\": " + V3(e.cur.ground);
            if (last >= 0) {
              o += ", \"lights\": [";
              for (int k = 0; k <= last; ++k) {
                const float* c4 = k < 4 ? e.cur.col[k] : e.col4;
                const float* d4 = k < 4 ? e.cur.dir[k] : e.dir4;
                o += std::string(k ? ", " : "") + "{";
                if (e.dir[k]) o += "\"dir\": " + V3(d4);
                if (e.col[k]) o += std::string(e.dir[k] ? ", " : "") + "\"color\": " + V3(c4);
                o += "}";
              }
              o += "]";
            }
            slots += o + "}";
            ++slot;
          }
          if (slot == 0) continue;
          text += std::string(ncue++ ? ",\n " : "\n ") + "{\"cue\": " + std::to_string(cv.first) + ", \"slots\": [" + slots + "\n ]}";
        }
        if (ncue == 0) return Err("no entrance edits to save");
        text += "\n ]\n}\n";
      } else {
        if (Ed().PatchCount() == 0) return Err("no edits to save");
        std::snprintf(fn, sizeof fn, "arena_%02d.json", last_arena);
        text = Ed().ToJson(last_arena, cmd.str("name", "viewer lighting edit (arena " + std::to_string(last_arena) + ")"));
      }
      const fs::path path = fs::u8path(dir) / fn;
      bool backed = false;
      if (fs::exists(path, ec)) {  // never overwrite silently: the old file becomes <name>.bak
        fs::path bak = path;
        bak += ".bak";
        fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
        if (ec) return Err("cannot back up " + path.string() + " (nothing written)");
        backed = true;
      }
      std::ofstream f(path, std::ios::binary);
      if (!f) return Err("cannot write " + path.string());
      f << text;
      f.close();
      const auto u8 = path.u8string();
      const std::string ps(u8.begin(), u8.end());
      return "{\"ok\":true,\"file\":\"" + jl::Esc(ps) + "\",\"mode\":\"" + (ent ? "entrance" : "arena") + "\",\"backup\":" + (backed ? "true" : "false") +
             ",\"patches\":" + std::to_string(ent ? EntCur().size() : Ed().PatchCount()) + "}";
    }

    if (c == "light_load") {
      const std::string file = cmd.str("file");
      if (file.empty()) return Err("missing file");
      std::ifstream f(std::filesystem::u8path(file), std::ios::binary);
      if (!f) return Err("cannot read " + file);
      std::stringstream ss;
      ss << f.rdbuf();
      const std::string text = ss.str();
      jl::Value j;
      std::string pe;
      if (!jl::Parse(text, j, pe)) return Err("not JSON: " + pe);
      if (j.str("kind", "arena") == "entrance") {
        const jl::Value* cues = j.get("cues");
        if (!cues || cues->t != jl::Value::Arr || cues->a.empty()) return Err("entrance mod without cues");
        ent_all.clear();
        // (the game's Vec rule: a whole array with a non-number is ignored)
        auto v3 = [&](const jl::Value* v, float* o) {
          if (!v || v->t != jl::Value::Arr || v->a.size() < 3) return false;
          for (int i = 0; i < 3; ++i)
            if (v->a[i].t != jl::Value::Num) return false;
          for (int i = 0; i < 3; ++i) o[i] = float(v->a[i].n);
          return true;
        };
        int n = 0;
        for (const jl::Value& cv : cues->a) {
          const jl::Value* sl = cv.get("slots");
          const jl::Value* cn = cv.get("cue");
          if (!sl || sl->t != jl::Value::Arr || !cn || cn->t != jl::Value::Num) continue;
          for (const jl::Value& s : sl->a) {
            const jl::Value* sn = s.get("set");
            if (!sn || sn->t != jl::Value::Num) continue;
            const int id = int(sn->n);
            const assets::LightSet* base = ModelSet(id);
            if (!base || id < 0) continue;
            EntEdit& e = ent_all[int(cn->n)][id];
            e.cur = *base;
            e.sky = v3(s.get("sky"), e.cur.sky), e.ground = v3(s.get("ground"), e.cur.ground);
            if (const jl::Value* ls = s.get("lights"); ls && ls->t == jl::Value::Arr)
              for (size_t k = 0; k < ls->a.size() && k < 5; ++k) {
                e.dir[k] = v3(ls->a[k].get("dir"), k < 4 ? e.cur.dir[k] : e.dir4);
                e.col[k] = v3(ls->a[k].get("color"), k < 4 ? e.cur.col[k] : e.col4);
              }
            ++n;
          }
        }
        ab = true;
        Apply(r, true);
        return "{\"ok\":true,\"mode\":\"entrance\",\"sets\":" + std::to_string(n) + "}";
      }
      if (!EnsureKey(last_arena, err)) return Err(err);
      if (!Ed().FromJson(text, err)) return Err(err);
      ab = true;
      Apply(r, false);
      return "{\"ok\":true,\"mode\":\"arena\",\"patches\":" + std::to_string(Ed().PatchCount()) + "}";
    }
    return Err("unknown light command '" + c + "'");
  }
};

}  // namespace view
