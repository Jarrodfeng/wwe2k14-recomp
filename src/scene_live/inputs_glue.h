// The scene inputs: the game plumbing (S103 lane A; docs\renderer\INPUTS_BOARD.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Gives the pure contributor (inputs_contrib.h) its guest reader (wrestler_hook.h's bounds- and fault-checked ReadGuest) and its model list (the wrestlers contributor's kept skeletons), and
// registers it with the scene writer. Read only: nothing is written to the guest, no register changes, no call is skipped. No hook is added: every reader here runs at the frame's latch
// (the game threads parked), so no hook of the game's walks is touched (a function takes one hook: S101).
#pragma once
#include "../guest_read.h"   // GuestWordReadableFast
#include <rex/logging.h>   // S110 CROWDFLIPS
#include "crowd_latch_reader.h"   // S103 D3b
#include <type_traits>
#include "guest_texture.h"       // PhysRangeOk (A8)
#include "inputs_contrib.h"
#include "wrestler_contrib.h"
#include "wrestler_hook.h"   // ReadGuest
#include "arena_world_contrib.h"   // S103 A3
#include "stage_fx_contrib.h"     // S103 A3
#include "arena_contrib.h"        // S103 A5: EmittedSlots
#include "circle_contrib.h"       // S103 A3 (the mesh-known test)
#include "ring_recompute_contrib.h"   // S103 A6: the ring object X (NotedFrame / Ring)

