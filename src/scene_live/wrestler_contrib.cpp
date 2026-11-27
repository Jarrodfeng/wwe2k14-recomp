// Class C4, the wrestlers (Session 101, lane 29). See wrestler_contrib.h.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "wrestler_contrib.h"
#include "../renderer_scene/detached_piece.h"   // S106 G4-2
#include "../renderer_scene/piece_hidden.h"   // S111 SOCKPIECE: OwnerMeshHidesPiece
#include "../renderer_scene/bone_tag_rows.h"
#include "piece_link.h"   // S105 CAWP fix F5: kSubmMaxNodes
#include "model_file_note.h"   // S110 SEATREL (was PACANS' pac_answer.h table): the file each slot descriptor's model was built from
#include "build_levels_note.h"   // S111 LEVELSBYTE: the texture level count each slot descriptor's model was built with (the entry's "/l" tag)
#include "emit_order.h"   // S111 CHARORDER: the record's object order (EmitItems, StoreVisible)

#include "../renderer_adapter/upd_lean.h"   // S102 U3 (the plant macro, the census)
#include "../renderer_game/scene_frame.h"   // S103 A3 (F7: the mode gate)
#include "../renderer_scene/named_load_fail.h"   // S103 DLW
#include "../assets/attire_build.h"   // S103 ARM2: the colours the game painted a composite attire with (build-step latch)
#include "../assets/change_mask.h"    // S108 THR2: the `/m` entry tag (a recoloured body loads with its four masks)
#include "../renderer_scene/rows_change_colour.h"   // S108 THR2: section 67
#include "../renderer_scene/change_colour_draw.h"   // S108 THR2: the draw side's census (printed with the store's)

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <set>

