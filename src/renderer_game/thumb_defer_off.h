// S103 RBC fix: R1, the replay-clip thumbnail, NEVER blocking (the stream-free build's port of src\thumb_defer.h; the design is src\renderer_adapter\thumb_defer_core.h's, whose ThumbBoard this uses).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Stream-free build only (d3d_textures.h includes it in the `#else` of the stream gate).
//
// The job sub_8274FA70 (r3 = the thumbnail object) locks its 128x128 8888 texture and copies the rows into obj+84, then moves its ready word obj+44 from 4 to 0; the file writer sub_8274FC60 saves obj+84 only
// when obj+44 == 0. It runs on the game thread, which also posts the presents: a blocking capture there stalls the match for the host's idle poll (~100 ms) plus a drain. So:
//   job entry      the hook answers it with a SCENE REQUEST (SceneHost::RequestScene, never blocks) and returns with obj+44 still 4 (to the writer: "the job is still running");
//   each present   (half2_hooks.h, the game's thread) a landed picture is written into the texture's memory (readback_ours.h Prepare / Write, the 128x128 guard) and the job is run here, on the same object, with
//                  t_thumb_replay set so its lock finds the picture in memory; the job copies the rows and sets obj+44 = 0 as always;
//   file writer    an object whose job is still deferred is delivered first (its picture if it landed, else the job runs on the memory as it is: counted before_write);
//   destructor     the object's deferral is dropped (its buffer and texture are freed without touching obj+44);
//   no picture     no 3D scene (the host answered not-ok) or kTimeout frames: the job runs on the memory as it is (counted no_scene / timed_out), so the clip is saved as it always was, never hangs.
// A picture is the SCENE of the newest OURS 3D frame, raw (no 2D, no HUD, no curve): the game's own thumbnail pass drew the 3D view too.
#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include "readbacks_start.h"   // S111 RBSWITCH: the start value of wwe2k14_rdr_readbacks
#include <rex/hook.h>
#include <rex/logging.h>

#include "../d3d_layer.h"
#include "../frame_stats.h"   // g_guest_frame
#include "../renderer_adapter/thumb_defer_core.h"
#include "readback_ours.h"
#include "title_thumb_off.h"   // S111 CACTHUMB: R7, the Create a Title thumbnail (its own board; delivered from OnPresent below)

REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_thumb_timeout);

REX_EXTERN(__imp__sub_8274FA70);
REX_EXTERN(__imp__sub_8274FC60);
REX_EXTERN(__imp__sub_82750580);

