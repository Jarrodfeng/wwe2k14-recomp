// The arena side of reading a scene record back (Session 101, lane 28): what an offline reader (tools\scene\rsc_draw.cpp, later the front-end)
// does with the arena contributor's part of a record. Header only; needs the asset model and the record tables, no GPU, no game, no SDK.
//   ReadArenaRecord   : the slot objects ("arena:slot:K[:r|:y]") and the light sets of a record -> RecordView
//   ApplyToModel      : makes a loaded arena Model show what the record says (light sets by id, the slot -> set choice, the twins, absent slots)
//   SnapshotFromModel : the offline snapshot "built from the files" (the viewer's evaluated sets + the slot text), the input of the selftest
// Until the front-end draws slot objects one by one, rsc_draw draws the whole arena Model once, after ApplyToModel (so the record decides what
// the picture shows).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../assets/asset_model.h"
#include "../renderer_scene/record_io.h"
#include "arena_contrib.h"

namespace rdr_scene::live::arena {

struct RecordView {
  int arena_id = -1;
  struct Slot {
    uint32_t set = kNone;
    bool base = false, r = false, y = false;
  };
  std::map<int, Slot> slots;
  std::map<uint32_t, assets::LightSet> sets;  // by the game's set id
  size_t lights = 0, screens = 0;
};

// True when the asset is one of the arena contributor's slot assets ("pac\bg\bgNN.pac" + entry "slot K"); fills the arena id and the slot.
inline bool ParseSlotAsset(const SceneTables& t, const AssetRef& a, int& arena, int& slot) {
  if (a.kind != uint8_t(AssetKind::Model)) return false;
  std::string pac(t.Str(a.pac));
  std::replace(pac.begin(), pac.end(), '/', '\\');
  int id = 0;
  if (std::sscanf(pac.c_str(), "pac\\bg\\bg%d.pac", &id) != 1) return false;
  const std::string entry(t.Str(a.entry));
  int k = 0;
  if (std::sscanf(entry.c_str(), "slot %d", &k) != 1) return false;
  arena = id, slot = k;
  return true;
}

inline bool ReadArenaRecord(const SceneTables& t, RecordView& v, std::string& err) {
  v = RecordView();
  for (const Object& o : t.objects) {
    if (o.asset >= t.assets.size()) continue;
    int arena = 0, slot = 0;
    if (!ParseSlotAsset(t, t.assets[o.asset], arena, slot)) continue;
    if (v.arena_id >= 0 && v.arena_id != arena) {
      err = "the record names two arenas (" + std::to_string(v.arena_id) + " and " + std::to_string(arena) + ")";
      return false;
    }
    v.arena_id = arena;
    RecordView::Slot& s = v.slots[slot];
    s.set = o.light_set;
    const std::string id(t.Str(o.id));
    if (id.size() > 2 && id.compare(id.size() - 2, 2, ":r") == 0) {
      if (!(o.flags & kObjMirrorX)) { err = "object " + id + " is a mirrored twin without kObjMirrorX"; return false; }
      s.r = (o.flags & kObjVisible) != 0;
    } else if (id.size() > 2 && id.compare(id.size() - 2, 2, ":y") == 0) {
      if (!(o.flags & kObjTurned)) { err = "object " + id + " is a turned twin without kObjTurned"; return false; }
      s.y = (o.flags & kObjVisible) != 0;
    } else {
      s.base = (o.flags & kObjVisible) != 0;
    }
  }
  for (const Light& l : t.lights) {
    const bool fresh = v.sets.find(l.set) == v.sets.end();
    assets::LightSet& ls = v.sets[l.set];
    if (fresh)
      for (float& f : ls.fall) f = -1.f;  // unknown until a directional light of the set says otherwise
    ls.id = int(l.set);
    if (l.kind == uint8_t(LightKind::Directional) && l.slot < 4) {
      std::memcpy(ls.dir[l.slot], l.dir_or_pos, sizeof ls.dir[0]);
      std::memcpy(ls.col[l.slot], l.color, sizeof ls.col[0]);
      ls.fall[l.slot] = l.range;
    } else if (l.kind == uint8_t(LightKind::Directional) && l.slot == 4) {
      std::memcpy(ls.hl_dir, l.dir_or_pos, sizeof ls.hl_dir);
      std::memcpy(ls.hl_col, l.color, sizeof ls.hl_col);
    } else if (l.kind == uint8_t(LightKind::Ambient) && l.slot == 5) {
      std::memcpy(ls.sky, l.color, sizeof ls.sky);
    } else if (l.kind == uint8_t(LightKind::Ambient) && l.slot == 6) {
      std::memcpy(ls.ground, l.color, sizeof ls.ground);
    }
    ++v.lights;
  }
  v.screens = t.movies.size();
  return true;
}

struct ApplyStats {
  int sets_replaced = 0, sets_added = 0, slots_set = 0, slots_hidden = 0, twins_dropped = 0, twins_kept = 0;
};

inline int FindSetIndex(const assets::Model& m, uint32_t id) {
  int best = -1;
  for (size_t i = 0; i < m.light_sets.size(); ++i)
    if (m.light_sets[i].id == int(id)) {
      if (m.light_sets[i].edit_id != -1) return int(i);
      if (best < 0) best = int(i);
    }
  return best;
}

inline ApplyStats ApplyToModel(const RecordView& v, assets::Model& m) {
  ApplyStats st;
  // 1. light sets by id: the record's evaluated values replace the model's (fallback copies of the same id too)
  for (assets::LightSet& ls : m.light_sets) {
    if (ls.id < 0) continue;
    auto it = v.sets.find(uint32_t(ls.id));
    if (it == v.sets.end()) continue;
    const assets::LightSet& r = it->second;
    std::memcpy(ls.dir, r.dir, sizeof ls.dir);
    std::memcpy(ls.col, r.col, sizeof ls.col);
    std::memcpy(ls.sky, r.sky, sizeof ls.sky);
    std::memcpy(ls.ground, r.ground, sizeof ls.ground);
    for (int k = 0; k < 4; ++k)
      if (r.fall[k] >= 0.f) ls.fall[k] = r.fall[k];  // review F1: a negative range is 'unknown': the model's own falloff stays
    ++st.sets_replaced;
  }
  // 2. the slot -> set choice, and which slots / twins exist
  std::map<int, std::vector<size_t>> by_slot;
  for (size_t i = 0; i < m.meshes.size(); ++i) {
    int k = 0;
    if (std::sscanf(m.meshes[i].part.c_str(), "slot %d", &k) == 1 && m.meshes[i].part == "slot " + std::to_string(k)) by_slot[k].push_back(i);
  }
  const assets::Matrix mirror = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const assets::Matrix turned = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
  for (auto& kv : by_slot) {
    const int slot = kv.first;
    auto rs = v.slots.find(slot);
    if (rs == v.slots.end()) {
      // review F5: only the stage slots 0-399 are hidden for being unlisted (they are the ones with a text row); the screens 800-809 and the ring
      // keys 951-974 are not in 50001 and are never hidden here
      if (slot >= 400) continue;
      // a slot the record does not list: drawn zero times
      assets::Placement pl;
      pl.label = "slot " + std::to_string(slot) + " absent from the record";
      m.placements.push_back(std::move(pl));
      for (size_t i : kv.second) m.meshes[i].placement = int(m.placements.size()) - 1;
      ++st.slots_hidden;
      continue;
    }
    const RecordView::Slot& s = rs->second;
    if (s.set != kNone) {
      int idx = FindSetIndex(m, s.set);
      if (idx < 0) {
        auto it = v.sets.find(s.set);
        if (it != v.sets.end()) {
          m.light_sets.push_back(it->second);
          for (float& f : m.light_sets.back().fall) f = f < 0.f ? 0.f : f;  // no asset value to keep for a set the model lacks
          idx = int(m.light_sets.size()) - 1;
          ++st.sets_added;
        }
      }
      if (idx >= 0) {
        for (size_t i : kv.second) m.meshes[i].light_set = idx;
        ++st.slots_set;
      }
    }
    // twins: only slots the loader gave a mirror placement ("slot K mirrored" / "slot K turned")
    const std::string want_a = "slot " + std::to_string(slot) + " mirrored", want_b = "slot " + std::to_string(slot) + " turned";
    for (assets::Placement& pl : m.placements) {
      if (pl.label != want_a && pl.label != want_b) continue;
      const size_t before = pl.transforms.size();
      pl.transforms.clear();
      if (s.base) pl.transforms.push_back(assets::Identity());
      if (s.r) pl.transforms.push_back(mirror);
      if (s.y) pl.transforms.push_back(turned);
      const size_t after = pl.transforms.size();
      if (after < before) st.twins_dropped += int(before - after);
      else st.twins_kept += int(after);
    }
  }
  return st;
}

// ---- the offline snapshot "built from the files" ---------------------------------------------------------------------------------------
// `m` = the arena as the viewer loads it (its light sets already evaluated at the stage key's time), `slot_text` = the arena pac's entry 50001.
// Slots = the "slot K" parts the model holds (so the match look's hidden slots are not listed), with the text's choices; sets = the model's
// non-fallback sets with their evaluated values and falloff.
inline ArenaSnapshot SnapshotFromModel(const assets::Model& m, int arena_id, std::string_view slot_text, Look look) {
  ArenaSnapshot s;
  s.arena_id = arena_id;
  s.look = look;
  std::map<int, SlotSnap> text;
  for (const SlotSnap& r : ParseSlotText(slot_text)) text[r.slot] = r;
  std::map<int, bool> present;
  for (const assets::Mesh& me : m.meshes) {
    int k = 0;
    if (std::sscanf(me.part.c_str(), "slot %d", &k) == 1 && me.part == "slot " + std::to_string(k)) present[k] = true;
  }
  for (auto& kv : present) {
    SlotSnap sl;
    auto it = text.find(kv.first);
    if (it != text.end()) sl = it->second;
    else sl.slot = uint16_t(kv.first);  // no text row: light_set stays kNone
    s.slots.push_back(sl);
    if (kv.first >= 800 && kv.first <= 809) {
      ScreenSnap sc;
      sc.key = uint16_t(kv.first);
      s.screens.push_back(sc);
    }
  }
  std::map<int, bool> seen;
  for (const assets::LightSet& ls : m.light_sets) {
    if (ls.id < 0 || ls.edit_id == -1 || seen[ls.id]) continue;
    seen[ls.id] = true;
    SetSnap n;
    n.set_id = uint32_t(ls.id);
    n.n_lights = 4;
    std::memcpy(n.dir, ls.dir, sizeof ls.dir);
    std::memcpy(n.col, ls.col, sizeof ls.col);
    std::memcpy(n.block_a, ls.sky, sizeof n.block_a);
    std::memcpy(n.block_b, ls.ground, sizeof n.block_b);
    n.has_fall = true;
    std::memcpy(n.fall, ls.fall, sizeof n.fall);
    s.sets.push_back(n);
  }
  return s;
}

}  // namespace rdr_scene::live::arena