namespace rdr_scene::live {

Mat4 SkinFromWorld(const Mat4& world, const Mat4& inv_bind) {
  // out row i = sum_k inv_bind[i][k] * world.row(k): sub_82296630 called as (out = skeleton+200, a = world, b = inv bind).
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += inv_bind.m[i * 4 + k] * world.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}

static std::atomic<bool> g_registered{false};

WrestlerContributor& Wrestlers() {
  static WrestlerContributor w;
  return w;
}

void RegisterWrestlers() {
  if (!g_registered.exchange(true)) Register(&Wrestlers());
}

bool WrestlersRegistered() { return g_registered.load(std::memory_order_relaxed); }

WrestlerContributor::Known* WrestlerContributor::KnownFor(uint32_t skel, bool count_revival) {
  auto it = known_.find(skel);
  if (it != known_.end()) { it->second.touch = ++touch_seq_; return &it->second; }
  if (known_.size() >= 2 * kMaxSkeletons) {
    // Full (Emit may not be running to age entries out: models are built at match load, before any sink attaches): evict the least recently used
    // entry that is not in the record. None to evict = refuse (bounded memory).
    auto victim = known_.end();
    for (auto k = known_.begin(); k != known_.end(); ++k)
      if (!cur_.count(k->first) && !last_.count(k->first) && (victim == known_.end() || k->second.touch < victim->second.touch)) victim = k;
    if (victim == known_.end()) return nullptr;
    if (faults_ & kFaultLinkInKnown) DropBodyLinkLocked(victim->first);   // S103 PZ F5: the old rule; today the link is the BODY's and outlives its Known record
    known_.erase(victim);
  }
  Known& n = known_.emplace(skel, Known{}).first->second;
  n.touch = ++touch_seq_;
  if (count_revival && body_links_.count(skel)) body_link_revived_.fetch_add(1, std::memory_order_relaxed);   // PZ F5 risk: a surviving link meets a new Known at its address (a returning body, or a reused address)
  return &n;
}

void WrestlerContributor::ListAddLocked(uint32_t model) {
  if (model <= 1) return;
  int32_t free_at = -1;
  uint32_t at = ListHash(model);
  for (uint32_t i = 0; i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
    const uint32_t v = listed_[at].load(std::memory_order_relaxed);
    if (v == model) return;
    if (v == 1 && free_at < 0) free_at = int32_t(at);
    if (v == 0) { if (free_at < 0) free_at = int32_t(at); break; }
  }
  if (free_at < 0) { listed_full_.fetch_add(1, std::memory_order_relaxed); return; }
  listed_[uint32_t(free_at)].store(model, std::memory_order_relaxed);
}

void WrestlerContributor::ListRemoveLocked(uint32_t model) {
  uint32_t at = ListHash(model);
  for (uint32_t i = 0; i < kListSlots; ++i, at = (at + 1) & (kListSlots - 1)) {
    const uint32_t v = listed_[at].load(std::memory_order_relaxed);
    if (v == 0) return;
    if (v == model) { listed_[at].store(1, std::memory_order_relaxed); return; }
  }
}

void WrestlerContributor::DropPiecesLocked(uint32_t body) {
  auto it = body_links_.find(body);   // S103 PZ F5: the link is the body's, not the Known record's
  if (it == body_links_.end()) return;
  for (const PieceRef& p : it->second.info.pieces) {
    auto o = piece_owner_.find(p.model);
    if (o != piece_owner_.end() && o->second == body) piece_owner_.erase(o), ListRemoveLocked(p.model);
  }
  it->second.info.pieces.clear();
  it->second.pieces_dropped = true;   // PZF2 A3: re-listed from [desc + 12] when a latch's descriptor check passes (RelistPieces)
  if (piece_owner_.empty())   // lane 206 (note): tombstones never clear by themselves; an empty owner table empties the lock-free one
    for (auto& a : listed_) a.store(0, std::memory_order_relaxed);
}

void WrestlerContributor::DropBodyLinkLocked(uint32_t body) {
  DropPiecesLocked(body);
  body_links_.erase(body);
}

std::string WrestlerContributor::BodyLinkText() const {
  auto u = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
  return "revived " + u(body_link_revived_) + " dropped_count " + u(body_link_dropped_count_) + " dropped_key " + u(body_link_dropped_key_) + " known_erased " + u(known_erased_linked_);
}

void WrestlerContributor::Put(uint32_t skel, int32_t slot, uint64_t frame, std::vector<Mat4>&& skin, bool consumer, bool walk) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (Known* k = KnownFor(skel)) {
    if (walk) k->walk_frame = std::max(k->walk_frame, frame);   // S103 L31B review C
    if (consumer) {
      if (slot < 0) slot = k->slot;  // the consumer hook cannot match a slot; the update hook did
    } else {
      if (k->max_count != 0 && k->max_count != uint32_t(skin.size()) && !(faults_ & kFaultKeepStaleKnown)) {   // BHF1 G1: another node count at a known address = another object: nothing of the old body's liveness / consumer / slot facts carries over
        k->palette_ptr = 0, k->has_consumer = false, k->consumer_frame = 0, k->slot_key = 0;
        k->walk_frame = k->draw_frame = k->draw_prev = k->hide_frame = k->hide_prev = 0;   // S111 VISLATCH: the ask / hide history goes with the old object too
        reuse_reset_.fetch_add(1, std::memory_order_relaxed);
      }
      if (!(faults_ & kFaultKeepStaleKnown)) k->slot = slot;   // BHF1 G1: unconditional: an unmatched update (-1) clears the slot, so a reused address cannot keep the old wrestler's character slot (the old `if (slot >= 0)` never cleared it)
      else if (slot >= 0) k->slot = slot;
      k->max_count = uint32_t(skin.size());  // the latest count, so a reused address does not keep an old one
      if (auto b = body_links_.find(skel); b != body_links_.end() && !(faults_ & kFaultLinkInKnown)) {   // S103 PZ F5: the same address reported with another node count = another object: the link is dead
        if (!b->second.seen_count) b->second.seen_count = k->max_count;
        else if (b->second.seen_count != k->max_count) {
          DropBodyLinkLocked(skel);
          body_link_dropped_count_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      k->upd_frame = std::max(k->upd_frame, frame);   // S103 A2: liveness, set before the consumer-fresh return below
      // The two hooks tag frames from different threads, so source priority must not depend on equal tags: while a consumer read for this skeleton
      // is fresh, a derived palette is bookkeeping only (slot and count above).
      if (k->has_consumer && frame <= k->consumer_frame + kConsumerFresh) return;
    }
    if (consumer) {
      k->has_consumer = true;
      k->consumer_frame = std::max(k->consumer_frame, frame);
      // derived updates that raced ahead of this read (higher tag, same skeleton) must not outrank it
      auto pc = cur_.find(skel);
      if (pc != cur_.end()) {
        auto& pu = pc->second.upds;
        pu.erase(std::remove_if(pu.begin(), pu.end(), [&](const Upd& x) { return !x.consumer && x.frame <= frame + kConsumerFresh; }), pu.end());
      }
    }
  }
  auto it = cur_.find(skel);
  if (it == cur_.end()) {
    const size_t held = last_.size() + cur_.size();
    // lane 206 (S1): a skin under the body size (a piece's own skeleton) leaves kBodyHeadroom places free for bodies
    const bool small_skin = skin.size() < MinNodes();
    if (!last_.count(skel) && (held >= kMaxSkeletons || (small_skin && held >= kMaxSkeletons - kBodyHeadroom))) { cap_refused_.fetch_add(1, std::memory_order_relaxed); NoteUnreadable(); return; }  // bounded memory
    it = cur_.emplace(skel, Pending{}).first;
  }
  auto& u = it->second.upds;
  for (Upd& x : u)
    if (x.frame == frame) {
      if (first_wins_) return;  // the planted fault: keep the first update of the frame
      if (x.consumer && !consumer) {  // the draw's own read beats a derived palette of the same frame, whichever hook ran last
        if (slot >= 0) x.slot = slot;
        return;
      }
      x.slot = slot >= 0 || !x.consumer ? slot : x.slot;
      x.consumer = consumer;
      x.skin = std::move(skin);  // the last update of a frame wins
      return;
    }
  Upd n;
  n.frame = frame, n.slot = slot, n.consumer = consumer, n.skin = std::move(skin);
  u.push_back(std::move(n));
  if (u.size() > kMaxPending) u.erase(u.begin());
}

void WrestlerContributor::SubmitSkin(uint32_t skel, int32_t slot, uint32_t count, const Mat4* skin, uint64_t frame) {
  if (!skin || count == 0) { NoteUnreadable(); return; }
  Put(skel, slot, frame, std::vector<Mat4>(skin, skin + count));
}

void WrestlerContributor::SubmitWorld(uint32_t skel, int32_t slot, uint32_t count, const Mat4* world, const Mat4* inv_bind, uint64_t frame) {
  if (!world || !inv_bind || count == 0) { NoteUnreadable(); return; }
  std::vector<Mat4> skin(count);
  for (uint32_t i = 0; i < count; ++i) skin[i] = SkinFromWorld(world[i], inv_bind[i]);
  Put(skel, slot, frame, std::move(skin));
}

// S102 U3 (R26 lane A (b)): Put's consumer path, minus the allocation. A wrestler's parts call sub_82C04DF0 many times a frame with the same skeleton and (usually) the same palette; the general path
// builds a fresh std::vector<Mat4> of count x 64 bytes for each call (a heap allocation, a copy, then the free of the previous one inside Put). When this frame already holds a consumer update for the
// skeleton, Put would do exactly: slot = Known.slot (the call passes -1); Known.has_consumer / consumer_frame; erase the derived updates the consumer outranks; then, at the update of the frame,
// slot / consumer / skin replaced ("the last update of a frame wins"). This does the same steps and assigns the skin into the existing vector (same size = no allocation). Anything else (no Known, no
// pending list for the skeleton, no update of this frame, the planted first-wins) returns false with NOTHING touched and the caller uses Put.
bool WrestlerContributor::PutConsumerInPlace(uint32_t skel, uint64_t frame, const Mat4* skin, uint32_t count, bool walk) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (first_wins_) return false;
  auto pc = cur_.find(skel);
  if (pc == cur_.end() || known_.find(skel) == known_.end()) return false;
  bool have = false;
  for (const Upd& x : pc->second.upds)
    if (x.frame == frame && x.consumer) { have = true; break; }
  if (!have) return false;
  Known* k = KnownFor(skel);   // the same one call Put makes (the LRU stamp moves exactly once)
  int32_t slot = k->slot;      // the consumer hook cannot match a slot; the update hook did
  if (walk) k->walk_frame = std::max(k->walk_frame, frame);   // S103 L31B review C
  k->has_consumer = true;
  k->consumer_frame = std::max(k->consumer_frame, frame);
  auto& pu = pc->second.upds;
  pu.erase(std::remove_if(pu.begin(), pu.end(), [&](const Upd& x) { return !x.consumer && x.frame <= frame + kConsumerFresh; }), pu.end());
  for (Upd& x : pu)
    if (x.frame == frame) {
      if (!UPD_PLANT(11)) x.slot = slot >= 0 ? slot : x.slot;   // (x.consumer is true here: Put's `slot >= 0 || !x.consumer ? slot : x.slot`)
      x.consumer = true;
      if (!UPD_PLANT(11)) x.skin.assign(skin, skin + count);   // (planted: the palette of the frame's first call stays)
      wwe2k14::upd_lean::Count(wwe2k14::upd_lean::kConsumerInPlace);
      return true;
    }
  return true;   // unreachable: the update of this frame is a consumer one and the erase keeps those
}

void WrestlerContributor::StampDrawForTest(uint32_t skel, uint64_t frame) {   // tests only: the find-only draw stamp NoteDrawAsked makes (the game asked to draw this body)
  std::lock_guard<std::mutex> lock(mutex_);
  auto kn = known_.find(skel);   // find, never create: only a body the update hook has met is a body
  if (kn == known_.end()) { draw_unknown_.fetch_add(1, std::memory_order_relaxed); return; }
  NoteAsk(kn->second, frame);   // S111 VISLATCH: the stamp NoteDrawAsked makes
  draw_asked_.fetch_add(1, std::memory_order_relaxed);
}

void WrestlerContributor::SubmitConsumer(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t frame, bool in_place) {
  if (!skin || count == 0) { NoteUnreadable(); return; }
  consumer_submits_.fetch_add(1, std::memory_order_relaxed);
  if (in_place && PutConsumerInPlace(skel, frame, skin, count, true)) return;
  Put(skel, -1, frame, std::vector<Mat4>(skin, skin + count), true, true);   // walk: the game's draw read it (review C)
}

void WrestlerContributor::LatchTargets(uint64_t hook_frame, std::vector<PaletteTarget>& out, uint32_t& missing) { LatchTargets(hook_frame, nullptr, out, missing); }

void WrestlerContributor::LatchTargets(uint64_t hook_frame, const SlotTable* slots, std::vector<PaletteTarget>& out, uint32_t& missing, bool count) {
  out.clear();
  missing = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto note = [count](std::atomic<uint64_t>& a) { if (count) a.fetch_add(1, std::memory_order_relaxed); };   // review E: Take's second ask counts nothing
  {   // S103 DRL: publish the slot bodies for the draw-ask filter (lock-free reads in NoteDrawAsked). No table = no bodies.
    uint32_t n = 0;
    if (slots)
      for (const SlotSkel& s : slots->skels)
        if (n < kDrawSlots && s.skel) {
          const uint64_t w = (uint64_t(s.shown_addr) << 32) | s.skel;
          if (draw_pub_[n].load(std::memory_order_relaxed) != w) draw_checked_[n].store(0, std::memory_order_relaxed);   // BHF1 B7: a republish that moved a different body into position n does not inherit the old occupant's "checked this frame"
          draw_pub_[n].store(w, std::memory_order_relaxed);
          ++n;
        }
    for (uint32_t i = n; i < kDrawSlots; ++i) draw_pub_[i].store(0, std::memory_order_relaxed), draw_checked_[i].store(0, std::memory_order_relaxed);
    draw_pub_frame_.store(hook_frame, std::memory_order_relaxed);
  }
  // S103 L31B: the character table's skeletons first, every latch, whatever the update hook's clock says (wrestler_contrib.h LatchTargets(slots)).
  std::set<uint32_t> slot_set;   // the slot skeletons that ARE targets (a count is known)
  std::set<uint32_t> piece_targeted;   // S105 CAWP: the linked pieces already pushed as targets this latch (consider does not push them again)
  if (slots)
    for (const SlotSkel& s : slots->skels) {
      if (slot_set.count(s.skel)) continue;   // two slots, one skeleton: read once
      auto kn = known_.find(s.skel);
      if (kn != known_.end() && !s.game_shown) HideLiveness(kn->second, hook_frame + kPaletteLead);   // S107 CAM: the game's own shown predicate (sub_823CBC50) is false: it hid him. S111 VISLATCH: labelled with the period this latch reads (hook + kPaletteLead, its palette read's tag), so the record built now (the walk before the hide) still shows him and the next does not
      if (kn == known_.end() || !kn->second.max_count) { note(slot_no_count_); continue; }
      // render-clock: lookup - freshness of the update hook's report, how old our own input is
      const bool quiet = kn->second.upd_frame + kConsumerFresh < hook_frame;
      if (quiet && !(faults_ & kFaultKeepGoneTargets)) {   // review D: a quiet body is read only while it is still kept (cur_, or last_ within kPruneAfter); after that it has left the record: not a target
        auto l = last_.find(s.skel);
        const bool kept = cur_.count(s.skel) || (l != last_.end() && l->second.age < kPruneAfter);
        if (!kept) { note(slot_gone_); continue; }
      }
      if (quiet) note(slot_hook_quiet_);   // the old rule would have dropped it here
      note(kn->second.slot < 0 ? slot_join_unmatched_ : kn->second.slot == s.slot ? slot_join_agree_ : slot_join_differ_);
      slot_set.insert(s.skel);
      out.push_back(PaletteTarget{s.skel, kn->second.max_count});
      note(slot_targets_);
    }
  draw_pos_.clear();   // S111 CAT33 (char_draw_list.h): each table skeleton's place in the game's category-33 list, replaced whole every latch (no table = none: slot order)
  if (slots)
    for (const SlotSkel& s : slots->skels)
      if (s.draw_pos >= 0) draw_pos_.emplace_back(s.skel, s.draw_pos);
  if (slots) {   // S111 OWNERLESS: this latch's DRTR nodes read off, for the record that shows the walk after it (labelled like the hide above)
    const uint64_t label = hook_frame + kPaletteLead;
    if (drtr_off_[0].label != label) std::swap(drtr_off_[0], drtr_off_[1]);
    DrtrOff& d = drtr_off_[0];
    d.label = label, d.off.clear(), d.on.clear(), d.pieces.clear();
    const bool no_doubt = (faults_ & kFaultDrtrNoDoubt) != 0;   // the planted twin
    if (no_doubt || !slots->far_layer)   // S111 OWNERLESS review: the far layer draws DRTR nodes whatever their enable: then "off" is no evidence
      for (const SlotSkel& s : slots->skels)
        if (s.drtr_on == 0 || (no_doubt && s.drtr_on < 0)) d.off.push_back(s.skel);   // an unread node is no evidence of "off"
    if (!slots->far_layer)   // S111 PIECEROUTE: the node's own enable read ON (an unread node is no route); none while the far layer may draw (fail closed)
      for (const SlotSkel& s : slots->skels)
        if (s.drtr_on == 1) {
          d.on.push_back(s.skel);
          if (s.trans_gates) d.pieces.push_back(s.skel);   // its DRTR node is on AND its slot 22 draws the pieces
        }
  }
  if (slots) {   // S103 NRBF (review F1): the ring family draws these models (the apron skirts among them): they are no wrestler objects, so no light-ref evidence is ever needed to exempt them
    ring_models_ = slots->ring_models;
    std::sort(ring_models_.begin(), ring_models_.end());
  }
  if (slots) {   // S103 NRB: the SUBM piece links of this latch (piece_link.h), replaced whole: a piece whose character left the table loses its link
    std::unordered_map<uint32_t, PieceLink> links;
    if (piece_rows_frame_ != hook_frame) {   // S103 HGF (review 1): a new latch: the rows of the one before are kept beside them (Collect pairs a body palette with the rows of ITS latch); a second ask of the same hook only rebuilds the current set
      piece_rows_prev_.swap(piece_rows_);
      piece_links_prev_.swap(piece_links_);   // S111 PERIODALL: the links of the latch before go with its rows (Collect's LinksOfRecordL)
      piece_rows_prev_frame_ = piece_rows_frame_;
      piece_seq_prev_ = piece_seq_;   // S111 PERIODALL review R3: and the seq of its descriptor checks (Collect's VerdictSeqOfRecordL)
    }
    piece_rows_.clear();   // S103 W2B: replaced whole, like the links
    piece_rows_frame_ = hook_frame;   // S103 HGF: the latch these rows are read in
    piece_seq_ = collect_seq_;   // S111 PERIODALL review R3: this latch's checks (ReadSlotSkeletons ran them before this call, with this seq)
    for (const scene_inputs::CharPiece& p : (slots->pieces_view ? *slots->pieces_view : slots->pieces)) {
      // S111 KEPTPIECES: the owner is the one the reader named (the character whose SUBM list holds the piece), never "the slot's first skeleton": the editor's slot 0 holds the table's new character AND the
      // kept old one (drawn_slots.h) until the flip, and the game draws each one's pieces with its own body (sub_825A3908). The owner must be a body of this latch at the piece's slot (none: no link).
      const uint32_t owner = p.owner;
      if (!owner || p.model == owner || std::none_of(slots->skels.begin(), slots->skels.end(), [&](const SlotSkel& s) { return s.skel == owner && s.slot == p.slot; })) continue;
      links[p.model] = PieceLinkOf(p, p.slot);   // S105 A5a: + the copy probe (S111 PIECECARRY: the one rule; NoteFreedLast makes the same link)
      if (p.rows_unposed && count) rdr_scene::g_spring_census.rows_unposed.fetch_add(1, std::memory_order_relaxed);   // S103 HGF: the piece's rows were still the bind palette (identity): dropped, the piece is absent this latch
      if (p.rows_bad && count) rdr_scene::g_spring_census.refused_unreadable.fetch_add(1, std::memory_order_relaxed);   // S103 W2B: the piece is MapUnreadable (a hole): its rows could not be read
      if (PieceHasRows(p)) AppendPieceRows(p, piece_rows_[owner]);   // S103 W2B: the piece's spring-hair rows, kept with the owner (S111 PIECECARRY: the one rule, shared with NoteFreedLast)
      if (count) {
        auto lg = piece_link_logged_.find(p.model);
        if (lg == piece_link_logged_.end() || lg->second != p.why) {   // once per model and proof (NoCount -> Ok when the piece is recorded later logs again)
          piece_link_logged_[p.model] = p.why;
          piece_links_made_.fetch_add(1, std::memory_order_relaxed);
          if (PieceLinkLogFn f = piece_link_log_.load(std::memory_order_relaxed)) f(p.model, p.slot, p.id, owner, WantedCount(p.model), p.why, p.build, p.kind);
        }
        // S103 DTP fix: the RAW owner-copy word (instance +24, CharPiece::copy) changed, whatever the proof says (a piece with no node count yet still has its word; the game's normal word is 1, so a first sight at another value is a change)
        if (p.copy != 0xFFFFFFFFu) {
          auto cg = piece_copy_logged_.find(p.model);
          const uint32_t prev = cg == piece_copy_logged_.end() ? 1u : cg->second;
          piece_copy_logged_[p.model] = p.copy;
          if (prev != p.copy)
            if (PieceDetachLogFn f = piece_detach_log_.load(std::memory_order_relaxed)) f(p.model, p.slot, p.id, owner, p.copy, hook_frame);
        }
      }
    }
    for (const auto& kv : test_rows_) piece_rows_[kv.first] = kv.second;   // tests only (empty in the game)
    for (auto& kv : piece_rows_) SortExtraRows(kv.second);   // S103 W2B: canonical order = piece id ascending, then node ascending (the front end's model order); stable, so two instances of one piece id stay in reader order and never match the model's tags
    for (auto it = piece_link_logged_.begin(); it != piece_link_logged_.end();) it = links.count(it->first) ? std::next(it) : piece_link_logged_.erase(it);
    for (auto it = piece_copy_logged_.begin(); it != piece_copy_logged_.end();) it = links.count(it->first) ? std::next(it) : piece_copy_logged_.erase(it);   // S103 DTP fix
    piece_links_.swap(links);
    // S105 CAWP (docs/research/s105/CAWP.md): every linked piece whose owner the character table holds is a latch target EVERY latch, whatever the update hook says (in a match it goes quiet for a CAW's hair: the piece sat 120 records
    // stale, was pruned and never came back). Liveness is the game's own: the link is re-read from the character's SUBM list at every latch (a piece that left it is no link), and a piece the game hides (inst +20 == 0) is NOT pushed (it ages out; Hidden is judged on the own-rows route and the detached branch of EmitItems only). S105 CAWP fix F2: only what an emit route can use is pushed (Ok = by owner, NoCopy = detached, or an owner the record cannot name = own rows), so a not-Ok stock piece never becomes a permanent hole.
    if (!(faults_ & kFaultHookOnlyPieces))
    for (const auto& kv : piece_links_) {
      const PieceLink& pl = kv.second;
      if (pl.slot < 0 || pl.count == 0 || slot_set.count(kv.first) || !slot_set.count(pl.owner) || piece_targeted.count(kv.first)) continue;
      if (!(faults_ & kFaultPushEverything) && pl.probe.drawn == 0) continue;   // F1: the game hides it (inst +20 == 0)
      if (!(faults_ & kFaultPushEverything) && pl.why != scene_inputs::PieceWhy::Ok && pl.why != scene_inputs::PieceWhy::NoCopy && RecordCanNameL(pl.owner)) continue;   // F2
      piece_targeted.insert(kv.first);
      out.push_back(PaletteTarget{kv.first, std::min<uint32_t>(pl.count, 1024)});
    }
  }
  // audit item 2 (piece liveness): a skeleton the update hook reports is alive while that hook is. A PIECE (hair, caps: no update hook of its own) is alive only while its OWNER body is alive AND the game's
  // descriptor still lists it (piece_owner_); an own-skeleton piece (lane 209, never in the descriptor) is alive only while some body is. The latch's own submits never keep anything alive: liveness is
  // judged from the update hook's stamps (upd_frame) alone, so a piece nobody feeds ages out of last_ (Collect prunes after kPruneAfter).
  // S105 CAWP: that holds for UNLINKED skeletons only. A SUBM-linked piece whose owner is in a slot was pushed above (the game's own liveness: the link is re-read every latch), so it never ages while its owner is held.
  auto body_alive = [&](uint32_t body) {
    if (slot_set.count(body)) return true;   // S103 L31B: a slot skeleton is alive while the character table holds it
    auto b = known_.find(body);
    // render-clock: lookup - freshness of the update hook's report, how old our own input is
    return b != known_.end() && b->second.max_count && b->second.upd_frame + kConsumerFresh >= hook_frame;
  };
  auto any_body_alive = [&]() {
    if (!slot_set.empty()) return true;
    for (const auto& kv : known_)
      // render-clock: lookup - freshness of the update hook's report, how old our own input is
      if (kv.second.max_count >= MinNodes() && kv.second.upd_frame + kConsumerFresh >= hook_frame) return true;
    return false;
  };
  auto consider = [&](uint32_t skel) {
    if (slot_set.count(skel) || piece_targeted.count(skel)) return;   // already a target (S103 L31B; S105 CAWP: a linked piece pushed above)
    auto kn = known_.find(skel);
    uint32_t count = 0;
    if (kn != known_.end() && kn->second.max_count) {
      // render-clock: lookup - freshness of the update hook's report, how old our own input is
      if (kn->second.upd_frame + kConsumerFresh >= hook_frame) count = kn->second.max_count;   // alive and sized by the update hook
      else return;                                                                              // the update hook stopped reporting it: dead, not fed (it ages out of last_)
    } else {                                                                                    // a piece: no update hook of its own
      auto o = piece_owner_.find(skel);
      auto pl = piece_links_.find(skel);   // S103 NRB: a SUBM-linked piece is alive while its own character is
      const bool alive = pl != piece_links_.end() ? body_alive(pl->second.owner)
                         : (o != piece_owner_.end() && o->second != skel) ? body_alive(o->second) : any_body_alive();
      if (!alive) return;
      count = std::min<uint32_t>(WantedCount(skel), 1024);
    }
    if (count) out.push_back(PaletteTarget{skel, count});
    else ++missing;
  };
  for (const auto& kv : last_) consider(kv.first);
  for (const auto& kv : cur_)
    if (!last_.count(kv.first)) consider(kv.first);
}

// S103 L31B: the same refresh rule Collect applies (an update tagged <= PickLimit(record) refreshes the kept entry), asked before Collect runs.
uint32_t WrestlerContributor::StaleVisibleAt(uint64_t record_frame, const SlotTable* slots) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t limit = PickLimit(record_frame);
  uint32_t n = 0;
  for (const auto& kv : last_) {
    bool refreshed = false;
    if (auto pc = cur_.find(kv.first); pc != cur_.end())
      for (const Upd& u : pc->second.upds) refreshed = refreshed || u.frame <= limit;
    if (refreshed || kv.second.age + 1 > kPruneAfter) continue;   // fresh, or pruned by this Collect
    int32_t slot = kv.second.slot;
    if (auto kn = known_.find(kv.first); kn != known_.end() && kn->second.slot >= 0) slot = kn->second.slot;   // Collect's rule: the update hook's slot wins
    if (slot < 0) continue;                                                                                // not visible (a skel: id)
    if (slots) {
      if (faults_ & kFaultPointerPresence) {   // the planted twin: the reviewed fail-open rule (presence by pointer)
        if (std::none_of(slots->skels.begin(), slots->skels.end(), [&](const SlotSkel& s) { return s.skel == kv.first; })) continue;
      } else if (slot < 9 && slots->state[slot] == kSlotEmpty) {
        continue;   // review A: only an EMPTY slot means he left (occupied by anyone, or unreadable = still there: walk)
      }
    }
    ++n;
  }
  return n;
}

