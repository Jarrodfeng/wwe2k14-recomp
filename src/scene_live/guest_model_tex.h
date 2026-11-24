// The guest model's sampler pictures (Session 103, lane CW-2b; docs\research\s103\CW2_review.md Q2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: no game or SDK includes. For each sampler of each part (guest_model_reader.h SamplerRead: the key word 0 = the registry handle, or the override block's handle) it resolves the handle through the ONE
// registry walk (guest_texture.h -> texture_registry.h: sub_82294F20's table [0x8392DD9C], wrapper, header, fetch dwords: the game's own texture object), reads mip 0 as linear BC1 / BC2 / BC3 blocks through the
// same reader the crowd's pictures use, and keeps the blocks by content hash. Cost control: the identity of a texture object (handle + its six fetch dwords) is cached, so the bytes are read once per identity;
// a texture that came from the OVERRIDE block (CEditEx composite textures [L]) is re-read every kOverrideRefreshFrames because the game may rewrite it in place; at most `max_reads` byte reads a call (the
// rest are deferred: ModelRead::tex_incomplete, asked again next call). Hard rule 1: the blocks live in process memory and in local scene records under out\ only.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "guest_model_reader.h"
#include "guest_texture.h"
#include "guest_tex_dirty.h"   // S105 A5c: the unlock flush marks a read picture dirty
#include "../renderer_scene/guest_model_rows.h"

