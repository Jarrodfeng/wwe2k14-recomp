// The crowd contributor (Session 101, lane 27). See crowd_contrib.h. No game or SDK includes.
#include "crowd_contrib.h"
#include "../renderer_scene/rows_crowd.h"   // S106 G4-2
#include "../renderer_scene/rows_pictures.h"
#include "sign_file_keys.h"   // S103 CSF
#include "../renderer_adapter/census_flush.h"  // S102 CEN1
static census_flush::Site g_cf_ids, g_cf_run;  // S102 CEN1

#include "../renderer_adapter/rdr_switches.h"  // S102 T11: CrowdIdsOnce, read every frame (no game or SDK includes)
#include "../renderer_scene/swap_once.h"        // S102 SW1
#include "../renderer_scene/crowd_prop_rows.h"   // S103 D5: AppendProp (header only)
#include "presence_rules.h"                     // S103 S2: the crowd's presence switch (g_crowd)

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace rdr_scene::live::crowd {

void Contributor::BeginDraw(uint32_t read_ring) {
  building_.clear();  // (the draw thread's own vector: no lock)
  building_other_ = 0;
  building_unobserved_ = 0;   // S103 D3b F1
  building_props_.clear();    // S103 D3c
  building_screen_.clear();   // S103 C1 review fix 2
  draw_ring_ = read_ring > 1 ? 0 : read_ring;
}

void Contributor::NoteMember(const MemberObs& m) { building_.push_back(m); }

void Contributor::NoteUnmodelled() { ++building_other_; }

void Contributor::EndDraw() {
  std::lock_guard<std::mutex> lock(mu_);
  published_.swap(building_);
  snap_latch_built_ = false;   // S103 D3b: a walk's snapshot
  published_other_ = building_other_;
  published_unobserved_ = building_unobserved_;   // S103 D3b F1
  published_props_.swap(building_props_);         // S103 D3c
  building_props_.clear();
  published_screen_.swap(building_screen_), building_screen_.clear();   // S103 C1 review fix 2
  building_.clear();
  published_palettes_ = palettes_[draw_ring_];  // the set the draw's read ring names
  ++draw_seq_;
}

// S103 D9: the crowd entry's own read (draw thread, before the game's draw or at the OURS skip). Locks only to hand the snapshot over; the reader runs on the draw thread's own scratch.
void Contributor::EntryRead() {
  if (!latch_reader_ || !SnapActive()) return;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (snap_plant == 11) return;   // planted: no entry read (the latch reads everything, as before)
#endif
  entry_scratch_.Clear();
  entry_scratch_.node = node_.load(std::memory_order_relaxed);
  entry_scratch_.entry_ring = entry_ring_.load(std::memory_order_relaxed);
  entry_scratch_.screen_cam = entry_cam_.load(std::memory_order_relaxed);   // S103 C1: the singles' screen test camera, as the draw thread sees it
  entry_scratch_.served = ours_skip_.load(std::memory_order_relaxed) && ours_served_.load(std::memory_order_relaxed);
  entry_scratch_.hand_cache = &hand_cache_;   // S103 SG review 3
  entry_scratch_.scope = 2;   // the entry half: only what the skin pass never writes (floor, props, holders, pictures)
  entry_scratch_.want_palettes = false;
  const uint32_t why = latch_reader_(entry_scratch_);
  std::lock_guard<std::mutex> lock(mu_);
  ++lstats_.entry_reads;
  if (entry_valid_) ++lstats_.entry_unused;   // a previous entry's snapshot nobody latched
  std::swap(entry_snap_, entry_scratch_);
  entry_why_ = why, entry_valid_ = true;
  entry_pal_.fill({});
  if (!why && entry_snap_.read_ring <= 1) entry_pal_ = palettes_[entry_snap_.read_ring];   // the set the draw of this frame reads, as the hooks stood at the entry (review 3)
}

void Contributor::SetPalette(uint32_t write_ring, uint32_t type, uint32_t lod, const float* matrices, uint32_t nodes) {
  if (write_ring > 1 || type >= kTypes || lod >= kLods || !matrices || nodes == 0 || nodes > kPaletteNodes) return;
  std::lock_guard<std::mutex> lock(mu_);
  palettes_[write_ring][lod * kTypes + type].assign(matrices, matrices + size_t(nodes) * 16);
  pal_seq_[write_ring] = ++pal_clock_;   // S103 D8
}

void Contributor::SetArena(int number) {
  std::lock_guard<std::mutex> lock(mu_);
  if (number != arena_) {  // another arena: the old one's members and poses are not this one's
    published_.clear(), building_.clear();
    published_other_ = 0;
    for (auto& set : palettes_) for (auto& p : set) p.clear();
    for (auto& set : pal_hist_) for (auto& p : set) p.clear();   // S103 D9
    pal_hist_n_ = 0;
    for (auto& p : published_palettes_) p.clear();
    stale_run_ = 0;
  }
  arena_ = number;
}

void Contributor::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  building_.clear(), published_.clear();
  building_other_ = published_other_ = 0;
  for (auto& set : palettes_) for (auto& p : set) p.clear();
  for (auto& p : published_palettes_) p.clear();
  draw_seq_ = emitted_seq_ = 0;
  stale_run_ = 0;
  lstats_ = LatchStats{};   // S103 D3b
  { std::lock_guard<std::mutex> hl(hand_cache_.mu); hand_cache_.valid = false; }   // S103 SG review 3
  for (auto& q : prop_bone_prev_) q = PropBonePrev{};   // S103 SG
  armed_ = false;
  snap_latch_built_ = false;
  latch_ok_ = false;
  building_unobserved_ = published_unobserved_ = 0;
  building_props_.clear(), published_props_.clear(), published_singles_.clear(), published_pictures_.clear();
  walked_props_.clear(), walked_pictures_.clear(), walked_valid_ = false, took_in_place_ = false;   // S103 SG2
  snap_withheld_ = 0;
  entry_cam_.store(kScreenCamUnknown, std::memory_order_relaxed);   // S103 C1
  building_screen_.clear(), published_screen_.clear();
  latch_scratch_.Clear();
  entry_valid_ = false;   // S103 D9
  for (auto& set : pal_hist_) for (auto& p : set) p.clear();
  pal_hist_n_ = 0;
  ours_skip_.store(false);
  snap_stamped_ = snap_judged_ = snap_last_ = 0;   // S103 D3 (test hook: the census restarts)
  for (auto& w : snap_why_) w = 0;
  snap_verdict_ = SnapWhy::kCount;
  draw_ring_ = 0;
  arena_ = -1;
  period_.emplace();   // S111 CUTCROWD2: a new pairing (the first latch after a Reset takes its own read; its census restarts)
}


// ---- S102 U1 (docs\renderer\SWAP_TAIL.md): the snapshot every Emit path takes, now in one place so the latch can take it before the release ----
// The block the three paths had (unchanged): no crowd draw since the last record -> the first two such frames repeat the snapshot (a frame the crowd was
// not drawn in), after that the crowd is not on screen (a menu, another scene) and nothing is emitted; counted once per episode.
void Contributor::SnapLocked(bool& skip, bool& count_stale, std::vector<MemberObs>& members, Palettes& palettes, int& arena, uint32_t& other) {
  skip = count_stale = false;
  const bool fresh = draw_seq_ != emitted_seq_;   // S103 D3: a crowd draw ended since the previous snapshot
  snap_verdict_ = SnapWhy::kCount;                // "not judged" (Mode Off, or no sink)
  if (draw_seq_ == emitted_seq_) {
    ++stale_run_;
    if (stale_run_ == 3) count_stale = true;
    if (stale_run_ >= 3) { skip = true; if (SnapActive()) snap_verdict_ = SnapWhy::Stale; return; }
  } else {
    stale_run_ = 0;
  }
  members = published_;  // (a copy: the hooks go on; into kept storage where the caller keeps it)
  for (size_t i = 0; i < palettes.size(); ++i) palettes[i] = published_palettes_[i];
  arena = arena_;
  other = published_other_;
  if (SnapActive()) snap_verdict_ = JudgeLocked(fresh);   // S103 D3: judged on the copy just taken (the published one), mu_ held
  emitted_seq_ = draw_seq_;
}

// ---- S103 D3 (required input bit 10; docs\renderer\INPUTS_A3.md): the verdict and the stamp ----
bool Contributor::SnapActive() const {
#ifdef RDR_CROWD_SNAP_PLANTS
  if (snap_plant == 4) return sink_.stamp != nullptr;   // planted: Mode Off judged and stamped anyway
#endif
  return sink_.active && sink_.stamp && sink_.active();
}

SnapWhy Contributor::JudgeLocked(bool fresh) const {
  int plant = 0;
#ifdef RDR_CROWD_SNAP_PLANTS
  plant = snap_plant;
#endif
  uint32_t unread = published_other_ + published_unobserved_;   // S103 D3b F1: floor people the observer is off for count like unmodelled ones
#ifdef RDR_CROWD_SNAP_PLANTS
  if (snap_plant == 6) unread = published_other_;   // planted: the unobserved floor people are ignored
#endif
  if (presence::g_crowd.load(std::memory_order_relaxed)) {   // S103 S2: hold by presence (crowd_snapshot_core.h): latch_partial lifted, a shown holder / sign / picture holds this frame only
    static_assert(kSingleHiddenLod == kShLod, "crowd_snapshot_core.h kSingleHiddenLod is crowd_contrib.h kShLod");
    static_assert(kSingleHiddenScreen == kShScreen, "crowd_snapshot_core.h kSingleHiddenScreen is crowd_contrib.h kShScreen");
    const SnapWhy w = EvaluateSnapshot(fresh, arena_, unread, published_, published_palettes_, kTypes, kLods, kUnknown, PresencePolicy(policy_), plant, snap_latch_built_, latch_ok_);
    SnapWhy v = ApplyPresence(w, presence_);
    // S103 C1 (R14-4, census LABEL only): on a walked frame each sign / foam-hand op 21 is a NoteProp row AND a NoteUnmodelled (crowd_hooks.h OnOp21), so a frame that shows a sign was judged `unmodelled`
    // before ApplyPresence could name it `holder_present`. Judged again without those prop counts: when that verdict is holder_present the frame is labelled so. Never Whole: the label changes, a stamp never does.
    bool relabel = v == SnapWhy::Unmodelled && !snap_latch_built_;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (snap_plant == 14) relabel = false;   // planted: the prop frames stay labelled unmodelled
#endif
    if (relabel) {
      const uint32_t prop_other = std::min<uint32_t>(uint32_t(published_props_.size()), published_other_);
      if (prop_other) {
        const SnapWhy w2 = EvaluateSnapshot(fresh, arena_, unread - prop_other, published_, published_palettes_, kTypes, kLods, kUnknown, PresencePolicy(policy_), plant, snap_latch_built_, latch_ok_);
        if (ApplyPresence(w2, presence_) == SnapWhy::HolderPresent) v = SnapWhy::HolderPresent;
      }
    }
    return v;
  }
  return EvaluateSnapshot(fresh, arena_, unread, published_, published_palettes_, kTypes, kLods, kUnknown, policy_, plant, snap_latch_built_, latch_ok_);
}