std::string WrestlerContributor::SlotCensus() const {
  const auto u = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
  const auto bins = [&](const char* name, const std::atomic<uint64_t>* a) {   // S103 AUSF: per slot (s0..s8, s- = no slot), only the non-zero ones
    std::string r;
    for (int i = 0; i < kSlotBins; ++i)
      if (const uint64_t v = a[i].load(std::memory_order_relaxed)) r += (r.empty() ? "" : " ") + std::string(i == 9 ? "s-" : "s" + std::to_string(i)) + "=" + std::to_string((unsigned long long)v);
    return std::string(", ") + name + " by slot [" + r + "]";
  };
  return "want_lock " + want_lock_.Text() + ", reuse_reset " + u(reuse_reset_) + ", slot_targets " + u(slot_targets_) + " (hook quiet " + u(slot_hook_quiet_) + "), slot_no_count " + u(slot_no_count_) + ", slot join agree " + u(slot_join_agree_) + " differ " + u(slot_join_differ_) +
         " hook_unmatched " + u(slot_join_unmatched_) + ", slot_gone " + u(slot_gone_) + ", quiet_unseen " + u(quiet_unseen_) + ", draw_asked " + u(draw_asked_) + " (hidden " + u(draw_hidden_) + ", unread " + u(draw_unread_) + ", unknown " + u(draw_unknown_) + ")" + ", stale_dropped " + u(stale_dropped_) + bins("stale_dropped", by_slot_stale_dropped_) + bins("quiet_unseen", by_slot_quiet_unseen_) +
         "; " + change_colour::Bodies().CensusLine() + "; " + rdr_scene::change_colour_draw::Draws().Text();   // S108 THR2: the Threads editor's live colours (the hook's reads, the rows, the draws)
}

int WrestlerContributor::SubmitLatchConsumer(uint32_t skel, uint32_t count, const Mat4* skin, uint64_t hook_frame) {
  if (!skin || count == 0) { NoteUnreadable(); return kSubmitted; }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // S111 HEADGUARD (docs\research\s111\HEADGUARD.md): no derived-head compare. The latch reads [skel+200] with the game parked after every writer of the period (the skin component, CHIK 0x82403D50
    // after the group-26 wait, the shifts 0x82BD3620), so the read IS the palette the game's draw 0x82C04DF0 takes for this body; the update-exit copy (+192 x +224) is older by construction. The game never
    // holds or refuses a palette, so neither do we: a body the character table does not hold (a hook-alive body: the referee, a manager, an entrance model) is submitted like a slot body.
    auto pc = cur_.find(skel);
    if (pc != cur_.end())
      for (const Upd& x : pc->second.upds)
        if (x.consumer && x.frame == hook_frame + kPaletteLead) {   // S103 W1: the walk copy of the SAME content (tag hook + kPaletteLead): keep it, compare ours (a walked frame: the stream build's mirror, or in either build a frame the decision did not make OURS, scene_frame.h DecideNow: hold / not ready, where the original sub_82C04DF0 and its mid-asm copy run)
          const bool same = x.skin.size() == count && std::memcmp(x.skin.data(), skin, size_t(count) * sizeof(Mat4)) == 0;
          (same ? latch_same_ : latch_differs_).fetch_add(1, std::memory_order_relaxed);
          return same ? kSame : kDiffers;
        }
  }
  consumer_submits_.fetch_add(1, std::memory_order_relaxed);
  if (PutConsumerInPlace(skel, hook_frame + kPaletteLead, skin, count)) return kSubmitted;   // S103 W1: the latch read is the content of stream hook + 2 = tag hook + kPaletteLead
  Put(skel, -1, hook_frame + kPaletteLead, std::vector<Mat4>(skin, skin + count), true);
  return kSubmitted;
}

uint32_t WrestlerContributor::PieceLinkCount(uint32_t model) {   // S105 CAWP
  rdr_scene::live::TimedLock<std::mutex> lock(mutex_, want_lock_);
  auto it = piece_links_.find(model);
  return it == piece_links_.end() ? 0u : it->second.count;
}

bool WrestlerContributor::GuestModelWanted(uint32_t model) {   // S105 CAWP; fix F4: one lock; a link is wanted only when the emitter cannot draw it from the owner's model (why != Ok) or the owner is not nameable
  rdr_scene::live::TimedLock<std::mutex> lock(mutex_, want_lock_);
  auto it = known_.find(model);
  if (it != known_.end() && it->second.max_count != 0) return true;
  if (faults_ & kFaultReaderIgnoresLinks) return false;
  auto pl = piece_links_.find(model);
  if (pl == piece_links_.end() || pl->second.count == 0) return false;
  return pl->second.why != scene_inputs::PieceWhy::Ok || !RecordCanNameL(pl->second.owner);
}

uint32_t WrestlerContributor::WantCount(uint32_t skel) {
  rdr_scene::live::TimedLock<std::mutex> lock(mutex_, want_lock_);   // BHF1 B2: counted, not cut (the cost is fitted first)
  auto it = known_.find(skel);
  return it == known_.end() ? 0u : it->second.max_count;
}

// S103 ARM2: the entry tag a body's colours ride in. The build-step latch (the game's own colours, read at sub_83418580) wins over the link-time read; switch off (the loader's hole rule = wwe2k14_rdr_attire_remap) = the link's tag only.
static std::string BuildTagOfLink(const WrestlerContributor::ModelKeyInfo& info) {
  if (rdr_scene::namedfail::HoleRule().load(std::memory_order_relaxed)) {
    std::string t = attire_build::Latch::Instance().TagForOwner(info.desc, info.key, info.chosen);
    if (!t.empty()) return t;
  }
  return info.tag;
}
static std::string BuildTagOfSlotKey(uint32_t chosen, uint32_t slot_desc) {   // a body named by its participant slot key: the capture of that chosen key made by THIS slot's descriptor (Heenan a1 24912 in runs 042 / 046: no link hook ever logged him)
  return rdr_scene::namedfail::HoleRule().load(std::memory_order_relaxed) ? attire_build::Latch::Instance().TagForSlot(chosen, slot_desc) : std::string();
}