namespace rdr_scene::live::guest_model {

// guest_texture.h's memory contract over the reader's M (u32(a, out) -> bool): a refused word sets `fail` (the picture then reports its own why).
template <class M>
struct WordMem {
  const M& m;
  mutable bool fail = false;
  uint32_t u32(uint32_t a) const {
    uint32_t v = 0;
    if (!m.u32(a, v)) fail = true;
    return v;
  }
};

// CTF: test only. 31 = a sampler with key 0 is not counted as its own reason (the old blind spot); 32 = no re-ask of a retryable miss (TexDue); 33 = tex_checked re-set on every call (TexCallStep).
// CTX2: 34 = the 8888 shape is not allowed (allow_8888 off: the CAW model's linear BGRA pictures are refused as format again).
// S105 A5c: 36 = a flushed (dirty) picture is not refreshed at once (the 61-frame guard only); 37 = the model's logo keys are not resolved.
inline int g_gm_tex_plant = 0;
// why[] slot for "the sampler has no key at all" (key word 0 == 0); 0..8 are guest_tex::Why.
inline constexpr uint32_t kWhyNoKey = 9;
// CTX2 (CAWD site 8): a k_8_8_8_8 picture whose creator is a render-target / movie creator (picture_source.h Creator::Gpu / Movie): a stale-surface risk, never read as bytes.
inline constexpr uint32_t kWhyGpuCreator = 10;
// CTX2 (CAWD, kind 1): a sampler whose texture object is a CUBE (the registry's wrapper kind picks the header; its fetch dword 5 dimension is 3): the engine supplies a default cube for it; the row's state is kGtEngineDefault,
// it is not a missing picture (not counted in why[] / missing).
inline constexpr uint32_t kWhyEngineCube = 11;
// CTX2 (review fix 1): a k_8_8_8_8 picture born at a site the kCreators table does not classify (or never seen born): refused like picture_source.h A4 refuses it (only a texture LOADER's pictures are CPU-written bytes).
inline constexpr uint32_t kWhyUnclassified = 12;
// The creator code the resolver's `born_of` hook returns (the same numbers as picsrc::Creator: Unknown, Loader, Movie, Gpu).
enum : uint8_t { kCreatorUnknown = 0, kCreatorLoader = 1, kCreatorMovie = 2, kCreatorGpu = 3 };
inline const char* CreatorName(uint8_t c) { return c == kCreatorLoader ? "loader" : c == kCreatorMovie ? "movie" : c == kCreatorGpu ? "gpu" : "unknown"; }
inline const char* WhyName(uint32_t i) {
  return i == kWhyNoKey ? "no key" : i == kWhyGpuCreator ? "gpu or movie picture" : i == kWhyEngineCube ? "engine default cube" : i == kWhyUnclassified ? "unclassified creator" : (i <= uint32_t(guest_tex::Why::Memory) ? guest_tex::WhyText(guest_tex::Why(i)) : "?");
}
// A refusal a LATER call can cure (the registry entry / wrapper / header / pixel page is not there yet); Format / Size / NotPlain2D are permanent, a key of 0 is the model's own word (a new layout read changes it).
inline bool WhyRetryable(uint32_t i) {
  return i == uint32_t(guest_tex::Why::NoRegistry) || i == uint32_t(guest_tex::Why::NoHandle) || i == uint32_t(guest_tex::Why::WrapperUnreadable) || i == uint32_t(guest_tex::Why::NoHeader) || i == uint32_t(guest_tex::Why::Memory);
}
struct TexStats {
  uint64_t samplers = 0, bound = 0, from_override = 0, missing = 0, override_refused = 0, byte_reads = 0, cache_hits = 0, deferred = 0, refreshed_changed = 0, retry_calls = 0;
  uint64_t engine_default_cube = 0;   // CTX2: samplers whose texture object is a cube (fetch dimension 3, CAWD kind 1): the engine's default stands, not a missing picture
  uint64_t tex8888 = 0, tex8888_by_creator[4] = {};   // CTX2: k_8_8_8_8 pictures read (first sight of an identity), by creator (kCreator*)
  uint64_t logo_bound = 0, logo_missing = 0, dirty_rereads = 0, dirty_deferred = 0, unwatched = 0;   // S105 A5c: logo pictures bound / not read; pictures re-read at once on the game's unlock flush; a flushed one the byte budget put off; headers the dirty table had no room to watch
  uint64_t tex8888_guard_same = 0, tex8888_rereads = 0;   // CTX2: refresh checks that found the sampled bytes unchanged / re-read the whole picture after the guard changed
  uint64_t why[16] = {};   // guest_tex::Why of a refused picture; [kWhyNoKey] = key 0. Counts ATTEMPTS (an override refusal and a key refusal of one sampler are two)
};
// "no handle 3, format not supported 1, no key 2" (nonzero slots only; "none" when empty).
inline std::string WhySummary(const TexStats& t) {
  std::string s;
  for (uint32_t i = 1; i < 16; ++i)
    if (t.why[i]) s += (s.empty() ? "" : ", ") + std::string(WhyName(i)) + " " + std::to_string(t.why[i]);
  return s.empty() ? "none" : s;
}
// One refused sampler's words on one line (contrib's TexLine prints the first kMaxRefusedProbes of a model).
inline std::string RefusedText(const ModelRead& m, uint32_t i) {
  const RefusedProbe& r = m.refused[i];
  const char* nm = (r.part < m.p.size() && r.sampler < m.p[r.part].samplers.size()) ? m.p[r.part].samplers[r.sampler].name : "?";
  char b[320];
  std::snprintf(b, sizeof b, "[rdr][caw] refused sampler%s: part %u sampler %u %.16s why %s key %08X %08X stored %08X wrapper %08X vt %08X kind %u header %08X fetch %08X %08X %08X %08X %08X %08X", r.ov ? " (override handle)" : "", r.part, r.sampler, nm, WhyName(r.why), r.key0, r.key1, r.stored_id, r.wrapper,
                r.vtable, r.kind, r.header, r.fetch[0], r.fetch[1], r.fetch[2], r.fetch[3], r.fetch[4], r.fetch[5]);
  return b;
}

// The resolver's result for one model, kept apart from the ModelRead so a call that changes nothing copies nothing (CTF review 4): the contributor owns one scratch TexOut and reuses its capacity.
struct TexOut {
  std::vector<std::vector<uint64_t>> tex_hash;
  std::vector<std::vector<uint8_t>> tex_state;
  std::vector<TexRead> textures;
  bool tex_incomplete = false, tex_retry = false, tex_8888 = false;
  uint8_t n_refused = 0;
  RefusedProbe refused[kMaxRefusedProbes];
  uint64_t logo_hash[2] = {};   // S105 A5c: the model's two logo pictures (ModelRead::logo)
  uint8_t logo_state[2] = {};
};
inline void ApplyTex(ModelRead& mr, TexOut&& o) {
  mr.tex_hash = std::move(o.tex_hash), mr.tex_state = std::move(o.tex_state), mr.textures = std::move(o.textures);
  mr.tex_incomplete = o.tex_incomplete, mr.tex_retry = o.tex_retry, mr.tex_8888 = o.tex_8888, mr.n_refused = o.n_refused;
  for (int i = 0; i < 2; ++i) mr.logo_hash[i] = o.logo_hash[i], mr.logo_state[i] = o.logo_state[i];   // S105 A5c
  for (uint32_t i = 0; i < kMaxRefusedProbes; ++i) mr.refused[i] = o.refused[i];
}
// A hash of the refusal set (which sampler, which reason, which handle): the log prints the refused lines only when it changes (CTF review 6).
inline uint64_t RefusedSig(uint32_t n, const RefusedProbe* r) {
  uint64_t h = 0x5EF05EDull;
  for (uint32_t i = 0; i < n && i < kMaxRefusedProbes; ++i) h = HashU32(uint32_t(r[i].part) << 24 | uint32_t(r[i].sampler) << 16 | uint32_t(r[i].why) << 8 | (r[i].ov ? 1u : 0u), HashU32(r[i].key0, h));
  return h;
}

class TexResolver {
 public:
  static constexpr uint64_t kOverrideRefreshFrames = 61;   // odd: a composite texture rewritten in place is seen within a second
  static constexpr size_t kMaxEntries = 256;
  // Resolves `mr`'s samplers into `o` (a scratch the caller owns; `mr` is only read). `phys(addr, n, dst)` reads pixel memory by PHYSICAL address. Returns true when any picture hash, state, retry / incomplete flag or refusal set differs from `mr`'s.
  // Counting (CTF review 5, chosen: count once): a re-ask of a retryable miss (mr.tex_retry on entry) adds nothing to samplers / missing / why[] / override_refused again, only retry_calls and any NEW bound picture.
  template <class M, class P>
  bool ResolveTo(const M& m, P&& phys, const ModelRead& mr, TexOut& o, uint64_t frame, uint32_t max_reads = 2) {
    const WordMem<M> wm{m};
    quiet_ = mr.tex_retry;
    if (quiet_) ++st_.retry_calls;
    o.textures.clear();
    o.tex_incomplete = false, o.tex_retry = false, o.tex_8888 = false, o.n_refused = 0;
    o.tex_hash.resize(mr.p.size()), o.tex_state.resize(mr.p.size());
    for (int i = 0; i < 2; ++i) o.logo_hash[i] = 0, o.logo_state[i] = uint8_t(kGtMissing);
    uint32_t reads = 0;
    reads8888_ = 0;
    for (size_t pi = 0; pi < mr.p.size(); ++pi) {
      const PartRead& pr = mr.p[pi];
      o.tex_hash[pi].assign(pr.samplers.size(), 0), o.tex_state[pi].assign(pr.samplers.size(), uint8_t(kGtMissing));
      for (size_t si = 0; si < pr.samplers.size(); ++si) {
        const SamplerRead& s = pr.samplers[si];
        if (!quiet_) ++st_.samplers;
        uint64_t hash = 0;
        uint8_t state = uint8_t(kGtMissing);
        bool deferred = false, retry = false, ov_refused = false;
        uint8_t ov_why = 0;
        if (s.has_override) {   // the override block first (CEditEx composite textures [L]); a handle that does not resolve falls back to the model's own key
          last_why_ = 0;
          const uint64_t h = One(wm, phys, s.ov_key, true, frame, reads, max_reads, o, deferred);
          if (h) hash = h, state = uint8_t(kGtOverride);
          else if (!deferred) {
            if (!quiet_) ++st_.override_refused;
            ov_refused = true, ov_why = last_why_, retry = WhyRetryable(ov_why);   // a composite that is not registered yet may appear later
          }
        }
        last_why_ = s.key ? 0 : uint8_t(kWhyNoKey);
        if (!hash && !deferred) {
          if (s.key) {
            const uint64_t h = One(wm, phys, s.key, false, frame, reads, max_reads, o, deferred);
            if (h) hash = h, state = uint8_t(kGtBound);
            else if (last_why_ == kWhyEngineCube) state = uint8_t(kGtEngineDefault);   // a cube (fetch dimension 3): the engine's default stands; no picture, not a refusal
          } else if (!s.has_override && g_gm_tex_plant != 31 && !quiet_) {
            ++st_.why[kWhyNoKey];   // key 0 and no composite: its own reason (CTEX claim 6). With an override block the composite's own reason is the one that matters
          }
        }
        if (deferred) o.tex_incomplete = true;
        o.tex_hash[pi][si] = hash, o.tex_state[pi][si] = state;
        const bool was = pi < mr.tex_state.size() && si < mr.tex_state[pi].size() && mr.tex_state[pi][si] == state;
        if (state == kGtBound) { if (!quiet_ || !was) ++st_.bound; }
        else if (state == kGtOverride) { if (!quiet_ || !was) ++st_.from_override; }
        else if (state == kGtEngineDefault) {
          // CAWD kind 1: the engine's own default cube; counted as engine_default_cube (in One), not as missing, never retried; its words are still probed (the first kMaxRefusedProbes) so a run can read the dimension
          NoteRefused(wm, o, uint32_t(pi), uint32_t(si), s.key, s.key1, uint8_t(kWhyEngineCube), false);
        } else if (!deferred) {
          if (!quiet_) ++st_.missing;
          // the composite's own reason first (the one CTEX site 6 needs), then the model key's when that was asked and refused too
          if (ov_refused) NoteRefused(wm, o, uint32_t(pi), uint32_t(si), s.ov_key, 0, ov_why, true);
          if (!s.has_override || s.key) NoteRefused(wm, o, uint32_t(pi), uint32_t(si), s.key, s.key1, last_why_, false), retry = retry || WhyRetryable(last_why_);
        }
        if (retry && !deferred) o.tex_retry = true;
      }
    }
    if (mr.logo.has && g_gm_tex_plant != 37) {   // S105 A5c (A5c.md item 3): the logo pictures, after the part samplers: the same `One` (override_src false, the 8888 path: a loader-born linear BGRA picture), a late composed one retried
      for (int li = 0; li < 2; ++li) {
        if (li == 1 && !o.logo_hash[0]) break;   // review F4 [C 0x822E6CC8 +50]: the game binds slot 4 only inside the `if` of slot 7's resolve
        const uint32_t key = mr.logo.key[li];
        if (!key) continue;
        bool deferred = false;
        last_why_ = 0;
        const uint64_t h = One(wm, phys, key, false, frame, reads, max_reads, o, deferred);
        if (h) o.logo_hash[li] = h, o.logo_state[li] = uint8_t(kGtBound);
        if (deferred) o.tex_incomplete = true;
        else if (!h && WhyRetryable(last_why_)) o.tex_retry = true;
        if (!quiet_ && !deferred) ++(h ? st_.logo_bound : st_.logo_missing);
      }
    }
    if (cache_.size() > kMaxEntries) Trim(frame);
    const bool changed = o.logo_hash[0] != mr.logo_hash[0] || o.logo_hash[1] != mr.logo_hash[1] || o.logo_state[0] != mr.logo_state[0] || o.logo_state[1] != mr.logo_state[1] ||
                         o.tex_hash != mr.tex_hash || o.tex_state != mr.tex_state || o.tex_retry != mr.tex_retry || o.tex_incomplete != mr.tex_incomplete || o.tex_8888 != mr.tex_8888 || RefusedSig(o.n_refused, o.refused) != RefusedSig(mr.n_refused, mr.refused);
    if (changed && !mr.tex_hash.empty()) ++st_.refreshed_changed;
    return changed;
  }
  // In place (the tests, the first read): the same, then the result replaces `mr`'s own.
  template <class M, class P>
  bool Resolve(const M& m, P&& phys, ModelRead& mr, uint64_t frame, uint32_t max_reads = 2) {
    TexOut o;
    const bool changed = ResolveTo(m, phys, mr, o, frame, max_reads);
    ApplyTex(mr, std::move(o));
    return changed;
  }
  const TexStats& stats() const { return st_; }
  size_t cached() const { return cache_.size(); }
  // CTX2 site 8: the birth hook's record of a texture key (picsrc::PictureSource::BirthOf): false = never seen born; true = born at `site`, whose class is `creator` (a kCreator* code; Unknown = a site nobody classified).
  // Null hook = never seen born. Set by the live contributor; the unit sets its own.
  bool (*born_of)(uint32_t key, uint8_t& creator, uint32_t& site) = nullptr;
  // One line per 8888 handle and verdict (a handle whose verdict changes is logged again), drained by the contributor's census; at most kMaxNotes a run.
  std::vector<std::string> TakeNotes() { return std::move(notes_); }
 private:
  static constexpr size_t kMaxNotes = 48;
  // verdict: 0 = read as bytes (loader), 1 = gpu / movie creator, 2 = born at an unclassified site, 3 = never seen born
  void NoteCreator(uint32_t handle, bool seen, uint8_t cr, uint32_t site, const guest_tex::Shape& sh, uint8_t verdict) {
    if (quiet_) return;
    const auto it = noted_.find(handle);
    if (it != noted_.end() && it->second == verdict) return;
    if (it == noted_.end()) ++st_.tex8888, ++st_.tex8888_by_creator[cr & 3];
    noted_[handle] = verdict;
    if (notes_logged_ >= kMaxNotes) return;
    ++notes_logged_;
    char b[260], born[48];
    if (seen) std::snprintf(born, sizeof born, "born at site %08X", site);
    else std::snprintf(born, sizeof born, "never seen born");
    std::snprintf(b, sizeof b, "[rdr][caw] 8888 picture: handle %08X %s, creator %s, %ux%u endian %u swizzle %03X base %08X (%s)", handle, born, CreatorName(cr), sh.width, sh.height, sh.endian, sh.fetch_swizzle, sh.base,
                  verdict == 0 ? "read as bytes" : verdict == 1 ? "refused: render target / movie creator" : verdict == 2 ? "refused: born at an unclassified site" : "refused: never seen born");
    notes_.emplace_back(b);
  }
  // CTX2 review 2: a cheap guard over a linear 8888 page: kGuardSamples texels spread over the picture (4 bytes each, raw memory), hashed. Equal = leave the held bytes; changed (or unreadable: 0) = read the whole picture again.
  // [U] a rewrite that touches none of the sampled texels is not seen until the next full read; the editor rewrites whole layers.
  static constexpr uint32_t kGuardSamples = 64;
  template <class P>
  static uint64_t SampleGuard(const guest_tex::Shape& sh, P&& phys) {
    uint64_t h = 0x9A8D0000ull ^ (uint64_t(sh.width) << 16) ^ sh.height;
    for (uint32_t i = 0; i < kGuardSamples; ++i) {
      const uint32_t x = (i * 37u + (i >> 3) * 11u) % sh.width, y = (i * 61u + 7u * (i / 4u)) % sh.height;   // fixed, spread over rows and columns
      uint8_t px[4];
      if (!phys(sh.base + guest_tex::BlockOffset(sh, x, y), 4u, px)) return 0;   // (S108 CAP2: the shared offset, a packed-tail picture's offset included)
      h = HashU32(uint32_t(px[0]) | uint32_t(px[1]) << 8 | uint32_t(px[2]) << 16 | uint32_t(px[3]) << 24, h);
    }
    return h ? h : 1;
  }
  std::unordered_map<uint32_t, uint8_t> noted_;
  std::vector<std::string> notes_;
  size_t notes_logged_ = 0;
  uint32_t reads8888_ = 0;   // full 8888 reads this call (at most one: a picture is up to 4 MB)
  struct Entry {
    uint64_t hash = 0, sample = 0;   // sample: the 8888 guard of the held bytes
    uint32_t width = 0, height = 0, format = 0;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    uint64_t last_read = 0, last_use = 0;
    uint8_t why = 0;   // guest_tex::Why of the last read (0 = ok)
    uint32_t header = 0, flush_gen = 0;   // S105 A5c review F5-F7: the texture header read and its guest_tex_dirty::Gen taken just before the last good read
  };
  template <class M, class P>
  uint64_t One(const WordMem<M>& wm, P& phys, uint32_t handle, bool override_src, uint64_t frame, uint32_t& reads, uint32_t max_reads, TexOut& o, bool& deferred) {
    guest_tex::Resolved r;
    guest_tex::Shape sh;
    guest_tex::Why w = guest_tex::ResolveHandle(wm, handle, r);
    if (w == guest_tex::Why::NotPlain2D && IsCube(wm, handle)) {   // CAWD kind 1: the texture object is a cube (the wrapper's mode word picks the header; its fetch dimension is 3): the engine's default stands, nothing to read
      last_why_ = uint8_t(kWhyEngineCube);
      if (!quiet_) ++st_.engine_default_cube;
      return 0;
    }
    // CAWD sites 1-8: k_8_8_8_8 is accepted (the CAW model's three linear BGRA pictures) with an identity-BGRA fetch swizzle (ShapeOfFetch refuses any other as format); a TILED 8888 stays refused (not a CPU-written picture [L]).
    if (w == guest_tex::Why::Ok) w = guest_tex::ShapeOfFetch(r.fetch, sh, g_gm_tex_plant != 34);
    if (w == guest_tex::Why::Ok && sh.game_format == 6 && sh.tiled) w = guest_tex::Why::Format;
    if (w != guest_tex::Why::Ok) {
      last_why_ = uint8_t(w);
      if (!quiet_) ++st_.why[last_why_ & 15];
      return 0;
    }
    const bool is8888 = sh.game_format == 6;
    if (is8888) {   // site 8, like picture_source.h A4: only a texture LOADER's picture is CPU-written bytes (k_8_8_8_8 can be a render target or a movie frame: a stale surface is the one wrong answer)
      uint8_t cr = kCreatorUnknown;
      uint32_t site = 0;
      const bool seen = born_of && born_of(handle, cr, site);
      if (cr != kCreatorLoader) {
        const uint8_t verdict = (cr == kCreatorGpu || cr == kCreatorMovie) ? 1 : (seen ? 2 : 3);
        last_why_ = uint8_t(verdict == 1 ? kWhyGpuCreator : kWhyUnclassified);
        if (!quiet_) ++st_.why[last_why_ & 15];   // (lint L11: the index is last_why_)
        NoteCreator(handle, seen, cr, site, sh, verdict);
        return 0;
      }
      NoteCreator(handle, seen, cr, site, sh, 0);
      o.tex_8888 = true;
    }
    uint64_t id = HashU32(handle, 0x7E57);
    id = Hash64(r.fetch, sizeof r.fetch, id);
    Entry& e = cache_[id];
    bool refresh_due = (override_src || is8888) && e.hash && frame >= e.last_read + kOverrideRefreshFrames;
    // S105 A5c (A5c.md item 4): the game says when a picture changed: the unlock flush 0x82CA6BE0 of its header (guest_tex_dirty.h). A flushed identity is due at once, whatever its format (the match composer's BC pictures
    // were never re-read before), and the sampled guard is skipped for it; the guard stays the fallback for a header the dirty table could not watch.
    const bool dirty = e.hash && r.header && g_gm_tex_plant != 36 && guest_tex_dirty::Gen(r.header) != e.flush_gen;   // review F6: per entry, so two handles of one header both see a flush
    if (dirty) refresh_due = true;
    if (refresh_due && is8888 && !dirty) {   // review 2: the 8888 page may be rewritten in place by the editor: a cheap sampled guard first, the whole picture only when it changed
      const auto pg = [&](uint32_t a, uint32_t n, uint8_t* d) { return phys(a, n, d); };
      const uint64_t g = SampleGuard(sh, pg);
      if (g && g == e.sample) e.last_read = frame, refresh_due = false, ++st_.tex8888_guard_same;
      else ++st_.tex8888_rereads;
    }
    if (!e.hash || refresh_due) {
      if (reads >= max_reads || (is8888 && reads8888_ >= 1)) {   // an 8888 picture is up to 4 MB: one full read a call
        deferred = true;
        ++st_.deferred;
        if (dirty) ++st_.dirty_deferred;   // (the dirty mark stays set: the next call reads it)
        return e.hash;   // an older read of the same identity stays valid until refreshed
      }
      ++reads, ++st_.byte_reads;
      if (is8888) ++reads8888_;
      uint32_t gen_before = 0;
      if (r.header) {
        if (dirty) ++st_.dirty_rereads;
        if (!guest_tex_dirty::Watch(r.header)) ++st_.unwatched;
        gen_before = guest_tex_dirty::Gen(r.header);   // taken BEFORE the read: a flush that lands during it is seen next call; a failed read never stores it (review F7)
      }
      std::vector<uint8_t> blocks;
      const auto p2 = [&](uint32_t a, uint32_t n, uint8_t* d) { return phys(a, n, d); };
      const guest_tex::Why rw = guest_tex::ReadMip0(sh, p2, blocks, kGmMaxTexBytes);
      e.last_read = frame;
      e.why = uint8_t(rw);
      if (rw != guest_tex::Why::Ok || blocks.size() != GuestTexBytes(sh.width, sh.height, sh.game_format)) {
        last_why_ = uint8_t(rw != guest_tex::Why::Ok ? rw : guest_tex::Why::Size);
        if (!quiet_) ++st_.why[last_why_ & 15];   // (a size mismatch with rw == Ok is Size, never slot 0 = ok)
        if (!e.hash) cache_.erase(id);
        return 0;
      }
      const uint64_t h = Hash64(blocks.data(), blocks.size(), HashU32(sh.width * 65536u + sh.height, HashU32(sh.game_format, 0x7E57)));
      e.header = r.header, e.flush_gen = gen_before;
      e.hash = h, e.width = sh.width, e.height = sh.height, e.format = sh.game_format;
      if (is8888) e.sample = SampleGuard(sh, p2);   // the guard of the bytes just read (the next refresh compares against it)
      e.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(blocks));
    } else {
      ++st_.cache_hits;
    }
    e.last_use = frame;
    for (const TexRead& t : o.textures)
      if (t.hash == e.hash) return e.hash;
    TexRead t;
    t.hash = e.hash, t.width = e.width, t.height = e.height, t.format = e.format, t.bytes = e.bytes;
    o.textures.push_back(std::move(t));
    return e.hash;
  }
  // CAWD kind 1 / CTX2 review 4: is the registry's texture object for `handle` a CUBE? The wrapper's mode word ([wrapper+52]) picks the header word: 0 -> +80, 1 -> +84, 2 -> +88 (sub_82BD4600 slot 7, E_inline_callbacks.md:142);
  // the header's fetch dword 5 holds the dimension in bits 9-10 (1 = 2D, 3 = cube). Only that dimension makes a cube: a refused wrapper of another kind stays a refusal. Reads are bounded by the registry's own [begin, end).
  template <class M>
  static bool IsCube(const WordMem<M>& wm, uint32_t handle) {
    const uint32_t reg = wm.u32(particles::kTextureHandleGlobal);
    const uint32_t b = reg ? wm.u32(reg + 8) : 0, e = reg ? wm.u32(reg + 16) : 0, idx = handle & 0xFFFFu;
    bool cube = false;
    if (b && e > b && idx < ((e - b) >> 3) && wm.u32(b + idx * 8) == handle) {
      const uint32_t wrapper = wm.u32(b + idx * 8 + 4);
      const uint32_t kind = wrapper ? wm.u32(wrapper + particles::kWrapperKind) : 9;
      const uint32_t header = (kind <= 2) ? wm.u32(wrapper + particles::kWrapperTexture + 4 * kind) : 0;
      if (header) cube = ((wm.u32(header + particles::kHeaderFetch + 20) >> 9) & 3u) == 3u;
    }
    const bool bad = wm.fail;
    wm.fail = false;
    return cube && !bad;
  }
  // The first kMaxRefusedProbes refusals of a model: the raw words of the registry walk (a plain read each, no allocation). Reads are bounded by the registry's own [begin, end).
  template <class M>
  static void NoteRefused(const WordMem<M>& wm, TexOut& o, uint32_t pi, uint32_t si, uint32_t key, uint32_t key1, uint8_t why, bool ov) {
    if (o.n_refused >= kMaxRefusedProbes) return;
    RefusedProbe& p = o.refused[o.n_refused++];
    p = RefusedProbe{};
    p.part = uint8_t(pi), p.sampler = uint8_t(si), p.why = why, p.ov = ov, p.key0 = key, p.key1 = key1;
    if (key) {
      const uint32_t reg = wm.u32(particles::kTextureHandleGlobal);
      const uint32_t b = reg ? wm.u32(reg + 8) : 0, e = reg ? wm.u32(reg + 16) : 0, idx = key & 0xFFFFu;
      if (b && e > b && idx < ((e - b) >> 3)) {
        p.stored_id = wm.u32(b + idx * 8), p.wrapper = wm.u32(b + idx * 8 + 4);
        if (p.wrapper) {
          p.vtable = wm.u32(p.wrapper), p.kind = wm.u32(p.wrapper + particles::kWrapperKind), p.header = wm.u32(p.wrapper + particles::kWrapperTexture + (p.kind == 1 || p.kind == 2 ? 4 * p.kind : 0u));   // (kind 1 / 2: the header sits at +84 / +88)
          if (p.header)   // texreg-lint: raw-dump (the refusal probe prints the six header words as the guest stores them; nothing decodes or uses a base from them)
            for (uint32_t i = 0; i < 6; ++i) p.fetch[i] = wm.u32(p.header + particles::kHeaderFetch + 4 * i);
        }
      }
    }
    wm.fail = false;
  }
  void Trim(uint64_t frame) {
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (frame > it->second.last_use && frame - it->second.last_use > 600) {
        const uint32_t hd = it->second.header;
        it = cache_.erase(it);
        if (hd) {   // review F5: free the dirty-table slot unless another entry still reads this header
          bool held = false;
          for (const auto& kv : cache_) held = held || kv.second.header == hd;
          if (!held) guest_tex_dirty::Unwatch(hd);
        }
      } else ++it;
    }
  }
  std::unordered_map<uint64_t, Entry> cache_;
  TexStats st_;
  uint8_t last_why_ = 0;   // the refusal of the last One() / key check (guest_tex::Why or kWhyNoKey)
  bool quiet_ = false;     // this call is a re-ask of a retryable miss: refusals already counted are not counted again
};