void Contributor::NoteVerdict(SnapWhy w, uint64_t record_frame, const Presence* shown) {
  if (w == SnapWhy::kCount) return;   // not judged (Mode Off)
  snap_judged_.fetch_add(1, std::memory_order_relaxed);
  snap_why_[uint32_t(w)].fetch_add(1, std::memory_order_relaxed);
  if (w == SnapWhy::HolderPresent) {   // S103 S2: what made the frame not whole
    const Presence& pr = shown ? *shown : presence_;   // S111 CUTCROWD2: the latch of the snapshot being stamped (the latch before this one in steady state)
    pres_frames_.fetch_add(1, std::memory_order_relaxed);
    pres_props_.fetch_add(pr.props, std::memory_order_relaxed), pres_singles_.fetch_add(pr.singles, std::memory_order_relaxed), pres_pictures_.fetch_add(pr.pictures, std::memory_order_relaxed);
  }
  if (w != SnapWhy::Whole) return;
  uint64_t hook = SnapHookFrame(record_frame);
#ifdef RDR_CROWD_SNAP_PLANTS
  if (snap_plant == 3) hook = record_frame;   // planted: the record's frame, not its hook frame
#endif
  const bool by_presence = presence::g_crowd.load(std::memory_order_relaxed);   // S103 S2: a whole frame with nothing unproven shown stamps under the presence switch
  if (!kCrowdOursWhole && !by_presence) {   // S103 D3c: judged whole, Capture withheld (see crowd_snapshot_core.h)
    snap_withheld_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!kCrowdOursWhole) pres_stamped_.fetch_add(1, std::memory_order_relaxed);
  sink_.stamp(hook);
  snap_stamped_.fetch_add(1, std::memory_order_relaxed);
  snap_last_.store(hook, std::memory_order_relaxed);
}

void Contributor::Census(std::string& out) {
  {   // S103 D3b: the walk-free source
    const LatchStats l = LatchCensus();
    uint64_t dec = 0;
    for (uint64_t d : l.declined) dec += d;
    out += " crowd_latch installed " + std::to_string((unsigned long long)l.installed) + " declined " + std::to_string((unsigned long long)dec) + " [no_manager " + std::to_string((unsigned long long)l.declined[1]) +
           " bad_ring " + std::to_string((unsigned long long)l.declined[2]) + " classify_pending " + std::to_string((unsigned long long)l.declined[3]) + " bad_pointer " + std::to_string((unsigned long long)l.declined[4]) +
           " too_many " + std::to_string((unsigned long long)l.declined[5]) + " unreadable " + std::to_string((unsigned long long)l.declined[6]) + " no_palette " + std::to_string((unsigned long long)l.declined[7]) + "] absent " + std::to_string((unsigned long long)l.absent) +
           " absent_unarmed " + std::to_string((unsigned long long)l.absent_unarmed) + " knocked_out " + std::to_string((unsigned long long)l.knocked_out) + " vs walk compared " + std::to_string((unsigned long long)l.compared) +
           " stands eq/near/diff " + std::to_string((unsigned long long)l.stands.equal) + "/" + std::to_string((unsigned long long)l.stands.almost) + "/" + std::to_string((unsigned long long)l.stands.differs) +
           " props(shown) eq/near/diff " + std::to_string((unsigned long long)l.props.equal) + "/" + std::to_string((unsigned long long)l.props.almost) + "/" + std::to_string((unsigned long long)l.props.differs) + " hidden_seen " + std::to_string((unsigned long long)l.prop_extra) +
           " read " + std::to_string((unsigned long long)l.props_installed) + " rows " + std::to_string((unsigned long long)prop_rows_.load(std::memory_order_relaxed)) + " (hidden " + std::to_string((unsigned long long)prop_rows_hidden_.load(std::memory_order_relaxed)) + ", refused " + std::to_string((unsigned long long)prop_rows_refused_.load(std::memory_order_relaxed)) + ")" + " singles " + std::to_string((unsigned long long)l.singles_read) + " (hidden " + std::to_string((unsigned long long)l.singles_hidden) + ", world_unread " + std::to_string((unsigned long long)l.singles_world_unread) + ", rows " + std::to_string((unsigned long long)single_rows_.load(std::memory_order_relaxed)) + ", refused " + std::to_string((unsigned long long)single_rows_refused_.load(std::memory_order_relaxed)) + ")" + " floor(subset) eq/near/diff " + std::to_string((unsigned long long)l.floor.equal) + "/" + std::to_string((unsigned long long)l.floor.almost) + "/" + std::to_string((unsigned long long)l.floor.differs) + " extra " + std::to_string((unsigned long long)l.floor_extra) +
           " unobserved " + std::to_string((unsigned long long)l.floor_unobserved) + " installed " + std::to_string((unsigned long long)l.floor_installed) + " unread_objects " + std::to_string((unsigned long long)l.floor_unread) + " cards eq/near/diff " + std::to_string((unsigned long long)l.cards.equal) + "/" + std::to_string((unsigned long long)l.cards.almost) + "/" + std::to_string((unsigned long long)l.cards.differs) +
           " palettes eq/diff " + std::to_string((unsigned long long)l.pal_equal) + "/" + std::to_string((unsigned long long)l.pal_differs) +
           " hand_map ok/bad " + std::to_string((unsigned long long)l.hand_map_ok) + "/" + std::to_string((unsigned long long)l.hand_map_bad) + " (types " + std::to_string(l.hand_types) + " of game " + std::to_string(l.hand_types_game) + ", key_unread " + std::to_string((unsigned long long)l.hand_key_unread) + ")" +
           " hand_idx " + std::to_string(l.hand_idx[0]) + "," + std::to_string(l.hand_idx[1]) + "," + std::to_string(l.hand_idx[2]) + "," + std::to_string(l.hand_idx[3]) + "," + std::to_string(l.hand_idx[4]) + "," + std::to_string(l.hand_idx[5]) +
           " bone_changed ours " + std::to_string((unsigned long long)l.bone_changed_ours) + "/" + std::to_string((unsigned long long)l.bone_seen_ours) + " walked " + std::to_string((unsigned long long)l.bone_changed_walked) + "/" + std::to_string((unsigned long long)l.bone_seen_walked);   // S103 SG: bone hash changed / bone-held props seen, per frame kind
    {   // S103 D8
      static const char* const kStageNames[8] = {"none", "node", "header", "sin", "lists", "palettes", "tail", "other"};
      static const char* const kPartNames[4] = {"floor", "props", "singles", "pictures"};
      char b[96];
      out += " latch_detail unreadable_at [";
      for (uint32_t i = 1; i < 8; ++i) {
        std::snprintf(b, sizeof b, "%s%s %llu@0x%08X", i > 1 ? " " : "", kStageNames[i], (unsigned long long)l.unreadable_stage[i], l.unreadable_addr[i]);
        out += b;
      }
      out += "] soft_holes [";
      for (uint32_t i = 0; i < 4; ++i) {
        std::snprintf(b, sizeof b, "%s%s %llu@0x%08X", i ? " " : "", kPartNames[i], (unsigned long long)l.soft[i], l.soft_addr[i]);
        out += b;
      }
      out += "] ring_moved " + std::to_string((unsigned long long)l.ring_moved) + " (stands diff then " + std::to_string((unsigned long long)l.ring_moved_stands_diff) + ") palettes other_ring eq/diff " + std::to_string((unsigned long long)l.pal_equal_other) + "/" + std::to_string((unsigned long long)l.pal_differs_other) +
             " newest_hook eq/diff " + std::to_string((unsigned long long)l.pal_equal_newest) + "/" + std::to_string((unsigned long long)l.pal_differs_newest) + " entry_read " + std::to_string((unsigned long long)l.entry_reads) + " used " + std::to_string((unsigned long long)l.entry_used) + " declined " + std::to_string((unsigned long long)l.entry_declined) + " unused " + std::to_string((unsigned long long)l.entry_unused) +
             " palettes_vs_walk eq/diff " + std::to_string((unsigned long long)l.pal_walk_eq) + "/" + std::to_string((unsigned long long)l.pal_walk_diff) + " other_ring eq/diff " + std::to_string((unsigned long long)l.pal_walk_other_eq) + "/" + std::to_string((unsigned long long)l.pal_walk_other_diff) +
             " prop_diff [site0 count/world " + std::to_string((unsigned long long)l.prop_diff_count[0]) + "/" + std::to_string((unsigned long long)l.prop_diff_world[0]) + " site1 " + std::to_string((unsigned long long)l.prop_diff_count[1]) + "/" + std::to_string((unsigned long long)l.prop_diff_world[1]) +
             " site2 " + std::to_string((unsigned long long)l.prop_diff_count[2]) + "/" + std::to_string((unsigned long long)l.prop_diff_world[2]) + " site3 " + std::to_string((unsigned long long)l.prop_diff_count[3]) + "/" + std::to_string((unsigned long long)l.prop_diff_world[3]) + "]" +
             " mirror props_read " + std::to_string((unsigned long long)l.mirror_props_read) + " singles seen " + std::to_string((unsigned long long)l.mirror_singles_seen) + " bound " + std::to_string((unsigned long long)l.mirror_singles_bound) + " hidden " + std::to_string((unsigned long long)l.mirror_singles_hidden) +
             " walked_kept props " + std::to_string((unsigned long long)l.walked_props_kept) + " pictures " + std::to_string((unsigned long long)l.walked_pictures_kept) + " singles_held " + std::to_string((unsigned long long)l.walked_singles_held) + " frames " + std::to_string((unsigned long long)l.in_place_frames);
      {   // S103 FHB: the pictures' fate by reason: reads by Why (ok / no_registry / no_handle / wrapper / not_2d / no_header / format / size / memory), a walked frame's unread, the first bad handle, then what AppendPictureRows dropped
        static const char* const kWhyNames[9] = {"ok", "no_registry", "no_handle", "wrapper", "not_2d", "no_header", "format", "size", "memory"};
        out += " pic_why [";
        for (uint32_t i = 0; i < 9; ++i) out += std::string(i ? " " : "") + kWhyNames[i] + " " + std::to_string((unsigned long long)l.pic_why[i]);
        char fb[192];
        std::snprintf(fb, sizeof fb, "] unread_walked %llu first_bad 0x%08llX rows %llu dropped [", (unsigned long long)l.pic_unread_walked, (unsigned long long)l.pic_first_bad_key, (unsigned long long)picture_rows_.load(std::memory_order_relaxed));
        out += fb;
        for (uint32_t i = 1; i < 9; ++i) out += std::string(i > 1 ? " " : "") + kWhyNames[i] + " " + std::to_string((unsigned long long)pic_drop_why_[i].load(std::memory_order_relaxed));
        out += " empty " + std::to_string((unsigned long long)pic_drop_empty_.load(std::memory_order_relaxed)) + " over_cap " + std::to_string((unsigned long long)pic_drop_size_.load(std::memory_order_relaxed)) + " dims " + std::to_string((unsigned long long)pic_drop_dims_.load(std::memory_order_relaxed)) + "]";
        {   // S103 CSF: which picture is refused `size` (docs\agents\s103\CS.md section 4) and from which site
          const PictureObs& f = l.pic_first_size;
          char sb[400];
          std::snprintf(sb, sizeof sb, " size_by_site sign %llu hand %llu first_size key 0x%08X site %u fetch %08X %08X %08X %08X %08X %08X (w %u h %u fmt %u tiled %u packed %u pitch_blocks %u) file_check verified %llu mismatch %llu",
                        (unsigned long long)l.pic_size_by_site[0], (unsigned long long)l.pic_size_by_site[1], f.key, unsigned(f.site), f.fetch[0], f.fetch[1], f.fetch[2], f.fetch[3], f.fetch[4], f.fetch[5],
                        f.shape_w, f.shape_h, f.shape_fmt, unsigned(f.tiled), unsigned(f.packed), f.pitch_blocks, (unsigned long long)SignFileKeys::Verifications().load(std::memory_order_relaxed), (unsigned long long)SignFileKeys::Mismatches().load(std::memory_order_relaxed));   // S103 CSF review 2: the one-time file-vs-guest checks
          out += sb;
        }
      }
      // S103 C1 (R15): what is left of unread_objects, by reason (crowd_contrib.h UnreadWhy; the reasons sum to unread_objects), and the holders the singles' screen test hid / the LOD 1-2 holders read
      static const char* const kUrNames[kUrCount] = {"hole", "forward", "no_nodes", "world3", "bind", "screen", "floor_lod", "other"};
      out += " unread_by [";
      for (uint32_t i = 0; i < kUrCount; ++i) out += std::string(i ? " " : "") + kUrNames[i] + " " + std::to_string((unsigned long long)l.unread_by[i]);
      out += "] screen_hidden " + std::to_string((unsigned long long)l.screen_hidden) + " lod_holders " + std::to_string((unsigned long long)l.lod_holders);
      if (l.forward_vt) { char fb[32]; std::snprintf(fb, sizeof fb, " forward_vt 0x%08X", l.forward_vt); out += fb; }   // S103 SN: the foreign class a remaining `forward` names
      out += " screen_check eq " + std::to_string((unsigned long long)l.screen_eq) + " screen_diff " + std::to_string((unsigned long long)l.screen_diff) + " reader_only " + std::to_string((unsigned long long)l.screen_reader_only) +
             " game_only " + std::to_string((unsigned long long)l.screen_game_only) + " (trust " + (presence::g_crowd_screen_trust.load(std::memory_order_relaxed) ? "on" : "off") + ")";   // S103 C1 review fix 2
    }
  }
  const SnapStats s = SnapCensus();
  out += " crowd_snapshot withheld " + std::to_string((unsigned long long)s.withheld) + " stamped " + std::to_string((unsigned long long)s.stamped) + "/" + std::to_string((unsigned long long)s.judged) + " [";
  for (uint32_t i = 1; i < uint32_t(SnapWhy::kCount); ++i) out += std::string(i > 1 ? " " : "") + SnapWhyName(SnapWhy(i)) + " " + std::to_string((unsigned long long)s.why[i]);
  out += "]";
  out += std::string(" presence (switch ") + (presence::g_crowd.load(std::memory_order_relaxed) ? "on" : "off") + ") holder_frames " + std::to_string((unsigned long long)pres_frames_.load(std::memory_order_relaxed)) +   // S103 S2
         " (shown props " + std::to_string((unsigned long long)pres_props_.load(std::memory_order_relaxed)) + ", holders " + std::to_string((unsigned long long)pres_singles_.load(std::memory_order_relaxed)) +
         ", pictures " + std::to_string((unsigned long long)pres_pictures_.load(std::memory_order_relaxed)) + ") stamped_by_presence " + std::to_string((unsigned long long)pres_stamped_.load(std::memory_order_relaxed));
  out += " " + period_->CensusLine("crowd_period");   // S111 CUTCROWD2: records that took the snapshot of the latch before theirs (previous) and the boundary (own_first / own_gap; latch_period.h)
}