namespace wwe2k14::thumb_off {

struct Mail {
  bool ok = false;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
};
inline std::mutex g_mu;
inline rdr_adapter::ThumbBoard g_board;                // under g_mu
inline std::unordered_map<uint32_t, Mail> g_mail;      // seq -> the answer (written by the host thread's callback)
inline std::unordered_set<uint32_t> g_live;            // seqs still on the board (a late callback of a dropped one is discarded)
inline uint32_t g_seq = 0;

inline constexpr uint32_t kSize = 128;

// Runs the job for `e` on this thread. `mail` null = no picture (the memory is used as it is).
inline void Deliver(PPCContext& ctx, uint8_t* base, const rdr_adapter::ThumbEntry& e, Mail* mail, const char* why) {
  using namespace wwe2k14::readback_ours;
  const auto ld = [&](uint32_t a) { return wwe2k14::d3d_layer::Ld32(base, a); };
  if (!rdr_adapter::DeliveryAllowed(e, ld, true)) {   // the object died or changed under us: nothing is touched
    g_counters.thumb_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int site = 0;   // R1 is kSites[0]
  if (mail && mail->ok) {
    uint32_t w[6];
    FetchWords(base, e.texture, w);
    Guard g;
    g.want_w = kSize, g.want_h = kSize, g.writable = &HostWritable;
    readback_tile::Plan p;
    readback_tile::Why r = Prepare(w, g, p);
    if (r == readback_tile::Why::Ok) r = Write(p, &PhysHost, g, mail->rgba.data(), mail->w, mail->h);
    if (r == readback_tile::Why::Ok) {
      NoteServed(site);
      g_counters.thumb_delivered.fetch_add(1, std::memory_order_relaxed);
    } else {
      NoteUnserved(site, r);
      REXLOG_WARN("[rdr][readback] R1 object {:08X}: texture {:08X} NOT written: {} (fetch {:08X} {:08X} {:08X} {:08X}); the job runs on the memory as it is", e.obj, e.texture, readback_tile::WhyText(r), w[0], w[1], w[2], w[3]);
    }
  } else {
    const std::string reason(why);   // its own census reason each: the host said no scene / no picture within the timeout / the writer came first
    NoteUnserved(site, reason == "no_scene" ? readback_tile::Why::NoScene : reason == "timed_out" ? readback_tile::Why::TimedOut : readback_tile::Why::NoPicture);
    if (reason == "no_scene") g_counters.thumb_no_scene.fetch_add(1, std::memory_order_relaxed);
  }
  const uint64_t n = g_counters.thumb_deferred.load(std::memory_order_relaxed);
  if (LogDue(n))
    REXLOG_INFO("[rdr][readback] R1 object {:08X}: job run after {} guest frames ({}), texture {:08X}", e.obj, wwe2k14::g_guest_frame.load(std::memory_order_relaxed) - std::min(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), e.lock_frame), why, e.texture);
  const PPCContext saved = ctx;
  ctx.r3.u64 = e.obj;
  t_thumb_replay = true;
  __imp__sub_8274FA70(ctx, base);
  t_thumb_replay = false;
  ctx.fpscr.restoreGuestBits(saved.fpscr.csr);   // F5: as thumb_defer.h RunJobs does
  ctx = saved;
}

// The job's hook, at its entry (r3 = the object). true: deferred, do not run it (obj+44 stays 4).
inline bool OnJobEnter(PPCContext& ctx, uint8_t* base) {
  using namespace wwe2k14::readback_ours;
  if (t_thumb_replay || !wwe2k14::readbacks_start::On() || !wwe2k14::g_scene_request_owner) return false;
  const uint32_t obj = ctx.r3.u32;
  if (!obj || wwe2k14::d3d_layer::Ld32(base, obj + 44) != 4) return false;   // the job would do nothing: let it
  const uint32_t texture = wwe2k14::d3d_layer::Ld32(base, obj + 52);
  if (!texture) return false;
  uint32_t w[6];
  FetchWords(base, texture, w);
  Guard g;
  g.want_w = kSize, g.want_h = kSize;
  readback_tile::Plan p;
  if (Prepare(w, g, p) != readback_tile::Why::Ok) {   // not the 128x128 8888 the job is known to lock: it runs as before (and its lock counts it)
    g_counters.thumb_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint32_t seq;
  {
    std::lock_guard lock(g_mu);
    seq = ++g_seq;
    if (!g_board.Add({obj, texture, seq, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), ctx.r13.u64})) {
      g_counters.thumb_refused.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    g_live.insert(seq);
  }
  const bool posted = wwe2k14::g_scene_request_owner([seq](bool ok, std::vector<uint8_t>&& rgba, uint32_t pw, uint32_t ph, uint64_t) {
    Mail made;   // F3: the 128x128 scale is done HERE (the host thread, once per clip), so the game thread's delivery only copies the 64 KB
    made.ok = ok && pw && ph && rgba.size() >= size_t(pw) * ph * 4;
    if (made.ok) {
      readback_tile::ScaleBox(rgba.data(), pw, ph, kSize, kSize, made.rgba);
      made.w = made.h = kSize;
    }
    std::lock_guard lock(g_mu);
    if (!g_live.count(seq)) return;
    g_mail[seq] = std::move(made);
  });
  if (!posted) {
    std::lock_guard lock(g_mu);
    rdr_adapter::ThumbEntry e;
    g_board.Remove(obj, &e);
    g_live.erase(seq);
    g_counters.thumb_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint64_t n = g_counters.thumb_deferred.fetch_add(1, std::memory_order_relaxed) + 1;
  if (LogDue(n)) REXLOG_INFO("[rdr][readback] R1 job {} deferred: object {:08X}, texture {:08X}; it returns at once with its ready word at 4", n, obj, texture);
  return true;
}

// The entry leaves the board and its mail is taken (g_mu held by the caller).
inline bool TakeEntry(uint32_t obj, rdr_adapter::ThumbEntry& e, Mail& mail, bool& have_mail) {
  if (!g_board.Remove(obj, &e)) return false;
  g_live.erase(e.image);
  const auto it = g_mail.find(e.image);
  have_mail = it != g_mail.end();
  if (have_mail) mail = std::move(it->second), g_mail.erase(it);
  return true;
}

// Called from the present hook (the game's thread; a present on another thread delivers nothing: the board filters by ctx.r13).
inline void OnPresent(PPCContext& ctx, uint8_t* base) {
  wwe2k14::title_thumb::OnPresent(ctx, base);   // S111 CACTHUMB: deferred R7 callbacks whose picture landed (or timed out); before R1's early return
  struct Item {
    rdr_adapter::ThumbEntry e;
    Mail mail;
    bool have = false, timed_out = false;
  };
  static thread_local std::vector<rdr_adapter::ThumbBoard::Ready> ready;
  static thread_local std::vector<Item> items;
  ready.clear(), items.clear();
  {
    std::lock_guard lock(g_mu);
    if (!g_board.Pending()) return;
    const uint64_t now = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    const uint64_t timeout = uint64_t(std::max(REXCVAR_GET(wwe2k14_rdr_readback_thumb_timeout), 0));
    g_board.Step(now, timeout, ctx.r13.u64, [&](const rdr_adapter::ThumbEntry& e) { return g_mail.count(e.image) != 0; }, ready);
    for (const auto& r : ready) {
      Item it;
      it.e = r.entry;
      g_live.erase(r.entry.image);
      const auto m = g_mail.find(r.entry.image);
      it.have = m != g_mail.end();
      if (it.have) it.mail = std::move(m->second), g_mail.erase(m);
      it.timed_out = r.why == rdr_adapter::ThumbBoard::Why::TimedOut;
      items.push_back(std::move(it));
    }
  }
  for (Item& it : items) {
    if (it.timed_out && !it.have) readback_ours::g_counters.thumb_timed_out.fetch_add(1, std::memory_order_relaxed);
    Deliver(ctx, base, it.e, it.have ? &it.mail : nullptr, it.have ? (it.mail.ok ? "picture" : "no_scene") : "timed_out");
  }
}

}  // namespace wwe2k14::thumb_off

// SKIP-ORIGINAL: OnJobEnter defers the thumbnail job until its picture is here (the present runs it later); every other call runs the original
REX_HOOK_RAW(sub_8274FA70) {
  if (wwe2k14::thumb_off::OnJobEnter(ctx, base)) return;
  __imp__sub_8274FA70(ctx, base);
}

// The file writer: an object whose job is still deferred is delivered first, so the thumbnail is saved, not skipped.
REX_HOOK_RAW(sub_8274FC60) {
  using namespace wwe2k14::thumb_off;
  rdr_adapter::ThumbEntry e;
  Mail mail;
  bool have = false, found;
  {
    std::lock_guard lock(g_mu);
    found = TakeEntry(ctx.r3.u32, e, mail, have);
  }
  if (found) {
    wwe2k14::readback_ours::g_counters.thumb_before_write.fetch_add(1, std::memory_order_relaxed);
    Deliver(ctx, base, e, have ? &mail : nullptr, have ? (mail.ok ? "picture" : "no_scene") : "before_write");
  }
  __imp__sub_8274FC60(ctx, base);
}

// The destructor frees obj+84 and the texture without touching +44 / +52: its deferral goes first.
REX_HOOK_RAW(sub_82750580) {
  using namespace wwe2k14::thumb_off;
  {
    std::lock_guard lock(g_mu);
    rdr_adapter::ThumbEntry e;
    Mail mail;
    bool have = false;
    if (TakeEntry(ctx.r3.u32, e, mail, have)) wwe2k14::readback_ours::g_counters.thumb_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82750580(ctx, base);
}
