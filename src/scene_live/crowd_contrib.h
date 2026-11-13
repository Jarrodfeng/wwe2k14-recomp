// The crowd as a live contributor to the scene record (Session 101, lane 27, class C2; docs\systems\crowd.md,
// docs\renderer\SCENE_RECORD.md "The live writer"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// What it records, per frame (a Contributor named "crowd", registered with rdr_scene::live):
//  - one Object per drawn stands member (class Crowd, id "crowd:block:B:member:M", B = the block draw's person type 0..19, M = the member's
//    index in that type's array), asset = the person body (pac = the arena's, entry 20000 + 40 * LOD group + type), world = the member's
//    matrix as the game gives its draw, layer = the LOD group (0..2);
//  - S101 lane 82: one Object per drawn seated floor person (id "crowd:floor:TT:member:I", TT = the group object's type, I = the member index in that
//    group; class Crowd, layer 0, the type's LOD 0 body, skinned with the type's LOD 0 palette; docs/renderer/CROWD_FAMILY_8276ED60.md);
//  - one Object per drawn far flat card (id "crowd:card:B:M", layer 3, asset entry "card:TT"): our renderer draws bodies instead of the 360's
//    run-time impostor, so a reader maps the card asset to the type's body (C2-1 "Cards"); a card takes its type's LOD 0 palette (the
//    impostor is a picture of the type's posed person);
//  - the skin palettes: the game CPU-skins every person TYPE once per frame (the same palette for all its members), so the record holds one
//    Mat4 range per (type, LOD group) used, shared by all members of that (type, group): SCENE_RECORD's bones_first / bones_count. A palette
//    is the game's own: one skin matrix per skeleton node (the stored body vertex * the matrix = the posed vertex), row vectors, in the
//    body's units (the member matrix carries the x10 scale).
// Space: the GAME's file space (-Y up), both for `world` and the palettes, with the game's camera (SCENE_RECORD "Space and projection": a Live
// record is one convention, game space). The body assets are the arena's file-space meshes; a reader that draws in the viewer's space converts
// once at the reader (MirrorY of world and palettes, S * viewproj for the camera; tools\scene\rsc_draw does, until the front-end's own live path).
//
// Hooks (src\scene_live\crowd_hooks.h, called from src\scene_crowd.h) call BeginDraw / NoteMember / EndDraw on the draw thread and SetPalette
// on the crowd manager's thread (inside its critical section); Emit copies the published snapshot on the writer's thread. No game or SDK
// includes here: tools\scene\crowd_make.cpp drives the same API from the files.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "../renderer_scene/live_scene.h"

namespace rdr_scene::live::crowd {

inline constexpr uint32_t kTypes = 20;      // person types = the blocks the draw walks (sub_82518340 arg 0..19)
inline constexpr uint32_t kLods = 3;        // LOD groups 0..2 (models[g * 20 + n]; the palette blocks are in the same order)
inline constexpr uint32_t kCardLayer = 3;   // Object::layer of a far card
inline constexpr uint32_t kPaletteNodes = 22;  // a palette block holds 22 matrices (stride 1408 = 22 * 64, sub_82509CB8); the rig uses fewer
inline constexpr uint32_t kUnknown = 0xFFFFFFFFu;

// One member as the draw hook saw it. `c` = the 12 floats of the draw's VS constants c76-c78 (op 21, r5): row r is COLUMN r of the member's
// row-vector 4x4 matrix (listing 0x82518504-0x82518550: the matrix built by sub_8250CA78 is transposed with vmrg words before it is recorded).
struct MemberObs {
  uint32_t block = kUnknown;   // person type 0..19
  uint32_t lod = kUnknown;     // LOD group 0..2 (cards: kCardLayer)
  uint32_t index = kUnknown;   // member index in the type's array; kUnknown when the record pointer was outside the expected range
  bool card = false;
  bool floor = false;          // S101 lane 82: a seated floor person (sub_8276EAA0, op 21 at 0x8276ED3C; crowd_floor.h): id "crowd:floor:TT:member:I", the type's LOD 0 body and palette
  float c[12] = {};
};

// The member matrix from the draw's constants: the row-vector 4x4 in the layout of rdr_scene::Mat4 (translation in [12..14]).
inline Mat4 WorldFromConstants(const float c[12]) {
  Mat4 m{};
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) m.m[k * 4 + r] = c[r * 4 + k];
  m.m[15] = 1.f;
  return m;
}

class Contributor final : public live::Contributor {
 public:
  const char* Name() const override { return "crowd"; }
  void Emit(const FrameInfo& f, SceneTables& t, Counters& c) override;

  // ---- hook side (any thread; cheap) -------------------------------------------------------------------------------------------
  // The crowd draw entry (sub_82519508) begins: the members of the draw so far are discarded (a tiled frame draws the crowd once per tile; the
  // last complete draw is what is published). `read_ring` = the manager's draw-read ring word ([manager + 544]): the palette set the draw uses.
  void BeginDraw(uint32_t read_ring);
  void NoteMember(const MemberObs& m);
  // A member of a population this contributor does not model (the sign / foam-hand people and the other op-21 sites of sub_82518950 /
  // sub_82518EB8): counted as unknown in the next Emit, not recorded.
  void NoteUnmodelled();
  // The crowd draw entry returned: the members become the snapshot Emit copies (with the palette set of the ring the draw read).
  void EndDraw();
  // The palette block of (type, lod) after the manager's copy, into the set of the skin WRITE ring ([manager + 548]): `matrices` = nodes * 16
  // host floats (only what the game wrote). The palettes at +608 are single-buffered but the pose is one update ahead of the draw's ring, so two
  // sets are kept and the draw picks the one its read ring names.
  void SetPalette(uint32_t write_ring, uint32_t type, uint32_t lod, const float* matrices, uint32_t nodes);
  // The arena the crowd belongs to ("pac\\bg\\bgNN.pac"): the arena contributor (lane 28) publishes the live arena id and calls this on a change
  // (-1 = unknown: the assets then carry no pac and every object counts one unknown). A change drops the old arena's snapshot.
  void SetArena(int number);
  // Test hook (tools\scene\crowd_make): drop everything.
  void Reset();
  // S101 lane 35 (the crowd's first promotion): on, Emit also writes, for every person type used this frame, one object that is NOT visible (id "crowd:pose:TT",
  // the type's LOD 0 body, kObjSkinned, bones = the type's LOD 0 palette), so a reader that draws every member as its type's LOD 0 body always finds the LOD 0
  // palette, also for a type seen only at LOD 1 / 2. Off by default (the record's object count is the lane 27 contract); the live owner path turns it on.
  void SetPoseObjects(bool on) { pose_objects_ = on; }

 private:
  std::mutex mu_;
  std::vector<MemberObs> building_;     // the draw thread's
  std::vector<MemberObs> published_;    // the last complete draw
  uint32_t building_other_ = 0, published_other_ = 0;
  uint32_t draw_ring_ = 0;
  uint64_t draw_seq_ = 0, emitted_seq_ = 0;
  uint32_t stale_run_ = 0;               // consecutive Emits with no new crowd draw
  std::array<std::vector<float>, kTypes * kLods> palettes_[2];  // [ring][slot = lod * 20 + type]
  std::array<std::vector<float>, kTypes * kLods> published_palettes_;  // the set of the ring the last draw read
  int arena_ = -1;
  bool pose_objects_ = false;
};

Contributor& The();         // the one instance (static lifetime)
// (Registration: crowd_hooks.h RegisterCrowd(), game side only, so this file links without the writer.)

}  // namespace rdr_scene::live::crowd