// S103 D3b. Called by Latch with mu_ held, game threads parked (the same precondition as every latch read).
void Contributor::LatchFeedLocked() {
  const bool skipped = ours_skip_.exchange(false, std::memory_order_relaxed);
  latch_ok_ = false;
  walked_valid_ = false;   // S103 SG2: filled again only by a walked frame whose read was used (the walked branch below)
  presence_ = Presence{};   // S103 S2: nothing shown until this latch's read says so (a frame the entry did not reach has no crowd, so none)
  const bool entry_ok = entry_valid_ && entry_why_ == 0;   // S103 D9 review 2: read AND cleared before any return (a snapshot is for ONE latch; mode Off / not-reached frames drop it)
  if (entry_valid_ && entry_why_ != 0) ++lstats_.entry_declined;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (snap_plant != 13)   // planted: the entry's snapshot survives a latch that returns early
#endif
    entry_valid_ = false;
  if (!latch_reader_ || !SnapActive()) {
    armed_ = false;
    return;
  }
  const bool was_armed = armed_;
  armed_ = true;
  const bool knocked_out = sink_.blocked && sink_.blocked();   // review F5: under ObserverKnockOut the hooks' observers are off, the walk's snapshot is not a witness; nothing is proved
  if (knocked_out) ++lstats_.knocked_out;
  const bool fresh_walk = draw_seq_ != emitted_seq_;
  if (!fresh_walk && !skipped) {   // the crowd entry was not reached this frame (a menu, a scene without a crowd): the game draws no crowd, so the frame's crowd is EMPTY and whole (review F5)
    published_.clear();
    published_props_.clear();   // S103 D5 review 1: no crowd, no signs either
    published_singles_.clear(), published_pictures_.clear();
    published_other_ = published_unobserved_ = 0;
    snap_latch_built_ = false;
    ++draw_seq_;
    if (was_armed && !knocked_out) ++lstats_.absent, latch_ok_ = true;   // "not reached" needs a previous active latch: the first frame after the mode came on proves nothing
    else if (!knocked_out) ++lstats_.absent_unarmed;   // (review item 8: a knocked-out frame is counted `knocked_out` above, not as a missing history)
    return;
  }
  latch_scratch_.Clear();
  latch_scratch_.node = node_.load(std::memory_order_relaxed);
  latch_scratch_.entry_ring = entry_ring_.load(std::memory_order_relaxed);   // S103 D8
  latch_scratch_.screen_cam = entry_cam_.load(std::memory_order_relaxed);   // S103 C1: the camera object the entry noted (its matrices are read at the latch: [L] unchanged since the entry)
  latch_scratch_.served = !fresh_walk && ours_served_.load(std::memory_order_relaxed);
  latch_scratch_.want_palettes = fresh_walk;   // mirror frames compare the +608 blocks; an OURS install uses the hook rings (items 4, 5)
  uint32_t why;
  // S103 D9: SPLIT BY WRITER. The crowd entry runs on the update thread concurrently with the skin pass (sub_8250BAF0, which writes the +608 palettes, the member classification flags / groups and the vertex rings
  // under the manager lock at +572), so only data the skin pass never writes is read at the entry (scope 2: floor seats, props, holders, pictures); the stands, cards and palettes are read at the latch (scope 1).
  latch_scratch_.scope = entry_ok ? 1 : 0;
  latch_scratch_.hand_cache = &hand_cache_;   // S103 SG review 3
  why = latch_reader_(latch_scratch_);
  if (entry_ok && !why) {
    ++lstats_.entry_used;
    for (const MemberObs& m : entry_snap_.members) latch_scratch_.members.push_back(m);   // the floor seats
    for (auto& q : entry_snap_.props) latch_scratch_.props.push_back(std::move(q));
    for (auto& q : entry_snap_.singles) latch_scratch_.singles.push_back(std::move(q));
    for (auto& q : entry_snap_.pictures) latch_scratch_.pictures.push_back(std::move(q));
    latch_scratch_.unread += entry_snap_.unread;
    for (uint32_t i = 0; i < kUrCount; ++i) latch_scratch_.unread_by[i] += entry_snap_.unread_by[i];   // S103 C1
    latch_scratch_.screen_hidden += entry_snap_.screen_hidden, latch_scratch_.lod_holders += entry_snap_.lod_holders;
    if (!latch_scratch_.forward_vt) latch_scratch_.forward_vt = entry_snap_.forward_vt;   // S103 SN
    for (uint32_t i = 0; i < 4; ++i) {
      latch_scratch_.soft[i] += entry_snap_.soft[i];
      if (!latch_scratch_.soft_addr[i]) latch_scratch_.soft_addr[i] = entry_snap_.soft_addr[i];
    }
    latch_scratch_.read_ring = entry_snap_.read_ring;   // the ring the game's draw read at the entry
    // S103 SG review 1: the foam-hand map is read by the entry half (ReadProps); the latch half (scope 1) leaves it default, so it comes over whole. (Every field a scope-2 part sets is merged here: lint s103_SG.snapshot_fields.)
    latch_scratch_.hand_map_ok = entry_snap_.hand_map_ok, latch_scratch_.hand_types = entry_snap_.hand_types, latch_scratch_.hand_types_game = entry_snap_.hand_types_game;
    for (uint32_t i = 0; i < 6; ++i) latch_scratch_.hand_h[i] = entry_snap_.hand_h[i], latch_scratch_.hand_idx[i] = entry_snap_.hand_idx[i];
  }
  if (why) {
    ++lstats_.declined[why < 8 ? why : 7];
    if (why == 6) {   // S103 D8: Unreadable: which hard stage and the first refused address
      const uint32_t st = latch_scratch_.fail_stage < 8 ? latch_scratch_.fail_stage : 7;
      if (lstats_.unreadable_stage[st]++ == 0) lstats_.unreadable_addr[st] = latch_scratch_.fail_addr;
    }
    if (!fresh_walk) {   // an OURS frame the reader could not read: draw NO crowd rather than repeat the previous frame's (review F6)
      published_.clear();
      published_props_.clear();   // S103 D5 review 1: a declined OURS frame draws no signs / hands either
      published_singles_.clear(), published_pictures_.clear();
      published_other_ = published_unobserved_ = 0;
      snap_latch_built_ = true;
      ++draw_seq_;
    }
    return;   // a fresh walk's snapshot goes ahead, unproved (no stamp)
  }
  latch_ok_ = !knocked_out;
  for (const PictureObs& q : latch_scratch_.pictures) {   // S103 FHB: every picture read by its Why, walked frames too (the walked path never counted one)
    ++lstats_.pic_why[q.why < 9 ? q.why : 8];
    if (q.why != 0 && lstats_.pic_first_bad_key == 0) lstats_.pic_first_bad_key = q.key;
    if (q.why == 7) {   // S103 CSF: size refusals split by site, and the first one's shape
      ++lstats_.pic_size_by_site[q.site <= 1 ? 0 : 1];
      if (lstats_.pic_first_size_key == 0) lstats_.pic_first_size_key = q.key, lstats_.pic_first_size = q;
    }
    if (q.why != 0 && fresh_walk) ++lstats_.pic_unread_walked;
  }
  presence_ = CountPresent(latch_scratch_.props, latch_scratch_.singles, latch_scratch_.pictures, presence::g_crowd_screen_trust.load(std::memory_order_relaxed));   // (S103 C1 review fix 2: a screen-hidden holder blocks until trusted)   // S103 S2: what the reader saw shown this frame (OURS and walked frames alike)
  if (fresh_walk) {   // a walked frame: the walk's own drawn props (NoteProp: hidden 0 only) count too
    uint32_t drawn = 0;
    for (const PropObs& p : published_props_) drawn += p.hidden == 0;
    presence_.props = std::max(presence_.props, drawn);
  }
  lstats_.ring_moved += latch_scratch_.ring_moved;   // S103 D8
  for (uint32_t i = 0; i < 4; ++i)   // S103 D8: soft holes (optional parts whose read was refused)
    if (latch_scratch_.soft[i]) {
      if (lstats_.soft[i] == 0) lstats_.soft_addr[i] = latch_scratch_.soft_addr[i];
      lstats_.soft[i] += latch_scratch_.soft[i];
    }
  NotePropCensusLocked(!fresh_walk);   // S103 SG: the hand map and the frozen-bone count, every read frame (before the props are swapped away)
  if (fresh_walk) {   // a mirror frame: the walk's own snapshot stands, the reader's is compared with it
    ++lstats_.compared;
    const uint64_t stands_diff_before = lstats_.stands.differs;
    CompareMembers(published_, latch_scratch_.members, false, lstats_.stands);
    if (latch_scratch_.ring_moved && lstats_.stands.differs != stands_diff_before) ++lstats_.ring_moved_stands_diff;   // S103 D8
    CompareMembers(published_, latch_scratch_.members, true, lstats_.cards);
    if (published_unobserved_ == 0) CompareFloor(published_, latch_scratch_.members, lstats_.floor, lstats_.floor_extra);   // review item 3: with the floor observer off the walk holds NO seats; nothing to compare
    else ++lstats_.floor_unobserved;
    const uint64_t walked_differs_before = lstats_.props.differs;   // SGW review S1: keep the walked set only when THIS frame's reader set equals the walk's
    {
      PropDiffWhy pw;
      CompareProps(published_props_, latch_scratch_.props, lstats_.props, lstats_.prop_extra, &pw);   // S103 D3c (S103 D9: and why a frame differs, per site)
      for (int i = 0; i < 4; ++i) lstats_.prop_diff_count[i] += pw.count[i], lstats_.prop_diff_world[i] += pw.world[i];
    }
    lstats_.mirror_props_read += latch_scratch_.props.size();   // S103 D8: a mirror frame installs nothing, so `read` / `singles` stay 0 there; these say what the reader saw
    for (const SingleObs& s : latch_scratch_.singles) {
      ++lstats_.mirror_singles_seen;
      lstats_.mirror_singles_hidden += s.hidden != 0 && s.hidden != kShForward;   // (S103 C1: kShForward is unread, not hidden)
      lstats_.mirror_singles_bound += (s.hidden == 0 && s.bind_ok);
    }
    lstats_.floor_unread += latch_scratch_.unread;
    NoteUnreadWhyLocked();   // S103 C1
    CompareScreenLocked();   // S103 C1 review fix 2: the reader's singles screen verdicts vs the game's, this walked frame
    ComparePalettes(palettes_[latch_scratch_.write_ring], latch_scratch_.palettes, lstats_.pal_equal, lstats_.pal_differs);
    {   // S103 D8: the same read against the OTHER ring and against the ring whose hook copy is the newest (run 27: 81 equal / 143859 differ against the write ring's: which set is the memory's?)
      ComparePalettes(palettes_[latch_scratch_.write_ring ^ 1u], latch_scratch_.palettes, lstats_.pal_equal_other, lstats_.pal_differs_other);
      ComparePalettes(palettes_[pal_seq_[1] > pal_seq_[0] ? 1 : 0], latch_scratch_.palettes, lstats_.pal_equal_newest, lstats_.pal_differs_newest);
    }   // the +608 read against the newest hook copy (the write ring's); review F2
    if (latch_scratch_.write_ring <= 1 && latch_scratch_.read_ring <= 1) {   // S103 D9 (run 28: 21 equal / 635619 differ vs the write ring): the +608 read is the NEWEST update's set, the walk publishes the one its draw ring names
      // Filed under the ring the hook actually wrote last (the SetPalette index; pal_seq_), then the history's set at the walk's read ring is compared with what the walk published. Census only: needs the entry's read
      // ring (the entry read used) and both rings filled once (the first frame after Reset / SetArena proves nothing).
      const uint32_t newest = pal_seq_[1] > pal_seq_[0] ? 1u : 0u;
#ifdef RDR_CROWD_SNAP_PLANTS
      if (snap_plant == 12) pal_hist_[newest ^ 1u] = latch_scratch_.palettes; else   // planted: filed under the wrong ring
#endif
      pal_hist_[newest] = latch_scratch_.palettes;
      ++pal_hist_n_;
      if (entry_ok && pal_hist_n_ >= 2) {
        ComparePalettes(published_palettes_, pal_hist_[latch_scratch_.read_ring], lstats_.pal_walk_eq, lstats_.pal_walk_diff);
        ComparePalettes(published_palettes_, pal_hist_[latch_scratch_.read_ring ^ 1u], lstats_.pal_walk_other_eq, lstats_.pal_walk_other_diff);
      }
    }
    // S103 SG2: the record must carry this frame's signs. The reader's set equals the walk's (run 37: props(shown) eq/near/diff 4643/0/0) and is the only one with picture / model_key / variant; on an OURS install it is the record's,
    // here (a walked frame) it was dropped, so no frame the mirror draws could ever show a sign. Keep it, flagged in place (the stream draws it); the holders are held back, counted, until the walk's members are known to carry them.
    walked_props_ = std::move(latch_scratch_.props);
    walked_pictures_ = std::move(latch_scratch_.pictures);
    // SGW review S1: a frame whose reader props differ from the walk (a latch read one update newer, or an empty walk from the entry's early-out) keeps nothing: signs off the hands would be drawn otherwise.
    walked_valid_ = lstats_.props.differs == walked_differs_before;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (snap_plant == 15) walked_props_.clear(), walked_pictures_.clear();   // planted (walked_props_dropped): the current rule, a walked frame's props are dropped: the record carries no sign
#endif
    lstats_.walked_props_kept += walked_props_.size(), lstats_.walked_pictures_kept += walked_pictures_.size(), ++lstats_.in_place_frames;
    for (const SingleObs& s : latch_scratch_.singles) lstats_.walked_singles_held += (s.hidden == 0 && s.bind_ok && s.world_kind != 3 && !s.screen_unknown);
    return;
  }
  published_.swap(latch_scratch_.members);   // an OURS frame: the reader's snapshot is the frame's, as a walk's EndDraw would publish it
  published_palettes_ = entry_ok ? entry_pal_ : palettes_[latch_scratch_.read_ring];   // (S103 D9: the entry's copy when the entry read was used)   // the set the walk would publish (review F2): the hook on sub_82509CB8 keeps filling the rings on OURS frames (update side)
  published_props_.swap(latch_scratch_.props);   // S103 D3c: read, not yet drawn (the record rows and the applier are another lane's)
  lstats_.props_installed += published_props_.size();
  published_singles_.clear(), published_pictures_.clear();   // S103 D6: the holders that are drawn (a row each); the rest were accounted by the reader (hidden / resolved to nothing) or stay unread
  for (const SingleObs& s : latch_scratch_.singles) {
    if (s.hidden == 0 && s.bind_ok && s.world_kind != 3 && !s.screen_unknown) published_singles_.push_back(s);   // (S103 C1: and the screen test ran)
    ++lstats_.singles_read;
    lstats_.singles_hidden += s.hidden != 0 && s.hidden != kShLod && s.hidden != kShForward;   // (kShLod / kShForward (S103 C1) = unread, not hidden)
    lstats_.singles_world_unread += (s.hidden == 0 && s.world_kind == 3);
  }
  published_pictures_ = latch_scratch_.pictures;   // S103 D6
  published_other_ = latch_scratch_.unread;   // objects of the floor class the reader could not model: unmodelled people (blocks the stamp)
  for (const PictureObs& q : published_pictures_)   // S103 D6: a shown prop whose picture cannot be read is a hole (never a grey guess)
    if (q.why != 0) ++published_other_, ++lstats_.pictures_unread;
  for (const PropObs& p : published_props_) {   // S103 D5: a SHOWN prop whose world the writer would refuse is unread (a hole), not a guess
    if (!p.hidden && !rdr_scene::crowd_rows::WorldUsable(p.c)) ++published_other_;
    else if (!p.hidden && p.site >= 2 && p.model_key == 0) ++published_other_, ++lstats_.hand_key_unread;   // S103 SG: a shown foam hand whose mesh (40999 + h) is not known is a hole, never 41016
  }
  published_unobserved_ = 0;
  snap_latch_built_ = true;
  ++draw_seq_;
  ++lstats_.installed;
  for (const MemberObs& m : published_) lstats_.floor_installed += m.floor ? 1 : 0;
  lstats_.floor_unread += latch_scratch_.unread;
  NoteUnreadWhyLocked();   // S103 C1
}