bool WrestlerContributor::RecordCanName(uint32_t skel) {   // S103 CW-2b: the Collect loop's three sources, in one place
  std::lock_guard<std::mutex> lock(mutex_);
  return RecordCanNameL(skel);
}

bool WrestlerContributor::RecordCanNameL(uint32_t skel) {   // the same, mutex_ held by the caller
  auto kn = known_.find(skel);
  if (kn == known_.end()) return false;
  // S103 DLW (COV2 GAP 2 part 3): "the record can name it" means the key LOADS. The emitter's precedence (declared link, the body's declared link, the participant slot's key): the link it would write
  // is tested against the loader's failures (named_load_fail.h); a failed one is not named, so the GuestModel reader is asked and the body is drawn from the game's own rows, not left a hole.
  const bool keep = (faults_ & kFaultNamedFailHole) != 0;   // planted twin: the old answer ("has a key")
  const Known& k = kn->second;
  if (k.declared.known) return keep || !rdr_scene::namedfail::FailedToGuest(k.declared.pac, k.declared.entry, k.declared.variant);
  auto bl = body_links_.find(skel);
  // F4: the cheap test first (one bit while no ref of the character ever failed); the tag (latch mutex + map walk + string) is built only when it can matter
  if (bl != body_links_.end() && bl->second.info.known && (bl->second.verified_seq == collect_seq_ || (faults_ & kFaultLinkInKnown)))
    return keep || !rdr_scene::namedfail::MaybeFailedChar(model_file_note::FileIdOf(bl->second.info.desc, bl->second.info.key)) || !KeyFailed(bl->second.info.key, BuildTagOfLink(bl->second.info), bl->second.info.desc);
  if (bl != body_links_.end() && !bl->second.info.tag.empty()) {   // S103 CMB: a composite attire whose link is not usable this latch is never named by the plain slot key (a hole, not the stock model)
    // S103 ARM (review F8): "can name" true = the GuestModel rows are not asked, the body goes out unnamed = a HOLE (the guest rows bind atlases this build never fills); counted. Switch off = the old answer.
    if (!rdr_scene::namedfail::HoleRule().load(std::memory_order_relaxed) || keep) return false;
    rdr_scene::namedfail::LinkHoles().fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (k.slot_key) return keep || !rdr_scene::namedfail::MaybeFailedChar(model_file_note::FileIdOf(k.model_desc, k.slot_key)) || !KeyFailed(k.slot_key, BuildTagOfSlotKey(k.slot_key, k.slot_desc), k.model_desc);
  return false;
}

bool WrestlerContributor::KeyFailed(uint32_t key, const std::string& composite_tag, uint32_t model_desc) {   // S103 DLW: one bit test while no ref of this character ever failed (the game thread asks per skeleton)
  if (!rdr_scene::namedfail::MaybeFailedChar(model_file_note::FileIdOf(model_desc, key))) return false;
  const Link l = KeyLink(key, composite_tag, model_desc);
  return rdr_scene::namedfail::FailedToGuest(l.pac, l.entry, l.variant);
}

WrestlerContributor::Link WrestlerContributor::KeyLink(uint32_t key, const std::string& composite_tag, uint32_t model_desc) {   // S103 B / DLW: the AssetRef a model key is named by (pac from the character, the key in `entry`, the attire)
  Link l;
  char pac[32];
  // S110 SEATREL (was S110 PACANS, a name-keyed table fed by every lookup): the file the game's model in this slot descriptor was BUILT from (model_file_note.h: the read's own answer, handed to
  // the descriptor at the build), while the descriptor still holds this key. The id rule (character = key / 100, the file's own key) is what the game's pac list answers when nothing is seated.
  pac_answer::Answer a;
  const bool game = model_file_note::FileOfDesc(model_desc, key, a);
  std::snprintf(pac, sizeof pac, "pac\\ch\\ch%03u.pac", game ? a.file_id : CharacterOfKey(key));
  // S110 LIBSIZE: a library character's file is `<KEY>\pac\ch\ch497.pac` under the one "lib" overlay (the folder is the game's own answer, not the id)
  l.pac = (game && !a.lib_rel.empty()) ? a.lib_rel : std::string(pac);
  // S111 LEVELSBYTE: + the level count the game built this descriptor's model with, when it is not the fresh request's 4 (build_levels_note.h; level_tag.h "/l<byte>")
  l.entry = "model_key:" + std::to_string(game ? a.file_key : key) + build_levels_note::EntryTag(model_desc, key) + composite_tag, l.variant = AttireOfKey(key), l.known = true;
  return l;
}

WrestlerContributor::Link WrestlerContributor::SlotKeyLink(uint32_t key, uint32_t slot_desc, uint32_t model_desc) {   // S110 ENTRBODIES: the slot-key route's one rule (Collect, src\scene_start_bodies.h)
  return KeyLink(key, BuildTagOfSlotKey(key, slot_desc), model_desc);
}

std::string WrestlerContributor::PieceWhyText() const {   // S103 NRBF
  std::string out;   // (the names come from the ONE table in slot_piece.h: SCENE_PIECE_WHY_LIST)
  for (size_t i = 0; i < size_t(scene_inputs::PieceWhy::kCount); ++i) out += (i ? " " : "") + std::string(scene_inputs::kPieceWhyNames[i]) + " " + std::to_string((unsigned long long)piece_why_[i].load(std::memory_order_relaxed));
  out += " detached " + std::to_string((unsigned long long)piece_detached_.load(std::memory_order_relaxed));   // S103 DTP: of no_copy, the ones flagged kObjDetachedPiece (the census line's `detached N`)
  out += " game_hidden " + std::to_string((unsigned long long)piece_game_hidden_.load(std::memory_order_relaxed));   // S111 SOCKPIECE: pieces of a named owner the game hides (instance +20 == 0), flagged kObjPieceHidden
  out += " own_rows [" + OwnRowsText() + "]";   // S105 A5a
  return out;
}

std::string WrestlerContributor::OwnRowsText() const {   // S105 A5a: the names come from the ONE table in own_rows_rule.h
  std::string out;
  for (size_t i = 0; i < size_t(scene_inputs::OwnRowsWhy::kCount); ++i) out += (i ? " " : "") + std::string(scene_inputs::kOwnRowsNames[i]) + " " + std::to_string((unsigned long long)own_rows_[i].load(std::memory_order_relaxed));
  return out;
}

void WrestlerContributor::NotePalettePtr(uint32_t skel, uint32_t palette_ptr) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (Known* k = KnownFor(skel)) k->palette_ptr = palette_ptr;
}

void WrestlerContributor::NoteEntryUnknownModel(uint32_t model, uint32_t palette_ptr) {
  bool piece = false, by_desc = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto o = piece_owner_.find(model);  // O(1): the descriptor lists it (the game's own owner table)
    if (o != piece_owner_.end() && o->second != model) piece = by_desc = true;
    if (!piece && palette_ptr)  // else the shared +200 pointer
      for (const auto& kv : known_)
        if (kv.first != model && kv.second.palette_ptr == palette_ptr) { piece = true; break; }
  }
  if (by_desc) entry_pieces_desc_.fetch_add(1, std::memory_order_relaxed);
  (piece ? entry_pieces_ : entry_unknown_model_).fetch_add(1, std::memory_order_relaxed);
}

void WrestlerContributor::SubmitModelLink(uint32_t body_model, uint32_t key, uint32_t pach_buf, uint32_t pach_size, uint32_t desc,
                                          std::vector<PieceRef>&& pieces, uint32_t game_node_count, std::string composite_tag, uint32_t chosen_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  Known* k = KnownFor(body_model, false);
  // PZF2 A5: a full Known table (nothing evictable) no longer loses the link: the link is the body's, not the Known record's (PZ F5), and the pose hooks make the Known later. Counted as before.
  if (!k) link_unreadable_.fetch_add(1, std::memory_order_relaxed);
  // A model was (re)built at this address: whatever the table held for the old one (node count, palette pointer, slot, links, consumer freshness)
  // belongs to a dead object. The pose hooks refill it within a frame or two.
  DropPiecesLocked(body_model);
  cur_.erase(body_model);   // pending / kept palettes of the dead object (with its slot) must not be emitted for the new one
  last_.erase(body_model);
  if (k) {
    *k = Known{};
    k->touch = ++touch_seq_;
  }
  if (body_links_.size() >= kMaxBodyLinks && !body_links_.count(body_model)) {   // bounded: evict the least recently used link
    auto victim = body_links_.begin();
    for (auto b = body_links_.begin(); b != body_links_.end(); ++b)
      if (b->second.touch < victim->second.touch) victim = b;
    DropBodyLinkLocked(victim->first);
  }
  BodyLink& bl = body_links_[body_model];   // (DropPiecesLocked above emptied the old pieces; the old entry is replaced whole)
  bl = BodyLink{};
  bl.touch = ++touch_seq_, bl.made_seq = collect_seq_;
  bl.seen_count = game_node_count;   // PZF2 A2
  bl.info.known = true, bl.info.key = key, bl.info.pach_buf = pach_buf, bl.info.pach_size = pach_size, bl.info.desc = desc, bl.info.chosen = chosen_key;
  bl.info.tag = std::move(composite_tag);   // S103 CMB (bl was reset whole above: a relink at this address never keeps the old colours)
  link_bodies_.fetch_add(1, std::memory_order_relaxed);
  link_pieces_.fetch_add(pieces.size(), std::memory_order_relaxed);
  link_kind_[KindOfKey(key)].fetch_add(1, std::memory_order_relaxed);
  for (const PieceRef& p : pieces) piece_owner_[p.model] = body_model, ListAddLocked(p.model);
  bl.info.pieces = std::move(pieces);
  bl.pieces_dropped = false;   // (the entries just listed are the descriptor's own, read in this build)
}

bool WrestlerContributor::VerifyBodyLink(uint32_t skel, bool ok) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = body_links_.find(skel);
  if (it == body_links_.end()) return false;
  BodyLink& bl = it->second;
  if (!ok) {
    if (bl.verified_seq != ~0ull) body_link_unverified_.fetch_add(1, std::memory_order_relaxed);   // counted on the change, not per latch
    if (bl.verified_seq != ~0ull && bl.verified_seq != collect_seq_) bl.verified_prev = bl.verified_seq;   // S111 PERIODALL review R3: an earlier latch's pass stays (the record built now takes that latch's check)
    bl.verified_seq = ~0ull;
    if (!bl.info.pieces.empty()) DropPiecesLocked(skel);   // fail closed: not named, no pieces listed or proven for a body the game's descriptors no longer name this way
    return false;
  }
  if (bl.verified_seq != collect_seq_) {   // (a second check of the same latch changes nothing)
    if (bl.verified_seq != ~0ull) bl.verified_prev = bl.verified_seq;   // S111 PERIODALL review R3: the pass before (the record built now takes the check of the latch before)
    bl.verified_seq = collect_seq_;
  }
  return bl.pieces_dropped;
}

void WrestlerContributor::RelistPieces(uint32_t skel, std::vector<PieceRef>&& pieces) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = body_links_.find(skel);
  if (it == body_links_.end() || !it->second.pieces_dropped || it->second.verified_seq != collect_seq_) return;   // only a verified link whose pieces were dropped
  BodyLink& bl = it->second;
  for (const PieceRef& p : pieces) piece_owner_[p.model] = skel, ListAddLocked(p.model);
  bl.info.pieces = std::move(pieces);
  bl.pieces_dropped = false;
  body_link_relisted_.fetch_add(1, std::memory_order_relaxed);
}

