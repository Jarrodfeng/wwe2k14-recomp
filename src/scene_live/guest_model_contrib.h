// The live side of the GuestModel rows (Session 103, lanes CW-2 and CW-2b): the model-entry hook's one call (OnModelEntry), the contributor that writes the rows into the record, and the census lines.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Hard rule 1: what is read here lives only in this process and in local scene records under out\.
//
// OnModelEntry runs at 0x82C04E1C in sub_82C04DF0 (r3 = model) on a game thread: it returns at once unless the class is registered, the frame is being recorded, the model is one the wrestler class sized
// (a skeleton, WantCount != 0) and the record can NOT name it (g_is_named = WrestlerContributor::RecordCanName: the emitter's own three sources; CW2_review bug 1). A model the record cannot name is read by
// the Planner (guest_model_reader.h): a words-only layout pass every call, the byte pass only on a layout change, a VB / IB unlock of one of its buffers (buffer_unlock_log.h) or the forced re-read period.
// The sampler textures are resolved through the registry (guest_model_tex.h) when a read is new or incomplete, and re-checked for the override block's textures every kOverrideRefreshFrames.
// Every byte run goes into the process byte store by hash (guest_model_store.h) so the in-process consumer never lacks bytes; the record also carries a hash's bytes the first time and every
// kGmResendFrames (odd) frames, for a file consumer. Log: `[scene][guest_model] model M parts P hash H vtx V idx I`, `[rdr][caw] textures: parts P, bound B, from override O, missing M` once per new content.
// Off Windows every read is page-checked first (wrestler_hook.h GuestRangeReadable / PhysRangeReadable), so it runs on the phone too (the caller skips it on a non-4 KB-page kernel).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "guest_model_emit.h"
#include "guest_model_reader.h"
#include "guest_model_store.h"
#include "guest_model_tex.h"
#include "picture_global.h"   // S103 CTX2: the birth hook's creator of a texture key (CAWD site 8)
#include "wrestler_hook.h"   // ReadWord / ReadGuest (SafeCopy): the game's memory
#include "lock_stats.h"   // BHF1: run_mu_ cost counters

namespace rdr_scene::live::guestmodel {

using guest_model::ModelRead;
using guest_model::PartRead;

struct GuestMemory {   // the reader's `M` over the game's memory
  bool u32(uint32_t a, uint32_t& v) const { return wwe2k14::wrestlers::ReadWord(a, v); }
  bool bytes(uint32_t a, uint32_t n, uint8_t* dst) const { return wwe2k14::wrestlers::ReadGuest(a, dst, n); }
};
// Pixel memory by PHYSICAL address (the same reader as inputs_glue.h CrowdGuestMem::phys; the range check is ours, A8).
inline bool PhysRead(uint32_t addr, uint32_t n, uint8_t* dst) {
  if (!n || n > (8u << 20) || !rdr_scene::live::guest_tex::PhysRangeOk(addr, n)) return false;
  const auto* host = wwe2k14::wrestlers::GuestMem()->TranslatePhysical<const uint8_t*>(addr);
  return host && wwe2k14::wrestlers::PhysRangeReadable(addr, n) && wwe2k14::wrestlers::SafeCopy(dst, host, n);
}

// CTX2 site 8: the resolver's creator oracle (the process-wide PictureSource's lives; Unknown when the key was never seen born).
inline bool BirthOfKey(uint32_t key, uint8_t& creator, uint32_t& site) {
  rdr_scene::live::picsrc::Creator c = rdr_scene::live::picsrc::Creator::Unknown;
  const bool seen = rdr_scene::live::picsrc::TheSource().BirthOf(key, c, site);
  creator = c == rdr_scene::live::picsrc::Creator::SceneCopy ? uint8_t(rdr_scene::live::picsrc::Creator::Unknown) : uint8_t(c);   // S105 A4: the model textures' creator codes stop at Gpu (tex8888_by_creator[4]); the scene copy is "unclassified" for them, as before
  return seen;
}

inline std::atomic<bool> g_registered{false};
inline bool (*g_is_named)(uint32_t model) = nullptr;   // true = the record can name this model (WrestlerContributor::RecordCanName): not read
inline bool (*g_want)(uint32_t model) = nullptr;       // true = a wrestler-class skeleton (the update hook sized it)

class GuestModelContributor final : public Contributor {
 public:
  const char* Name() const override { return "guest_models"; }