// S103 C1 review fix 2: per node-list object, the reader's SingleScreen verdict (taken at the entry, before the draw) against the game's sub_8276E2C0 return in the same draw (crowd_hooks.h).
// Until a run shows screen_diff 0, a holder the reader hides still counts as present (crowd_snapshot_core.h CountPresent, g_crowd_screen_trust off).
void Contributor::CompareScreenLocked() {
  std::vector<uint8_t> used(published_screen_.size(), 0);
  for (const SingleObs& s : latch_scratch_.singles) {
    if (!s.screen_verdict) continue;
    bool found = false;
    for (size_t i = 0; i < published_screen_.size() && !found; ++i)
      if (!used[i] && published_screen_[i].object == s.object) {
        used[i] = 1, found = true;
        (published_screen_[i].verdict == s.screen_verdict ? lstats_.screen_eq : lstats_.screen_diff) += 1;
      }
    if (!found) ++lstats_.screen_reader_only;
  }
  for (uint8_t u : used) lstats_.screen_game_only += u ? 0 : 1;
}

// S103 C1: the reader's unread objects by reason, holders the screen test hid and holders above LOD 0 read (mirror and OURS frames alike; the census line's `unread_by`)
void Contributor::NoteUnreadWhyLocked() {
  for (uint32_t i = 0; i < kUrCount; ++i) lstats_.unread_by[i] += latch_scratch_.unread_by[i];
  lstats_.screen_hidden += latch_scratch_.screen_hidden;
  lstats_.lod_holders += latch_scratch_.lod_holders;
  if (!lstats_.forward_vt) lstats_.forward_vt = latch_scratch_.forward_vt;   // S103 SN
}