WrestlerContributor::BodyKey WrestlerContributor::BodyKeyFor(uint32_t body_model) {
  std::lock_guard<std::mutex> lock(mutex_);
  BodyKey r;
  auto it = body_links_.find(body_model);
  if (it == body_links_.end() || !it->second.info.known) return r;
  r.known = true, r.verified = it->second.verified_seq == collect_seq_, r.desc = it->second.info.desc, r.key = it->second.info.key;
  return r;
}

WrestlerContributor::ModelKeyInfo WrestlerContributor::ModelKeyFor(uint32_t body_model) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = body_links_.find(body_model);
  return it == body_links_.end() ? ModelKeyInfo{} : it->second.info;
}

uint32_t WrestlerContributor::OwnerOfPiece(uint32_t piece_model) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto o = piece_owner_.find(piece_model);
  return o == piece_owner_.end() ? 0u : o->second;
}

void WrestlerContributor::SubmitAssetLink(uint32_t skel, const std::string& pac, const std::string& entry, uint32_t variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  Known* k = KnownFor(skel);
  if (!k || pac.empty()) { NoteUnreadable(); return; }
  k->declared.pac = pac, k->declared.entry = entry, k->declared.variant = variant, k->declared.known = true;
}

void WrestlerContributor::SubmitAssetGuess(uint32_t skel, const std::string& pac, const std::string& entry, uint32_t variant) {
  std::lock_guard<std::mutex> lock(mutex_);
  Known* k = KnownFor(skel);
  if (!k || pac.empty()) { NoteUnreadable(); return; }
  k->guessed.pac = pac, k->guessed.entry = entry, k->guessed.variant = variant, k->guessed.known = true;
  guessed_links_.fetch_add(1, std::memory_order_relaxed);
}

WrestlerContributor::Link WrestlerContributor::GuessFor(uint32_t skel) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = known_.find(skel);
  return it == known_.end() ? Link{} : it->second.guessed;
}

void WrestlerContributor::ApplyDelta(uint32_t skel, float dx, float dy, float dz) {
  std::lock_guard<std::mutex> lock(mutex_);
  // S103 W1 (docs\systems\wrestler_pose.md "The guard's head and sub_82BD3620"): the game adds (dx,dy,dz) to the translation of every node of +192 AND +200 (listing 0x82BD3634-0x82BD36DC and the loop to 0x82BD3834:
  // fadds into +48..+64 of each 64-byte matrix; r4 = the vector, read by the existing wwe2k14_skel_shift hook). skin = inv_bind x world and inv_bind's last column is (0,0,0,1), so a world shift is
  // exactly +d on the skin's row 3: a pending derived palette gets the same add below. (S111 HEADGUARD: the guard's head it also shifted is gone; S111 PIECELINK review: so is the summed shift of the own-rows row compare.)
  std::vector<Mat4>* skin = nullptr;
  auto p = cur_.find(skel);
  if (p != cur_.end() && !p->second.upds.empty()) {
    if (!p->second.upds.back().consumer) skin = &p->second.upds.back().skin;  // a consumer palette is what +200 held at the draw: the delta is the game's to have applied (or not) by then
  } else if (auto l = last_.find(skel); l != last_.end()) {
    if (!l->second.consumer) skin = &l->second.skin;
  }
  if (!skin) return;
  for (Mat4& m : *skin) m.m[12] += dx, m.m[13] += dy, m.m[14] += dz;
}

// S102 U1: the latch (tail_latch.h): the item list and the light set, taken while the game threads are parked; Emit then formats only these.
bool WrestlerContributor::Latch(const FrameInfo& f) {
  if (PreLatchFn pre = pre_latch_.load(std::memory_order_relaxed)) pre(f.guest_frame);   // S103 A2 (W1)
  latch_items_.clear();
  latch_counts_ = Counters{};
  Collect(f, latch_items_, latch_counts_);
  StoreVisible(latch_items_);
  latch_light_ = light_set_.load(std::memory_order_relaxed);
  latch_.Set(f.guest_frame);
  return true;
}

void WrestlerContributor::Emit(const FrameInfo& f, SceneTables& t, Counters& c) {
  using Item = EmitItem;
  std::vector<Item> items;
  uint32_t light = kUnknown;
  if (latch_.Use(f.guest_frame, Name())) {   // S102 U1
    items.swap(latch_items_);
    c.stale += latch_counts_.stale;
    c.unknown += latch_counts_.unknown;
    light = latch_light_;
  } else {
    // S111 LATCHMISS (docs\research\s111\LATCHMISS.md): no palette read here. Every production Emit follows its own Latch (live_scene.cpp LatchImpl: each contributor's Latch, then
    // BuildFrame; EndFrame takes that route since S103 D7; EmitLatched is the deferred half of the same latch), so only a caller that skipped the latch (a unit) reaches this branch.
    // The old S103 A3 read here ran with the game threads running (a torn +200): a read the game never makes. Collect uses the parked reads already submitted; the palette input
    // is not stamped (inputs_unit a3.palette_no_latch; its planted twin a3_no_prelatch makes the old read from the unit, so no fault bit lives here).
    Collect(f, items, c);
    StoreVisible(items);
    light = light_set_.load(std::memory_order_relaxed);
  }
  if (items.empty()) return;
  EmitItems(f, t, c, items, light);
}

// S103 V1F F1: EmitItems' claimant rule on the collected items (sorted by EmitItems' own key, emit_order.h; the first claim of each slot >= 0 is the visible one: the lowest address).
void WrestlerContributor::StoreVisible(const std::vector<EmitItem>& items) {
  std::vector<const EmitItem*> v;
  v.reserve(items.size());
  for (const EmitItem& i : items) v.push_back(&i);
  std::sort(v.begin(), v.end(), [](const EmitItem* a, const EmitItem* b) { return EmitBefore(EmitOrderOf(*a), EmitOrderOf(*b)); });   // S111 CHARORDER: the one key
  std::vector<uint32_t> out;
  std::set<int32_t> claimed;
  for (const EmitItem* i : v)
    if (i->slot >= 0 && !i->pieces_only && claimed.insert(i->slot).second) out.push_back(i->skel);   // S111 PIECEROUTE: a pieces-only owner is no drawn body
  std::lock_guard<std::mutex> lock(mutex_);
  visible_skels_.swap(out);
}

// S111 PIECECARRY (docs\research\s111\PIECECARRY.md): LatchTargets' rule for one SUBM piece, shared with NoteFreedLast.
WrestlerContributor::PieceLink WrestlerContributor::PieceLinkOf(const scene_inputs::CharPiece& p, int32_t slot) {
  return PieceLink{slot, p.id, p.owner, p.why, p.build, p.kind, p.probe, p.nodes <= scene_inputs::kSubmMaxNodes ? p.nodes : 0u};   // S105 A5a: + the copy probe
}

bool WrestlerContributor::PieceHasRows(const scene_inputs::CharPiece& p) {   // S103 W2B: a proven piece whose -1 nodes' rows were read
  return p.why == scene_inputs::PieceWhy::Ok && !p.free_nodes.empty() && p.rows.size() == p.free_nodes.size() * 16;
}

void WrestlerContributor::AppendPieceRows(const scene_inputs::CharPiece& p, std::vector<ExtraRow>& er) {   // S103 W2B: the piece's spring-hair rows, in reader order (SortExtraRows makes the canonical order)
  for (size_t k = 0; k < p.free_nodes.size(); ++k) {
    ExtraRow row;
    row.piece_id = p.id, row.node = p.free_nodes[k], row.piece_nodes = p.nodes;
    std::memcpy(row.m.m, p.rows.data() + 16 * k, sizeof row.m.m);
    er.push_back(row);
  }
}

void WrestlerContributor::SortExtraRows(std::vector<ExtraRow>& v) {
  std::stable_sort(v.begin(), v.end(), [](const ExtraRow& a, const ExtraRow& b) { return rdr_scene::ExtraKeyLess({a.piece_id, a.node}, {b.piece_id, b.node}); });
}

// S111 PIECECARRY: the freed body's last record (wrestler_contrib.h NoteFreedLast). The links are made as LatchTargets makes them, with the body's own Known::slot (the slot Collect gives its item, so
// EmitItems pairs each piece with it); `targets` follows LatchTargets' push rule for a linked piece of a table owner (a count, drawn by the game, an emit route that can use the palette).
void WrestlerContributor::NoteFreedLast(uint32_t skel, uint64_t hook_frame, const std::vector<scene_inputs::CharPiece>& pieces, std::vector<PaletteTarget>& targets) {
  targets.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  auto kn = known_.find(skel);
  if (kn == known_.end() || kn->second.slot < 0) return;   // not a slot body: its pieces have no owner item to pair with
  if (freed_last_pending_.size() >= kMaxSkeletons && !freed_last_pending_.count(skel)) return;   // bounded (Collect empties it)
  const int32_t slot = kn->second.slot;
  FreedLast& fl = freed_last_pending_[skel];
  fl = FreedLast{};
  fl.hook = hook_frame;
  for (const scene_inputs::CharPiece& p : pieces) {
    if (p.owner != skel || p.model == skel || p.model < 0x10000) continue;
    const PieceLink pl = PieceLinkOf(p, slot);
    fl.links[p.model] = pl;
    if (PieceHasRows(p)) AppendPieceRows(p, fl.rows);
    if (pl.count && pl.probe.drawn != 0 && (pl.why == scene_inputs::PieceWhy::Ok || pl.why == scene_inputs::PieceWhy::NoCopy || !RecordCanNameL(skel)))
      targets.push_back(PaletteTarget{p.model, std::min<uint32_t>(pl.count, 1024)});
  }
  SortExtraRows(fl.rows);
}

// S111 MATCHFREE (docs\research\s111\MATCHFREE.md): the freed bodies this record still shows. A body comes back when its copy's newest draw ask is of this record's period or later and AskedFor
// holds (the walk this record pairs with drew it); a piece when the links of this record's latch name a revived body as its owner. A copy whose address holds a live entry again yields (a new
// model there). Every other copy is dropped: after its free the game never asks for it again, so no later record can show it.
// S111 PIECECARRY (docs\research\s111\PIECECARRY.md): a revived body whose copy carries its last record (char_release.h: the reads latch X would have made of it) labelled with the latch this record
// takes its links and rows from (RecordLatchHookL: latch X for record X + 1) is served WHOLE from it: its pieces' links join the record's (`serve.links`; a link the record's latch read itself wins)
// and its spring-hair rows are the body's (`serve.rows`, attached in Collect when its palette is that latch's read). One read, one label: a piece pairs with its owner within it.
std::vector<uint32_t> WrestlerContributor::FreedRevive(uint64_t record_frame, FreedServe& serve) {
  std::vector<uint32_t> revived;
  freed_last_pending_.clear();   // S111 PIECECARRY: a record no ForgetModel took before this Collect is no freed body's: dropped (a model the character destructor did not delete, sub_825A1510 +160 / sub_823CCDD0 +4C, and that no later free of the same tick took: the descriptor pass sub_8258C4E8 runs after the character deletes, sub_82512B80 +30 then +44)
  if (freed_copies_.empty()) return revived;
  const uint64_t hook = record_frame ? record_frame - 1 : 0;
  std::set<uint32_t> bodies;
  for (const auto& kv : freed_copies_)
    if (kv.second.has_known && kv.second.known.draw_frame >= hook && AskedFor(kv.second.known, hook)) bodies.insert(kv.first);
  const uint64_t latch_hook = RecordLatchHookL(record_frame);
  for (uint32_t b : bodies) {
    const FreedCopy& fc = freed_copies_.find(b)->second;
    if (!fc.has_last || latch_hook == ~0ull || fc.last.hook != latch_hook) continue;
    if (!serve.on) {
      serve.on = true;
      serve.hook = latch_hook;
      serve.links = LinksOfRecordL(record_frame);
    }
    for (const auto& kv : fc.last.links) serve.links.emplace(kv.first, kv.second);
    if (!fc.last.rows.empty()) serve.rows[b] = fc.last.rows;
  }
  const auto& links = *(serve.on ? &serve.links : &LinksOfRecordL(record_frame));
  for (auto it = freed_copies_.begin(); it != freed_copies_.end();) {
    const uint32_t m = it->first;
    const auto pl = links.find(m);
    const bool want = bodies.count(m) || (pl != links.end() && bodies.count(pl->second.owner));
    if (!want || (faults_ & kFaultFreedNoKeep) || last_.count(m) || cur_.count(m) || known_.count(m)) { it = freed_copies_.erase(it); continue; }
    FreedCopy& fc = it->second;
    if (bodies.count(m)) {   // S111 MATCHFREE review: a body the record shows with no palette of its period (FreedUnposed)
      bool posed = false;
      for (const Upd& u : fc.pend.upds) posed = posed || u.frame <= PickLimit(record_frame);
      if (!posed) freed_unposed_.fetch_add(1, std::memory_order_relaxed);
    }
    if (fc.has_kept) last_[m] = fc.kept;
    if (!fc.pend.upds.empty()) cur_[m] = fc.pend;
    if (fc.has_known) known_[m] = fc.known;
    revived.push_back(m);
    ++it;
  }
  if (!revived.empty()) freed_served_.fetch_add(1, std::memory_order_relaxed);
  return revived;
}

