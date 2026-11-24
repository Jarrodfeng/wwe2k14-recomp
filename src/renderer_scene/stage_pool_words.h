// The entrance page's floor pools + follow spot, read from the record's light object (Session 103, lane L2; docs\renderer\STAGE_LIGHTING.md "The live read"). Header only, no game or SDK includes.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WHERE THE VALUES ARE [C, listings + run 14]. sub_827F8808 copies, per entry i of the manager's second list, the rig object (+136)[i]'s evaluated rows into the light object (+140)[i] at R = object + 48. Its unrolled
// tail (generated\default\wwe2k14_recomp.34.cpp, after the 5-light loop; r29 = 7 there, so entry i = track t = 7 + i, i = 0..9) stores, per track:
//   R + 416 + 16 (t - 7)  <- the rig object's row 16 * (267 + t)        the CURRENT position (x at +0, y +4, z +8, w +12)  ["ld r9,0(r6)/ld r6,8(r6) ... std r3,416(r31) / 424(r31)"; stride 16: 432, 448, ...]
//   R + 576 + 16 (t - 7)  <- the rig object's row 16 * (216 + t)        (the rotation / direction row)
//   R + 736 + 16 (t - 7)  <- the rig object's block at 4816 + 80 t + 32  the CURRENT tint (xyz, w)  ["ld r7,4848(r11) / std r7,736(r31)" for t = 7: 4848 = 4816 + 32; "ld r9,5088(r11) / std r9,784(r31)": 5088 = 4816 + 80 * 3 + 32, t = 10]
// (sub_82417810 lerps the block's +32 between +0 (from) and +16 (target); the row 267 + t is the lerp of 250 (from) and 233 (target): the game's own readers use the current ones: lane L2 review B1.) The light object is the 896-byte block
// A2's kScLightObjects records (LightState::light order: word w = R + 4 w), so the position row of track t is words [104 + 4 (t - 7) ..] and its tint words [184 + 4 (t - 7) ..], t = 7 .. 16: the last tint ends at word 224.
// Track 7 = the follow spot (rig track 50), t = 8 = track 51 (drawn only with the binding words, S103 TNB), t = 9..16 = the eight pools (tracks 52..59). The pool slot is entry 2 of the list (run 14: the crowd's rig object). [C run 14, STAGE_LIGHTING.md]
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "record_io.h"
#include "rows_inputs.h"   // S106 G4-2
#include "rows_stage_look.h"