// S103 SG: census only. (1) the hand-type map against the loader's own count [M + 0x198D0] (hand_map ok / bad; hand_types = the last read count). (2) The RISK of CROWD_PROPS_GATES.md 11: the props read the
// bone worlds [[E+48]+192] before the node loop; if the walk an OURS frame skips makes them, the bone stays put while the holder moves. Per bone-held prop (site 0 / 2, object j), against the previous read frame:
// bone_seen / bone_changed = bone-held props read with a previous read, and those whose bone hash (bone_sig) differs from it; split by the frame kind (ours / walked), so a run shows whether OURS frames freeze it and walked ones do not.
void Contributor::NotePropCensusLocked(bool ours) {
  if (latch_scratch_.hand_map_ok) ++lstats_.hand_map_ok; else ++lstats_.hand_map_bad;
  lstats_.hand_types = latch_scratch_.hand_types, lstats_.hand_types_game = latch_scratch_.hand_types_game;
  for (uint32_t i = 0; i < 6; ++i) lstats_.hand_idx[i] = latch_scratch_.hand_idx[i];
  for (const PropObs& p : latch_scratch_.props) {
    if (p.site != 0 && p.site != 2) continue;
    const uint32_t k = (p.site == 2 ? 32u : 0u) + (p.index & 31u);
    PropBonePrev& q = prop_bone_prev_[k];
    if (q.valid) {
      ++(ours ? lstats_.bone_seen_ours : lstats_.bone_seen_walked);
      if (p.bone_sig != q.sig) ++(ours ? lstats_.bone_changed_ours : lstats_.bone_changed_walked);
    }
    q.valid = true, q.sig = p.bone_sig;
  }
}

bool Contributor::Latch(const FrameInfo& f) {
  std::lock_guard<std::mutex> lock(mu_);
  LatchFeedLocked();   // S103 D3b
  // S111 CUTCROWD2 (crowd_contrib.h slots_): THIS latch's snapshot (period f.guest_frame: what walk f drew, or the reader's lists at the skipped entry f) goes into the slot the previous record took;
  // slots_[held_] still holds latch f - 1's, the one record f carries.
  const uint8_t w = uint8_t(held_ ^ 1u);
  InputLatch& b = slots_[w];
  SnapLocked(b.skip, b.count_stale, b.members, b.palettes, b.arena, b.other);
  b.props.clear();   // S103 D5: rows only for a snapshot BUILT at the latch (an OURS frame); a walked frame's props are the stream's
  b.in_place = false;
  if (!b.skip && snap_latch_built_) b.props = published_props_;
  else if (!b.skip && latch_ok_ && walked_valid_) b.props = walked_props_, b.in_place = true;   // S103 SG2: a walked frame's reader props
  b.singles.clear(), b.pictures.clear();   // S103 D6
  if (!b.skip && snap_latch_built_) b.singles = published_singles_, b.pictures = published_pictures_;
  else if (!b.skip && latch_ok_ && walked_valid_) b.pictures = walked_pictures_;   // S103 SG2 (the holders stay held back)
  b.judged = snap_verdict_, b.presence = presence_;   // judged on THIS snapshot (SnapLocked, mu_ held); stamped for the record that carries it
  // The pairing (latch_period.h Pick, the scene camera's rule), fed under the camera's own condition: the camera is picked only on a latch with the mode on (inputs_contrib.h Latch runs Take only then), and
  // SnapActive() is that test here (the sink's active() is CurrentMode() != Off, crowd_hooks.h RegisterCrowd). Record f carries latch f - 1's snapshot; at the first latch or after a gap (a mode-Off latch is one:
  // it does not feed the pairing), its own, copied into the other slot so slots_[w] stays whole for record f + 1. A mode-Off latch: its own, not copied (the next active latch sees a gap; Off judges nothing).
  uint8_t take = w;
  bool paired = SnapActive();
#ifdef RDR_CROWD_SNAP_PLANTS
  if (snap_plant == 17) paired = true;   // planted (crowd_pairs_off): a mode-Off latch feeds the pairing too, where the camera's is not fed
#endif
  if (paired) {
    take = period_->Pick(f.guest_frame, w);
#ifdef RDR_CROWD_SNAP_PLANTS
    if (snap_plant == 16) take = w;   // planted (crowd_leads): the record takes its own latch's snapshot, one period ahead of its camera
#endif
    if (take == w) slots_[w ^ 1u] = b, take = uint8_t(w ^ 1u);
  }
  held_ = w, hand_ = take;
  InputLatch& l = slots_[hand_];
  NoteVerdict(l.judged, f.guest_frame, &l.presence);   // S103 D3 F2: the stamp is made HERE, at the latch (the game threads parked), not at a deferred Emit (S111 CUTCROWD2: the carried snapshot's verdict, this record's hook frame)
  l.verdict = SnapWhy::kCount;           // TakeInputs must not count / stamp it again
  l.mark.Set(f.guest_frame);
  return true;
}

bool Contributor::TakeInputs(const FrameInfo& f, Counters& c, std::vector<MemberObs>& members, Palettes& palettes, int& arena, uint32_t& other) {
  bool skip = false, count_stale = false;
  SnapWhy verdict = SnapWhy::kCount;   // S103 D3
  InputLatch& l = slots_[hand_];   // S111 CUTCROWD2: the snapshot this record carries (Latch chose it)
  if (l.mark.Use(f.guest_frame, Name())) {
    skip = l.skip, count_stale = l.count_stale;
    verdict = l.verdict;
    took_props_.clear(), took_singles_.clear(), took_pictures_.clear();
    took_in_place_ = l.in_place;   // S103 SG2
    if (!skip) {
      took_props_.swap(l.props);   // S103 D5
      took_singles_.swap(l.singles);   // S103 D6
      took_pictures_.swap(l.pictures);
      members.swap(l.members);   // (capacity cycles between the latch and the caller's kept storage)
      for (size_t i = 0; i < palettes.size(); ++i) palettes[i].swap(l.palettes[i]);
      arena = l.arena;
      other = l.other;
    }
  } else {
    std::lock_guard<std::mutex> lock(mu_);
    latch_ok_ = false;   // S103 D3b: an Emit with no latch cannot read the guest (a deferred Emit must not): nothing proved, no stamp
    SnapLocked(skip, count_stale, members, palettes, arena, other);
    took_props_.clear(), took_singles_.clear(), took_pictures_.clear();   // S103 D5
    took_in_place_ = false;   // S103 SG2: no latch = nothing proved (latch_ok_ is false), so a walked frame's set is not used on this path
    if (!skip && snap_latch_built_) took_props_ = published_props_, took_singles_ = published_singles_, took_pictures_ = published_pictures_;
    verdict = snap_verdict_;
  }
  NoteVerdict(verdict, f.guest_frame);   // S103 D3: counted always, stamped only when whole (a skipped, stale snapshot is judged Stale)
  if (count_stale) ++c.stale;
  return !skip;
}

// S103 D5: the sign / foam-hand rows of a latch-built frame, through lane D4's writer API (the same code its unit proves). A prop the game's own gates would not draw carries kCpfHidden (never dropped, never
// drawn). A row the writer refuses (a world that is not finite / has a zero scale: a wrong offset) is a counted hole: `unknown` (and the verdict already counted a SHOWN one as unread, LatchFeedLocked).
void Contributor::AppendPropRows(SceneTables& t, Counters& c) {
  (void)c;   // kept for the signature (E5 gate builds with -Werror)
  if (took_props_.empty()) return;
  rdr_scene::crowd_rows::Counts counts;
  uint64_t hidden = 0;
  t.crowd_props.reserve(t.crowd_props.size() + took_props_.size());
  for (const PropObs& p : took_props_) {
    if (p.hidden && !rdr_scene::crowd_rows::WorldUsable(p.c)) continue;   // S103 D5 review 4: a gated-off prop with a bad world is not a hole (never drawn): no row, no count
    if (!p.hidden && p.site >= 2 && p.model_key == 0) continue;   // S103 SG review 4: a SHOWN foam hand with no known mesh is a hole (counted at LatchFeedLocked): no row, never the kind's default 41016
    rdr_scene::crowd_rows::PropIn in;
#ifdef RDR_CROWD_SNAP_PLANTS
    const bool kind_swapped = snap_plant == 8;   // planted: the two loops' kinds swapped
#else
    constexpr bool kind_swapped = false;
#endif
    in.kind = (p.site < 2) != kind_swapped ? kCpSign : kCpHand;   // sites 0 / 1 = sub_82518950 (signs, 0x82518D60), 2 / 3 = sub_82518EB8 (foam hands, 0x825192D0); the technique is the phase word's, not the kind's
    in.model_key = p.model_key;   // S103 SG: a hand's mesh 40999 + h (reader HandTypeMap); signs 0 = 40000
    in.variant = p.variant;
    in.picture = p.picture;
    in.file_key = p.site < 2 ? p.file_key : 0u;   // S103 CSF: a sign's file key (the applier serves the picture from pac\audience.pac)
    in.flags = ((p.site & 1u) ? kCpfFree : kCpfBone) | (p.hidden ? kCpfHidden : 0u) | (took_in_place_ ? kCpfInPlace : 0u);   // S103 SG2
#ifdef RDR_CROWD_SNAP_PLANTS
    if (snap_plant == 7) in.flags &= ~uint32_t(kCpfHidden);   // planted: a gated-off prop is drawn
#endif
    std::memcpy(in.c, p.c, sizeof in.c);
    if (rdr_scene::crowd_rows::AppendProp(t, in, &counts) == rdr_scene::crowd_rows::Result::Ok && p.hidden) ++hidden;
  }
  prop_rows_.fetch_add(counts.props_ok, std::memory_order_relaxed);
  prop_rows_hidden_.fetch_add(hidden, std::memory_order_relaxed);
  prop_rows_refused_.fetch_add(counts.props_refused, std::memory_order_relaxed);
  // (a SHOWN refused row is counted once, as `other` in LatchFeedLocked: c.unknown += other at Emit; review 3)
}

