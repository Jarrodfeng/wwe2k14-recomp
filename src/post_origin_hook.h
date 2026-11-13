// wwe2k14 - Session 101 lane PN: the hooks that tell the op-68 recorder which post effect is queuing (renderer_adapter\post_origin.h).
// sub_82C1F5E8 (22 call sites in 13 effect functions) only tail-branches to sub_82C1F420, so its caller's LR arrives unchanged in 82C1F420;
// 82C1F420's other direct callers (82C1F728 T_FillColorDepth, 827E89C0, 827EA098, 82813BD0), sub_82C1F4C0 (caller 82814760) and sub_82C1F610
// (caller sub_82C1D9C0 = T_Final, the combine) carry their own LR. One hook per queue function covers all of them [C: q callers; listings of
// the three functions in docs\research\s101\A0-3.md F]. Included from draw_origin.h.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "renderer_adapter/post_origin.h"

REX_HOOK_RAW(sub_82C1F420) {
  rdr_adapter::origin::PostSiteScope scope(uint32_t(ctx.lr));
  __imp__sub_82C1F420(ctx, base);
}
REX_HOOK_RAW(sub_82C1F4C0) {
  rdr_adapter::origin::PostSiteScope scope(uint32_t(ctx.lr));
  __imp__sub_82C1F4C0(ctx, base);
}
REX_HOOK_RAW(sub_82C1F610) {
  rdr_adapter::origin::PostSiteScope scope(uint32_t(ctx.lr));
  __imp__sub_82C1F610(ctx, base);
}
