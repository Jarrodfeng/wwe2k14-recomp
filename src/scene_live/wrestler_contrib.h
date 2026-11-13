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

#include "../renderer_scene/live_scene.h"

namespace rdr_scene::live {

// skin = inv_bind x world (row vectors: the layout of Mat4, translation in [12..14]); the order the game's own sub_82296630 uses.
Mat4 SkinFromWorld(const Mat4& world, const Mat4& inv_bind);

class WrestlerContributor final : public Contributor {
 public:
  static constexpr uint32_t kUnknown = 0xFFFFFFFFu;
  static constexpr uint32_t kDefaultMinNodes = 60;  // wrestler skeletons have ~109 nodes (assets.md); a size filter, NOT an identity test [U]

  const char* Name() const override { return "wrestlers"; }
  void Emit(const FrameInfo& f, SceneTables& t, Counters& c) override;

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
  void SubmitConsumer(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t frame = 0);
  // Node count the hook must copy for this model object: the largest count sub_82BD30E8 saw for it, 0 = never seen (then no copy is made).
  uint32_t WantCount(uint32_t skel);
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

  // ---- the declared route, read at the exit of sub_82720658 (R1_wrestler_assets.md section 1; model_hooks.h ModelLinkRead) ----
  // What the game's slot descriptor holds after the build [C]: key = desc+28 = character*100 + attire*10 + kind (kind = key % 10), body = desc+4 (the
  // drawn model = the skeleton object, the pairing key of the draw), pieces = 24-byte entries at [desc+12] (model at +4, u16 id at +8), the attire's
  // PACH buffer pointer and size (reader+68 / +72). The pac / entry NAME is [U] (R1 open): the record carries the key as `entry` and the attire as
  // `variant` of a pac-less AssetRef (pac 0 = unknown, drawers skip it), never a guessed pac path.
  struct PieceRef { uint32_t model = 0; uint32_t id = 0; };
  struct ModelKeyInfo {
    bool known = false;
    uint32_t key = 0, pach_buf = 0, pach_size = 0, desc = 0;
    std::vector<PieceRef> pieces;
  };
  void SubmitModelLink(uint32_t body_model, uint32_t key, uint32_t pach_buf, uint32_t pach_size, uint32_t desc, std::vector<PieceRef>&& pieces);
  void NoteLinkNoBody() { link_no_body_.fetch_add(1, std::memory_order_relaxed); }
  void NoteLinkEmptyKey() { link_empty_key_.fetch_add(1, std::memory_order_relaxed); }
  void NoteLinkUnreadable() { link_unreadable_.fetch_add(1, std::memory_order_relaxed); }
  void NoteLinkBadPiece() { link_bad_piece_.fetch_add(1, std::memory_order_relaxed); }
  ModelKeyInfo ModelKeyFor(uint32_t body_model);
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
    bool consumer = false;
    std::vector<Mat4> skin;
    uint32_t age = 0;                          // Emits since it was last refreshed
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
  void Put(uint32_t skel, int32_t slot, uint64_t frame, std::vector<Mat4>&& skin, bool consumer = false);
  struct Known {
    int32_t slot = -1;
    uint32_t max_count = 0;  // the LATEST full count the update hook saw (not a high-water mark: a reused address must not keep a stale count)
    uint32_t palette_ptr = 0;
    Link declared, guessed;
    ModelKeyInfo model;
    uint64_t touch = 0;          // monotonic use stamp (eviction when the table is full)
    uint32_t age = 0;            // Emits since it was in last_ / cur_ (pruned past kKnownAge)
    bool seen_as_skeleton = false;
    bool has_consumer = false;   // a consumer palette arrived: derived palettes are bookkeeping only while it is fresh (kConsumerFresh frames)
    uint64_t consumer_frame = 0;
  };
 public:
  static constexpr uint32_t kKnownAge = 1800;      // Emits without the skeleton in the record before its bookkeeping is dropped
  static constexpr uint64_t kConsumerFresh = 4;    // frames: a derived palette within this of the newest consumer read never competes with it
 private:
  uint64_t touch_seq_ = 0;
  std::unordered_map<uint32_t, uint32_t> piece_owner_;  // piece model -> body model (built by SubmitModelLink; O(1) for the entry hook)
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
  void ListAddLocked(uint32_t model);
  void ListRemoveLocked(uint32_t model);
  void DropPiecesLocked(uint32_t body);
  Known* KnownFor(uint32_t skel);  // mutex_ held; null when the table is full (bounded like cur_)

  std::mutex mutex_;
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
};

WrestlerContributor& Wrestlers();   // the one instance (static lifetime)
void RegisterWrestlers();            // Register(&Wrestlers()) once, at startup (idempotent); the hooks do nothing until this has run
bool WrestlersRegistered();          // one relaxed load (the hook's gate: an unregistered class would collect forever and never emit)

}  // namespace rdr_scene::live
