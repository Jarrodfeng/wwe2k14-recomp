// The weapon drawer's pose / bind part (S103 lane PW-3; contract src\renderer_scene\weapon_rows.h, evidence docs\research\s103\PW_adversary.md). Header only, no GPU, no game, no SDK: the applier
// (rf_props.cpp ApplyWeaponRows) and the offline unit (tools\scene\weapon_draw_unit.cpp) run THESE functions.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WHAT IT DOES: each frame the record's three weapon sections (59 slots, 60 per-call rows, 61 node rows) say which model objects the game drew and with which node rows.
//   Plan() (weapon_plan.h, split out of this file): the newest frame of the record, the id from the call itself, the weapon's own submit (r4 0), the ring steps / belts skipped and counted; one item per CALL (two chairs = two items).
//   Bind / Pose: the stored model (assets::LoadM2Weapon: the player's own m2.pac p0/1000/<id>, arena stream layout) is bound to the game's node numbers once (ring_pose.h BindMesh) and skinned with the call's
//            node rows (PoseMesh), exactly as the ring pieces are: slot k of a part's palette is the call's row palette_nodes[k] (sub_82C04DF0 0x82C0A00C-0x82C0A098).
// Material and effect come from the model's own effect names through the loader's read rule (the yProp_* path), not from here.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/asset_model.h"
#include "ring_pose.h"
#include "weapon_plan.h"   // View, Plan, Census, Plants (the pure plan; PW integration: split out so the contributors' unit can run it without the asset code)