  // ---- hook side (a game thread) ----
  void Run(uint32_t model, uint64_t frame) {
    if (g_want && !g_want(model)) return;   // before any lock: most model entries are not wrestler-class skeletons
    if (g_is_named && g_is_named(model)) { named_skips_.fetch_add(1, std::memory_order_relaxed); return; }   // the record names it: nothing is read (CW2_review bug 1)
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = pending_.find(model);
      if (it != pending_.end() && it->second.frame == frame) return;   // once per model per frame
    }
    TimedLock<std::mutex> run(run_mu_, run_lock_);   // BHF1 B3: counted, not cut: the wait behind another worker's read and the time held across the whole read (census: run_lock)
    tex_.born_of = &BirthOfKey;   // CTX2 site 8
    bool fresh = false;
    std::shared_ptr<ModelRead> rd = planner_.Update(GuestMemory{}, model, true, false, frame, &fresh);
    if (!rd) return;
    if (rd->why != guest_model::Why::Ok) { Count(*rd); return; }
    Planner::State* st = planner_.Find(model);
    bool has_override = false;
    for (const PartRead& p : rd->p)
      for (const guest_model::SamplerRead& s : p.samplers) has_override = has_override || s.has_override;
    // `st` is never null here: Planner::Update creates states_[model] before it returns a read, and Prune runs only at the end of this call under run_mu_ (CTF review 7). Nothing below depends on it anyway: without a state the model is re-asked every call.
    uint64_t no_state = 0;
    uint64_t no_seen = 0;
    const bool tex_due = guest_model::TexCallStep(*rd, has_override, fresh, st ? st->tex_checked : no_state, frame, st ? &st->dirty_seen : &no_seen);   // S105 A5c: + the flush rule   // the one due / checked rule (the unit calls the same function)
    if (fresh) {
      tex_.Resolve(GuestMemory{}, &PhysRead, *rd, frame);   // `rd` is not shared yet: in place
      PutBytes(*rd, frame);
      Count(*rd);
    } else if (tex_due) {
      // resolve into the reused scratch; the read (its vertex / index bytes) is copied only when something changed (CTF review 4)
      if (tex_.ResolveTo(GuestMemory{}, &PhysRead, *rd, scratch_, frame)) {
        auto c = std::make_shared<ModelRead>(*rd);   // copy on write: the pending / emitted read is never mutated
        guest_model::ApplyTex(*c, std::move(scratch_));
        rd = c;
        if (st) st->read = c;
        PutBytes(*rd, frame);
      }
    }
    Submit(rd, frame);
    if ((frame & 255) == 0) planner_.Prune(frame);
  }

