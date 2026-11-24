// Class C4, the wrestlers, as a live scene contributor (Session 101, lane 29; docs\systems\wrestler_pose.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game poses a wrestler's skeleton in sub_82BD30E8 (the "world" matrices at skeleton+192, one 64-byte row-vector matrix per node,
// 2-3 updates per skeleton per frame on guest thread 7). The skinning matrices the draw reads (skeleton+200) are made by a SEPARATE
// function, sub_82BD35B0, as  skin[i] = inv_bind[i] x world[i]  (row vectors; verified from the listings of sub_82BD35B0 and sub_82296630,
// docs\systems\wrestler_pose.md). So the hook at the exit of sub_82BD30E8 (src\scene_live\wrestler_hook.h) hands this class the world
// matrices and the inverse bind (skeleton+224), and this class derives the palette itself. This file has no game or SDK includes: the
// hook side reads guest memory, byte-swaps, and calls Submit*; Emit writes the part of the record.
//
// Per frame, per skeleton updated this frame: the palette of the LAST update (later updates overwrite earlier ones). One Object per
// skeleton: id "actor:slot:N" when the hook matched the skeleton to character slot N (pointer equality, see wrestler_hook.h), else
// "skel:XXXXXXXX" (the skeleton object's guest address in hex: stable within one run, not across runs). The asset is UNKNOWN (the
// skeleton -> wrestler -> pac link and the attire are not verified): the object points at one placeholder AssetRef (pac 0 = none,
// variant 0xFFFFFFFF) so the record still parses, and the field is counted in Counters::unknown. Rules: live_scene.h.
//
// S101 lane 43 (W1; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 2-3):
//  * PALETTE AT THE CONSUMER. The exit of sub_82BD30E8 is NOT where the draw reads: Brock's nodes 59-108 differ there (0.02-1.3 units). The hook at
//    sub_82C04DF0 (model_hooks.h; r3 = model, r5 = [model+200]) copies node-count x 64 bytes from the array the c76 rows are cut from and calls
//    SubmitConsumer. A consumer palette is exact by construction. The two sources carry frame tags from different threads, so priority does NOT
//    depend on the tags: while a consumer read for a skeleton is fresh (within kConsumerFresh frames of the derived tag) derived palettes are
//    bookkeeping only (slot, node count). An Emit that had to use a derived (exit) palette counts it in Counters::unknown. The delta of
//    sub_82BD3620 is NOT applied to a consumer palette: it is whatever +200 held when the draw read it (the game applies the delta to +200 itself),
//    so adding it again would double-count if the game ran it first and would disagree with the draw if it ran after.
//  * ENGINE OBJECTS. Every Object this class emits writes its skeleton's guest address into SceneTables::engine_objects (the pairing key with
//    DrawOrigin::object; the map's section 3). Emit first resizes the vector to the objects already there (live_scene.cpp's padding rule).
//  * ASSET LINK. SubmitAssetLink (declared: from the char model reader, sub_82720658 -> Factory_Model / Factory_SubModel) gives the Object a real
//    AssetRef. The guest-side hook that reads pac / entry / attire is [U] until lane 44 (R1) lands; the API is ready. SubmitAssetGuess (fingerprint
//    fallback, by content on first sight) NEVER enters the record (SCENE_RECORD.md: unknown stays unknown); it is kept and counted, and the draw
//    side asks GuessFor(skel).
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../keep_stream.h"   // S103 AUSF: WWE2K14_STREAM() (the no-walk rules below)
#include "../renderer_scene/live_scene.h"
#include "frame_delta.h"   // S103 A3 (F3)
#include "tail_latch.h"  // S102 U1
#include "slot_piece.h"  // S103 NRBF: scene_inputs::CharPiece / PieceWhy, the one shared piece type
#include "own_rows_rule.h"  // S105 A5a: JudgeOwnRows (the copy proof of a CAW own-skeleton piece)
#include "lock_stats.h"  // BHF1: WantCount lock cost counters
#include "change_colour.h"  // S108 THR: the Threads editor's live colours, read in NoteDrawAsked

namespace rdr_scene::live {

// skin = inv_bind x world (row vectors: the layout of Mat4, translation in [12..14]); the order the game's own sub_82296630 uses.
Mat4 SkinFromWorld(const Mat4& world, const Mat4& inv_bind);

class WrestlerContributor final : public Contributor {
 public:
  static constexpr uint32_t kUnknown = 0xFFFFFFFFu;
  static constexpr uint32_t kDefaultMinNodes = 60;  // wrestler skeletons have ~109 nodes (assets.md); a size filter, NOT an identity test [U]

  const char* Name() const override { return "wrestlers"; }
  void Emit(const FrameInfo& f, SceneTables& t, Counters& c) override;
  bool Latch(const FrameInfo& f) override;
  // S103 A2 (W1): a function run at the very start of Latch (the inputs contributor reads the palettes at [skeleton+200] and submits them as consumer palettes, so this latch collects them). Null = none.
  using PreLatchFn = void (*)(uint64_t record_frame, bool latch_thread);   // S103 HGF (review 2): latch_thread false = Emit's fallback (game threads running): palettes only
  void SetPreLatch(PreLatchFn f) { pre_latch_.store(f, std::memory_order_relaxed); }
  // S102 U1 (tail_latch.h): Emit's item list (the two mutex_ blocks) and the light set, while the game threads are parked

