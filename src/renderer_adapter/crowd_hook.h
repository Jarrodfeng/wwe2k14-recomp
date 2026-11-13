// The crowd's first promotion (Session 101, lane 35; docs\renderer\CROWD_PROMOTION.md): the seam between the binder's walk of a replayed
// frame and the code that draws the crowd from the scene record. The binder (binder.h) knows nothing of the scene record, the loaders or
// the front-end: it calls a CrowdHook at the first draw the game's crowd recorder submitted, hands it the state of that draw, and drops every
// such draw of the frame. No game or SDK includes.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../renderer/api.h"
#include "../renderer/frame_builder.h"
#include "capture_reader.h"

namespace rdr_adapter {

// The recorder calls (DrawOrigin::call_site) that submit the crowd's draws: the stands members' blocks (sub_82518340 via 0x82518574) and the
// far flat cards (sub_82512F70 via 0x825130C4). Source: docs\renderer\CROWD_PROMOTION_MAP.md section 1 (scene_crowd.h:5-6, 17-18).
inline constexpr uint32_t kCrowdBlocksSite = 0x82518574u;
inline constexpr uint32_t kCrowdCardsSite = 0x825130C4u;
// S101 lane 82 (docs/renderer/CROWD_FAMILY_8276ED60.md): the seated floor people, sub_8276EAA0's indexed draw (op 15 at 0x8276ED60): the type's LOD 0 body from the stands' LOD 0
// vertex buffer, the same program and state as the block draws. Claimed only when the hook's DrawsFloor() is true (its own switch).
inline constexpr uint32_t kCrowdFloorSite = 0x8276ED60u;

// What the dropped draw was: everything the hook needs to draw our members "the way the game would have", by name and register.
struct CrowdInject {
  const LoadedFrame* frame = nullptr;
  const Draw* draw = nullptr;
  uint32_t draw_index = 0;
  rdr::FrameBuilder* builder = nullptr;   // the frame being built; the hook appends draws to it, into the targets bound at this point
  int vs = -1, ps = -1;                   // rdr::fx::kVariants indices the binder chose for the dropped draw (the yAudience pair)
  rdr::DrawState state{};                 // the dropped draw's fixed state, program set; vertex_layout is the stream's (the hook replaces it)
  rdr::DynamicState dynamic{};            // viewport, scissor, clip flip, jitter
  rdr::TextureSet textures{};             // the sampler states and every slot the dropped draw binds; the hook replaces slot 0 only (per member, from that member's own stream draw)
  const rdr::ImageId* images = nullptr;   // the binder's image table: a capture image id -> the renderer image (a member's own stream draw names its texture by capture id)
  uint32_t image_count = 0;
  // The capture's / the live store's geometry bytes by the draw's vertex / index offset (verify only: the pose check reads the stream's captured vertices). Empty: not available.
  std::function<bool(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out)> read_geometry;
  const uint8_t* vs_block = nullptr;      // the dropped draw's packed vertex / fragment constant blocks (Binder::PackConstants' layout)
  uint32_t vs_block_bytes = 0;
  const uint8_t* fs_block = nullptr;
  uint32_t fs_block_bytes = 0;
  int density = 100;                      // g_crowd_density (percent of the members drawn)
  int (*rank)(uint32_t w3, uint32_t w7, uint32_t w11) = nullptr;  // binder.h CrowdRank: the member's 0-99 rank from its world position
  // S101 lane CD (the far cards, docs\renderer\CROWD_CARDS_PROMOTION_MAP.md): the program of (vertex variant, pixel variant) the binder builds (its alpha-test twin when `alpha_test`),
  // so the hook can draw its own vertex program with the dropped draw's pixel stage; `alpha_test` = the dropped draw's fixed state asks for the alpha test.
  std::function<uint16_t(int vs, int ps, bool alpha)> program;
  bool alpha_test = false;
  // lane 175 (card_thin_core.h), OUT: what InjectCards saw. 0 = nothing to report, 1 = a full frame drawn, 2 = a full frame refused (every stream card stays), 3 = a thin frame drawn, 4 = a thin frame ours
  // could not draw (a hole: its dropped cards are gone). The binder reports it to the cut's gate.
  uint8_t card_result = 0;
  // lane 272 (member_thin_core.h), OUT: what Inject saw. 0 = nothing to report, 1 = a full frame drawn clean (nothing left to the stream, every class uniform), 2 = a full frame refused or not clean, 3 = a thin frame drawn,
  // 4 = a thin frame ours could not draw (a hole). The binder reports it to the members' cut gate.
  uint8_t member_result = 0;
};

class CrowdHook {
 public:
  virtual ~CrowdHook() = default;
  // Once per frame, at the first crowd draw: is there a usable record (and loaded bodies) for this frame? False: the stream's draws stay.
  virtual bool FrameReady() = 0;
  // Appends our crowd to `in.builder`. Returns the members drawn (0: nothing was drawn, nothing appended; the stream's draws stay).
  virtual uint32_t Inject(CrowdInject& in) = 0;
  // S101 lane 35 (the look fix): does the hook draw the far cards? False (the default): the stream's card draws (a 4-vertex quad with the game's own picture of the
  // person) are left alone and only the block draws are replaced. True: the cards are dropped too (ours draws them as bodies, or, as a planted fault, not at all).
  virtual bool DrawsCards() const { return false; }
  // S101 lane 82: does the hook draw the seated floor people (site kCrowdFloorSite)? False (the default): those draws are left alone and the injection happens at the first stands block draw as before.
  virtual bool DrawsFloor() const { return false; }
  // A bound frame with no crowd draw at all (the binder calls it once per such frame while the switch is on): lets the hook release what it loaded at a load screen.
  virtual void NoCrowdFrame() {}
  // After a successful Inject: this stream crowd draw (index in the frame's draws) is NOT replaced (a member we could not draw: no body, no palette). The binder draws it the stream's way.
  virtual bool KeepDraw(uint32_t draw_index) const { (void)draw_index; return false; }
  // S101 lane CD (the far cards drawn by us from the record, CROWD_CARDS_PROMOTION_MAP.md C1): true = the cards have their OWN injection. The binder then calls InjectCards at the
  // frame's first card draw (call site kCrowdCardsSite) instead of treating the card draws as block members: returns the cards drawn (0: every card draw of the frame stays the
  // stream's), and a card draw is dropped unless KeepCardDraw says it stays. DrawsCards() stays false for it (the crowd gate's P2b would drop the game's card recording, InjectCards would never fire and every far card would be missing); a gate that should know about cards needs cards_state_ == 1 && card_draws_kept == 0 per frame.
  virtual bool InjectsCards() const { return false; }
  // lane 156: the floor counters for the 600-frame census window (empty = floor off): ", floor members drawn F, floor draws kept K of S floor stream draws, ..."
  virtual std::string FloorDetail() const { return {}; }
  virtual uint32_t InjectCards(CrowdInject& in) { (void)in; return 0; }
  virtual bool KeepCardDraw(uint32_t draw_index) const { (void)draw_index; return false; }
  // lane 272: the frame holds crowd draws but FrameReady said no (no record / bodies not loaded): was it a THIN frame (the game dropped member draws)? 4 = yes, a hole (the gate resets); 0 = no / unknown.
  virtual uint8_t MemberNotReadyResult(const LoadedFrame& frame) { (void)frame; return 0; }
};

// The census numbers the binder keeps for the switch (one line per window: "[rdr][crowd] ours frames N, fallback F, ...").
struct CrowdCounters {
  uint64_t ours_frames = 0;       // frames whose crowd we drew
  uint64_t fallback_frames = 0;   // frames with crowd draws where the stream's stayed (no record / bodies not ready / nothing drawn)
  uint64_t draws_dropped = 0;     // stream block / card draws left out
  uint64_t draws_kept = 0;        // stream crowd draws kept for members ours could not draw (no body / no LOD 0 palette)
  uint64_t members_drawn = 0;     // members (bodies) we drew
  double ms_ours = 0;             // time inside Inject
  uint64_t persist_only = 0, card_persist_only = 0;   // lane 239: block / card frames of a persistent-only bind (never shown): apart from ours / fallback
  // S101 lane CD (the far cards through InjectCards): frames drawn / frames the stream's cards stayed, card draws dropped / kept, cards drawn, time inside InjectCards.
  uint64_t card_frames_ours = 0, card_frames_fallback = 0, card_draws_dropped = 0, card_draws_kept = 0, cards_drawn = 0;
  double card_ms_ours = 0;
};

}  // namespace rdr_adapter