namespace rdr_scene::weapons {

inline const char* PacOf() { return "pac\\m2.pac"; }
inline std::string EntryOf(int32_t id, uint32_t entry = 0) { if (BeltSubOfId(id) >= 0) return "belt:" + std::to_string(BeltSubOfId(id)); return "p0/1000/" + std::to_string(id) + (entry ? "/" + std::to_string(entry) : std::string()); }   // the loader's entry name (live_assets.cpp); entry 1 / 2 = a secondary model

// S103 BLF: an EVENT PROP model (entrance belt / mic / cap / stage prop): pac\evt\evtobj.pac, entry "evtprop:<prop id>/<k>" = entry k (0..9) of the PACH the loader resolves from the prop id (assets::PropPach; live_assets.cpp).
// Entry k is the model MakeWeapon built for that entry id (sub_8272D710 loop 0x8272DEAC..0x8272EA58): its own CSkinModel, its own palette, so each (id, k) is its own stored model, posed by its own call's rows.
// The drawer's hook to close the loader's evtobj.pac unpack (~160 MB) when the LAST event prop model is retired (set by live_assets.cpp when it first serves an evtprop entry; null in tools / the viewer: nothing is freed there).
// S103 GBR (BH4 item 4): the release runs on the RENDER thread and the loader's mutex can be held by a worker for seconds (the arena / crowd loads that start as an entrance ends): the hook try_locks and answers false when it could not
// (the caller retries next frame; nothing blocks). Returns true when it is done (released, or nothing to release / no hook).
inline bool (*g_evtobj_release)() = nullptr;
inline bool ReleaseEvtobjSlot() {
  return g_evtobj_release ? g_evtobj_release() : true;
}
// The retry rule (the applier, rf_props.cpp, and the unit run THIS): a wanted release stays pending until the hook answers done. `drop_on_busy` is the planted twin (the old behaviour: one try, a busy answer forgets the release).
// S103 GBR (review 8): `entrance_live` (an event prop is still asked for) cancels a pending release first: it never fires mid-entrance between two loads (the next entrance prop would reopen the 160 MB unpack).
inline void RetryRelease(bool& pending, bool entrance_live = false, bool drop_on_busy = false) {
  if (entrance_live) pending = false;
  if (!pending) return;
  const bool done = ReleaseEvtobjSlot();
  pending = drop_on_busy ? false : !done;
}
inline const char* EvtPacOf() { return "pac\\evt\\evtobj.pac"; }
// S111 FOLLOWUPS3 (docs\research\s111\FOLLOWUPS3.md; READANSWER review F2): `k` may carry kWantRecoloured (weapon_rows.h): the model the game built from its RECOLOURED read (sub_83391F18, /SOBJ/<id>
// only) is entry "evtprop:<id>/<k>/s"; a plain read's model (sub_825AE720) keeps "evtprop:<id>/<k>". One id read both ways in one scene is two models, as in the game (live_assets.cpp: EvtEntryRecoloured).
inline std::string EvtEntryOf(int32_t id, uint32_t k) { return "evtprop:" + std::to_string(id) + "/" + std::to_string(k & ~kWantRecoloured) + ((k & kWantRecoloured) ? "/s" : ""); }
inline bool EvtEntryRecoloured(const std::string& entry) { return entry.size() > 2 && entry.compare(entry.size() - 2, 2, "/s") == 0; }
// S111 FOLLOWUPS3 review F1: the drawer's pool stem of event model (id, k) (rf_props.cpp ApplyWeaponRows: the pool, its bound copy "weapon:bound:" + the stem's tail, its pose slots). The two reads' models
// of one (id, k) are two meshes, so each has its own pool: a shared stem would draw the second model with the bound copy of whichever was bound first. Neither stem is a prefix of the other (PoolTrack and
// ModelCache::RetireOther match by prefix); both start "weapon:ev", so the entrance end's RetireOther("weapon:ev") still takes both.
inline std::string EvtPoolStemOf(int32_t id, uint32_t k, bool recoloured) { return std::string(recoloured ? "weapon:evs" : "weapon:ev") + std::to_string(id) + "/" + std::to_string(k); }

// S103 CGF: a STAGE STRUCTURE piece (steel cage / Hell in a Cell / Elimination Chamber / Inferno): pac\bg\bgEtc.pac, entry "stg:<PACH>/<k>" = nested key 1 entry k of top-level PACH `pach` (docs\formats\bgetc_stg_structures.md);
// textures from the PACH's top-level entry 0 (+ 10). The arena loader's own static copy of bgEtc (assets::OpenProps) serves it: no second unpack, no single-slot cache thrash.
inline const char* StgPacOf() { return "pac\\bg\\bgEtc.pac"; }
inline std::string StgEntryOf(int32_t pach, uint32_t k) { return "stg:" + std::to_string(pach) + "/" + std::to_string(k); }

// S103 GBR: a GMSK set object = pac\gm.pac entry "gm:<arena>/<key>" (the arena's GMGA / GMGB bank, entry 4 key `key`); a blood splat model = the arena's own bg pac, entry "blood:<texture index>" (key 32000 with the n-th entry of key 32010).
inline const char* GmPacOf() { return "pac\\gm.pac"; }
inline std::string GmEntryOf(int arena, uint32_t key, bool holder_tex = false) { return "gm:" + std::to_string(arena) + "/" + std::to_string(key) + (holder_tex ? "/h" : ""); }
// S111 GMBORROW: the loader's reading of GmEntryOf: "gm:<arena>/<key>" (the game handed the stage's own list: settings byte G+41 clear) or "gm:<arena>/<key>/h" (the stage holder's set); false for any other text.
inline bool ParseGmEntry(const std::string& e, int& arena, uint32_t& key, bool& holder_tex) {
  int a = 0, k = 0, n = 0;
  if (std::sscanf(e.c_str(), "gm:%d/%d%n", &a, &k, &n) != 2 || k < 0) return false;
  const std::string rest = e.substr(size_t(n));
  if (!rest.empty() && rest != "/h") return false;
  arena = a, key = uint32_t(k), holder_tex = !rest.empty();
  return true;
}
inline std::string BloodPacOf(int arena) {
  char b[48];
  std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
  return b;
}
inline std::string BloodEntryOf(uint32_t tex) { return "blood:" + std::to_string(tex); }
// The blood colour scalar (bfloat16 of the id) reduced to 16 levels: a pool per (texture, level) keeps the posed copies' material colour fixed (a continuously fading colour must not make a pool per value).
inline uint32_t BloodColourLevel(float c) { return c != c || c <= 0.f ? 0u : c >= 1.f ? 15u : uint32_t(c * 15.f + 0.5f); }
inline float BloodLevelColour(uint32_t level) { return float(level) / 15.f; }

// The stored model's meshes bound to the game's node numbers (BindMesh). Returns how many meshes could not be bound (they stay at their file positions when posed).
inline uint32_t BindModel(const assets::Model& stored, assets::Model& bound) {
  bound = stored;
  uint32_t bad = 0;
  for (assets::Mesh& m : bound.meshes)
    if (!rf::c::BindMesh(m)) ++bad;
  return bad;
}

// S108 FAN: the rows a bound model's vertices ask for: rf::c::RowsNeeded (ring_pose.h; S108 AL1 moved it there, the ring's pieces use it too). A call that gives a skinned model fewer rows leaves every vertex
// past them at its file position (PoseMesh): reported by pose_rows_report.h ReportWeaponRows (the shared draw_shortfall), never only as a count.
using rf::c::RowsNeeded;

// Poses the vertices of `out` (a structural copy of `bound`: same meshes and vertex counts; its textures may be empty) with the call's node rows. `pal` is a caller scratch.
inline void PoseCall(const assets::Model& bound, const View& v, const WeaponCall& w, const Plants& p, std::vector<assets::Matrix>& pal, assets::Model& out) {
  pal.resize(w.node_count);
  for (uint32_t k = 0; k < w.node_count; ++k) {   // the ONE row -> matrix function: the ring's (rf::c::WorldFromRowsCore, ring_pose.h, which rf::c::WorldFromRows calls: row -> matrix and the viewer's mirror); the transpose twin flips the 3x3 part of the row first
    float r[12];
    const float* src = v.nodes + size_t(w.node_first + k) * 12;
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 4; ++b) r[4 * a + b] = (p.transpose_rows && b < 3) ? src[4 * b + a] : src[4 * a + b];
    pal[k] = rf::c::WorldFromRowsCore(r);   // (it already ends in MirrorY: S X S; a second mirror here would undo it)
  }
  if (out.meshes.size() != bound.meshes.size()) out.meshes = bound.meshes;
  for (size_t i = 0; i < bound.meshes.size(); ++i) rf::c::PoseMesh(bound.meshes[i], pal.data(), pal.size(), out.meshes[i]);
}

}  // namespace rdr_scene::weapons