void WrestlerContributor::FreedStow(const std::vector<uint32_t>& revived) {
  for (uint32_t m : revived) {
    auto it = freed_copies_.find(m);
    if (it == freed_copies_.end()) continue;
    FreedCopy& fc = it->second;
    if (auto k = last_.find(m); k != last_.end()) fc.kept = std::move(k->second), fc.has_kept = true, last_.erase(k);
    else fc.has_kept = false;
    if (auto p = cur_.find(m); p != cur_.end()) fc.pend = std::move(p->second), cur_.erase(p);
    else fc.pend.upds.clear();
    if (auto kn = known_.find(m); kn != known_.end()) fc.known = kn->second, fc.has_known = true, known_.erase(kn);
    quiet_last_.erase(m);
  }
}

// Emit's two locked blocks (unchanged; S102 U1 moved them here so the latch can run them before the update thread is released).
void WrestlerContributor::Collect(const FrameInfo& f, std::vector<EmitItem>& items, Counters& c) {
  using Item = EmitItem;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    FreedServe freed_serve;   // S111 PIECECARRY: the revived bodies' last records served with their copies (links, rows)
    const std::vector<uint32_t> freed_revived = FreedRevive(f.guest_frame, freed_serve);   // S111 MATCHFREE: a body freed after the walk this record pairs with drew it, back for this block only
    std::set<uint32_t> refreshed;
    for (auto it = cur_.begin(); it != cur_.end();) {
      auto& u = it->second.upds;
      int pick = -1;  // the newest update at or before this record's frame
      for (size_t i = 0; i < u.size(); ++i)
        if (u[i].frame <= PickLimit(f.guest_frame)) pick =  /* S103 W1: record j takes the palette of stream j (tag <= j - kPaletteLead), not stream j + 1 */ (pick < 0 || u[i].frame >= u[size_t(pick)].frame) ? int(i) : pick;
      if (pick >= 0) {
        Kept& k = last_[it->first];
        k.slot = u[size_t(pick)].slot;
        k.consumer = u[size_t(pick)].consumer;
        k.skin = std::move(u[size_t(pick)].skin);
        k.frame = u[size_t(pick)].frame;   // S103 HGF
        k.age = 0;
        refreshed.insert(it->first);
        const uint64_t picked_frame = u[size_t(pick)].frame;
        u.erase(std::remove_if(u.begin(), u.end(), [&](const Upd& x) { return x.frame <= picked_frame; }), u.end());
      }
      it = u.empty() ? cur_.erase(it) : std::next(it);
    }
    const uint64_t hook = f.guest_frame ? f.guest_frame - 1 : 0;   // the record's hook frame (record j carries hook j - 1: inputs_core.h kHookToRecord)
    const auto& links = *(freed_serve.on ? &freed_serve.links : &LinksOfRecordL(f.guest_frame));   // S111 PERIODALL: the piece links of the latch whose palettes this record takes (S111 PIECECARRY: + a served freed body's own)
    const uint64_t name_seq = VerdictSeqOfRecordL(f.guest_frame);   // S111 PERIODALL review R3: and the descriptor check of that latch (a body link names its body by it)
    for (auto it = last_.begin(); it != last_.end();) {
      bool stale = false;
      if (!refreshed.count(it->first)) {
        ++it->second.age;
        if (it->second.age > kPruneAfter) { it = last_.erase(it); continue; }
        ++c.stale;  // per skeleton not refreshed since the last record
        stale = true;
      }
      if (std::binary_search(ring_models_.begin(), ring_models_.end(), it->first)) {   // S103 NRBF: a ring model (the ring class table at G): the ring family draws it, not a wrestler object
        ring_left_out_.fetch_add(1, std::memory_order_relaxed);
        ++it;
        continue;
      }
      Link link;
      int32_t slot = it->second.slot;
      change_colour::Body cc_body;   // S108 THR2: the body's change-colour state as the game's draw left it (NoteDrawAsked); on = the game draws it recoloured
      bool cc_on = false;
      bool game_undrawn = false;   // S111 PIECEHOLES: the quiet gate below hid this body (the game did not ask to draw it in the record's period): EmitItems writes kObjGameUndrawn, so the draw side never counts it a hole
      int32_t hid_slot = -1;   // S111 PIECEROUTE: the slot the quiet gate took (Collect may give it back, pieces only)
      if (auto kn = known_.find(it->first); kn != known_.end()) {
        kn->second.age = 0;
        if (kn->second.slot >= 0) slot = kn->second.slot;  // the slot the update hook matched wins (a consumer palette carries none)
        // S103 L31B review C: a hook-quiet body is shown only while the game's own draw saw it lately; else emitted invisible = a hole (the frame goes to the game), the palette still in the record
        // (only a REFRESHED body: a stale visible one is the guard's, below, so a body that left the table is dropped, not turned into a 120-record hole)
        // S107 CAM: a body shows only while the game ASKED to draw it (draw_frame: the sub_82C04DF0 hook): the editor's extra slots are posed by the skeleton update but never asked, and lay on the mat.
        // S111 CAWTPOSE (HEADGUARD F1): ONE rule in both builds. The draw ask is taken in either build, before the Ours test (draw_origin.h REX_HOOK_RAW(sub_82C04DF0)), so the stream build's older
        // update / walk rule (kept for "un-asked commentators": a slot character is drawn through the same character node and model draw, so it is asked) only let a body that left the table, its hook
        // quiet, draw again as actor:slot:N from its old Known::slot. kFaultUpdShows = that older rule (the twin).
        const bool seen = !(faults_ & kFaultUpdShows) ? AskedFor(kn->second, hook)   // S111 VISLATCH: the asks / hides of the period this record's palette is from (record f pairs with stream f = the walk of period f - 1)
                                                      : (kn->second.upd_frame + kConsumerFresh >= hook || kn->second.walk_frame + kQuietSeenWindow >= hook || AskedFor(kn->second, hook));   // review 1b: a draw ask comes every guest frame while drawn, so its window is short (a hidden body the game stops asking about goes quiet within kConsumerFresh)   // S103 DRL: the game's draw ask
        if (!stale && slot >= 0 && !(faults_ & kFaultNoWalkGate) && !seen) {
          by_slot_quiet_unseen_[SlotBin(slot)].fetch_add(1, std::memory_order_relaxed);   // S103 AUSF
          if (uint64_t& ql = quiet_last_[it->first]; ql == 0 || f.guest_frame != ql + 1) EventLog(2, f.guest_frame, it->first, slot, kn->second.upd_frame, kn->second.draw_frame, 0);   // on change: the first record of a run of hides
          quiet_last_[it->first] = f.guest_frame;
          hid_slot = slot;   // S111 PIECEROUTE
          slot = -1;
          game_undrawn = true;
          quiet_unseen_.fetch_add(1, std::memory_order_relaxed);
        }
        auto bl = body_links_.find(it->first);   // S103 PZ F5: the body's declared link (outlives the Known record)
        // PZF2 A1: the link names the body only while a descriptor check passed (inputs_contrib.h ReadSlotSkeletons -> VerifyBodyLink): links never age, so identity is re-proven every latch.
        // S111 PERIODALL review R3: the check of the latch whose palettes this record takes (name_seq), not this latch's: the walk of that period drew the body this record shows.
        const bool has_link = bl != body_links_.end() && bl->second.info.known && (VerifiedAtSeq(bl->second, name_seq) || (faults_ & kFaultLinkInKnown));
        if (has_link) bl->second.touch = ++touch_seq_;
        if (has_link && !kn->second.seen_as_skeleton) {
          kn->second.seen_as_skeleton = true;  // desc+4 (the body model) turned up as a skeleton the pose hooks update: the identity holds for this body
          linked_seen_as_skel_.fetch_add(1, std::memory_order_relaxed);
        }
        link = kn->second.declared;  // a guess never enters the record
        if (!link.known && has_link) {
          const uint32_t model_key = bl->second.info.key;
          // The game's model key is [C]; the pac NAME is [U] (R1): a pac-less AssetRef (pac 0 = unknown) whose entry names the key and whose variant is the attire.
          // S103 B: the pac is named from the key (character = key / 100; pac\ch\chNNN.pac holds every attire of the character [L: 187 = the ring announcer and 150 / 153 =
          // the commentators in db\ids\characters.json; the viewer loads wrestler N attire A from that pac]): a drawer needs no side table. The key stays in `entry`.
          link = KeyLink(model_key, BuildTagOfLink(bl->second.info), bl->second.info.desc);   // S110 SEATREL: + the descriptor whose build note names the file. S103 CMB: a composite attire's tag rides in the body's own link; a composite attire never falls back to the plain slot key (below)
        } else if (!link.known && kn->second.slot_key && !(bl != body_links_.end() && !bl->second.info.tag.empty())) {   // S103 CMB: a body whose link carries a composite tag (verification pending / failed this latch) goes out unnamed: a hole, never the stock attire
          // S103 B: no declared link: the participant slot key
          link = SlotKeyLink(kn->second.slot_key, kn->second.slot_desc, kn->second.model_desc);   // S110 SEATREL: + the slot's descriptor. S103 ARM2: + the build step's colours when the game composed this attire. S110 ENTRBODIES: the rule the start's early ask shares
        }
        // S108 THR2: a body the game draws recoloured (the Threads editor) is named with the `/m` tag: the loader keeps its PACH's four masks (change_mask.h), a load cached apart
        // from the plain one. Before the failed-load test below, so that test asks about the entry this record really names. A refusal entry ("/r") is never tagged (it stays a hole).
        cc_on = change_colour::Bodies().Get(it->first, hook, cc_body);
        if (cc_on && link.known && !attire_compose::IsRefusalTag(link.entry)) link.entry = change_mask::WithTag(link.entry);
        if (cc_on && !link.known) rdr_scene::change_colour_draw::Draws().unnamed.fetch_add(1, std::memory_order_relaxed);   // a recoloured body the record cannot name (guest rows): no masks, drawn plain
        // S103 DLW (COV2 GAP 2 part 3): a link whose model the loader could not load is not written: the object goes out unnamed (pac 0) and the GuestModel rows draw it (RecordCanName says the same).
        if (link.known && !(faults_ & kFaultNamedFailHole) && rdr_scene::namedfail::FailedToGuest(link.pac, link.entry, link.variant)) {
          link = Link();
          rdr_scene::namedfail::CountFallback();
        }
      }
      // S103 L31B (the interim guard, review B): in mode Ours a VISIBLE body (resolved slot >= 0) is never written with an old palette (kept, aged, pruned as before). A stale INVISIBLE one (a piece,
      // a skel:) is still emitted: rf_wrestlers counts it a hole, so the frame is never ours while it is missing.
      // S111 FOLLOWUPS3 (docs\research\s111\FOLLOWUPS3.md part 3): no pose hold, in either build. S103 AUSF held such a body for up to kConsumerFresh records in the build with no walk, for the latch reads
      // the derived-head guard refused (S103 AUS2); S111 HEADGUARD removed that guard. The game keeps one palette buffer per built body ([skel+200]: the ctor zeroes it, the build sets it, the release frees
      // and zeroes it), so a stale record here is our own transition, not a state the game draws; the tip census never saw the hold fire (stale_held 0 in every run log of 3 tips).
      if (stale && stale_guard_ && scene_frame::CurrentMode() == scene_frame::Mode::Ours && (slot >= 0 || (faults_ & kFaultDropInvisible))) {
        stale_dropped_.fetch_add(1, std::memory_order_relaxed);
        by_slot_stale_dropped_[SlotBin(slot)].fetch_add(1, std::memory_order_relaxed);
        if (it->second.age <= 1) EventLog(0, f.guest_frame, it->first, slot, it->second.age, it->second.frame, 0);   // the line that names the moment a visible body leaves the record (the glue caps it)
        ++it;
        continue;
      }
      items.push_back(Item{it->first, slot, it->second.consumer, link, it->second.skin});
      items.back().stale = stale;   // S103 NRB
      items.back().game_undrawn = game_undrawn;   // S111 PIECEHOLES
      items.back().hid_slot = hid_slot;   // S111 PIECEROUTE
      if (cc_on) {   // S108 THR2: the colours ride with the body to EmitItems (section 67)
        items.back().cc_on = true, items.back().cc_unsettled = cc_body.unsettled;
        std::memcpy(items.back().cc_colours, cc_body.colours, sizeof items.back().cc_colours);
      }
      items.back().tag = it->second.frame;   // S105 A5a: the palette's frame tag (the own-rows rule pairs a piece with its owner only within one latch)
      if (!stale) {   // S103 W2B: the body's spring-hair rows
        // S103 HGF (HGP.md section 3 / 6, review 1): the rows must come from the SAME latch as the body palette. A latch consumer palette read at hook H is tagged H + kPaletteLead; in record R Collect can only pick a tag <= R - kPaletteLead,
        // i.e. the PREVIOUS latch's palette, whose rows are the set before the current one. Attach the set whose hook + kPaletteLead equals the picked tag; neither = no extras this record (absent, never skewed), counted. A derived
        // (update-hook) palette carries another thread's tag: nothing to compare, the current set as before.
        const std::unordered_map<uint32_t, std::vector<ExtraRow>>* set = nullptr;
        const Kept& kp = it->second;
        if ((faults_ & kFaultHairAnyFrame) || !kp.consumer) set = &piece_rows_;
        else if (faults_ & kFaultHairCurrentSet) set = kp.frame == piece_rows_frame_ + kPaletteLead ? &piece_rows_ : nullptr;
        else if (freed_serve.rows.count(it->first) && kp.frame == freed_serve.hook + kPaletteLead) set = &freed_serve.rows;   // S111 PIECECARRY: a freed body's own last record (read at its destructor, labelled with the latch it stands in for)
        else if (kp.frame == piece_rows_prev_frame_ + kPaletteLead) set = &piece_rows_prev_;
        else if (kp.frame == piece_rows_frame_ + kPaletteLead) set = &piece_rows_;
        if (set) {
          if (auto pr = set->find(it->first); pr != set->end()) items.back().extra = pr->second;
        } else if (rdr_scene::g_spring_hair_rows.load(std::memory_order_relaxed) && (!piece_rows_.empty() || !piece_rows_prev_.empty())) {
          rdr_scene::g_spring_census.rows_frame_skew.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (auto pl = links.find(it->first); pl != links.end() && slot < 0) items.back().piece_slot = pl->second.slot, items.back().piece_owner = pl->second.owner, items.back().piece_why = pl->second.why, items.back().piece_id = pl->second.id, items.back().probe = pl->second.probe;   // S105 A5a: + the copy probe; S111 PERIODALL: the links of the palette's latch
      ++it;
    }
    // S103 DTP fix (review): the owner-mesh skip follows the latch's link proof (copy word == 0), not the piece object's palette. After the ELBW ends the game sets only +20 = 0 (the SUBM draw loop skips it) and
    // never puts +24 back, so the pad's palette stops, goes stale and is pruned: the merged owner mesh must not draw it on the arm again. A NoCopy link with no item of its own is written link-only (no palette).
    // S111 SOCKPIECE (piece_hidden.h): a link the game HIDES (instance +20 == 0) has no item either (LatchTargets does not push it), so the owner's merged mesh would draw it: it is written link-only too,
    // with the link's probe, and EmitItems turns it into a kObjPieceHidden object (the owner's alpha-0 row). kFaultHideIgnored = the old rule (the twin).
    if (!links.empty()) {   // S111 PERIODALL: the same links as the pieces above
      std::set<uint32_t> have;
      for (const Item& it2 : items) have.insert(it2.skel);
      for (const auto& pl : links) {
        const bool nocopy_link = pl.second.why == scene_inputs::PieceWhy::NoCopy && !(faults_ & kFaultDetachNeedsFresh);
        const bool hidden_link = pl.second.probe.drawn == 0 && !(faults_ & kFaultHideIgnored);
        if ((!nocopy_link && !hidden_link) || pl.second.slot < 0 || have.count(pl.first) || std::binary_search(ring_models_.begin(), ring_models_.end(), pl.first)) continue;
        Item li{pl.first, -1, false, Link(), {}};
        li.piece_slot = pl.second.slot, li.piece_owner = pl.second.owner, li.piece_why = pl.second.why, li.piece_id = pl.second.id, li.link_only = true, li.stale = true, li.probe = pl.second.probe;
        items.push_back(std::move(li));
      }
    }
    FreedStow(freed_revived);   // S111 MATCHFREE: out of every list a guest reader walks again, before the lock goes
  }
  {  // bookkeeping of skeletons that are in neither cur_ nor last_ ages out (a reused address must not inherit a stale count / link)
    std::lock_guard<std::mutex> lock(mutex_);
    ++collect_seq_;
    for (auto it = known_.begin(); it != known_.end();) {
      if (cur_.count(it->first) || last_.count(it->first)) { ++it; continue; }
      if (++it->second.age > kKnownAge) {
        if (auto bl = body_links_.find(it->first); bl != body_links_.end()) {   // S103 PZ F5: a model-linked Known goes; the link stays with the body (the log line settles PZ's open [U])
          const uint64_t n = known_erased_linked_.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 32)
            if (KnownEraseLogFn f = known_erase_log_.load(std::memory_order_relaxed)) f(it->first, bl->second.info.key, it->second.age, collect_seq_ - bl->second.made_seq);
          if (faults_ & kFaultLinkInKnown) DropBodyLinkLocked(it->first);   // planted twin: today's rule
          else DropPiecesLocked(it->first);   // PZF2 A3: the NAME stays with the body, the listed pieces go with the Known: re-listed from [desc + 12] when a latch's descriptor check passes (A1)
        }
        it = known_.erase(it);
      } else {
        ++it;
      }
    }
  }
  {   // S111 CAT33: each slot claimant's place in the game's category-33 list (LatchTargets' draw_pos_; -1 = not listed), for EmitItems' order
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t rec_hook = f.guest_frame ? f.guest_frame - 1 : 0;   // S111 OWNERLESS: the walk this record shows = the latch labelled with the record's hook
    const DrtrOff* shown = nullptr;
    for (const DrtrOff& d : drtr_off_)
      if (d.label != 0 && d.label == rec_hook) shown = &d;
    if (faults_ & kFaultDrtrNewest) shown = drtr_off_[0].label != 0 ? &drtr_off_[0] : nullptr;   // the planted twin: the newest latch decides
    const auto has = [](const std::vector<uint32_t>& v, uint32_t k) { return std::find(v.begin(), v.end(), k) != v.end(); };
    for (Item& i : items) {   // S111 PIECEROUTE (slot22_pieces.h): a quiet-hidden named body whose slot 22 drew its SUBM pieces in the walk this record shows is its slot's body again, pieces only
      if (i.hid_slot < 0) continue;
      ::rdr_scene::live::slot22::OwnerCase oc;
      oc.hid_slot = i.hid_slot, oc.shown_read = shown != nullptr, oc.drtr_on = shown && has(shown->on, i.skel), oc.gates = shown && has(shown->pieces, i.skel), oc.named = i.link.known && !i.link.pac.empty();
      for (const Item& o2 : items) oc.slot_taken = oc.slot_taken || (&o2 != &i && o2.slot == i.hid_slot);
      if (::rdr_scene::live::slot22::PiecesOnly(oc, (faults_ & kFaultPiecesNoGates) != 0)) i.slot = i.hid_slot, i.pieces_only = true;
    }
    for (Item& i : items) {
      i.draw_pos = -1;
      if (i.slot >= 0)
        for (const auto& d : draw_pos_)
          if (d.first == i.skel) { i.draw_pos = d.second; break; }
      i.owner_trans_off = i.piece_slot >= 0 && i.piece_owner != 0 && shown && has(shown->off, i.piece_owner);   // S111 OWNERLESS
    }
  }
  c.unknown += uint32_t(unreadable_.exchange(0, std::memory_order_relaxed));
}