// S103 D6: the holders (sign / foam-hand people, sub_8276E7B0) of a latch-built frame as CrowdSingleRows. Body = BodyAsset(arena, type), type = the entity index [obj+28] (< 20 = LOD 0) [L]; palette = the skin
// matrices the reader made (root out, the row's world is it); c = the world in the members' constants form. A row the writer refuses, or a frame with no arena to name the body in, is a counted hole (c.unknown).
void Contributor::AppendPictureRows(SceneTables& t, Counters& c) {
  (void)c;
  t.crowd_pictures.clear();
  for (const PictureObs& q : took_pictures_) {
    if (q.why != 0) { pic_drop_why_[q.why < 9 ? q.why : 8].fetch_add(1, std::memory_order_relaxed); continue; }   // (an unreadable key was counted as other at the install: the frame is not whole) S103 FHB: counted by reason
    if (q.bytes.empty()) { pic_drop_empty_.fetch_add(1, std::memory_order_relaxed); continue; }
    if (q.bytes.size() > kCrowdPictureBytes) { pic_drop_size_.fetch_add(1, std::memory_order_relaxed); continue; }
    if (q.width > 0xFFFF || q.height > 0xFFFF) { pic_drop_dims_.fetch_add(1, std::memory_order_relaxed); continue; }
    CrowdPictureRow r;
    r.key = q.key, r.width = uint16_t(q.width), r.height = uint16_t(q.height), r.format = q.format, r.nbytes = uint32_t(q.bytes.size());
    std::memcpy(r.bytes, q.bytes.data(), q.bytes.size());
    t.crowd_pictures.push_back(r);
  }
  picture_rows_.fetch_add(t.crowd_pictures.size(), std::memory_order_relaxed);
}
void Contributor::AppendSingleRows(SceneTables& t, Counters& c, int arena) {
  AppendPictureRows(t, c);   // S103 D6 (the same three Emit paths)
  if (took_singles_.empty()) return;
  if (arena < 0) { c.unknown += uint32_t(took_singles_.size()); return; }
  rdr_scene::crowd_rows::Counts counts;
  std::vector<Mat4> pal;
  for (const SingleObs& s : took_singles_) {
    pal.resize(s.nodes);
    for (uint32_t i = 0; i < s.nodes; ++i) std::memcpy(pal[i].m, s.skin.data() + size_t(i) * 16, sizeof pal[i].m);
    rdr_scene::crowd_rows::SingleIn in;
    in.asset = rdr_scene::crowd_rows::BodyAsset(t, arena, int(s.type), int(s.lod));   // S103 C1: a holder above LOD 0 (entity index 20..59) is that LOD's body
    in.type = uint8_t(s.type);
    in.flags = 0;
    std::memcpy(in.c, s.c, sizeof in.c);
    in.palette = pal.data(), in.nodes = s.nodes;
    if (rdr_scene::crowd_rows::AppendSingle(t, in, &counts) != rdr_scene::crowd_rows::Result::Ok) ++c.unknown;
  }
  single_rows_.fetch_add(counts.singles_ok, std::memory_order_relaxed);
  single_rows_refused_.fetch_add(counts.singles_refused, std::memory_order_relaxed);
}

// S102 T11 F7: Emit (this classic body) and EmitCached below make the same record. A change to what one writes (an object field, a string, the order of the
// AddString calls) goes into BOTH; tools\crowd\crowd_ids_check.py (the byte-equality arms) proves they still agree.
void Contributor::Emit(const FrameInfo& finfo, SceneTables& t, Counters& c) {
  // S102 T19: the marker path (a record with CrowdRun markers, not the same bytes: the expanded member list is the same); read every frame at the point of use.
  if (rdr_sw::BoolSeen(rdr_sw::kCrowdRunMarkers, g_crowd_run_markers.load(std::memory_order_relaxed), finfo.guest_frame)) {
    EmitRuns(finfo, t, c);
    return;
  }
  // S102 T11: the cached path (same record bytes); read every frame at the point of use.
  if (rdr_sw::BoolSeen(rdr_sw::kCrowdIdsOnce, g_crowd_ids_once.load(std::memory_order_relaxed), finfo.guest_frame)) {
    EmitCached(finfo, t, c);
    return;
  }
  std::vector<MemberObs> members;
  std::array<std::vector<float>, kTypes * kLods> palettes;
  int arena = -1;
  uint32_t other = 0;
  if (!TakeInputs(finfo, c, members, palettes, arena, other)) return;   // S102 U1: the block moved to SnapLocked (unchanged), the latch's copy or one now
  AppendPropRows(t, c);   // S103 D5
  AppendSingleRows(t, c, arena);   // S103 D6
  c.unknown += other;  // members of the populations not modelled (signs / foam hands, the other op-21 sites): seen, not recorded
  uint32_t pac_field = 0;
  if (arena >= 0) {
    char b[64];
    std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
    pac_field = t.AddString(b);
  }
  // Assets: one per (card?, type, lod), made on first use.
  std::map<uint32_t, uint32_t> asset_of;  // key = (card << 16) | (lod << 8) | type
  auto asset = [&](bool card, uint32_t type, uint32_t lod) {
    const uint32_t key = (card ? 1u << 16 : 0u) | (lod << 8) | type;
    if (auto it = asset_of.find(key); it != asset_of.end()) return it->second;
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_field;
    char e[32];
    if (card) std::snprintf(e, sizeof e, "card:%02u", type);
    else std::snprintf(e, sizeof e, "%u", 20000u + 40u * lod + type);
    a.entry = t.AddString(e);
    a.variant = 0;  // the texture variant (the game's table index, aud_NNa): unknown, 'a' assumed [U]
    if (!pac_field) ++c.unknown;
    ++c.unknown;  // (the variant)
    t.assets.push_back(a);
    return asset_of[key] = uint32_t(t.assets.size() - 1);
  };
  // Palettes: one range of kScBones per (type, lod) actually used.
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> range_of;
  auto palette = [&](uint32_t type, uint32_t lod, uint32_t& first, uint32_t& count) {
    const uint32_t slot = lod * kTypes + type;
    if (auto it = range_of.find(slot); it != range_of.end()) {
      first = it->second.first, count = it->second.second;
      return count != 0;
    }
    const std::vector<float>& p = palettes[slot];
    first = uint32_t(t.bones.size()), count = uint32_t(p.size() / 16);
    for (uint32_t i = 0; i < count; ++i) {
      Mat4 m;
      std::memcpy(m.m, p.data() + size_t(i) * 16, sizeof m.m);
      t.bones.push_back(m);
    }
    range_of[slot] = {first, count};
    return count != 0;
  };
  uint32_t unknown_ordinal = 0;  // members with no readable index: unique within this record (not stable across frames)
  for (const MemberObs& m : members) {
    if (m.block >= kTypes) {
      ++c.unknown;  // a member whose block was not read: nothing to name it by
      continue;
    }
    const bool card = m.card;
    const bool floor = m.floor && !card;  // S101 lane 82: a floor person is always the type's LOD 0 body (crowd_floor.h refuses any other group)
    const uint32_t lod = card ? kCardLayer : (m.lod < kLods ? m.lod : 0);   // S103 C1: a floor person is its group's LOD body (g = 20 lod + type, 0x8276EAC4; the walk's decode gives LOD 0 only)
    if (!card && m.lod >= kLods) ++c.unknown;
    Object o;
    o.asset = asset(card, m.block, card ? 0 : lod);
    o.klass = rdr_adapter::DrawClass::Crowd;
    o.layer = uint8_t(lod);
    o.flags = kObjVisible;
    o.world = WorldFromConstants(m.c);
    char id[64];
    const char* kind = card ? "card" : floor ? "floor" : "block";
    if (m.index != kUnknown) std::snprintf(id, sizeof id, "crowd:%s:%u:%s%u", kind, m.block, card ? "" : "member:", m.index);
    else std::snprintf(id, sizeof id, "crowd:%s:%u:%su%u", kind, m.block, card ? "" : "member:", unknown_ordinal++), ++c.unknown;
    o.id = t.AddString(id);
    {
      // (a card is the impostor of its type's posed person: it takes the type's LOD 0 palette, so a reader drawing a body for it poses it like the type)
      uint32_t first = 0, count = 0;
      if (palette(m.block, card ? 0 : lod, first, count)) {
        o.flags |= kObjSkinned;
        o.bones_first = first, o.bones_count = count;
      } else {
        ++c.unknown;  // no palette read for this (type, group): the body stays at its stored pose
      }
    }
    t.objects.push_back(o);
    ++c.objects;
  }
  if (pose_objects_) {  // S101 lane 35: the LOD 0 palette of every used type, by an invisible object (not counted in c.objects)
    std::array<bool, kTypes> used{};
    for (const MemberObs& m : members)
      if (m.block < kTypes) used[m.block] = true;
    for (uint32_t type = 0; type < kTypes; ++type) {
      if (!used[type]) continue;
      uint32_t first = 0, count = 0;
      if (!palette(type, 0, first, count)) continue;
      Object o;
      o.world = IdentityMat4();  // (Mat4 has no initialiser: an unset world would write stack bytes into every record)
      o.asset = asset(false, type, 0);
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjSkinned;  // (not kObjVisible)
      char id[32];
      std::snprintf(id, sizeof id, "crowd:pose:%02u", type);
      o.id = t.AddString(id);
      o.bones_first = first, o.bones_count = count;
      t.objects.push_back(o);
    }
  }
}

