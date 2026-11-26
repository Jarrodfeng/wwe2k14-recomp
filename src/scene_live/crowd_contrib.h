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
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "tail_latch.h"  // S102 U1
#include "crowd_snapshot_core.h"  // S103 D3: the completeness rule of required input bit 10
#include "latch_period.h"   // S111 CUTCROWD2: record r carries latch r - 1's snapshot, the latch whose scene camera it carries

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

// S103 D3c: a sign / foam hand prop world (the 12 floats of the op 21 at one of the four sites; crowd_hooks.h kOtherOp21Return order): site 0 sub_82518950 pass 1 (signs on seated people), 1 its pass 2 (free signs),
// 2 sub_82518EB8 pass 1 (foam hands), 3 its pass 2. `index` = the object / entry slot j (the reader only).
// S103 D5: `hidden` = the game's own gate that would NOT draw this prop (PropHide, 0 = drawn); `variant` = the picture-style byte (signs: elt+18 / the free entry's style byte; foam hands: elt+19 / the same);
// `picture` = word 0 of the 8-byte texture key the loop resolves with sub_82294F20 (0 = not read). The walk's NoteProp rows are the DRAWN props only (hidden 0).
enum PropHide : uint8_t { kPhNone = 0, kPhSignGlobal = 1, kPhResource = 2, kPhExited = 3, kPhTexture = 4, kPhDistance = 5 };
// S103 SG: model_key = the mesh the game binds for this prop: a hand = 40999 + h of its type b (crowd_latch_reader.h HandTypeMap, the arena loader replicated); 0 = not known (a SHOWN hand without it is
// unread, never 41016 by default); signs keep 0 (= 40000, their one mesh). bone_sig / holder_t (pass 1 only, census): a hash of the 64 bone bytes the prop rides and the holder's world translation
// (M0 row 3), for the frozen-bone count (CROWD_PROPS_GATES.md 11: the bone worlds may be made in the walk an OURS frame skips).
struct PropObs {
  uint32_t site = 0, index = 0;
  float c[12] = {};
  uint8_t hidden = 0, variant = 0;
  uint32_t picture = 0;
  uint32_t file_key = 0;   // S103 CSF: a sign's file key u32([[0x83767C5C] + 16] + 4 b) (0 = a hand / not read); with it the sign's picture is the file's (sign_file_keys.h)
  uint32_t model_key = 0;
  uint32_t bone_sig = 0;
  float holder_t[3] = {};
};

// S103 D6: a prop picture read from the guest's texture object (guest_texture.h): ok = the blocks are valid; otherwise why (guest_tex::Why) and the prop is not whole.
struct PictureObs {
  uint32_t key = 0, width = 0, height = 0, format = 0;
  uint8_t why = 0;
  // S103 CSF census (docs\agents\s103\CS.md section 4): which site first named the key (0 / 1 sign, 2 / 3 foam hand), the registry's fetch dwords and the decoded shape (also for a refused read; zero when the handle did not resolve)
  uint8_t site = 0, tiled = 0, packed = 0;
  uint32_t fetch[6] = {}, shape_w = 0, shape_h = 0, shape_fmt = 0, pitch_blocks = 0;
  std::vector<uint8_t> bytes;
};