namespace wwe2k14::scene_inputs_glue {

// ReadGuest page-checks off Windows and has the SEH on Windows (wrestler_hook.h); the name stays for the readers that use it.
inline bool ReadChecked(uint32_t guest, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuestNoNote(guest, dst, n); }   // S110 FEEDREC: the reader type (ReadFnT) notes the read at its caller

// S103 D3b: guest memory for the crowd's latch reader (crowd_latch_reader.h): fault-checked, big-endian undone, `fail` set when a read is refused.
struct CrowdGuestMem {
  mutable bool fail = false;
  mutable uint32_t fail_addr = 0;   // S103 D8: the first refused address since `fail` was last clear (crowd_latch_reader.h Scoped resets `fail` per unit)
  // S108 RDS phase 2: the snapshot's line copy (safe_guest_pages.h LineCopy), owned by CrowdLatchRead for ONE snapshot; null = one checked read per field (the hasher's phys-only instance). The floor's per-seat
  // words (ReadFloor's SoftEach: e+152/+156/+160, the 16-byte position, the 64-byte matrix, the flags halfword) were one page check each: ~2,200 a frame in Fandango's entrance (S108 RDS tally, thread 8B28).
  wwe2k14::safe_pages::LineCopy<>* lines = nullptr;
  bool Read(uint32_t a, void* dst, size_t n) const {
    if (!lines) return ReadChecked(a, dst, n);
    return lines->Get(a, uint32_t(n), dst, [](uint32_t x, uint32_t k, void* o) { return ReadChecked(x, o, k); });   // n <= the snapshot's largest read: a 64 KB table is a direct read (Get takes n > 128 directly)
  }
  void Refuse(uint32_t a) const { if (!fail) fail_addr = a; fail = true; }
  uint32_t u32(uint32_t a WWE_FEED_LOC_PARAM) const {
    uint32_t v = 0;
    WWE_FEED_NOTE(a, 4);   // S110 FEEDREC: Read() below is the un-noted leaf (line copy or checked read)
    if (!Read(a, &v, 4)) return Refuse(a), 0;
    return scene_inputs::Swap32(v);
  }
  float f32(uint32_t a WWE_FEED_LOC_PARAM) const {
    const uint32_t v = u32(a WWE_FEED_LOC_ARG);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  void words(uint32_t a, uint32_t n, uint32_t* dst WWE_FEED_LOC_PARAM) const {
    WWE_FEED_NOTE(a, n * 4u);
    if (uint64_t(n) * 4 > 0xFFFFFFFFull || !Read(a, dst, size_t(n) * 4)) {
      Refuse(a);
      std::memset(dst, 0, size_t(n) * 4);
      return;
    }
    for (uint32_t i = 0; i < n; ++i) dst[i] = scene_inputs::Swap32(dst[i]);
  }
  // S103 D6: n bytes of PIXEL memory at a physical (GPU) address, as stored (guest_texture.h does the untile / endian swap). A refusal is NOT a `fail` of the whole snapshot: the picture reports its own `why`.
  bool phys(uint32_t addr, uint32_t n, uint8_t* dst) const {
    if (!n || n > (1u << 20) || !rdr_scene::live::guest_tex::PhysRangeOk(addr, n)) return false;   // A8: the range check is ours
    const auto* host = wwe2k14::wrestlers::GuestMem()->TranslatePhysical<const uint8_t*>(addr);
    return host && wwe2k14::wrestlers::PhysRangeReadable(addr, n) && wwe2k14::wrestlers::SafeCopy(dst, host, n);
  }
};
inline uint32_t CrowdLatchRead(rdr_scene::live::crowd::LatchSnapshot& out) {
  wwe2k14::safe_pages::LineCopy<> lines;   // S108 RDS: one snapshot = one pass; dropped on return (no line is read at a later latch)
  CrowdGuestMem mem;
  mem.lines = &lines;
  const uint32_t why = uint32_t(rdr_scene::live::crowd::latch::ReadSnapshot(mem, out));
  if (wwe2k14::crowd_flips::g_line_due.exchange(false, std::memory_order_acquire)) {   // S110 CROWDFLIPS: one census line per 600 latches
    char line[1024];
    wwe2k14::crowd_flips::The().Line(line, sizeof line);
    REXLOG_INFO("[wwe2k14][crowd_flips] {}", line);
  }
  return why;
}

// Once, at start (wwe2k14_app.h OnPostSetup, with the other class contributors, after RegisterWrestlers so Emit order is wrestlers first). Idempotent.
inline void Register() {
  auto& c = rdr_scene::live::inputs::Inputs();
  c.SetReader(&ReadChecked);
  c.SetModels([](std::vector<uint32_t>& out) { rdr_scene::live::Wrestlers().KeptSkeletons(out); });
  c.SetVisibleModels([](std::vector<uint32_t>& out) { rdr_scene::live::Wrestlers().EmittedSkeletons(out); });   // S103 V1F F2: the light coverage only
  // S103 A2 (W1): the palettes are read at the start of the wrestlers' latch and submitted as consumer palettes.
  c.SetPalettes(
      [](uint64_t hook, std::vector<rdr_scene::live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing) {
        static thread_local std::vector<rdr_scene::live::WrestlerContributor::PaletteTarget> t;   // the latch thread's scratch
        rdr_scene::live::Wrestlers().LatchTargets(hook, t, missing);
        out.clear();
        for (const auto& x : t) out.push_back({x.skel, x.count});
      },
      [](uint32_t skel, uint32_t n, const rdr_scene::Mat4* skin, uint64_t f) { return rdr_scene::live::Wrestlers().SubmitLatchConsumer(skel, n, skin, f); });
  // S103 L31B: the targets come from the character table (every occupied slot's skeleton [char + 48], every latch), not from the update hook's liveness; the guard's count rides along. Replaces the
  // TargetsFn above (kept as the fallback when this is unset, and as the unit's planted twin).
  c.SetOffLatch([](bool v) { rdr_scene::live::WrestlerContributor::SetOffLatchThread(v); });   // S103 AUSF review 1
  c.SetSlotPalettes(
      [](uint64_t hook, uint64_t record, const rdr_scene::live::inputs::InputsContributor::SlotTable& slots, std::vector<rdr_scene::live::inputs::InputsContributor::PaletteTarget>& out, uint32_t& missing,
         uint32_t* stale_visible, bool count) {
        static thread_local std::vector<rdr_scene::live::WrestlerContributor::PaletteTarget> t;   // the caller's thread's scratch
        static thread_local rdr_scene::live::WrestlerContributor::SlotTable s;
        s.skels.clear();
        for (const auto& x : slots.skels) s.skels.push_back({x.skel, x.slot, x.shown_addr, x.game_shown});
        static_assert(std::is_same_v<decltype(s.pieces), decltype(slots.pieces)>, "the piece list is ONE shared type (slot_piece.h)");   // S103 NRBF
        s.pieces.clear();
        s.pieces_view = &slots.pieces;   // S103 NRB; S103 HGF (XLB 3): a view of the latch's own list, no deep copy of every piece's vectors (LatchTargets reads pieces_view)
        s.ring_models = slots.ring_models;   // S103 NRBF
        static_assert(int(rdr_scene::live::inputs::InputsContributor::kSlotEmpty) == int(rdr_scene::live::WrestlerContributor::kSlotEmpty) &&
                      int(rdr_scene::live::inputs::InputsContributor::kSlotOccupied) == int(rdr_scene::live::WrestlerContributor::kSlotOccupied) &&
                      int(rdr_scene::live::inputs::InputsContributor::kSlotUnreadable) == int(rdr_scene::live::WrestlerContributor::kSlotUnreadable), "the two SlotState enums must agree");
        for (int i = 0; i < 9; ++i) s.state[i] = slots.state[i];
        auto& w = rdr_scene::live::Wrestlers();
        w.LatchTargets(hook, &s, t, missing, count);
        if (stale_visible) *stale_visible = w.StaleVisibleAt(record, &s);
        out.clear();
        for (const auto& x : t) out.push_back({x.skel, x.count});
      },
      [](uint32_t skel, uint32_t n, const rdr_scene::Mat4* skin, uint64_t f) { return rdr_scene::live::Wrestlers().SubmitLatchConsumer(skel, n, skin, f); });
  c.SetPieceNodeCount([](uint32_t m) { return rdr_scene::live::Wrestlers().WantedCount(m); });   // S103 NRBF: lane 209's wanted count (an own skeleton has no update-hook count): the node-map proof reads exactly the recorded node count
  c.SetPieceHookCount([](uint32_t m) { return rdr_scene::live::Wrestlers().WantCount(m); });   // S105 A5a F1: the update hook's count, asked only for a model neither the wanted table nor the game's own count knows (a CAW piece). Latch thread, mutex_ not held in ReadSlotSkeletons
  c.SetPieceBodyDesc([](uint32_t skel) {   // S103 PZ F2: the owner body's declared link ([char + 48] = desc + 4 = the body model, the identity the contributor counts as LinkedSeenAsSkeleton)
    const auto info = rdr_scene::live::Wrestlers().BodyKeyFor(skel);   // PZF2 B-4: desc + key only (no piece-vector copy under the mutex)
    scene_inputs::BodyDesc d;
    d.known = info.known && info.verified, d.desc = info.desc, d.key = info.key;   // PZF2 A1: a link whose descriptor check did not pass this latch is no link
    return d;
  });
  c.SetBodyIdentity(   // S103 PZF2 A1 / A3: the latch verifies each slot body's link against the game's persistent descriptors (inputs_contrib.h VerifyBodyIdentity)
      [](uint32_t skel) {
        const auto info = rdr_scene::live::Wrestlers().BodyKeyFor(skel);
        scene_inputs::BodyDesc d;
        d.known = info.known, d.desc = info.desc, d.key = info.key;   // raw: verified or not
        return d;
      },
      [](uint32_t skel, bool ok) { return rdr_scene::live::Wrestlers().VerifyBodyLink(skel, ok); },
      [](uint32_t skel, const std::vector<scene_inputs::DescPiece>& pieces) {
        std::vector<rdr_scene::live::WrestlerContributor::PieceRef> v;
        v.reserve(pieces.size());
        for (const auto& p : pieces) v.push_back({p.model, p.id});
        rdr_scene::live::Wrestlers().RelistPieces(skel, std::move(v));
      });
  // S103 A3 (A1-A4): the arena's object list is the stage object's slot models (lane C2's contributor reads the stage the existing MotionSetup post-hook stored).
  c.SetArena([](std::vector<uint32_t>& out) { return rdr_scene::live::stagefx::StageFx().StageModels(out); });
  rdr_scene::live::arena::SetSlotRecordReader([](uint8_t* out, size_t n) { return rdr_scene::live::stagefx::StageFx().ReadSlotRecords(out, n); });   // S103 V1 (R17): the game's per-slot 'shown' bit (stage + 1220 records)
  c.SetArenaEmitted([](std::vector<uint16_t>& out) { return rdr_scene::live::arena::EmittedSlots(out); });   // S103 A5: bit 4 asks only about the slot models the record emits
  c.SetSlotLightSet([](uint16_t slot, bool* expl) { return rdr_scene::live::arena::SlotLightSet(slot, rdr_scene::live::stagefx::StageFx().StageArena(), expl); });   // S103 A7: bit 4's arena-slot models by their l(N) (only with lightstamp::g_from_ids)
  c.SetArenaSlots([](std::vector<uint32_t>& out, std::vector<uint16_t>& slots) { return rdr_scene::live::stagefx::StageFx().StageModelsWithSlots(out, slots); });   // S103 A5: holes named by stage slot
  // audit item 1: bit 5 (arena_worlds) is stamped by stage_fx's own take, the same frame that judged the stage clean. NO clock claim: the J1 comparison (d3_record.h:7, d3_arena.h:108) proved c186 is never
  // frame - origin, so the kAwClock row stays out of OURS frames and c186 is an open input (INPUTS_A3.md).
  rdr_scene::live::stagefx::StageFx().SetStamp([](uint64_t record_frame) { scene_inputs::Capture(scene_inputs::kInArenaWorlds, scene_inputs::HookFrameOfRecord(record_frame)); });
  c.SetRing([](uint64_t hook) -> uint32_t {   // S103 A6: the ring object X lane C3 notes every frame (its colour callback's r3 - 24); stale (the ring task not running) = 0
    auto& r = rdr_scene::live::ringrecompute::RingRecompute();
    return r.NotedFrame() + 2 >= hook ? r.Ring() : 0u;
  });
  c.SetMeshKnown([]() { return rdr_scene::live::circles::CirclesRegistered() && rdr_scene::live::circles::The().tracker.has_mesh(); });   // bit-8 review 4
  c.SetPaletteCensus([]() {
    auto& w = rdr_scene::live::Wrestlers();
    return "latch_same " + std::to_string((unsigned long long)w.LatchSame()) + " latch_differs " + std::to_string((unsigned long long)w.LatchDiffers()) + " derived_mismatch " + std::to_string((unsigned long long)w.DerivedMismatch()) +
           " derived_unchecked " + std::to_string((unsigned long long)w.DerivedUnchecked()) + " mismatch_walk_same " + std::to_string((unsigned long long)w.MismatchWalkSame()) + " mismatch_walk_differs " + std::to_string((unsigned long long)w.MismatchWalkDiffers()) + " mismatch_submitted " + std::to_string((unsigned long long)w.MismatchSubmitted()) + " mismatch_not_present " + std::to_string((unsigned long long)w.MismatchNotPresent()) + " mismatch_no_evidence " + std::to_string((unsigned long long)w.MismatchNoEvidence()) + " " + w.DerivedDeltaStr() + ", " + w.SlotCensus();   // S103 L31B
  });
  rdr_scene::live::stagefx::StageFx().SetStageChanged([]() { rdr_scene::live::lightidx::The().Clear(); });   // F2: a new stage / arena starts a new table
  rdr_scene::live::lightidx::RegisteredFlag().store(true, std::memory_order_relaxed);   // S103 A3 (bit 4): the install / model hooks start learning
  rdr_scene::live::planeproxy::RegisteredFlag().store(true, std::memory_order_relaxed);   // S103 A3 (S8): the plane node's entry hook starts copying the proxy list
  rdr_scene::live::Wrestlers().SetPreLatch([](uint64_t record_frame, bool latch_thread) { rdr_scene::live::inputs::Inputs().LatchPalettes(record_frame, latch_thread); });
  // S103 S (2026-11-16, run 20 = wwe2k14_7273, mode 1, Brock's entrance) [C]: palette latch_same 17437 / latch_differs 0, and the derived head
  // that matches sits at hook - head = -1 on 15760 of 17724 records (0: 320 = still poses, where the -1 head is equal too; +1: 17; other: 51).
  // Settled: only the -1 head is accepted for the bit-3 guard (A3 re-review must 7).
  // S103 W1: the same -1, written as what it is: the latch at hook H reads the content tagged H + kPaletteLead (wrestler_contrib.h), so the equal derived head is at H - head = -kPaletteLead (tagging change: no new value; unit w1.guard_*).
  rdr_scene::live::Wrestlers().SetAcceptedHeadDelta(-int64_t(rdr_scene::live::WrestlerContributor::kPaletteLead));
  rdr_scene::live::crowd::The().SetLatchReader(&CrowdLatchRead);   // S103 D3b: the walk-free crowd snapshot (an OURS frame), compared with the walk's on a walked frame
  rdr_scene::live::inputs::RegisterInputs();
  rdr_scene::live::SetEmitStampFill(&::scene_inputs::FillEmitStamps);   // S103 L1C review B1: bit 14 is stamped at Emit
  rdr_scene::live::SetStampFill(&::scene_inputs::FillStamps);   // S103 S (OURS_INPUT_AUDIT item 7): one stamp snapshot per record, at the end of the latch
}

}  // namespace wwe2k14::scene_inputs_glue