  void Emit(const FrameInfo& f, SceneTables& t, Counters& c) override {
    std::lock_guard<std::mutex> lk(mu_);
    GuestModelTables& g = t.guest_models;
    guest_model::ByteStore::Process().Tick(f.guest_frame);
    for (auto it = pending_.begin(); it != pending_.end();) {
      const Item& item = it->second;
      if (item.frame + 2 < f.guest_frame) { it = pending_.erase(it); continue; }   // the model was not drawn for 2 frames: gone
      const ModelRead& m = *item.read;
      if (m.parts_ok) {
        const uint32_t added = guest_model::AppendModelRows(m, g, blobs_, f.guest_frame);
        c.objects += added;
        {   // keep what is still emitted alive in the process byte store (its idle sweep is by the writer's frame)
          guest_model::ByteStore& bs = guest_model::ByteStore::Process();
          for (const PartRead& p : m.p)
            if (p.Ok()) bs.Has(p.vtx_hash, f.guest_frame), bs.Has(p.idx_hash, f.guest_frame);
          for (const guest_model::TexRead& tx : m.textures) bs.Has(tx.hash, f.guest_frame);
        }
        const uint64_t sig = guest_model::TexSignature(m);
        if (added && (last_logged_[m.model] != m.hash || last_tex_sig_[m.model] != sig)) {
          const bool new_geom = last_logged_[m.model] != m.hash;
          last_logged_[m.model] = m.hash, last_tex_sig_[m.model] = sig;
          if (new_geom)
            pending_lines_.push_back("[scene][guest_model] model " + Hex(m.model) + " parts " + std::to_string(m.parts_ok) + " hash " + Hex64(m.hash) + " vtx " + std::to_string(m.vertices) + " idx " + std::to_string(m.indices) +
                                     (m.parts_unreadable ? " (unreadable parts " + std::to_string(m.parts_unreadable) + ")" : std::string()) + (m.parts_extra_streams ? " (extra streams " + std::to_string(m.parts_extra_streams) + ")" : std::string()) + EffectsText(m));
          pending_lines_.push_back(TexLine(m));
          if (m.ov_dump_n && ov_dumped_.size() < kMaxOvDumpLines && ov_dumped_.emplace(m.model, true).second) pending_lines_.push_back(OverrideLine(m));   // CTX2: once per model
        }
        // the refused samplers' words (CTEX site 4): only when the refusal set changed, and at most kMaxRefusedLines lines a run (CTF review 6)
        const uint64_t rsig = guest_model::RefusedSig(m.n_refused, m.refused);
        if (added && m.n_refused && last_refused_sig_[m.model] != rsig) {
          last_refused_sig_[m.model] = rsig;
          for (uint32_t i = 0; i < m.n_refused && refused_lines_printed_ < kMaxRefusedLines; ++i, ++refused_lines_printed_) pending_lines_.push_back(guest_model::RefusedText(m, i));
        }
      }
      ++it;
    }
    // render-clock: ctl - memo-prune cadence by guest-frame mask (housekeeping of our own caches, never a picture value)
    if ((f.guest_frame & 255) == 0) {   // prune the by-hash / by-model memos
      blobs_.Prune(f.guest_frame);
      for (auto l = last_logged_.begin(); l != last_logged_.end();) {
        if (!pending_.count(l->first)) last_tex_sig_.erase(l->first), last_refused_sig_.erase(l->first), l = last_logged_.erase(l);
        else ++l;
      }
    }
  }
  void Census(std::string& out) override {
    std::string lines;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (const std::string& l : pending_lines_) lines += "\n" + l;
      pending_lines_.clear();
    }
    std::lock_guard<std::mutex> run(run_mu_);
    const guest_model::PlannerStats& s = planner_.stats();
    for (std::string& n : tex_.TakeNotes()) lines += "\n" + n;   // CTX2: one line per distinct 8888 picture (its creator)
    const guest_model::TexStats& t = tex_.stats();
    out += " | guest models calls " + std::to_string(s.calls) + ", named (not read) " + std::to_string(s.skipped_named) + ", named by the record (skipped before the planner) " + std::to_string(named_skips_.load(std::memory_order_relaxed)) + ", layout reads " + std::to_string(s.layout_reads) + ", byte reads " + std::to_string(s.byte_reads) + " (" +
           std::to_string(s.bytes_read >> 10) + " KB), reused " + std::to_string(s.reused) + ", forced " + std::to_string(s.forced) + " (missed unlock " + std::to_string(s.missed_unlock) + "), unreadable parts " + std::to_string(unreadable_) +
           ", bad flags " + std::to_string(bad_flags_) + ", no descriptor " + std::to_string(no_desc_) + " (D of another class " + std::to_string(d_other_class_) + "), via D " + std::to_string(via_d_) + " (H+16 != m+372: " + std::to_string(parts_mismatch_) + ")" + ", extra-stream parts (HOLE) " + std::to_string(extra_) + ", textures: samplers " + std::to_string(t.samplers) + " bound " +
           std::to_string(t.bound) + " override " + std::to_string(t.from_override) + " missing " + std::to_string(t.missing) + " deferred " + std::to_string(t.deferred) + ", refused: " + guest_model::WhySummary(t) + ", override refused " + std::to_string(t.override_refused) + ", byte reads " + std::to_string(t.byte_reads) + ", retry calls " + std::to_string(t.retry_calls) +
           ", override block entries " + std::to_string(ov_entries_) + " matched " + std::to_string(ov_matched_) + " (equal only ignoring case " + std::to_string(ov_ci_) + "), 8888 pictures " + std::to_string(t.tex8888) + " (creator unknown " + std::to_string(t.tex8888_by_creator[0]) + " loader " +
           std::to_string(t.tex8888_by_creator[1]) + " movie " + std::to_string(t.tex8888_by_creator[2]) + " gpu " + std::to_string(t.tex8888_by_creator[3]) + "), logos bound " + std::to_string(t.logo_bound) + " missing " + std::to_string(t.logo_missing) + ", flush re-reads " + std::to_string(t.dirty_rereads) + " (deferred " + std::to_string(t.dirty_deferred) + ", unwatched " + std::to_string(t.unwatched) + "), 8888 guard unchanged " + std::to_string(t.tex8888_guard_same) + " re-read " + std::to_string(t.tex8888_rereads) + ", engine default cube " + std::to_string(t.engine_default_cube) + ", store " + std::to_string(guest_model::ByteStore::Process().bytes() >> 20) + " MB, run_lock " + run_lock_.Text();
    out += lines;
  }
  guest_model::PlannerStats PlannerStats() { std::lock_guard<std::mutex> run(run_mu_); return planner_.stats(); }

 private:
  using Planner = guest_model::Planner;
  struct Item { std::shared_ptr<const ModelRead> read; uint64_t frame = 0; };
  static std::string Hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
  static std::string Hex64(uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }
  void Submit(const std::shared_ptr<ModelRead>& r, uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_[r->model] = Item{r, frame};
  }
  // S103 WC R3 (the one-time model line): the descriptors' source ("via D" = [m+824] was 0, the data object's [H+20]), D's vtable word when it is not the expected class, and each distinct part effect name ([desc+124]) so a live run names the CAW / weapon effects.
  static std::string EffectsText(const ModelRead& m) {
    std::string t = m.desc_via_d ? " desc via D" : std::string();
    if (m.d_vtable) t += " D vt " + Hex(m.d_vtable);
    if (m.parts_mismatch) t += " (H+16 != m+372)";
    std::string names;
    uint32_t shown = 0;
    for (size_t i = 0; i < m.p.size() && shown < 8; ++i) {
      if (!m.p[i].Ok() || !m.p[i].effect[0]) continue;
      const std::string e(m.p[i].effect, strnlen(m.p[i].effect, guest_model::kNameBytes));
      if (names.find("," + e + ",") != std::string::npos || names.compare(0, e.size() + 1, e + ",") == 0) continue;
      names += e + ",";
      ++shown;
    }
    if (!names.empty()) t += " fx " + names.substr(0, names.size() - 1);
    return t;
  }
  void Count(const ModelRead& r) {
    if (r.desc_via_d) ++via_d_;
    if (r.parts_mismatch) ++parts_mismatch_;
    if (r.why == guest_model::Why::NoDesc && r.d_vtable) ++d_other_class_;
    if (r.why == guest_model::Why::NoDesc) ++no_desc_;
    ov_entries_ += r.ov_entries, ov_matched_ += r.ov_matched, ov_ci_ += r.ov_matched_ci;
    unreadable_ += r.parts_unreadable, extra_ += r.parts_extra_streams, bad_flags_ += r.parts_bad_flags;
  }
  // the process byte store: everything the rows will name by hash (the in-process consumer asks here)
  void PutBytes(const ModelRead& m, uint64_t frame) {
    guest_model::ByteStore& bs = guest_model::ByteStore::Process();
    for (const PartRead& p : m.p) {
      if (!p.Ok() || !p.vtx) continue;
      bs.PutShared(p.vtx_hash, p.vtx, frame);
      std::vector<uint8_t> ib(p.idx.size() * 2);
      std::memcpy(ib.data(), p.idx.data(), ib.size());
      bs.Put(p.idx_hash, std::move(ib), frame);
    }
    for (const guest_model::TexRead& t : m.textures) bs.PutShared(t.hash, t.bytes, frame);
  }
  // CTX2 (CAWD Q2): the override block's entry names (+8 -> the string) and the first word of each entry's handle pointer, so one run says why `matched 0` of `entries 11`.
  static std::string OverrideLine(const ModelRead& m) {
    std::string s = "[rdr][caw] override block of model " + Hex(m.model) + ": " + std::to_string(m.ov_entries) + " entries, " + std::to_string(m.ov_matched) + " matched by name, " + std::to_string(m.ov_matched_ci) + " more equal only ignoring case:";
    for (uint32_t j = 0; j < m.ov_dump_n; ++j) s += " [" + std::to_string(j) + "] '" + std::string(m.ov_names[j]) + "' h0=" + Hex(m.ov_handle0[j]);
    return s;
  }
  static std::string TexLine(const ModelRead& m) {
    uint32_t parts = 0, bound = 0, ov = 0, missing = 0, cube = 0;
    for (size_t pi = 0; pi < m.p.size(); ++pi) {
      if (!m.p[pi].Ok()) continue;
      ++parts;
      for (size_t si = 0; pi < m.tex_state.size() && si < m.tex_state[pi].size(); ++si) {
        const uint8_t s = m.tex_state[pi][si];
        if (s == kGtBound) ++bound;
        else if (s == kGtOverride) ++ov;
        else if (s == kGtEngineDefault) ++cube;
        else ++missing;
      }
    }
    std::string line = "[rdr][caw] textures: parts " + std::to_string(parts) + ", bound " + std::to_string(bound) + ", from override " + std::to_string(ov) + ", missing " + std::to_string(missing) + ", engine default cube " + std::to_string(cube) + ", override block entries " + std::to_string(m.ov_entries) + " matched " +
                       std::to_string(m.ov_matched) + (m.logo.has ? std::string(", logo7 ") + (m.logo_state[0] == kGtBound ? "bound" : "missing") + " logo4 " + (m.logo_state[1] == kGtBound ? "bound" : "missing") : std::string()) + (m.tex_retry ? ", retrying" : "") + " (model " + Hex(m.model) + " hash " + Hex64(m.hash) + ")";
    return line;
  }
  std::mutex mu_, run_mu_;   // mu_: pending_ + the Emit-side memos; run_mu_: the planner and the resolver (hook threads); never held together by Emit
  LockStats run_lock_;
  std::unordered_map<uint32_t, Item> pending_;
  guest_model::BlobSender blobs_;
  std::unordered_map<uint32_t, uint64_t> last_logged_, last_tex_sig_;
  std::vector<std::string> pending_lines_;
  std::atomic<uint64_t> named_skips_{0};
  Planner planner_;
  guest_model::TexResolver tex_;
  guest_model::TexOut scratch_;   // the resolver's reused result (run_mu_)
  std::unordered_map<uint32_t, uint64_t> last_refused_sig_;   // per model: the refusal set last printed
  uint32_t refused_lines_printed_ = 0;                        // per run cap (kMaxRefusedLines)
  static constexpr uint32_t kMaxRefusedLines = 96;
  uint64_t ov_entries_ = 0, ov_matched_ = 0, ov_ci_ = 0;
  std::unordered_map<uint32_t, bool> ov_dumped_;   // models whose override entries were printed (CTX2; at most kMaxOvDumpLines)
  static constexpr size_t kMaxOvDumpLines = 8;
  uint64_t unreadable_ = 0, no_desc_ = 0, extra_ = 0, bad_flags_ = 0;
  uint64_t via_d_ = 0, parts_mismatch_ = 0, d_other_class_ = 0;   // S103 WC R3: models whose descriptors came from the data object; [H+16] != [m+372]; [m+824] == 0 with a D that is not the expected class
};

inline GuestModelContributor& Contributor_() { static GuestModelContributor c; return c; }
inline void Register() {
  if (g_registered.exchange(true)) return;   // off Windows the reads are page-checked first (wrestler_hook.h GuestRangeReadable / PhysRangeReadable)
  rdr_scene::live::Register(&Contributor_());
}

// The model-entry hook's call (fp_probe.h wwe2k14_model_entry): one small call, read only.
inline void OnModelEntry(uint32_t model, uint64_t frame) {
  if (!g_registered.load(std::memory_order_relaxed) || !rdr_scene::live::Enabled()) return;
  Contributor_().Run(model, frame);
}

}  // namespace rdr_scene::live::guestmodel