// S103 D5: a single person (the sign / foam-hand holder, sub_8276E7B0): what the game would draw, read at the latch (crowd_latch_reader.h ReadSingles). hidden = SingleHide (0 = drawn by the gates read);
// world_kind 1 stored matrix / 2 manager table / 3 not replicated (sub_82C334A8: c is not valid); c = the 12 floats of the world as the stands' members carry them; palette = nodes x 16 floats after the bone's RotY.
// S103 C1 (R15): kShLod is no longer made by the reader (an index 20..59 holder is drawn as the LOD idx / 20 body of type idx % 20); kept for the presence rule's tests. kShForward = the entity's node count / bone table
// forwards along [x+220] to an object that is NOT a CSkinModel (vtable 0x8206AF90): its getter is another class's slot +8 / +12, not read (unread). kShScreen = the game's own screen test sub_8276E2C0 hides it (not drawn).
enum SingleHide : uint8_t { kShNone = 0, kShFlags = 1, kShNoEntity = 2, kShNoNodes = 3, kShLod = 4, kShForward = 5, kShScreen = 6 };
// S103 C1: why an object of the node's list is `unread` (LatchSnapshot::unread_by; the census line names each). hole = a refused read (soft hole); forward = kShForward; no_nodes = node count <= 0 / too large;
// world3 = the world comes from sub_82C334A8 (not replicated); bind = the pose chain could not be read (ReadSingleSkin); screen = the screen test could not be run (no draw-thread camera known);
// floor_lod = a seat group whose node LOD gives no group (lod >= 3); other = any other class / an unreadable seat group (count cap, null elements, bad type).
enum UnreadWhy : uint32_t { kUrHole = 0, kUrForward, kUrNoNodes, kUrWorld3, kUrBind, kUrScreen, kUrFloorLod, kUrOther, kUrCount };
inline constexpr uint32_t kScreenCamUnknown = 0xFFFFFFFFu;   // LatchSnapshot::screen_cam: the draw thread's camera was not noted
struct SingleObs {
  uint32_t object_slot = 0, slot = 0, index = 0, entity = 0, nodes = 0, bone = 0;
  uint32_t lod = 0;             // S103 C1: the body LOD = index / 20 (the 60 = 3 x 20 entity array) [L]; the body asset is entry 20000 + 40 lod + type
  uint8_t hidden = 0, world_kind = 0;
  float yaw = 0.f;
  float c[12] = {};
  std::vector<float> palette;   // the block after the bone's RotY: LOCAL matrices (the form the game writes into [x+180])
  // S103 D6: the pose the game's vtable +44 makes from them (sub_82BD4F80 -> sub_82BD3038: +92 world update sub_82BD30E8, +96 -> +100 skin sub_82BD35B0), root excluded:
  // chain[k] = local[k] x chain[parent[k]] (parent -1: identity; the root [x+32] is the row's world `c`), skin[k] = invbind[k] x chain[k]. bind_ok = every input was readable;
  // otherwise the single is NOT modelled (counted as unread: the frame is not whole).
  bool bind_ok = false;
  bool screen_unknown = false;  // S103 C1: the screen test could not run (no draw-thread camera): never a row, counted unread `screen`
  uint8_t screen_verdict = 0;   // S103 C1 review fix 2: the reader's SingleScreen verdict, 0 not run, 1 shown, 2 hidden (compared with the game's on walked frames: screen_check)
  uint32_t object = 0;          // S103 C1 review fix 2: the node-list object (sub_8276E7B0's r3 / r27): the key the game's verdict is matched by
  uint32_t type = 0;           // the body type = the entity index [obj+28] % 20 (S103 C1; the 3 x 20 array) [L]
  std::vector<float> skin;      // nodes x 16
};
// S103 D3b: the palette slots (slot = lod * kTypes + type) and what a latch-time reader of the crowd manager's own lists hands the contributor (crowd_latch_reader.h).
using PaletteSet = std::array<std::vector<float>, kTypes * kLods>;
// S103 SG review 3: the foam-hand type map frozen per manager generation (the game's loader sub_8250B1E8 runs once per arena load; the map must not follow the live G / R words afterwards). Key = the manager pointer + the six
// picture-object words at [M + 0x19A94]. Owned by the Contributor, filled by crowd_latch_reader.h HandTypeMap (only from a clean, count-matching read); the entry read and the latch both use it, so it has its own mutex.
struct HandMapCache {
  std::mutex mu;
  bool valid = false;
  uint32_t manager = 0, pics[6] = {};
  uint8_t h[6] = {};
  uint32_t types = 0;
};
struct LatchSnapshot {
  std::vector<MemberObs> members;   // stands members (block draw rule) then cards (card loop rule), the manager's order
  PaletteSet palettes;
  std::vector<PropObs> props;            // S103 D3c: the signs / foam hands (crowd_latch_reader.h ReadProps)
  std::vector<SingleObs> singles;        // S103 D5: the holders' people (ReadSingles); read, not yet drawn
  std::vector<PictureObs> pictures;      // S103 D6: one per distinct key of a SHOWN prop (ReadPictures)
  // S103 SG: out. The foam-hand types the arena loader made (static_SetUpHandSignBoard sub_8250B1E8, replicated by crowd_latch_reader.h HandTypeMap): hand_h[b] = h of type b (mesh 40999 + h,
  // picture 42000 + h - 1); hand_types = how many; hand_types_game = the loader's own count [M + 0x198D0]; hand_map_ok = the walk read AND its count equals the game's (else no hand gets a key).
  uint8_t hand_h[6] = {};
  uint32_t hand_types = 0, hand_types_game = 0;
  bool hand_map_ok = false;
  // S103 SG review 3: hand_idx[b] = the loader's own strip index count of type b ([[M + 28 b + 0x198E0]]; census column, to be compared with the audience strip of key 40999 + h: a mismatch means the type is another mesh).
  // hand_cache (in): the map is computed once per manager generation; null = computed at every read (tools, units).
  uint32_t hand_idx[6] = {};
  HandMapCache* hand_cache = nullptr;
  uint32_t unread = 0;                   // out: floor-class objects that are NOT kind-0 seat groups and would be drawn (sub_8276E7B0's people), plus a floor LOD other than 0: seen, not read
  bool served = false;                   // in: the classifier serve step is done for this frame (CrowdKeepRegistration): a pending [mgr+252] is then not a decline (review item 4)
  bool want_palettes = true;             // in: read the +608 blocks (mirror frames compare them); an OURS install takes the hook's rings and needs neither the blocks nor the write ring (item 5)
  uint32_t node = 0;                     // in: the crowd node object (r3 of sub_82519508; 0 = unknown, its gate [node+556] is then not tested)
  uint32_t read_ring = 0, write_ring = 0;   // out: [manager+544] / [manager+548] (review F2: the walk publishes palettes_[read ring])
  // S103 D8: out. fail_stage (crowd_latch_reader.h HardStage) / fail_addr: where an Unreadable decline came from. soft[part] (SoftPart) = units of an optional part (floor, props, singles, pictures) whose read was refused and became holes
  // (counted in `unread`); soft_addr[part] = the first refused address.
  uint32_t fail_stage = 0, fail_addr = 0;
  uint32_t soft[4] = {}, soft_addr[4] = {};
  // S103 D8: in: the table ring index [0x83762F84] as the crowd entry saw it (0xFFFFFFFF = unknown: the reader reads it itself). The game's sub_825185B0 reads that word inside the entry; on an OURS frame the entry is skipped and the
  // latch reads later, so a flip in between (the update side running ahead) would read the OTHER ring's lists. out: ring_moved = the word at the latch differs from the entry's.
  uint32_t entry_ring = 0xFFFFFFFFu;
  bool ring_moved = false;
  uint8_t scope = 0;   // S103 D9: in: 0 everything; 1 the LATCH half (stands, cards, palettes: what the skin pass writes); 2 the ENTRY half (floor, props, holders, pictures: never written by the skin pass); see crowd_latch_reader.h
  // S103 C1: in: the camera object sub_82BA7748 returned on the draw thread at the crowd entry (crowd_hooks.h EntryCamera; 0 = none: the game's singles screen test then hides every holder; kScreenCamUnknown = not noted:
  // the test cannot run and a holder that would be drawn is unread `screen`). out: unread_by[UnreadWhy] (sums to `unread`); screen_hidden = holders the screen test hid; lod_holders = holders with index >= 20 read.
  uint32_t screen_cam = kScreenCamUnknown;
  uint32_t unread_by[kUrCount] = {};
  uint32_t screen_hidden = 0, lod_holders = 0;
  uint32_t forward_vt = 0;   // S103 SN: the first foreign vtable word a `forward` holder showed (0 = none)
  void Clear() { members.clear(); props.clear(); singles.clear(); pictures.clear(); unread = 0; fail_stage = fail_addr = 0; ring_moved = false; for (auto& s : soft) s = 0; for (auto& s : soft_addr) s = 0; for (auto& p : palettes) p.clear(); for (auto& u : unread_by) u = 0; screen_hidden = lod_holders = forward_vt = 0; hand_types = hand_types_game = 0; hand_map_ok = false; for (auto& v : hand_h) v = 0; for (auto& v : hand_idx) v = 0; }
};

