// wwe2k14 - Session 101 lane PN (docs\renderer\POST_PROMOTION_MAP.md section 7; docs\systems\post_effects.md lane PR section 7): name every post
// quad at its source. Every post quad is recorded by op 68 (sub_82BCBDD8) from one of three generic queue functions (sub_82C1F420 / 4C0 / 610),
// so its origin used to be the queue, not the effect. The effect function that queued it is on the same thread, inline, while the frame is
// recorded: the queue function's LR is the effect's return address. The hooks (src\post_origin_hook.h) open a PostSiteScope with that LR; the
// op-68 RecordTag asks ApplyPostSite for the key: call_site = the effect's call (lr - 4: a row of origin_table.h names the effect function and
// its technique), part = the generic queue site (the old call_site). Tools that keyed on 0x82C1F48C / 0x82C1F514 / 0x82C1F674 read it back through tools\particles\pcap.py queue_site()
// (post_census, particles\census, shadow_census; they read call_site directly before lane PN's follow-up and would have broken on a capture with these hooks).
// No game, no SDK: the unit test (tools\names\post_origin_test.cpp) includes this file and drives the shipped functions.
#pragma once

#include <cstdint>

#include "origin_intern.h"

namespace rdr_adapter::origin {

// The effect call site being queued on this thread (lr - 4 of the call into the queue function); 0 = none.
inline thread_local uint32_t t_post_site = 0;

#ifdef S101_PLANT_POST_DROP_LR  // planted fault (tools\names\post_origin_test.cpp --planted build): the hook forgets the LR; the quads stay unnamed
inline constexpr bool kPlantDropLr = true;
#else
inline constexpr bool kPlantDropLr = false;
#endif

// Opened by the queue-function hooks around the original; restores the outer value on exit (the queue functions do not nest, but a scope is cheap).
class PostSiteScope {
 public:
  explicit PostSiteScope(uint32_t lr) : saved_(t_post_site) {
    if (!g_on.load(std::memory_order_relaxed)) return;  // origins off: nothing is written, nothing read (the cost is one relaxed load)
    t_post_site = kPlantDropLr ? 0u : (lr >= 4u ? lr - 4u : 0u);
  }
  ~PostSiteScope() { t_post_site = saved_; }  // always writes back (a no-op when origins are off: saved_ was the value on entry)
  PostSiteScope(const PostSiteScope&) = delete;
  PostSiteScope& operator=(const PostSiteScope&) = delete;

 private:
  uint32_t saved_;
};

// The op-68 recorder's key (RecordTag): `queue_site` = the recorder's own caller (lr - 4). Inside a PostSiteScope the quad is named by the
// effect call site; outside one (a callback queued by something we do not hook) it keeps the queue site, as before.
inline void ApplyPostSite(Key& k, uint32_t op, uint32_t queue_site) {
  k.call_site = queue_site;
  if (op == 68 && t_post_site) k.call_site = t_post_site, k.part = queue_site;
}

}  // namespace rdr_adapter::origin
