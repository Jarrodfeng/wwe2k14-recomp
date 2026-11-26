// The scene inputs as a live contributor to the scene record (S103 lane A; docs\renderer\INPUTS_BOARD.md rows P1-P9, W3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes: the game glue (inputs_glue.h) gives it the guest reader and the registered flag.
//
// What it does: at the frame's LATCH (the game threads parked at the frame barrier, after every update task of the frame, S102 U1 / tail_latch.h) it reads, from guest memory, the values our
// frame needs that the game's logic computes at run time and that the scene walks used to read on the way:
//   * the post settings object S (kScPostParams): the post node's walk (sub_82C1E840) read them inside itself, so a skipped walk read none;
//   * per known wrestler model, MdlAlpha = [model+252] (kScModelRuntime): sub_82C13C88 / sub_82C13A80 / sub_82C13DA8 pass it to sub_82C04DF0 inside the model walk.
// It stamps each (inputs_core.h Capture) with the frame of the latch and writes the rows into the record at Emit. It is read only: nothing is written to the guest, no register changes.
// Off by default and free when off: not registered until the glue registers it (the switch is the scene records'), and a latch with no sink attached returns at its first test.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/rows_circle.h"   // S106 G4-2
#include "../renderer_scene/rows_inputs.h"
#include "../renderer_scene/rows_plane.h"
#include "../renderer_scene/rows_stage_look.h"
#include "../renderer_scene/props_light_rows.h"
#include "../renderer_scene/bone_tag_rows.h"   // S103 W2B: g_spring_hair_rows
#include <algorithm>
#include "circle_points.h"   // S103 A3 (S6)
#include "piece_link.h"      // S103 NRB: the characters' SUBM pieces
#include "../renderer_scene/wrk_join.h"   // S103 WRK: Live()
#include "plane_proxy.h"     // S103 A3 (S8)
#include "../renderer_scene/plane_game_gate.h"   // S103 PLF
#include "light_index_core.h"   // S103 A3 (bit 4)
#include "../renderer_scene/light_fixed_id.h"   // S103 A7: the ring / arena-slot light objects by their fixed set id
#include "ring_rows_core.h"                      // S103 A7: the ring's model table (RingTable) for the class of each ring model
#include "light_latch.h"       // S103 A6 (bits 4 / 7 from memory)
#include "wrestler_light_latch.h"   // S103 A8 (the wrestlers' light object at the latch)
#include "frame_delta.h"       // S103 A3 (F1)
#include "../renderer_scene/stage_pool_words.h"   // S103 L2
#include "inputs_core.h"
#include "latch_period.h"   // S111 CUTSTUTTER: the record's camera from the latch whose palettes it takes
#include "gsf_latch.h"   // S103 FXF
#include "../renderer_scene/cam_gate.h"   // S105 A3
#include "slot22_pieces.h"   // S111 PIECEROUTE: SlotSkel::trans_gates
#include "../renderer_scene/stageless_scene.h"   // S111 STAGELESS2: header bit 6 (the game holds no stage)
#include "draw_camera_core.h"   // S103 WC R5: the draw thread's camera, the fallback for no scene manager

#include "plane_eye_read.h"   // S103 E5
#include "props_light_read.h"   // S103 KS1
#include "../renderer_scene/props_map_switch.h"   // S103 KS1: the read is behind --wwe2k14_rdr_props_map
#include "tail_latch.h"   // LatchMark
#include "drawn_slots.h"   // S111 CAWTPOSE: a departed slot occupant the game still draws stays its slot's body
#include "char_draw_list.h"   // S111 CAT33: the game's category-33 draw list (each table skeleton's draw place)
#include "cat33_places.h"     // S111 PROPORDER: every drawn place of that list (characters and the entrance props' CWeapon nodes)
#include "../renderer_scene/draw_place_rows.h"   // S111 PROPORDER: their kSlDrawPlace rows

namespace rdr_scene::live::inputs {

class InputsContributor final : public Contributor {
 public:
  const char* Name() const override { return "inputs"; }