class Contributor final : public live::Contributor {
 public:
  const char* Name() const override { return "crowd"; }
  void Emit(const FrameInfo& f, SceneTables& t, Counters& c) override;
  void Census(std::string& out) override;   // S103 D3: the crowd_snapshot stamp census
  bool Latch(const FrameInfo& f) override;   // S102 U1 (tail_latch.h): the snapshot copy every Emit path makes under mu_, taken while the game threads are parked

  // ---- hook side (any thread; cheap) -------------------------------------------------------------------------------------------
  // The crowd draw entry (sub_82519508) begins: the members of the draw so far are discarded (a tiled frame draws the crowd once per tile; the
  // last complete draw is what is published). `read_ring` = the manager's draw-read ring word ([manager + 544]): the palette set the draw uses.
  void BeginDraw(uint32_t read_ring);
  void NoteMember(const MemberObs& m);
  // A member of a population this contributor does not model (the sign / foam-hand people and the other op-21 sites of sub_82518950 /
  // sub_82518EB8): counted as unknown in the next Emit, not recorded.
  void NoteUnmodelled();
  // S103 D3b (review F1): a seated floor person the observer is switched off for (crowd_hooks.h, g_floor_observe false = the default): on screen, not read. Judged like NoteUnmodelled.
  void NoteUnobserved() { ++building_unobserved_; }
  // S103 D3c: the prop's world as the walk recorded it (a sign / foam hand op 21), the PropObs mirror the latch reader is compared with. Called IN ADDITION to NoteUnmodelled (the snapshot stays unmodelled).
  // S103 C1 review fix 2: the game's own verdict of the singles' screen test sub_8276E2C0 (r3 on return to 0x8276E9A4) for node-list object `object` (r27), on a walked frame's draw thread.
  void NoteScreenVerdict(uint32_t object, bool shown) { building_screen_.push_back({object, uint8_t(shown ? 1 : 2)}); }
  void NoteProp(uint32_t site, const float c[12]) {
    PropObs p;
    p.site = site, p.index = uint32_t(building_props_.size());
    for (int k = 0; k < 12; ++k) p.c[k] = c[k];
    building_props_.push_back(p);
  }
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