// ---- S102 T11: the cached path -------------------------------------------------------------------------------------------------------------
// Same objects, same strings in the same order, so the record's bytes are the classic path's (the offline test serialises both and compares). What
// changes: a member's id string is formatted once per (kind, type, index) and kept; the strings are interned through the flat index (no heap string
// per call); the member and palette copies, the asset / palette lookups and the object vector reuse their storage across frames.
const std::string* Contributor::CachedId(uint32_t variant, uint32_t block, uint32_t n, PooledField** pooled) {
#ifdef RDR_CROWD_IDS_PLANTS
  const uint32_t pool_variant = plant == 5 ? (variant >= 3 ? 3u : 0u) : variant;   // planted 5: the pool field is kept without the kind
  if (plant == 1) variant = variant >= 3 ? 3u : 0u;   // the kind is ignored: block, floor and card share the entries of kind "block"
  if (plant == 2) block = 0;               // the type is ignored
#else
  const uint32_t pool_variant = variant;
#endif
  if (n >= kIdCacheMax) return nullptr;
  std::vector<std::string>& v = id_cache_[variant][block];
  if (v.size() <= n) v.resize(size_t(n) + 1);
  if (pooled) {   // S102 T11b
    std::vector<PooledField>& pv = id_pool_[pool_variant][block];
    if (pv.size() <= n) pv.resize(size_t(n) + 1);
    *pooled = &pv[n];
  }
  std::string& id = v[n];
  if (id.empty()) {
    char b[64];
    const bool card = variant % 3 == 2, floor = variant % 3 == 1;
    const char* kind = card ? "card" : floor ? "floor" : "block";
    if (variant < 3) std::snprintf(b, sizeof b, "crowd:%s:%u:%s%u", kind, block, card ? "" : "member:", n);
    else std::snprintf(b, sizeof b, "crowd:%s:%u:%su%u", kind, block, card ? "" : "member:", n);
    id = b;
    ++ids_stats_.id_made;
  } else {
    ++ids_stats_.id_hits;
  }
  return &id;
}

// S102 SW1: the kept pool field of (variant, block, n); the same slot (and the same plant) CachedId hands out. n < kIdCacheMax, block < kTypes (the caller's checks).
PooledField* Contributor::PoolSlot(uint32_t variant, uint32_t block, uint32_t n) {
#ifdef RDR_CROWD_IDS_PLANTS
  const uint32_t pool_variant = plant == 5 ? (variant >= 3 ? 3u : 0u) : variant;
#else
  const uint32_t pool_variant = variant;
#endif
  std::vector<PooledField>& pv = id_pool_[pool_variant][block];
  if (pv.size() <= n) pv.resize(size_t(n) + 1);
  return &pv[n];
}

// S102 T11 F7: the cached twin of Emit above; change both (see there). The entry-string cache (entry_cache_) is keyed (card, lod, type), which is all the text
// depends on; AssetRef::variant is the constant 0 here ([U], see Emit), so it is not part of the key: if variant ever becomes a value read from the game, the key
// must gain it (the byte-equality arms and planted twin 4, a cache keyed without the lod, show such a key mistake is caught).
void Contributor::EmitCached(const FrameInfo& f, SceneTables& t, Counters& c) {
  int arena = -1;
  uint32_t other = 0;
  if (!TakeInputs(f, c, scratch_members_, scratch_palettes_, arena, other)) {   // S102 U1 (kept storage: swapped with the latch's, no allocation once grown)
    ++ids_stats_.declined;
    if (StringPool* sp = t.SeedPool()) ++sp->stats.crowd_stale;   // S102 T11b
    return;
  }
  AppendPropRows(t, c);   // S103 D5
  AppendSingleRows(t, c, arena);   // S103 D6
  const std::vector<MemberObs>& members = scratch_members_;
  const auto& palettes = scratch_palettes_;
  c.unknown += other;
  ++ids_stats_.frames;
  ids_stats_.members += members.size();
  // (the ids do not depend on the arena: the cache is kept across arena changes; only the pac string and the asset entries are per arena, and the pac string is keyed by it)
  if (f.guest_frame >= ids_window_start_ + 600 || ids_window_start_ == 0 || census_flush::Due(g_cf_ids)) {
    if (ids_window_start_ != 0) {   // the census line, every 600 frames while the lever is on (R13 / S102 rule)
      char line[320];
      std::snprintf(line, sizeof line,
                    "[rdr][crowd_ids_once] frames %llu-%llu, taken %llu, declined %llu (stale crowd), members %llu, id hits %llu, made %llu, uncached %llu",
                    (unsigned long long)ids_window_start_, (unsigned long long)f.guest_frame, (unsigned long long)ids_stats_.frames,
                    (unsigned long long)ids_stats_.declined, (unsigned long long)ids_stats_.members, (unsigned long long)ids_stats_.id_hits,
                    (unsigned long long)ids_stats_.id_made, (unsigned long long)ids_stats_.id_uncached);
      if (auto log = rdr_sw::g.log.load(std::memory_order_relaxed)) log(line);
    }
    // render-clock: lookup - start of a diagnostic counting window, not game progress
    ids_window_start_ = f.guest_frame ? f.guest_frame : 1;
  }

  // S102 T11b: a table seeded from the string pool already has its index (the pool's strings are in it) and needs room for only the strings this frame still adds (the pac string,
  // the asset entries, the pose ids, and any id the pool does not hold yet); the member ids are the pool's fields.
  StringPool* pool = t.SeedPool();
  if (pool) {
    if (pooled_for_ != pool->id()
#ifdef RDR_CROWD_IDS_PLANTS
        && plant != 6
#endif
    ) {   // another pool: the fields kept for the old one are not this one's
      for (auto& per_kind : id_pool_)
        for (auto& v : per_kind) v.clear();
      pooled_for_ = pool->id();
    }
#ifdef RDR_CROWD_IDS_PLANTS
    if (plant == 6) pooled_for_ = pool->id();
#endif
  } else {
    t.UseFlatStringIndex(members.size() + 2 * kTypes + 128, members.size() * 28 + 8192);
  }
  uint64_t pooled_hits = 0, pooled_misses = 0, fast_n = 0;
  const bool fast_ids = pool && swap_once::Has(swap_once::kCrowdIds);   // S102 SW1 (wwe2k14_rdr_swap_once bit 2): a member whose pool field is kept skips the id cache
  t.objects.reserve(t.objects.size() + members.size() + kTypes);
  t.assets.reserve(t.assets.size() + 2 * kLods * kTypes);
  {
    size_t nodes = 0;
    for (const auto& p : palettes) nodes += p.size() / 16;
    t.bones.reserve(t.bones.size() + nodes);
  }

  uint32_t pac_field = 0;
  if (arena >= 0) {
#ifdef RDR_CROWD_IDS_PLANTS
    const bool keep = plant == 3 && pac_arena_ != -2;   // planted: the pac string of the first arena stays
#else
    const bool keep = false;
#endif
    if (pac_arena_ != arena && !keep) {
      char b[64];
      std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
      pac_cache_ = b;
      pac_arena_ = arena;
    }
    pac_field = t.AddString(pac_cache_);
  }
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  std::array<uint32_t, 2 * kLods * kTypes> asset_of;
  asset_of.fill(kNone);
  auto asset = [&](bool card, uint32_t type, uint32_t lod) {
    uint32_t& slot = asset_of[(size_t(card) * kLods + lod) * kTypes + type];
    if (slot != kNone) return slot;
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_field;
#ifdef RDR_CROWD_IDS_PLANTS
    std::string& e = entry_cache_[card ? 1 : 0][plant == 4 ? 0 : lod][type];   // planted 4: keyed without the lod
#else
    std::string& e = entry_cache_[card ? 1 : 0][lod][type];
#endif
    if (e.empty()) {
      char b[32];
      if (card) std::snprintf(b, sizeof b, "card:%02u", type);
      else std::snprintf(b, sizeof b, "%u", 20000u + 40u * lod + type);
      e = b;
    }
    a.entry = t.AddString(e);
    a.variant = 0;
    if (!pac_field) ++c.unknown;
    ++c.unknown;
    t.assets.push_back(a);
    return slot = uint32_t(t.assets.size() - 1);
  };
  struct Range { uint32_t first = 0, count = 0; bool have = false; };
  std::array<Range, kTypes * kLods> range_of;
  auto palette = [&](uint32_t type, uint32_t lod, uint32_t& first, uint32_t& count) {
    const uint32_t slot = lod * kTypes + type;
    Range& r = range_of[slot];
    if (r.have) {
      first = r.first, count = r.count;
      return count != 0;
    }
    const std::vector<float>& p = palettes[slot];
    first = uint32_t(t.bones.size()), count = uint32_t(p.size() / 16);
    for (uint32_t i = 0; i < count; ++i) {
      Mat4 m;
      std::memcpy(m.m, p.data() + size_t(i) * 16, sizeof m.m);
      t.bones.push_back(m);
    }
    r = {first, count, true};
    return count != 0;
  };
  uint32_t unknown_ordinal = 0;
  for (const MemberObs& m : members) {
    if (m.block >= kTypes) {
      ++c.unknown;
      continue;
    }
    const bool card = m.card;
    const bool floor = m.floor && !card;
    const uint32_t lod = card ? kCardLayer : (m.lod < kLods ? m.lod : 0);   // S103 C1: a floor person is its group's LOD body (g = 20 lod + type, 0x8276EAC4; the walk's decode gives LOD 0 only)
    if (!card && m.lod >= kLods) ++c.unknown;
    Object o;
    o.asset = asset(card, m.block, card ? 0 : lod);
    o.klass = rdr_adapter::DrawClass::Crowd;
    o.layer = uint8_t(lod);
    o.flags = kObjVisible;
    o.world = WorldFromConstants(m.c);
    const uint32_t kind = card ? 2u : floor ? 1u : 0u;
    const bool known = m.index != kUnknown;
    const uint32_t n = known ? m.index : unknown_ordinal++;
    if (!known) ++c.unknown;
    PooledField* pf = nullptr;
    if (fast_ids && n < kIdCacheMax && (pf = PoolSlot(kind + (known ? 0u : 3u), m.block, n))->field) {   // S102 SW1: the field is kept: no id string, no cache entry touched
      o.id = pf->field;
      ++fast_n, ++pooled_hits;
    } else if (const std::string* id = CachedId(kind + (known ? 0u : 3u), m.block, n, pool ? &pf : nullptr)) {
      uint32_t fld = 0;
      if (pf) {   // S102 T11b: the pool field the entry kept: no hashing, no probe, no compare
        fld = pf->field ? pf->field : PooledFieldOf(*pool, *pf, [id]() { return *id; });
        ++(fld ? pooled_hits : pooled_misses);
      }
      o.id = fld ? fld : t.AddString(*id);   // (a string the pool does not hold yet: interned this frame the old way, queued for the pool)
    } else {   // an index past the cache: formatted into the stack, still no heap
      char b[64];
      const char* kn = card ? "card" : floor ? "floor" : "block";
      std::snprintf(b, sizeof b, known ? "crowd:%s:%u:%s%u" : "crowd:%s:%u:%su%u", kn, m.block, card ? "" : "member:", n);
      o.id = t.AddString(b);
      ++ids_stats_.id_uncached;
    }
    {
      uint32_t first = 0, count = 0;
      if (palette(m.block, card ? 0 : lod, first, count)) {
        o.flags |= kObjSkinned;
        o.bones_first = first, o.bones_count = count;
      } else {
        ++c.unknown;
      }
    }
    t.objects.push_back(o);
    ++c.objects;
  }
  if (pose_objects_) {
    std::array<bool, kTypes> used{};
    for (const MemberObs& m : members)
      if (m.block < kTypes) used[m.block] = true;
    for (uint32_t type = 0; type < kTypes; ++type) {
      if (!used[type]) continue;
      uint32_t first = 0, count = 0;
      if (!palette(type, 0, first, count)) continue;
      Object o;
      o.world = IdentityMat4();
      o.asset = asset(false, type, 0);
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjSkinned;
      std::string& pid = pose_id_[type];
      if (pid.empty()) {
        char b[32];
        std::snprintf(b, sizeof b, "crowd:pose:%02u", type);
        pid = b;
      }
      o.id = t.AddString(pid);
      o.bones_first = first, o.bones_count = count;
      t.objects.push_back(o);
    }
  }
  if (pool) pool->stats.crowd_fields += pooled_hits, pool->stats.crowd_fallback += pooled_misses;   // S102 T11b
  if (fast_ids) {   // S102 SW1: the census (one relaxed add each a frame); the id cache's own hit count keeps counting the members the fast path served
    ids_stats_.id_hits += fast_n;
    swap_once::g_count.crowd_fast.fetch_add(fast_n, std::memory_order_relaxed);
    swap_once::g_count.crowd_slow.fetch_add(uint64_t(members.size()) - fast_n, std::memory_order_relaxed);
  }
}