// Emit's formatting (unchanged; S102 U1: reads only `items` and `light`, never the hooks' shared state).
void WrestlerContributor::EmitItems(const FrameInfo& f, SceneTables& t, Counters& c, std::vector<EmitItem>& items, uint32_t light) {
  (void)f;
  using Item = EmitItem;
  // S111 CHARORDER (emit_order.h): character by character (S111 CAT33: in the order of the game's own category-33 list, SlotDrawRanksOf; the unlisted after, in slot order); inside a slot each
  // claimant (by address) followed by its own pieces (piece_owner; the game's SUBM order), then the skeletons of no slot by address. The colour pass keeps this order for the translucent parts.
  const SlotDrawRanks ranks = SlotDrawRanksOf(items);
  std::sort(items.begin(), items.end(), [&ranks](const Item& a, const Item& b) { return EmitBefore(EmitOrderOf(a, ranks), EmitOrderOf(b, ranks)); });

  // One placeholder AssetRef for "asset unknown" (pac 0 = none): the record's reader requires every Object::asset to be in range.
  // A drawer skips an AssetRef with pac 0 (rsc_draw does).
  // S101 lane 43: the engine-object column is parallel to objects; pad it for the classes before this one (live_scene.cpp pads after us too).
  t.engine_objects.resize(t.objects.size(), 0u);
  const uint32_t unknown_asset = uint32_t(t.assets.size());
  AssetRef none;
  none.kind = uint8_t(AssetKind::Model);
  none.variant = 0xFFFFFFFFu;
  t.assets.push_back(none);
  // (S102 U1: `light` is the latched / read light set, a parameter)
  std::set<int32_t> used_slots;
  std::map<std::string, uint32_t> linked;
  // S103 NRB / NRBF (docs\research\s103\NRB_pieces.md section 6): the game's SUBM component update copies the OWNER skeleton's rows into each piece instance through a name-built node map
  // (sub_8259DA88 -> sub_8259D230), so our merged body model (the owner's PACH entry, every piece mesh with the right bones) already draws a mapped piece. A linked piece is therefore "drawn by its
  // owner" (kObjDrawnByOwner, still invisible, no model of its own: rf_wrestlers decides whether it is a hole) only when ALL of: the reader proved the instance (PieceWhy::Ok: map complete, deferred
  // build, no special id, copy on), the slot's drawn owner body is in this record AND is the skeleton the link was read from ([char+48], review F1), and the piece's palette is this record's.
  // Anything else stays an ordinary invisible skel: object (a hole while the game draws it), counted by reason.
  std::map<int32_t, const Item*> owner_of;   // slot -> the body drawn as actor:slot:N (the slot's first claimant)
  {
    std::set<int32_t> seen;
    for (const Item& s : items)
      if (s.slot >= 0 && seen.insert(s.slot).second) owner_of[s.slot] = &s;
  }
  for (const Item& s : items) {
    // A slot claimed twice in one record: the first claim keeps "actor:slot:N", a second becomes a skel: id (and invisible).
    const bool slot_ok = s.slot >= 0 && used_slots.insert(s.slot).second;
    char id[40];
    if (slot_ok) std::snprintf(id, sizeof(id), "actor:slot:%d", s.slot);
    else std::snprintf(id, sizeof(id), "skel:%08X", s.skel);
    Object o;
    o.asset = unknown_asset;
    const Link& link = s.link;
    bool by_owner = false, detached = false, own_rows = false, own_refused = false;   // S105 A5a: own_rows = kObjOwnRowsPiece (below); S111 PIECELINK: own_refused = kObjOwnRowsRefused
    bool piece_undrawn = false;   // S111 OWNERLESS: written with kObjGameUndrawn (below)
    bool game_hidden = false;     // S111 SOCKPIECE (piece_hidden.h): written kObjPieceHidden (below): the game hides this piece of a named owner, so the owner's merged mesh must skip it
    if (!slot_ok && s.piece_slot >= 0) {   // S103 NRBF
      scene_inputs::PieceWhy why = s.piece_why;
      const auto ow = owner_of.find(s.piece_slot);
      game_hidden = !(faults_ & kFaultHideIgnored) && rdr_scene::OwnerMeshHidesPiece(s.probe.drawn, s.piece_id, ow != owner_of.end() && ow->second->skel == s.piece_owner, ow != owner_of.end() && ow->second->link.known && !ow->second->link.pac.empty());
      if (game_hidden) piece_game_hidden_.fetch_add(1, std::memory_order_relaxed);
      if (why == scene_inputs::PieceWhy::Ok) {
        if (ow == owner_of.end()) why = scene_inputs::PieceWhy::OwnerNotDrawn;
        else if (ow->second->skel != s.piece_owner) why = scene_inputs::PieceWhy::OwnerMismatch;
        else if (s.stale) why = scene_inputs::PieceWhy::Stale;
      }
      by_owner = why == scene_inputs::PieceWhy::Ok && !game_hidden;
      // S103 DTP (PEP.md section 3): the game set the piece's owner-copy flag to 0 and poses the piece from its own palette (the People's Elbow pad). Owner identity and a fresh palette are the same proof as above;
      // only the copy flag differs. The class is any NoCopy piece, not only ids 26 / 27. Off by --wwe2k14_rdr_detached_pieces=false: the piece stays a hole.
      const bool hidden_nocopy = why == scene_inputs::PieceWhy::NoCopy && s.probe.drawn == 0;   // S105 CAWP F1: the game hides it (inst +20 == 0): not drawn, never a hole
      detached = !hidden_nocopy && why == scene_inputs::PieceWhy::NoCopy && ow != owner_of.end() && ow->second->skel == s.piece_owner && s.piece_id != 0 && !((faults_ & kFaultDetachNeedsFresh) && s.stale) && rdr_scene::g_detached_pieces.load(std::memory_order_relaxed);   // S103 DTP fix: no freshness test (the flag follows the link proof; a stale / gone palette is written without bones, see link_only below); no piece id = never flagged (a flag with no pad is unusable)
      if (detached) piece_detached_.fetch_add(1, std::memory_order_relaxed);
      piece_why_[size_t(why)].fetch_add(1, std::memory_order_relaxed);
      if (hidden_nocopy && !game_hidden) continue;
      if (!by_owner && !detached && !s.link_only && !game_hidden) {   // S105 A5a (A5a_ADV.md site 7): a linked own-skeleton piece of a GUEST-ROW owner (a created superstar's hair / necklace / braces) is drawn from its own latch-read palette (own_rows_rule.h: a fresh read of its owner's latch)
        scene_inputs::OwnRowsCase oc;
        oc.probe = s.probe;
        oc.owner_found = ow != owner_of.end();
        if (oc.owner_found) {
          const Item& own = *ow->second;
          oc.owner_skel_matches = own.skel == s.piece_owner;
          oc.owner_named = own.link.known && !own.link.pac.empty();   // the value this record writes for the owner's asset (pac 0 = unnamed): a guest body
          oc.owner_consumer = own.consumer, oc.owner_tag = own.tag;
        }
        oc.piece_consumer = s.consumer, oc.piece_stale = s.stale, oc.piece_tag = s.tag;
        const scene_inputs::OwnRowsWhy ow_why = scene_inputs::JudgeOwnRows(oc, (faults_ & kFaultNoCopyProof) != 0);
        own_rows_[size_t(ow_why)].fetch_add(1, std::memory_order_relaxed);
        if (ow_why == scene_inputs::OwnRowsWhy::Hidden) continue;   // the game's SUBM loop skips this instance (inst +20 == 0): no object, not a hole
        own_rows = ow_why == scene_inputs::OwnRowsWhy::Ok;
        own_refused = scene_inputs::OwnRowsRefusalCarried(ow_why) && !(faults_ & kFaultRefusedUnlinked);   // S111 PIECELINK: a guest-row owner's piece the proof refused keeps its owner link (pad / layer below): counted on the draw side, never a hole
      }
      // S108 THR2: a piece of a recoloured owner drawn as its own object has no colours on the draw side (only the owner's merged model draws with them): counted + reported
      // (change_colour_piece) until the piece path carries them (lane RDS's files).
      if ((detached || own_rows) && ow != owner_of.end() && ow->second->cc_on) change_colour::Bodies().NotePieceApart(s.skel, ow->second->skel);
      // S111 OWNERLESS (docs\research\s111\OWNERLESS.md): the owner is no slot item because the quiet gate hid it (no draw ask in this record's period: no opaque node drew it, and with it the pieces that node
      // draws), AND its translucent DRTR node was off for that walk (no slot-22 piece draw). The game drew none of these pieces: not a hole. With the DRTR node on the game draws them without the body
      // (sub_829D2750 / sub_833683F8 / sub_8259FAE8 read no char+1208): that stays a counted hole (no route draws pieces of a hidden owner).
      if (ow == owner_of.end() && s.owner_trans_off)
        for (const Item& own : items)
          if (own.skel == s.piece_owner && own.game_undrawn) { piece_undrawn = true; break; }
    }
    if (s.link_only && !detached && !game_hidden) continue;   // S103 DTP fix: a link-only entry is written only as a detached piece (its owner is drawn); nothing else to say about a model with no palette   // S111 SOCKPIECE: or as a hidden one
    const bool no_palette = (detached && (s.stale || s.link_only)) || game_hidden;   // S103 DTP fix: the piece's own palette is gone or old: the flag (the owner's skip) stays, no bones are written, rf_wrestlers never guest-routes it   // S111 SOCKPIECE: a hidden piece writes no bones (nothing of it is drawn)
    if (link.known) {  // declared link (the char model reader): one AssetRef per distinct (pac, entry, variant) in this record
      const std::string key = link.pac + '\n' + link.entry + '\n' + std::to_string(link.variant);
      auto f2 = linked.find(key);
      if (f2 == linked.end()) {
        AssetRef a;
        a.kind = uint8_t(AssetKind::Model);
        a.pac = t.AddString(link.pac);
        a.entry = t.AddString(link.entry);
        a.variant = link.variant;
        f2 = linked.emplace(key, uint32_t(t.assets.size())).first;
        t.assets.push_back(a);
      }
      o.asset = f2->second;
    }
    o.klass = rdr_adapter::DrawClass::ModelPart;
    // A skeleton that matches no character slot (managers, waiting entrance models, extras; size filter only) is not shown: visible only with a slot. (S103 NRBF: a proven linked piece stays
    // invisible, with kObjDrawnByOwner: its owner's body draws it.)
    o.flags = (no_palette ? 0u : uint32_t(kObjSkinned)) | ((slot_ok && !s.pieces_only) ? uint32_t(kObjVisible) : 0u) | ((slot_ok && s.pieces_only) ? uint32_t(kObjPiecesOnly) : 0u) | (by_owner ? uint32_t(kObjDrawnByOwner) : 0u) | (detached ? uint32_t(kObjDetachedPiece) : 0u) | (own_rows ? uint32_t(kObjOwnRowsPiece) : 0u) | (own_refused ? uint32_t(kObjOwnRowsRefused) : 0u) | ((s.game_undrawn || piece_undrawn) ? uint32_t(kObjGameUndrawn) : 0u) | (game_hidden ? uint32_t(kObjPieceHidden) : 0u);   // S105 A5a; S111 PIECELINK; S111 PIECEHOLES; S111 OWNERLESS; S111 PIECEROUTE (kObjPiecesOnly: the owner's merged model draws only its pieces); S111 SOCKPIECE
    if (by_owner || detached || own_rows || own_refused || game_hidden) o.pad = uint16_t(s.piece_id), o.layer = uint8_t(s.piece_slot + 1);   // PZF2 B-1 / EB: the draw side asks the owner's resident model for a mesh of THIS piece id (scene_record.h kObjDrawnByOwner); S111 PIECELINK: a refused own-rows piece carries its owner the same way; S111 SOCKPIECE: a hidden piece names the mesh its owner skips
    o.id = t.AddString(id);
    o.world = IdentityMat4();  // the palette is already in world space (the root matrix is folded in by sub_82BD30E8); unconfirmed against the front-end
    o.bones_first = uint32_t(t.bones.size());
    o.bones_count = no_palette ? 0u : uint32_t(s.skin.size());
    if (!no_palette) t.bones.insert(t.bones.end(), s.skin.begin(), s.skin.end());
    if (!no_palette && slot_ok && !s.extra.empty() && rdr_scene::g_spring_hair_rows.load(std::memory_order_relaxed)) {   // S103 W2B: the body's spring-hair rows after its own, tagged (piece id, node); only the body the slot claims (the first claimant)
      for (const ExtraRow& r : s.extra) {
        t.bone_tags.push_back(rdr_scene::BoneTag{rdr_scene::kBoneTagLayout, uint32_t(t.bones.size()), r.piece_id, r.node, r.piece_nodes});
        t.bones.push_back(r.m);
      }
      o.bones_count += uint32_t(s.extra.size());
      rdr_scene::g_spring_census.rows.fetch_add(s.extra.size(), std::memory_order_relaxed);
    }
    o.light_set = light;
    // mesh mask stays all-ones (every mesh shown): the attire's piece list is unknown, so this is "unknown", not a claim.
    t.objects.push_back(o);
    t.engine_objects.push_back(s.skel);  // the pairing key with DrawOrigin::object (the model object IS the skeleton; map section 1)
    if (s.cc_on && slot_ok) {   // S108 THR2: section 67, keyed by the engine object (the draw side joins it through kScEngineObjects, as the liquid maps)
      rdr_scene::ChangeColourRow row;
      row.object = s.skel;
      row.flags = s.cc_unsettled ? rdr_scene::kCcUnsettled : 0u;
      std::memcpy(row.colours, s.cc_colours, sizeof row.colours);
      t.change_colours.push_back(row);
      change_colour::Bodies().NoteRow();
    }
    ++c.objects;
    c.unknown += (link.known && !link.pac.empty()) ? 1 : 2;   // (asset: a key without a pac name is still unknown,) mesh mask
    if (!s.consumer && !s.link_only) {   // the palette came from the exit of sub_82BD30E8: not where the draw reads it (Brock's nodes 59-108)
      ++c.unknown;
      derived_fallbacks_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!slot_ok && !by_owner && !own_rows && !game_hidden) ++c.unknown;   // (S105 A5a: an own-rows piece is linked to its owner too)   // the skeleton -> wrestler link (S103 NRBF: a piece drawn by its owner is linked to it; S111 SOCKPIECE: a hidden one too)
    if (light == kUnknown) ++c.unknown;  // light set
  }
}

}  // namespace rdr_scene::live