  // S102 T11 (switch wwe2k14_rdr_crowd_ids_once; fallback g_crowd_ids_once for tools): the census of the cached path, read by the log and by the offline test.
  struct IdsStats {
    uint64_t frames = 0;         // Emits that took the cached path
    uint64_t declined = 0;       // Emits with the switch on that did not (nothing to write)
    uint64_t id_hits = 0;        // member ids answered from the cache
    uint64_t id_made = 0;        // member ids formatted (first sight of a (kind, type, index))
    uint64_t id_uncached = 0;    // member ids formatted into a stack buffer (an index past kIdCacheMax)
    uint64_t members = 0;
  };
  const IdsStats& IdsCensus() const { return ids_stats_; }
  static constexpr uint32_t kIdCacheMax = 4096;  // member indices past this are formatted every time (no cache)
  // S102 T19 (switch wwe2k14_rdr_crowd_run_markers; fallback g_crowd_run_markers for tools): the census of the marker path (crowd_contrib.cpp EmitRuns), read by the log and by the offline test.
  struct RunStats {
    uint64_t frames = 0;         // Emits that wrote markers
    uint64_t declined_stale = 0; // Emits with the switch on and no new crowd draw (nothing written, as for the plain path)
    uint64_t declined_empty = 0; // markers made for a draw with no recordable member (only the pose objects, if any, are written)
    uint64_t runs = 0, members = 0, singles = 0;   // markers written, members in them, markers that hold one member
  };
  const RunStats& RunCensus() const { return run_stats_; }
#ifdef RDR_CROWD_RUNS_PLANTS
  int run_plant = 0;   // test only (crowd_run_markers_test.cpp): 1 a run is not broken where the person type / LOD (so the palette) changes; 3 `at` is taken after the pose objects. (g_run_plant 2, 4, 6, 7 are in the expander: rdr_scene::g_run_plant.)
#endif
#ifdef RDR_CROWD_IDS_PLANTS
  // Test only (tools\crowd\crowd_ids_test.cpp): a planted cache fault. 0 none; 1 the id cache ignores the kind (block / floor / card share an entry);
  // 2 it ignores the person type; 3 the arena's pac string is kept across an arena change; 4 the asset entry cache is keyed without the LOD.
  int plant = 0;
  // S102 T11b: 5 the pool field is kept per (type, index) without the kind (a block member's field answers a floor member); 6 the kept fields survive another pool (no id check).
#endif