namespace rdr_scene::stage_pool_words {

inline constexpr uint32_t kPoolSlot = 2;                  // the pool object index in the manager's second list (kLoPool j = 2)
inline constexpr uint32_t kFirstTrack = 7, kLastTrack = 16;
inline constexpr float kPageC = 3000.f;   // C = [0x83622180] (an image constant, read once at 0x825BAD18): the sprite's world size = ints x C / 512 (TEN_ADV 3c [C]: 64 px -> 375, 32 px -> 187.5)
inline constexpr uint32_t kPosWord0 = 104, kTintWord0 = 184;   // R + 416 and R + 736, in words; 4 words a track
inline constexpr uint32_t kMaxRows = 10;                  // the 10 tracks (S103 TNB: track 1 joins; before: the spot + 8 pools)

inline float W2F(uint32_t w) {
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// S103 TNB (docs\agents\s103\TEN.md): the game's PER-TRACK binding. sub_825BACE0 walks the 10 tracks (i = 0..9, t = 7 + i); at phase 4 a track whose manager word [mgr + 148 + 4 i] is a slot 0..8 is drawn at THAT character's
// skeleton position [[0x836F38D0 + 4 b] + 176] (the 16 bytes written over the track's own row, 0x825BAECC), else at the track's own rig row; the key u64 [mgr + 268 + 8 i] picks the sprite (a partner's track carries the follow
// spot's key: a disk). Nothing here asks "who is entering". `read` false = the record carries no track words (an older record, a failed read, the switch off): Derive then draws the OLD way (the spot on the entering slot's root, every other track a pool).
struct Tracks {
  bool read = false;                // the manager row had kLoTracksRead (layout 3)
  bool phase4 = false;              // [0x836F3940] == 4 at the latch (the kSlPhase row); only then the bindings move a track
  int32_t bind[kLightTracks];       // the manager's binding word as an int (0..8 = a slot)
  bool has_pos[kLightTracks] = {};  // a kSlTrackBind row exists: the character's position was read
  bool by_act[kLightTracks] = {};   // that row says the bound character's act is 25000
  float x[kLightTracks] = {}, z[kLightTracks] = {};
  uint64_t key[kLightTracks] = {};  // the track's sprite key (0 = the default sprite)
  int32_t w[kLightTracks] = {}, h[kLightTracks] = {};   // the size ints [mgr + 188 + 4 i] / [mgr + 228 + 4 i] (texels of the key's sprite; 0 = unresolved)
  Tracks() { for (int32_t& b : bind) b = -1; }
};
inline bool g_plant_pos_first = false, g_plant_size_ignored = false;   // TOOLS ONLY (tnb_unit planted twins): the old check order; the size ints dropped
// What one Derive saw (the census): per frame, summed by the caller.
struct DeriveStats {
  uint32_t keyed_other = 0, nonsquare = 0, bound = 0, by_act = 0, by_code208 = 0, disks = 0, pools = 0, track1 = 0, unbound_spot = 0, disk_size_unknown = 0;
};
// The manager row (rows[0]) + the frame's kSlTrackBind rows + the phase word -> Tracks. False = no track words (read stays false).
inline bool MakeTracks(const LightObjectRow* mgr, const StageLookRow* binds, size_t nbinds, bool phase_known, uint32_t phase, Tracks& tr) {
  tr = Tracks();
  if (!mgr || mgr->kind != kLoManager || mgr->layout < 3 || !(mgr->flags & kLoTracksRead)) return false;
  tr.read = true;
  tr.phase4 = phase_known && phase == 4;
  for (uint32_t i = 0; i < kLightTracks; ++i) {
    tr.bind[i] = int32_t(mgr->words[kLmTrackBind + i]);
    tr.key[i] = (uint64_t(mgr->words[kLmTrackKey + 2 * i]) << 32) | mgr->words[kLmTrackKey + 2 * i + 1];
    tr.w[i] = int32_t(mgr->words[kLmTrackSize + i]), tr.h[i] = int32_t(mgr->words[kLmTrackSize + kLightTracks + i]);
  }
  for (size_t k = 0; k < nbinds; ++k) {
    const StageLookRow& r = binds[k];
    if (r.kind != kSlTrackBind || r.rig_slot < 1 || r.rig_slot > kLightTracks) continue;
    const uint32_t i = uint32_t(r.rig_slot) - 1;
    tr.has_pos[i] = true, tr.by_act[i] = (r.flags & 0x100u) != 0, tr.x[i] = r.dir[0], tr.z[i] = r.dir[1];
  }
  return true;
}

// Rows from the 224 words of the pool object's block. False = refuse (a non-finite or absurd value anywhere in the tracks 7..16: NaN / inf / |tint| > 16 / |x|, |z| > 20000); then `n` is 0. A track whose tint is all zero
// contributes nothing and is not made a row (the entrance's first step has tint 0 everywhere and needs no entrant root; a track the rig never bound reads zero as well). `n` <= kMaxRows.
// With `tr` (tr->read): every track 7..16 (track 1 = t 8 included) is a row; its position is the bound character's when phase 4 and a kSlTrackBind row exists, else the track's own row (the follow spot too: no kSlfAtEntrant); a
// DISK (kSlSpot with an explicit position and size) is a track whose key is non-zero and equals track 0's key, at phase 4 only; every other track is a pool, half = size int x 3000 / 512 when its key resolved at phase 4, else the
// default 375 (TEN_ADV 3b / 3c). `disk_size_unknown` counts a keyed track whose size int is 0. Without `tr` (or !tr->read): the old rule (t = 8 skipped, the spot on the entering slot's root).
inline bool Derive(const uint32_t* words, StageLookRow* out, uint32_t& n, const Tracks* tr = nullptr, DeriveStats* st = nullptr) {
  n = 0;
  const bool tracks = tr && tr->read;
  StageLookRow rows[kMaxRows];
  uint32_t m = 0;
  DeriveStats ds;
  for (uint32_t t = kFirstTrack; t <= kLastTrack; ++t) {
    if (t == 8 && !tracks) continue;
    const uint32_t i = t - kFirstTrack;
    const uint32_t pw = kPosWord0 + 4 * i, tw = kTintWord0 + 4 * i;
    float tint[3], x = 0.f, z = 0.f;
    for (int c = 0; c < 3; ++c) {
      tint[c] = W2F(words[tw + uint32_t(c)]);
      if (!std::isfinite(tint[c]) || std::fabs(tint[c]) > 16.f) return false;
    }
    const bool bound = tracks && tr->phase4 && tr->has_pos[i];
    const bool dark = tint[0] == 0.f && tint[1] == 0.f && tint[2] == 0.f;
    auto position = [&]() -> bool {   // the position is read and checked only for a track the game draws (the game's order: the tint test comes first, 0x825BAE68-0x825BAE88)
      if (bound) x = tr->x[i], z = tr->z[i];
      else if (tracks || t != 7) x = W2F(words[pw]), z = W2F(words[pw + 2]);
      if (bound || tracks || t != 7) return std::isfinite(x) && std::isfinite(z) && std::fabs(x) <= 20000.f && std::fabs(z) <= 20000.f;
      return true;
    };
    if (g_plant_pos_first && !position()) return false;   // planted twin (tools only): the old order, a dark track's bad position refuses the frame
    if (tracks && tr->bind[i] >= 0 && tr->bind[i] < 9) {
      ++ds.bound;
      if (i == 0 && tr->by_act[i]) ++ds.by_act;   // [L] the word does not say which writer bound it: track 0 on a character in act 25000 is sub_8231FCC0's, every other binding is YAT2 code 208's (sub_823F9930)
      else ++ds.by_code208;
    }
    if (dark) continue;   // nothing to draw
    if (!position()) return false;
    StageLookRow r;
    if (!tracks) {
      if (t == 7) r.kind = kSlSpot, r.flags = kSlfAtEntrant;
      else r.kind = kSlPool, r.dir[0] = x, r.dir[1] = z, r.dir[2] = 375.f;
    } else {
      // The game reads keys and sizes ONLY at phase 4 (TEN_ADV 3b); an unresolved key (0, or size 0) draws the default sprite at its natural size (the 64 px pool sprite = 375). A resolved key: half = size int x C / 512.
      const bool keyed = tr->phase4 && tr->key[i] != 0 && tr->w[i] > 0;
      const float half = keyed && !g_plant_size_ignored ? float(tr->w[i]) * kPageC / 512.f : 375.f;
      if (keyed && tr->key[i] != tr->key[0]) ++ds.keyed_other;   // [U] a keyed track whose sprite is not track 0's: drawn as a pool of its size (the texture is unknown)
      if (keyed && tr->w[i] != tr->h[i]) ++ds.nonsquare;         // the Pool has one half: w is used
      if (tr->phase4 && tr->key[i] != 0 && tr->w[i] <= 0) ++ds.disk_size_unknown;
      const bool disk = keyed && tr->key[i] == tr->key[0];   // [U] the disk texture is key-list index 5 of a gm.pac bank (TEN_ADV 3e): equal to track 0's key stands for it
      r.kind = disk ? kSlSpot : kSlPool, r.dir[0] = x, r.dir[1] = z, r.dir[2] = half;
      ++(disk ? ds.disks : ds.pools);
      if (t == 8) ++ds.track1;
      if (i == 0 && !bound) ++ds.unbound_spot;
    }
    r.col[0] = tint[0], r.col[1] = tint[1], r.col[2] = tint[2];
    rows[m++] = r;
  }
  for (uint32_t i = 0; i < m; ++i) out[i] = rows[i];
  n = m;
  if (st) *st = ds;
  return true;
}

// The pool object's block row of a record's (or a latch frame's) light objects: the manager row (rows[0]) names it. nullptr = not there / not read.
inline const uint32_t* PoolBlock(const std::vector<LightObjectRow>& rows) {
  if (rows.empty() || rows[0].kind != kLoManager) return nullptr;
  const uint32_t r = rows[0].words[kLmRefBase + kLightMaxSets + kPoolSlot];
  if (r == kLightNoRow || r >= rows.size() || !(rows[r].flags & kLoBlockRead)) return nullptr;
  return rows[r].words;
}

}  // namespace rdr_scene::stage_pool_words