  // ---- hook side (any thread, short lock) ----
  // `skel` = the skeleton object's guest address (the stable key), `slot` = character slot 0..8 or -1 when not matched.
  // Palette given as skin matrices, or as world + inverse bind (the game's own pair; skin is derived with SkinFromWorld).
  // `frame` = the guest frame counter read on the updating thread (S101 lane 29 review 7): Emit takes, per skeleton, the newest update with
  // frame <= FrameInfo::guest_frame (0 = untagged: always eligible), so a thread running ahead of the boundary does not leak into an earlier record.
  void SubmitSkin(uint32_t skel, int32_t slot, uint32_t count, const Mat4* skin, uint64_t frame = 0);
  void SubmitWorld(uint32_t skel, int32_t slot, uint32_t count, const Mat4* world, const Mat4* inv_bind, uint64_t frame = 0);
  // sub_82BD3620 (the hook wwe2k14_skel_shift): the game adds (dx,dy,dz) to the translation (floats 12..14) of every node of +192 AND +200 after the
  // update, unseen at the exit of sub_82BD30E8. Applied to the newest stored palette of that skeleton (pending update, else the persistent one).
  void ApplyDelta(uint32_t skel, float dx, float dy, float dz);
  // Lane 43 (hook side, any thread): the palette AS THE DRAW READS IT (copied at sub_82C04DF0's entry). `count` = WantCount(skel) nodes.
  // S102 U3 (bit 16): `in_place` = a call whose skeleton already holds a consumer update of this frame is written into that update's own buffer (no allocation, same contents and flags).
  void SubmitConsumer(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t frame = 0, bool in_place = false);
  // Node count the hook must copy for this model object: the largest count sub_82BD30E8 saw for it, 0 = never seen (then no copy is made).
  uint32_t WantCount(uint32_t skel);
  // S105 CAWP: the node count of a model that is a CURRENT SUBM piece link (the last latch that read the character table), 0 = not a link / no count. The guest model reader asks it for a piece the update hook never sized (a created superstar's dog tag).
  uint32_t PieceLinkCount(uint32_t model);
  bool GuestModelWanted(uint32_t model);   // S105 CAWP: the guest model reader's want: sized by the update hook (WantCount) OR a current piece link with a count (PieceLinkCount)
  // S103 CW-2b (CW2_review bug 1): true when the record the emitter writes for this skeleton NAMES its model (Link::known): the declared link of the Known record, a body link the latch's descriptor check
  // verified (or the planted fault's), or a participant slot key; the emitter's own three sources (wrestler_contrib.cpp Collect), asked on the hook thread. False for a skeleton nobody knows too
  // (the caller has already sized it with WantCount). A model the record cannot name is the guest model reader's (guest_model_contrib.h).
  bool RecordCanName(uint32_t skel);
  bool RecordCanNameL(uint32_t skel);   // S105 CAWP fix: mutex_ held by the caller
  // (S105 A5a F9: for an own-skeleton piece the count is the wanted count, else the game's own, else the update hook's max_count (CAW pieces: the stream-free build has no wanted table).)
  // S103 A2 (review fixes 1-4): the latch reader's view of this class. One short lock.
  //  * LatchTargets: every skeleton the latch should read the palette of = the union of the pending (cur_) and kept (last_) skeletons whose update hook is ALIVE (Known::upd_frame within kConsumerFresh of
  //    `hook_frame`: a wrestler who left stops being updated, so the latch stops feeding it and it ages out after kPruneAfter). Count = the update hook's max_count, or for an own-skeleton piece (lane 209: the
  //    update hook never sees it, max_count 0) the wanted count; a piece has NO update-side liveness signal [U], it is read while it stays kept. `missing` = kept skeletons with no count at all (the caller must not stamp).
  struct PaletteTarget { uint32_t skel = 0, count = 0; };
  void LatchTargets(uint64_t hook_frame, std::vector<PaletteTarget>& out, uint32_t& missing);
  // S103 L31B: the targets from the game's CHARACTER TABLE, independent of update-hook liveness. `slots` = (skeleton, slot) of every occupied slot of the table at the latch: character
  // [0x836F38D0 + 4 * slot], skeleton [char + 48] (sub_8259FAE8 0x8259FCE4 `lwz r3,48(r30)`, the model its draw poses; sub_827CCB38 0x827CCB38-0x827CCB68 reads [[char+48]+288] and [[char+48]+192], the node-index
  // and world-array fields sub_82BD30E8 uses on its `this`; sub_82812E90 reads [[char+48]+192] for the circle points). Every slot skeleton whose node count the update hook ever reported (Known::max_count) is a
  // target EVERY latch, alive or not by the hook's clock (run 30 f6001: a body the hook stopped reporting was dropped from the latch and emitted with its last palette for up to kPruneAfter records). The
  // other skeletons (no slot: managers, waiting models, pieces) follow the hook-liveness rule as before; a piece whose owner is a slot skeleton is alive with it. A slot skeleton with no count is NOT read
  // (counted slot_no_count: the hook never sized it, so it is not in the record either). The 3-argument form is the old rule (no slots), kept for the twin.
  // Review D: a slot skeleton whose update hook is quiet is a target only while it is still KEPT (in cur_, or in last_ within kPruneAfter): one whose reads keep failing ages out of last_ and then leaves the
  // targets (counted slot_gone), so an unreadable [skel+200] cannot hold the stamp back for kKnownAge records.
  // `count` = false: the same targets with no census counted (Take's second ask of the same latch, review E).
  struct SlotSkel { uint32_t skel = 0; int32_t slot = -1; uint32_t shown_addr = 0; bool game_shown = true; };   // S103 DRL: shown_addr = the guest address of [char+232] (0 = unknown: a draw ask is then not filtered by it)   // S107 CAM: game_shown = the game's predicate sub_823CBC50 ([char+1208] && [char+224] && [char+232]); false = the game hid him: LatchTargets clears his liveness stamps at once
  enum SlotState : uint8_t { kSlotEmpty = 0, kSlotOccupied = 1, kSlotUnreadable = 2 };   // review A: per slot of the table, read at the latch
  struct SlotTable { std::vector<SlotSkel> skels; uint8_t state[9] = {}; std::vector<scene_inputs::CharPiece> pieces; std::vector<uint32_t> ring_models; const std::vector<scene_inputs::CharPiece>* pieces_view = nullptr; /* S103 HGF (XLB 3): when set, LatchTargets reads the pieces from here instead of `pieces` (the glue passes the inputs table's list without a deep copy) */ };   // S103 NRB: pieces = ONE shared type (slot_piece.h); NRBF: ring_models = models the ring family draws (left out of the wrestler objects)
  void LatchTargets(uint64_t hook_frame, const SlotTable* slots, std::vector<PaletteTarget>& out, uint32_t& missing, bool count = true);
  // S103 L31B (the interim guard): how many kept skeletons this record's Collect would emit with an OLD palette (no update tagged <= PickLimit(record_frame)) that are visible (a slot) AND still PRESENT.
  // Review A: presence is decided by SLOT, never by pointer: a stale visible body is present while its slot (the update hook's match) is occupied or unreadable in the table (a wrong [char+48] [L] or a
  // one-frame read failure can then never drop a wrestler while the frame goes ours); only an EMPTY slot means he left. null = unknown: every stale visible body counts. In mode Ours the caller withholds
  // kInWrestlerPalette when this is > 0, so the frame walks (the STREAM build; a build with no walk has no such frame: Collect holds the body for up to kConsumerFresh records instead, S103 AUSF, see SetNoWalk).
  uint32_t StaleVisibleAt(uint64_t record_frame, const SlotTable* slots);
  // S107 CAM: in the stream-free (player) build a refreshed slot body is visible ONLY while the game asked to draw it (draw_frame, NoteDrawAsked): the update hook says "posed", not "drawn" (Create a Moveset
  // poses two extra slot bodies it never draws). The paragraph below is the older rule, kept for the dev-only STREAM build (and the planted twin kFaultUpdShows). Also LatchTargets hides at once a body whose
  // game predicate (SlotSkel::game_shown = sub_823CBC50) is false.
  // Review C: a slot body whose update hook has been quiet longer than kConsumerFresh is emitted VISIBLE only while the game's own draw (the consumer hook sub_82C04DF0, SubmitConsumer) saw it within
  // kQuietSeenWindow hook frames: the early exit [this+220] == 0 && [this+296] == 0 may mean "not posed, not drawn" (a waiting opponent). Otherwise it is emitted invisible (a skinned skel: object =
  // rf_wrestlers' hole: the frame goes to the game) and counted quiet_unseen. The palette is still read.
  static constexpr uint64_t kQuietSeenWindow = 60;
  // S107 CAM: the replay track's pose hook (S103 RPF) is gone: the game ASKS to draw a replay body like any other (draw_frame, below), so a pose is no liveness signal.
  void StampDrawForTest(uint32_t skel, uint64_t frame);   // tests only: the find-only draw stamp NoteDrawAsked makes
  // S103 DRL (docs\research\s103\FRZ.md, wrestler_pose.md "fixed by DRL"): the game ASKS to draw the body (sub_82C04DF0 raw hook, read before the Ours return) even when nothing updated it (the post-match freeze
  // frame: upd_frame stops, no walk in the stream-free build). A draw ask for a known slot body stamps draw_frame: in the stream-free build the quiet gate's only term (S107 CAM), in the stream build one of three. Filter first, lock-free, against the latch's published slot skeletons
  // (<= kDrawSlots, LatchTargets); one check per body per frame; a character the game HIDES ([char+232] == 0) is not stamped and loses its stamp at once (a draw ask must not revive a hidden body).
  // `read_word(addr, v)` = the guest read (page-checked); an unreadable flag is not a stamp (counted).
  static constexpr uint32_t kDrawSlots = 9;
  static constexpr uint32_t kShownToSkeleton = 184;   // [char+232] (shown) - [char+48] (skeleton), light_latch.h kCharSkeletonOff
  template <class ReadWordFn>
  void NoteDrawAsked(uint32_t skel, uint64_t frame, ReadWordFn read_word) {
    if (!skel) return;
    if (frame > draw_pub_frame_.load(std::memory_order_relaxed) + kQuietSeenWindow) return;   // review 3: the published list is stale (latch skipped / scene over): a reused address cannot match
    uint32_t i = 0;
    uint32_t shown_addr = 0;
    for (; i < kDrawSlots; ++i) {
      const uint64_t pub = draw_pub_[i].load(std::memory_order_relaxed);   // review 2: ONE word per slot = (shown_addr << 32) | skel, so a pair is never torn
      if (pub != 0 && uint32_t(pub) == skel) { shown_addr = uint32_t(pub >> 32); break; }
    }
    if (i == kDrawSlots) return;   // not a slot body (the other ~95 asks of the frame): nothing more
    uint64_t prev = draw_checked_[i].load(std::memory_order_relaxed);   // review 4: once per body per frame, claimed by compare-exchange (two threads cannot both count it) [U: which threads call sub_82C04DF0 is not settled; the CAS makes either answer safe]
    if (prev >= frame || !draw_checked_[i].compare_exchange_strong(prev, frame, std::memory_order_relaxed)) return;
    // BHF1 B6: the shown flag is read UNDER mutex_ (HideLiveness takes the same lock after the game hid the character), so a hide always wins: the stamp either read the flag before the hide and the hide's clear follows it, or reads 0.
    // The flag read: 0 = shown word read and non-zero, 1 = unread, 2 = hidden. (BHF1 fix: the planted twin kFaultFlagReadUnlocked does it BEFORE the lock, the old code.)
    auto read_flag = [&]() -> int {
      if (!shown_addr) return 0;
      uint32_t shown = 0, owner = 0;
      // BHF1 (BH3-4): the published flag address can be up to kQuietSeenWindow frames old; the character must still own this skeleton ([char+48] == skel; shown = char+232) or the word is arbitrary: unread, never a stamp.
      if (!read_word(shown_addr - kShownToSkeleton, owner) || owner != skel) return 1;
      if (!read_word(shown_addr, shown)) return 1;
      return shown == 0 ? 2 : 0;
    };
    int flag = 0;
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    if (faults_ & kFaultFlagReadUnlocked) flag = read_flag();
    lock.lock();
    if (!(faults_ & kFaultFlagReadUnlocked)) flag = read_flag();
    auto kn = known_.find(skel);   // find, never create: only a body the update hook has met is a body
    if (kn == known_.end()) { draw_unknown_.fetch_add(1, std::memory_order_relaxed); return; }
    if (flag == 1) { draw_unread_.fetch_add(1, std::memory_order_relaxed); return; }
    if (flag == 2) { HideLiveness(kn->second); draw_hidden_.fetch_add(1, std::memory_order_relaxed); return; }
    kn->second.draw_frame = std::max(kn->second.draw_frame, frame);
    draw_asked_.fetch_add(1, std::memory_order_relaxed);
    // S108 THR (change_colour.h): the Threads editor's live colours, read at the game's own draw of this body (skel = the model it draws); the editor object is
    // re-read from [skel+328] here, never remembered. Outside mutex_ (the store has its own lock).
    lock.unlock();
    change_colour::Bodies().Note(skel, frame, change_colour::ReadDraw(read_word, skel));
  }
  uint64_t DrawAsked() const { return draw_asked_.load(std::memory_order_relaxed); }
  uint64_t QuietUnseen() const { return quiet_unseen_.load(std::memory_order_relaxed); }
  uint64_t SlotGone() const { return slot_gone_.load(std::memory_order_relaxed); }
  // S103 AUSF: that is the STREAM build's rule (its frame walks); with no walk a stale visible body still in the character table is HELD (stale_held, <= kConsumerFresh records), dropped only after it left the table or is older.
  // In mode Ours Collect never emits a VISIBLE kept skeleton (resolved slot >= 0) that was not refreshed for this record (it ages and is pruned as before); a stale INVISIBLE one (a piece, a skel:) is still
  // emitted, so rf_wrestlers counts it a hole (review B). Counted here.
  uint64_t StaleDropped() const { return stale_dropped_.load(std::memory_order_relaxed); }
  // The slot path's census (since start): slot targets read, slot skeletons with no count, slot targets the hook-liveness rule would have dropped (the run-30 case), and the join of the table's slot with the
  // update hook's slot match (char+176 == [skel+192] + 176): agree / differ / the hook never matched a slot.
  std::string SlotCensus() const;
  uint64_t SlotTargets() const { return slot_targets_.load(std::memory_order_relaxed); }
  uint64_t SlotHookQuiet() const { return slot_hook_quiet_.load(std::memory_order_relaxed); }
  uint64_t SlotNoCount() const { return slot_no_count_.load(std::memory_order_relaxed); }
  // test only: the planted twin "the guard off" (a stale body is emitted with its old palette in mode Ours, the old behaviour)
  void SetStaleGuardForTest(bool on) { stale_guard_ = on; }
  // test only: the review's planted twins (the old behaviours): A presence by pointer, B invisible stale dropped, C no walk gate, D gone slot bodies kept as targets
  enum : uint32_t { kFaultPointerPresence = 1, kFaultDropInvisible = 2, kFaultNoWalkGate = 4, kFaultKeepGoneTargets = 8, kFaultStrictDerived = 16, kFaultNoMismatchEvidence = 32 };   // 32 = review F3's twin: an OURS mismatch submit without the walk evidence   // S103 L33F: 16 = the old strict derived guard (every mismatch withholds)
  void SetFaultsForTest(uint32_t f) { faults_ = f; }
  // S103 PZ F5: 64 = the old rule (the declared link and its pieces live in the aged Known record and go with it): the run-36 hole, kept as the planted twin
  static constexpr uint32_t kFaultLinkInKnown = 64;
  static constexpr uint32_t kFaultHairCurrentSet = 8192;   // S103 HGF twin: the first HGF rule (rows of the CURRENT latch only): never true in the real order, so the hair vanished
  static constexpr uint32_t kFaultHairAnyFrame = 4096;   // S103 HGF planted twin: the old rule, hair rows attach to any fresh body palette whatever latch they were read in
  // BHF1 planted twins: 256 = the old sticky slot / no reset on a node-count change (BH2 G1); 512 = a draw-ask hide clears only draw_frame (BH4 item 6)
  static constexpr uint32_t kFaultKeepStaleKnown = 256, kFaultHideKeepsStamps = 512, kFaultFlagReadUnlocked = 1024, kFaultDetachNeedsFresh = 2048;   // 2048 = S103 DTP fix twin: the detach flag needs a FRESH piece palette and no link-only entry is written (the pad returns to the arm once its palette goes stale / is pruned)   // 1024 = B6: the shown flag is read before the lock (the old code)
  struct KnownProbe { int32_t slot = -1; uint32_t count = 0; uint64_t walk = 0, draw = 0, upd = 0; };
  bool ProbeForTest(uint32_t skel, KnownProbe& out) {   // tests: one Known record's slot / count / liveness stamps
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = known_.find(skel);
    if (it == known_.end()) return false;
    out = {it->second.slot, it->second.max_count, it->second.walk_frame, it->second.draw_frame, it->second.upd_frame};
    return true;
  }
  bool TryLockForTest() { if (!mutex_.try_lock()) return false; mutex_.unlock(); return true; }   // tests: is mutex_ free right now (B6: the flag read must hold it)
  uint64_t ReuseReset() const { return reuse_reset_.load(std::memory_order_relaxed); }
  static constexpr uint32_t kFaultNamedFailHole = 128;   // S103 DLW: the old RecordCanName ("has a key", not "the key loads") and an emitter that keeps naming a failed model: the DLC hole, kept as the planted twin
  // F5: the one-time line when a Known that carried a declared link is erased (settles PZ's open [U]: why two slot-6 characters give [char+48] = the referee's skeleton). (body, key, age in Collects at the erase, Collects since the link was made)
  using KnownEraseLogFn = void (*)(uint32_t, uint32_t, uint32_t, uint64_t);
  void SetKnownEraseLog(KnownEraseLogFn f) { known_erase_log_.store(f, std::memory_order_relaxed); }
  // S103 AUSF (AUS2 log lines 1-3): the stream-free build names a body it leaves out. kind: 0 = stale drop, 1 = latch read refused (once per run of refusals per skeleton), 2 = quiet hide. (kind, record/hook frame, skeleton, slot, a, b, c)
  // 0: a = age in records, b = last palette tag, c = hold frames still allowed (kConsumerFresh); 1: a = run start frame; 2: a = update-hook frame, b = draw-ask frame, c = replay pose frame. Each kind prints at most 64 lines a run (the glue's logger).
  using WrestlerEventLogFn = void (*)(int, uint64_t, uint32_t, int32_t, uint64_t, uint64_t, uint64_t);
  void SetWrestlerEventLog(WrestlerEventLogFn f) { wrestler_event_log_.store(f, std::memory_order_relaxed); }
  // S103 AUSF (AUS2): a build with NO command-stream walk (!WWE2K14_STREAM()) has no frame that "falls back to the game": the latch read of a slot-present body is SUBMITTED on a mismatch with no walk evidence
  // (the draw's own source, [model+200]), and a stale visible body still in the character table is held (drawn with its kept palette) for up to kConsumerFresh records, dropped only when it left the table. The default
  // follows the build; the unit rig flips it to prove both builds in one binary. The stream build keeps today's rules exactly.
  void SetNoWalk(bool v) { no_walk_ = v; }
  // S103 AUSF review 1: the latch's mismatch rule above is for reads made on the LATCH thread (the game parked). The off-latch fallback (Emit's missed-latch read, game threads running) calls this around its reads
  // (inputs_contrib.h SetOffLatch): a mismatch there stays refused (mismatch_no_evidence), as in the stream build. Per calling thread.
  static void SetOffLatchThread(bool v) { t_off_latch_ = v; }
  uint64_t MismatchSubmittedNoWalk() const { return mismatch_submitted_nowalk_.load(std::memory_order_relaxed); }
  uint64_t HeldWithExtras() const { return held_with_extras_.load(std::memory_order_relaxed); }   // S103 AUSF review 2: held stale bodies that kept their hair rows
  void SetPieceRowsForTest(uint32_t owner, uint32_t piece_id) { std::lock_guard<std::mutex> lock(mutex_); ExtraRow r; r.piece_id = piece_id; r.node = 1; r.piece_nodes = 2; test_rows_[owner].assign(1, r); }   // tests: rows Latch re-adds to piece_rows_ every latch
  uint64_t StaleHeld() const { return stale_held_.load(std::memory_order_relaxed); }
  static constexpr uint32_t kFaultHoldAny = 32768, kFaultHoldNoSlotCheck = 65536, kFaultHoldOffLatch = 131072, kFaultHoldNoExtras = 262144;   // S103 AUSF review twins: hold a hidden body / hold over a fresh slot claimant / submit an off-latch mismatch / hold without the hair rows
  static constexpr uint32_t kFaultNoWalkRefuse = 16384;   // S103 AUSF planted twin: today's rule in a no-walk build (a mismatch with no walk evidence is refused, a stale visible body is dropped at once): the vanishing wrestler
  uint64_t BodyLinkRevived() const { return body_link_revived_.load(std::memory_order_relaxed); }           // a body link used again after its Known record was gone (the address-reuse risk, counted)
  uint64_t BodyLinkDroppedCount() const { return body_link_dropped_count_.load(std::memory_order_relaxed); }  // dropped: the update hook reported another node count at the address
  uint64_t BodyLinkDroppedKey() const { return body_link_dropped_key_.load(std::memory_order_relaxed); }     // dropped: the slot key names another character
  uint64_t KnownErasedLinked() const { return known_erased_linked_.load(std::memory_order_relaxed); }
  size_t BodyLinkCount() { std::lock_guard<std::mutex> lock(mutex_); return body_links_.size(); }
  std::string BodyLinkText() const;   // "revived N dropped_count N dropped_key N known_erased N"
  // The palette the latch read, as the consumer palette of this frame. When the skeleton already holds a consumer update of this hook frame or later (the walk's own copy: mirror mode) NOTHING is
  // overwritten: the two are compared (returns kSame / kDiffers, counted: the T7 compare); else submitted (in place when the frame already has the update).
  enum LatchSubmitResult : int { kSubmitted = 0, kSame = 1, kDiffers = 2, kDerivedMismatch = 3 };   // 3: nodes 0-58 of the latch read differ from the update hook's derived palette AND (S103 L33F) from the walk's copy of the same frame: NOT submitted, the caller withholds the stamp. A mismatch with an equal walk copy returns kSame; with no walk copy (OURS) the read is submitted (kSubmitted) and only counted
  static constexpr uint32_t kGuardNodes = 59;
  // review must 7: once a mirror run's `delta 0/+1/-1` census settles which derived head the latch read equals, set this to that delta (latch hook frame - the head's tag); only that head is then accepted, so a
  // palette that matches an OLDER frame's head by chance no longer passes. 0x7FFFFFFF = not settled (any of the last three heads).
  static constexpr int64_t kDeltaUnsettled = 0x7FFFFFFF;
  void SetAcceptedHeadDelta(int64_t d) { accepted_delta_.store(d, std::memory_order_relaxed); }
  int SubmitLatchConsumer(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t hook_frame);
  uint64_t LatchSame() const { return latch_same_.load(std::memory_order_relaxed); }
  uint64_t LatchDiffers() const { return latch_differs_.load(std::memory_order_relaxed); }
  uint64_t DerivedMismatch() const { return derived_mismatch_.load(std::memory_order_relaxed); }
  // S103 L33F: of the derived mismatches, those the walk's copy of the same frame accepted (equal), refused (differs), and those submitted with no walk copy (OURS frames)
  uint64_t MismatchWalkSame() const { return mismatch_walk_same_.load(std::memory_order_relaxed); }
  uint64_t MismatchWalkDiffers() const { return mismatch_walk_differs_.load(std::memory_order_relaxed); }
  uint64_t MismatchSubmitted() const { return mismatch_submitted_.load(std::memory_order_relaxed); }
  uint64_t MismatchNoEvidence() const { return mismatch_no_evidence_.load(std::memory_order_relaxed); }   // review F3: an OURS mismatch with no walk evidence for this body: refused
  uint64_t MismatchNotPresent() const { return mismatch_not_present_.load(std::memory_order_relaxed); }   // a mismatch with no walk copy for a body the character table does not hold: refused
  uint64_t DerivedUnchecked() const { return derived_unchecked_.load(std::memory_order_relaxed); }
  std::string DerivedDeltaStr() const { return derived_delta_.Str(); }
  uint64_t DerivedChecked() const { return derived_checked_.load(std::memory_order_relaxed); }
  // S103 A: the skeleton (= model) addresses the wrestlers keep every frame (the kept set), for the latch reader of per-model runtime values (inputs_contrib.h). Short lock; `out` is cleared first.
  // S108 RDS: the game destroyed this model (the base destructor sub_82BD3B40, model_release_hook.h; reached from the deleting destructor sub_82BD99F8, the non-deleting sub_82BD9888 and the base class's
  // sub_82BD3BF0, always before the block is freed): every remembered row of the address goes, so no latch reads it after the free (the kept set, the pending updates, the Known record with its count and
  // palette pointer). A new model later allocated at the same address is a new body to us: the update hook registers it again.
  // The body links (body_links_) stay: the latch re-verifies them against the game's descriptors every latch (PZF2 A1).
  void ForgetModel(uint32_t model) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_.erase(model), cur_.erase(model), known_.erase(model), last_extra_.erase(model);
    released_.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t Released() const { return released_.load(std::memory_order_relaxed); }
  bool NamesModel(uint32_t model) {   // test / census: does any per-address container still hold it
    std::lock_guard<std::mutex> lock(mutex_);
    return last_.count(model) || cur_.count(model) || known_.count(model) || last_extra_.count(model);
  }
  void KeptSkeletons(std::vector<uint32_t>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(last_.size());
    for (const auto& kv : last_) out.push_back(kv.first);
  }
  // S103 V1F (F1, review): the skeletons the last Latch / Emit wrote VISIBLE (actor:slot:N, the first claimant of a slot), taken from the collected items with EmitItems' exact rule (sort by slot then skeleton,
  // first claim of s.slot >= 0), so a quiet-unseen body (slot -1) or a dropped stale body never hands its slot to the next claimant in this list. Used only by the light coverage (bit 4): a hidden body is not asked.
  void EmittedSkeletons(std::vector<uint32_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = visible_skels_;
  }
  // sub_82C04DF0 calls the hook could not turn into a palette: model never seen by the update hook (a piece of a known skeleton when its r5 equals
  // that skeleton's palette pointer: hair, pads, the map's slice 1b, counted apart), or r5 != [model+200].
  void NoteEntryUnknownModel(uint32_t model, uint32_t palette_ptr);
  void NoteEntryPtrMismatch() { entry_ptr_mismatch_.fetch_add(1, std::memory_order_relaxed); }
  void NoteEntryUnreadable() { entry_unreadable_.fetch_add(1, std::memory_order_relaxed); }
  // The +200 pointer a known skeleton's draws use (for the piece match).
  void NotePalettePtr(uint32_t skel, uint32_t palette_ptr);
  // Asset link of a skeleton. Declared = read from the char model reader's data; the record carries it. Strings are paths / names, not addresses.
  void SubmitAssetLink(uint32_t skel, const std::string& pac, const std::string& entry, uint32_t variant);
  // Fingerprint fallback: counted, kept for the draw side, never written to the record.
  void SubmitAssetGuess(uint32_t skel, const std::string& pac, const std::string& entry, uint32_t variant);
  struct Link { std::string pac, entry; uint32_t variant = 0; bool known = false; };
  Link GuessFor(uint32_t skel);
  static bool KeyFailed(uint32_t key, const std::string& composite_tag = std::string(), uint32_t model_desc = 0);   // S103 DLW: the loader failed this key's model (named_load_fail.h); S110 SEATREL: model_desc = the descriptor whose file note names the file
  // S103 DLW: the AssetRef of a model key (the one rule the emitter and RecordCanName share). S103 CMB: `composite_tag` is appended to `entry`, so a composite attire caches apart from the stock attire of the same loaded key.
  // S110 SEATREL: `model_desc` = the slot descriptor whose model the body is: the file is the one that descriptor's model was BUILT from (model_file_note.h), while it still holds `key`; 0 = the id rule.
  static Link KeyLink(uint32_t key, const std::string& composite_tag = std::string(), uint32_t model_desc = 0);
  // S103 B: the participant slot's key (entrant_slots A: P + 148 + 56 * slot, +16; match_setup.md, R1_wrestler_assets.md section 2): character * 100 + attire * 10 + kind, the model the
  // game builds for that character slot. The declared route (SubmitModelLink) never linked a wrestler in any capture; this names the model of a slot-matched skeleton from the setup table.
  // [U live: participant row == character slot is the R1 open point. Known to FAIL for slots 6-8 (referee / role-4 people are rows 40-42, db\ids\characters.json ch255, match_state.md) and in a
  // Rumble the rows rotate, so the hook uses the key only for character slot < 6 and only while every row 6..39 is empty. The one trace that settles it: a 1v1 and a tag run, the keys of rows 0..5
  // beside each slot's model address (the wrestler_hook census line); lodged in docs\renderer\WRESTLERS_RECORD.md.]
  // An empty / invalid key (0, 0xC800 = 51200, key / 100 >= 500) CLEARS it: a stale key must never outlive the slot (review B2 / B3).
  // S103 ARM2 F2: `slot_desc` = the slot manager's descriptor of this body (the character's descriptor by the game's index rule, D+7 else the slot index: slotkey::SlotDescriptor; desc+4 is never written for a character slot) when the hook found it and its occupant builds (attire_build.h SlotOwner), else 0: the build-step colours match only that descriptor.
  // S110 SEATREL: `model_desc` = the same slot descriptor by the game's index rule, found for EVERY keyed slot (slot_desc above only under the attire latch): the file the game's
  // model was built from is noted per descriptor (model_file_note.h), and the key's file is named from that note only while the descriptor still holds this key. 0 = unknown (the id rule).
  void SubmitSlotKey(uint32_t skel, uint32_t key, uint32_t slot_desc = 0, uint32_t model_desc = 0) {
    if (key == 0 || key / 100 >= 500) key = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (key) {   // S103 PZ F5: a slot key (slot < 6) naming another character than the declared link at this address: the address now holds another body: the link is dead
      auto b = body_links_.find(skel);
      if (b != body_links_.end() && CharacterOfKey(b->second.info.key) != CharacterOfKey(key) && !(faults_ & kFaultLinkInKnown)) {
        DropBodyLinkLocked(skel);
        body_link_dropped_key_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (Known* k = KnownFor(skel)) k->slot_key = key, k->slot_desc = key ? slot_desc : 0, k->model_desc = key ? model_desc : 0;
  }

  // ---- the declared route, read at the exit of sub_82720658 (R1_wrestler_assets.md section 1; model_hooks.h ModelLinkRead) ----
  // What the game's slot descriptor holds after the build [C]: key = desc+28 = character*100 + attire*10 + kind (kind = key % 10), body = desc+4 (the
  // drawn model = the skeleton object, the pairing key of the draw), pieces = 24-byte entries at [desc+12] (model at +4, u16 id at +8), the attire's
  // PACH buffer pointer and size (reader+68 / +72). The pac / entry NAME is [U] (R1 open): the record carries the key as `entry` and the attire as
  // `variant` of a pac-less AssetRef (pac 0 = unknown, drawers skip it), never a guessed pac path.
  struct PieceRef { uint32_t model = 0; uint32_t id = 0; };
  struct ModelKeyInfo {
    bool known = false;
    uint32_t key = 0, pach_buf = 0, pach_size = 0, desc = 0, chosen = 0;   // S103 ARM2 F1: chosen = desc+92 read at the link (0 = unknown: the build-step colours never match)
    std::vector<PieceRef> pieces;
    std::string tag;   // S103 CMB: the composite attire's entry tag (attire_compose.h EntryTag: "/a:<chosen>/c:<hex>"), empty = the plain base; lives and dies with this link
  };
  // PZF2 A2: `game_node_count` = the body's own node count read at the build ([model+300], piece_link.h GameNodeCountW; 0 = unknown) seeds BodyLink::seen_count, so a link whose address is reused by
  // another object BEFORE the update hook ever reports the body is dropped by the first report with another count (it used to adopt that other count).
  void SubmitModelLink(uint32_t body_model, uint32_t key, uint32_t pach_buf, uint32_t pach_size, uint32_t desc, std::vector<PieceRef>&& pieces, uint32_t game_node_count = 0, std::string composite_tag = std::string(), uint32_t chosen_key = 0);
  // PZF2 A1 (body links never age, so identity is checked at each latch against the game's persistent descriptors; inputs_contrib.h ReadSlotSkeletons): `ok` = exactly one descriptor has +4 == skel and
  // its +28 == the link's key. A link is NAMED (Collect) and its pieces proven only while its verdict for THIS latch is ok; a failure also drops the listed pieces (kept unlisted until it passes again).
  // Returns true when the verdict passed and the pieces were dropped (the caller re-reads them from [desc + 12] and calls RelistPieces). No link at `skel`: false, nothing happens.
  bool VerifyBodyLink(uint32_t skel, bool ok);
  void RelistPieces(uint32_t skel, std::vector<PieceRef>&& pieces);
  // PZF2 B-4: what the latch needs from the link and nothing else (no vector copy under the mutex). known = a link exists; verified = its verdict is ok for the current latch.
  struct BodyKey { bool known = false, verified = false; uint32_t desc = 0, key = 0; };
  BodyKey BodyKeyFor(uint32_t body_model);
  uint64_t BodyLinkUnverified() const { return body_link_unverified_.load(std::memory_order_relaxed); }   // verdicts that said "another body / key at this address"
  uint64_t BodyLinkRelisted() const { return body_link_relisted_.load(std::memory_order_relaxed); }       // piece lists re-read after a drop
  void NoteLinkNoBody() { link_no_body_.fetch_add(1, std::memory_order_relaxed); }
  void NoteLinkEmptyKey() { link_empty_key_.fetch_add(1, std::memory_order_relaxed); }
  void NoteLinkUnreadable() { link_unreadable_.fetch_add(1, std::memory_order_relaxed); }
  void NoteLinkBadPiece() { link_bad_piece_.fetch_add(1, std::memory_order_relaxed); }
  ModelKeyInfo ModelKeyFor(uint32_t body_model);   // the whole link (copies the piece list): tests and diagnostics; the latch uses BodyKeyFor
  uint32_t OwnerOfPiece(uint32_t piece_model);  // the body model whose descriptor lists it, 0 = none
  // S101 lane 206: lock-free "the game's own descriptor lists this model as a wrestler piece" (the skeleton update hook asks ~134 times a frame; OwnerOfPiece takes the mutex). A small
  // open-addressing table of atomics, written under mutex_ (SubmitModelLink / DropPiecesLocked), read with no lock: a hair / cap / glasses skeleton under MinNodes() that a body lists is kept.
  // The hook's size filter (lane 206): a skeleton is recorded when it has the body's size, or is under it but the game's descriptor lists it as a piece.
  bool KeepSkeleton(uint32_t self, uint32_t count) const { return count != 0 && (count >= MinNodes() || IsListedPiece(self) || IsWanted(self, count)); }
  // S101 lane 207: own-skeleton pieces (a wrestler's hair, elbow pads: skeleton object = draw object) are never in the descriptor's list. The scene core, which sees the draw object and its joined
  // model's node count, WANTS that (address, count): a small skeleton is kept when it matches exactly. A separate lock-free table (DropPiecesLocked zeroes listed_).
  void WantSkeleton(uint32_t model, uint32_t count) {
    if (model <= 1 || !count) return;
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t free_at = -1;
    uint32_t at = ListHash(model);
    for (uint32_t i = 0; i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
      const uint64_t v = wanted_[at].load(std::memory_order_relaxed);
      if (v > 1 && uint32_t(v >> 32) == model) { wanted_[at].store((uint64_t(model) << 32) | count, std::memory_order_relaxed); return; }
      if (v == 1 && free_at < 0) free_at = int32_t(at);
      if (v == 0) { if (free_at < 0) free_at = int32_t(at); break; }
    }
    if (free_at < 0) { wanted_full_.fetch_add(1, std::memory_order_relaxed); return; }
    wanted_[uint32_t(free_at)].store((uint64_t(model) << 32) | count, std::memory_order_relaxed);
    wanted_n_.fetch_add(1, std::memory_order_relaxed);
  }
  void UnwantSkeleton(uint32_t model) {
    if (model <= 1) return;
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_hook_saw_.erase(model); own_skel_logged_.erase(model);  // review N3: bounded, and a reused address logs again
    uint32_t at = ListHash(model);
    for (uint32_t i = 0; i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
      const uint64_t v = wanted_[at].load(std::memory_order_relaxed);
      if (v == 0) return;
      if (v > 1 && uint32_t(v >> 32) == model) { wanted_[at].store(1, std::memory_order_relaxed); wanted_n_.fetch_sub(1, std::memory_order_relaxed); return; }
    }
  }
  void ClearWants() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& a : wanted_) a.store(0, std::memory_order_relaxed);
    wanted_n_.store(0, std::memory_order_relaxed);
    wanted_hook_saw_.clear(); own_skel_logged_.clear();  // review N3
  }
  bool IsWanted(uint32_t model, uint32_t count) const {
    if (model <= 1) return false;
    for (uint32_t i = 0, at = ListHash(model); i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
      const uint64_t v = wanted_[at].load(std::memory_order_relaxed);
      if (v == 0) return false;
      if (v > 1 && uint32_t(v >> 32) == model) return uint32_t(v) == count;
    }
    return false;
  }
  // S101 lane 209: the wanted node count for this object (0 = not wanted). Lock-free; the palette hook (sub_82C04DF0) uses it for own-skeleton pieces the update hook never reports under that key.
  uint32_t WantedCount(uint32_t model) const {
    if (model <= 1) return 0;
    for (uint32_t i = 0, at = ListHash(model); i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
      const uint64_t v = wanted_[at].load(std::memory_order_relaxed);
      if (v == 0) return 0;
      if (v > 1 && uint32_t(v >> 32) == model) return uint32_t(v);
    }
    return 0;
  }
  // Update-hook side (OnSkelIn): a wanted address reached the hook. Returns true the first time, when its count differs from the wanted one (the caller logs it once).
  bool NoteWantedHookSaw(uint32_t self, uint32_t count, uint32_t wanted) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool first = wanted_hook_saw_.emplace(self, count).second;
    return first && count != wanted;
  }
  bool WantedHookSaw(uint32_t model) {
    std::lock_guard<std::mutex> lock(mutex_);
    return wanted_hook_saw_.count(model) != 0;
  }
  // Palette-hook side: true when r5 is another known skeleton's palette pointer (a name-mapped borrower: stays on the piece route).
  bool IsBorrowedPalette(uint32_t model, uint32_t palette_ptr) {
    if (!palette_ptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : known_)
      if (kv.first != model && kv.second.palette_ptr == palette_ptr) return true;
    return false;
  }
  // First time a wanted model reaches the palette hook's own-skeleton branch: true once per model (the caller logs it).
  bool FirstOwnSkelEntry(uint32_t model) {
    std::lock_guard<std::mutex> lock(mutex_);
    return own_skel_logged_.insert(model).second;
  }
  void NoteOwnSkelConsumer(bool ptr_mismatch) {
    own_skel_consumer_.fetch_add(1, std::memory_order_relaxed);
    if (ptr_mismatch) own_skel_ptr_mismatch_.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t OwnSkelConsumer() const { return own_skel_consumer_.load(std::memory_order_relaxed); }
  uint64_t OwnSkelPtrMismatch() const { return own_skel_ptr_mismatch_.load(std::memory_order_relaxed); }
  // Optional log sink for the one-time own-skeleton line (installed by the game-side hook; null in tests): model, r5, [model+200] (0 unreadable), count, update hook saw the address.
  using OwnSkelLogFn = void (*)(uint32_t, uint32_t, uint32_t, uint32_t, bool);
  // S103 NRB: the one-time line per SUBM piece instance: (model, owner slot, piece id, owner skeleton, the count the scene core wanted for that model (0 = never recorded as its own skeleton))
  using PieceLinkLogFn = void (*)(uint32_t, int32_t, uint32_t, uint32_t, uint32_t, scene_inputs::PieceWhy, scene_inputs::PieceBuild, uint32_t);   // + the reader's proof, how the game built it (B1 / B2 / deferred, PZF2 B-3), the model kind from the latch's descriptor read (0xFFFFFFFF = not read)
  void SetPieceLinkLog(PieceLinkLogFn f) { piece_link_log_.store(f, std::memory_order_relaxed); }
  uint64_t PieceLinksMade() const { return piece_links_made_.load(std::memory_order_relaxed); }
  // S103 DTP: one line per change of a piece's owner-copy flag (SUBM instance +24: 1 -> 0 = the game detached it, 0 -> 1 = it joined its owner again): (model, owner slot, piece id, owner skeleton, copy flag NOW (0 / 1), frame)
  using PieceDetachLogFn = void (*)(uint32_t, int32_t, uint32_t, uint32_t, uint32_t, uint64_t);
  void SetPieceDetachLog(PieceDetachLogFn f) { piece_detach_log_.store(f, std::memory_order_relaxed); }
  uint64_t PieceDetached() const { return piece_detached_.load(std::memory_order_relaxed); }   // S103 DTP: per emit, pieces flagged kObjDetachedPiece
  // S103 NRBF: per emitted record, how each linked piece came out: Ok = drawn by its owner's body (not a hole); every other PieceWhy = refused (a hole while the game draws it). Indexed by PieceWhy.
  uint64_t RingLeftOut() const { return ring_left_out_.load(std::memory_order_relaxed); }   // S103 NRBF: emits skipped because the skeleton is a ring model
  uint64_t PieceByOwner() const { return piece_why_[0].load(std::memory_order_relaxed); }
  uint64_t PieceWhyCount(scene_inputs::PieceWhy w) const { return piece_why_[size_t(w)].load(std::memory_order_relaxed); }
  std::string PieceWhyText() const;   // "ok N no_count N ..." (the 600-frame wrestlers line)
  // S105 A5a: the own-rows route (a linked own-skeleton piece of a guest-row owner, drawn from its own latch-read palette). Test twins: 524288 = the latch accepts no piece read (the old refusal: the piece keeps its derived palette),
  // 1048576 = no copy proof in EmitItems (the old rule's hole reappears as a WRONG pose: any guest-owner piece with a link is drawn from whatever palette it has). The latch's acceptance is `piece_latch_submitted`.
  static constexpr uint32_t kFaultNoPieceLatch = 524288, kFaultNoCopyProof = 1048576, kFaultNoPrevRows = 2097152, kFaultRotationOnly = 4194304;   // S105 A5a F5 twins: the owner's previous palette is not offered / the translation is not compared (the first rule)
    static constexpr uint32_t kFaultHookOnlyPieces = 8388608, kFaultReaderIgnoresLinks = 16777216, kFaultPushEverything = 33554432;   // S105 CAWP twin: the old rule, a linked piece is a latch target only while the update hook reports it
    static constexpr uint32_t kFaultUpdShows = 67108864;   // S107 CAM twin: the old stream-free rule, a body the skeleton update posed (upd_frame) shows with no draw ask: the Create a Moveset extra body
    uint64_t PieceLatchSubmitted() const { return piece_latch_submitted_.load(std::memory_order_relaxed); }
  uint64_t OwnRowsCount(scene_inputs::OwnRowsWhy w) const { return own_rows_[size_t(w)].load(std::memory_order_relaxed); }
  std::string OwnRowsText() const;   // "ok N hidden_by_game N ..." + piece_latch_submitted (the 600-frame wrestlers line)
  void SetOwnSkelLog(OwnSkelLogFn f) { own_skel_log_.store(f, std::memory_order_relaxed); }
  void LogOwnSkel(uint32_t model, uint32_t r5, uint32_t field, uint32_t count) {
    if (OwnSkelLogFn f = own_skel_log_.load(std::memory_order_relaxed)) f(model, r5, field, count, WantedHookSaw(model));
  }
  uint64_t Wanted() const { return wanted_n_.load(std::memory_order_relaxed); }
  uint64_t WantedFull() const { return wanted_full_.load(std::memory_order_relaxed); }
  bool IsListedPiece(uint32_t model) const {
    if (model <= 1) return false;
    for (uint32_t i = 0, at = ListHash(model); i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
      const uint32_t v = listed_[at].load(std::memory_order_relaxed);
      if (v == 0) return false;
      if (v == model) return true;
    }
    return false;
  }
  static uint32_t AttireOfKey(uint32_t key) { return (key % 100) / 10; }
  static uint32_t CharacterOfKey(uint32_t key) { return key / 100; }
  static uint32_t KindOfKey(uint32_t key) { return key % 10; }
  uint64_t LinkBodies() const { return link_bodies_.load(std::memory_order_relaxed); }
  uint64_t LinkPieces() const { return link_pieces_.load(std::memory_order_relaxed); }
  uint64_t LinkNoBody() const { return link_no_body_.load(std::memory_order_relaxed); }
  uint64_t LinkEmptyKey() const { return link_empty_key_.load(std::memory_order_relaxed); }
  uint64_t LinkUnreadable() const { return link_unreadable_.load(std::memory_order_relaxed); }
  void NoteLinkCountRefused() { link_count_refused_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t LinkCountRefused() const { return link_count_refused_.load(std::memory_order_relaxed); }
  // Linked bodies (desc+4) later seen as a skeleton by the pose hooks: the model == skeleton identity ([L] until this counts as many as LinkBodies).
  // PZF2 A5 (semantics changed): the flag lives in the Known record, the link outlives it (PZ F5), so a body whose Known aged out and came back counts AGAIN, and a link stored while the Known table was
  // full (SubmitModelLink no longer refuses it) counts when its Known first exists. It is a count of (link, Known) meetings, not of distinct links.
  uint64_t LinkedSeenAsSkeleton() const { return linked_seen_as_skel_.load(std::memory_order_relaxed); }
  size_t KnownCount() { std::lock_guard<std::mutex> lock(mutex_); return known_.size(); }
  uint64_t LinkBadPiece() const { return link_bad_piece_.load(std::memory_order_relaxed); }
  uint64_t LinkKind(uint32_t kind) const { return kind < 10 ? link_kind_[kind].load(std::memory_order_relaxed) : 0; }
  uint64_t EntryPiecesByDescriptor() const { return entry_pieces_desc_.load(std::memory_order_relaxed); }
  // Totals for the log line / tests.
  uint64_t ConsumerSubmits() const { return consumer_submits_.load(std::memory_order_relaxed); }
  uint64_t EntryUnknownModel() const { return entry_unknown_model_.load(std::memory_order_relaxed); }
  uint64_t EntryPieces() const { return entry_pieces_.load(std::memory_order_relaxed); }
  uint64_t EntryPtrMismatch() const { return entry_ptr_mismatch_.load(std::memory_order_relaxed); }
  uint64_t EntryUnreadable() const { return entry_unreadable_.load(std::memory_order_relaxed); }
  uint64_t GuessedLinks() const { return guessed_links_.load(std::memory_order_relaxed); }
  uint64_t DerivedFallbacks() const { return derived_fallbacks_.load(std::memory_order_relaxed); }
  void NoteSkippedSmall() { skipped_small_.fetch_add(1, std::memory_order_relaxed); }
  void NoteUnreadable() { unreadable_.fetch_add(1, std::memory_order_relaxed); }

  // ---- settings ----
  void SetMinNodes(uint32_t n) { min_nodes_.store(n, std::memory_order_relaxed); }
  uint32_t MinNodes() const { return min_nodes_.load(std::memory_order_relaxed); }
  // The light set the wrestlers use (index into the record's Light::set): "light set 10 in a match" (arena_look.md) comes from the arena
  // contributor's table by id; whoever wires the classes calls this with that id. kUnknown until then (counted as unknown).
  void SetLightSet(uint32_t set) { light_set_.store(set, std::memory_order_relaxed); }

  // ---- test only: the planted fault "the palette of the FIRST update of the frame" (a pose one step behind) ----
  void SetFirstWinsForTest(bool v) { first_wins_ = v; }
  // Totals since start (for the log line / tests).
  uint64_t CapRefused() const { return cap_refused_.load(std::memory_order_relaxed); }   // lane 206: skins refused by the skeleton cap (incl. a small skin refused to keep body headroom)
  uint64_t ListedFull() const { return listed_full_.load(std::memory_order_relaxed); }   // lane 206: piece models the lock-free table could not take
  uint64_t SkippedSmall() const { return skipped_small_.load(std::memory_order_relaxed); }

 private:
  struct Upd {
    uint64_t frame = 0;
    int32_t slot = -1;
    bool consumer = false;                     // read at sub_82C04DF0 (exact) rather than derived at the exit of sub_82BD30E8
    std::vector<Mat4> skin;
  };
  struct Pending { std::vector<Upd> upds; };  // newest last, at most kMaxPending
  struct Kept {                                // the persistent snapshot: emitted every frame until pruned
    int32_t slot = -1;
    uint64_t frame = 0;                        // S103 HGF: the stream tag of the update this palette is (hook frame + kPaletteLead for a latch consumer palette)
    bool consumer = false;
    std::vector<Mat4> skin;
    uint32_t age = 0;                          // Emits since it was last refreshed
    // S105 A5a F5: the palette this one replaced (the owner's PREVIOUS latch read: a piece's owner-row copy may run before the owner's own update of the pass, so it holds one copy behind) and the shifts (sub_82BD3620) recorded for each
    std::vector<Mat4> prev_skin;
    bool prev_consumer = false;
    float shift[3] = {0, 0, 0}, prev_shift[3] = {0, 0, 0};
  };
  static constexpr size_t kMaxPending = 4;
  // bound on distinct skeletons held (hooks cannot grow memory without limit). Lane 206 (review S1): 96, since listed piece skeletons (hair, caps: up to ~15 a body) are recorded now; the last
  // kBodyHeadroom places are for skeletons of body size, so first-come pieces cannot push a later body out.
 public:
  static constexpr size_t kMaxSkeletons = 96;
  static constexpr size_t kBodyHeadroom = 16;
 private:
 public:
  static constexpr uint32_t kPruneAfter = 120; // Emits without an update before a skeleton leaves the record
 private:
  void Put(uint32_t skel, int32_t slot, uint64_t frame, std::vector<Mat4>&& skin, bool consumer = false, bool walk = false);   // walk: the game's draw (review C)
  bool PutConsumerInPlace(uint32_t skel, uint64_t frame, const Mat4* skin, uint32_t count, bool walk = false);   // S102 U3: Put's consumer path when the frame's update exists (false = nothing touched, use Put)
  struct Known {
    int32_t slot = -1;
    uint32_t max_count = 0;  // the LATEST full count the update hook saw (not a high-water mark: a reused address must not keep a stale count)
    uint32_t palette_ptr = 0;
    Link declared, guessed;
    uint64_t touch = 0;         // monotonic use stamp (eviction when the table is full)
    uint32_t age = 0;            // Emits since it was in last_ / cur_ (pruned past kKnownAge)
    bool seen_as_skeleton = false;
    bool has_consumer = false;   // a consumer palette arrived: derived palettes are bookkeeping only while it is fresh (kConsumerFresh frames)
    uint64_t consumer_frame = 0;
    float shift[3] = {0, 0, 0};   // S105 A5a F5: the sum of the ApplyDelta shifts since the last derived update (the game adds them to +200 after the piece copy)
    bool mismatch_walk_ok = false;   // S103 L33F review F3: the last derived mismatch WITH a walk copy had that copy equal to the latch read (set) / different (cleared): the evidence an OURS-frame mismatch submit needs
    // S103 A3 (audit item 2, F3): nodes 0..58 of the last THREE derived palettes the update hook reported, each with its tag (the guard matches the latch's [skel+200] read against each and censuses the
    // matching delta: the update hook's frame tag and the latch's differ by a clock the census must settle)
    struct Head { uint64_t frame = ~0ull; std::vector<Mat4> m; };
    Head heads[3];
    uint32_t head_next = 0;
    uint64_t upd_frame = 0;      // S103 A2: the newest hook frame the UPDATE hook (derived palette) reported this skeleton; the latch reader's liveness signal
    uint64_t walk_frame = 0;     // S103 L31B review C: the newest tag the GAME'S DRAW (SubmitConsumer, sub_82C04DF0) read this skeleton; the latch's own reads never set it
    uint64_t draw_frame = 0;     // S103 DRL: the newest hook frame the game ASKED to draw this skeleton (sub_82C04DF0 raw hook) while its character was shown; the stream-free build's ONLY liveness signal (S107 CAM: upd_frame is the latch reader's signal, not a visibility one)
    uint32_t slot_desc = 0;      // S103 ARM2 F2: the descriptor the hook proved is this slot's (0 = unknown)
    uint32_t model_desc = 0;     // S110 SEATREL: the slot's descriptor for the file note (SubmitSlotKey); 0 = unknown
    uint32_t slot_key = 0;       // S103 B: the model key of the participant slot the skeleton matched (SubmitSlotKey); 0 = none
  };
 public:
  static constexpr uint32_t kKnownAge = 1800;      // Emits without the skeleton in the record before its bookkeeping is dropped
  static constexpr uint64_t kConsumerFresh = 4;    // frames: a derived palette within this of the newest consumer read never competes with it
  // S103 W1 (docs\systems\wrestler_pose.md "The palette leads the stream by one frame"): MEASURED on run 26's case (tools\wrestler\w1_skin_check.cpp): the palette a record j holds is the one the game's draws of stream frame
  // j + 1 read, bit for bit (31464 of 31464 palette slots at record f - 1 for stream f; at record f every slot differs, mean 9-20 game units per vertex). A palette tagged t (the walk's copy, the update hook's derived
  // palette, the latch read tagged hook + kPaletteLead) is the content of stream t + 1, so a record j (paired with stream j) takes the newest tag <= j - kPaletteLead, never <= j.
  static constexpr uint64_t kPaletteLead = 1;
  static uint64_t PickLimit(uint64_t record_frame) { return record_frame == 0 ? ~0ull : (record_frame >= kPaletteLead ? record_frame - kPaletteLead : 0); }   // S103 W1
 private:
  uint64_t touch_seq_ = 0;
  std::unordered_map<uint32_t, uint32_t> piece_owner_;  // piece model -> body model (built by SubmitModelLink; O(1) for the entry hook)
  // S103 PZ F5 (docs\research\s103\PZ_piece_holes.md section 5/6): the declared model link lives with the BODY (keyed by its guest address), not with the aged Known record: run 36's referee body
  // left the record for 2078 frames at the entrance -> match load, its Known aged out (kKnownAge) and took the link and its pieces with it, and nothing re-submits it (the build hook fires once).
  // Replaced by the next SubmitModelLink at that address; dropped when the update hook reports that address with another node count (another object), when a slot key (slot < 6) names another
  // character, or (bounded table) when it is the least recently used. RISK [U]: a deferred body built later at a freed linked address with the same node count inherits the name; slots 6-8 have
  // no slot key to cross-check, so address reuse is only counted (body_link_revived_: a link used again after its Known was gone).
  struct BodyLink {
    ModelKeyInfo info;
    uint64_t touch = 0;       // LRU stamp (bound kMaxBodyLinks)
    uint64_t made_seq = 0;    // collect_seq_ when linked
    uint32_t seen_count = 0;  // the body's node count: the game's own ([model+300]) at the build (PZF2 A2), else the first update-hook count seen at this address after the link (0 = not yet known)
    uint64_t verified_seq = ~0ull;   // PZF2 A1: collect_seq_ of the latch whose descriptor check passed (~0 = never / failed): the link is named and its pieces proven only while this == collect_seq_
    bool pieces_dropped = false;     // PZF2 A3: the pieces were dropped (Known aged out, or a failed verdict): re-read from [desc + 12] when the verdict passes
  };
  static constexpr size_t kMaxBodyLinks = 4 * kMaxSkeletons;
  std::map<uint32_t, BodyLink> body_links_;
  uint64_t collect_seq_ = 0;   // Collects so far (under mutex_)
  std::atomic<uint64_t> body_link_revived_{0}, body_link_dropped_count_{0}, body_link_dropped_key_{0}, known_erased_linked_{0}, body_link_unverified_{0}, body_link_relisted_{0};
  void DropBodyLinkLocked(uint32_t body);   // pieces out of piece_owner_ / the lock-free list, then the link itself
  static constexpr uint32_t kListSlots = 4096;   // power of two; 0 empty, 1 removed (a guest address is never 0 or 1)
  static uint32_t ListHash(uint32_t m) { return (m * 2654435761u >> 12) & (kListSlots - 1); }
  std::atomic<uint32_t> listed_[kListSlots] = {};
  std::atomic<uint64_t> listed_full_{0}, cap_refused_{0};
  std::atomic<uint64_t> wanted_[kListSlots] = {};   // lane 207: (model << 32 | count); 0 empty, 1 removed
  std::atomic<uint64_t> wanted_n_{0}, wanted_full_{0};
  std::map<uint32_t, uint32_t> wanted_hook_saw_;      // lane 209 (mutex_): wanted address -> first count the update hook saw
  std::set<uint32_t> own_skel_logged_;               // lane 209 (mutex_)
  std::atomic<uint64_t> own_skel_consumer_{0}, own_skel_ptr_mismatch_{0};
  std::atomic<OwnSkelLogFn> own_skel_log_{nullptr};
  // S103 NRB: the SUBM piece links of the last LatchTargets(slots) (model -> owner slot, piece id, owner skeleton); under mutex_. Replaced whole every latch that reads the table.
  struct PieceLink { int32_t slot = -1; uint32_t id = 0, owner = 0; scene_inputs::PieceWhy why = scene_inputs::PieceWhy::NoCount; scene_inputs::PieceBuild build = scene_inputs::PieceBuild::Unknown; uint32_t kind = 0xFFFFFFFFu; scene_inputs::OwnRowsProbe probe; uint32_t count = 0; };   // S105 A5a: probe = the latch's read of the copy words + one mapped node pair; S105 CAWP: count = the node count the reader sized the piece with (CharPiece::nodes, 0 = none)
  std::unordered_map<uint32_t, PieceLink> piece_links_;
  // S103 W2B (bone_tag_rows.h, docs\agents\s103\W2.md): the spring-hair rows of the pieces the reader proved Ok, per OWNER skeleton, in canonical order (piece id ascending, then node); under mutex_, replaced whole by every LatchTargets(slots).
  struct ExtraRow { uint32_t piece_id = 0, node = 0, piece_nodes = 0; Mat4 m = {}; };
  std::unordered_map<uint32_t, std::vector<ExtraRow>> piece_rows_;
  std::unordered_map<uint32_t, std::vector<ExtraRow>> piece_rows_prev_;   // S103 HGF (review 1): the PREVIOUS latch's rows (the body palette Collect picks is the previous latch's: tag = its hook + kPaletteLead <= record - kPaletteLead)
  uint64_t piece_rows_prev_frame_ = ~0ull;
  uint64_t piece_rows_frame_ = ~0ull;   // S103 HGF: the hook frame of the latch that read piece_rows_ (a body palette that is that latch's consumer update is tagged piece_rows_frame_ + kPaletteLead)
  std::map<uint32_t, uint32_t> piece_copy_logged_;   // S103 DTP fix: a piece model -> its last logged raw owner-copy word (CharPiece::copy)
  std::map<uint32_t, scene_inputs::PieceWhy> piece_link_logged_;  // a model logged once per proof it had (bounded by the table: erased when it leaves)
  std::atomic<PieceLinkLogFn> piece_link_log_{nullptr};
  std::atomic<PieceDetachLogFn> piece_detach_log_{nullptr};   // S103 DTP
  std::atomic<uint64_t> piece_detached_{0};                   // S103 DTP
  std::atomic<uint64_t> piece_offlatch_refused_{0};   // S105 CAWP F3
  std::atomic<uint64_t> piece_latch_submitted_{0};            // S105 A5a: a SUBM-linked piece's latch read accepted without the derived-head evidence (its owner is slot-present at this latch)
  std::atomic<uint64_t> own_rows_[size_t(scene_inputs::OwnRowsWhy::kCount)] = {};   // S105 A5a: per emit, how each candidate own-skeleton piece of a guest-row owner came out (index 0 = Ok = written as kObjOwnRowsPiece)
  std::atomic<KnownEraseLogFn> known_erase_log_{nullptr};
  std::atomic<WrestlerEventLogFn> wrestler_event_log_{nullptr};
  static inline thread_local bool t_off_latch_ = false;   // S103 AUSF review 1
  std::unordered_map<uint32_t, std::vector<ExtraRow>> test_rows_;   // tests only (SetPieceRowsForTest)
  std::unordered_map<uint32_t, std::vector<ExtraRow>> last_extra_;   // S103 AUSF review 2: the spring-hair rows last attached to a FRESH item of this skeleton (a held stale body keeps them); under mutex_
  bool no_walk_ = !WWE2K14_STREAM();   // S103 AUSF: see SetNoWalk
  std::atomic<uint64_t> mismatch_submitted_nowalk_{0}, stale_held_{0}, held_with_extras_{0};
  static constexpr int kSlotBins = 10;   // slots 0-8 and bin 9 = no slot
  std::atomic<uint64_t> by_slot_stale_dropped_[kSlotBins] = {}, by_slot_quiet_unseen_[kSlotBins] = {}, by_slot_no_evidence_[kSlotBins] = {};   // S103 AUSF: the census says WHOSE body
  std::map<uint32_t, uint64_t> quiet_last_;   // S103 AUSF: skeleton -> record frame of its last quiet hide (a run logs once); under mutex_
  std::map<uint32_t, uint64_t> refuse_last_;   // S103 AUSF: skeleton -> hook frame of its last refusal (a run of refusals logs once); under mutex_
  static int SlotBin(int32_t slot) { return slot >= 0 && slot < 9 ? int(slot) : 9; }
  void EventLog(int kind, uint64_t frame, uint32_t skel, int32_t slot, uint64_t a, uint64_t b, uint64_t c) const { if (WrestlerEventLogFn f = wrestler_event_log_.load(std::memory_order_relaxed)) f(kind, frame, skel, slot, a, b, c); }
  std::vector<uint32_t> ring_models_;   // S103 NRBF (under mutex_): the ring's model pointers of the last LatchTargets(slots), sorted
  std::atomic<uint64_t> ring_left_out_{0};
  std::atomic<uint64_t> piece_links_made_{0};
  std::atomic<uint64_t> piece_why_[size_t(scene_inputs::PieceWhy::kCount)] = {};   // S103 NRBF: per emit, by reason (index 0 = Ok = drawn by its owner)
  void ListAddLocked(uint32_t model);
  void ListRemoveLocked(uint32_t model);
  void DropPiecesLocked(uint32_t body);
  Known* KnownFor(uint32_t skel, bool count_revival = true);  // mutex_ held; null when the table is full (bounded like cur_)

  std::atomic<PreLatchFn> pre_latch_{nullptr};   // S103 A2
  std::atomic<int64_t> accepted_delta_{kDeltaUnsettled};   // the game sets the settled value at registration (inputs_glue.h Register)
  std::atomic<uint64_t> latch_same_{0}, latch_differs_{0}, derived_mismatch_{0}, derived_checked_{0}, derived_unchecked_{0};
  std::atomic<uint64_t> mismatch_walk_same_{0}, mismatch_walk_differs_{0}, mismatch_submitted_{0}, mismatch_not_present_{0}, mismatch_no_evidence_{0};   // S103 L33F
  std::vector<uint32_t> slot_present_;   // S103 L33F: the slot skeletons of the last LatchTargets(slots) (sorted; under mutex_); empty on the old 3-argument path
  DeltaCensus derived_delta_;   // latch hook frame - the matching derived head's tag
  std::mutex mutex_;
  std::atomic<uint64_t> released_{0};   // S108 RDS: ForgetModel calls (the release hook)
  std::map<uint32_t, Pending> cur_;   // by skeleton address: updates not yet merged
  std::map<uint32_t, Kept> last_;     // by skeleton address: what Emit writes
  std::map<uint32_t, Known> known_;   // by skeleton address: slot, node count, palette pointer, asset links
  std::atomic<uint32_t> min_nodes_{kDefaultMinNodes};
  std::atomic<uint32_t> light_set_{kUnknown};
  std::atomic<uint64_t> skipped_small_{0}, unreadable_{0};
  std::atomic<uint64_t> consumer_submits_{0}, entry_unknown_model_{0}, entry_pieces_{0}, entry_ptr_mismatch_{0}, entry_unreadable_{0}, guessed_links_{0},
      derived_fallbacks_{0}, link_bodies_{0}, link_pieces_{0}, link_no_body_{0}, link_empty_key_{0}, link_unreadable_{0}, link_bad_piece_{0},
      entry_pieces_desc_{0}, link_count_refused_{0}, linked_seen_as_skel_{0};
  std::atomic<uint64_t> link_kind_[10] = {};
  bool first_wins_ = false;
  bool stale_guard_ = true;   // S103 L31B
  uint32_t faults_ = 0;       // S103 L31B review (test only)
  std::atomic<uint64_t> draw_pub_[kDrawSlots] = {};   // S103 DRL: per slot (shown_addr << 32) | skeleton, 0 = none (LatchTargets publishes)
  std::atomic<uint64_t> draw_pub_frame_{0};           // the hook frame of the last publish: a list older than kQuietSeenWindow is ignored
  std::atomic<uint64_t> draw_checked_[kDrawSlots] = {};
  rdr_scene::live::LockStats want_lock_;   // BHF1 B2: the cost of the lock WantCount takes on every model call (census: want_lock)
  std::atomic<uint64_t> draw_asked_{0}, draw_hidden_{0}, draw_unread_{0}, draw_unknown_{0};
  std::atomic<uint64_t> reuse_reset_{0};   // BHF1 G1: Known records reset because the update hook reported another node count at the address
  void HideLiveness(Known& k) { k.draw_frame = 0; if (!(faults_ & kFaultHideKeepsStamps)) k.walk_frame = 0; }   // BHF1 (BH4 6): a hidden character keeps no walk / draw stamp (upd_frame stays: the update hook reports what it updates)
  std::atomic<uint64_t> quiet_unseen_{0}, slot_gone_{0};   // S103 L31B review C / D
  std::atomic<uint64_t> stale_dropped_{0}, slot_targets_{0}, slot_no_count_{0}, slot_hook_quiet_{0}, slot_join_agree_{0}, slot_join_differ_{0}, slot_join_unmatched_{0};   // S103 L31B
  // ---- S102 U1: the latch (docs\renderer\SWAP_TAIL.md). Collect = Emit's two locked blocks, unchanged; the formatting reads only the items. ----
  struct EmitItem { uint32_t skel; int32_t slot; bool consumer; Link link; std::vector<Mat4> skin; bool stale = false; int32_t piece_slot = -1; uint32_t piece_owner = 0; scene_inputs::PieceWhy piece_why = scene_inputs::PieceWhy::NoCount; uint32_t piece_id = 0; bool link_only = false; std::vector<ExtraRow> extra; uint64_t tag = 0; scene_inputs::OwnRowsProbe probe; std::vector<Mat4> prev_skin; bool prev_consumer = false; float shift[3] = {0, 0, 0}, prev_shift[3] = {0, 0, 0}; bool cc_on = false, cc_unsettled = false; uint8_t cc_colours[48] = {}; };   // S108 THR2: cc_* = the body's change-colour state (Bodies().Get in Collect): section 67   // S105 A5a F5: the owner's previous palette + the recorded shifts   // S105 A5a: tag = the Kept palette's frame tag (hook + kPaletteLead for a latch read: the own-rows rule pairs a piece and its owner only when both are of the SAME latch); probe = the link's copy probe   // S103 W2B: extra = the owner body's spring-hair rows of this latch (canonical order), appended after its palette by EmitItems   // S103 DTP fix: link_only = no palette of its own (the piece object is gone or stale): written from the latch's link proof alone   // S103 NRB: stale / the SUBM link (S103 NRBF: the owner skeleton the link was read from + the reader's proof)
  void Collect(const FrameInfo& f, std::vector<EmitItem>& items, Counters& c);
  void EmitItems(const FrameInfo& f, SceneTables& t, Counters& c, std::vector<EmitItem>& items, uint32_t light);
  LatchMark latch_;
  std::vector<EmitItem> latch_items_;
  std::vector<uint32_t> visible_skels_;   // S103 V1F F1: guarded by mutex_; set by StoreVisible (Latch, and Emit without a latch)
  void StoreVisible(const std::vector<EmitItem>& items);
  Counters latch_counts_;
  uint32_t latch_light_ = kUnknown;
};

WrestlerContributor& Wrestlers();   // the one instance (static lifetime)
void RegisterWrestlers();            // Register(&Wrestlers()) once, at startup (idempotent); the hooks do nothing until this has run
bool WrestlersRegistered();          // one relaxed load (the hook's gate: an unregistered class would collect forever and never emit)

}  // namespace rdr_scene::live