  // ---- S103 D3 (required input bit 10, `crowd_snapshot`; docs\renderer\INPUTS_A3.md): the stamp lives HERE, in the owner. ----
  // The glue gives a sink: `active()` = the scene mode is not Off (read every frame; null = never stamp, the offline tools), `stamp(hook_frame)` = scene_inputs::Capture(kInCrowdSnapshot, hook_frame).
  // Every Emit path takes its inputs through TakeInputs, which judges the snapshot it took (crowd_snapshot_core.h) and stamps only a WHOLE one, for the hook frame of the record (record j carries hook frame j - 1).
  // Mode Off: active() is false, nothing is judged, nothing is counted (one call of a function pointer per frame).
  struct StampSink {
    bool (*active)() = nullptr;
    void (*stamp)(uint64_t hook_frame) = nullptr;
    bool (*blocked)() = nullptr;   // review F5: true = the observers are knocked out (ObserverKnockOut, a test switch): the latch proves nothing, no stamp
  };
  void SetStampSink(const StampSink& s) { sink_ = s; }
  void SetSnapPolicy(const SnapPolicy& p) { policy_ = p; }
  struct SnapStats {
    uint64_t stamped = 0, judged = 0, withheld = 0;   // withheld: whole snapshots whose Capture kCrowdOursWhole = false held back
    uint64_t why[uint32_t(SnapWhy::kCount)] = {};
    uint64_t last_stamp_hook = 0;
  };
  SnapStats SnapCensus() const {
    SnapStats s;
    s.withheld = snap_withheld_.load(std::memory_order_relaxed);
    s.stamped = snap_stamped_.load(std::memory_order_relaxed), s.judged = snap_judged_.load(std::memory_order_relaxed), s.last_stamp_hook = snap_last_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < uint32_t(SnapWhy::kCount); ++i) s.why[i] = snap_why_[i].load(std::memory_order_relaxed);
    return s;
  }
  // ---- S103 D3b: the walk-free source. On an OURS frame the crowd entry is skipped (crowd_hooks.h calls NoteOursSkip), so no op-21 / palette hook runs; the Latch then reads the manager's own lists
  // (crowd_latch_reader.h, the glue's `reader`) and the result is the frame's snapshot (published exactly as a walk's EndDraw would: fresh, palettes and members). Returns 0 = read, else a decline code
  // (crowd_latch_reader.h Why). On a WALKED frame the reader runs too (only when the sink is active, i.e. mode != Off) and its result is COMPARED with the walk's: the census says equal / near / differs.
  using LatchReadFn = uint32_t (*)(LatchSnapshot& out);
  void SetLatchReader(LatchReadFn f) { latch_reader_ = f; }
  void NoteEntryRing(uint32_t ring) { entry_ring_.store(ring, std::memory_order_relaxed); }   // S103 D8: [0x83762F84] as the entry saw it (crowd_hooks.h)
  void NoteEntryCamera(uint32_t cam) { entry_cam_.store(cam, std::memory_order_relaxed); }   // S103 C1: sub_82BA7748's answer on the draw thread at the entry (the singles' screen test camera)
  // S103 D9: the ENTRY read. The in-place parts of the snapshot (the props' bone tables, the floor's seats, the holders) are not double-buffered: the update side runs between the crowd entry and the latch (run 28: the table
  // ring moved on 10593 of 10596 reads), so a read at the latch sees the NEXT update's data (448 shown-prop diffs, 82 floor diffs, all in the entrances). EntryRead runs the same reader on the draw thread at the entry
  // (before the game's own draw, or at the skip), and the latch takes that snapshot; only the +608 palettes are still read at the latch (a ring history keeps the sets). Falls back to the latch read when the entry's declined.
  void EntryRead();
  void NoteCrowdNode(uint32_t node) { node_.store(node, std::memory_order_relaxed); }   // r3 of sub_82519508, every time the entry is reached (walked or skipped)
  void NoteOursSkip(uint32_t node = 0, bool classify_done = false) { if (node) NoteCrowdNode(node); ours_served_.store(classify_done, std::memory_order_relaxed); ours_skip_.store(true, std::memory_order_relaxed); }   // the crowd entry was reached and skipped because the frame is OURS
  struct LatchStats {
    uint64_t installed = 0, declined[8] = {}, compared = 0, absent = 0, absent_unarmed = 0, knocked_out = 0, floor_extra = 0, floor_installed = 0, floor_unread = 0, floor_unobserved = 0;
    CmpCount stands, cards, floor, props;   // props (S103 D3c): the walk's sign / foam worlds must be a subsequence of the reader's
    uint64_t prop_extra = 0, props_installed = 0, singles_read = 0, singles_hidden = 0, singles_world_unread = 0, pictures_unread = 0;   // S103 D5: ReadSingles on OURS installs (S103 D6: pictures_unread = shown props whose picture could not be read)   // floor: the walk's seated people must all be in the reader's set (the reader skips no off-screen one: superset), worlds equal
    uint64_t pal_equal = 0, pal_differs = 0;
    // S103 SG (NotePropCensusLocked): the hand-type map vs the loader's count; shown hands with no mesh key (holes); bone-held props seen with a previous read, and how many of them had a CHANGED bone_sig (the bone world
    // hash), per frame kind: an OURS frame that freezes the bones shows changed ~ 0 where a walked frame shows changed ~ seen.
    uint64_t hand_map_ok = 0, hand_map_bad = 0, hand_key_unread = 0, bone_seen_ours = 0, bone_changed_ours = 0, bone_seen_walked = 0, bone_changed_walked = 0;
    uint32_t hand_types = 0, hand_types_game = 0, hand_idx[6] = {};   // hand_idx: the loader's strip index count per type (census column, review 3)
    // S103 D8: where the Unreadable declines came from (HardStage) and the first refused address of each; the soft holes by part (SoftPart); the mirror frames' holders and rows (the install path counts the OURS ones)
    uint64_t unreadable_stage[8] = {}, soft[4] = {};
    uint32_t unreadable_addr[8] = {}, soft_addr[4] = {};
    uint64_t pal_equal_other = 0, pal_differs_other = 0, pal_equal_newest = 0, pal_differs_newest = 0, ring_moved = 0, ring_moved_stands_diff = 0;   // the palette compare against the other ring / the ring with the newest hook copy; reads whose table ring moved since the entry
    uint64_t entry_reads = 0, entry_used = 0, entry_declined = 0, entry_unused = 0;   // S103 D9
    uint64_t pal_walk_eq = 0, pal_walk_diff = 0, pal_walk_other_eq = 0, pal_walk_other_diff = 0;   // S103 D9: the ring-history set at the walk's read ring (the frame's own set) / at the other ring, against the walk's published palettes
    uint64_t prop_diff_count[4] = {}, prop_diff_world[4] = {}, prop_diff_world_pass2 = 0;   // S103 D9: per site, a shown-set size mismatch / a world mismatch (the first differing frame's cause)
    uint64_t walked_props_kept = 0, walked_pictures_kept = 0, walked_singles_held = 0, in_place_frames = 0;   // S103 SG2: a walked frame's reader props / pictures kept for the record (flagged kCpfInPlace); its holders held back (not emitted)
    uint64_t pic_size_by_site[2] = {}, pic_first_size_key = 0;   // S103 CSF: reads refused `size`, split sign (sites 0 / 1) / hand (2 / 3); the first one's key
    PictureObs pic_first_size;                        // S103 CSF: and its site / fetch / shape (printed as first_size)
    uint64_t pic_why[9] = {}, pic_unread_walked = 0, pic_first_bad_key = 0;   // S103 FHB: the reader's picture reads by guest_tex::Why (0 ok, 1 no registry, 2 no handle, 3 wrapper unreadable, 4 not plain 2D, 5 no header, 6 format, 7 size, 8 memory), walked AND latch-built frames; a walked frame's unread ones; the first bad handle
    uint64_t mirror_props_read = 0, mirror_singles_seen = 0, mirror_singles_bound = 0, mirror_singles_hidden = 0;
    uint64_t unread_by[kUrCount] = {}, screen_hidden = 0, lod_holders = 0;
    uint32_t forward_vt = 0;   // S103 SN: first foreign vtable word
    uint64_t screen_eq = 0, screen_diff = 0, screen_reader_only = 0, screen_game_only = 0;   // S103 C1 review fix 2: the reader's SingleScreen verdict vs the game's (walked frames), by object   // S103 C1: the reader's unread objects by reason (UnreadWhy), holders the screen test hid, holders above LOD 0 read
  };
  LatchStats LatchCensus() const { std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_)); return lstats_; }