// ---- S102 T19: the marker path --------------------------------------------------------------------------------------------------------------------
// One CrowdRun per run of consecutive members that share (kind, person type, LOD group), one CrowdMember (12 constants + the index) per member; no Object, no id string. The
// assets, palettes, pac / entry strings and pose objects are made exactly as in Emit / EmitCached (same order of first use, same counters), so rdr_scene::ExpandCrowdRuns of
// the record equals the plain record's member list (crowd_run_markers_test.cpp compares every object). A change to what Emit writes goes into all THREE paths.
void Contributor::EmitRuns(const FrameInfo& f, SceneTables& t, Counters& c) {
  int arena = -1;
  uint32_t other = 0;
  if (!TakeInputs(f, c, scratch_members_, scratch_palettes_, arena, other)) { ++run_stats_.declined_stale; return; }   // S102 U1
  AppendPropRows(t, c);   // S103 D5
  AppendSingleRows(t, c, arena);   // S103 D6
  const std::vector<MemberObs>& members = scratch_members_;
  const auto& palettes = scratch_palettes_;
  c.unknown += other;
  ++run_stats_.frames;
  if (f.guest_frame >= run_window_start_ + 600 || run_window_start_ == 0 || census_flush::Due(g_cf_run)) {
    if (run_window_start_ != 0) {   // the census line, every 600 frames while the lever is on (S102 rule); the counts are the window's
      char line[320];
      std::snprintf(line, sizeof line,
                    "[rdr][crowd_run_markers] frames %llu-%llu, taken %llu, runs %llu, members %llu, declined %llu (stale crowd %llu, no recordable member %llu), single-member runs %llu",
                    (unsigned long long)run_window_start_, (unsigned long long)f.guest_frame, (unsigned long long)run_stats_.frames, (unsigned long long)run_stats_.runs,
                    (unsigned long long)run_stats_.members, (unsigned long long)(run_stats_.declined_stale + run_stats_.declined_empty), (unsigned long long)run_stats_.declined_stale,
                    (unsigned long long)run_stats_.declined_empty, (unsigned long long)run_stats_.singles);
      if (auto log = rdr_sw::g.log.load(std::memory_order_relaxed)) log(line);
      run_stats_ = RunStats{};
      ++run_stats_.frames;   // (this frame is the new window's first)
    }
    // render-clock: lookup - start of a diagnostic counting window, not game progress
    run_window_start_ = f.guest_frame ? f.guest_frame : 1;
  }

  t.UseFlatStringIndex(2 * kTypes + 2 * kLods * kTypes + 16, 2048);
  t.assets.reserve(t.assets.size() + 2 * kLods * kTypes);
  t.crowd_members.reserve(t.crowd_members.size() + members.size());
  t.crowd_runs.reserve(t.crowd_runs.size() + last_runs_ + 8);   // S102 T19 F5: from the previous frame's count
  if (pose_objects_) t.objects.reserve(t.objects.size() + kTypes);   // (the pose objects: at most one per person type)
  {
    size_t nodes = 0;
    for (const auto& p : palettes) nodes += p.size() / 16;
    t.bones.reserve(t.bones.size() + nodes);
  }
  uint32_t pac_field = 0;
  if (arena >= 0) {
    if (pac_arena_ != arena) {
      char b[64];
      std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", arena);
      pac_cache_ = b;
      pac_arena_ = arena;
    }
    pac_field = t.AddString(pac_cache_);
  }
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  std::array<uint32_t, 2 * kLods * kTypes> asset_of;
  asset_of.fill(kNone);
  auto asset = [&](bool card, uint32_t type, uint32_t lod) {
    uint32_t& slot = asset_of[(size_t(card) * kLods + lod) * kTypes + type];
    if (slot != kNone) return slot;
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = pac_field;
    std::string& e = entry_cache_[card ? 1 : 0][lod][type];
    if (e.empty()) {
      char b[32];
      if (card) std::snprintf(b, sizeof b, "card:%02u", type);
      else std::snprintf(b, sizeof b, "%u", 20000u + 40u * lod + type);
      e = b;
    }
    a.entry = t.AddString(e);
    a.variant = 0;
    if (!pac_field) ++c.unknown;
    ++c.unknown;
    t.assets.push_back(a);
    return slot = uint32_t(t.assets.size() - 1);
  };
  struct Range { uint32_t first = 0, count = 0; bool have = false; };
  std::array<Range, kTypes * kLods> range_of;
  auto palette = [&](uint32_t type, uint32_t lod, uint32_t& first, uint32_t& count) {
    const uint32_t slot = lod * kTypes + type;
    Range& r = range_of[slot];
    if (r.have) {
      first = r.first, count = r.count;
      return count != 0;
    }
    const std::vector<float>& p = palettes[slot];
    first = uint32_t(t.bones.size()), count = uint32_t(p.size() / 16);
    for (uint32_t i = 0; i < count; ++i) {
      Mat4 m;
      std::memcpy(m.m, p.data() + size_t(i) * 16, sizeof m.m);
      t.bones.push_back(m);
    }
    r = {first, count, true};
    return count != 0;
  };
  const uint32_t at = uint32_t(t.objects.size());   // the members go here when a reader expands the markers (the pose objects stay plain, after them)
  const size_t runs_before = t.crowd_runs.size();
  std::array<bool, kTypes> used{};
  uint32_t run_kind = 9, run_block = 0, run_layer = 0;   // the open run's key (kind 9 = none open)
  bool run_has_palette = false;
  uint32_t recorded = 0;
  for (const MemberObs& m : members) {
    if (m.block >= kTypes) {
      ++c.unknown;
      continue;
    }
    used[m.block] = true;
    const bool card = m.card;
    const bool floor = m.floor && !card;
    const uint32_t lod = card ? kCardLayer : (m.lod < kLods ? m.lod : 0);   // S103 C1: a floor person is its group's LOD body (g = 20 lod + type, 0x8276EAC4; the walk's decode gives LOD 0 only)
    if (!card && m.lod >= kLods) ++c.unknown;
    const uint32_t kind = card ? uint32_t(kRunCard) : floor ? uint32_t(kRunFloor) : uint32_t(kRunBlock);
#ifdef RDR_CROWD_RUNS_PLANTS
    const bool same = run_kind == kind && run_block == m.block && (run_plant == 1 || run_layer == lod);   // planted 1: a run goes on across a LOD (= palette) change
#else
    const bool same = run_kind == kind && run_block == m.block && run_layer == lod;
#endif
    if (!same) {
      CrowdRun r;
      r.at = at;
      r.first_member = uint32_t(t.crowd_members.size());
      r.count = 0;
      r.asset = asset(card, m.block, card ? 0 : lod);
      uint32_t first = 0, count = 0;
      run_has_palette = palette(m.block, card ? 0 : lod, first, count);
      if (run_has_palette) r.skinned = 1, r.bones_first = first, r.bones_count = count;
      r.kind = uint8_t(kind), r.block = uint8_t(m.block), r.layer = uint8_t(lod);
      t.crowd_runs.push_back(r);
      run_kind = kind, run_block = m.block, run_layer = lod;
    }
    ++t.crowd_runs.back().count;
    CrowdMember cm;
    std::memcpy(cm.c, m.c, sizeof cm.c);
    if (m.index != kUnknown) cm.index = m.index;
    else cm.index = kCrowdNoIndex, ++c.unknown;   // (the id's per-record ordinal is the expander's count of such members, in order)
    t.crowd_members.push_back(cm);
    if (!run_has_palette) ++c.unknown;   // no palette read for this (type, group): the body stays at its stored pose (counted per member, as in Emit)
    ++c.objects;
    ++recorded;
  }
  run_stats_.members += recorded;
  run_stats_.runs += t.crowd_runs.size() - runs_before;
  last_runs_ = t.crowd_runs.size() - runs_before;
  if (!recorded) ++run_stats_.declined_empty;
  for (size_t i = runs_before; i < t.crowd_runs.size(); ++i)
    if (t.crowd_runs[i].count == 1) ++run_stats_.singles;
  if (pose_objects_) {
    for (uint32_t type = 0; type < kTypes; ++type) {
      if (!used[type]) continue;
      uint32_t first = 0, count = 0;
      if (!palette(type, 0, first, count)) continue;
      Object o;
      o.world = IdentityMat4();
      o.asset = asset(false, type, 0);
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjSkinned;
      std::string& pid = pose_id_[type];
      if (pid.empty()) {
        char b[32];
        std::snprintf(b, sizeof b, "crowd:pose:%02u", type);
        pid = b;
      }
      o.id = t.AddString(pid);
      o.bones_first = first, o.bones_count = count;
      t.objects.push_back(o);
    }
  }
#ifdef RDR_CROWD_RUNS_PLANTS
  if (run_plant == 3)   // planted 3: `at` taken after the pose objects (the members would be expanded behind them)
    for (size_t i = runs_before; i < t.crowd_runs.size(); ++i) t.crowd_runs[i].at = uint32_t(t.objects.size());
#endif
}

Contributor& The() {
  static Contributor c;
  return c;
}

}  // namespace rdr_scene::live::crowd
