// S111 CACTHUMB: R7, the Create a Title thumbnail, DEFERRED and never blocking (R1's design, thumb_defer_off.h; its ThumbBoard). Stream-free build only: thumb_defer_off.h includes it.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. The game's side and the rules: title_thumb_core.h, docs\research\s111\CACTHUMB.md.
//
//   callback entry  sub_833438B0 (the render thread, op 68; [r3] = the object): a waiting object (title_thumb_core.h Waiting) gets a SCENE REQUEST (never blocks) and the callback returns without
//                   running: obj+28 stays 0, so the game's own state 5 waits (and the editor's save sequence with it);
//   host callback   the scene picture's top-left (the resolve rect) is box-scaled to 256x128 (MakePicture), once;
//   each present    (thumb_off::OnPresent, the same render thread) a landed picture, or none after wwe2k14_rdr_readback_thumb_timeout frames, is handed over (t_delivered) and the callback is run with
//                   r3 = 0x839BFC30 (its word 0 is the object); its lock (0x8334398C, readback_ours.h ServeTitleLock) writes the picture or BLACK, then the game encodes / swaps / saves as always;
//   moved on        an object no longer waiting (state 7: the editor released it) is dropped: nothing runs, nothing waits for it.
#pragma once

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../d3d_layer.h"
#include "../frame_stats.h"   // g_guest_frame
#include "../shown_frame.h"   // g_scene_request_owner
#include "../renderer_adapter/thumb_defer_core.h"
#include "readback_ours.h"
#include "title_thumb_core.h"

#include "readbacks_start.h"   // S111 RBSWITCH: the start value of wwe2k14_rdr_readbacks
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_readback_thumb_timeout);

REX_EXTERN(__imp__sub_833438B0);

namespace wwe2k14::title_thumb {

struct Mail {
  bool ok = false;
  std::vector<uint8_t> rgba;   // kW x kH
};
inline std::mutex g_mu;
inline rdr_adapter::ThumbBoard g_board;             // under g_mu (entry.image = the request's seq)
inline std::unordered_map<uint32_t, Mail> g_mail;   // seq -> the answer (written by the host thread's callback)
inline std::unordered_set<uint32_t> g_live;         // seqs still on the board (a late answer of a dropped one is discarded)
inline uint32_t g_seq = 0;

// Runs the callback for `e` on this thread. `mail` null or not ok = no picture (black).
inline void Deliver(PPCContext& ctx, uint8_t* base, const rdr_adapter::ThumbEntry& e, Mail* mail, const char* why) {
  using namespace wwe2k14::readback_ours;
  const auto ld = [&](uint32_t a) { return wwe2k14::d3d_layer::Ld32(base, a); };
  if (!Waiting(e.obj, ld)) {   // the editor moved the object on: its state no longer waits for obj+28
    const uint64_t d = g_counters.title_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogDue(d)) REXLOG_WARN("[rdr][readback] R7 object {:08X}: no longer waiting (state {}, done {}): dropped, the callback is not run", e.obj, ld(e.obj + 16), ld(e.obj + 28));
    return;
  }
  const bool pic = mail && mail->ok && mail->rgba.size() == size_t(kW) * kH * 4;
  if (pic) g_counters.title_delivered.fetch_add(1, std::memory_order_relaxed);
  else if (mail) g_counters.title_no_scene.fetch_add(1, std::memory_order_relaxed);
  const uint64_t n = g_counters.title_deferred.load(std::memory_order_relaxed);
  if (LogDue(n))
    REXLOG_INFO("[rdr][readback] R7 object {:08X}: callback run after {} guest frames ({})", e.obj,
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed) - std::min(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), e.lock_frame), why);
  const PPCContext saved = ctx;
  ctx.r3.u64 = kObjGlobal;   // the payload: word 0 = the object (the callback reads only [r3]; Waiting checked [kObjGlobal] == obj)
  t_delivered.active = true;
  t_delivered.rgba = pic ? mail->rgba.data() : nullptr;
  __imp__sub_833438B0(ctx, base);
  t_delivered = Delivered{};
  ctx.fpscr.restoreGuestBits(saved.fpscr.csr);   // as thumb_defer_off.h Deliver
  ctx = saved;
}

// The callback's hook, at its entry. true: deferred, do not run it (obj+28 stays 0).
inline bool OnJobEnter(PPCContext& ctx, uint8_t* base) {
  using namespace wwe2k14::readback_ours;
  if (t_delivered.active || !wwe2k14::readbacks_start::On() || !wwe2k14::g_scene_request_owner) return false;
  const auto ld = [&](uint32_t a) { return wwe2k14::d3d_layer::Ld32(base, a); };
  const uint32_t obj = ctx.r3.u32 ? ld(ctx.r3.u32) : 0u;
  if (!Waiting(obj, ld)) {   // not the waiting thumbnail object: the callback runs as before (its lock writes black)
    g_counters.title_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint32_t gw = 0, gh = 0;
  GameSize(ld(0x8392DC3Cu), gw, gh);
  uint32_t seq;
  {
    std::lock_guard lock(g_mu);
    seq = ++g_seq;
    if (!g_board.Add({obj, 0u, seq, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), ctx.r13.u64})) {
      g_counters.title_refused.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    g_live.insert(seq);
  }
  const bool posted = wwe2k14::g_scene_request_owner([seq, gw, gh](bool ok, std::vector<uint8_t>&& rgba, uint32_t pw, uint32_t ph, uint64_t) {
    Mail made;   // the crop and the 256x128 scale are done HERE (the host thread, once), so the delivery only copies 128 KB
    made.ok = ok && pw && ph && rgba.size() >= size_t(pw) * ph * 4 && MakePicture(rgba.data(), pw, ph, gw, gh, made.rgba);
    std::lock_guard lock(g_mu);
    if (!g_live.count(seq)) return;
    g_mail[seq] = std::move(made);
  });
  if (!posted) {
    std::lock_guard lock(g_mu);
    rdr_adapter::ThumbEntry e;
    g_board.Remove(obj, &e);
    g_live.erase(seq);
    g_counters.title_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint64_t n = g_counters.title_deferred.fetch_add(1, std::memory_order_relaxed) + 1;
  if (LogDue(n)) REXLOG_INFO("[rdr][readback] R7 callback {} deferred: object {:08X} (game frame {}x{}); it returns at once, the object waits at state 5", n, obj, gw, gh);
  return true;
}

// Called from thumb_off::OnPresent (the present's thread; an entry of another thread is left alone by the board).
inline void OnPresent(PPCContext& ctx, uint8_t* base) {
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
    if (it.timed_out && !it.have) readback_ours::g_counters.title_timed_out.fetch_add(1, std::memory_order_relaxed);
    Deliver(ctx, base, it.e, it.have ? &it.mail : nullptr, it.have ? (it.mail.ok ? "picture" : "no_scene") : "timed_out");
  }
}

}  // namespace wwe2k14::title_thumb

REX_HOOK_RAW(sub_833438B0) {
  if (wwe2k14::title_thumb::OnJobEnter(ctx, base)) return;
  __imp__sub_833438B0(ctx, base);
}