#ifdef RDR_CROWD_SNAP_PLANTS
  const PaletteSet& PublishedPalettes() const { return published_palettes_; }
  const std::vector<PropObs>& PublishedProps() const { return published_props_; }   // test only (review F2)
  // Test only (tools\scene\crowd_snapshot_unit.cpp): 1 a stale (repeated) snapshot is stamped; 2 an unreadable member (index unknown) is ignored; 3 the stamp is for the record's frame, not its hook frame;
  // 4 the stamp is made in Mode Off; 5 a member's missing palette is ignored.
  int snap_plant = 0;
#endif

 private:
  std::mutex mu_;
  std::vector<MemberObs> building_;     // the draw thread's
  std::vector<MemberObs> published_;    // the last complete draw
  uint32_t building_other_ = 0, published_other_ = 0;
  uint32_t building_unobserved_ = 0, published_unobserved_ = 0;   // S103 D3b F1
  std::vector<PropObs> building_props_, published_props_;          // S103 D3c
  uint32_t draw_ring_ = 0;
  uint64_t draw_seq_ = 0, emitted_seq_ = 0;
  size_t last_runs_ = 0;                 // S102 T19 F5: the previous marker record's run count (the next one reserves it)
  uint32_t stale_run_ = 0;              // consecutive Emits with no new crowd draw
  std::array<std::vector<float>, kTypes * kLods> palettes_[2];  // [ring][slot = lod * 20 + type]
  std::array<std::vector<float>, kTypes * kLods> published_palettes_;  // the set of the ring the last draw read
  int arena_ = -1;
  bool pose_objects_ = false;

  // ---- S102 T11 (the cached path; crowd_contrib.cpp) ----
  void EmitCached(const FrameInfo& f, SceneTables& t, Counters& c);
  const std::string* CachedId(uint32_t variant, uint32_t block, uint32_t n, PooledField** pooled = nullptr);
  PooledField* PoolSlot(uint32_t variant, uint32_t block, uint32_t n);   // S102 SW1: the entry's kept pool field (made when absent); the same slot CachedId hands out   // S102 T11b: `pooled` = the entry's kept pool field
  // variant: 0 block / 1 floor / 2 card with a known index; 3 / 4 / 5 the same with a per-record ordinal (no readable index)
  std::array<std::array<std::vector<std::string>, kTypes>, 6> id_cache_;
  // S102 T11b (wwe2k14_rdr_scene_strings_once; string_pool.h): the pool field of each cached id, kept beside it (same indices); dropped when the pool is another one.
  std::array<std::array<std::vector<PooledField>, kTypes>, 6> id_pool_;
  uint32_t pooled_for_ = 0;
  std::array<std::array<std::array<std::string, kTypes>, kLods>, 2> entry_cache_;  // [card][lod][type]: the asset entry text
  std::array<std::string, kTypes> pose_id_;                                          // "crowd:pose:TT"
  std::string pac_cache_;
  int pac_arena_ = -2;
  std::vector<MemberObs> scratch_members_;
  std::array<std::vector<float>, kTypes * kLods> scratch_palettes_;
  IdsStats ids_stats_;
  uint64_t ids_window_start_ = 0;
  // ---- S102 U1 (docs\renderer\SWAP_TAIL.md): the inputs of one Emit, shared by the three paths (Emit, EmitCached, EmitRuns) ----
  using Palettes = std::array<std::vector<float>, kTypes * kLods>;
  void SnapLocked(bool& skip, bool& count_stale, std::vector<MemberObs>& members, Palettes& palettes, int& arena, uint32_t& other);   // mu_ held
  // The snapshot for frame f: the latch's (swapped in) or one taken now under mu_. False = stale (nothing to emit; c.stale counted as before).
  bool TakeInputs(const FrameInfo& f, Counters& c, std::vector<MemberObs>& members, Palettes& palettes, int& arena, uint32_t& other);
  struct InputLatch {
    LatchMark mark;
    bool skip = false, count_stale = false;
    std::vector<MemberObs> members;
    Palettes palettes;
    int arena = -1;
    uint32_t other = 0;
    SnapWhy verdict = SnapWhy::Stale;   // S103 D3: judged in the latch, stamped by TakeInputs
    std::vector<PropObs> props;         // S103 D5: the latch-built (OURS) frame's sign / foam-hand rows (empty on a walked frame: the stream draws them)
    std::vector<SingleObs> singles;     // S103 D6: the same for the holders (single people)
    std::vector<PictureObs> pictures;   // S103 D6: and the pictures of the shown props
    bool in_place = false;              // S103 SG2: props / pictures are a WALKED frame's reader set (the stream draws them in place): rows get kCpfInPlace
    SnapWhy judged = SnapWhy::kCount;   // S111 CUTCROWD2: this snapshot's own verdict (SnapLocked's; kCount = not judged), stamped for the record that carries it
    Presence presence;                  // S111 CUTCROWD2: what its latch saw shown (NoteVerdict's holder census)
  };
  // S111 CUTCROWD2 (docs\research\s111\CUTCROWD2.md; FRAME_CONTRACT.md 3; latch_period.h): the crowd is a model-class input (walk g lands in stream g + 1), so record r carries the snapshot of latch r - 1, the
  // latch whose scene camera it carries (inputs_contrib.h Take, cam_period_). Two slots, no history: slots_[held_] = the newest latch's snapshot, kept for the next record; slots_[hand_] = the one this record's
  // TakeInputs takes (its vectors swapped out, then rebuilt by the next latch). period_ pairs them (the record's own read at the first latch or after a gap); Reset makes a new one.
  InputLatch slots_[2];
  uint8_t held_ = 1, hand_ = 0;
  std::optional<::scene_inputs::period::OfPalette<uint8_t>> period_{std::in_place};
  // ---- S103 D5: the sign / foam-hand rows of a latch-built frame (crowd_prop_rows.h; docs\renderer\CROWD_PROPS_GATES.md) ----
  std::vector<PropObs> walked_props_;         // S103 SG2: mu_: the reader's shown / gated props of the last WALKED frame whose read was used (proven equal to the walk's: props(shown) eq 4643 / 0 / 0)
  std::vector<PictureObs> walked_pictures_;   // S103 SG2: and their pictures
  bool walked_valid_ = false;                 // S103 SG2: this latch's walked_* sets are filled (cleared at the start of every LatchFeedLocked)
  bool took_in_place_ = false;                // S103 SG2: TakeInputs' props are a walked frame's (AppendPropRows flags them kCpfInPlace)
  std::vector<PropObs> took_props_;   // TakeInputs' (the emitting thread's), cleared when the frame is a walked one
  void AppendPropRows(SceneTables& t, Counters& c);   // one row per took_props_ entry through the writer API; a refusal is a counted hole (c.unknown)
  std::atomic<uint64_t> prop_rows_{0}, prop_rows_hidden_{0}, prop_rows_refused_{0};
  // ---- S103 D6: the single people (sign / foam-hand holders) of a latch-built frame: one CrowdSingleRow each (crowd_prop_rows.h AppendSingle), body = BodyAsset(arena, type) ----
  std::vector<SingleObs> took_singles_, published_singles_;
  std::vector<PictureObs> took_pictures_, published_pictures_;   // the sign / foam-hand pictures (kScCrowdPictures)
  void AppendPictureRows(SceneTables& t, Counters& c);
  std::atomic<uint64_t> picture_rows_{0}, picture_unread_{0};
  std::atomic<uint64_t> pic_drop_why_[9] = {}, pic_drop_empty_{0}, pic_drop_size_{0}, pic_drop_dims_{0};   // S103 FHB: AppendPictureRows' silent drops, by reason
  void AppendSingleRows(SceneTables& t, Counters& c, int arena);   // a refusal / a missing arena is a counted hole (c.unknown)
  std::atomic<uint64_t> single_rows_{0}, single_rows_refused_{0};
  // ---- S103 D3b ----
  void LatchFeedLocked();   // mu_ held: take the reader's snapshot (OURS frame) or compare it with the walk's
  void NoteUnreadWhyLocked();
  void NotePropCensusLocked(bool ours);   // S103 SG: mu_ held: hand map + frozen-bone census from latch_scratch_.props
  struct PropBonePrev { bool valid = false; uint32_t sig = 0; };
  PropBonePrev prop_bone_prev_[64];       // S103 SG: per bone-held prop (sign j / hand 32 + j), the previous read frame's bone hash
  void CompareScreenLocked();   // S103 C1 review fix 2: mu_ held, walked frame: lstats_ screen_eq / screen_diff / reader_only / game_only   // S103 C1: mu_ held: latch_scratch_'s unread_by / screen_hidden / lod_holders into lstats_
  LatchReadFn latch_reader_ = nullptr;
  std::atomic<bool> ours_skip_{false}, ours_served_{false};
  std::atomic<uint32_t> node_{0}, entry_ring_{0xFFFFFFFFu}, entry_cam_{kScreenCamUnknown};   // (S103 C1: entry_cam_)
  struct ScreenVerdict { uint32_t object; uint8_t verdict; };
  std::vector<ScreenVerdict> building_screen_, published_screen_;   // S103 C1 review fix 2: the draw thread's (BeginDraw..EndDraw), the last complete draw's (mu_)
  uint64_t pal_seq_[2] = {0, 0}, pal_clock_ = 0;   // S103 D8: which ring's hook copy is the newest (SetPalette order)
  bool armed_ = false;              // the previous Latch ran with the sink active (review F5: "no crowd" needs a frame of history)
  bool snap_latch_built_ = false;
  bool latch_ok_ = false;           // this frame's snapshot was proved by the latch (reader read, or the crowd entry was not reached); set by LatchFeedLocked, cleared on every other path   // the published snapshot came from the latch reader (cleared by a walk's EndDraw)
  LatchSnapshot latch_scratch_;
  HandMapCache hand_cache_;   // S103 SG review 3: the foam-hand type map, frozen per manager generation (own mutex)
  LatchSnapshot entry_scratch_, entry_snap_;   // S103 D9: the draw thread's read (entry_scratch_), the handed-over one (entry_snap_, mu_), the latch's palette-only read
  bool entry_valid_ = false;   // mu_
  uint32_t entry_why_ = 0;     // mu_
  PaletteSet entry_pal_;       // S103 D9: palettes_[the entry's read ring] copied at the entry (mu_): the set the draw of THAT frame reads
  uint32_t pal_hist_n_ = 0;    // fills of pal_hist_ since Reset / SetArena (a compare needs both rings filled)
  PaletteSet pal_hist_[2];     // S103 D9: the reader's own +608 reads by the ring they were written into (the same index as palettes_)
  LatchStats lstats_;
  // ---- S103 D3 ----
  bool SnapActive() const;                  // the sink is attached and the scene mode is not Off
  SnapWhy JudgeLocked(bool fresh) const;   // mu_ held: the verdict of the published snapshot (the one SnapLocked just copied)
  SnapWhy snap_verdict_ = SnapWhy::Stale;   // SnapLocked's (mu_ held)
  void NoteVerdict(SnapWhy w, uint64_t record_frame, const Presence* shown = nullptr);   // counts and stamps (S111 CUTCROWD2: shown = what the carried snapshot's latch saw; null = presence_)
  StampSink sink_;
  SnapPolicy policy_;
  std::atomic<uint64_t> snap_withheld_{0}, snap_stamped_{0}, snap_judged_{0}, snap_last_{0}, snap_why_[uint32_t(SnapWhy::kCount)] = {};
  // S103 S2 (hold by presence, crowd_snapshot_core.h Presence): what THIS latch saw shown (mu_ held: set by LatchFeedLocked, read by JudgeLocked) and the census of the rule
  Presence presence_;
  std::atomic<uint64_t> pres_frames_{0}, pres_props_{0}, pres_singles_{0}, pres_pictures_{0}, pres_stamped_{0};
 public:
  Presence LastPresence() { std::lock_guard<std::mutex> lock(mu_); return presence_; }   // S103 S2: test access
  uint64_t PresenceStamped() const { return pres_stamped_.load(std::memory_order_relaxed); }  // S103 S2: test access (stamps the class constant alone would have withheld)
 private:
  // ---- S102 T19 (the marker path; crowd_contrib.cpp) ----
  void EmitRuns(const FrameInfo& f, SceneTables& t, Counters& c);
  RunStats run_stats_;
  uint64_t run_window_start_ = 0;
};

// The switch's fallback where the cvar table is not live (offline tools); the game's table overrides it once it is.
inline std::atomic<bool> g_crowd_ids_once{false};
inline std::atomic<bool> g_crowd_run_markers{false};   // S102 T19: the same for wwe2k14_rdr_crowd_run_markers

Contributor& The();         // the one instance (static lifetime)
// (Registration: crowd_hooks.h RegisterCrowd(), game side only, so this file links without the writer.)

}  // namespace rdr_scene::live::crowd