// Does `rd` need the resolver again? Deferred pictures: next call; a retryable miss (CTF: a late-composed texture) or an override block: about every kOverrideRefreshFrames since `tex_checked`.
// Pure and allocation-free: the contributor asks this every frame for every drawn model.
inline bool TexDue(const ModelRead& rd, bool has_override, uint64_t tex_checked, uint64_t frame) {
  if (rd.tex_incomplete) return true;
  const bool retry = rd.tex_retry && g_gm_tex_plant != 32;
  return (has_override || retry || (rd.tex_8888 && g_gm_tex_plant != 35)) && frame >= tex_checked + TexResolver::kOverrideRefreshFrames;   // 35 (test): the 8888 refresh is off (pictures freeze)
}
// The contributor's ONE per-call decision (CTF review 3; the unit calls the same function): does the resolver run for this call (a fresh read, or due), and the model's `tex_checked` after it.
// `tex_checked` moves only when the resolver ran (g_gm_tex_plant 33 = the fault: it is re-set every call, so the 61-frame period never comes due for a model drawn each frame).
// S105 A5c: `dirty_seen` (the model's last-seen guest_tex_dirty::Seq) makes a flush of ANY watched picture since the model's last run due at once (one extra run per model per flush batch; null = the old rule).
inline bool TexCallStep(const ModelRead& rd, bool has_override, bool fresh, uint64_t& tex_checked, uint64_t frame, uint64_t* dirty_seen = nullptr) {
  const uint64_t seq = guest_tex_dirty::Seq();
  const bool flushed = dirty_seen && g_gm_tex_plant != 36 && seq != *dirty_seen;
  const bool run = fresh || flushed || TexDue(rd, has_override, tex_checked, frame);
  if (run || g_gm_tex_plant == 33) tex_checked = frame;
  if (run && dirty_seen) *dirty_seen = seq;
  return run;
}

}  // namespace rdr_scene::live::guest_model