  // The glue sets these once at start (null in the offline tests, which set their own).
  void SetReader(scene_inputs::ReadFn r) { read_ = r; }
  // The scene phase word (1 = match; S103 L2 / D1: the arena look's entrance rule). Also ends bit 4's kept arena entries when it changes (S103 L33F review F2).
  static constexpr uint32_t kPhaseWord = 0x836F3940u;
  static constexpr uint32_t kClearBlockPtr = 0x83929A68u;   // S103 CLRB (CLR.md 1, sub_82C224A0 0x82C224F0): the object whose +240 / +244 / +248 / +252 are the scene clear's R, G, B, A (0..255)
  static constexpr uint32_t kCardFactorWord = 0x8376383Cu;  // S103 B3 (T3b): the card factor word, 850 in the captures [L value; the word's writer is unfound [U]]
  // The models whose runtime values the latch reads (default: the wrestlers contributor's kept skeletons).
  using ModelsFn = void (*)(std::vector<uint32_t>& out);
  void SetModels(ModelsFn m) { models_ = m; }
  // S103 V1F (F2): the skeletons the record writes visible, used ONLY by the light coverage (bit 4); every other use of the kept set stays on SetModels.
  void SetVisibleModels(ModelsFn m) { visible_models_ = m; }
  // S103 A2 (W1): which skeletons the latch reads and where the palette goes. Both are the wrestlers contributor's (inputs_glue.h): TargetsFn = LatchTargets (alive skeletons of cur_ union last_, their node counts,
  // and how many kept skeletons have no count: those make the stamp refuse), SubmitFn = SubmitLatchConsumer (0 submitted, 1 same as the walk's own copy, 2 differs: the walk's copy is kept, review fix 2).
  // S103 A3 (A1-A4): the arena's object list (the stage object's slot models: stage_fx_contrib.h StageModels) and "the arena input is ready" (stage known, last stage take clean, stage clock origin known).
  using ArenaModelsFn = bool (*)(std::vector<uint32_t>& out);
  void SetArena(ArenaModelsFn m) { arena_models_ = m; }
  // S103 A5: the same list WITH each model's stage slot (parallel arrays), so the census names a hole by its arena slot (the 50001 slot text). Used instead of SetArena's function when set.
  using ArenaSlotsFn = bool (*)(std::vector<uint32_t>& models, std::vector<uint16_t>& slots);
  void SetArenaSlots(ArenaSlotsFn f) { arena_slots_fn_ = f; }
  // S103 A5: the stage slots the record EMITS (arena_contrib.h EmittedSlots). With it (and the slots list) bit 4 asks about those slot models only; a slot model the frame does not emit is not asked. false = unknown: ask about all.
  using ArenaEmittedFn = bool (*)(std::vector<uint16_t>& out);
  void SetArenaEmitted(ArenaEmittedFn f) { arena_emitted_fn_ = f; }
  // S103 A5 (piece 3): the world-space bounding spheres of the emitted slot models (parallel to `slots`; r < 0 = unknown). NOT wired in the game yet (the bounds source is [U], INPUTS_A5.md 7): with it unset every emitted slot is asked.
  using ArenaBoundsFn = bool (*)(const std::vector<uint16_t>& slots, std::vector<lightidx::Sphere>& out);
  void SetArenaBounds(ArenaBoundsFn f) { arena_bounds_fn_ = f; }
  // S103 A7: the slot text's l(N) of a stage slot (arena_contrib.h SlotLightSet; 0xFFFFFFFF = unreadable row / slot >= 399 with no row / no table: the model is not computed). Used only with lightstamp::g_from_ids.
  using SlotLightSetFn = uint32_t (*)(uint16_t slot, bool* is_explicit);   // *is_explicit: the text names the set (false = the loader default 100)
  void SetSlotLightSet(SlotLightSetFn f) { slot_set_fn_ = f; }
  // S103 A6: the ring object X at the given hook frame (the ring colour callback's r3 - 24, noted every frame by lane C3), 0 = not noted lately. Null = no ring prediction (counted).
  using RingFn = uint32_t (*)(uint64_t hook_frame);
  void SetRing(RingFn f) { ring_fn_ = f; }
  // S103 A3 (bit-8 review 4): the circle rows can only be drawn when the disc mesh is known (circles contributor registered AND its tracker read the mesh once)
  using MeshKnownFn = bool (*)();
  void SetMeshKnown(MeshKnownFn f) { mesh_known_ = f; }
  using CensusFn = std::string (*)();   // S103 A3 (F3): the wrestlers' palette counters (latch_same / differs against the walk's copy, then the slot census)
  void SetPaletteCensus(CensusFn f) { palette_census_ = f; }   // (bit 5 is stamped by stage_fx's own take, audit item 1)
  // S103 HGF planted twin (tests only): 2 = the old double read (Take asks the table again after LatchPalettes). (1, the old off-latch fallback re-read, went with that fallback: S111 HEADGUARD.)
  static constexpr uint32_t kHgfNoTargetReuse = 2;
  static constexpr uint32_t kHgfFarLayerIgnored = 4;   // S111 OWNERLESS review twin: the landed rule, the far layer is never read (only the category-33 walk draws the DRTR nodes)
  static constexpr uint32_t kHgfDrtrByList = 8;   // S111 OWNERLESS review twin: no node read (the landed rule took the enable from list 33 only)
  void SetHgfFaultsForTest(uint32_t f) { hgf_faults_ = f; }
  struct PaletteTarget { uint32_t skel = 0, count = 0; };
  using TargetsFn = void (*)(uint64_t hook_frame, std::vector<PaletteTarget>& out, uint32_t& missing);
  using SubmitFn = int (*)(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t hook_frame);
  void SetPalettes(TargetsFn t, SubmitFn s) { targets_ = t, submit_ = s; }
  // S103 L31B: the targets from the CHARACTER TABLE (wrestler_contrib.h LatchTargets(slots)). When set it is used instead of TargetsFn: the latch reads the table itself (ReadSlotSkeletons: character
  // [0x836F38D0 + 4 * slot], skeleton [char + 48]) and passes (skeleton, slot) per occupied slot plus each slot's state (review A: empty / occupied / unreadable). `stale_visible` non-null = also ask
  // WrestlerContributor::StaleVisibleAt for this record (the guard: > 0 in mode Ours = no stamp); null = do not ask. `count` = false: no census counted (Take's second ask of the same latch, review E).
  struct SlotSkel { uint32_t skel = 0; int32_t slot = -1; uint32_t shown_addr = 0; bool game_shown = true; int32_t draw_pos = -1; int8_t drtr_on = -1; bool trans_gates = false; };   // S111 OWNERLESS review: drtr_on = DrtrEnableOf (1 / 0 / -1 unread)   // S111 PIECEROUTE: trans_gates = slot 22 of this character draws its SUBM pieces when its DRTR node is on (slot22_pieces.h Gates); a failed read = false   // S103 DRL: shown_addr = [char + 232] (the game's shown flag)   // S107 CAM: game_shown = the game's own predicate sub_823CBC50 ([char+1208] && [char+224] && [char+232]); an unread word counts as shown (never a guessed hide)   // S111 CAT33: draw_pos = the place of this skeleton's character in the game's category-33 list (char_draw_list.h; -1 = not listed / unread)
  enum SlotState : uint8_t { kSlotEmpty = 0, kSlotOccupied = 1, kSlotUnreadable = 2 };
  // S103 NRBF (review F1): ring_models = the ring's model pointers (class table at G, ring_rows_core.h): the ring family draws them, so they are no wrestler objects (the skirts were the "ring-drawn" exemption).
  struct SlotTable { std::vector<SlotSkel> skels; uint8_t state[9] = {}; std::vector<scene_inputs::CharPiece> pieces; std::vector<uint32_t> ring_models; bool far_layer = false; };   // S103 NRB: pieces = the slot characters' SUBM instances (piece_link.h; the ONE shared type, slot_piece.h)   // S111 OWNERLESS review: far_layer = StampFarLayer
  // S103 NRBF: the node count the scene core recorded for a piece model (0 = none), set by the glue; the node-map proof reads exactly that many words.
  void SetPieceNodeCount(scene_inputs::PieceCountFn f) { piece_count_ = f; }
  void SetPieceHookCount(scene_inputs::PieceCountFn f) { piece_hook_count_ = f; }   // S105 A5a F1
  // S103 PZ F2: the owner body's declared link (descriptor + key) for the build-path proof of a NotDeferred piece, set by the glue (the wrestlers contributor's body link); null = NotDeferred pieces stay holes.
  void SetPieceBodyDesc(scene_inputs::BodyDescFn f) { piece_body_ = f; }
  // S103 PZF2 A1 / A3: the identity check of the body links (links never age). `raw` = the body's declared link whether or not it is verified (known / desc / key), `verify` = hand the latch's verdict to the
  // wrestlers contributor (true back = its pieces were dropped: re-list them), `relist` = give it the pieces re-read from [desc + 12]. All null = no check (every link stays unnamed: fail closed).
  using BodyVerifyFn = bool (*)(uint32_t skel, bool ok);
  using BodyRelistFn = void (*)(uint32_t skel, const std::vector<scene_inputs::DescPiece>& pieces);
  void SetBodyIdentity(scene_inputs::BodyDescFn raw, BodyVerifyFn verify, BodyRelistFn relist) { body_raw_ = raw, body_verify_ = verify, body_relist_ = relist; }
  using SlotTargetsFn = void (*)(uint64_t hook_frame, uint64_t record_frame, const SlotTable& slots, std::vector<PaletteTarget>& out, uint32_t& missing, uint32_t* stale_visible, bool count);
  void SetSlotPalettes(SlotTargetsFn t, SubmitFn s) { slot_targets_ = t, submit_ = s; }
  // S103 NRB / S111 KEPTPIECES: the SUBM pieces of character `ch` (skeleton `skel`, the body drawn in `slot`), appended to out.pieces, each named with `skel` as its owner (CharPiece::owner, set by the reader).
  // An unreadable component is counted and gives no links (the pieces then stay holes: never guessed).
  void ReadPiecesOf(uint32_t ch, uint32_t skel, int32_t slot, SlotTable& out, bool count) {
    static thread_local std::vector<scene_inputs::CharPiece> piece_buf;   // the caller's thread's scratch
    if (scene_inputs::ReadCharacterPieces(read_, ch, piece_buf, piece_count_, piece_body_, skel, 0, rdr_scene::g_spring_hair_rows.load(std::memory_order_relaxed), piece_hook_count_)) {   // S105 A5a F1: + the hook-count fallback   // S103 W2B: -1 nodes = spring-hair rows read here, not Unmapped
      for (scene_inputs::CharPiece& p : piece_buf) p.slot = slot, out.pieces.push_back(std::move(p));   // S103 HGF (XLB 3): moved out of the scratch (it is cleared by the next read), never copied
    } else if (count) {
      slot_pieces_unreadable_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Reads the 9 words of the character table and each occupied character's skeleton. An unreadable table word, or a character whose +48 is null / unreadable, is state UNREADABLE (counted, never guessed;
  // review A: it is never "left").
  void ReadSlotSkeletons(SlotTable& out, bool count = true) {
    out.skels.clear();
    out.pieces.clear();
    out.ring_models.clear();
    if (read_) {   // S103 NRBF: the ring's models (none = no ring in this scene: a menu)
      ringrows::RingTable rt;
      const auto rd = [this](uint32_t a, uint32_t& v) { return scene_inputs::ReadWord(read_, a, v); };
      if (ringrows::ReadRingTable(rd, rt))
        for (uint32_t i = 0; i < ringrows::kClassTableCount; ++i)
          if (rt.ptr[i] >= 0x10000) out.ring_models.push_back(rt.ptr[i]);
    }
    scene_inputs::drawn_slots::Kept occ[scene_inputs::drawn_slots::kSlots]{};   // S111 CAWTPOSE: this latch's table occupants, for drawn_slots::Step below
    for (uint32_t s = 0; s < lightlatch::kCharSlots; ++s) {
      uint32_t ch = 0, skel = 0;
      out.state[s] = kSlotEmpty;
      if (!scene_inputs::ReadWord(read_, lightlatch::kCharSlotTable + 4 * s, ch)) { out.state[s] = kSlotUnreadable; if (count) slot_unreadable_.fetch_add(1, std::memory_order_relaxed); continue; }
      if (ch < 0x10000) continue;   // empty slot
      if (!scene_inputs::ReadWord(read_, ch + lightlatch::kCharSkeletonOff, skel) || skel < 0x10000) { out.state[s] = kSlotUnreadable; if (count) slot_no_skel_.fetch_add(1, std::memory_order_relaxed); continue; }
      out.state[s] = kSlotOccupied;
      if (s < scene_inputs::drawn_slots::kSlots) occ[s] = scene_inputs::drawn_slots::Kept{ch, skel};   // S111 CAWTPOSE
      bool shown = true;   // S107 CAM: sub_823CBC50 = [char+1208] != 0 && [char+224] != 0 && [char+232] != 0 (listing); a read that fails leaves "shown"
      bool trans_gates = false;   // S111 PIECEROUTE (slot22_pieces.h): the class word [char+0] and slot 22's gate words; a read that fails = false (no pieces-only route, the hole as before)
      {
        uint32_t w1208 = 0, w224 = 0, w232 = 0;
        if (scene_inputs::ReadWord(read_, ch + 1208, w1208) && scene_inputs::ReadWord(read_, ch + 224, w224) && scene_inputs::ReadWord(read_, ch + 232, w232)) shown = w1208 != 0 && w224 != 0 && w232 != 0;
        uint32_t vt = 0, w3888 = 0, g224 = 0, g232 = 0;
        trans_gates = scene_inputs::ReadWord(read_, ch, vt) && scene_inputs::ReadWord(read_, ch + 3888, w3888) && scene_inputs::ReadWord(read_, ch + 224, g224) && scene_inputs::ReadWord(read_, ch + 232, g232) &&
                      ::rdr_scene::live::slot22::Gates(vt, w3888, g224, g232);
      }
      out.skels.push_back(SlotSkel{skel, int32_t(s), ch + 232, shown, -1, DrtrEnableOf(ch), trans_gates});   // S111 OWNERLESS review: + the DRTR node's own enable; S111 PIECEROUTE: + slot 22's gate
      VerifyBodyIdentity(skel);   // PZF2 A1: BEFORE the pieces are read (their proof asks for the verified link) and before Collect names the body
      ReadPiecesOf(ch, skel, int32_t(s), out, count);   // S103 NRB: the character's own pieces (SUBM component), each named with its owner (S111 KEPTPIECES)
    }
    {   // S111 CAWTPOSE (drawn_slots.h): an occupant that left the table while the game still draws it (the editor's old character, until its flip sub_822E7CA0) stays its slot's body; a departure the game does not draw = named once hidden
      scene_inputs::drawn_slots::Out dep[scene_inputs::drawn_slots::kSlots];
      const auto rd = [this](uint32_t a, uint32_t& v) { return scene_inputs::ReadWord(read_, a, v); };
      const uint32_t nd = departed_.Step(rd, occ, dep);
      for (uint32_t k = 0; k < nd; ++k) {
        out.skels.push_back(SlotSkel{dep[k].skel, dep[k].slot, dep[k].ch + 232, dep[k].shown});
        // S111 VISLATCH: the latch that names it hidden still builds the record of the walk BEFORE the hide (record r pairs with stream r = walk r - 1, wrestler_contrib.h AskedFor). S111 PERIODALL (+ review
        // R3): that record takes its piece links and its name from the latch before (wrestler_contrib.h RecordLatchL), which read and verified it, so a departure named hidden is read no more.
        if (dep[k].shown) {
          VerifyBodyIdentity(dep[k].skel);
          ReadPiecesOf(dep[k].ch, dep[k].skel, dep[k].slot, out, count);   // S111 KEPTPIECES: the game draws the kept character's pieces with it (its own node, sub_825A3908 +3D8 / +478) until the flip: read and named like an occupant's
        }
      }
    }
  }
  // PZF2 A1 / A3 (piece_link.h DescriptorOfBody): a slot skeleton that has a declared body link is named only when exactly one of the game's 12 persistent descriptors has +4 == the skeleton and its
  // +28 == the link's key. The verdict goes to the wrestlers contributor before Collect; a passing verdict that finds the link's pieces dropped re-reads them from [desc + 12] (the Known aged out, or an
  // earlier verdict failed). No link at the skeleton: nothing to verify (the slot key names it, wrestler_hook.h).
  void VerifyBodyIdentity(uint32_t skel) {
    if (!body_raw_ || !body_verify_ || !read_) return;
    const scene_inputs::BodyDesc link = body_raw_(skel);
    if (!link.known) return;
    const bool ok = scene_inputs::VerifyBodyLinkAgainstDescriptors(read_, skel, link.key);
    if (body_verify_(skel, ok) && ok && body_relist_) {
      static thread_local std::vector<scene_inputs::DescPiece> pieces;   // the latch thread's scratch
      const auto rw = [this](uint32_t a, uint32_t& v) { return scene_inputs::ReadWord(read_, a, v); };
      if (scene_inputs::ReadDescriptorPieces(rw, link.desc, pieces)) body_relist_(skel, pieces);   // unreadable: the pieces stay dropped (holes), tried again next latch
    }
  }
  // S111 CAT33 (char_draw_list.h): each table skeleton's place in the game's category-33 draw list (SlotSkel::draw_pos). An unread list leaves every place -1 (the record keeps the slot order), counted.
  void StampCharDrawOrder(SlotTable& t, bool count) {
    static thread_local std::vector<uint32_t> order;   // the latch thread's scratch
    const auto rd = [this](uint32_t a, uint32_t& v) { return read_ && scene_inputs::ReadWord(read_, a, v); };
    const bool ok = scene_inputs::char_draw_list::ReadSkeletonOrder(rd, order);
    if (!ok) order.clear();
    if (count) (ok ? cat33_read_ : cat33_unread_).fetch_add(1, std::memory_order_relaxed);
    scene_inputs::char_draw_list::StampDrawPos(order, t.skels);
  }
  // S111 OWNERLESS review: the enable word of character `ch`'s DRTR node (char_draw_list.h kDrtrTag): 1 = on, 0 = off, -1 = no node or a word that cannot be read (never "off"). The latch thread only.
  int8_t DrtrEnableOf(uint32_t ch) const {
    namespace cdl = scene_inputs::char_draw_list;
    if (!read_) return -1;
    bool unreadable = false;
    const uint32_t comp = scene_inputs::FindByTag(read_, ch + scene_inputs::kCharCompList, cdl::kDrtrTag, unreadable);
    uint32_t node = 0, en = 0;
    if (unreadable || comp == 0 || !scene_inputs::ReadWord(read_, comp + cdl::kDrtrCompNode, node) || !cdl::Ptr(node) || !scene_inputs::ReadWord(read_, node + cdl::kNodeEnable, en)) return -1;
    if (hgf_faults_ & kHgfDrtrByList) return -1;   // planted twin: the landed evidence (list 33 only), which a re-keyed node never reaches
    return en != 0 ? 1 : 0;
  }
  // S111 OWNERLESS review (docs\research\s111\OWNERLESS.md, Review): a SECOND drawer of the characters' DRTR nodes. The depth-sorted scene draw sub_827FC798 (slot 1 of vtable 0x82034230 via sub_827FCB28; its
  // category-38 node is built and registered enabled at boot: sub_827E1AB8 -> sub_827FBD88 +7C / +90) sorts the slot characters by distance (sub_827FC460) and, from sorted place s on, draws each one's opaque node
  // (+1B8, no char+1208 test) and its DRTR node found by tag (+200..+218, no node+8 test). s = [[eHsC + 24] + 56] (char_draw_list.h kFar*) is set only by a camera / move effect of type 100-110 (sub_825DB0A8 ->
  // sub_82741178: s = type - 100, so 100 clears it) and stays until the next. far_layer = s may be non-zero: a DRTR node read off is then no evidence that its pieces are not drawn (wrestler_contrib.cpp
  // LatchTargets). No scene object, no component, no object or s == 0: the draw skips its character loops (+154 / +164 / +170). An unreadable word = may draw (fail closed). The latch thread only.
  void StampFarLayer(SlotTable& t) {
    namespace cdl = scene_inputs::char_draw_list;
    t.far_layer = true;
    if (hgf_faults_ & kHgfFarLayerIgnored) { t.far_layer = false; return; }
    if (!read_) return;
    uint32_t obj = 0, comp_obj = 0, split = 0;
    if (!scene_inputs::ReadWord(read_, cdl::kFarSceneObj, obj)) return;
    if (obj == 0) { t.far_layer = false; return; }
    bool unreadable = false;
    const uint32_t comp = scene_inputs::FindByTag(read_, obj + cdl::kFarCompList, cdl::kFarLayerTag, unreadable);
    if (unreadable) return;
    if (comp == 0) { t.far_layer = false; return; }
    if (!scene_inputs::ReadWord(read_, comp + cdl::kFarCompObj, comp_obj)) return;
    if (comp_obj == 0) { t.far_layer = false; return; }
    if (!scene_inputs::ReadWord(read_, comp_obj + cdl::kFarSplit, split)) return;
    t.far_layer = split != 0;
  }
  // The targets of this latch: the character table's (slot path) when set, else the old liveness rule. `stale_visible` is 0 on the old path (it has no table to ask).
  void Targets(uint64_t hook, uint64_t record_frame, SlotTable& scratch, std::vector<PaletteTarget>& out, uint32_t& missing, uint32_t* stale_visible = nullptr, bool count = true) {
    if (stale_visible) *stale_visible = 0;
    if (slot_targets_) {
      ReadSlotSkeletons(scratch, count);
      StampCharDrawOrder(scratch, count);   // S111 CAT33
      StampFarLayer(scratch);   // S111 OWNERLESS review
      slot_targets_(hook, record_frame, scratch, out, missing, stale_visible, count);
    } else if (targets_) {
      targets_(hook, out, missing);
    }
  }
  // S108 RDS part A: Take's one list of the models the game names at this latch (S103 L31B's parameter-target rule, moved here unchanged so the runtime rows and the node-light filter share it). list = null:
  // no target function is set (units): the caller falls back to the kept set. (S111 HEADGUARD: Take runs on the latch thread only, so the list is always this latch's.)
  struct MemoList {
    const std::vector<PaletteTarget>* list = nullptr;
    bool done = false;
  };
  void ResolveModelList(uint64_t hook, uint64_t frame, MemoList& out) {
    out = MemoList{};
    out.done = true;
    if (!targets_ && !slot_targets_) return;
    if (slot_targets_ && pal_targets_hook_ == hook && !(hgf_faults_ & kHgfNoTargetReuse)) {
      out.list = &pal_targets_;   // S103 HGF (XLB 3): this latch's own targets (LatchPalettes just built them): no second ReadSlotSkeletons / ReadPieceRows / VerifyBodyIdentity
    } else {
      uint32_t missing = 0;
      SlotTable local;   // (no slot targets: the old liveness rule reads no table; or a latch without this hook's LatchPalettes: its own scratch)
      SlotTable& sc = (slot_targets_ && !(hgf_faults_ & kHgfNoTargetReuse)) ? slot_buf_ : local;
      Targets(hook, frame, sc, param_targets_, missing, nullptr, false);   // review E: the latch's ask already counted; no StaleVisibleAt, no census
      out.list = &param_targets_;
    }
  }
  // S111 FREEHOOK (docs\research\s111\FREEHOOK.md): the game is destroying character `ch` (char_release.h): the departure reader forgets it (drawn_slots.h Departed::Forget), so no later latch reads its
  // block, and the update hook's slot match (g_kept_char) and the light latch (KeptChar) drop it at once. Any thread.
  void ForgetCharacter(uint32_t ch) { departed_.Forget(ch); }
  // S103 A2 (W1): called at the START of the wrestlers' latch (wrestler_contrib.cpp Latch calls the pre-latch pointer), so the palettes it submits are in the items that latch collects. Reads each live skeleton's
  // [skel+200] block (inputs_core.h ReadPalette), submits it as the consumer palette tagged with the hook frame, stamps kInWrestlerPalette only when EVERY kept skeleton was read (an unreadable one, or one with no
  // count, is no stamp). Mode Off = nothing read, nothing stamped (as Latch). One thread (the latch); no allocation once the scratch has grown.
  // S111 LATCHMISS: the latch thread only (the S103 HGF off-latch branch for Emit's missed-latch read is gone with that read).
  void LatchPalettes(uint64_t record_frame) {
    if (!read_ || (!targets_ && !slot_targets_) || !submit_ || scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
    wwe2k14::safe_pages::PassLines pass_lines;   // S108 RDS: this pass's line copy (the game's ReadGuest serves this thread's reads through it until return)
    const uint64_t hook = scene_inputs::HookFrameOfRecord(record_frame);
    uint32_t missing = 0, stale_visible = 0;
    Targets(hook, record_frame, slot_buf_, pal_targets_, missing, &stale_visible);
    pal_targets_hook_ = hook;   // S103 HGF (XLB 3): Take's param targets of this latch are THESE (no second table / piece read)
    bool all = missing == 0;
    if (missing) palette_nocount_.fetch_add(missing, std::memory_order_relaxed);
    if (stale_visible && scene_frame::CurrentMode() == scene_frame::Mode::Ours) {   // S103 L31B: a body the game still holds in its table would be dropped from this record (no fresh palette): the STREAM build walks the frame. A build with no walk (!WWE2K14_STREAM()) has no such fallback: there Collect HOLDS the body with its kept palette for up to kConsumerFresh records instead (S103 AUSF), and this withheld bit only stamps nothing (unstamped frames are drawn by us anyway)
      all = false;
      palette_stale_withheld_.fetch_add(1, std::memory_order_relaxed);
    }
    for (const PaletteTarget& t : pal_targets_) {
      if (scene_inputs::ReadPalette(read_, t.skel, t.count, pal_buf_, pal_raw_)) {
        submit_(t.skel, t.count, pal_buf_.data(), hook);   // S111 HEADGUARD: submitted as it stands (no derived-head veto, so no withheld stamp for a palette the game's draw reads)
        palettes_.fetch_add(1, std::memory_order_relaxed);
      } else {
        all = false;
        palette_unreadable_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (all) scene_inputs::Capture(scene_inputs::kInWrestlerPalette, hook);
  }

  // The latch: take this frame's values while the game threads are parked. The mode is read EVERY frame: with --wwe2k14_rdr_scene = 0 (every player's default) the latch reads nothing and Emit writes nothing
  // (lane A review 1), so the default path pays one relaxed load.
  bool Latch(const FrameInfo& fi) override {
    if (!read_) return false;
    if (scene_frame::CurrentMode() == scene_frame::Mode::Off) {
      latched_.Clear();
      latched_.disabled = true;
    } else {
      Take(fi.guest_frame, latched_);
    }
    mark_.Set(fi.guest_frame);
    return true;
  }
  void Emit(const FrameInfo& fi, SceneTables& t, Counters& c) override {
    if (!mark_.Use(fi.guest_frame, Name())) {   // not latched for this frame
      if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
      ++c.stale;   // S111 HEADGUARD (the LATCHMISS review's list): no read here. Every production Emit follows its own Latch (live_scene.cpp LatchImpl / EmitLatched), so only a unit reaches this line
      return;      // (the old fallback Take ran with the game threads running: a read the game never makes)
    }
    const Frame* f = &latched_;
    if (f->disabled) return;
    t.header.flags = (t.header.flags & ~rdr_scene::kSceneFactMask) | rdr_scene::SceneFactBits(f->scene_fact_read, f->scene_drawn);   // S110 ENTRBLACK: the game draws scene geometry this frame (header bits 1 / 2, cam_gate.h)
    t.header.flags = (t.header.flags & ~rdr_scene::stageless::kSceneNoStageHeld) | rdr_scene::stageless::NoStageHeldBits(f->no_stage_held);   // S111 STAGELESS2: the game holds no stage at this latch (bit 6, stageless_scene.h)
    if (f->have_camera) {   // K1: the render camera, from the engine's slot-0 camera object (kScCamera)
      t.has_camera = true;
      t.camera = f->camera;
    }
    if (f->have_post) {
      t.post_params.push_back(f->post);
      if (!(f->post.flags & kPpRead)) ++c.unknown;   // S not readable: the row says so, the frame's post inputs are missing
    }
    t.model_runtime.insert(t.model_runtime.end(), f->rec.models.begin(), f->rec.models.end());   // S111 CUTREACH: the model-class rows of the latch whose palettes the record takes (Take: model_period_)
    if (f->rec.have_props) t.props_light.push_back(f->rec.props);   // S111 CUTREACH2: latch r - 1's props light (the shadow fit task, group 24, fits it before the walk whose colour calls land in this record)
    else if (f->rec.props_degenerate || f->light_degenerate) { PropsLight d; d.frame = uint32_t(f->rec.props_degenerate ? f->rec.props_degenerate_frame : f->degenerate_frame), d.flags = kPlDegenerate; t.props_light.push_back(d); }   // S103 PMC2: the light is degenerate (the game draws no blobs): a row that says so, never a ShadowPrm (PrmFromRecord needs kPlRead | kPlHaveVp). S111 CUTREACH2: the props read's state of latch r - 1, the circle module's of THIS latch (walk-time)
    //   // S103 KS1: the props light's ShadowPrm from the shadow module's light-1 record (props_light_read.h); behind --wwe2k14_rdr_props_map
    if (f->have_eyes) t.plane_eyes.push_back(f->eyes);   // S103 E5: EyePos per proxy kind + PlanePos, recomputed from the plane module's tables at the latch (plane_eye_read.h)
    t.circle_shadows.insert(t.circle_shadows.end(), f->circles.begin(), f->circles.end());   // S103 A3 (S6): the latch's rows; circle_contrib.h compares and replaces them with the walk's
    t.plane_draws.insert(t.plane_draws.end(), f->plane_draws.begin(), f->plane_draws.end());   // S103 A3 (S8)
    if (f->have_plane) t.plane_frames.push_back(f->plane);   // S103 A2 (S7): the latch's block. The plane contributor emits AFTER us (registration order): where the walk's recorder wrote the same stamp it compares and replaces this block (plane_contrib.h Emit)
    t.param_overrides.insert(t.param_overrides.end(), f->rec.params.begin(), f->rec.params.end());   // S103 A2 (W4 / A4); S111 CUTREACH: latch r - 1's
    t.light_objects.insert(t.light_objects.end(), f->rec.lights.begin(), f->rec.lights.end());   // S103 A2 (W2): the light objects and the selection words; S111 CUTREACH: latch r - 1's
    if (f->ent_ok) t.stage_look.push_back(f->ent);   // S111 PAGEPOOLS: the entrance row, the binds and the page are THIS latch's (the stage page sub_825BB0E0 is walk-time: page_node_clock +1); the light objects above are latch r - 1's
    t.stage_look.insert(t.stage_look.end(), f->tbinds.begin(), f->tbinds.end());   // S103 TNB: the bound tracks' character positions (kSlTrackBind; empty on a match frame / the switch off)
    t.stage_look.push_back(f->page);   // S111 PAGEPOOLS: the kSlPage row (stage_look.h LivePage: the page's gate) and its pools / spot (kSlPool / kSlSpot), derived in Take from THIS latch's light objects
    t.stage_look.insert(t.stage_look.end(), f->page_rows.begin(), f->page_rows.end());
    //   // S103 L2: the entrance row of THIS latch frame (the entering slot); no rows on match frames. The pool / spot VALUES travel as the light object (id 32): stage_look.h derives them
    if (f->phase_readable) { StageLookRow ph; ph.kind = kSlPhase, ph.flags = f->phase; ph.rig_slot = f->participants >= 0 ? uint16_t(f->participants + 1) : uint16_t(0), ph.dir[0] = f->ring_gate >= 0 ? float(f->ring_gate + 1) : 0.f, ph.dir[1] = f->plane_module >= 0 ? float(f->plane_module + 1) : 0.f; t.stage_look.push_back(ph); }   // S103 MBC: the same row carries the participant count + 1 and the ring gate word + 1 (scene_record.h kSlPhase)   // S103 D1: the phase word of THIS latch frame (the read rule's selector / shadow bit)
    if (f->lm_ok) t.stage_look.push_back(f->lm_row);   // S103 B3: the game's card factor word of THIS latch frame; absent = not read, counted at the latch (card_factor unread)
    if (f->clr_ok) t.stage_look.push_back(f->clr_row);   // S103 CLRB: the game's scene clear colour of THIS latch frame; absent = not read (the front end clears black)
    if (f->clock_ok) t.stage_look.push_back(f->page_clock);   // S103 FD: the game's own stage-page clock of THIS latch frame (UpdatePages takes the fade alpha from it)
    t.stage_look.insert(t.stage_look.end(), f->places.begin(), f->places.end());   // S111 PROPORDER: the category-33 places of THIS latch frame (kSlDrawPlace; none = unread)
    c.objects += uint32_t(f->rec.models.size());
    c.unknown += f->rec.models_unreadable;   // S111 CUTREACH review: the unread models of the latch whose rows the record carries
    t.input_stamps.insert(t.input_stamps.end(), f->stamps.begin(), f->stamps.end());   // the snapshot Take made (review 4): never the live counters
  }
  void Census(std::string& out) override {
    out += " " + scene_inputs::CensusLine() + " models_unreadable " + std::to_string((unsigned long long)unreadable_.load(std::memory_order_relaxed)) + " no_camera " + std::to_string((unsigned long long)no_camera_.load(std::memory_order_relaxed));
    out += " " + scene_inputs::handed::CensusLine();   // S110 CUTCROWD: the handed camera's census (handed_camera.h)
    out += " " + cam_period_.CensusLine("camera_period");   // S111 CUTSTUTTER: records that took the camera of the latch before theirs (latch_period.h)
    out += " " + model_period_.CensusLine("model_class_period");   // S111 CUTREACH: records that took the light / model / parameter rows of the latch before theirs
    out += " " + scene_inputs::period::g_post_node_clock.CensusLine("post_node_clock") + " " + scene_inputs::period::g_page_node_clock.CensusLine("page_node_clock");   // S111 CUTREACH review: 0 = model-class, +1 = walk-time (latch_period.h NodeClock)
    out += " camera_source scene_manager (" + std::to_string((unsigned long long)drawcam_used_.load(std::memory_order_relaxed)) + " frames from the draw thread)" + drawcam::Cam().CensusText();   // S103 WC R5
    if (no_camera_.load(std::memory_order_relaxed)) {   // S103 S: camera diagnosis
      out += " [camera reasons mgr:";
      for (int i = 0; i < 8; ++i) if (const uint64_t n = cam_reason_[i].load(std::memory_order_relaxed)) out += " r" + std::to_string(i) + "=" + std::to_string((unsigned long long)n);
      out += " slot0: ok " + std::to_string((unsigned long long)slot0_ok_.load(std::memory_order_relaxed));   // S103 CCF: the real table slot 0 (what the editor's draw thread reads), census only
      for (int i = 0; i < 8; ++i) if (const uint64_t n = slot0_reason_[i].load(std::memory_order_relaxed)) out += " r" + std::to_string(i) + "=" + std::to_string((unsigned long long)n);
      out += " slot1: ok " + std::to_string((unsigned long long)slot1_ok_.load(std::memory_order_relaxed));
      for (int i = 0; i < 8; ++i) if (const uint64_t n = slot1_reason_[i].load(std::memory_order_relaxed)) out += " r" + std::to_string(i) + "=" + std::to_string((unsigned long long)n);
      std::lock_guard<std::mutex> lk(diag_mu_);
      for (const std::string& d : cam_dump_) if (!d.empty()) out += " " + d;
      out += "]";
    }
    out += " palettes " + std::to_string((unsigned long long)palettes_.load(std::memory_order_relaxed)) + " palette_unreadable " + std::to_string((unsigned long long)palette_unreadable_.load(std::memory_order_relaxed)) +
           " palette_nocount " + std::to_string((unsigned long long)palette_nocount_.load(std::memory_order_relaxed)) +
           " palette_stale_withheld " + std::to_string((unsigned long long)palette_stale_withheld_.load(std::memory_order_relaxed)) + " slot_table " + (slot_targets_ ? "on" : "off") +   // S103 L31B
           " slot_unreadable " + std::to_string((unsigned long long)slot_unreadable_.load(std::memory_order_relaxed)) + " slot_no_skeleton " + std::to_string((unsigned long long)slot_no_skel_.load(std::memory_order_relaxed)) + " slot_pieces_unreadable " + std::to_string((unsigned long long)slot_pieces_unreadable_.load(std::memory_order_relaxed)) +
           " cat33_read " + std::to_string((unsigned long long)cat33_read_.load(std::memory_order_relaxed)) + " cat33_unread " + std::to_string((unsigned long long)cat33_unread_.load(std::memory_order_relaxed)) +   // S111 CAT33
           " draw_places_read " + std::to_string((unsigned long long)places_read_.load(std::memory_order_relaxed)) + " draw_places_unread " + std::to_string((unsigned long long)places_unread_.load(std::memory_order_relaxed));   // S111 PROPORDER
    if (palette_census_) out += " [palette " + palette_census_() + "]";
    out += " [plane_proxies rows " + std::to_string((unsigned long long)proxy_rows_.load(std::memory_order_relaxed)) + ", no_copy " + std::to_string((unsigned long long)proxy_missing_.load(std::memory_order_relaxed)) + ", " + proxy_delta_.Str() + ", noted " + std::to_string((unsigned long long)planeproxy::The().Noted()) + ", dropped " + std::to_string((unsigned long long)planeproxy::The().Dropped()) + "]";
    out += " [plane read " + std::to_string((unsigned long long)plane_why_[0].load(std::memory_order_relaxed)) + ", no_module " + std::to_string((unsigned long long)plane_why_[1].load(std::memory_order_relaxed)) + ", disabled " + std::to_string((unsigned long long)plane_why_[2].load(std::memory_order_relaxed)) +
           ", suppressed " + std::to_string((unsigned long long)plane_why_[3].load(std::memory_order_relaxed)) + ", no_camera " + std::to_string((unsigned long long)plane_why_[4].load(std::memory_order_relaxed)) + ", read_fail " + std::to_string((unsigned long long)plane_why_[5].load(std::memory_order_relaxed)) + ", holder_null " + std::to_string((unsigned long long)plane_why_[6].load(std::memory_order_relaxed)) + ", nonfinite_fade " + std::to_string((unsigned long long)plane_why_[7].load(std::memory_order_relaxed)) + ", nonfinite_camera " + std::to_string((unsigned long long)plane_why_[8].load(std::memory_order_relaxed)) + ", nonfinite_fit " + std::to_string((unsigned long long)plane_why_[9].load(std::memory_order_relaxed)) + ", offscreen_fit " + std::to_string((unsigned long long)plane_why_[10].load(std::memory_order_relaxed));
    {   // S103 PLF: why no plane module (SHD.md PLB): the game's gate bytes G+18 / +19 / +24 and its participant count, read on NoModule latches only. Absent = the module existed every latch, or nothing was readable.
      std::lock_guard<std::mutex> lk(diag_mu_);
      if (!plane_struct_text_.empty()) out += " [plane_structure " + plane_struct_text_ + ", changes " + std::to_string((unsigned long long)plane_struct_changes_.load(std::memory_order_relaxed)) + ", unreadable " + std::to_string((unsigned long long)plane_struct_unreadable_.load(std::memory_order_relaxed)) + "]";
      else if (plane_struct_unreadable_.load(std::memory_order_relaxed)) out += " [plane_structure unreadable " + std::to_string((unsigned long long)plane_struct_unreadable_.load(std::memory_order_relaxed)) + "]";
    }
    {   // S103 CE: the first bad block of each non-finite reason (dev log, once per reason)
      std::lock_guard<std::mutex> lk(diag_mu_);
      for (const std::string& d : plane_bad_) if (!d.empty()) out += " " + d;
      if (!off_bursts_.empty()) {   // S103 PB: the first 16 offscreen_fit bursts
        out += " {plane_offscreen bursts";
        for (const auto& r : off_bursts_) out += " " + std::to_string((unsigned long long)r.first) + "-" + std::to_string((unsigned long long)r.second);
        out += "}";
      }
    }
    out += "]";
    out += " [plane_eyes read " + std::to_string((unsigned long long)eye_why_[0].load(std::memory_order_relaxed)) + ", no_module " + std::to_string((unsigned long long)eye_why_[1].load(std::memory_order_relaxed)) + ", unreadable " + std::to_string((unsigned long long)eye_why_[2].load(std::memory_order_relaxed)) + ", none_loaded " + std::to_string((unsigned long long)eye_why_[3].load(std::memory_order_relaxed)) + "]";   // S103 E5
    out += " [props_light read " + std::to_string((unsigned long long)props_why_[0].load(std::memory_order_relaxed)) + ", no_module " + std::to_string((unsigned long long)props_why_[1].load(std::memory_order_relaxed)) + ", unreadable " + std::to_string((unsigned long long)props_why_[2].load(std::memory_order_relaxed)) + ", non_finite " + std::to_string((unsigned long long)props_why_[3].load(std::memory_order_relaxed)) + ", no_range " + std::to_string((unsigned long long)props_why_[4].load(std::memory_order_relaxed)) + "]";   // S103 KS1 / PMC2 (was one `bad`)
    out += " [circle_points chars " + std::to_string((unsigned long long)circle_chars_.load(std::memory_order_relaxed)) + ", points " + std::to_string((unsigned long long)circle_points_.load(std::memory_order_relaxed)) + ", unset " + std::to_string((unsigned long long)circle_unset_.load(std::memory_order_relaxed)) +
           ", gated " + std::to_string((unsigned long long)circle_gated_.load(std::memory_order_relaxed)) + ", no_component " + std::to_string((unsigned long long)circle_nocomp_.load(std::memory_order_relaxed)) + ", unreadable " + std::to_string((unsigned long long)circle_unreadable_.load(std::memory_order_relaxed)) + ", no_height " + std::to_string((unsigned long long)circle_noheight_.load(std::memory_order_relaxed)) + ", no_module " + std::to_string((unsigned long long)circle_nomodule_.load(std::memory_order_relaxed)) + ", no_mesh " + std::to_string((unsigned long long)circle_nomesh_.load(std::memory_order_relaxed)) + "]";
    out += " [circle_module unreadable: module_ptr " + std::to_string((unsigned long long)circle_mod_why_[1].load(std::memory_order_relaxed)) + ", circle_word " + std::to_string((unsigned long long)circle_mod_why_[2].load(std::memory_order_relaxed)) + ", mode_words " + std::to_string((unsigned long long)circle_mod_why_[3].load(std::memory_order_relaxed)) + ", handle_words " + std::to_string((unsigned long long)circle_mod_why_[4].load(std::memory_order_relaxed)) + ", view_proj_read " + std::to_string((unsigned long long)circle_mod_why_[5].load(std::memory_order_relaxed)) + ", param_read " + std::to_string((unsigned long long)circle_mod_why_[6].load(std::memory_order_relaxed)) + "; degenerate (module+2240 non-finite, game draws none) " + std::to_string((unsigned long long)circle_mod_why_[7].load(std::memory_order_relaxed)) + "]";   // S103 PMC2
    out += " [params rows " + std::to_string((unsigned long long)param_last_rows_.load(std::memory_order_relaxed)) + ", unreadable_models " + std::to_string((unsigned long long)param_unreadable_.load(std::memory_order_relaxed)) + ", overflow " + std::to_string((unsigned long long)param_overflow_.load(std::memory_order_relaxed)) + "]";
    out += " [arena_models n " + std::to_string((unsigned long long)arena_rows_.load(std::memory_order_relaxed)) + ", unreadable " + std::to_string((unsigned long long)arena_unreadable_.load(std::memory_order_relaxed)) + ", overflow " + std::to_string((unsigned long long)arena_overflow_.load(std::memory_order_relaxed)) + "]";
    out += " [lights rows " + std::to_string((unsigned long long)light_last_rows_.load(std::memory_order_relaxed)) + ", bytes " + std::to_string((unsigned long long)light_last_rows_.load(std::memory_order_relaxed) * sizeof(LightObjectRow)) +
           " per record, null_objects " + std::to_string((unsigned long long)light_null_.load(std::memory_order_relaxed)) + ", unreadable_blocks " + std::to_string((unsigned long long)light_unreadable_.load(std::memory_order_relaxed)) +
           ", manager_unreadable " + std::to_string((unsigned long long)light_missing_.load(std::memory_order_relaxed)) + ", no_sets " + std::to_string((unsigned long long)light_nosets_.load(std::memory_order_relaxed)) + ", node_models " + std::to_string((unsigned long long)light_models_.load(std::memory_order_relaxed)) + ", node_objects " + std::to_string((unsigned long long)light_node_objects_.load(std::memory_order_relaxed)) + ", node_objects_unreadable " + std::to_string((unsigned long long)light_node_unread_.load(std::memory_order_relaxed)) + ", installs_seen " + std::to_string((unsigned long long)lightidx::The().GetStats().objects) + ", expired " + std::to_string((unsigned long long)light_expired_.load(std::memory_order_relaxed)) + ", models_without_entry " + std::to_string((unsigned long long)light_missing_models_.load(std::memory_order_relaxed)) + ", models_stale " + std::to_string((unsigned long long)light_stale_models_.load(std::memory_order_relaxed)) + ", light_hole_first [" + HoleList() + "], holes_lapsed " + std::to_string((unsigned long long)light_lapsed_.load(std::memory_order_relaxed)) + ", holes_never " + std::to_string((unsigned long long)light_never_.load(std::memory_order_relaxed)) + ", holes_wrestler " + std::to_string((unsigned long long)light_wrestler_holes_.load(std::memory_order_relaxed)) + ", hole_slots [" + HoleSlotList() + "], arena_asked " + std::to_string((unsigned long long)light_arena_asked_.load(std::memory_order_relaxed)) + " of " + std::to_string((unsigned long long)light_arena_all_.load(std::memory_order_relaxed)) + ", arena_offscreen " + std::to_string((unsigned long long)light_arena_offscreen_.load(std::memory_order_relaxed)) + ", drawn_without_install " + std::to_string((unsigned long long)lightidx::The().GetStats().no_install) + ", stamp_flag " + (scene_inputs::g_stamp_light_states ? "yes" : "NO") + ", planned_holds " + std::to_string((unsigned long long)light_planned_holds_.load(std::memory_order_relaxed)) + " (arena " + std::to_string((unsigned long long)light_planned_by_[0].load(std::memory_order_relaxed)) + ", skeleton " + std::to_string((unsigned long long)light_planned_by_[1].load(std::memory_order_relaxed)) + "), null_block_threads " + std::to_string((unsigned long long)lightidx::The().GetStats().null_block) + ", table_clears " + std::to_string((unsigned long long)lightidx::The().GetStats().cleared) + ", consumer " + (scene_inputs::g_light_map_consumer ? "yes" : "NO") + ", " + LightIdCensus() + "]";
    { uint32_t tb = clock_last_t_.load(std::memory_order_relaxed); float lt; std::memcpy(&lt, &tb, 4); char b[260]; std::snprintf(b, sizeof b, " page_clock [found %llu, miss %llu, obj %08X, last_t %.1f, flags %u, groups_ptr %08X, groups %u, group11_tasks %u]", (unsigned long long)clock_found_.load(std::memory_order_relaxed), (unsigned long long)clock_miss_.load(std::memory_order_relaxed), clock_obj_.load(std::memory_order_relaxed), lt, clock_last_flags_.load(std::memory_order_relaxed), clock_gptr_.load(std::memory_order_relaxed), clock_gcount_.load(std::memory_order_relaxed), clock_gtasks_.load(std::memory_order_relaxed)); out += b; }   // S103 FD (F3): did the live read find the object
    out += " [card_factor read " + std::to_string((unsigned long long)lm_fed_.load(std::memory_order_relaxed)) + ", unfed " + std::to_string((unsigned long long)lm_unfed_.load(std::memory_order_relaxed)) + ", word_zero " + std::to_string((unsigned long long)lm_zero_.load(std::memory_order_relaxed)) + "]";   // S103 B3 (B3c: word_zero = reads that found the word 0, the cards draw unscaled)
    out += " [clear_colour read " + std::to_string((unsigned long long)clr_fed_.load(std::memory_order_relaxed)) + ", unfed " + std::to_string((unsigned long long)clr_unfed_.load(std::memory_order_relaxed)) + ", last_rgba " + std::to_string((unsigned long long)clr_last_.load(std::memory_order_relaxed)) + "]";   // S103 CLRB (last_rgba = R<<24 | G<<16 | B<<8 | A of the last read)
    out += " " + lightlatch::The().Census();   // S103 A6: light_index / light_state latch==walk
    out += " " + wlatch::The().Census();   // S103 A8: wrestler_light latch==walk, differ (changed frames only)
    out += " [scene][stage] tracks bound " + std::to_string((unsigned long long)tb_bound_.load(std::memory_order_relaxed)) + " (by act " + std::to_string((unsigned long long)tb_act_.load(std::memory_order_relaxed)) + " / code208 " + std::to_string((unsigned long long)tb_c208_.load(std::memory_order_relaxed)) +
           "), disks " + std::to_string((unsigned long long)tb_disks_.load(std::memory_order_relaxed)) + ", pools " + std::to_string((unsigned long long)tb_pools_.load(std::memory_order_relaxed)) + ", track1 drawn " + std::to_string((unsigned long long)tb_t1_.load(std::memory_order_relaxed)) +
           ", unbound spot " + std::to_string((unsigned long long)tb_unbound_spot_.load(std::memory_order_relaxed)) + ", disk_size_unknown " + std::to_string((unsigned long long)tb_size_unknown_.load(std::memory_order_relaxed)) + ", keyed_other " + std::to_string((unsigned long long)tb_keyed_other_.load(std::memory_order_relaxed)) + ", nonsquare " + std::to_string((unsigned long long)tb_nonsquare_.load(std::memory_order_relaxed)) + " (frames " + std::to_string((unsigned long long)tb_frames_.load(std::memory_order_relaxed)) +
           ", bind_unresolved " + std::to_string((unsigned long long)tb_unresolved_.load(std::memory_order_relaxed)) + ", switch " + (rdr_scene::entrance_tracks::On() ? "on" : "off") + ")";   // S103 TNB (TEN.md 4)
    out += " [stage_pools refused on entrance frames: no_unique_entering_slot " + std::to_string((unsigned long long)pools_why_[1].load(std::memory_order_relaxed)) + ", no_pool_block " + std::to_string((unsigned long long)pools_why_[2].load(std::memory_order_relaxed)) + ", block_refused " + std::to_string((unsigned long long)pools_why_[3].load(std::memory_order_relaxed)) + "]";   // S103 L2
  }

  // Test access: what one frame's take holds.
  struct Frame {
    bool have_post = false, have_camera = false, disabled = false, have_plane = false;
    std::vector<InputStampRow> stamps;   // the stamps as they stood when this frame was taken
    Camera camera;
    scene_inputs::CameraInfo cam_info;
    PostParamsRow post;
    std::vector<ModelRuntimeRow> models;
    std::vector<LightObjectRow> lights;   // S103 A2 (W2)
    std::vector<ParamOverrideRow> params; // S103 A2 (W4 / A4)
    PlaneFrame plane;                     // S103 A2 (S7)
    std::vector<PlaneDraw> plane_draws;   // S103 A3 (S8)
    std::vector<CircleShadow> circles;    // S103 A3 (S6)
    PlaneEyes eyes;                       // S103 E5
    bool have_eyes = false;               // S103 E5
    PropsLight props;                     // S103 KS1
    bool have_props = false;              // S103 KS1
    bool light_degenerate = false;        // S103 PMC2: the circle read found module+2240 non-finite (S111 CUTREACH2: the props read's own NonFinite / NoRange is props_degenerate below)
    uint64_t degenerate_frame = 0;
    bool props_degenerate = false;        // S111 CUTREACH2: the props light read NonFinite / NoRange at THIS latch (carried to the record through ModelClass)
    uint64_t props_degenerate_frame = 0;
    std::vector<StageLookRow> tbinds;     // S103 TNB: kSlTrackBind rows of THIS latch frame (entrance frames only)
    StageLookRow ent;                     // S103 L2: the entrance row (flags + the entering slot + 1) of THIS latch frame; no per-latch vector
    bool ent_ok = false, is_entrance = false, entrance_known = false, phase_readable = false, clock_ok = false;   // S103 PHS: entrance_known = the stage-page object's entrance word [obj+88] was read (this latch, or the last latch-thread one for the fallback take)   // S103 FD: clock_ok = the page-clock row below was read
    StageLookRow lm_row;                  // S103 B3: the card factor row (kSlCardFactor) of THIS latch frame; lm_ok = the word was read
    bool lm_ok = false;
    bool scene_fact_read = false, scene_drawn = false;   // S110 ENTRBLACK: the stage's show word read / the game draws scene geometry at this latch (cam_gate.h ReadSceneFact)
    bool no_stage_held = false;   // S111 STAGELESS2: [0x83763544] was read at this latch and is 0 (stageless_scene.h kSceneNoStageHeld); unreadable = false
    StageLookRow clr_row;                 // S103 CLRB: the scene clear colour row (kSlClearColor) of THIS latch frame; clr_ok = all five reads succeeded
    bool clr_ok = false;
    std::vector<StageLookRow> places;     // S111 PROPORDER: the kSlDrawPlace rows of THIS latch frame (Take fills them every latch; empty = the list unread)
    StageLookRow page_clock;              // S103 FD: the stage-page pass object's own clock (inputs_core.h ReadStagePageClock)
    StageLookRow page;                    // S111 PAGEPOOLS: the kSlPage row of THIS latch (the stage page is walk-time: Emit writes it, never through ModelClass)
    std::vector<StageLookRow> page_rows;  // S111 PAGEPOOLS: the page's kSlPool / kSlSpot rows of THIS latch (stage_pool_words::Derive, the rule stage_look::Pools runs on a record)
  uint32_t phase = 0;                   // S103 D1: the phase word when phase_readable
    int32_t plane_module = -1;                   // S103 RML (F3 / F4): 0 = the plane module singleton is null, 1 = it exists, -1 = unread (read every latch, independent of the plane pass's `why`)
    int32_t participants = -1, ring_gate = -1;   // S103 MBC: read every latch (-1 = unknown): the participant count (slot-loop path only) and [0x836F39A0]
    uint32_t models_unreadable = 0;
    // S111 CUTREACH (docs\research\s111\CUTREACH.md): the MODEL-CLASS rows (FRAME_CONTRACT 3: consumed by the model draw sub_82C04DF0, whose walk of period g lands in stream g + 1), as Emit writes them:
    // the light objects, the model run-time rows and the parameter overrides of the latch whose palettes the record takes (latch r - 1; the latch's own read at the boundary). The fields above stay THIS
    // latch's reads: the stamps and the latch's own logic use them. S111 PAGEPOOLS: the entrance row, the track binds and the page are NOT here: the stage page is walk-time (page_node_clock +1).
    struct ModelClass {
      std::vector<ModelRuntimeRow> models;
      std::vector<LightObjectRow> lights;
      std::vector<ParamOverrideRow> params;
      uint32_t models_unreadable = 0;   // S111 CUTREACH review: the named models whose run-time row that latch could not read (Emit's c.unknown)
      PropsLight props;                   // S111 CUTREACH2 (docs\research\s111\CUTREACH2.md): the props light (light 1 of the shadow module) and its read's degenerate state
      bool have_props = false, props_degenerate = false;
      uint64_t props_degenerate_frame = 0;
    };
    ModelClass rec;
    void Clear() { have_post = have_camera = disabled = have_plane = false, have_eyes = false, have_props = false, light_degenerate = false, props_degenerate = false, stamps.clear(), models.clear(), lights.clear(), tbinds.clear(), params.clear(), circles.clear(), plane_draws.clear(), models_unreadable = 0, participants = ring_gate = plane_module = -1, lm_ok = false, clr_ok = false, scene_fact_read = scene_drawn = no_stage_held = false, ent_ok = is_entrance = entrance_known = phase_readable = clock_ok = false; }
  };
  void Take(uint64_t frame, Frame& f) {   // the latch thread only (S111 HEADGUARD: Emit's off-latch fallback take is gone)
    wwe2k14::safe_pages::PassLines pass_lines;   // S108 RDS: this pass's line copy (the game's ReadGuest serves this thread's reads through it until return)
    f.Clear();
    const uint64_t hook = scene_inputs::HookFrameOfRecord(frame);   // every stamp is in the hook clock (review 7)
    scene_inputs::period::g_post_node_clock.AtLatch(hook), scene_inputs::period::g_page_node_clock.AtLatch(hook);   // S111 CUTREACH review: which clock the post node / the stage page ran on (latch_period.h NodeClock; log only)
    // S108 RDS (docs\research\s108\RDS_plan.md part A): the models the GAME names at this latch (the palette / parameter targets: the character table's skeletons, their SUBM pieces, the hook-alive rest), resolved once per
    // Take and used by every reader below that dereferences a model. The kept set (models_) outlives a released model by up to 120 Emits (a match load freed the old bodies, their blocks were reused: [model+480] read
    // as 0x41700001), so it is never the list of a guest read here; it stays the list of membership-only uses and of the unit fallback (no target function set).
    MemoList ml;
    const auto model_list = [&]() -> const MemoList& {
      if (!ml.done) ResolveModelList(hook, frame, ml);
      return ml;
    };
    f.post = scene_inputs::ReadPostSettings(read_, frame);
    scene_inputs::gsf::Fill(f.post);   // S103 FXF: the gGsf node's classification (review 7: the latch thread changes its state)
    f.have_post = true;
    if (f.post.flags & kPpRead) scene_inputs::Capture(scene_inputs::kInPostSettings, hook);   // an unreadable S is NOT a stamp: the decision stays at InputsMissing
    {   // S103 CAE: the shared read (inputs_core.h ReadSceneCameraOrDraw): the handed block, else the composer's copy (S110 CUTCROWD; S103 S: run 7259, the camera table holds the HUD camera at the latch), else, with no scene manager (the CAW editor,
        // S103 WC R5), the draw thread's camera the game's own draw reads (S103 CCF: Get on the RECORD clock; the kInCamera stamp below stays on the hook clock).
      scene_inputs::HandedCensusLatch(read_);   // S110 CUTCROWD: the handed camera's census (source taken, differ / longest_differ, the reach words)
      // S111 CUTSTUTTER (latch_period.h): this latch's read is the camera of period `frame` (its hand-off already ran); the record takes the one of the latch whose palettes it takes (latch frame - 1).
      LatchCamera now{};
      now.have = scene_inputs::ReadSceneCameraOrDraw(read_, frame, &drawcam::GetForLatch, now.camera, now.info, &now.from_draw);
      const LatchCamera rec = cam_period_.Pick(frame, now);
      f.have_camera = rec.have, f.camera = rec.camera, f.cam_info = rec.info;
      if (rec.from_draw) drawcam_used_.fetch_add(1, std::memory_order_relaxed);
    }
    scene_inputs::ReadSceneFactAtLatch(read_, f.scene_fact_read, f.scene_drawn);   // S110 ENTRBLACK: the game's own "scene geometry this frame" (the record's header fact, Emit)
    {   // S111 STAGELESS2 (stageless_scene.h): the game's stage holder at this latch; 0 = the game holds no stage (Create a Title frees it and builds none). Unreadable: not said.
      uint32_t holder = 0;
      f.no_stage_held = read_ && scene_inputs::ReadWord(read_, rdr_scene::kLoadedStage, holder) && holder == 0;
    }
    if (f.have_camera) scene_inputs::Capture(scene_inputs::kInCamera, hook);   // no clear winner / unreadable: no camera, no stamp
    else {
      no_camera_.fetch_add(1, std::memory_order_relaxed);
      // S103 S (diagnosis, run 7258: no_camera on every frame): the reason histogram, slot 1 probed for the census only (never used), and the
      // first rejected camera's raw values once per reason, so one log settles the slot / layout question without a trace build.
      if (f.cam_info.reason < 8) cam_reason_[f.cam_info.reason].fetch_add(1, std::memory_order_relaxed);
      rdr_scene::Camera raw, c0, c1;
      scene_inputs::CameraInfo i0, i1, is0;
      scene_inputs::ReadSceneCamera(read_, raw, i0, &raw);
      if (scene_inputs::ReadCamera(read_, c0, is0, scene_inputs::kCamTable)) slot0_ok_.fetch_add(1, std::memory_order_relaxed);   // S103 CCF
      else if (is0.reason < 8) slot0_reason_[is0.reason].fetch_add(1, std::memory_order_relaxed);
      if (scene_inputs::ReadCamera(read_, c1, i1, scene_inputs::kCamTable + 4)) slot1_ok_.fetch_add(1, std::memory_order_relaxed);
      else if (i1.reason < 8) slot1_reason_[i1.reason].fetch_add(1, std::memory_order_relaxed);
      const uint32_t r = f.cam_info.reason < 8 ? f.cam_info.reason : 7;
      std::lock_guard<std::mutex> lk(diag_mu_);
      if (cam_dump_[r].empty() && i0.object) {
        char b[700];
        const float* v = raw.view.m;
        std::snprintf(b, sizeof b, "{r%u obj 0x%08X view %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g fov %.3g}",
                      r, i0.object, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], raw.fov_y_deg);
        cam_dump_[r] = b;
      }
    }
    // S103 A3 (review F2): the arena's object list. CurrentStage (the arena-id guard) runs inside StageModels, so a freed stage object is never read; false = no current stage.
    arena_buf_.clear();
    arena_slot_buf_.clear();   // S103 A5
    const bool arena_have = arena_slots_fn_ ? arena_slots_fn_(arena_buf_, arena_slot_buf_) : (arena_models_ && arena_models_(arena_buf_));
    {   // S103 A2 (W2): the light objects. No stamp yet: kInLightStates also needs the per-node (index, mode) table (INPUTS_A2.md), so the bit stays open and the decision keeps walking.
      scene_inputs::LightReadInfo li;
      if (scene_inputs::ReadLightObjects(read_, frame, f.lights, li)) {
        if (!rdr_scene::entrance_tracks::On() && !f.lights.empty()) f.lights[0].flags &= ~kLoTracksRead;   // S103 TNB F1: the switch off writes the old record on EVERY frame (no track words: the consumer takes the old rule)
        if (!lightlatch::The().HasHook(uint32_t(hook))) {   // S103 A6 (bit 4 / bit 7 from memory): once per hook frame (Emit's own Take after a missed latch must not store / count it twice); the install each wrestler / ring model follows, read from the nodes' own data (light_latch.h). Compared with the walk-learned table BEFORE anything is fed to it
          // (census `light_index latch==walk`), and with the walk's LightState by the lights contributor's Emit (`light_state latch==walk`). Fed to the table only behind lightlatch::kLatchEntries (off).
          static_assert(rdr_scene::wlight::kSlots == scene_inputs::drawn_slots::kSlots, "one slot count");
          uint32_t kept_chars[rdr_scene::wlight::kSlots] = {};   // S111 DARKDIP: the departed characters the game still draws (this latch's Departed::Step: the wrestlers' latch ran it first, LatchPalettes)
          for (uint32_t s = 0; s < rdr_scene::wlight::kSlots; ++s) kept_chars[s] = departed_.KeptChar(s);
          wlatch::The().OnLatch(read_, f.lights, uint32_t(hook), kept_chars);   // S103 A8: each character's light block at this hook (embedded object char + 3984, or the manager's override object); read-only. S111 DARKDIP: + the kept bodies' own blocks
          std::vector<lightlatch::Pred> preds;
          preds.reserve(21);
          lightlatch::Stats ps, cmp;
          lightlatch::PredictWrestlers(read_, f.lights, preds, ps);
          if (models_) {   // class present, no prediction: a kept wrestler skeleton the slot table did not give (the referee when it is not in the table) is counted, never silent
            models_(addrs_);
            lightlatch::The().NoteNoPred(preds, addrs_);
            nopred_buf_.clear(), nopred_hook_ = hook;   // S103 B4X F4: the kept skeletons with NO character slot (the referee): bit 4 still asks about them while A8 synthesis is on, they have no synthesised light
            for (uint32_t m : addrs_) {
              bool have = false;
              for (const lightlatch::Pred& p : preds) have = have || p.model == m;
              if (!have) nopred_buf_.push_back(m);
            }
          }
          lightlatch::PredictRing(read_, f.lights, ring_fn_ ? ring_fn_(hook) : 0u, preds, ps);
          lightlatch::The().Mark(preds);   // Pred::changed against the previous latch (before the compare, which counts block / state on changed predictions only)
          lightlatch::CompareIndex(
              preds, uint32_t(hook), [](uint32_t m, lightidx::NodeLight& e) { return lightidx::The().Find(m, e); }, [this](uint32_t a, uint32_t* w) { return scene_inputs::ReadLightBlockAt(read_, a, w); }, cmp);
          lightlatch::The().AddIndexCompare(cmp);
          if (lightlatch::kLatchEntries) {
            uint64_t fed = 0;
            for (const lightlatch::Pred& p : preds)
              if (p.have_block) lightidx::The().OnLatchEntry(p.model, p.source, p.index, p.mode, uint32_t(hook)), ++fed;
            lightlatch::The().NoteLatchEntries(fed);
          }
          lightlatch::The().Put(uint32_t(hook), std::move(preds), ps);
        }
        // S103 A3 (bit 4): the per-node install table learned on walked frames, as kLoModelMap rows after the object rows (74 triples a row). Bit 4 is stamped when the objects were read AND the table holds an entry
        // confirmed within kMaxAge hook frames (an empty table = nothing known about any model's light = no stamp; the game walks and relearns).
        std::vector<lightidx::NodeLight> nodes;
        bool nodes_ok = true;
        uint32_t expired = 0;
        lightidx::The().Snapshot(uint32_t(hook), nodes, &expired);   // S103 B4X: learned entries only live kMaxAge; the ring / arena-slot models are computed below
        light_expired_.fetch_add(expired, std::memory_order_relaxed);
        if (lightstamp::g_from_ids) ApplyLightIds(f.lights, hook, nodes);   // S103 A7: the ring / arena-slot models by their fixed set id, from the rows just read (li above), merged over the learned entries
        for (size_t i = 0; i < nodes.size(); i += kLightModelsPerRow) {
          LightObjectRow r;
          r.frame = frame, r.kind = kLoModelMap, r.flags = kLoBlockRead;
          r.aux0 = uint32_t(std::min<size_t>(kLightModelsPerRow, nodes.size() - i));
          for (uint32_t k = 0; k < r.aux0; ++k) {
            const lightidx::NodeLight& n = nodes[i + k];
            SetModelMapEntry(r.words, k, ModelMapEntry{n.model, n.object, n.index, n.mode});   // the shared accessor (scene_record.h)
          }
          f.lights.push_back(r);
        }
        {   // blocks the map names that the manager's lists do not carry (the wrestlers' EMBEDDED objects, flash / crowd-group items): read here as kLoNodeObject rows. Only entries of models the latch knows
          // THIS frame (the kept wrestler skeletons and the arena's models: arena_buf_ is filled above, this frame), tested against EVERY manager object address (li.objects, review must 5/6).
          std::vector<uint32_t> known;
          if (const MemoList& l = model_list(); l.list) {   // S108 RDS (plan (a)): the models the game names at this latch, never the kept set (a released model's learned object would be read for up to kMaxAge)
            for (const PaletteTarget& t : *l.list) known.push_back(t.skel);
          } else if (models_) {
            models_(addrs_);
            known = addrs_;
          }
          known.insert(known.end(), arena_buf_.begin(), arena_buf_.end());
          std::sort(known.begin(), known.end());
          std::vector<uint32_t> srcs;
          for (const lightidx::NodeLight& n : nodes)
            if (std::binary_search(known.begin(), known.end(), n.model)) srcs.push_back(n.object);
          std::sort(srcs.begin(), srcs.end());
          srcs.erase(std::unique(srcs.begin(), srcs.end()), srcs.end());
          uint32_t extra = 0, unread = 0;
          for (uint32_t o : srcs) {
            const bool in_manager = std::find(li.objects.begin(), li.objects.end(), o - 48u) != li.objects.end();   // an object install copies obj + 48
            if (in_manager) continue;
            if (extra >= kNodeObjectMax) { ++unread; continue; }   // over the bound: counted, and the stamp is withheld below
            LightObjectRow r;
            r.frame = frame, r.kind = kLoNodeObject, r.object = o;   // `object` = the SOURCE block address (obj + 48 for an object install)
            if (scene_inputs::ReadLightBlockAt(read_, o, r.words)) {
              r.flags = kLoBlockRead;
              f.lights.push_back(r);
              ++extra;
            } else {
              ++unread;
            }
          }
          light_node_objects_.store(extra, std::memory_order_relaxed);
          light_node_unread_.store(unread, std::memory_order_relaxed);
          nodes_ok = unread == 0;
        }
        light_models_.store(nodes.size(), std::memory_order_relaxed);
        // F2 (2): stamp only when EVERY model the latch knows (the kept wrestler skeletons and the arena's models) has an entry, never "the table is not empty"; F2 (1): and only once a consumer reads the map
        // S103 A4: lightidx::Coverage over every drawn model (the kept wrestler skeletons + the arena's models) at THIS record's hook frame: a missing entry is a hole, a stale one is refused; the first holes are named in the census
        lightidx::Cover cov;
        std::vector<uint32_t> ever;   // S103 A5: classifies the holes (lapsed / never); fills nothing
        lightidx::The().Ever(ever);
        uint32_t wrestler_holes = 0;
        std::vector<uint32_t> asked_models;   // S103 B4X F5: every model asked this frame (sorted below): the refresh plan looks at these only
        // S103 B4X (Q1 bit4.wrestler_coverage): with A8's wrestler light synthesis ON the character wrestlers are not asked here (their light comes from the A8 latch, bit 7 requires it for every actor; asking walk-learned
        // entries of them forced a refresh walk every kMaxAge frames when an off-screen wrestler lapsed). A kept skeleton with NO character slot (the referee) has no synthesised light: it is still asked (F4), so an OURS frame never
        // draws one without a light. Synthesis off: every kept skeleton is asked, as before.
        if (ModelsFn lm = visible_models_ ? visible_models_ : models_) {   // S103 V1F: the skeletons Emit draws as visible (falls back to the kept set)
          lm(addrs_);
          if (wlatch::g_on && nopred_hook_ == hook) {   // S103 merge V1F + B4X2: the no-prediction skeletons that are also drawn visible
            std::vector<uint32_t> keep;
            for (uint32_t x : nopred_buf_) if (std::find(addrs_.begin(), addrs_.end(), x) != addrs_.end()) keep.push_back(x);
            addrs_.swap(keep);
          }   // F4 (the latch thread owns both buffers); no no-prediction set for THIS hook (the latch ran elsewhere): ask every kept skeleton, never drop one silently
          asked_models.insert(asked_models.end(), addrs_.begin(), addrs_.end());
          const lightidx::Cover cw = lightidx::Coverage(nodes, addrs_, uint32_t(hook), &ever);
          wrestler_holes = cw.missing + cw.stale;
          cov.Add(cw);
        }
        // S103 A5: ask only about the slot models the record emits (a model the frame does not emit is not asked). A model emitted but never entered stays a hole (no default light set, INPUTS_A5.md).
        // S103 L33F: NOT built here: the entrance NEVER-holes (run 32: slots 301 / 303 / 304 and 1-4 wrestler holes, never drawn through the model hook since the stage) need the visibility filter (INPUTS_A5.md 4a: the 50020 tree's flags, or bounds for SetArenaBounds); until then an entrance frame keeps walking on them.
        std::vector<uint32_t> asked_arena;
        const std::vector<uint32_t>* arena_ask = &arena_buf_;
        arena_all_ = arena_buf_.size();
        if (arena_emitted_fn_ && arena_slot_buf_.size() == arena_buf_.size()) {
          std::vector<uint16_t> em;
          if (arena_emitted_fn_(em)) {
            asked_arena = lightidx::FilterEmitted(arena_buf_, arena_slot_buf_, em);
            arena_ask = &asked_arena;
          }
        }
        std::vector<uint32_t> visible_arena;
        uint32_t offscreen = 0;
        if (arena_bounds_fn_ && f.have_camera && arena_slot_buf_.size() == arena_buf_.size()) {   // S103 A5: our frame can only see what is inside the camera frustum
          std::vector<uint32_t> ask_models;   // the asked models in slot order, with their slots, so the bounds are parallel
          std::vector<uint16_t> ask_slots;
          for (size_t i = 0; i < arena_buf_.size(); ++i)
            if (std::find(arena_ask->begin(), arena_ask->end(), arena_buf_[i]) != arena_ask->end()) ask_models.push_back(arena_buf_[i]), ask_slots.push_back(arena_slot_buf_[i]);
          std::vector<lightidx::Sphere> sph;
          if (arena_bounds_fn_(ask_slots, sph) && sph.size() == ask_models.size()) {
            visible_arena = lightidx::FilterVisible(ask_models, sph, lightidx::FrustumFromViewProj(f.camera.view.m, f.camera.proj.m), &offscreen);
            arena_ask = &visible_arena;
          }
        }
        light_arena_offscreen_.store(offscreen, std::memory_order_relaxed);
        light_arena_asked_.store(arena_ask->size(), std::memory_order_relaxed);
        light_arena_all_.store(arena_all_, std::memory_order_relaxed);
        cov.Add(lightidx::Coverage(nodes, *arena_ask, uint32_t(hook), &ever));
        asked_models.insert(asked_models.end(), arena_ask->begin(), arena_ask->end());
        std::sort(asked_models.begin(), asked_models.end());
        {   // S103 A5: the first arena holes by STAGE SLOT (arena_slot_buf_ is parallel to arena_buf_ when SetArenaSlots is set): the next mirror run names them against the 50001 slot text
          uint32_t k = 0;
          for (size_t i = 0; i < arena_buf_.size() && i < arena_slot_buf_.size() && k < 8; ++i) {
            const uint32_t m = arena_buf_[i];
            if (std::find(arena_ask->begin(), arena_ask->end(), m) == arena_ask->end()) continue;   // S103 A5: not emitted, not a hole
            auto it = std::lower_bound(nodes.begin(), nodes.end(), m, [](const lightidx::NodeLight& x, uint32_t v) { return x.model < v; });
            if (it == nodes.end() || it->model != m) light_hole_slot_[k++].store(uint32_t(arena_slot_buf_[i]) + 1u, std::memory_order_relaxed);
          }
          for (; k < 8; ++k) light_hole_slot_[k].store(0, std::memory_order_relaxed);
        }
        light_lapsed_.store(cov.lapsed, std::memory_order_relaxed);
        light_never_.store(cov.never, std::memory_order_relaxed);
        light_wrestler_holes_.store(wrestler_holes, std::memory_order_relaxed);
        const uint32_t missing_models = cov.missing + cov.stale;
        light_missing_models_.store(cov.missing, std::memory_order_relaxed);
        light_stale_models_.store(cov.stale, std::memory_order_relaxed);
        for (uint32_t i = 0; i < 8; ++i) light_hole_first_[i].store(i < cov.nfirst ? cov.first_missing[i] : 0, std::memory_order_relaxed);
        if (scene_inputs::g_stamp_light_states && scene_inputs::g_light_map_consumer && !nodes.empty() && missing_models == 0 && nodes_ok) {
          scene_inputs::Capture(scene_inputs::kInLightStates, hook);
          // F2 (3): plan the refresh. The oldest entry expires after kMaxAge hook frames without a walked confirmation; one frame before that, hold the pass (GameWalks for two frames) so the refresh walk happens
          // on purpose instead of the input silently dropping out
          uint64_t oldest = ~0ull;   // S103 B4X F5: the soonest expiry over the ASKED, LEARNED entries only (a computed entry is recomputed at every latch and never ages; an entry nobody asked about does not gate the stamp)
          uint32_t oldest_model = 0;
          for (const lightidx::NodeLight& n : nodes) {
            if (!std::binary_search(asked_models.begin(), asked_models.end(), n.model) || std::binary_search(computed_models_.begin(), computed_models_.end(), n.model)) continue;
            const uint64_t exp = uint64_t(n.frame) + lightidx::Lifetime(n);
            if (exp < oldest) oldest = exp, oldest_model = n.model;
          }
          if (oldest != ~0ull && hook + 2 >= oldest) {
            rdr_own::PassState& ps = rdr_own::S(rdr_own::Pass::Scene3D);
            const uint64_t want = hook + 3;
            uint64_t cur = ps.hold_until.load(std::memory_order_relaxed);
            while (cur < want && !ps.hold_until.compare_exchange_weak(cur, want, std::memory_order_acq_rel)) {}
            light_planned_holds_.fetch_add(1, std::memory_order_relaxed);
            // S103 LH: WHO forced it, so the next run names the class that still needs a walk (arena = a slot model whose learned entry differs from the computed one or has no id; skeleton = a kept skeleton with no character slot)
            const bool is_arena = std::find(arena_ask->begin(), arena_ask->end(), oldest_model) != arena_ask->end();
            light_planned_by_[is_arena ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
          }
        }
        light_last_rows_.store(f.lights.size(), std::memory_order_relaxed);
        light_null_.fetch_add(li.null_objects, std::memory_order_relaxed);
        light_unreadable_.fetch_add(li.unreadable, std::memory_order_relaxed);
      } else {
        f.lights.clear();
        (li.why == scene_inputs::LightWhy::NoSets ? light_nosets_ : light_missing_).fetch_add(1, std::memory_order_relaxed);   // n == 0 is "no sets yet", not "manager unreadable" (review 6)
      }
    }
    {   // S103 A3 (S6): the circle blob shadows, complete (circle_points.h): body points + ShadowParam + ViewProj, read from the update-side state. The rows go into the record; the walk's tracker (circle_contrib.h, emitting
      // after us) compares and replaces them where it recorded the same (frame, slot). Bit 8 is stamped when the slot table and the module part read and every drawn character got its ground height and its points.
      scene_inputs::CircleReadInfo ci;
      uint32_t no_height = 0;
      bool module_ok = false;
      if (scene_inputs::ReadCircleShadows(read_, frame, f.circles, ci, no_height, module_ok)   /* F1: tagged with the RECORD frame, like the walk's op-21 recorder rows (draw-half clock) */) {
        circle_chars_.store(ci.chars, std::memory_order_relaxed), circle_points_.store(ci.points, std::memory_order_relaxed), circle_unset_.store(ci.unset, std::memory_order_relaxed);
        circle_gated_.fetch_add(ci.gated, std::memory_order_relaxed), circle_nocomp_.fetch_add(ci.no_component, std::memory_order_relaxed), circle_unreadable_.fetch_add(ci.unreadable, std::memory_order_relaxed);
        circle_noheight_.fetch_add(no_height, std::memory_order_relaxed);
        if (!module_ok) circle_nomodule_.fetch_add(1, std::memory_order_relaxed);
        circle_mod_why_[uint32_t(ci.module_why)].fetch_add(1, std::memory_order_relaxed);   // S103 PMC2: which ReadCircleModule branch answered
        if (ci.light_degenerate) f.light_degenerate = true, f.degenerate_frame = frame;
        const bool mesh_ok = f.circles.empty() || (mesh_known_ && mesh_known_());   // rows without a mesh the renderer can draw them with are not a valid input
        if (!mesh_ok) circle_nomesh_.fetch_add(1, std::memory_order_relaxed);
        if (module_ok && no_height == 0 && ci.unreadable == 0 && mesh_ok) scene_inputs::Capture(scene_inputs::kInCirclePoints, hook);
      } else {
        f.circles.clear();
        circle_unreadable_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    {   // S103 L2: the entrance page's pools + spot (the rig object of pool slot 2). Read every latch (the values move every frame); a failed read leaves no rows (and below, no stamp on an entrance frame)
      // The pool / spot VALUES are not read here: they are the light object's block (id 32, read just above into f.lights), the game's CURRENT values (stage_pool_words.h). Only the entering slot is read (entrance-step states).
      uint32_t phase = 0;
      f.phase_readable = read_ && scene_inputs::ReadWord(read_, kPhaseWord, phase);
      {   // S103 FD (F1): page_obj_ / page_misses_ belong to the latch thread (Take runs only there); an object mid-construction (vtable stored before +108) is read only there, between frames
        task_walk::GroupInfo gi;
        f.clock_ok = scene_inputs::ReadStagePageClock(read_, page_obj_, page_misses_, f.page_clock, &gi);
        if (gi.base) clock_gptr_.store(gi.base, std::memory_order_relaxed), clock_gcount_.store(gi.count, std::memory_order_relaxed), clock_gtasks_.store(gi.tasks, std::memory_order_relaxed);   // S103 FDP: what the last rescan saw of the group array
        if (f.clock_ok) { clock_found_.fetch_add(1, std::memory_order_relaxed); uint32_t tb; std::memcpy(&tb, &f.page_clock.dir[0], 4); clock_last_t_.store(tb, std::memory_order_relaxed), clock_last_flags_.store(f.page_clock.flags, std::memory_order_relaxed), clock_obj_.store(page_obj_, std::memory_order_relaxed); }
        else clock_miss_.fetch_add(1, std::memory_order_relaxed);
      }
      {   // S103 PHS (docs\research\s103\PHS.md 3 / 4): an ENTRANCE (the pools are required) is exactly when the game runs the pool draw sub_825BACE0: the stage-page pass object's entrance word [obj+88] != 0
          // (ReadStagePageClock flags bit 1). NOT the scene phase word: 4 is the game's generic "not the match" word, written by the editor (seq 76, 0x83397CA0) and the menu sequences as well; the page object's
          // word stays 0 there. An unreadable page object = unknown = no stamp (the decision walks), as an unreadable word was. The phase word still drives every LOOK test (== 1 match look, the game's own rule).
          const int ps = scene_inputs::PageEntranceState(f.clock_ok, f.page_clock.flags, f.phase_readable, phase);
        f.entrance_known = ps != 0, f.is_entrance = ps == 2;
      }
      {   // S103 B3 (T3b): the card factor word [0x8376383C] (sub_827F22C8 0x827F23B8: the card technique's c184.z = the gain x (float)word * 0.001 when > 0). A run-time word of the game, so it is read here;
          // the arena's own c184 comes from the player's table at arena load (assets\arena_lm.h), not from here. Unread (never mapped) = no row, counted; the front end draws the cards unscaled then.
        uint32_t word = 0;
        f.lm_ok = read_ && scene_inputs::ReadWord(read_, kCardFactorWord, word);
        if (f.lm_ok) { f.lm_row = StageLookRow(); f.lm_row.kind = rdr_scene::kSlCardFactor, f.lm_row.flags = 1, f.lm_row.dir[0] = float(int32_t(word)) * 0.001f; lm_fed_.fetch_add(1, std::memory_order_relaxed); if (word == 0) lm_zero_.fetch_add(1, std::memory_order_relaxed); }
        else lm_unfed_.fetch_add(1, std::memory_order_relaxed);
      }
      {   // S103 CLRB (docs\agents\s103\CLR.md 3): the game's main-scene clear colour, [[0x83929A68] + 240 / 244 / 248 / 252] = R, G, B, A (0..255, sub_82C224A0 packs them as a D3DCOLOR). A run-time global (black everywhere we have
          // seen; 77 white / 79 green in a story sequence), so it is read here. A row only when the pointer and all four words read; otherwise no row (the front end clears black), counted.
        uint32_t blk = 0, w[4] = {};
        bool ok = read_ && scene_inputs::ReadWord(read_, kClearBlockPtr, blk) && blk >= 0x10000;
        for (uint32_t k = 0; ok && k < 4; ++k) ok = scene_inputs::ReadWord(read_, blk + 240 + 4 * k, w[k]);
        f.clr_ok = ok;
        if (ok) {
          f.clr_row = StageLookRow(); f.clr_row.kind = rdr_scene::kSlClearColor, f.clr_row.flags = 1;
          for (int k = 0; k < 4; ++k) f.clr_row.col[k] = float(w[k] & 0xFFu) / 255.f;
          clr_last_.store((uint64_t(w[0] & 0xFF) << 24) | (uint64_t(w[1] & 0xFF) << 16) | (uint64_t(w[2] & 0xFF) << 8) | (w[3] & 0xFF), std::memory_order_relaxed);
          clr_fed_.fetch_add(1, std::memory_order_relaxed);
        } else clr_unfed_.fetch_add(1, std::memory_order_relaxed);
      }
      {   // S111 PROPORDER (cat33_places.h, draw_place_rows.h): every drawn place of the game's category-33 list (the characters' skeletons, the entrance props' CWeapon models) as kSlDrawPlace rows of THIS latch; unread = no rows (the colour pass keeps the order before), counted
        static thread_local std::vector<scene_inputs::cat33_places::Place> scratch;   // the latch thread's scratch
        const auto rd = [this](uint32_t a, uint32_t& v) { return read_ && scene_inputs::ReadWord(read_, a, v); };
        f.places.clear();
        if (scene_inputs::cat33_places::ReadPlaces(rd, scratch)) {
          for (const scene_inputs::cat33_places::Place& q : scratch) f.places.push_back(rdr_scene::draw_place::Pack(q.addr, q.place, q.prop, q.additive, q.material_state, q.body));
          places_read_.fetch_add(1, std::memory_order_relaxed);
        } else {
          places_unread_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      f.phase = phase;   // S103 D1
      {   // S103 MBC (RMP.md 2.3 site 2): map B's mode gate needs the participant count and the ring gate word EVERY frame (the old read ran on a NoModule latch only). Pure guest reads; unknown stays -1.
        scene_inputs::PlaneStructure ps2;
        if (scene_inputs::ReadPlaneStructure(read_, ps2) && ps2.count_known) f.participants = int32_t(ps2.participants);
        uint32_t gate = 0;
        if (read_ && scene_inputs::ReadWord(read_, scene_inputs::kPlSuppressWord, gate)) f.ring_gate = gate != 0 ? 1 : 0;
        uint32_t modw = 0;   // S103 RML (F3): the module singleton, read directly (the game's sub_827ECB60 reads [0x83924758] and [0x836F39A0] only, never module+40, so Disabled / ReadFail of the plane pass must not mask it)
        if (read_ && scene_inputs::ReadWord(read_, scene_inputs::kPlaneModulePtr, modw)) f.plane_module = modw >= 0x10000 ? 1 : 0;
      }
      if (rdr_scene::entrance_tracks::On() && !f.lights.empty() && f.phase_readable && phase == 4) {   // S103 TNB F1: the bound characters' positions on EVERY phase-4 frame that read the manager (the old record path's Entrance(t) is the remap, which can hold on a frame the page object does not call an entrance)
        scene_inputs::TrackBindInfo tbi;
        scene_inputs::ReadTrackBinds(read_, f.lights[0], f.tbinds, tbi);
        tb_unresolved_.fetch_add(tbi.unresolved + tbi.nonfinite, std::memory_order_relaxed);
      }
      // S111 PAGEPOOLS (docs\research\s111\PAGEPOOLS.md, review): the stage PAGE of THIS latch (kSlPage + its kSlPool / kSlSpot rows; Emit writes them, never through ModelClass). The page node sub_825BB0E0 runs
      // on the record frame's clock (walk-time: census page_node_clock +1 324 of 324), so record r draws latch r's page, while the light objects ride latch r - 1 (ModelClass, the model draws). Its gate is the
      // GAME's: the page object's entrance word [obj+88] (0x825BB21C: [this+72], this = obj+16; != 0 = page 00 at t 0 + the track sprites sub_825BACE0, 0 = the fade by the clock [obj+108]) = f.is_entrance,
      // NOT "some set is remapped" (the look's rule, stage_look::Entrance; tnb_unit F / I hold frames where the two differ). Its rows are the Derive below, the one the decision runs.
      f.page = StageLookRow();
      f.page.kind = kSlPage;
      f.page_rows.clear();
      if (f.is_entrance) {
        uint32_t slot = 0, n = 0;
        StageLookRow rows[rdr_scene::stage_pool_words::kMaxRows];
        const uint32_t* blk = rdr_scene::stage_pool_words::PoolBlock(f.lights);
        // S103 TNB (TEN.md 4, the game's per-track binding): the manager's 10 binding words are in f.lights[0] (ReadLightObjects); the bound characters' positions are read here, then the SAME Derive the consumer runs decides
        // whether the frame has rows. With the switch off, or when the track words were not read, the old gate stands (one unique entering slot).
        rdr_scene::stage_pool_words::Tracks tr;
        const bool have_tracks = !f.lights.empty() && rdr_scene::stage_pool_words::MakeTracks(&f.lights[0], f.tbinds.data(), f.tbinds.size(), f.phase_readable, phase, tr);
        rdr_scene::stage_pool_words::DeriveStats ds;
        const bool slot_ok = have_tracks || scene_inputs::ReadEntranceSlot(read_, slot);
        const bool derive_ok = blk && rdr_scene::stage_pool_words::Derive(blk, rows, n, have_tracks ? &tr : nullptr, &ds);
        f.ent_ok = slot_ok && derive_ok;
        f.page.flags = kSlfPageEntrance | (blk && !derive_ok ? uint32_t(kSlfPageRefused) : 0u);   // S111 PAGEPOOLS: no pool block = no rows (the base page, as the record derive gave); a refused block = said (Pools: incomplete)
        if (derive_ok) f.page_rows.assign(rows, rows + n);
        if (f.ent_ok) {
          f.ent = StageLookRow();
          f.ent.kind = kSlEntrance, f.ent.flags = kSlfEntrancePage, f.ent.rig_slot = have_tracks ? (tr.has_pos[0] ? uint16_t(tr.bind[0] + 1) : uint16_t(0)) : uint16_t(slot + 1);   // NOT kSlfEntrance: Entrance() keeps the "some set is remapped" rule (review N1). With the track words: track 0's bound slot + 1 (0 = unbound: the offline fallback's slot 0, never read by the draw)
          if (have_tracks) {   // the [scene][stage] census: summed per entrance frame that has rows
            tb_frames_.fetch_add(1, std::memory_order_relaxed), tb_bound_.fetch_add(ds.bound, std::memory_order_relaxed), tb_act_.fetch_add(ds.by_act, std::memory_order_relaxed), tb_c208_.fetch_add(ds.by_code208, std::memory_order_relaxed);
            tb_disks_.fetch_add(ds.disks, std::memory_order_relaxed), tb_pools_.fetch_add(ds.pools, std::memory_order_relaxed), tb_t1_.fetch_add(ds.track1, std::memory_order_relaxed);
            tb_unbound_spot_.fetch_add(ds.unbound_spot, std::memory_order_relaxed), tb_size_unknown_.fetch_add(ds.disk_size_unknown, std::memory_order_relaxed), tb_keyed_other_.fetch_add(ds.keyed_other, std::memory_order_relaxed), tb_nonsquare_.fetch_add(ds.nonsquare, std::memory_order_relaxed);
          }
        } else {
          pools_why_[!slot_ok ? 1 : !blk ? 2 : 3].fetch_add(1, std::memory_order_relaxed);   // 1 no unique entering slot, 2 no pool block in the light objects, 3 the block refused (NaN / absurd)
        }
      }
    }
    {   // S103 A2 (S7): the plane module. No stamp on bit 9: S8 (the per-draw rows) is not built, so kInPlaneConstants stays open.
      scene_inputs::PlaneBadBlock bad;
      const scene_inputs::PlaneWhy why = scene_inputs::ReadPlaneFrame(read_, frame, f.plane, &bad);   // F1: tagged with the RECORD frame, like the walk's op-21 recorder rows (draw-half clock)
      f.have_plane = why == scene_inputs::PlaneWhy::Ok || why == scene_inputs::PlaneWhy::OffScreenFit;   // S103 PB: the game's no-draw frame is a stamped frame
      // S103 A3 (S8, audit item 4): the proxy list copies the plane node took at its entry. Mask-kind PlaneDraw rows, object only (the kind decode and the plane position are [U], `have` = 0), flagged
      // kPlaneFromLatch so plane_contrib.h can drop them where the walk recorded rows of the same frame. The plane-constants input (bit 9) is stamped when S7 read this frame AND the node's copy for this hook
      // frame exists (the node ran: an empty list is a list).
      planeproxy::The().TakeUpTo(uint32_t(frame), proxy_buf_);   // F1: the node is a draw-half hook: its copies carry the RECORD frame; newer copies stay for the next record
      bool have_copy = false;
      for (const planeproxy::Snap& sn : proxy_buf_) {
        proxy_delta_.Note(int64_t(sn.frame) - int64_t(frame));
        if (uint64_t(sn.frame) != frame) continue;   // an older copy: stale, counted by the delta census
        have_copy = true;
        for (uint32_t i = 0; i < sn.count && i < planeproxy::kWords; ++i) {
          PlaneDraw d;
          d.frame = sn.frame, d.kind = kPlaneProxy, d.flags = kPlaneFromLatch, d.object = sn.words[i], d.part = 0, d.have = 0, d.ordinal = i;
          f.plane_draws.push_back(d);
        }
      }
      proxy_rows_.store(f.plane_draws.size(), std::memory_order_relaxed);
      if (!have_copy) proxy_delta_.Never();
      if (f.have_plane && have_copy) scene_inputs::Capture(scene_inputs::kInPlaneConstants, hook);
      else if (f.have_plane) proxy_missing_.fetch_add(1, std::memory_order_relaxed);
      plane_why_[uint32_t(why)].fetch_add(1, std::memory_order_relaxed);
      rdr_scene::g_plane_game_no_module.store(why == scene_inputs::PlaneWhy::NoModule, std::memory_order_relaxed);   // S103 PLF: the plane pass names the skip
      rdr_scene::g_plane_game_suppressed.store(f.plane_module == 1 && f.ring_gate > 0, std::memory_order_relaxed);   // S103 RML (F3): from the direct reads above, not from `why` (census / log only: the draw reads SceneFrame::plane_game_off)   // S103 RML: [0x836F39A0] != 0: sub_827ECB60 is 0 too (the ring mat reads the props map)
      if (why == scene_inputs::PlaneWhy::NoModule) {   // S103 PLF: WHY (the game's own gate bytes + participant count), read on every NoModule latch, printed once per value; pure guest reads
        scene_inputs::PlaneStructure ps;
        if (scene_inputs::ReadPlaneStructure(read_, ps)) {
          const uint64_t packed = (uint64_t(ps.a & 0xFF) << 40) | (uint64_t(ps.b & 0xFF) << 32) | (uint64_t(ps.c & 0xFF) << 24) | (uint64_t(ps.participants & 0xFF) << 8) | (ps.loop_path ? 1u : 0u) | 0x100000000000000ull;
          if (plane_struct_.exchange(packed, std::memory_order_relaxed) != packed) plane_struct_changes_.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard<std::mutex> lk(diag_mu_);
          plane_struct_text_ = scene_inputs::PlaneStructureText(ps);
        } else {
          plane_struct_unreadable_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (why == scene_inputs::PlaneWhy::OffScreenFit) {   // S103 PB (log only): the first 16 burst ranges (start-end guest frames) so the next milestone run maps them to shots
        std::lock_guard<std::mutex> lk(diag_mu_);
        if (off_bursts_.empty() || frame > off_bursts_.back().second + 1) { if (off_bursts_.size() < 16) off_bursts_.push_back({frame, frame}); }
        else if (frame > off_bursts_.back().second) off_bursts_.back().second = frame;
      }
      if (bad.n && uint32_t(why) >= 7 && uint32_t(why) <= 9) {   // S103 CE: the first bad block of this reason, once
        std::lock_guard<std::mutex> lk(diag_mu_);
        std::string& d = plane_bad_[uint32_t(why) - 7];
        if (d.empty()) {
          d = std::string("{plane_bad ") + (why == scene_inputs::PlaneWhy::NonFiniteFade ? "fade" : why == scene_inputs::PlaneWhy::NonFiniteCamera ? "camera" : "fit") + " frame " + std::to_string((unsigned long long)frame) + ":";
          char b[32];
          for (uint32_t k = 0; k < bad.n; ++k) { std::snprintf(b, sizeof b, " %.6g", double(bad.v[k])); d += b; }
          d += "}";
        }
      }
    }
    {   // S103 E5: the plane mask's EyePos / PlanePos. No stamp of its own: bit 9 is stamped above (the plane frame + the proxy copy); the eyes are an input of the plane map only and a frame without them leaves a counted hole.
      const scene_inputs::EyeWhy ew = scene_inputs::ReadPlaneEyes(read_, frame, f.eyes);   // S103 L9: tagged with the RECORD frame like the plane frame above (it was `hook`: run 27 f500 held plane frame 15078 and eyes 15077, BuildE2Input refuses a mismatch, so the plane map never had its EyePos)
      f.have_eyes = ew == scene_inputs::EyeWhy::Ok;
      eye_why_[uint32_t(ew)].fetch_add(1, std::memory_order_relaxed);
    }
    if (rdr_scene::props_map::On()) {   // S103 KS1 (latch thread only, like gsf::Fill; Default ON is intended: Jarrod kept tonight's fixes on, this rides --wwe2k14_rdr_props_map): the props light, read from the module's light-1 record (the install under OURS is overwritten by light 2 before any model call, so R never holds it). Census: props_light read / no_module / unreadable / bad
      const scene_inputs::PropsWhy pw = scene_inputs::ReadPropsLight(read_, frame, f.props);
      f.have_props = pw == scene_inputs::PropsWhy::Ok;
      props_why_[uint32_t(pw)].fetch_add(1, std::memory_order_relaxed);
      if (scene_inputs::PropsDegenerate(pw)) f.props_degenerate = true, f.props_degenerate_frame = frame;   // S103 PMC2; S111 CUTREACH2: its own flag (it rides ModelClass to the record of the next latch)
    }
    if (const MemoList& l = model_list(); l.list || models_) {   // S108 RDS part A: the runtime rows of the models the game names at THIS latch (was: the kept set, which read a released model)
      if (!l.list) models_(addrs_);   // the unit fallback (no target function set): the kept set, as before
      else {
        addrs_.clear();
        for (const PaletteTarget& t : *l.list) addrs_.push_back(t.skel);
      }
      bool all = true;   // every named model read (S111 HEADGUARD: the targets are always this latch's)
      for (uint32_t m : addrs_) {
        ModelRuntimeRow row;
        if (scene_inputs::ReadModelRuntime(read_, m, frame, row, rdr_scene::wrk::Live() ? (scene_inputs::kReadPassMode | scene_inputs::kReadPartE20) : 0u)) {   // S103 WRK: wrestlers only, the switch on
          f.models.push_back(row);
        } else {
          ++f.models_unreadable;
          unreadable_.fetch_add(1, std::memory_order_relaxed);
          all = false;
        }
      }
      if (all) scene_inputs::Capture(scene_inputs::kInModelRuntime, hook);   // every named WRESTLER model read (none named = nothing to read = still a stamp)
    }
    {   // S103 A2 (W4): the override tables of the LIVE wrestler skeletons (the wrestlers contributor's LatchTargets: update hook alive, as W1), not a list of its own; the unit-test fallback is the models list.
      // No stamp when any model's table is unreadable, a half-null part table, a live pointer or a name that could not be read, or the wrestlers' own row budget ran out (counted apart). PIECES are NOT covered.
      bool pall = true;
      auto one = [&](uint32_t m) {
        scene_inputs::ParamReadInfo pi;
        if (!scene_inputs::ReadParamOverrides(read_, m, frame, f.params, pi)) {
          pall = false;
          (pi.overflow ? param_overflow_ : param_unreadable_).fetch_add(1, std::memory_order_relaxed);
        } else if (pi.value_unreadable || pi.name_unreadable) {
          pall = false;
          param_unreadable_.fetch_add(1, std::memory_order_relaxed);
        }
      };
      if (const MemoList& l = model_list(); l.list) {   // S103 L31B: the same targets as the palettes (the character table's when set); S108 RDS: resolved once per Take (ResolveModelList), shared with the runtime rows
        for (const PaletteTarget& t : *l.list) one(t.skel);
      } else if (models_) {
        models_(addrs_);
        for (uint32_t m : addrs_) one(m);
      }
      param_last_rows_.store(f.params.size(), std::memory_order_relaxed);
      if (pall) scene_inputs::Capture(scene_inputs::kInMaterialParams, hook);
    }
    if (arena_have) {   // S103 A3 (review F4): the arena's models have their OWN input (bit 14): MdlAlpha and the override tables, their own row budget, their own counters. A failure here never costs the wrestlers' bits.
      bool aall = true;
      for (uint32_t m : arena_buf_) {
        ModelRuntimeRow row;
        if (scene_inputs::ReadModelRuntime(read_, m, frame, row)) {
          f.models.push_back(row);
        } else {
          aall = false;
          arena_unreadable_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      const size_t limit = f.params.size() + scene_inputs::kArenaOverrideMaxRows;
      for (uint32_t m : arena_buf_) {
        scene_inputs::ParamReadInfo pi;
        if (!scene_inputs::ReadParamOverrides(read_, m, frame, f.params, pi, limit)) {
          aall = false;
          (pi.overflow ? arena_overflow_ : arena_unreadable_).fetch_add(1, std::memory_order_relaxed);
        } else if (pi.value_unreadable || pi.name_unreadable) {
          aall = false;
          arena_unreadable_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      arena_rows_.store(arena_buf_.size(), std::memory_order_relaxed);
      if (aall) scene_inputs::Capture(scene_inputs::kInArenaModels, hook);
    }
    {   // S103 L2 (decision side, coordinator item 4): the entrance page's pool / spot input. An ENTRANCE frame (the stage-page object's entrance word [obj+88] != 0, S103 PHS: exactly when the game runs the
      // pool draw) stamps only when the rows exist (kStagePoolRowsExist: false today), so the decision walks every entrance frame; any other frame (the match, the editor) stamps. An unreadable page object = no stamp (walks).
      // Take decided for THIS latch frame (f.ent_ok = one unique entering slot AND the pool block derivable from the light objects it just read: no NaN, a block present); a stamp is never carried over.
      if (scene_inputs::PoolsStamp(f.entrance_known ? (f.is_entrance ? 2 : 1) : 0, scene_inputs::kStagePoolRowsExist, f.ent_ok)) scene_inputs::Capture(scene_inputs::kInStagePools, hook);
    }
    {   // S111 CUTREACH: the model-class rows the record carries (latch_period.h, the camera's rule). Review: ONE copy a latch (the Frame's own fields stay this latch's reads, the units read them); the pick
        // moves the copy in and the kept value out.
      Frame::ModelClass now;
      now.models = f.models, now.lights = f.lights, now.params = f.params;
      now.models_unreadable = f.models_unreadable;
      now.props = f.props, now.have_props = f.have_props, now.props_degenerate = f.props_degenerate, now.props_degenerate_frame = f.props_degenerate_frame;   // S111 CUTREACH2
      f.rec = model_period_.Pick(frame, std::move(now));
    }
    scene_inputs::FillStamps(f.stamps);   // the snapshot Emit writes (review 4)
  }

 private:
  scene_inputs::ReadFn read_ = nullptr;
  uint32_t page_obj_ = 0, page_misses_ = 0;   // S103 FD: the stage-page pass object found last time and the rescan counter (latch thread only: Take runs only there)
  std::atomic<uint64_t> lm_zero_{0};   // S103 B3: reads that found the word 0 (the game then draws the cards unscaled)
  std::atomic<uint64_t> clr_fed_{0}, clr_unfed_{0}, clr_last_{0};   // S103 CLRB: latches with / without the scene clear colour row
  std::atomic<uint64_t> lm_fed_{0}, lm_unfed_{0};   // S103 B3: latches with / without a LightMapParam row (unread = no row: the cards draw unscaled)
  std::atomic<uint64_t> clock_found_{0}, clock_miss_{0};   // S103 FD (F3): latches with / without the page-clock row
  std::atomic<uint32_t> clock_last_t_{0}, clock_last_flags_{0}, clock_obj_{0}, clock_gptr_{0}, clock_gcount_{0}, clock_gtasks_{0};   // the last row read (t as float bits)
  ModelsFn models_ = nullptr;
  ModelsFn visible_models_ = nullptr;
  LatchMark mark_;
  Frame latched_;
  std::vector<uint32_t> addrs_;
  std::atomic<uint64_t> unreadable_{0}, no_camera_{0};
  std::atomic<uint64_t> cam_reason_[8] = {}, slot1_reason_[8] = {}, slot1_ok_{0}, slot0_reason_[8] = {}, slot0_ok_{0}, drawcam_used_{0};   // S103 WC R5: drawcam_used_ = frames whose camera came from the draw thread   // S103 S: camera diagnosis (census only)
  struct LatchCamera {   // S111 CUTSTUTTER: one latch's scene camera read, as Take keeps it for the next record
    bool have = false, from_draw = false;
    Camera camera{};
    scene_inputs::CameraInfo info{};
  };
  scene_inputs::period::OfPalette<LatchCamera> cam_period_;   // S111 CUTSTUTTER: record r takes latch r - 1's camera (latch_period.h)
  scene_inputs::period::OfPalette<Frame::ModelClass> model_period_;   // S111 CUTREACH: record r takes latch r - 1's light / model / parameter rows
  std::mutex diag_mu_;
  std::string cam_dump_[8];
  std::vector<std::pair<uint64_t, uint64_t>> off_bursts_;   // S103 PB: first 16 offscreen_fit frame ranges (diag_mu_)
  std::atomic<uint64_t> plane_struct_{0}, plane_struct_changes_{0}, plane_struct_unreadable_{0};   // S103 PLF: the packed gate bytes (0 = never read on a NoModule latch), its changes, failed reads
  std::string plane_struct_text_;   // S103 PLF: `structure a/b/c, participants N` of the last NoModule latch (diag_mu_)
  std::string plane_bad_[3];   // S103 CE: the first non-finite block per reason (fade, camera, fit)
  std::atomic<uint64_t> palettes_{0}, palette_unreadable_{0}, palette_nocount_{0}, plane_why_[scene_inputs::kPlaneWhyCount] = {}, eye_why_[4] = {}, props_why_[uint32_t(scene_inputs::PropsWhy::Count)] = {}, circle_mod_why_[uint32_t(scene_inputs::CircleModuleWhy::Count)] = {}, param_last_rows_{0}, param_unreadable_{0}, param_overflow_{0}, arena_unreadable_{0}, arena_overflow_{0}, arena_rows_{0}, light_last_rows_{0}, light_null_{0}, light_unreadable_{0}, light_missing_{0}, light_nosets_{0}, light_models_{0}, light_node_objects_{0}, light_node_unread_{0}, light_expired_{0}, light_missing_models_{0}, light_planned_holds_{0}, light_planned_by_[2] = {}, light_stale_models_{0}, light_hole_first_[8] = {};
  std::atomic<uint64_t> light_lapsed_{0}, light_never_{0}, light_wrestler_holes_{0};   // S103 A5
  std::atomic<uint32_t> light_hole_slot_[8] = {};                                      // S103 A5: stage slot + 1, 0 = none
  ArenaSlotsFn arena_slots_fn_ = nullptr;
  ArenaEmittedFn arena_emitted_fn_ = nullptr;
  std::atomic<uint64_t> light_arena_asked_{0}, light_arena_all_{0}, light_arena_offscreen_{0};
  ArenaBoundsFn arena_bounds_fn_ = nullptr;
  RingFn ring_fn_ = nullptr;   // S103 A6
  size_t arena_all_ = 0;
  std::vector<uint16_t> arena_slot_buf_;
  SlotLightSetFn slot_set_fn_ = nullptr;
  std::vector<uint32_t> nopred_buf_;      // S103 B4X F4: the kept skeletons with no character slot at the last latch
  uint64_t nopred_hook_ = ~0ull;          // the hook frame nopred_buf_ belongs to
  std::vector<uint32_t> computed_models_;   // S103 B4X F5: the models ApplyLightIds computed (and merged into `nodes`) at the last latch, sorted
  std::atomic<uint64_t> id_models_{0}, id_produced_{0}, id_absent_{0}, id_unresolved_{0}, id_equal_{0}, id_differ_{0}, id_direct_{0}, id_unlearned_{0}, id_noset_{0}, id_differ_default_{0}, id_filled_default_{0}, id_refreshed_default_{0};   // S103 A7: the last latch's census (light_by_id)
  // S103 A7 (INPUTS_A7.md): the light object of every ring and arena-slot model computed from the record alone: the model's class -> its fixed set id (light_fixed_id.h) -> position in the manager's id list ->
  // sub_826203E8's override rule + sub_827F6828's remap. `nodes` (sorted by model) is the walk's learned table: it is only COMPARED (the census) and then overwritten, entry by entry, by the computed one, which carries
  // frame = hook: it is recomputed at every latch, so it never ages. Wrestlers are not computed (the embedded object depends on mgr+380: not in the record), the crowd has no model list yet.
  void ApplyLightIds(const std::vector<LightObjectRow>& rows, uint64_t hook, std::vector<lightidx::NodeLight>& nodes) {
    std::vector<FixedModel> fm;
    {   // the ring: the class table at G (ring_rows_core.h), the class of each model pointer -> the fixed class (canvas 30, pieces 31, ropes 32)
      ringrows::RingTable rt;
      const auto rd = [this](uint32_t a, uint32_t& v) { return scene_inputs::ReadWord(read_, a, v); };
      if (read_ && ringrows::ReadRingTable(rd, rt))
        for (uint32_t i = 0; i < ringrows::kClassTableCount; ++i) {
          const FixedClass c = FixedClassOfRing(ringrows::kClassTable[i].cls);
          if (rt.ptr[i] >= 0x10000 && c != FixedClass::kUnknown) fm.push_back(FixedModel{rt.ptr[i], c, 0});
        }
    }
    uint32_t noset = 0;   // slot models SlotLightSet has no set for (an le( / lg( row, a slot >= 399 with no row, a table of another arena): not computed, counted. A row WITHOUT l(N) and a slot < 399 with no row are computed with the loader's default set 100 (SlotLightSet, B4X)
    if (slot_set_fn_ && arena_slot_buf_.size() == arena_buf_.size())   // the arena: the slot text's l(N) of each slot model (default 100: sub_824E8E38 fills slot words 0..398 (399 words) with the position of set 100, 0x824E9010-0x824E9030; docs\formats\resolver_census_findings.md section 2 [C])
      for (size_t i = 0; i < arena_buf_.size(); ++i) {
        bool expl = false;
        const uint32_t set = slot_set_fn_(arena_slot_buf_[i], &expl);
        if (set != 0xFFFFFFFFu) fm.push_back(FixedModel{arena_buf_[i], FixedClass::kArena, set, !expl});   // F2: a DEFAULT-100 slot only fills holes (below)
        else ++noset;
      }
    id_noset_.store(noset, std::memory_order_relaxed);
    std::vector<LearnedInstall> learned;
    learned.reserve(nodes.size());
    for (const lightidx::NodeLight& n : nodes) learned.push_back(LearnedInstall{n.model, n.object, n.index, n.mode});
    std::vector<ByIdEntry> es;
    const ByIdCensus c = ComputeByIds(rows, fm, learned, es);
    id_models_.store(c.models, std::memory_order_relaxed), id_produced_.store(c.produced, std::memory_order_relaxed), id_absent_.store(c.absent_id, std::memory_order_relaxed);
    id_unresolved_.store(c.unresolved, std::memory_order_relaxed), id_equal_.store(c.equal, std::memory_order_relaxed), id_differ_.store(c.differ, std::memory_order_relaxed);
    id_direct_.store(c.direct, std::memory_order_relaxed), id_unlearned_.store(c.unlearned, std::memory_order_relaxed);
    computed_models_.clear();
    uint32_t differ_default = 0, filled_default = 0, refreshed_default = 0;
    for (const ByIdEntry& e : es) {
      {   // a learned DIRECT install of another object (the ropes' modified copy) stays the walk's: not replaced (light_fixed_id.h KeepLearned)
        auto lt = std::lower_bound(learned.begin(), learned.end(), e.model, [](const LearnedInstall& x, uint32_t v) { return x.model < v; });
        const LearnedInstall* li = lt != learned.end() && lt->model == e.model ? &*lt : nullptr;
        if (KeepLearned(li, e)) continue;
        if (e.is_default) {   // S103 B4X F2: the default (set 100) was never verified against a record: a fresh learned entry stays the walk's; a hole is filled
          // S103 LH: ... unless the learned entry EQUALS the computed one (index, mode; the object is not compared: rows are deduplicated by block): then it is re-dated to this hook like any computed entry (DefaultMergeFor), so it never ages and no refresh walk is planned for it
          const DefaultMerge dm = DefaultMergeFor(li, e, lightstamp::g_refresh_default);
          if (dm == DefaultMerge::kKeep) {
            if (li->index != e.index || li->mode != e.mode || (li->index == kFixedNoIndex && li->object != e.object)) ++differ_default;
            continue;
          }
          if (dm == DefaultMerge::kRefresh) ++refreshed_default;
          else ++filled_default;
        }
      }
      lightidx::NodeLight n;
      n.model = e.model, n.object = e.object, n.index = e.index, n.mode = e.mode;
      n.frame = uint32_t(hook);   // S103 B4X: recomputed at every latch (frame = hook), so no age / keep state of its own
      auto it = std::lower_bound(nodes.begin(), nodes.end(), e.model, [](const lightidx::NodeLight& x, uint32_t v) { return x.model < v; });
      if (it != nodes.end() && it->model == e.model) *it = n;
      else nodes.insert(it, n);
      computed_models_.push_back(e.model);
    }
    std::sort(computed_models_.begin(), computed_models_.end());
    id_differ_default_.store(differ_default, std::memory_order_relaxed), id_filled_default_.store(filled_default, std::memory_order_relaxed), id_refreshed_default_.store(refreshed_default, std::memory_order_relaxed);
  }
  std::string LightIdCensus() const {
    if (!lightstamp::g_from_ids) return "light_by_id off";
    const auto u = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
    return "light_by_id == walk " + u(id_equal_) + ", differ " + u(id_differ_) + ", direct " + u(id_direct_) + ", unlearned " + u(id_unlearned_) + ", differ_default " + u(id_differ_default_) + ", filled_default " + u(id_filled_default_) + ", refreshed_default " + u(id_refreshed_default_) + ", absent_id " + u(id_absent_) + ", no_slot_set " + u(id_noset_) + ", unresolved " + u(id_unresolved_) + ", models " + u(id_models_) + ", produced " + u(id_produced_);
  }
  std::string HoleSlotList() const {   // S103 A5
    std::string o;
    char b[16];
    for (const auto& a : light_hole_slot_) {
      const uint32_t v = a.load(std::memory_order_relaxed);
      if (!v) break;
      std::snprintf(b, sizeof b, "%s%u", o.empty() ? "" : " ", v - 1u);
      o += b;
    }
    return o;
  }
  std::string HoleList() const {   // S103 A4: the first models the stamp found without a fresh entry (guest addresses, hex), for the next mirror run to name
    std::string o;
    char b[16];
    for (const auto& a : light_hole_first_) {
      const uint32_t v = a.load(std::memory_order_relaxed);
      if (!v) break;
      std::snprintf(b, sizeof b, "%s%08X", o.empty() ? "" : " ", v);
      o += b;
    }
    return o;
  }
  std::atomic<uint64_t> tb_frames_{0}, tb_bound_{0}, tb_act_{0}, tb_c208_{0}, tb_disks_{0}, tb_pools_{0}, tb_t1_{0}, tb_unbound_spot_{0}, tb_size_unknown_{0}, tb_keyed_other_{0}, tb_nonsquare_{0}, tb_unresolved_{0};   // S103 TNB: the [scene][stage] census (entrance frames with the track words)
  std::atomic<uint64_t> pools_why_[5] = {};   // pools_why_: S103 L2 (PoolsWhy histogram: why no pool rows)
  std::atomic<uint64_t> circle_chars_{0}, circle_points_{0}, circle_unset_{0}, circle_gated_{0}, circle_nocomp_{0}, circle_unreadable_{0}, circle_noheight_{0}, circle_nomodule_{0}, circle_nomesh_{0};
  MeshKnownFn mesh_known_ = nullptr;
  CensusFn palette_census_ = nullptr;
  std::vector<planeproxy::Snap> proxy_buf_;
  DeltaCensus proxy_delta_;   // noted - record frame, per copy taken (0 expected: draw-half clock)
  std::atomic<uint64_t> proxy_rows_{0}, proxy_missing_{0};
  ArenaModelsFn arena_models_ = nullptr;
  std::vector<uint32_t> arena_buf_;
  TargetsFn targets_ = nullptr;
  SlotTargetsFn slot_targets_ = nullptr;   // S103 L31B
  SlotTable slot_buf_;                     // S103 L31B: the latch thread's scratch
  scene_inputs::drawn_slots::Departed departed_{scene_inputs::drawn_slots::g_kept_char};   // S111 CAWTPOSE: this latch's departed occupants (per contributor; published for the update hook, cleared when it goes)
  std::atomic<uint64_t> palette_stale_withheld_{0}, slot_unreadable_{0}, slot_no_skel_{0};   // S103 L31B
  std::atomic<uint64_t> slot_pieces_unreadable_{0};   // S103 NRB
  std::atomic<uint64_t> cat33_read_{0}, cat33_unread_{0};   // S111 CAT33: latches whose category-33 list was read / could not be read (the record then keeps the slot order)
  std::atomic<uint64_t> places_read_{0}, places_unread_{0};   // S111 PROPORDER: latches whose category-33 places were read / could not be read (no kSlDrawPlace rows: the colour pass keeps the order before)
  scene_inputs::PieceCountFn piece_hook_count_ = nullptr;   // S105 A5a F1
  scene_inputs::PieceCountFn piece_count_ = nullptr;   // S103 NRBF
  scene_inputs::BodyDescFn piece_body_ = nullptr;      // S103 PZ F2
  scene_inputs::BodyDescFn body_raw_ = nullptr;        // S103 PZF2 A1
  BodyVerifyFn body_verify_ = nullptr;
  BodyRelistFn body_relist_ = nullptr;
  SubmitFn submit_ = nullptr;
  std::vector<PaletteTarget> pal_targets_, param_targets_;   // param_targets_: Take (S103 A2 W4)
  uint32_t hgf_faults_ = 0;   // S103 HGF planted twins (tests only)
  uint64_t pal_targets_hook_ = ~0ull;           // S103 HGF: the hook frame pal_targets_ was built for (latch thread only)
  std::vector<Mat4> pal_buf_;
  std::vector<uint32_t> pal_raw_;
};

inline InputsContributor& Inputs() {
  static InputsContributor r;
  return r;
}
inline std::atomic<bool>& RegisteredFlag() {
  static std::atomic<bool> f{false};
  return f;
}
inline void RegisterInputs() {   // idempotent
  if (!RegisteredFlag().exchange(true)) {
    scene_inputs::RegisterAll();
    Register(&Inputs());
  }
}
inline bool InputsRegistered() { return RegisteredFlag().load(std::memory_order_relaxed); }

}  // namespace rdr_scene::live::inputs
